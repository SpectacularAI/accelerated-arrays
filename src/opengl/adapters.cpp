#include <cassert>
#include <sstream>

#include "adapters.hpp"
#include "../image.hpp"

namespace accelerated {
namespace opengl {
void checkError(const char *tag) {
    GLint error;
    bool any = false;
    while((error = glGetError())) {
        any = true;
        log_error("%s produced glError (0x%x)", tag, error);
    }
    if (any) {
        std::abort();
    }
}

void checkError(std::string tag) {
    checkError(tag.c_str());
}

// could be exposed in the hpp file but not used currently elsewhere
struct Texture : Destroyable, Binder::Target {
    static std::unique_ptr<Texture> create(int w, int h, const ImageTypeSpec &spec);
    virtual int getId() const = 0;
};

namespace {

/**
 * Ensures an OpenGL flag is in the given state and returns it to its
 * original state afterwards
 */
template <GLuint flag, bool targetState> class GlFlagSetter {
private:
    const bool origState;

    void logChange(bool state) {
        (void)state;
        LOG_TRACE("%s GL flag 0x%x (target state %s)",
            state ? "enabling" : "disabling", flag,
            targetState ? "enabled" : "disabled");
    }

public:
    GlFlagSetter() : origState(glIsEnabled(flag)) {
        if (origState != targetState) {
            logChange(targetState);
            if (targetState) glEnable(flag);
            else glDisable(flag);
        }
    }

    ~GlFlagSetter() {
        if (origState != targetState) {
            logChange(origState);
            if (origState) glEnable(flag);
            else glDisable(flag);
        }
    }
};

class TextureImplementation : public Texture {
private:
    const GLuint bindType;
    GLuint id;

public:
    TextureImplementation(int width, int height, const ImageTypeSpec &spec)
    : bindType(getBindType(spec)), id(0) {
        glGenTextures(1, &id);
        LOG_TRACE("created texture %d of size %d x %d x %d", id, width, height, spec.channels);

        // glActiveTexture(GL_TEXTURE0); // TODO: required?

        Binder binder(*this);
        glTexImage2D(GL_TEXTURE_2D, 0,
            getTextureInternalFormat(spec),
            width, height, 0,
            getCpuFormat(spec),
            getCpuType(spec), nullptr);

        // TODO: move somewhere else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

        checkError(__FUNCTION__);
    }

    void destroy() final {
        if (id != 0) {
            LOG_TRACE("deleting texture %d", id);
            glDeleteTextures(1, &id);
        }
        id = 0;
    }

    ~TextureImplementation() {
        if (id != 0) {
            log_warn("leaking GL texture");
        }
    }

    void bind() final {
        glBindTexture(bindType, id);
        LOG_TRACE("bound texture %d", id);
        checkError(__FUNCTION__);
    }

    void unbind() final {
        // NOTE/TODO: to be most "correct" this should not assume that no
        // texture was bound before bind() was called but restore the one
        // that was.
        // See: https://www.khronos.org/opengl/wiki/Common_Mistakes

        // However there is a little practical benefit in making this work
        // optimally in the middle of any other OpenGL processing. Usually
        // whatever other operation cares about the bound texture state will
        // just overwrite this anyway.
        glBindTexture(bindType, 0);
        LOG_TRACE("unbound texture");
        checkError(__FUNCTION__);
    }

    int getId() const { return id; }
};

class FrameBufferImplementation : public FrameBuffer {
private:
    int width, height;
    ImageTypeSpec spec;
    GLuint id;
    TextureImplementation texture;

public:
    FrameBufferImplementation(int w, int h, const ImageTypeSpec &spec)
    : width(w), height(h), spec(spec), id(0), texture(w, h, spec) {
        glGenFramebuffers(1, &id);
        LOG_TRACE("generated frame buffer %d", id);
        checkError(std::string(__FUNCTION__) + "/glGenFramebuffers");

        assert(spec.storageType == Image::StorageType::GPU_OPENGL);

        Binder binder(*this);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.getId(), 0);
        checkError(std::string(__FUNCTION__) + "/glFramebufferTexture2D");
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }

