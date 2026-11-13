#include "benchmark/gpu_clock_telemetry.h"
#include "benchmark/microprofiler.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
bool gpu_clock_telemetry_test() {
    using namespace heaven::profile;
    Recorder recorder;
    recorder.set_thread_name("Main");
    recorder.begin_capture();
    const Context first_capture{0, 0, 0, 0, 0, recorder.session_id()};
    recorder.counter("CPU only", 1);
    if (recorder.has_gpu_observations()) return false;
    // Explicitly unsupported GPU results still establish a native producer;
    // neither a timestamp nor fabricated zero counters are required.
    recorder.gpu_sample(first_capture, 1, 0, "Unavailable native query", 0, 0,
        SampleStatus::Unsupported, TimerSource::Unavailable, "test unsupported adapter");
    if (!recorder.has_gpu_observations()) return false;
    recorder.begin_capture();
    if (recorder.has_gpu_observations()) return false;
    recorder.gpu_sample(first_capture, 2, 0, "Previous capture", 0, 0,
        SampleStatus::Unsupported, TimerSource::Unavailable, "late prior epoch");
    if (recorder.has_gpu_observations()) return false;
    recorder.set_active(false);
    const auto root = std::filesystem::temp_directory_path() /
        ("heaven-gpu-clocks-" + std::to_string(Recorder::steady_now_ns()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto adapter = root / "0000:03:00.0";
    const auto clocks = adapter / "tile0/gt1/freq0";
    std::filesystem::create_directories(clocks);
    std::ofstream(adapter / "vendor") << "0x8086\n";
    std::ofstream(adapter / "device") << "0xe223\n";
    std::ofstream(clocks / "act_freq") << "0\n";
    std::ofstream(clocks / "cur_freq") << "400\n";
    std::ofstream(clocks / "min_freq") << "1200\n";
    std::ofstream(clocks / "max_freq") << "2800\n";
    const PciIdentity identity{0x8086, 0xe223, 0, 3, 0, 0};
    auto found = discover_xe_clocks(identity, root);
    const auto check = [](bool ok, const char* message) {
        if (!ok) std::printf("[unit_gpu_clock_telemetry] %s\n", message);
        return ok;
    };
    if (!check(found.pci_address == "0000:03:00.0" && found.sources.size() == 1 &&
            found.sources.front().label == "tile0/gt1", "sparse xe topology discovery failed")) return false;
    const auto sample = read_xe_clocks(found.sources.front());
    if (!check(sample.actual_mhz == 0 && sample.requested_mhz == 400 &&
            sample.minimum_mhz == 1200 && sample.maximum_mhz == 2800,
            "clock units changed or idle zero was reported unavailable")) return false;
    std::ofstream(clocks / "act_freq") << "2800 garbage\n";
    std::ofstream(clocks / "cur_freq") << "-1\n";
    std::ofstream(clocks / "min_freq") << "4294967296\n";
    std::filesystem::remove(clocks / "max_freq");
    const auto unavailable = read_xe_clocks(found.sources.front());
    if (!check(!unavailable.actual_mhz && !unavailable.requested_mhz &&
            !unavailable.minimum_mhz && !unavailable.maximum_mhz,
            "malformed or missing sysfs values became invented measurements")) return false;
    auto wrong = identity;
    wrong.device = 0x1234;
    if (!check(discover_xe_clocks(wrong, root).sources.empty(), "mismatched device accepted")) return false;
    wrong = identity;
    wrong.bus = 4;
    if (!check(discover_xe_clocks(wrong, root).sources.empty(), "unrelated adapter substituted")) return false;
    wrong = identity;
    wrong.function = -1;
    if (!check(discover_xe_clocks(wrong, root).sources.empty(), "partial PCI identity accepted")) return false;
    wrong = identity;
    wrong.vendor = 0x1002;
    return check(discover_xe_clocks(wrong, root).sources.empty(), "non-Intel adapter accepted");
}
const heaven::testing::Register registered{gpu_clock_telemetry_test};
}
