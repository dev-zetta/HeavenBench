#include "engine/cli_parser.h"

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <iostream>
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

heaven::ParseResult parse(std::initializer_list<std::string_view> arguments) {
    return heaven::parse_cli(arguments);
}

bool unit_visual_capture_cli_valid() {
    const auto defaults = parse({"heaven"});
    if (defaults.capture.invalid || defaults.capture.camera_pose || defaults.capture.tour_time ||
        defaults.capture.tour_motion ||
        defaults.capture.output || defaults.capture.fov != 60.0f || defaults.capture.frames != 90) return false;

    const auto pose = parse({"heaven", "--camera-pose", "-25,-10,20,20,-5", "--camera-fov", "75",
                             "--capture", "images with spaces/view.ppm", "--capture-frames", "120",
                             "--width", "1280"});
    if (pose.capture.invalid || !pose.diagnostics.empty() || !pose.capture.camera_pose ||
        *pose.capture.camera_pose != std::array<float, 5>{-25, -10, 20, 20, -5} ||
        pose.capture.fov != 75.0f || pose.capture.frames != 120 ||
        pose.capture.output != "images with spaces/view.ppm" || pose.capture.tour_time ||
        pose.seen_keys != std::set<std::string>{"width"} || pose.config.width != 1280) return false;

    const auto tour = parse({"heaven", "--camera-tour-time", "97.15", "--capture", "tour.ppm"});
    if (tour.capture.invalid || !tour.diagnostics.empty() || !tour.capture.tour_time ||
        *tour.capture.tour_time != 97.15f || tour.capture.camera_pose || !tour.seen_keys.empty()) return false;

    for (const auto fov : {"1", "170"}) {
        if (parse({"heaven", "--camera-fov", fov}).capture.invalid) return false;
    }
    for (const auto frames : {"1", "36000"}) {
        if (parse({"heaven", "--capture-frames", frames}).capture.invalid) return false;
    }
    for (const auto time : {"0", "-0", "1e2"}) {
        if (parse({"heaven", "--camera-tour-time", time}).capture.invalid) return false;
    }
    for (const auto pose_value : {"0,0,0,0,-89", "0,0,0,0,89"}) {
        if (parse({"heaven", "--camera-pose", pose_value}).capture.invalid) return false;
    }

    const auto repeat = parse({"heaven", "--camera-fov", "1", "--camera-fov", "170",
                               "--capture", "first.ppm", "--capture", "last.ppm",
                               "--capture-frames", "1", "--capture-frames", "36000"});
    return !repeat.capture.invalid && repeat.capture.fov == 170.0f &&
           repeat.capture.frames == 36000 && repeat.capture.output == "last.ppm";
}

bool unit_visual_capture_cli_invalid() {
    struct Case { std::string_view flag; std::string_view value; };
    constexpr std::array cases{
        Case{"--camera-pose", ""}, Case{"--camera-pose", "1,2,3,4"},
        Case{"--camera-pose", "1,2,3,4,5,6"}, Case{"--camera-pose", "1,2,3,4,"},
        Case{"--camera-pose", "1,2,,4,5"}, Case{"--camera-pose", "1,2,3,4,5junk"},
        Case{"--camera-pose", "nan,2,3,4,5"}, Case{"--camera-pose", "1,inf,3,4,5"},
        Case{"--camera-pose", "1,2,1e99,4,5"},
        Case{"--camera-pose", "1,2,3,4,90"}, Case{"--camera-pose", "1,2,3,4,-90"},
        Case{"--camera-tour-time", "-1"}, Case{"--camera-tour-time", "nan"},
        Case{"--camera-tour-time", "inf"}, Case{"--camera-tour-time", "12s"},
        Case{"--camera-fov", "0"}, Case{"--camera-fov", "170.1"},
        Case{"--camera-fov", "nan"}, Case{"--camera-fov", "inf"},
        Case{"--camera-fov", "60degrees"},
        Case{"--capture-frames", "0"}, Case{"--capture-frames", "-1"},
        Case{"--capture-frames", "36001"}, Case{"--capture-frames", "1.5"},
        Case{"--capture-frames", "99999999999999"}, Case{"--capture-frames", "90frames"},
        Case{"--capture", ""},
        Case{"--capture-count", "0"}, Case{"--capture-count", "601"},
        Case{"--capture-count", "2frames"}, Case{"--capture-count", "-1"},
        Case{"--capture-fps", "0"}, Case{"--capture-fps", "241"},
        Case{"--capture-fps", "nan"}, Case{"--capture-fps", "inf"},
        Case{"--profile", ""}, Case{"--profile-seconds", "62"},
        Case{"--profile-seconds", "3601"}, Case{"--profile-seconds", "nan"},
    };
    for (const auto& test : cases) {
        const auto result = parse({"heaven", test.flag, test.value});
        if (!result.capture.invalid || result.diagnostics.empty() || !result.seen_keys.empty()) {
            std::cerr << "Expected invalid visual-audit argument: " << test.flag << ' ' << test.value << '\n';
            return false;
        }
    }
    constexpr std::array flags{"--camera-pose", "--camera-tour-time", "--camera-fov", "--capture", "--capture-frames", "--capture-count", "--capture-fps", "--profile", "--profile-seconds"};
    const std::string help = heaven::generate_help();
    for (const std::string_view flag : flags) {
        if (!help.contains(flag)) return false;
        if (!parse({"heaven", flag}).capture.invalid) return false;
        const auto missing = parse({"heaven", flag, "--no-grass"});
        if (!missing.capture.invalid || missing.config.features.grass ||
            missing.seen_keys != std::set<std::string>{"features.grass"}) return false;
    }
    const auto conflict = parse({"heaven", "--camera-pose", "1,2,3,4,5", "--camera-tour-time", "0"});
    const auto benchmark_first = parse({"heaven", "--benchmark", "--capture", "out.ppm"});
    const auto benchmark_last = parse({"heaven", "--capture", "out.ppm", "--benchmark"});
    const auto sticky = parse({"heaven", "--camera-fov", "nan", "--camera-fov", "60"});
    return conflict.capture.invalid && benchmark_first.capture.invalid &&
           benchmark_last.capture.invalid && sticky.capture.invalid && sticky.capture.fov == 60.0f;
}

