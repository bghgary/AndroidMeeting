package com.babylonjs.embedding;

import android.app.Activity;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.view.Surface;

/**
 * JVM binding for the C++ Babylon::Embedding layer. Native methods
 * mirror byte-for-byte the {@code extern "C" JNIEXPORT} entry points in
 * {@code Embedding/Android/src/main/cpp/BabylonNativeEmbedding.cpp}
 * (built into {@code libBabylonNativeEmbedding.so} by this module's
 * external CMake build of BabylonJS/BabylonNative).
 *
 * <p>Thin facade — owns no state, exposes the C++ API as static methods.
 * Hosts typically wrap it in their own {@code View} subclass (see
 * {@link com.babylonjs.meeting.BabylonView}).
 *
 * <p>Lifecycle:
 * <ol>
 *   <li>{@link #setContext(Context)} once at app startup.</li>
 *   <li>Create a Runtime via {@link #runtimeCreate()} or
 *       {@link #runtimeCreate(RuntimeOptions)} and keep the handle.</li>
 *   <li>Optional: queue scripts via {@link #runtimeLoadScript(long, String)};
 *       they run after the first {@link #viewAttach(long, Surface)}.</li>
 *   <li>Attach a View and drive {@link #viewRenderFrame(long)} from your draw loop.</li>
 *   <li>Tear down with {@link #viewDetach(long)} then {@link #runtimeDestroy(long)}.</li>
 * </ol>
 */
public final class BabylonNative {
    /**
     * One participant in a meeting-stage state update.
     */
    public static final class MeetingStageParticipantState {
        public final int id;
        public final String displayName;
        public final boolean muted;
        public final boolean videoOn;
        public final Integer videoObjectId;

        public MeetingStageParticipantState(
                int id, String displayName, boolean muted, boolean videoOn) {
            this(id, displayName, muted, videoOn, null);
        }

        public MeetingStageParticipantState(
                int id,
                String displayName,
                boolean muted,
                boolean videoOn,
                Integer videoObjectId) {
            if (displayName == null) {
                throw new IllegalArgumentException("displayName must not be null.");
            }
            this.id = id;
            this.displayName = displayName;
            this.muted = muted;
            this.videoOn = videoOn;
            this.videoObjectId = videoObjectId;
        }
    }

    /**
     * Construction options. Defaults match the C++
     * {@code Babylon::Embedding::RuntimeOptions}. Fields are public for
     * simple object-initializer patterns.
     */
    public static final class RuntimeOptions {
        /** Optional MSAA samples (0, 2, 4, 8, 16). */
        public Integer msaaSamples = null;

        /** Enable the JavaScript debugger (engine-dependent). */
        public boolean enableDebugger = false;

        /** Enable Babylon::DebugTrace output through the logcat sink. */
        public boolean enableDebugTrace = false;

        /** Block engine startup until a debugger attaches (engine-dependent). */
        public boolean waitForDebugger = false;

        /** Optional persistent on-disk shader cache path. */
        public String shaderCachePath = null;
    }

    static {
        System.loadLibrary("BabylonNativeEmbedding");
    }

    private BabylonNative() {}

    // -------------------------------------------------------------------
    // Process-wide platform lifecycle
    // -------------------------------------------------------------------

    /**
     * Register the application Context. Call once at app startup
     * (typically from {@code Application.onCreate}) before constructing
     * any Runtime. Repeated calls replace the existing Context global ref.
     */
    public static native void setContext(Context context);

    public static native void setCurrentActivity(Activity activity);

    public static native void pause();

    public static native void resume();

    public static native void requestPermissionsResult(
            int requestCode, String[] permissions, int[] grantResults);

    // -------------------------------------------------------------------
    // Runtime
    // -------------------------------------------------------------------

    /** Returns an opaque handle; release with {@link #runtimeDestroy(long)}. */
    public static native long runtimeCreate();

    /**
     * Returns an opaque handle; release with {@link #runtimeDestroy(long)}.
     * Pass {@code null} for the same defaults as {@link #runtimeCreate()}.
     *
     * <p>If {@link RuntimeOptions#shaderCachePath} is non-null, the cache
     * is loaded on first {@link #viewAttach(long, Surface)} and saved on
     * suspend / destroy. Throws {@link IllegalStateException} when
     * shaderCachePath is set but {@code BABYLON_NATIVE_PLUGIN_SHADERCACHE}
     * is disabled in the native build.
     */
    public static native long runtimeCreate(RuntimeOptions options);

    public static native void runtimeDestroy(long handle);

    public static native void runtimeLoadScript(long handle, String url);

    /** Load a GPU shader cache directly from an Android asset (no on-disk copy). */
    public static native void runtimeLoadShaderCache(long handle, String assetName);

    public static native void runtimeEval(long handle, String source, String sourceUrl);

