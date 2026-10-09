#ifndef HEAVENBENCH_SRC_ENGINE_RUNTIME_PATHS_H_
#define HEAVENBENCH_SRC_ENGINE_RUNTIME_PATHS_H_

#include <filesystem>
#include <optional>
#include <string>

namespace heaven {

// Package launchers opt into a writable per-user root with HEAVEN_USER_DIR.
// Without it, desktop development keeps its existing paths.
class RuntimePaths {
public:
    RuntimePaths(std::filesystem::path executable_directory,
                 std::optional<std::filesystem::path> user_directory = std::nullopt);

    [[nodiscard]] const std::optional<std::filesystem::path>& user_directory() const noexcept;
    [[nodiscard]] std::filesystem::path default_config_file() const;
    [[nodiscard]] std::filesystem::path resolve_ui_config_path(const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path results_directory() const;
    [[nodiscard]] std::filesystem::path loading_results_directory(const std::filesystem::path& requested) const;
    [[nodiscard]] std::filesystem::path bundled_resources_directory(const std::filesystem::path& fallback) const;
    [[nodiscard]] std::string tessellation_cache_file(const std::string& configured,
                                                     const std::string& data_root,
                                                     bool data_root_is_zip) const;

private:
    std::filesystem::path executable_directory_;
    std::optional<std::filesystem::path> user_directory_;
};

[[nodiscard]] std::optional<std::filesystem::path> runtime_path_from_environment(const char* name);
[[nodiscard]] std::filesystem::path bundled_resource_directory(const std::filesystem::path& fallback);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_RUNTIME_PATHS_H_
