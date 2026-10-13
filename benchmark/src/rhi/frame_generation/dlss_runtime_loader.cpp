#include "dlss_runtime_loader.h"

#include <array>
#include <system_error>

#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sl_security.h>
#endif

namespace heaven {

DlssRuntimeLoader::~DlssRuntimeLoader() {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE && defined(_WIN32)
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
#endif
}

bool DlssRuntimeLoader::load([[maybe_unused]] const std::filesystem::path& absolute_directory, std::string& reason) {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE && defined(_WIN32)
    if (module_) { reason = "Streamline interposer is already loaded"; return false; }
    if (absolute_directory.empty() || !absolute_directory.is_absolute()) {
        reason = "DLSS needs an explicit absolute directory of signed production SDK libraries";
        return false;
    }
    std::error_code error;
    const auto directory = std::filesystem::canonical(absolute_directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        reason = "Streamline plugin directory is unavailable";
        return false;
    }
    // Require the complete production plugin set before initializing. Streamline
    // validates its plugins internally; its dual-signature helper applies to
    // the interposer, not every NGX dependency's signing arrangement.
    constexpr std::array files{L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
        L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll"};
    for (const auto* file : files) {
        const auto path = directory / file;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            reason = "A required Streamline/DLSS production library is missing";
            return false;
        }
    }
    const auto interposer = directory / L"sl.interposer.dll";
    // Official SDK procedure: OS trust plus NVIDIA secondary signature before
    // any vendor code is loaded. Unsigned development interposers are refused.
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        reason = "The Streamline interposer's NVIDIA production signature is invalid";
        return false;
    }
    module_ = LoadLibraryExW(interposer.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
        reason = "Windows could not load the signed Streamline interposer";
        return false;
    }
    return true;
#else
    reason = "Native DLSS frame generation requires the optional Windows Streamline build";
    return false;
#endif
}

DlssRuntimeLoader::Function DlssRuntimeLoader::find_export([[maybe_unused]] const char* name) const noexcept {
#if defined(HEAVEN_HAS_STREAMLINE) && HEAVEN_HAS_STREAMLINE && defined(_WIN32)
    return module_ && name ? reinterpret_cast<Function>(
        GetProcAddress(static_cast<HMODULE>(module_), name)) : nullptr;
#else
    return nullptr;
#endif
}

} // namespace heaven
