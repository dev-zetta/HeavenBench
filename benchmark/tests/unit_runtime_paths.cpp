#include "engine/runtime_paths.h"
#include "engine/tessellation_cache_archive.h"

#include <filesystem>
#include <chrono>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_runtime_paths() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "heaven_runtime_paths";
    const fs::path executable = base / "read-only-install" / "bin";
    const fs::path user = base / "user data";
    const heaven::RuntimePaths packaged(executable, user);
    if (packaged.default_config_file() != user / "config.json" ||
        packaged.resolve_ui_config_path("alternate.json") != user / "alternate.json" ||
        packaged.results_directory() != user / "results" ||
        packaged.loading_results_directory("loading-results") != user / "loading-results" ||
        packaged.tessellation_cache_file("", "read-only-assets.zip", true) !=
            heaven::default_tessellation_cache_archive(user.string(), false)) {
        return false;
    }

    // Explicit paths remain caller-owned, including relative CLI cache paths.
    const fs::path explicit_path = base / "explicit.json";
    if (packaged.resolve_ui_config_path(explicit_path) != explicit_path ||
        packaged.resolve_ui_config_path({}) != fs::path{} ||
        packaged.loading_results_directory(explicit_path) != explicit_path ||
        packaged.tessellation_cache_file("relative-cache.zip", "unused", true) != "relative-cache.zip") {
        return false;
    }

    // A relative override is anchored once, not changed by later working-directory changes.
    const heaven::RuntimePaths relative(executable, fs::path("user-data"));
    if (relative.default_config_file() != fs::absolute("user-data") / "config.json") return false;

    const heaven::RuntimePaths development(executable);
    const heaven::RuntimePaths empty_override(executable, fs::path{});
    return !development.user_directory() && !empty_override.user_directory() &&
           development.default_config_file() == "config.json" &&
           development.resolve_ui_config_path("alternate.json") == executable / "alternate.json" &&
           development.results_directory() == executable / "results" &&
           development.loading_results_directory("loading-results") == executable / "loading-results" &&
           development.tessellation_cache_file("", "assets/data.zip", true) ==
               heaven::default_tessellation_cache_archive("assets/data.zip", true) &&
           development.tessellation_cache_file("", "loose-assets", false) ==
               heaven::default_tessellation_cache_archive("loose-assets", false);
}

bool unit_bundled_resource_paths() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven_resource_paths_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    fs::create_directories(root / "windows/resources");
    fs::create_directories(root / "linux/usr/bin");
    fs::create_directories(root / "linux/usr/share/heavenbench/resources");
    fs::create_directories(root / "macos/HeavenBench.app/Contents/MacOS");
    fs::create_directories(root / "macos/HeavenBench.app/Contents/Resources/resources");
    const heaven::RuntimePaths windows(root / "windows");
    const heaven::RuntimePaths linux_package(root / "linux/usr/bin");
    const heaven::RuntimePaths macos_package(root / "macos/HeavenBench.app/Contents/MacOS");
    const heaven::RuntimePaths development(root / "checkout/build");
    const auto override = heaven::runtime_path_from_environment("HEAVEN_RESOURCE_DIR");
    if (override) {
        return windows.bundled_resources_directory("fallback") == *override &&
               linux_package.bundled_resources_directory("fallback") == *override &&
               macos_package.bundled_resources_directory("fallback") == *override &&
               development.bundled_resources_directory("fallback") == *override;
    }
    return windows.bundled_resources_directory("fallback") == root / "windows/resources" &&
           linux_package.bundled_resources_directory("fallback") == root / "linux/usr/share/heavenbench/resources" &&
           macos_package.bundled_resources_directory("fallback") == root / "macos/HeavenBench.app/Contents/Resources/resources" &&
           development.bundled_resources_directory("fallback") == "fallback";
}

const heaven::testing::Register g_register_runtime_paths{&unit_runtime_paths};
const heaven::testing::Register g_register_bundled_resources{&unit_bundled_resource_paths};

} // namespace
