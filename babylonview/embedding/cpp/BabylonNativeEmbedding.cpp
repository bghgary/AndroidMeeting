// JNI interop for Babylon::Embedding on Android.
//
// This file is the C++ side of the Android interop layer. It exposes
// `extern "C" JNIEXPORT` entry points that any JVM-language host can call
// via matching `external fun` / `native` declarations.
//
// We deliberately ship no Kotlin/Java class library on top — interop ends
// at "Kotlin/Java can call into Babylon".
//
// Convention: opaque C++ pointers cross JNI as `jlong`.
// `unique_ptr::release()` transfers ownership to the JVM side; the
// matching `*Destroy` function `delete`s the raw pointer.
//
// Symbols here follow `Java_com_babylonjs_embedding_BabylonNative_*`.
// Hosts can use any class name as long as Java signatures match.

#include <Babylon/Embedding/Runtime.h>
#include <Babylon/Embedding/View.h>
#include <Babylon/Embedding/Android/RuntimeHandle.h>

#include <AndroidExtensions/Globals.h>

#include <napi/napi.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#if BABYLON_NATIVE_PLUGIN_SHADERCACHE
#include <Babylon/Plugins/ShaderCache.h>
#endif

#include <android/asset_manager.h>

#include <sstream>

#include <jni.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    using Babylon::Embedding::LogLevel;
    using Babylon::Embedding::Runtime;
    using Babylon::Embedding::RuntimeOptions;
    using Babylon::Embedding::View;

    constexpr const char* SetMeetingStageStateFunctionName{"setMeetingStageState"};
    constexpr const char* ResetMeetingStageFunctionName{"resetMeetingStage"};

    struct MeetingStageParticipantState
    {
        int32_t id{};
        std::string displayName{};
        bool muted{};
        bool videoOn{};
    };

    struct MeetingStageState
    {
        std::string stageId{};
        int32_t layout{};
        std::optional<int32_t> activeSpeakerId{};
        std::vector<MeetingStageParticipantState> participants{};
    };

    // Wraps a Runtime plus two `android::global` event tickets that
    // auto-Suspend/Resume on Activity lifecycle. Tickets are declared
    // AFTER the Runtime so they're destroyed BEFORE it — guaranteeing no
    // callback fires on a dead Runtime during teardown.
    //
    // Runtime is held by unique_ptr so its address is stable for the
    // ticket lambdas (the tickets aren't move-assignable, so we have to
    // know the address before constructing the wrapper).
    //
    // Runtime::Suspend/Resume are refcounted, so this composes safely
    // with explicit host-side runtimeSuspend / runtimeResume calls.
    struct AndroidRuntime
    {
        std::unique_ptr<Runtime> runtime;
        android::global::AppStateChangedCallbackTicket pauseTicket;
        android::global::AppStateChangedCallbackTicket resumeTicket;

#if BABYLON_NATIVE_PLUGIN_NATIVEXR
        // The ANativeWindow currently handed to Runtime::SetXrWindow.
        // NativeXr stores the raw pointer without acquiring a reference (and
        // only consumes it lazily at XR session creation on the JS thread),
        // so the JNI layer owns this reference: it's acquired via
        // ANativeWindow_fromSurface in runtimeSetXrSurface, released and
        // replaced when the surface changes, and released in runtimeDestroy
        // after the Runtime (and thus NativeXr) is torn down.
        ANativeWindow* xrWindow{nullptr};
#endif
    };

    AndroidRuntime* AsAndroidRuntime(jlong handle) { return reinterpret_cast<AndroidRuntime*>(handle); }
    Runtime* AsRuntime(jlong handle) { return AsAndroidRuntime(handle)->runtime.get(); }
    View* AsView(jlong handle) { return reinterpret_cast<View*>(handle); }

    std::mutex g_runtimeHandlesMutex;
    std::unordered_set<AndroidRuntime*> g_runtimeHandles;

    Napi::Function GetRequiredGlobalFunction(Napi::Env env, const char* name)
    {
        Napi::Value value = env.Global().Get(name);
        if (!value.IsFunction())
        {
            throw Napi::TypeError::New(
                env,
                std::string{"globalThis."} + name + " must be a function before meeting-stage state is sent.");
        }
        return value.As<Napi::Function>();
    }

    Napi::Object BuildMeetingStageState(Napi::Env env, const MeetingStageState& state)
    {
        auto jsState = Napi::Object::New(env);
        jsState.Set("stageId", Napi::String::New(env, state.stageId));
        jsState.Set("layout", Napi::Number::New(env, state.layout));
        jsState.Set(
            "activeSpeakerId",
            state.activeSpeakerId
                ? Napi::Value{Napi::Number::New(env, *state.activeSpeakerId)}
                : Napi::Value{env.Null()});

        auto jsParticipants = Napi::Array::New(env, state.participants.size());
        for (size_t index = 0; index < state.participants.size(); ++index)
        {
            const auto& participant = state.participants[index];
            auto jsParticipant = Napi::Object::New(env);
            jsParticipant.Set("id", Napi::Number::New(env, participant.id));
            jsParticipant.Set("displayName", Napi::String::New(env, participant.displayName));
            jsParticipant.Set("muted", Napi::Boolean::New(env, participant.muted));
            jsParticipant.Set("videoOn", Napi::Boolean::New(env, participant.videoOn));
            jsParticipants.Set(static_cast<uint32_t>(index), jsParticipant);
        }
        jsState.Set("participants", jsParticipants);
        return jsState;
    }

    // ---- Secondary-surface mirror (self-contained, GLES) ----
    // Each secondary SurfaceView gets its own EGL window surface + a context
    // sharing bgfx's main GL context. Each frame, the main backbuffer is copied
    // into a shared texture and drawn as a fullscreen quad on every surface, so
    // they mirror the primary render. All calls happen on the render thread.
    struct MirrorWindow
    {
        ANativeWindow* window;
        EGLSurface surface;
        EGLContext context;
        int width;
        int height;
        GLuint vao;
    };
    std::vector<MirrorWindow> g_mirrors;
    GLuint g_mirrorTex{0};
    GLuint g_mirrorProgram{0};
    int g_mirrorTexW{0};
    int g_mirrorTexH{0};

    void MirrorAddSurface(ANativeWindow* window, int w, int h)
    {
        EGLDisplay display = eglGetCurrentDisplay();
        EGLContext mainCtx = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || mainCtx == EGL_NO_CONTEXT)
        {
            return;
        }
        const EGLint cfgAttrs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        EGLConfig cfg{}; EGLint n = 0;
        eglChooseConfig(display, cfgAttrs, &cfg, 1, &n);
        EGLSurface sfc = eglCreateWindowSurface(display, cfg, window, nullptr);
        const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        EGLContext ctx = eglCreateContext(display, cfg, mainCtx, ctxAttrs);
        g_mirrors.push_back({window, sfc, ctx, w, h, 0});
    }

    void MirrorRemoveSurface(ANativeWindow* window)
    {
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface mainDraw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface mainRead = eglGetCurrentSurface(EGL_READ);
        EGLContext mainCtx = eglGetCurrentContext();
        for (auto it = g_mirrors.begin(); it != g_mirrors.end(); ++it)
        {
            if (it->window != window) { continue; }
            if (it->vao != 0)
            {
                eglMakeCurrent(display, it->surface, it->surface, it->context);
                glDeleteVertexArrays(1, &it->vao);
                eglMakeCurrent(display, mainDraw, mainRead, mainCtx);
            }
            eglDestroySurface(display, it->surface);
            eglDestroyContext(display, it->context);
            ANativeWindow_release(it->window);
            g_mirrors.erase(it);
            break;
        }
    }

    void MirrorFrame()
    {
        if (g_mirrors.empty()) { return; }
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface mainDraw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface mainRead = eglGetCurrentSurface(EGL_READ);
        EGLContext mainCtx = eglGetCurrentContext();
        GLint vp[4] = {0,0,0,0};
        glGetIntegerv(GL_VIEWPORT, vp);
        const int srcW = vp[2], srcH = vp[3];
        if (srcW <= 0 || srcH <= 0) { return; }

        if (g_mirrorProgram == 0)
        {
            const char* vs = "#version 300 es\nout vec2 v;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);v=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
            const char* fs = "#version 300 es\nprecision mediump float;in vec2 v;uniform sampler2D t;out vec4 c;void main(){c=texture(t,v);}";
            GLuint vsh=glCreateShader(GL_VERTEX_SHADER); glShaderSource(vsh,1,&vs,0); glCompileShader(vsh);
            GLuint fsh=glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fsh,1,&fs,0); glCompileShader(fsh);
            g_mirrorProgram=glCreateProgram(); glAttachShader(g_mirrorProgram,vsh); glAttachShader(g_mirrorProgram,fsh); glLinkProgram(g_mirrorProgram);
            glGenTextures(1,&g_mirrorTex);
        }
        if (srcW!=g_mirrorTexW || srcH!=g_mirrorTexH)
        {
            glBindTexture(GL_TEXTURE_2D,g_mirrorTex);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,srcW,srcH,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            g_mirrorTexW=srcW; g_mirrorTexH=srcH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        glBindTexture(GL_TEXTURE_2D,g_mirrorTex);
        glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,0,0,srcW,srcH,0);
        for (auto& m : g_mirrors)
        {
            eglMakeCurrent(display, m.surface, m.surface, m.context);
            if (m.vao==0) glGenVertexArrays(1,&m.vao);
            glBindVertexArray(m.vao);
            glViewport(0,0,m.width,m.height);
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(g_mirrorProgram);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g_mirrorTex);
            glUniform1i(glGetUniformLocation(g_mirrorProgram,"t"),0);
            glDrawArrays(GL_TRIANGLES,0,3);
            eglSwapBuffers(display, m.surface);
        }
        eglMakeCurrent(display, mainDraw, mainRead, mainCtx);
    }

    int LogPriorityFor(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::Verbose: return ANDROID_LOG_VERBOSE;
            case LogLevel::Log:     return ANDROID_LOG_INFO;
            case LogLevel::Warn:    return ANDROID_LOG_WARN;
            case LogLevel::Error:   return ANDROID_LOG_ERROR;
            case LogLevel::Fatal:   return ANDROID_LOG_FATAL;
        }
        return ANDROID_LOG_INFO;
    }

    // Convert a jstring to std::string. Returns empty string if `jstr`
    // is null or UTF lookup fails.
    std::string ToStdString(JNIEnv* env, jstring jstr)
    {
        if (jstr == nullptr)
        {
            return {};
        }
        const char* utf = env->GetStringUTFChars(jstr, nullptr);
        if (utf == nullptr)
        {
            return {};
        }
        std::string result{utf};
        env->ReleaseStringUTFChars(jstr, utf);
        return result;
    }

    void ThrowJavaException(JNIEnv* env, const char* className, const char* message)
    {
        jclass excClass = env->FindClass(className);
        if (excClass != nullptr)
        {
            env->ThrowNew(excClass, message);
        }
    }

    // Throws a Java IllegalStateException. Used when a plugin wasn't
    // compiled in, so the misconfiguration surfaces as a clean Java
    // exception rather than UnsatisfiedLinkError or silent no-op.
    // [[maybe_unused]]: when every plugin flag is enabled no `#else`
    // branch references this and -Werror=unused-function would fail.
    [[maybe_unused]] void ThrowPluginNotEnabled(JNIEnv* env, const char* message)
    {
        ThrowJavaException(env, "java/lang/IllegalStateException", message);
    }

    bool ReadJavaMeetingStageParticipants(
        JNIEnv* env,
        jobjectArray javaParticipants,
        std::vector<MeetingStageParticipantState>& participants)
    {
        if (javaParticipants == nullptr)
        {
            ThrowJavaException(env, "java/lang/IllegalArgumentException", "participants must not be null.");
            return false;
        }

        jclass participantClass =
            env->FindClass("com/babylonjs/embedding/BabylonNative$MeetingStageParticipantState");
        if (participantClass == nullptr)
        {
            return false;
        }

        jfieldID idField = env->GetFieldID(participantClass, "id", "I");
        jfieldID displayNameField =
            env->GetFieldID(participantClass, "displayName", "Ljava/lang/String;");
        jfieldID mutedField = env->GetFieldID(participantClass, "muted", "Z");
        jfieldID videoOnField = env->GetFieldID(participantClass, "videoOn", "Z");
        if (idField == nullptr || displayNameField == nullptr ||
            mutedField == nullptr || videoOnField == nullptr)
        {
            env->DeleteLocalRef(participantClass);
            return false;
        }

        const jsize count = env->GetArrayLength(javaParticipants);
        participants.reserve(static_cast<size_t>(count));
        for (jsize index = 0; index < count; ++index)
        {
            jobject javaParticipant = env->GetObjectArrayElement(javaParticipants, index);
            if (javaParticipant == nullptr)
            {
                env->DeleteLocalRef(participantClass);
                ThrowJavaException(
                    env,
                    "java/lang/IllegalArgumentException",
                    "participants must not contain null elements.");
                return false;
            }

            auto displayName =
                static_cast<jstring>(env->GetObjectField(javaParticipant, displayNameField));
            if (displayName == nullptr)
            {
                env->DeleteLocalRef(javaParticipant);
                env->DeleteLocalRef(participantClass);
                ThrowJavaException(
                    env,
                    "java/lang/IllegalArgumentException",
                    "participant displayName must not be null.");
                return false;
            }

            MeetingStageParticipantState participant{};
            participant.id = static_cast<int32_t>(env->GetIntField(javaParticipant, idField));
            participant.displayName = ToStdString(env, displayName);
            participant.muted = env->GetBooleanField(javaParticipant, mutedField) == JNI_TRUE;
            participant.videoOn = env->GetBooleanField(javaParticipant, videoOnField) == JNI_TRUE;
            env->DeleteLocalRef(displayName);
            env->DeleteLocalRef(javaParticipant);
            if (env->ExceptionCheck())
            {
                env->DeleteLocalRef(participantClass);
                return false;
            }
            participants.push_back(std::move(participant));
        }

        env->DeleteLocalRef(participantClass);
        return true;
    }

    bool ApplyJavaRuntimeOptions(JNIEnv* env, jobject javaOptions, RuntimeOptions& options)
    {
        if (javaOptions == nullptr)
        {
            return true;
        }

        jclass optionsClass = env->GetObjectClass(javaOptions);
        if (optionsClass == nullptr)
        {
            return false;
        }

        auto readBoolean = [&](const char* name, bool& target) {
            jfieldID field = env->GetFieldID(optionsClass, name, "Z");
            if (field == nullptr)
            {
                return false;
            }
            target = env->GetBooleanField(javaOptions, field) == JNI_TRUE;
            return true;
        };

        if (!readBoolean("enableDebugger", options.enableDebugger) ||
            !readBoolean("enableDebugTrace", options.enableDebugTrace) ||
            !readBoolean("waitForDebugger", options.waitForDebugger))
        {
            env->DeleteLocalRef(optionsClass);
            return false;
        }

        jfieldID msaaSamplesField = env->GetFieldID(optionsClass, "msaaSamples", "Ljava/lang/Integer;");
        if (msaaSamplesField == nullptr)
        {
            env->DeleteLocalRef(optionsClass);
            return false;
        }

        jobject msaaSamples = env->GetObjectField(javaOptions, msaaSamplesField);
        if (msaaSamples != nullptr)
        {
            jclass integerClass = env->GetObjectClass(msaaSamples);
            if (integerClass == nullptr)
            {
                env->DeleteLocalRef(msaaSamples);
                env->DeleteLocalRef(optionsClass);
                return false;
            }

            jmethodID intValue = env->GetMethodID(integerClass, "intValue", "()I");
            if (intValue == nullptr)
            {
                env->DeleteLocalRef(integerClass);
                env->DeleteLocalRef(msaaSamples);
                env->DeleteLocalRef(optionsClass);
                return false;
            }

            const jint value = env->CallIntMethod(msaaSamples, intValue);
            options.msaaSamples = value >= 0 && value <= 255 ? static_cast<uint8_t>(value) : 0;
            env->DeleteLocalRef(integerClass);
            env->DeleteLocalRef(msaaSamples);
        }

        jfieldID shaderCachePathField = env->GetFieldID(optionsClass, "shaderCachePath", "Ljava/lang/String;");
        if (shaderCachePathField == nullptr)
        {
            env->DeleteLocalRef(optionsClass);
            return false;
        }

        auto shaderCachePath = static_cast<jstring>(env->GetObjectField(javaOptions, shaderCachePathField));
        if (shaderCachePath != nullptr)
        {
#if BABYLON_NATIVE_PLUGIN_SHADERCACHE
            options.shaderCachePath = ToStdString(env, shaderCachePath);
            env->DeleteLocalRef(shaderCachePath);
            if (env->ExceptionCheck())
            {
                env->DeleteLocalRef(optionsClass);
                return false;
            }
#else
            env->DeleteLocalRef(shaderCachePath);
            env->DeleteLocalRef(optionsClass);
            ThrowPluginNotEnabled(env,
                "shaderCachePath was provided but BABYLON_NATIVE_PLUGIN_SHADERCACHE "
                "was not enabled at native build time.");
            return false;
#endif
        }

        env->DeleteLocalRef(optionsClass);
        return true;
    }

    // Shared body for the runtimeCreate JNI overloads. Installs the
    // default logcat sink, constructs the Runtime, and registers
    // Activity-lifecycle auto-Suspend/Resume tickets.
    jlong MakeRuntimeHandle(RuntimeOptions options)
    {
        options.log = [](LogLevel level, std::string_view message) {
            // logcat needs a NUL-terminated C string.
            std::string text{message};
            __android_log_write(LogPriorityFor(level), "BabylonNative", text.c_str());
        };

        auto runtime = std::make_unique<Runtime>(std::move(options));
        Runtime* runtimePtr = runtime.get();

        // Auto-Suspend/Resume on process-wide Activity lifecycle. Hosts
        // call androidGlobalPause/Resume once per state change; every
        // Runtime in the process reacts. Refcounted, so composes with
        // any host-side explicit suspend/resume.
        auto pauseTicket = android::global::AddPauseCallback([runtimePtr]() {
            runtimePtr->Suspend();
        });
        auto resumeTicket = android::global::AddResumeCallback([runtimePtr]() {
            runtimePtr->Resume();
        });

        auto wrapper = std::unique_ptr<AndroidRuntime>{new AndroidRuntime{
            std::move(runtime),
            std::move(pauseTicket),
            std::move(resumeTicket),
        }};

        AndroidRuntime* wrapperPtr = wrapper.get();
        {
            std::lock_guard lock{g_runtimeHandlesMutex};
            g_runtimeHandles.insert(wrapperPtr);
        }

        // Ownership transfers to the JVM side via the returned jlong.
        return reinterpret_cast<jlong>(wrapper.release());
    }
}