bool unit_visual_capture_explicit_fov() {
    // An absent FOV retains the authored tour lens. An explicit value equal
    // to the ordinary default must still override it; comparing only the
    // numeric value cannot distinguish those two user requests.
    const auto authored = parse({"heaven", "--camera-tour-time", "97.15"});
    const auto explicit_default = parse({"heaven", "--camera-tour-time", "97.15",
                                         "--camera-fov", "60"});
    const auto explicit_before = parse({"heaven", "--camera-fov", "75",
                                        "--camera-tour-time", "97.15"});
    const auto invalid = parse({"heaven", "--camera-tour-time", "97.15",
                               "--camera-fov", "nan"});
    const bool ok = !authored.capture.invalid && !authored.capture.fov_explicit &&
        !explicit_default.capture.invalid && explicit_default.capture.fov_explicit &&
        explicit_default.capture.fov == 60.0f &&
        !explicit_before.capture.invalid && explicit_before.capture.fov_explicit &&
        explicit_before.capture.fov == 75.0f &&
        explicit_before.capture.tour_time == authored.capture.tour_time &&
        invalid.capture.invalid && !invalid.capture.fov_explicit &&
        authored.seen_keys.empty() && explicit_default.seen_keys.empty() &&
        explicit_before.seen_keys.empty();
    if (!ok) std::cerr << "Explicit capture FOV did not retain its override intent\n";
    return ok;
}

bool unit_visual_capture_tour_motion() {
    // Exercise every combination of the switch and its prerequisites. The
    // start time zero is present, and must not be confused with an absent time.
    for (int mask = 0; mask < 8; ++mask) {
        const bool motion = (mask & 1) != 0;
        const bool output = (mask & 2) != 0;
        const bool tour = (mask & 4) != 0;
        std::vector<std::string_view> arguments{"heaven"};
        if (motion) arguments.emplace_back("--capture-tour-motion");
        if (output) arguments.insert(arguments.end(), {"--capture", "motion.ppm"});
        if (tour) arguments.insert(arguments.end(), {"--camera-tour-time", "0"});
        const auto result = heaven::parse_cli(arguments);
        const bool invalid = motion && (!output || !tour);
        if (result.capture.tour_motion != motion || result.capture.invalid != invalid ||
            result.diagnostics.empty() == invalid || !result.seen_keys.empty()) return false;
    }
    const auto repeated = parse({"heaven", "--capture", "motion.ppm",
        "--camera-tour-time", "18", "--capture-tour-motion", "--capture-tour-motion",
        "--width", "1280"});
    const auto pose_conflict = parse({"heaven", "--capture-tour-motion",
        "--capture", "motion.ppm", "--camera-tour-time", "18", "--camera-pose", "0,0,0,0,0"});
    const auto benchmark_conflict = parse({"heaven", "--capture-tour-motion",
        "--capture", "motion.ppm", "--camera-tour-time", "18", "--benchmark"});
    return !repeated.capture.invalid && repeated.capture.tour_motion &&
        repeated.capture.tour_time == 18.0f && repeated.diagnostics.empty() &&
        repeated.config.width == 1280 && repeated.seen_keys == std::set<std::string>{"width"} &&
        pose_conflict.capture.invalid && benchmark_conflict.capture.invalid &&
        heaven::generate_help().contains("--capture-tour-motion");
}