    /**
     * Set the current meeting-stage state without evaluating generated
     * JavaScript source.
     *
     * <p>The initial script must define
     * {@code globalThis.setMeetingStageState(state)}. The state object has
     * {@code stageId}, {@code layout}, nullable {@code activeSpeakerId}, and a
     * {@code participants} array. Each participant has {@code id},
     * {@code displayName}, {@code muted}, {@code videoOn}, and nullable
     * {@code videoObjectId}.
     *
     * <p>The call is serialized behind scripts and state changes previously
     * queued through this Runtime, so call it after
     * {@link #runtimeLoadScript(long, String)} queues the script that installs
     * the function.
     *
     * <p>Throws {@link IllegalArgumentException} for invalid arguments and
     * {@link IllegalStateException} for an invalid or destroyed Runtime handle.
     * A missing JavaScript function or an exception thrown by it is reported
     * through the Runtime's uncaught-JavaScript error handler.
     */
    public static native void runtimeSetMeetingStageState(
            long runtimeHandle,
            String stageId,
            int layout,
            Integer activeSpeakerId,
            MeetingStageParticipantState[] participants);

    /**
     * Reset a meeting stage by calling
     * {@code globalThis.resetMeetingStage(stageId)} on the JavaScript thread.
     *
     * <p>The call has the same ordering and error behavior as
     * {@link #runtimeSetMeetingStageState(long, String, int, Integer,
     * MeetingStageParticipantState[])}.
     */
    public static native void runtimeResetMeetingStage(long runtimeHandle, String stageId);

    /**
     * Create a detached SurfaceTexture for one decoded meeting-video stream.
     * The caller must install it on the SlimCore GLTextureView and then call
     * {@link #runtimeAttachMeetingVideoSurfaceTexture(long, int)}.
     */
    public static native SurfaceTexture runtimeCreateMeetingVideoSurfaceTexture(
            long runtimeHandle, int videoObjectId, int width, int height);

    /**
     * Attach a previously created meeting-video SurfaceTexture to Babylon's
     * shared OpenGL context and publish its GPU texture to JavaScript.
     */
    public static native void runtimeAttachMeetingVideoSurfaceTexture(
            long runtimeHandle, int videoObjectId);

    /** Latch and convert pending meeting-video frames entirely on the GPU. */
    public static native void runtimeUpdateMeetingVideoTextures(long runtimeHandle);

    /** Stop publishing and release one meeting-video SurfaceTexture. */
    public static native void runtimeReleaseMeetingVideoTexture(
            long runtimeHandle, int videoObjectId);

    // No per-Runtime Suspend/Resume here: each Runtime auto-subscribes to
    // pause/resume in runtimeCreate. Hosts call those once per Activity
    // state change and every Runtime reacts. Use the C++ API for
    // finer-grained control.

    /**
     * Set the Surface that XR renders into (typically a transparent
     * SurfaceView overlay). Pass {@code null} to clear. Throws
     * {@link IllegalStateException} when {@code BABYLON_NATIVE_PLUGIN_NATIVEXR}
     * is disabled in the native build.
     */
    public static native void runtimeSetXrSurface(long handle, Surface surface);

    /**
     * Whether an XR session is active. Returns {@code false} (never
     * throws) when {@code BABYLON_NATIVE_PLUGIN_NATIVEXR} is disabled.
     */
    public static native boolean runtimeIsXrActive(long handle);

    // -------------------------------------------------------------------
    // View
    // -------------------------------------------------------------------

    /** Returns an opaque handle; release with {@link #viewDetach(long)}. */
    public static native long viewAttach(long runtimeHandle, Surface surface);

    public static native void viewDetach(long handle);

    // -------------------------------------------------------------------
    // Multiview: mirror the primary render into secondary SurfaceViews so
    // several views (e.g. across displays) can show the same scene while a
    // single view drives rendering (bgfx allows one render device per process).
    // -------------------------------------------------------------------

    /** Register a secondary SurfaceView to mirror the main render. */
    public static native void runtimeAddSecondarySurface(long runtimeHandle, Surface surface);

    /** Detach a secondary SurfaceView and destroy its GL context. */
    public static native void runtimeRemoveSecondarySurface(long runtimeHandle, Surface surface);

    /** Mirror the main backbuffer to secondary surfaces (call once per frame). */
    public static native void runtimeMirrorFrame(long runtimeHandle);

    public static native void viewRenderFrame(long handle);

    /**
     * Push the surface's new pixel-buffer dimensions. {@code width} and
     * {@code height} are in physical pixels — pass the values from
     * {@code SurfaceHolder.Callback.surfaceChanged} unchanged. The native
     * View divides by DPR internally.
     */
    public static native void viewResize(long handle, int width, int height);

    /**
     * Pointer events. Pass {@code MotionEvent.getX/getY} through
     * unchanged (physical pixels); native View converts internally
     * before forwarding to Babylon.js's clientX/clientY pipeline.
     *
     * <p>Throws {@link IllegalStateException} when
     * {@code BABYLON_NATIVE_PLUGIN_NATIVEINPUT} is disabled.
     */
    public static native void viewPointerDown(long handle, int pointerId, float x, float y);

    public static native void viewPointerMove(long handle, int pointerId, float x, float y);

    public static native void viewPointerUp(long handle, int pointerId, float x, float y);
}
