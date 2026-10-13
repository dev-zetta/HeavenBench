#include "engine/capture_camera_motion.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace {
bool capture_camera_motion_contract() {
    using namespace heaven;
    const CaptureCameraPose start{1, 2, 3, 359, -20}, end{5, -2, 7, 1, 40};
    const auto first = sample_capture_camera_motion(start, end, 0, 5);
    const auto middle = sample_capture_camera_motion(start, end, 2, 5);
    const auto last = sample_capture_camera_motion(start, end, 4, 5);
    if (!first || !middle || !last || first->pose != start || last->pose != end ||
        first->fraction != 0 || middle->fraction != .5 || last->fraction != 1 ||
        middle->pose != CaptureCameraPose{3,0,5,360,10} || middle->source_index != 3 ||
        middle->total_sources != 5) return false;
    const auto& reverse_start = end;
    const auto& reverse_end = start;
    const auto reverse = sample_capture_camera_motion(reverse_start, reverse_end, 2, 5);
    if (!reverse || reverse->pose[3] != 0) return false;
    // The final source includes the consecutive screenshot tail: warmup 89
    // plus 32 captures gives 120 accepted sources, with the endpoint at 120.
    const auto tail = sample_capture_camera_motion(start, end, 119, 89 + 32 - 1);
    const auto before_tail = sample_capture_camera_motion(start, end, 88, 89 + 32 - 1);
    if (!tail || !before_tail || tail->pose != end || before_tail->fraction >= 1) return false;
    // Repeat rejected attempts without advancing the accepted count. Every
    // retry must reuse the exact source pose; generated presents are absent
    // from this clock. This is the same stateless input used by main.cpp.
    std::uint64_t accepted = 0;
    for (const bool presented : {false, false, true, false, true, true, false}) {
        const auto attempt = sample_capture_camera_motion(start, end, accepted, 5);
        const auto retry = sample_capture_camera_motion(start, end, accepted, 5);
        if (!attempt || !retry || attempt->pose != retry->pose ||
            attempt->fraction != retry->fraction || attempt->source_index != accepted + 1) return false;
        if (presented) ++accepted;
    }
    if (accepted != 3) return false;
    const auto clamped = sample_capture_camera_motion(start, end, std::numeric_limits<std::uint64_t>::max(), 5);
    if (!clamped || clamped->pose != end || clamped->source_index != 5) return false;
    CaptureCameraPose invalid = end;
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        invalid = end; invalid[i] = std::numeric_limits<float>::quiet_NaN();
        if (sample_capture_camera_motion(start, invalid, 0, 5)) return false;
        invalid[i] = std::numeric_limits<float>::infinity();
        if (sample_capture_camera_motion(invalid, end, 0, 5)) return false;
    }
    invalid = end; invalid[4] = 89.001f;
    if (sample_capture_camera_motion(start, invalid, 0, 5) ||
        sample_capture_camera_motion(start, end, 0, 0) ||
        sample_capture_camera_motion(start, end, 0, 1)) return false;
    // Convex position interpolation uses double intermediates: finite extreme
    // endpoints must not overflow their difference in float arithmetic.
    const float maximum = std::numeric_limits<float>::max();
    const auto extreme = sample_capture_camera_motion({-maximum,0,0,0,-89}, {maximum,0,0,0,89}, 1, 3);
    return extreme && extreme->pose[0] == 0 && extreme->pose[4] == 0;
}
} // namespace

#ifdef HEAVEN_CAPTURE_CAMERA_MOTION_TEST_MAIN
int main() {
    if (!capture_camera_motion_contract()) {
        std::puts("FAIL: deterministic accepted-source camera motion");
        return 1;
    }
    std::puts("PASS: camera endpoints, midpoint, shortest yaw, capture tail, rejected retries and finite limits; no GPU");
}
#else
namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
const Register register_capture_camera_motion_contract(capture_camera_motion_contract);
} // namespace heaven::testing
#endif
