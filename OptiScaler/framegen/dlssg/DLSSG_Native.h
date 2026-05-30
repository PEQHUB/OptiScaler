#pragma once

#include <Config.h>
#include <Util.h>
#include <cstring>
#include <filesystem>

namespace DLSSGNative
{
    inline const char* ModeName(DLSSGNativeMode mode)
    {
        switch (mode)
        {
        case DLSSGNativeMode::Auto: return "auto";
        case DLSSGNativeMode::Legacy: return "legacy";
        case DLSSGNativeMode::Attach: return "attach";
        case DLSSGNativeMode::Passthrough: return "passthrough";
        default: return "unknown";
        }
    }

    inline bool IsAttachActive()
    {
        auto& state = State::Instance();
        if (state.activeFgOutput != FGOutput::DLSSG)
            return false;

        const auto mode = Config::Instance()->FGDLSSGNativeMode.value_or_default();
        return mode == DLSSGNativeMode::Attach ||
               (mode == DLSSGNativeMode::Auto && state.dlssgNativeStreamlineDetected && state.currentFG == nullptr);
    }

    inline bool IsPassthroughActive()
    {
        auto& state = State::Instance();
        if (state.activeFgOutput != FGOutput::DLSSG)
            return false;

        return Config::Instance()->FGDLSSGNativeMode.value_or_default() == DLSSGNativeMode::Passthrough;
    }

    inline bool IsNativeRuntimeActive()
    {
        return IsAttachActive() || IsPassthroughActive();
    }

    inline bool ShouldSkipOptiDLSSGContext()
    {
        return IsNativeRuntimeActive();
    }

    inline bool ShouldSkipStreamlinePluginHooks()
    {
        return IsNativeRuntimeActive();
    }

    inline bool ShouldHookNativeInterposer()
    {
        return IsAttachActive();
    }

    inline bool IsNativeEvaluateFresh(uint64_t currentFrame, uint64_t maxAgeFrames = 4)
    {
        const auto& state = State::Instance();
        if (!IsAttachActive() || state.dlssgNativeEvaluateCount == 0)
            return false;

        return currentFrame <= state.dlssgNativeLastEvaluateFrame + maxAgeFrames;
    }

    inline std::wstring LowerNormalizedPath(const std::filesystem::path& input)
    {
        auto path = input;
        std::error_code ec;
        if (path.is_relative())
        {
            auto absolute = std::filesystem::absolute(path, ec);
            if (!ec)
                path = absolute;
        }

        auto lowered = path.lexically_normal().wstring();
        to_lower_in_place(lowered);
        return lowered;
    }

    inline bool PathStartsWith(const std::wstring& path, const std::wstring& root)
    {
        if (root.empty() || path.size() < root.size())
            return false;

        if (path.rfind(root, 0) != 0)
            return false;

        return path.size() == root.size() || path[root.size()] == L'\\' || path[root.size()] == L'/';
    }

    inline bool IsNativeStreamlinePath(const std::filesystem::path& rawPath)
    {
        const auto path = LowerNormalizedPath(rawPath);
        const auto optiSlPath = LowerNormalizedPath(Util::DllPath().parent_path() / L"sl");

        if (path.empty())
            return false;

        if (path.contains(L"sl.interposer_output.dll"))
            return false;

        if (PathStartsWith(path, optiSlPath))
            return false;

        return true;
    }

    inline void RefreshRuntimeMode()
    {
        auto& state = State::Instance();
        const auto mode = Config::Instance()->FGDLSSGNativeMode.value_or_default();
        state.dlssgNativeAttachActive =
            state.activeFgOutput == FGOutput::DLSSG &&
            (mode == DLSSGNativeMode::Attach ||
             (mode == DLSSGNativeMode::Auto && state.dlssgNativeStreamlineDetected && state.currentFG == nullptr));
        state.dlssgNativePassthroughActive =
            state.activeFgOutput == FGOutput::DLSSG && mode == DLSSGNativeMode::Passthrough;
    }

    inline void MarkNativeStreamlineModule(const std::filesystem::path& rawPath, const char* moduleName)
    {
        if (!IsNativeStreamlinePath(rawPath))
            return;

        auto& state = State::Instance();
        const bool firstDetection = !state.dlssgNativeStreamlineDetected;
        state.dlssgNativeStreamlineDetected = true;

        if (strcmp(moduleName, "sl.interposer") == 0)
            state.dlssgNativeInterposerLoadCount++;
        else if (strcmp(moduleName, "sl.common") == 0)
            state.dlssgNativeCommonLoadCount++;
        else if (strcmp(moduleName, "sl.dlss_g") == 0)
            state.dlssgNativeDlssgLoadCount++;

        RefreshRuntimeMode();

        strncpy_s(state.dlssgNativeLastModule, moduleName, _TRUNCATE);
        const auto pathString = rawPath.lexically_normal().string();
        strncpy_s(state.dlssgNativeLastPath, pathString.c_str(), _TRUNCATE);

        if (firstDetection || state.dlssgNativeAttachActive || state.dlssgNativePassthroughActive)
        {
            LOG_INFO("Native Streamline detected module={} path={} mode={} attach={} passthrough={}",
                     moduleName,
                     pathString,
                     ModeName(Config::Instance()->FGDLSSGNativeMode.value_or_default()),
                     state.dlssgNativeAttachActive,
                     state.dlssgNativePassthroughActive);
        }
    }

    inline void MarkNativeStreamlineModule(HMODULE module, const char* moduleName)
    {
        if (module == nullptr)
            return;

        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(module, path, MAX_PATH) > 0)
            MarkNativeStreamlineModule(std::filesystem::path(path), moduleName);
    }
}
