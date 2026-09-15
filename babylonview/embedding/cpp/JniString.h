#pragma once

#include <jni.h>

#include <string>

namespace Babylon::Embedding::Android
{
    inline std::u16string ToUtf16String(JNIEnv* env, jstring value)
    {
        if (value == nullptr)
        {
            return {};
        }

        const jsize length = env->GetStringLength(value);
        const jchar* chars = env->GetStringChars(value, nullptr);
        if (chars == nullptr)
        {
            return {};
        }

        try
        {
            std::u16string result;
            result.reserve(static_cast<size_t>(length));
            for (jsize index = 0; index < length; ++index)
            {
                result.push_back(static_cast<char16_t>(chars[index]));
            }
            env->ReleaseStringChars(value, chars);
            return result;
        }
        catch (...)
        {
            env->ReleaseStringChars(value, chars);
            throw;
        }
    }
}