    void destroy() final {
        if (id != 0) {
            LOG_TRACE("destroying frame buffer %d", id);
            glDeleteFramebuffers(1, &id);
        }
        id = 0;
        texture.destroy();
    }

    int getWidth() const final { return width; }
    int getHeight() const final { return height; }

    ~FrameBufferImplementation() {
        if (id != 0) {
            log_warn("leaking frame buffer %d", id);
        }
    }

    void bind() final {
        LOG_TRACE("bound frame buffer %d", id);
        glBindFramebuffer(GL_FRAMEBUFFER, id);
        checkError(__FUNCTION__);
    }

    void unbind() final {
        LOG_TRACE("unbound frame buffer");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        checkError(__FUNCTION__);
    }

    void setViewport() final {
        LOG_TRACE("glViewport(0, 0, %d, %d)", width, height);
        glViewport(0, 0, width, height);
        checkError(__FUNCTION__);
    }

    void readPixels(uint8_t *pixels) final {
        LOG_TRACE("reading frame buffer %d", id);
        Binder binder(*this);
        // Note: OpenGL ES only supports GL_RGBA / GL_UNSIGNED_BYTE (in practice)
        // Note: check this
        // https://www.khronos.org/opengl/wiki/Common_Mistakes#Slow_pixel_transfer_performance
        glReadPixels(0, 0, width, height, getReadPixelFormat(spec), getCpuType(spec), pixels);
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        checkError(__FUNCTION__);
    }

    void writePixels(const uint8_t *pixels) final {
        Binder binder(texture);

        glTexImage2D(GL_TEXTURE_2D, 0,
            getTextureInternalFormat(spec),
            width, height, 0,
            getCpuFormat(spec),
            getCpuType(spec),
            pixels);
        checkError(__FUNCTION__);
    }

    int getId() const { return id; }

    int getTextureId() const final {
        return texture.getId();
    }
};

static GLuint loadShader(GLenum shaderType, const char* shaderSource) {
    const GLuint shader = glCreateShader(shaderType);
    assert(shader);

    LOG_TRACE("compiling shader:\n %s\n", shaderSource);

    glShaderSource(shader, 1, &shaderSource, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (!compiled) {
        GLint len = 0;

        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        assert(len);

        std::vector<char> buf(static_cast<std::size_t>(len));
        glGetShaderInfoLog(shader, len, nullptr, buf.data());
        log_error("Error compiling shader:\n%s", buf.data());
        log_error("Failing shader source:\n%s", shaderSource);
        glDeleteShader(shader);
        assert(false);
    }

    return shader;
}

GLuint createProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = glCreateProgram();
    assert(program);
    glAttachShader(program, vertexShader);
    checkError("glAttachShader");
    glAttachShader(program, fragmentShader);
    checkError("glAttachShader");
    glLinkProgram(program);
    GLint linkStatus = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus != GL_TRUE) {
        GLint bufLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
        if (bufLength) {
            std::vector<char> buf(static_cast<std::size_t>(bufLength));
            glGetProgramInfoLog(program, bufLength, nullptr, buf.data());
            log_error("Could not link program:\n%s", buf.data());
        }
        glDeleteProgram(program);
        assert(false);
    }
    return program;
}

class GlslProgramImplementation : public GlslProgram {
private:
    GLuint program;

public:
    GlslProgramImplementation(const char *vs, const char *fs)
    : program(createProgram(vs, fs)) {}

    int getId() const final { return program; }

    void bind() final {
        LOG_TRACE("activating shader: glUseProgram(%d)", program);
        glUseProgram(program);
    }

    void unbind() final {
        LOG_TRACE("deactivating shader: glUseProgram(0)", program);
        glUseProgram(0);
    }

    void destroy() final {
        if (program != 0) {
            LOG_TRACE("deleting GL program %d", program);
            glDeleteProgram(program);
            program = 0;
        }
    }

    ~GlslProgramImplementation() {
        if (program != 0) {
            log_warn("leaking GL program %d", program);
        }
    }
};

class GlslFragmentShaderImplementation : public GlslFragmentShader {
private:
    GLuint vertexBuffer = 0, vertexIndexBuffer = 0;
    GLuint aVertexData = 0;
    GlslProgramImplementation program;

