# AndroidMeeting

An Android **library** project that produces an **AAR** embedding
[BabylonJS/BabylonNative](https://github.com/BabylonJS/BabylonNative) as a
transparent 3D view.

## What the AAR contains

- **Native `.so`** — `libBabylonNativeEmbedding.so`, produced by an *external
  CMake build* (`babylonview/CMakeLists.txt`) that **fetches** upstream
  BabylonNative via CMake `FetchContent` and enables the cross-platform
  `Embedding` facade. Upstream's Android JNI layer is intentionally disabled;
  instead this repo builds its **own** `BabylonNativeEmbedding` target from a
  local JNI layer (`babylonview/embedding/cpp`). It adds the secondary-surface
  **mirror** API, asset shader-cache loading, and the meeting-video
  external-texture bridge, so the integration lives here rather than in a
  BabylonNative fork.
- **Java definitions**
  - `com.babylonjs.embedding.BabylonNative` — the JNI binding whose native
    methods map to the `Java_com_babylonjs_embedding_BabylonNative_*` symbols in
    `babylonview/embedding/cpp/BabylonNativeEmbedding.cpp`. Includes the multiview
    `runtimeAddSecondarySurface` / `runtimeRemoveSecondarySurface` /
    `runtimeMirrorFrame` and the asset-based `runtimeLoadShaderCache`.
  - `com.babylonjs.meeting.BabylonView` — a `SurfaceView`-based view configured
    for **transparency**.

## Transparency

`BabylonView` renders into a `SurfaceView` set up so that OpenGL back-buffer
pixels with **alpha == 0 are transparent**:

- `SurfaceView.setZOrderOnTop(true)` — the surface is alpha-blended by the
  compositor instead of punched out as an opaque hole.
- `SurfaceHolder.setFormat(PixelFormat.TRANSLUCENT)` — an RGBA_8888 buffer with
  a real alpha channel.

bgfx's GL backend already selects an EGL config with `EGL_ALPHA_SIZE = 8` for
its RGBA8 back buffer, so the alpha the scene writes survives composition. On
the JS side the scene must clear with a zero-alpha colour:

```js
scene.clearColor = new BABYLON.Color4(0, 0, 0, 0);
```

## Building

The AAR can be built with either QuickJS (the default) or V8, and with or
without the runtime shader compiler:

```bash
# Shader-cache flavor (default): smallest .so, NO runtime shader compiler.
# A prebuilt GPU shader cache is REQUIRED at runtime (RuntimeOptions.shaderCachePath).
./gradlew :babylonview:assembleRelease

# Dynamic-shaders flavor: bundles the glslang/SPIRV-Cross shader compiler, so
# shaders compile at runtime and no prebuilt cache is needed (larger .so).
./gradlew :babylonview:assembleRelease -PdynamicShaders

# Select V8 instead of QuickJS. Combine this with -PdynamicShaders for the
# V8 + dynamic-shaders variant.
./gradlew :babylonview:assembleRelease -PjsEngine=V8
./gradlew :babylonview:assembleRelease -PjsEngine=V8 -PdynamicShaders
```

The first configure fetches BabylonNative and all of its dependencies (bgfx,
JsRuntimeHost, the JS engine, glslang, ...) and compiles them, so the initial
build is long. Restrict to a single ABI while iterating:

```bash
./gradlew :babylonview:assembleRelease -PARM64Only
```

Output: `babylonview/build/outputs/aar/babylonview-release.aar` (all variants
write to the same path — build one at a time, or use the separately staged CI
artifacts).

### Requirements

- JDK 17
- Android SDK, NDK `29.0.14206865`, CMake `3.22.1+`, Ninja

## CI

`.github/workflows/build-aar.yml` builds all four JavaScript engine / shader
flavor combinations on GitHub Actions (device + emulator ABIs) and uploads each
as a separate artifact:

- `babylonview-shadercache-required-release.aar` — no runtime shader compiler;
  QuickJS; **requires a prebuilt shader cache** (`RuntimeOptions.shaderCachePath`).
- `babylonview-dynamic-shaders-release.aar` — bundles the shader compiler and
  uses QuickJS; **supports runtime (dynamic) shader compilation**; no prebuilt
  cache needed.
- `babylonview-v8-shadercache-required-release.aar` — V8; **requires a prebuilt
  shader cache**.
- `babylonview-v8-dynamic-shaders-release.aar` — V8; bundles the shader compiler
  and supports runtime shader compilation.

## Native build size trade-offs

To keep `libBabylonNativeEmbedding.so` small, the following are **disabled** in
the native build:

- **Image loading / decoding** (`NATIVEENGINE_LOAD_IMAGES=OFF`) — removes bimg
  decode/encode and WebP. Textures must be provided in a GPU-ready form; runtime
  decoding of PNG/JPEG/WebP is not available.
- **Image encoding** (`NATIVEENCODING=OFF`) — no native PNG encoding /
  `EncodeImageAsync` (screenshots, asset export).
- **Runtime shader compilation** (`NATIVEENGINE_COMPILESHADERS` + `SHADERCOMPILER`)
  — removes glslang + SPIRV-Cross. This is the **shader-cache flavor** (default,
  `-PdynamicShaders` off). The **dynamic-shaders flavor** (`-PdynamicShaders`)
  re-enables them so shaders compile at runtime. `SHADERCACHE` (the cache
  load/save plugin) stays enabled in both.
- **Networking polyfills**: WebSocket (`POLYFILL_WEBSOCKET=OFF`) and the JS
  `URL` global (`POLYFILL_URL=OFF`). `fetch`/`XMLHttpRequest` remain available
  (XMLHttpRequest is always linked by the Embedding layer and cannot be gated
  off by a flag).

Size-minimizing codegen is also applied to the whole native build: `-Oz`,
function/data sections with `--gc-sections`, hidden symbol visibility with
`--exclude-libs,ALL`, identical-code folding (`--icf=all`) and ThinLTO. CI
verifies the `Java_com_babylonjs_embedding_BabylonNative_*` JNI exports survive
the stripping.

`libbimg` remains in the binary because it is a hard dependency of **bgfx**
(the renderer), independent of the disabled NativeEngine image paths.

Because shader compilation is off, **you must supply a prebuilt GPU shader
cache** or nothing will render. The `ShaderCache` plugin stays enabled and is
exposed through the embedding layer via `RuntimeOptions.shaderCachePath`:

- On the **first view attach**, the file at `shaderCachePath` is loaded
  (`ShaderCache::Load`); a missing/unreadable file is ignored.
- On **suspend** and on **runtime destroy**, the cache is written back
  (`ShaderCache::Save`).

Populate the cache once from a build that has shader compilation enabled, ship
that file with your app, and point `shaderCachePath` at a writable copy of it.

## Using the view

```java
BabylonNative.setContext(getApplicationContext());

// Shader compilation is disabled in this build, so a prebuilt shader cache is
// required. Point shaderCachePath at a writable file seeded from your prebuilt
// cache; it is loaded on first attach and saved on suspend/destroy.
BabylonNative.RuntimeOptions options = new BabylonNative.RuntimeOptions();
options.shaderCachePath = new File(getFilesDir(), "shaders.bin").getAbsolutePath();

long runtime = BabylonNative.runtimeCreate(options);
BabylonNative.runtimeLoadScript(runtime, "app:///scene.js");
BabylonView view = new BabylonView(this, runtime);
setContentView(view);
```

## Updating meeting-stage state from Java

The Java binding can pass a typed meeting-stage snapshot to JavaScript without
serializing JSON or evaluating generated JavaScript:

```js
globalThis.setMeetingStageState = (state) => {
    // state.stageId
    // state.layout
    // state.activeSpeakerId (number or null)
    // state.participants[]:
    //   { id, displayName, muted, videoOn, videoObjectId }
};

globalThis.resetMeetingStage = (stageId) => {
    // Discard state belonging to stageId.
};
```

```java
BabylonNative.runtimeLoadScript(runtime, "app:///scene.js");
BabylonNative.runtimeSetMeetingStageState(
        runtime,
        "stage-1",
        7,
        42,
        new BabylonNative.MeetingStageParticipantState[] {
            new BabylonNative.MeetingStageParticipantState(42, "Ada", true, true, 84)
        });
```

`runtimeSetMeetingStageState` creates the JavaScript object and participant
array directly with Node-API. `runtimeResetMeetingStage` passes the stage ID as
a JavaScript string. Stage IDs and participant display names preserve their
UTF-16 contents, including embedded nulls and supplementary characters. Both
calls run on the runtime's JavaScript thread and are
serialized behind scripts and state changes queued earlier for that runtime.
Queue the script that installs both functions before sending state. Missing
functions and JavaScript exceptions are reported through the runtime's
uncaught-JavaScript error handler. Invalid Java arguments and invalid or
destroyed runtime handles throw Java exceptions synchronously.

## Supplying meeting video as external GPU textures

The Android embedding can route a decoder into a `SurfaceTexture` owned by
Babylon Native without reading pixels back to the CPU:

```java
SurfaceTexture surfaceTexture =
        BabylonNative.runtimeCreateMeetingVideoSurfaceTexture(runtime, videoObjectId, 640, 360);
textureView.setSurfaceTexture(surfaceTexture);
BabylonNative.runtimeAttachMeetingVideoSurfaceTexture(runtime, videoObjectId);
```

Keep that `TextureView` detached from the Android view hierarchy. Its renderer
may use the existing `SurfaceTexture` as a decoder target, while Babylon Native
latches each frame and converts the external OES image to a wrapped 2D GPU
texture. `BabylonView` calls
`runtimeUpdateMeetingVideoTextures(runtime)` immediately before each
`viewRenderFrame`; custom render loops must preserve that ordering. Call
`runtimeReleaseMeetingVideoTexture(runtime, videoObjectId)` when the stream
stops.

The scene script must define these callbacks before a video surface is
attached:

```js
globalThis.setMeetingStageVideoTexture =
    (videoObjectId, nativeTexture, width, height) => {
        // Wrap nativeTexture with engine.wrapNativeTexture(...).
    };

globalThis.clearMeetingStageVideoTexture = (videoObjectId) => {
    // Dispose the Babylon texture bound to videoObjectId.
};
```
