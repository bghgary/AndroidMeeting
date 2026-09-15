# External CMake build for the AndroidMeeting Babylon view AAR.
#
# This build FETCHES upstream BabylonJS/BabylonNative and enables the
# cross-platform `Embedding` facade (Runtime + View). Upstream's Android JNI
# interop layer is NOT used; instead this module builds its OWN
# `BabylonNativeEmbedding` shared library from a local, multiview-capable copy
# of the JNI source (embedding/cpp/BabylonNativeEmbedding.cpp). That file adds
# the secondary-surface mirror API (runtimeAddSecondarySurface /
# runtimeRemoveSecondarySurface / runtimeMirrorFrame) and an asset shader-cache
# loader on top of the upstream Embedding facade — the only source that needs to
# diverge from upstream, so it lives here rather than in a BabylonNative fork.
#
# The result is a single shared library:
#
#     libBabylonNativeEmbedding.so
#
# whose exported JNI symbols follow `Java_com_babylonjs_embedding_BabylonNative_*`
# and are bound by `src/main/java/com/babylonjs/embedding/BabylonNative.java`.
#
# Gradle's `externalNativeBuild { cmake { path "CMakeLists.txt" } }` invokes
# this file once per ABI; the produced .so is packaged into the AAR.

cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(AndroidMeetingBabylon)

include(FetchContent)

# --- Babylon Native configuration -------------------------------------------
# Build the cross-platform Embedding facade (Runtime + View). Do NOT build
# upstream's Android JNI layer — this module defines its own multiview-capable
# BabylonNativeEmbedding target below, so upstream's single-output one would only
# clash on the target name.
set(BABYLON_NATIVE_EMBEDDING ON CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_EMBEDDING_ANDROID OFF CACHE BOOL "" FORCE)

# We only want the library; skip the sample apps and install targets.
set(BABYLON_NATIVE_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_INSTALL OFF CACHE BOOL "" FORCE)

# ARCore-backed and other unneeded plugins/polyfills are disabled to keep the
# AAR lean and the build self-contained. NativeCamera/NativeXr would otherwise
# require extracting the ARCore native libraries at build time.
set(BABYLON_NATIVE_POLYFILL_CANVAS OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_NATIVECAMERA OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_NATIVECAPTURE OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_NATIVEXR OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_TESTUTILS OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_EXTERNALTEXTURE OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_NATIVETRACING OFF CACHE BOOL "" FORCE)

# Image loading (decode) and image encoding are dropped in BOTH flavors to keep
# the shipped .so small.
set(BABYLON_NATIVE_PLUGIN_NATIVEENGINE_LOAD_IMAGES OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_PLUGIN_NATIVEENCODING OFF CACHE BOOL "" FORCE)

# The ShaderCache plugin (GPU shader cache load/save) is always enabled.
set(BABYLON_NATIVE_PLUGIN_SHADERCACHE ON CACHE BOOL "" FORCE)

# The offline ShaderTool (compiles .fx files into a cache at build time) is not
# needed for either flavor.
set(BABYLON_NATIVE_PLUGIN_SHADERTOOL OFF CACHE BOOL "" FORCE)

# Runtime shader compilation toggle -- this is what distinguishes the two AAR
# flavors produced by CI:
#
#   AM_DYNAMIC_SHADERS=OFF (default): no runtime shader compiler. Smallest .so,
#     but the app MUST supply a prebuilt GPU shader cache at runtime via
#     RuntimeOptions.shaderCachePath -- otherwise nothing renders.
#
#   AM_DYNAMIC_SHADERS=ON: bundles the shader compiler (ShaderCompiler ->
#     glslang + SPIRV-Cross). Larger .so, but compiles shaders at runtime, so no
#     prebuilt cache is required.
option(AM_DYNAMIC_SHADERS "Include the runtime shader compiler (glslang/SPIRV-Cross)." OFF)
if(AM_DYNAMIC_SHADERS)
    set(BABYLON_NATIVE_PLUGIN_NATIVEENGINE_COMPILESHADERS ON CACHE BOOL "" FORCE)
    set(BABYLON_NATIVE_PLUGIN_SHADERCOMPILER ON CACHE BOOL "" FORCE)
    message(STATUS "AndroidMeeting: runtime shader compilation ENABLED (dynamic-shaders flavor).")