bool unit_postprocess_cli_toggles() {
    struct Toggle {
        std::string_view enable;
        std::string_view disable;
        std::string_view key;
        bool heaven::FeatureToggles::* member;
    };
    constexpr std::array toggles{
        Toggle{"--bloom", "--no-bloom", "features.bloom", &heaven::FeatureToggles::bloom},
        Toggle{"--ambient-occlusion", "--no-ambient-occlusion", "features.ambient_occlusion", &heaven::FeatureToggles::ambient_occlusion},
        Toggle{"--taa", "--no-taa", "features.taa", &heaven::FeatureToggles::taa},
        Toggle{"--depth-of-field", "--no-depth-of-field", "features.depth_of_field", &heaven::FeatureToggles::depth_of_field},
    };
    return std::ranges::all_of(toggles, [](const auto& toggle) {
        const auto enabled = parse({"heaven", toggle.disable, toggle.enable});
        const auto disabled = parse({"heaven", toggle.enable, toggle.disable});
        return (heaven::FeatureToggles{}.*toggle.member) && (enabled.config.features.*toggle.member) &&
            !(disabled.config.features.*toggle.member) && enabled.diagnostics.empty() &&
            disabled.diagnostics.empty() && !enabled.capture.invalid && !disabled.capture.invalid &&
            enabled.seen_keys == std::set<std::string>{std::string(toggle.key)} &&
            disabled.seen_keys == enabled.seen_keys;
    });
}

bool unit_visual_capture_sequence() {
    const auto sequence = parse({"heaven", "--capture-count", "16", "--capture-floating-motion",
        "--capture", "scene.ppm", "--capture-frames", "120"});
    const auto missing = parse({"heaven", "--capture-count", "16"});
    const auto motion_missing = parse({"heaven", "--capture-floating-motion"});
    const auto defaults = parse({"heaven", "--capture", "still.ppm"});
    return !sequence.capture.invalid && sequence.diagnostics.empty() && sequence.seen_keys.empty() &&
        sequence.capture.count == 16 && sequence.capture.frames == 120 && sequence.capture.floating_motion &&
        missing.capture.invalid && motion_missing.capture.invalid && !defaults.capture.invalid &&
        defaults.capture.count == 1 && !defaults.capture.floating_motion;
}

bool unit_visual_capture_clocks_and_profile() {
    const auto slow = parse({"heaven", "--capture", "frames.ppm", "--capture-fps", "15"});
    const auto live = parse({"heaven", "--capture", "frames.ppm", "--capture-realtime"});
    const auto conflict = parse({"heaven", "--capture", "frames.ppm", "--capture-realtime", "--capture-fps", "15"});
    const auto profile = parse({"heaven", "--profile", "timings.json", "--profile-seconds", "65"});
    const auto combined = parse({"heaven", "--profile", "timings.json", "--capture", "frames.ppm"});
    const auto short_run = parse({"heaven", "--profile", "timings.json", "--profile-seconds", "60"});
    const auto missing = parse({"heaven", "--capture-realtime"});
    const auto scored = parse({"heaven", "--benchmark", "--profile", "timings.json"});
    const auto orphan_duration = parse({"heaven", "--profile-seconds", "63"});
    return !slow.capture.invalid && slow.capture.fps == 15 && slow.seen_keys.empty() &&
        !live.capture.invalid && live.capture.realtime && conflict.capture.invalid &&
        !profile.capture.invalid && profile.capture.profile_output == "timings.json" &&
        profile.seen_keys.empty() && combined.capture.invalid && short_run.capture.invalid &&
        missing.capture.invalid && scored.capture.invalid && orphan_duration.capture.invalid;
}

const heaven::testing::Register register_capture_clocks(unit_visual_capture_clocks_and_profile);
const heaven::testing::Register register_capture_sequence(unit_visual_capture_sequence);
const heaven::testing::Register register_capture_valid(unit_visual_capture_cli_valid);
const heaven::testing::Register register_capture_invalid(unit_visual_capture_cli_invalid);
const heaven::testing::Register register_capture_fov(unit_visual_capture_explicit_fov);
const heaven::testing::Register register_capture_motion(unit_visual_capture_tour_motion);
const heaven::testing::Register register_postprocess_toggles(unit_postprocess_cli_toggles);

} // namespace
