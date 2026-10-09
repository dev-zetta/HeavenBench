#include "rhi/hdr_surface_formats.h"

#include <array>
#include <cstdio>
#include <functional>
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

bool unit_hdr_surface_formats() {
    using namespace heaven;
    bool ok = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_hdr_surface_formats] FAILED: %s\n", message);
            ok = false;
        }
    };
    constexpr auto pq = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    constexpr auto srgb = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    constexpr std::array allowed{
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    for (const auto format : allowed) {
        const std::array hdr{VkSurfaceFormatKHR{format, pq}};
        const std::array sdr{VkSurfaceFormatKHR{format, srgb}};
        check(supports_hdr10_surface(hdr), "supported HDR10 pair rejected");
        check(!supports_hdr10_surface(sdr), "high-precision SDR accepted as HDR10");
    }
    const std::array wrong_pairs{
        VkSurfaceFormatKHR{VK_FORMAT_R16G16B16A16_SFLOAT, srgb},
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, pq},
        VkSurfaceFormatKHR{VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}};
    check(!supports_hdr10_surface(wrong_pairs),
          "format and colorspace from different pairs were combined");
    check(!supports_hdr10_surface({}), "empty surface list accepted");

    // A compositor may change its list between the count and data calls. Never
    // accept the partial HDR pair until a complete retry succeeds.
    int data_queries = 0;
    auto grows_then_sdr = require_hdr10_surface(
        [&](std::uint32_t* count, VkSurfaceFormatKHR* formats) {
            *count = 1;
            if (!formats) return VK_SUCCESS;
            ++data_queries;
            formats[0] = {VK_FORMAT_R16G16B16A16_SFLOAT,
                          data_queries == 1 ? pq : srgb};
            return data_queries == 1 ? VK_INCOMPLETE : VK_SUCCESS;
        });
    check(!grows_then_sdr && data_queries == 2,
          "incomplete HDR list bypassed the complete SDR result");

    data_queries = 0;
    auto grows_then_hdr = require_hdr10_surface(
        [&](std::uint32_t* count, VkSurfaceFormatKHR* formats) {
            *count = 1;
            if (!formats) return VK_SUCCESS;
            formats[0] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, pq};
            return ++data_queries == 1 ? VK_INCOMPLETE : VK_SUCCESS;
        });
    check(grows_then_hdr.has_value() && data_queries == 2,
          "stable HDR retry rejected");

    data_queries = 0;
    auto unstable = require_hdr10_surface(
        [&](std::uint32_t* count, VkSurfaceFormatKHR* formats) {
            *count = 1;
            if (!formats) return VK_SUCCESS;
            ++data_queries;
            formats[0] = {VK_FORMAT_R16G16B16A16_SFLOAT, pq};
            return VK_INCOMPLETE;
        });
    check(!unstable && data_queries == 4,
          "continually changing surface list was accepted or not bounded");
    auto empty = require_hdr10_surface(
        [](std::uint32_t* count, VkSurfaceFormatKHR*) {
            *count = 0;
            return VK_SUCCESS;
        });
    check(!empty, "empty enumerated list accepted");
    auto failed_count = require_hdr10_surface(
        [](std::uint32_t*, VkSurfaceFormatKHR*) {
            return VK_ERROR_SURFACE_LOST_KHR;
        });
    check(!failed_count, "failed count query accepted");
    auto failed_data = require_hdr10_surface(
        [](std::uint32_t* count, VkSurfaceFormatKHR* formats) {
            *count = 1;
            return formats ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
        });
    check(!failed_data, "failed data query accepted");
    return ok;
}

const heaven::testing::Register register_hdr_surface_formats(unit_hdr_surface_formats);

} // namespace