    static std::string vertexShaderSource(bool withTexCoord) {
        const char *varyingTexCoordName = "v_texCoord";

        std::ostringstream oss;
        oss << "#version 300 es\n";
        oss << "precision highp float;\n";
        oss << "attribute vec4 a_vertexData;\n";
        if (withTexCoord) {
            oss << "out vec2 " << varyingTexCoordName << ";\n";
        }

        oss << "void main() {\n";

        if (withTexCoord) {
            oss << varyingTexCoordName << " = a_vertexData.zw;\n";
        }

        oss << "gl_Position = vec4(a_vertexData.xy, 0, 1);\n";
        oss << "}\n";

        return oss.str();
    }

public:
    GlslFragmentShaderImplementation(const char *fragementShaderSource, bool withTexCoord = true)
    : program(vertexShaderSource(withTexCoord).c_str(), fragementShaderSource)
    {
        glGenBuffers(1, &vertexBuffer);
        glGenBuffers(1, &vertexIndexBuffer);

        // Set up vertices
        float vertexData[] {
                // x, y, u, v
                -1, -1, 0, 0,
                -1, 1, 0, 1,
                1, 1, 1, 1,
                1, -1, 1, 0
        };
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertexData), vertexData, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Set up indices
        GLuint indices[] { 2, 1, 0, 0, 3, 2 };
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertexIndexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        aVertexData = glGetAttribLocation(program.getId(), "a_vertexData");
    }

    void destroy() final {
        if (vertexBuffer != 0) {
            glDeleteBuffers(1, &vertexBuffer);
            glDeleteBuffers(1, &vertexIndexBuffer);
        }
        program.destroy();
    }

    // if this leaks, then the "program" member also leaks, which produces
    // a warning so this can be noticed without customizing the dtor here

    void bind() final {
        program.bind();

        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vertexIndexBuffer);
        checkError(std::string(__FUNCTION__) + "/glBindBuffer x 2");

        glEnableVertexAttribArray(aVertexData);
        glVertexAttribPointer(aVertexData, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        checkError(std::string(__FUNCTION__) + "/glVertexAttribPointer(aVertexData, ...)");
    }

    void unbind() final {
        glDisableVertexAttribArray(aVertexData);
        checkError(std::string(__FUNCTION__) + "/glDisableVertexAttribArray x 2");

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        checkError(std::string(__FUNCTION__) + "/glBindBuffer x 2");

        program.unbind();
    }

    void call(FrameBuffer &frameBuffer) final {
        // might typically be enabled, thus checking
        GlFlagSetter<GL_DEPTH_TEST, false> noDepthTest;
        GlFlagSetter<GL_BLEND, false> noBlend;
        // GlFlagSetter<GL_ALPHA_TEST, false> noAlphaTest;

        Binder frameBufferBinder(frameBuffer);
        frameBuffer.setViewport();

        // GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 }; // single output at location 0
        // glDrawBuffers(sizeof(bufs), bufs);
        // checkError(__FUNCTION__);

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        checkError(__FUNCTION__);
    }

    int getId() const final {
        return program.getId();
    }
};

class TextureUniformBinder : public Binder::Target {
private:
    const unsigned slot;
    const GLuint bindType, uniformId;
    int textureId = -1;

public:
    TextureUniformBinder(unsigned slot, GLuint bindType, GLuint uniformId)
    : slot(slot), bindType(bindType), uniformId(uniformId) {
        LOG_TRACE("got texture uniform %d for slot %u", uniformId, slot);
    }

    void bind() final {
        LOG_TRACE("bind texture / uniform at slot %u -> %d", slot, textureId);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(bindType, textureId);
        glUniform1i(uniformId, slot);
    }

    void unbind() final {
        LOG_TRACE("unbind texture / uniform at slot %u", slot);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(bindType, 0);
        // restore active texture to the default slot
        glActiveTexture(GL_TEXTURE0);
    }

    TextureUniformBinder &setTextureId(int id) {
        textureId = id;
        return *this;
    }
};

