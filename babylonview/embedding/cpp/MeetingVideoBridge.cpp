#include "MeetingVideoBridge.h"

#include <AndroidExtensions/Globals.h>
#include <AndroidExtensions/OpenGLHelpers.h>
#include <Babylon/Graphics/GL/Texture.h>

#include <android/log.h>
#include <bgfx/bgfx.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* LogTag{"BabylonMeetingVideo"};

    constexpr char VertexShader[]{R"(#version 300 es
        precision highp float;
        uniform mat4 textureTransform;
        out vec2 uv;
        const vec2 positions[4] = vec2[4](
            vec2(-1.0, -1.0),
            vec2( 1.0, -1.0),
            vec2(-1.0,  1.0),
            vec2( 1.0,  1.0));
        const vec2 textureCoordinates[4] = vec2[4](
            vec2(0.0, 0.0),
            vec2(1.0, 0.0),
            vec2(0.0, 1.0),
            vec2(1.0, 1.0));
        void main() {
            gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
            uv = (textureTransform * vec4(textureCoordinates[gl_VertexID], 0.0, 1.0)).xy;
        }
    )"};

    constexpr char FragmentShader[]{R"(#version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision mediump float;
        in vec2 uv;
        uniform samplerExternalOES videoTexture;
        layout(location = 0) out vec4 color;
        void main() {
            color = texture(videoTexture, uv);
        }
    )"};

    void ThrowIfGlError(const char* operation)
    {
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            throw std::runtime_error{
                std::string{operation} + " failed with OpenGL error " + std::to_string(error)};
        }
    }

    class ScopedEglContext final
    {
    public:
        ScopedEglContext(EGLDisplay display, EGLContext context)
            : m_previousDisplay{eglGetCurrentDisplay()}
            , m_previousContext{eglGetCurrentContext()}
            , m_previousDrawSurface{eglGetCurrentSurface(EGL_DRAW)}
            , m_previousReadSurface{eglGetCurrentSurface(EGL_READ)}
            , m_display{display}
        {
            if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE)
            {
                throw std::runtime_error{
                    "Unable to make the meeting-video EGL context current. Error: " +
                    std::to_string(eglGetError())};
            }
        }

        ~ScopedEglContext()
        {
            if (m_previousDisplay != EGL_NO_DISPLAY)
            {
                eglMakeCurrent(
                    m_previousDisplay,
                    m_previousDrawSurface,
                    m_previousReadSurface,
                    m_previousContext);
            }
            else
            {
                eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }

    private:
        EGLDisplay m_previousDisplay;
        EGLContext m_previousContext;
        EGLSurface m_previousDrawSurface;
        EGLSurface m_previousReadSurface;
        EGLDisplay m_display;
    };

    jmethodID GetSurfaceTextureMethod(
        JNIEnv* env,
        jobject surfaceTexture,
        const char* name,
        const char* signature)
    {
        jclass surfaceTextureClass = env->GetObjectClass(surfaceTexture);
        if (surfaceTextureClass == nullptr)
        {
            throw std::runtime_error{"Unable to resolve android.graphics.SurfaceTexture."};
        }

        jmethodID method = env->GetMethodID(surfaceTextureClass, name, signature);
        env->DeleteLocalRef(surfaceTextureClass);
        if (method == nullptr)
        {
            throw std::runtime_error{
                std::string{"Unable to resolve SurfaceTexture."} + name + signature};
        }
        return method;
    }

    void ThrowIfJavaException(JNIEnv* env, const char* operation)
    {
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
            throw std::runtime_error{std::string{operation} + " failed."};
        }
    }
}

namespace Babylon::Embedding::Android
{
    struct MeetingVideoBridge::Impl
    {
        struct TextureEntry
        {
            jobject surfaceTexture{};
            uint32_t width{};
            uint32_t height{};
            GLuint oesTexture{};
            GLuint rgbaTexture{};
            GLuint framebuffer{};
            bool attached{};
            Graphics::GL::SharedPtr<Graphics::GL::Texture> texture{};
            std::optional<Plugins::ExternalTexture> externalTexture{};
        };

        struct RetiredTexture
        {
            GLuint rgbaTexture{};
            Graphics::GL::SharedPtr<Graphics::GL::Texture> texture{};
        };

