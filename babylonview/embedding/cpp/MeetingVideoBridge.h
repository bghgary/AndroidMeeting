#pragma once

#include <Babylon/Plugins/ExternalTexture.h>

#include <jni.h>

#include <cstdint>
#include <memory>

namespace Babylon::Embedding::Android
{
    class MeetingVideoBridge final
    {
    public:
        MeetingVideoBridge();
        ~MeetingVideoBridge();

        MeetingVideoBridge(const MeetingVideoBridge&) = delete;
        MeetingVideoBridge& operator=(const MeetingVideoBridge&) = delete;

        jobject CreateSurfaceTexture(
            JNIEnv* env,
            int32_t videoObjectId,
            uint32_t width,
            uint32_t height);

        Babylon::Plugins::ExternalTexture AttachSurfaceTexture(
            JNIEnv* env,
            int32_t videoObjectId);

        void ReleaseSurfaceTexture(JNIEnv* env, int32_t videoObjectId);
        void Update(JNIEnv* env);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
