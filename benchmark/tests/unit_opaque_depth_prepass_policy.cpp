#include "rhi/opaque_depth_prepass_policy.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {
using namespace heaven;

bool require(bool condition, const char* message) {
    if (!condition) std::printf("[unit_opaque_depth_prepass_policy] %s\n", message);
    return condition;
}

bool test_opaque_depth_prepass_device_scope() {
    constexpr OpaqueDepthPrepassDevice qualified{
        true, 0x8086u, 0xe223u, DriverFamily::Mesa, "26.0.8-1ubuntu0.3"};
    static_assert(qualifies_opaque_depth_prepass_device(qualified));
    if (!require(!qualifies_opaque_depth_prepass_device({}), "unknown device admitted")) return false;
    for (auto const family : {DriverFamily::Unknown, DriverFamily::Nvidia, DriverFamily::MesaNvk,
             DriverFamily::Amd, DriverFamily::Intel, DriverFamily::Nouveau, DriverFamily::Software}) {
        auto device = qualified;
        device.driver_family = family;
        if (!require(!qualifies_opaque_depth_prepass_device(device), "unqualified driver family admitted")) return false;
    }
    for (auto const version : {"", "26.0.8", "26.0.8-1ubuntu0.2", "26.0.8-1ubuntu0.4",
             "26.0.9-1ubuntu0.3", "26.0.8-1ubuntu0.3 ", "Mesa 26.0.8-1ubuntu0.3"}) {
        auto device = qualified;
        device.driver_version = version;
        if (!require(!qualifies_opaque_depth_prepass_device(device), "unqualified driver version admitted")) return false;
    }
    auto device = qualified;
    device.vulkan = false;
    if (!require(!qualifies_opaque_depth_prepass_device(device), "non-Vulkan device admitted")) return false;
    device = qualified;
    device.vendor_id = 0x1002u;
    if (!require(!qualifies_opaque_depth_prepass_device(device), "different vendor admitted")) return false;
    device = qualified;
    device.device_id = 0xe222u;
    return require(!qualifies_opaque_depth_prepass_device(device), "different Intel device admitted");
}

bool test_opaque_depth_prepass_frame_scope() {
    constexpr auto moon = render_tuning::lighting.moon_intensity_lux;
    struct Case {
        const char* name;
        bool qualified;
        bool contact;
        QualityLevel quality;
        float elevation;
        float intensity;
        bool expected;
    };
    const std::array cases{
        Case{"qualified High daylight",true,true,QualityLevel::High,.8f,110000,true},
        Case{"unqualified device",false,true,QualityLevel::High,.8f,110000,false},
        Case{"contact disabled",true,false,QualityLevel::High,.8f,110000,false},
        Case{"Low quality",true,true,QualityLevel::Low,.8f,110000,false},
        Case{"Medium quality",true,true,QualityLevel::Medium,.8f,110000,false},
        Case{"Sun below horizon",true,true,QualityLevel::High,-.1f,110000,false},
        Case{"Sun at horizon",true,true,QualityLevel::High,0,110000,false},
        Case{"negative-zero horizon",true,true,QualityLevel::High,-0.0f,110000,false},
        Case{"smallest positive elevation",true,true,QualityLevel::High,std::nextafter(0.0f,1.0f),110000,true},
        Case{"night without Sun",true,true,QualityLevel::High,.8f,0,false},
        Case{"below Moon intensity",true,true,QualityLevel::High,.8f,std::nextafter(moon,0.0f),false},
        Case{"equal Moon intensity",true,true,QualityLevel::High,.8f,moon,false},
        Case{"one ULP above Moon",true,true,QualityLevel::High,.8f,
            std::nextafter(moon,std::numeric_limits<float>::infinity()),true},
        Case{"negative intensity",true,true,QualityLevel::High,.8f,-1,false},
        Case{"NaN elevation",true,true,QualityLevel::High,std::numeric_limits<float>::quiet_NaN(),110000,false},
        Case{"infinite elevation",true,true,QualityLevel::High,std::numeric_limits<float>::infinity(),110000,false},
        Case{"NaN intensity",true,true,QualityLevel::High,.8f,std::numeric_limits<float>::quiet_NaN(),false},
        Case{"infinite intensity",true,true,QualityLevel::High,.8f,std::numeric_limits<float>::infinity(),false}
    };
    for (const auto& item : cases) {
        if (!require(should_use_opaque_depth_prepass(item.qualified,item.contact,item.quality,
                item.elevation,item.intensity) == item.expected,item.name)) return false;
    }
    // Each call represents fresh settings/lighting after a live edit or reload.
    // The qualification cache remains true while the workload transitions.
    const std::array transitions{
        should_use_opaque_depth_prepass(true, true, QualityLevel::High, .8f, 110000),
        should_use_opaque_depth_prepass(true, true, QualityLevel::High, -.2f, 0),
        should_use_opaque_depth_prepass(true, true, QualityLevel::High, .8f, 110000),
        should_use_opaque_depth_prepass(true, true, QualityLevel::Medium, .8f, 110000),
        should_use_opaque_depth_prepass(true, false, QualityLevel::High, .8f, 110000),
        should_use_opaque_depth_prepass(false, true, QualityLevel::High, .8f, 110000),
        should_use_opaque_depth_prepass(true, true, QualityLevel::High, .8f, moon)};
    if (!require(transitions == std::array{true, false, true, false, false, false, false},
            "day/night, quality, feature or device transition kept stale activation")) return false;
    std::printf("[unit_opaque_depth_prepass_policy] %zu frame boundaries + 7 transitions PASS\n", cases.size());
    return true;
}

heaven::testing::Register register_opaque_depth_prepass_device(test_opaque_depth_prepass_device_scope);
heaven::testing::Register register_opaque_depth_prepass_frame(test_opaque_depth_prepass_frame_scope);
} // namespace