        ~Impl()
        {
            if (!entries.empty())
            {
                JNIEnv* env = android::global::GetEnvForCurrentThread();
                std::vector<int32_t> videoObjectIds{};
                videoObjectIds.reserve(entries.size());
                for (const auto& [videoObjectId, entry] : entries)
                {
                    (void)entry;
                    videoObjectIds.push_back(videoObjectId);
                }
                for (const int32_t videoObjectId : videoObjectIds)
                {
                    try
                    {
                        ReleaseSurfaceTexture(env, videoObjectId);
                    }
                    catch (const std::exception& exception)
                    {
                        __android_log_print(
                            ANDROID_LOG_ERROR,
                            LogTag,
                            "SurfaceTexture cleanup failed: %s",
                            exception.what());
                    }
                }
            }

            if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT)
            {
                try
                {
                    ScopedEglContext scopedContext{display, context};
                    for (auto& retired : retiredTextures)
                    {
                        if (retired.rgbaTexture != 0)
                        {
                            glDeleteTextures(1, &retired.rgbaTexture);
                        }
                    }
                    if (vertexArray != 0)
                    {
                        glDeleteVertexArrays(1, &vertexArray);
                    }
                    if (program != 0)
                    {
                        glDeleteProgram(program);
                    }
                    glFlush();
                }
                catch (const std::exception& exception)
                {
                    __android_log_print(
                        ANDROID_LOG_ERROR,
                        LogTag,
                        "Meeting-video cleanup failed: %s",
                        exception.what());
                }
            }

