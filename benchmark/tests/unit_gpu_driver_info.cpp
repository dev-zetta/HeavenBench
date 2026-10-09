#include "engine/gpu_driver_info.h"

#include <array>
#include <cstdio>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_gpu_driver_info() {
    using namespace heaven;
    bool ok = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_gpu_driver_info] FAILED: %s\n", message);
            ok = false;
        }
    };

    check(decode_driver_version(DriverVersionEncoding::Nvidia, 0x911481c0u) ==
              "580.82.7.0", "NVIDIA's 10/8/8/6 version layout changed");
    check(decode_driver_version(DriverVersionEncoding::IntelWindows, 0x00195442u) ==
              "101.5186", "Intel Windows final two version fields changed");
    check(decode_driver_version(DriverVersionEncoding::Mesa, 0x06402006u) ==
              "25.2.6", "Mesa's 10/10/12 version layout changed");
    check(decode_driver_version(DriverVersionEncoding::Mesa, 0x06402006u,
              "Mesa 25.3.0-devel (git-1234)") == "25.3.0-devel",
          "Mesa prerelease was reported as an ordinary final release");
    check(decode_driver_version(DriverVersionEncoding::Mesa, 0,
              "Mesa 25.1.0-rc2 (LLVM 19.1.7)") == "25.1.0-rc2",
          "explicit Mesa version was lost when packed version is unavailable");
    check(decode_driver_version(DriverVersionEncoding::Nvidia, 0xffffffffu) ==
              "1023.255.255.63", "NVIDIA field boundaries overlap");
    check(decode_driver_version(DriverVersionEncoding::IntelWindows, 0xffffffffu) ==
              "262143.16383", "Intel field boundaries overlap");
    check(decode_driver_version(DriverVersionEncoding::Unknown, 0x06402006u).empty(),
          "unknown implementation used Vulkan's API version encoding");
    for (const auto encoding : {DriverVersionEncoding::Nvidia,
                               DriverVersionEncoding::IntelWindows,
                               DriverVersionEncoding::Mesa}) {
        check(decode_driver_version(encoding, 0).empty(),
              "missing driver version became an invented zero release");
    }

    struct GlCase {
        std::string_view vendor;
        std::string_view renderer;
        std::string_view version;
        DriverFamily family;
        std::string_view driver_version;
        std::string_view api_version;
    };
    constexpr std::array cases{
        GlCase{"NVIDIA Corporation", "NVIDIA RTX GPU",
               "4.6.0 NVIDIA 580.82.07", DriverFamily::Nvidia, "580.82.07", "4.6.0"},
        GlCase{"AMD", "AMD Radeon (radeonsi, LLVM 19.1.7)",
               "4.6 (Core Profile) Mesa 25.2.6", DriverFamily::Mesa, "25.2.6", "4.6"},
        GlCase{"Intel", "Mesa Intel(R) Xe Graphics",
               "4.6 (Core Profile) Mesa 26.0.0-devel (git-abcd)",
               DriverFamily::Mesa, "26.0.0-devel", "4.6"},
        GlCase{"Mesa", "zink Vulkan 1.4 NVIDIA RTX GPU (MESA_NVK)",
               "4.6 (Core Profile) Mesa 25.3.0-rc1",
               DriverFamily::MesaNvk, "25.3.0-rc1", "4.6"},
        GlCase{"nouveau", "NV106", "4.3 (Core Profile) Mesa 24.3.4",
               DriverFamily::Nouveau, "24.3.4", "4.3"},
        GlCase{"Mesa/X.org", "llvmpipe (LLVM 19.1.7, 256 bits)",
               "4.5 (Core Profile) Mesa 25.2.6", DriverFamily::Software, "25.2.6", "4.5"},
        GlCase{"Google Inc.", "SwiftShader Device (Subzero)",
               "OpenGL ES 3.1 SwiftShader 5.0", DriverFamily::Software, "", "3.1"},
        GlCase{"Microsoft Corporation", "GDI Generic", "1.1.0",
               DriverFamily::Software, "", "1.1.0"},
        GlCase{"Microsoft Corporation", "Microsoft Basic Render Driver", "4.1",
               DriverFamily::Software, "", "4.1"},
        GlCase{"Microsoft Corporation", "Microsoft Basic Display Adapter", "4.1",
               DriverFamily::Software, "", "4.1"},
        GlCase{"Intel", "Intel(R) Arc(TM) A770 Graphics",
               "4.6.0 - Build 32.0.101.8131", DriverFamily::Intel, "32.0.101.8131", "4.6.0"},
        GlCase{"ATI Technologies Inc.", "AMD Radeon RX GPU",
               "4.6.0 Compatibility Profile Context 25.20.23.10",
               DriverFamily::Amd, "", "4.6.0"},
        GlCase{"unknown vendor", "NVIDIA hardware under a custom implementation",
               "4.6 CustomDriver 123.45", DriverFamily::Unknown, "", "4.6"},
        GlCase{"Mesa", "Mesa renderer", "OpenGL ES 3.2 Mesa 25.1.0",
               DriverFamily::Mesa, "25.1.0", "3.2"},
        GlCase{"Mesa", "Mesa renderer", "OpenGL ES-CM 1.1 Mesa 25.1.0",
               DriverFamily::Mesa, "25.1.0", "1.1"},
        GlCase{"Mesa", "Mesa renderer", "OpenGL ES-CL 1.1 Mesa 25.1.0",
               DriverFamily::Mesa, "25.1.0", "1.1"},
        GlCase{"", "", "", DriverFamily::Unknown, "", ""},
    };
    for (const auto& item : cases) {
        const auto actual = describe_opengl_driver(item.vendor, item.renderer, item.version);
        if (actual.family != item.family || actual.driver_version != item.driver_version ||
            actual.api_version != item.api_version) {
            std::printf("[unit_gpu_driver_info] FAILED: GL vendor '%.*s', version '%.*s' "
                        "became family=%.*s driver='%s' API='%s'\n",
                static_cast<int>(item.vendor.size()), item.vendor.data(),
                static_cast<int>(item.version.size()), item.version.data(),
                static_cast<int>(driver_family_name(actual.family).size()),
                driver_family_name(actual.family).data(), actual.driver_version.c_str(),
                actual.api_version.c_str());
            ok = false;
        }
    }

    for (const std::string_view malformed : {"Mesa ", "Mesa 25", "Mesa 25.",
                                             "Mesa 25..1", "Mesa 25.1.0garbage"}) {
        check(decode_driver_version(DriverVersionEncoding::Mesa, 0, malformed).empty(),
              "malformed Mesa version was accepted");
        check(decode_driver_version(DriverVersionEncoding::Mesa, 0x06402006u,
                                    malformed).empty(),
              "malformed Mesa text fell back to an apparently final packed release");
    }
    check(classify_driver_text("NVIDIA", "Mesa 25.2.6", "NVK") == DriverFamily::MesaNvk,
          "hardware vendor overrode the NVK implementation");
    check(classify_driver_text("NVIDIA", "Mesa 25.2.6", "llvmpipe") == DriverFamily::Software,
          "hardware vendor overrode software rendering");
    check(classify_driver_text("AMD", "Mesa 25.2.6", "AMD Radeon") == DriverFamily::Mesa,
          "hardware vendor overrode Mesa");
    return ok;
}

const heaven::testing::Register register_gpu_driver_info(unit_gpu_driver_info);

} // namespace
