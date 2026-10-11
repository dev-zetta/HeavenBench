#include "engine/runtime_paths.h"

#include "engine/tessellation_cache_archive.h"

#include <array>
#include <cstdlib>
#include <utility>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace heaven {

RuntimePaths::RuntimePaths(std::filesystem::path executable_directory,
                           std::optional<std::filesystem::path> user_directory)
    : executable_directory_(std::move(executable_directory)),
      user_directory_(std::move(user_directory)) {
    if (user_directory_ && user_directory_->empty()) user_directory_.reset();
    if (user_directory_) {
        *user_directory_ = std::filesystem::absolute(*user_directory_).lexically_normal();
    }
}

const std::optional<std::filesystem::path>& RuntimePaths::user_directory() const noexcept {
    return user_directory_;
}

std::filesystem::path RuntimePaths::default_config_file() const {
    return user_directory_ ? *user_directory_ / "config.json"
                           : std::filesystem::path("config.json");
}

std::filesystem::path RuntimePaths::resolve_ui_config_path(const std::filesystem::path& path) const {
    if (path.empty() || path.is_absolute()) return path;
    return user_directory_.value_or(executable_directory_) / path;
}

std::filesystem::path RuntimePaths::results_directory() const {
    return user_directory_.value_or(executable_directory_) / "results";
}

std::filesystem::path RuntimePaths::loading_results_directory(const std::filesystem::path& requested) const {
    if (requested.is_absolute()) return requested;
    return user_directory_.value_or(executable_directory_) / requested;
}

std::filesystem::path RuntimePaths::bundled_resources_directory(
        const std::filesystem::path& fallback) const {
    if (const auto configured = runtime_path_from_environment("HEAVEN_RESOURCE_DIR")) {
        return *configured;
    }
    if (!executable_directory_.empty()) {
        // ZIP, AppDir and macOS app resources are relative to the executable,
        // so direct tool invocation works from an unrelated working directory.
        const std::array candidates{
            executable_directory_ / "resources",
            executable_directory_ / "../Resources/resources",
            executable_directory_ / "../share/heavenbench/resources"};
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_directory(candidate, error)) {
                return candidate.lexically_normal();
            }
        }
    }
    return fallback;
}

std::string RuntimePaths::tessellation_cache_file(const std::string& configured,
                                                const std::string& data_root,
                                                bool data_root_is_zip) const {
    if (!configured.empty()) return configured;
    if (user_directory_) {
        return default_tessellation_cache_archive(user_directory_->string(), false);
    }
    return default_tessellation_cache_archive(data_root, data_root_is_zip);
}

std::optional<std::filesystem::path> runtime_path_from_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::filesystem::path(value);
}

std::filesystem::path bundled_resource_directory(const std::filesystem::path& fallback) {
    std::filesystem::path executable_directory;
#ifdef _WIN32
    // Grow to the Windows extended-path limit; a truncated path is never used.
    std::wstring buffer(1024, L'\0');
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) break;
        if (length < buffer.size()) {
            buffer.resize(length);
            executable_directory = std::filesystem::path(buffer).parent_path();
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
#elifdef __linux__
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) executable_directory = executable.parent_path();
#elifdef __APPLE__
    // dyld reports the actual executable independently of the working directory.
    // Resolve its possible symlinks before locating Contents/Resources.
    uint32_t size = 1024;
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) buffer.resize(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::error_code error;
        auto executable = std::filesystem::weakly_canonical(buffer.c_str(), error);
        if (!error) executable_directory = executable.parent_path();
    }
#endif
    return RuntimePaths(std::move(executable_directory)).bundled_resources_directory(fallback);
}

} // namespace heaven