class GlslPipelineImplementation : public GlslPipeline {
private:
    GLuint outSizeUniform;
    GlslFragmentShaderImplementation program;
    std::vector<TextureUniformBinder> textureBinders;

    std::string textureName(unsigned index, unsigned nTextures) const {
        std::ostringstream oss;
        assert(index < nTextures);
        oss << "u_texture";
        if (nTextures >= 2 || index > 1) {
            oss << (index + 1);
        }
        return oss.str();
    }

    static std::string outSizeName() {
        return "u_outSize";
    }

    static bool hasExternal(const std::vector<ImageTypeSpec> &inputs) {
        for (const auto &in : inputs)
            if (getBindType(in) != GL_TEXTURE_2D) return true;
        return false;
    }

    std::string buildShaderSource(const char *fragmentMain, const std::vector<ImageTypeSpec> &inputs, const ImageTypeSpec &output) const {
        std::ostringstream oss;
        oss << "#version 300 es\n";
        if (hasExternal(inputs)) {
            oss << "#extension GL_OES_EGL_image_external : require\n";
        }
        oss << "layout(location = 0) out " << getGlslVecType(output) << " outValue;\n";
        oss << "precision highp float;\n";

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            oss << "uniform " << getGlslSamplerType(inputs.at(i)) << " " << textureName(i, inputs.size()) << ";\n";
        }

        oss << "uniform vec2 " << outSizeName() << ";\n";
        oss << "in vec2 v_texCoord;\n";
        oss << fragmentMain;
        oss << std::endl;
        return oss.str();
    }

public:
    GlslPipelineImplementation(const char *fragmentMain, const std::vector<ImageTypeSpec> &inputs, const ImageTypeSpec &output)
    :
        outSizeUniform(0),
        program(buildShaderSource(fragmentMain, inputs, output).c_str())
    {
        outSizeUniform = glGetUniformLocation(program.getId(), outSizeName().c_str());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            textureBinders.push_back(TextureUniformBinder(
                i,
                getBindType(inputs.at(i)),
                glGetUniformLocation(program.getId(), textureName(i, inputs.size()).c_str())
            ));
        }
        checkError(__FUNCTION__);
    }

    Binder::Target &bindTexture(unsigned index, int textureId) final {
        return textureBinders.at(index).setTextureId(textureId);
    }

    void call(FrameBuffer &frameBuffer) final {
        LOG_TRACE("setting out size uniform");
        glUniform2f(outSizeUniform, frameBuffer.getWidth(), frameBuffer.getHeight());

        checkError(__FUNCTION__);
        program.call(frameBuffer);
    }

    void destroy() final { program.destroy(); }
    void bind() final { program.bind(); }
    void unbind() final { program.unbind(); }
    int getId() const final { return program.getId(); }
};

}

Binder::Binder(Target &target) : target(target) { target.bind(); }
Binder::~Binder() { target.unbind(); }
Destroyable::~Destroyable() = default;

std::unique_ptr<Texture> Texture::create(int w, int h, const ImageTypeSpec &spec) {
    return std::unique_ptr<Texture>(new TextureImplementation(w, h, spec));
}

std::unique_ptr<FrameBuffer> FrameBuffer::create(int w, int h, const ImageTypeSpec &spec) {
    return std::unique_ptr<FrameBuffer>(new FrameBufferImplementation(w, h, spec));
}

std::unique_ptr<GlslProgram> GlslProgram::create(const char *vs, const char *fs) {
    return std::unique_ptr<GlslProgram>(new GlslProgramImplementation(vs, fs));
}

std::unique_ptr<GlslFragmentShader> GlslFragmentShader::create(const char *fragementShaderSource) {
    return std::unique_ptr<GlslFragmentShader>(new GlslFragmentShaderImplementation(fragementShaderSource));
}

std::unique_ptr<GlslPipeline> GlslPipeline::create(const char *fragmentMain, const std::vector<ImageTypeSpec> &inputs, const ImageTypeSpec &output) {
    return std::unique_ptr<GlslPipeline>(new GlslPipelineImplementation(fragmentMain, inputs, output));
}

}
}