// Public handle-decoding entry point. Hosts that ship app-specific JNI
// helpers in `libBabylonNativeEmbedding.so` must use this rather than
// `reinterpret_cast<Runtime*>(handle)`, because each Runtime is wrapped
// in `AndroidRuntime` to hold Activity-lifecycle tickets.
namespace Babylon::Embedding::Android
{
    Runtime* RuntimeFromHandle(jlong handle)
    {
        return handle == 0 ? nullptr : AsRuntime(handle);
    }
}

extern "C"
{

// =====================================================================
// Android platform lifecycle (interop layer surface).
//
// Not part of the cross-platform Runtime/View API — exists because
// plugins like NativeCamera require the host to register the JavaVM and
// current Activity via AndroidExtensions::Globals.
// =====================================================================

// Set the application Context. Hosts call this once at app startup
// (typically from Application.onCreate) before constructing any Runtime.
// Calling more than once is harmless — Initialize replaces the existing
// Context global ref.
JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_setContext(
    JNIEnv* env, jclass, jobject context)
{
    JavaVM* javaVM{nullptr};
    if (env->GetJavaVM(&javaVM) != JNI_OK)
    {
        return;
    }
    android::global::Initialize(javaVM, context);
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_setCurrentActivity(
    JNIEnv*, jclass, jobject activity)
{
    android::global::SetCurrentActivity(activity);
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_pause(JNIEnv*, jclass)
{
    android::global::Pause();
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_resume(JNIEnv*, jclass)
{
    android::global::Resume();
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_requestPermissionsResult(
    JNIEnv* env, jclass, jint requestCode, jobjectArray permissions, jintArray grantResults)
{
    std::vector<std::string> nativePermissions{};
    const jsize permissionCount = env->GetArrayLength(permissions);
    nativePermissions.reserve(static_cast<size_t>(permissionCount));
    for (jsize i = 0; i < permissionCount; ++i)
    {
        auto perm = static_cast<jstring>(env->GetObjectArrayElement(permissions, i));
        nativePermissions.push_back(ToStdString(env, perm));
        env->DeleteLocalRef(perm);
    }

    std::vector<int32_t> nativeGrantResults{};
    const jsize grantCount = env->GetArrayLength(grantResults);
    if (grantCount > 0)
    {
        jint* grantElements = env->GetIntArrayElements(grantResults, nullptr);
        nativeGrantResults.assign(grantElements, grantElements + grantCount);
        env->ReleaseIntArrayElements(grantResults, grantElements, JNI_ABORT);
    }

    android::global::RequestPermissionsResult(
        static_cast<int32_t>(requestCode),
        nativePermissions,
        nativeGrantResults);
}

// =====================================================================
// Runtime
// =====================================================================

JNIEXPORT jlong JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeCreate__(JNIEnv*, jclass)
{
    // Default Android consumers want logcat output; Console polyfill,
    // uncaught JS exceptions, and (when enabled) DebugTrace all route
    // there. Hosts wanting different behavior use the C++ API directly.
    RuntimeOptions options{};
    return MakeRuntimeHandle(std::move(options));
}

JNIEXPORT jlong JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeCreate__Lcom_babylonjs_embedding_BabylonNative_00024RuntimeOptions_2(
    JNIEnv* env, jclass, jobject javaOptions)
{
    // RuntimeOptions is a Java object so callers have a stable
    // construction API as fields are added.
    RuntimeOptions options{};
    if (!ApplyJavaRuntimeOptions(env, javaOptions, options))
    {
        return 0;
    }
    return MakeRuntimeHandle(std::move(options));
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeDestroy(JNIEnv* env, jclass, jlong handle)
{
    AndroidRuntime* androidRuntime = AsAndroidRuntime(handle);
    {
        std::lock_guard lock{g_runtimeHandlesMutex};
        if (androidRuntime == nullptr || g_runtimeHandles.erase(androidRuntime) == 0)
        {
            ThrowJavaException(
                env,
                "java/lang/IllegalStateException",
                "runtimeDestroy received an invalid or destroyed Runtime handle.");
            return;
        }
    }

#if BABYLON_NATIVE_PLUGIN_NATIVEXR
    // Grab the XR window before tearing down the Runtime; it can only be
    // released once the Runtime (and NativeXr, which holds the raw pointer)
    // is gone.
    ANativeWindow* xrWindow = androidRuntime->xrWindow;
#endif

    // Reverse declaration order: tickets unsubscribe before the Runtime
    // is destroyed, so no callback fires on a dead Runtime.
    delete androidRuntime;

#if BABYLON_NATIVE_PLUGIN_NATIVEXR
    if (xrWindow != nullptr)
    {
        ANativeWindow_release(xrWindow);
    }
#endif
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeLoadScript(
    JNIEnv* env, jclass, jlong handle, jstring url)
{
    AsRuntime(handle)->LoadScript(ToStdString(env, url));
}

// Load a GPU shader cache from a local file path. Call after runtimeCreate and
// before the first viewAttach.
// Load a GPU shader cache directly from an Android asset (no on-disk copy).
// `assetName` is relative to the assets root (e.g. "shadercache.bin"). Reads
// via the process AAssetManager registered by setContext. Call after
// runtimeCreate and before the first viewAttach.
JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeLoadShaderCache(
    JNIEnv* env, jclass, jlong, jstring assetName)
{
#if BABYLON_NATIVE_PLUGIN_SHADERCACHE
    std::string name = ToStdString(env, assetName);
    AAssetManager* assetManager = android::global::GetAssetManager();
    if (assetManager == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, "BabylonNative",
            "runtimeLoadShaderCache: no AssetManager (call setContext first).");
        return;
    }

    AAsset* asset = AAssetManager_open(assetManager, name.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, "BabylonNative",
            "runtimeLoadShaderCache: asset '%s' not found.", name.c_str());
        return;
    }

    Babylon::Plugins::ShaderCache::Enable();

    const auto length = static_cast<size_t>(AAsset_getLength(asset));
    const char* buffer = static_cast<const char*>(AAsset_getBuffer(asset));
    std::istringstream stream{std::string{buffer, length}, std::ios::binary};
    uint32_t count = Babylon::Plugins::ShaderCache::Load(stream);
    AAsset_close(asset);

    __android_log_print(ANDROID_LOG_INFO, "BabylonNative",
        "Loaded shader cache asset '%s': %u entries", name.c_str(), count);
#else
    (void)env;
    (void)assetName;
    __android_log_print(ANDROID_LOG_ERROR, "BabylonNative",
        "runtimeLoadShaderCache: ShaderCache plugin disabled at build time.");
#endif
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeEval(
    JNIEnv* env, jclass, jlong handle, jstring source, jstring sourceUrl)
{
    AsRuntime(handle)->Eval(ToStdString(env, source), ToStdString(env, sourceUrl));
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeSetMeetingStageState(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring stageId,
    jint layout,
    jobject activeSpeakerId,
    jobjectArray participants)
{
    if (stageId == nullptr)
    {
        ThrowJavaException(env, "java/lang/IllegalArgumentException", "stageId must not be null.");
        return;
    }
    if (participants == nullptr)
    {
        ThrowJavaException(env, "java/lang/IllegalArgumentException", "participants must not be null.");
        return;
    }

    try
    {
        MeetingStageState state{};
        state.stageId = ToStdString(env, stageId);
        state.layout = static_cast<int32_t>(layout);
        if (env->ExceptionCheck())
        {
            return;
        }
        if (state.stageId.empty())
        {
            ThrowJavaException(env, "java/lang/IllegalArgumentException", "stageId must not be empty.");
            return;
        }

        if (activeSpeakerId != nullptr)
        {
            jclass integerClass = env->GetObjectClass(activeSpeakerId);
            if (integerClass == nullptr)
            {
                return;
            }
            jmethodID intValue = env->GetMethodID(integerClass, "intValue", "()I");
            if (intValue == nullptr)
            {
                env->DeleteLocalRef(integerClass);
                return;
            }
            state.activeSpeakerId =
                static_cast<int32_t>(env->CallIntMethod(activeSpeakerId, intValue));
            env->DeleteLocalRef(integerClass);
            if (env->ExceptionCheck())
            {
                return;
            }
        }

        if (!ReadJavaMeetingStageParticipants(env, participants, state.participants))
        {
            return;
        }

        std::lock_guard lock{g_runtimeHandlesMutex};
        AndroidRuntime* androidRuntime = AsAndroidRuntime(handle);
        if (androidRuntime == nullptr || !g_runtimeHandles.contains(androidRuntime))
        {
            ThrowJavaException(
                env,
                "java/lang/IllegalStateException",
                "runtimeSetMeetingStageState received an invalid or destroyed Runtime handle.");
            return;
        }

        androidRuntime->runtime->RunOnJsThread(
            [state = std::move(state)](Napi::Env jsEnv) {
                GetRequiredGlobalFunction(jsEnv, SetMeetingStageStateFunctionName)
                    .Call(jsEnv.Global(), {BuildMeetingStageState(jsEnv, state)});
            },
            true);
    }
    catch (const std::exception& exception)
    {
        ThrowJavaException(env, "java/lang/RuntimeException", exception.what());
    }
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeResetMeetingStage(
    JNIEnv* env, jclass, jlong handle, jstring stageId)
{
    if (stageId == nullptr)
    {
        ThrowJavaException(env, "java/lang/IllegalArgumentException", "stageId must not be null.");
        return;
    }

    try
    {
        std::string nativeStageId = ToStdString(env, stageId);
        if (env->ExceptionCheck())
        {
            return;
        }
        if (nativeStageId.empty())
        {
            ThrowJavaException(env, "java/lang/IllegalArgumentException", "stageId must not be empty.");
            return;
        }

        std::lock_guard lock{g_runtimeHandlesMutex};
        AndroidRuntime* androidRuntime = AsAndroidRuntime(handle);
        if (androidRuntime == nullptr || !g_runtimeHandles.contains(androidRuntime))
        {
            ThrowJavaException(
                env,
                "java/lang/IllegalStateException",
                "runtimeResetMeetingStage received an invalid or destroyed Runtime handle.");
            return;
        }

        androidRuntime->runtime->RunOnJsThread(
            [stageId = std::move(nativeStageId)](Napi::Env jsEnv) {
                GetRequiredGlobalFunction(jsEnv, ResetMeetingStageFunctionName)
                    .Call(jsEnv.Global(), {Napi::String::New(jsEnv, stageId)});
            },
            true);
    }
    catch (const std::exception& exception)
    {
        ThrowJavaException(env, "java/lang/RuntimeException", exception.what());
    }
}

// No per-Runtime Suspend/Resume on the JNI surface: lifecycle wiring
// happens automatically in MakeRuntimeHandle via the android::global
// pause/resume tickets. Hosts call androidGlobalPause/Resume once per
// Activity state change; every Runtime in the process reacts. Use the
// C++ API for finer-grained control.

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeSetXrSurface(
    JNIEnv* env, jclass, jlong handle, jobject surface)
{
#if BABYLON_NATIVE_PLUGIN_NATIVEXR
    AndroidRuntime* androidRuntime = AsAndroidRuntime(handle);

    ANativeWindow* window{nullptr};
    if (surface != nullptr)
    {
        // +1 reference owned by us; released when replaced below or in
        // runtimeDestroy. NativeXr only stores the raw pointer.
        window = ANativeWindow_fromSurface(env, surface);
    }

    androidRuntime->runtime->SetXrWindow(window);

    // Release the previously-held window now that NativeXr has been pointed
    // at the new one. Hosts must not swap the XR surface out from under an
    // active session; the usual flow is set(surface) … set(null) bracketing
    // a session.
    if (androidRuntime->xrWindow != nullptr)
    {
        ANativeWindow_release(androidRuntime->xrWindow);
    }
    androidRuntime->xrWindow = window;
#else
    (void)handle;
    (void)surface;
    ThrowPluginNotEnabled(env,
        "runtimeSetXrSurface was called but BABYLON_NATIVE_PLUGIN_NATIVEXR "
        "was not enabled at native build time.");
#endif
}

JNIEXPORT jboolean JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeIsXrActive(JNIEnv*, jclass, jlong handle)
{
#if BABYLON_NATIVE_PLUGIN_NATIVEXR
    return AsRuntime(handle)->IsXrActive() ? JNI_TRUE : JNI_FALSE;
#else
    // Non-throwing: "no XR active" is correct when XR is off.
    (void)handle;
    return JNI_FALSE;
#endif
}

// =====================================================================
// View
// =====================================================================

JNIEXPORT jlong JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewAttach(
    JNIEnv* env, jclass, jlong runtimeHandle, jobject surface)
{
    if (surface == nullptr)
    {
        return 0;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr)
    {
        return 0;
    }
    View* view = nullptr;
    try
    {
        view = new View{*AsRuntime(runtimeHandle), window};
    }
    catch (const std::exception&)
    {
        ANativeWindow_release(window);
        return 0;
    }

    // Kick off the first resize using the surface's current pixel
    // dimensions. Hosts will typically also wire their SurfaceHolder
    // callback's `surfaceChanged(holder, format, w, h)` to `viewResize`
    // for subsequent size changes — this initial call composes cleanly
    // with that because `View::Resize` past the first call is an
    // idempotent `Device::UpdateSize`. If the surface reports zero
    // (rare; surface not fully realized yet), skip and rely on the
    // host's `surfaceChanged` to deliver the first size.
    const int32_t surfaceWidth = ANativeWindow_getWidth(window);
    const int32_t surfaceHeight = ANativeWindow_getHeight(window);
    if (surfaceWidth > 0 && surfaceHeight > 0)
    {
        view->Resize(static_cast<uint32_t>(surfaceWidth),
                      static_cast<uint32_t>(surfaceHeight),
                      Babylon::Embedding::CoordinateUnits::Physical);
    }

    // bgfx retains its own reference on the ANativeWindow for the
    // surface-binding lifetime, so release our local acquire here.
    ANativeWindow_release(window);
    return reinterpret_cast<jlong>(view);
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewDetach(JNIEnv*, jclass, jlong handle)
{
    delete AsView(handle);
}

// Register a secondary surface mirrored from the main backbuffer. Retains the
// ANativeWindow until removed.
JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeAddSecondarySurface(
    JNIEnv* env, jclass, jlong, jobject surface)
{
    if (surface == nullptr)
    {
        return;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr)
    {
        return;
    }
    const int32_t w = ANativeWindow_getWidth(window);
    const int32_t h = ANativeWindow_getHeight(window);
    MirrorAddSurface(window, w > 0 ? w : 1, h > 0 ? h : 1);
}

// Detach a secondary surface and destroy its EGL context.
JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeRemoveSecondarySurface(
    JNIEnv* env, jclass, jlong, jobject surface)
{
    if (surface == nullptr)
    {
        return;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr)
    {
        return;
    }
    MirrorRemoveSurface(window);
    // Release the lookup ref; MirrorRemoveSurface released the retained one.
    ANativeWindow_release(window);
}

// Mirror the main backbuffer onto secondary surfaces. Call once per frame on
// the render thread, right after viewRenderFrame.
JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_runtimeMirrorFrame(JNIEnv*, jclass, jlong)
{
    MirrorFrame();
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewRenderFrame(JNIEnv*, jclass, jlong handle)
{
    AsView(handle)->RenderFrame();
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewResize(
    JNIEnv*, jclass, jlong handle, jint width, jint height)
{
    // Java callers pass the SurfaceView's pixel-buffer dimensions
    // (physical pixels on Android).
    AsView(handle)->Resize(static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height),
                            Babylon::Embedding::CoordinateUnits::Physical);
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewPointerDown(
    JNIEnv* env, jclass, jlong handle, jint pointerId, jfloat x, jfloat y)
{
#if BABYLON_NATIVE_PLUGIN_NATIVEINPUT
    (void)env;
    // MotionEvent.getX/getY are physical pixels.
    AsView(handle)->OnPointerDown(static_cast<int32_t>(pointerId), x, y,
                                   Babylon::Embedding::CoordinateUnits::Physical);
#else
    (void)handle; (void)pointerId; (void)x; (void)y;
    ThrowPluginNotEnabled(env,
        "viewPointerDown was called but BABYLON_NATIVE_PLUGIN_NATIVEINPUT "
        "was not enabled at native build time.");
#endif
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewPointerMove(
    JNIEnv* env, jclass, jlong handle, jint pointerId, jfloat x, jfloat y)
{
#if BABYLON_NATIVE_PLUGIN_NATIVEINPUT
    (void)env;
    AsView(handle)->OnPointerMove(static_cast<int32_t>(pointerId), x, y,
                                   Babylon::Embedding::CoordinateUnits::Physical);
#else
    (void)handle; (void)pointerId; (void)x; (void)y;
    ThrowPluginNotEnabled(env,
        "viewPointerMove was called but BABYLON_NATIVE_PLUGIN_NATIVEINPUT "
        "was not enabled at native build time.");
#endif
}

JNIEXPORT void JNICALL
Java_com_babylonjs_embedding_BabylonNative_viewPointerUp(
    JNIEnv* env, jclass, jlong handle, jint pointerId, jfloat x, jfloat y)
{
#if BABYLON_NATIVE_PLUGIN_NATIVEINPUT
    (void)env;
    AsView(handle)->OnPointerUp(static_cast<int32_t>(pointerId), x, y,
                                 Babylon::Embedding::CoordinateUnits::Physical);
#else
    (void)handle; (void)pointerId; (void)x; (void)y;
    ThrowPluginNotEnabled(env,
        "viewPointerUp was called but BABYLON_NATIVE_PLUGIN_NATIVEINPUT "
        "was not enabled at native build time.");
#endif
}

} // extern "C"