else()
    set(BABYLON_NATIVE_PLUGIN_NATIVEENGINE_COMPILESHADERS OFF CACHE BOOL "" FORCE)
    set(BABYLON_NATIVE_PLUGIN_SHADERCOMPILER OFF CACHE BOOL "" FORCE)
    message(STATUS "AndroidMeeting: runtime shader compilation DISABLED (prebuilt shader cache required).")
endif()

# Networking polyfills not needed by this view.
set(BABYLON_NATIVE_POLYFILL_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(BABYLON_NATIVE_POLYFILL_URL OFF CACHE BOOL "" FORCE)

# --- Fetch BabylonNative -----------------------------------------------------
# BabylonNative pulls all of its own dependencies (bgfx, JsRuntimeHost, the JS
# engine, arcana, glslang, ...) via FetchContent, so a plain clone is enough.
# Pinned to an upstream commit that supports the QuickJS engine. The local
# multiview JNI source calls the Embedding facade API, which is unchanged from
# the source's authoring base up to here.
FetchContent_Declare(BabylonNative
    GIT_REPOSITORY https://github.com/BabylonJS/BabylonNative.git
    GIT_TAG 345037350838f1ea8fb2ebefb03b9255d43ac773)

FetchContent_MakeAvailable(BabylonNative)

# The pinned ASTC encoder enables the 64-bit POPCNT intrinsic whenever the
# compiler advertises POPCNT, including 32-bit x86 where that intrinsic is not
# available. Use its portable fallback for that ABI.
if(ANDROID_ABI STREQUAL "x86" AND TARGET bimg)
    target_compile_definitions(bimg PRIVATE ASTCENC_POPCNT=0)
endif()

# --- Our own Android JNI interop layer (multiview) ---------------------------
# Build libBabylonNativeEmbedding.so from the local multiview JNI source instead
# of upstream's Embedding/Android target (disabled above). It links against the
# upstream Embedding facade and uses upstream's unmodified Android platform
# header (RuntimeHandle.h) from the fetched tree.
add_library(BabylonNativeEmbedding SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/embedding/cpp/BabylonNativeEmbedding.cpp")

target_include_directories(BabylonNativeEmbedding PRIVATE
    "${babylonnative_SOURCE_DIR}/Embedding/Include/Platform/Android")

target_link_libraries(BabylonNativeEmbedding
    PRIVATE Embedding          # cross-platform facade (propagates its public defs/includes)
    PRIVATE AndroidExtensions
    PRIVATE android            # ANativeWindow_fromSurface
    PRIVATE log
    PRIVATE EGL                # required by bgfx GL backend + secondary-surface mirror
    PRIVATE GLESv3
    PRIVATE -lz)

# The multiview JNI source uses Babylon::Plugins::ShaderCache directly (guarded by
# BABYLON_NATIVE_PLUGIN_SHADERCACHE). The facade links ShaderCache PRIVATEly, so
# link it here too to bring in its header + symbols.
if(TARGET ShaderCache)
    target_link_libraries(BabylonNativeEmbedding PRIVATE ShaderCache)
endif()

# --- Minimal-size linking ----------------------------------------------------
# Drop unreferenced code/data, fold identical functions, hide symbols pulled in
# from static archives (only our JNIEXPORT entry points need to be exported),
# and finish ThinLTO codegen at link time.
target_link_options(BabylonNativeEmbedding PRIVATE
    "-flto=thin"
    "-Wl,--gc-sections"
    "-Wl,--icf=all"
    "-Wl,--exclude-libs,ALL")

# --- Link-size diagnostics ---------------------------------------------------
# Emit an lld linker map next to the .so so CI can report exactly which static
# archives (.a) were pulled into the final library and how many bytes each
# contributed. Cheap to produce and does not affect the shipped binary.
target_link_options(BabylonNativeEmbedding PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/BabylonNativeEmbedding.map")