            if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT)
            {
                eglDestroyContext(display, context);
            }
        }

        void EnsureContext()
        {
            if (context != EGL_NO_CONTEXT)
            {
                return;
            }

            const bgfx::InternalData* internalData = bgfx::getInternalData();
            if (internalData == nullptr || internalData->context == nullptr)
            {
                throw std::runtime_error{
                    "Babylon graphics must be initialized before creating a meeting-video texture."};
            }

            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE)
            {
                throw std::runtime_error{"Unable to initialize EGL for meeting video."};
            }

            constexpr EGLint configAttributes[]{
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 16,
                EGL_STENCIL_SIZE, 8,
                EGL_NONE};

            EGLConfig config{};
            EGLint configCount{};
            if (eglChooseConfig(display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
                configCount == 0)
            {
                throw std::runtime_error{
                    "Unable to choose an EGL configuration for meeting video. Error: " +
                    std::to_string(eglGetError())};
            }

            constexpr EGLint contextAttributes[]{
                EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                EGL_CONTEXT_MINOR_VERSION_KHR, 0,
                EGL_NONE};

            context = eglCreateContext(
                display,
                config,
                static_cast<EGLContext>(internalData->context),
                contextAttributes);
            if (context == EGL_NO_CONTEXT)
            {
                throw std::runtime_error{
                    "Unable to create the meeting-video EGL context. Error: " +
                    std::to_string(eglGetError())};
            }

            ScopedEglContext scopedContext{display, context};
            program = android::OpenGLHelpers::CreateShaderProgram(VertexShader, FragmentShader);
            glGenVertexArrays(1, &vertexArray);
            ThrowIfGlError("glGenVertexArrays");
        }

        jobject CreateSurfaceTexture(
            JNIEnv* env,
            int32_t videoObjectId,
            uint32_t width,
            uint32_t height)
        {
            if (videoObjectId <= 0)
            {
                throw std::invalid_argument{"videoObjectId must be positive."};
            }
            if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
            {
                throw std::invalid_argument{"Meeting-video dimensions are invalid."};
            }

            auto existing = entries.find(videoObjectId);
            if (existing != entries.end())
            {
                return env->NewLocalRef(existing->second.surfaceTexture);
            }

            jclass surfaceTextureClass = env->FindClass("android/graphics/SurfaceTexture");
            if (surfaceTextureClass == nullptr)
            {
                throw std::runtime_error{"Unable to find android.graphics.SurfaceTexture."};
            }

            jmethodID constructor = env->GetMethodID(surfaceTextureClass, "<init>", "(Z)V");
            if (constructor == nullptr)
            {
                env->DeleteLocalRef(surfaceTextureClass);
                throw std::runtime_error{"Detached SurfaceTexture constructor is unavailable."};
            }

            jobject localSurfaceTexture =
                env->NewObject(surfaceTextureClass, constructor, static_cast<jboolean>(JNI_FALSE));
            env->DeleteLocalRef(surfaceTextureClass);
            ThrowIfJavaException(env, "SurfaceTexture construction");
            if (localSurfaceTexture == nullptr)
            {
                throw std::runtime_error{"SurfaceTexture construction returned null."};
            }

            TextureEntry entry{};
            entry.surfaceTexture = env->NewGlobalRef(localSurfaceTexture);
            entry.width = width;
            entry.height = height;
            if (entry.surfaceTexture == nullptr)
            {
                env->DeleteLocalRef(localSurfaceTexture);
                throw std::runtime_error{"Unable to retain the meeting-video SurfaceTexture."};
            }

            jmethodID setDefaultBufferSize =
                GetSurfaceTextureMethod(env, localSurfaceTexture, "setDefaultBufferSize", "(II)V");
            env->CallVoidMethod(
                localSurfaceTexture,
                setDefaultBufferSize,
                static_cast<jint>(width),
                static_cast<jint>(height));
            ThrowIfJavaException(env, "SurfaceTexture.setDefaultBufferSize");

            entries.emplace(videoObjectId, std::move(entry));
            return localSurfaceTexture;
        }

        Plugins::ExternalTexture AttachSurfaceTexture(JNIEnv* env, int32_t videoObjectId)
        {
            auto iterator = entries.find(videoObjectId);
            if (iterator == entries.end())
            {
                throw std::invalid_argument{"Unknown meeting-video object ID."};
            }

            auto& entry = iterator->second;
            if (entry.externalTexture)
            {
                return *entry.externalTexture;
            }

            EnsureContext();
            ScopedEglContext scopedContext{display, context};

            glGenTextures(1, &entry.oesTexture);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry.oesTexture);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            ThrowIfGlError("Meeting-video OES texture creation");

            jmethodID attachToGlContext =
                GetSurfaceTextureMethod(env, entry.surfaceTexture, "attachToGLContext", "(I)V");
            env->CallVoidMethod(
                entry.surfaceTexture,
                attachToGlContext,
                static_cast<jint>(entry.oesTexture));
            ThrowIfJavaException(env, "SurfaceTexture.attachToGLContext");
            entry.attached = true;

            glGenTextures(1, &entry.rgbaTexture);
            glBindTexture(GL_TEXTURE_2D, entry.rgbaTexture);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA8,
                static_cast<GLsizei>(entry.width),
                static_cast<GLsizei>(entry.height),
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            glGenFramebuffers(1, &entry.framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, entry.framebuffer);
            glFramebufferTexture2D(
                GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D,
                entry.rgbaTexture,
                0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            {
                throw std::runtime_error{"Meeting-video framebuffer is incomplete."};
            }
            glViewport(
                0,
                0,
                static_cast<GLsizei>(entry.width),
                static_cast<GLsizei>(entry.height));
            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            ThrowIfGlError("Meeting-video RGBA texture creation");

            entry.texture = Graphics::GL::MakeTexture(
                Graphics::GL::Texture::Descriptor{
                    .Handle = entry.rgbaTexture,
                    .Width = entry.width,
                    .Height = entry.height,
                    .Layers = 1,
                    .Format = GL_RGBA8,
                    .Usage = Graphics::GL::Texture::Usage::Sampled});
            entry.externalTexture.emplace(entry.texture.Get());
            return *entry.externalTexture;
        }

        void Update(JNIEnv* env)
        {
            if (context == EGL_NO_CONTEXT || entries.empty())
            {
                return;
            }

            ScopedEglContext scopedContext{display, context};
            glBindVertexArray(vertexArray);
            glUseProgram(program);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            const GLint textureTransformLocation =
                glGetUniformLocation(program, "textureTransform");
            const GLint videoTextureLocation = glGetUniformLocation(program, "videoTexture");
            glUniform1i(videoTextureLocation, 0);

            for (auto& [id, entry] : entries)
            {
                (void)id;
                if (!entry.attached)
                {
                    continue;
                }

                jmethodID updateTexImage =
                    GetSurfaceTextureMethod(env, entry.surfaceTexture, "updateTexImage", "()V");
                env->CallVoidMethod(entry.surfaceTexture, updateTexImage);
                if (env->ExceptionCheck())
                {
                    env->ExceptionClear();
                    continue;
                }

                std::array<jfloat, 16> transform{};
                jfloatArray transformArray = env->NewFloatArray(transform.size());
                if (transformArray == nullptr)
                {
                    continue;
                }
                jmethodID getTransformMatrix =
                    GetSurfaceTextureMethod(env, entry.surfaceTexture, "getTransformMatrix", "([F)V");
                env->CallVoidMethod(entry.surfaceTexture, getTransformMatrix, transformArray);
                if (env->ExceptionCheck())
                {
                    env->ExceptionClear();
                    env->DeleteLocalRef(transformArray);
                    continue;
                }
                env->GetFloatArrayRegion(
                    transformArray,
                    0,
                    transform.size(),
                    transform.data());
                env->DeleteLocalRef(transformArray);

                glBindFramebuffer(GL_FRAMEBUFFER, entry.framebuffer);
                glViewport(
                    0,
                    0,
                    static_cast<GLsizei>(entry.width),
                    static_cast<GLsizei>(entry.height));
                glUniformMatrix4fv(textureTransformLocation, 1, GL_FALSE, transform.data());
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry.oesTexture);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glUseProgram(0);
            glBindVertexArray(0);
            glFlush();
            ThrowIfGlError("Meeting-video texture update");
        }

        void ReleaseSurfaceTexture(JNIEnv* env, int32_t videoObjectId)
        {
            auto iterator = entries.find(videoObjectId);
            if (iterator == entries.end())
            {
                return;
            }

            auto entry = std::move(iterator->second);
            entries.erase(iterator);

            if (context != EGL_NO_CONTEXT)
            {
                ScopedEglContext scopedContext{display, context};
                if (entry.attached)
                {
                    jmethodID detachFromGlContext =
                        GetSurfaceTextureMethod(
                            env,
                            entry.surfaceTexture,
                            "detachFromGLContext",
                            "()V");
                    env->CallVoidMethod(entry.surfaceTexture, detachFromGlContext);
                    if (env->ExceptionCheck())
                    {
                        env->ExceptionClear();
                    }
                }

                if (entry.oesTexture != 0)
                {
                    glDeleteTextures(1, &entry.oesTexture);
                }
                if (entry.framebuffer != 0)
                {
                    glDeleteFramebuffers(1, &entry.framebuffer);
                }
                glFlush();
            }

            jmethodID release =
                GetSurfaceTextureMethod(env, entry.surfaceTexture, "release", "()V");
            env->CallVoidMethod(entry.surfaceTexture, release);
            if (env->ExceptionCheck())
            {
                env->ExceptionClear();
            }
            env->DeleteGlobalRef(entry.surfaceTexture);
            entry.surfaceTexture = nullptr;

            if (entry.rgbaTexture != 0)
            {
                retiredTextures.push_back(
                    RetiredTexture{entry.rgbaTexture, std::move(entry.texture)});
                entry.rgbaTexture = 0;
            }
        }

        EGLDisplay display{EGL_NO_DISPLAY};
        EGLContext context{EGL_NO_CONTEXT};
        GLuint program{};
        GLuint vertexArray{};
        std::unordered_map<int32_t, TextureEntry> entries{};
        std::vector<RetiredTexture> retiredTextures{};
    };

    MeetingVideoBridge::MeetingVideoBridge()
        : m_impl{std::make_unique<Impl>()}
    {
    }

    MeetingVideoBridge::~MeetingVideoBridge() = default;

    jobject MeetingVideoBridge::CreateSurfaceTexture(
        JNIEnv* env,
        int32_t videoObjectId,
        uint32_t width,
        uint32_t height)
    {
        return m_impl->CreateSurfaceTexture(env, videoObjectId, width, height);
    }

    Plugins::ExternalTexture MeetingVideoBridge::AttachSurfaceTexture(
        JNIEnv* env,
        int32_t videoObjectId)
    {
        return m_impl->AttachSurfaceTexture(env, videoObjectId);
    }

    void MeetingVideoBridge::ReleaseSurfaceTexture(JNIEnv* env, int32_t videoObjectId)
    {
        m_impl->ReleaseSurfaceTexture(env, videoObjectId);
    }

    void MeetingVideoBridge::Update(JNIEnv* env)
    {
        m_impl->Update(env);
    }
}
