#include "engine/gpu_selection.h"

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

bool unit_gpu_selection() {
    using heaven::GpuSelectionIdentity;
    const GpuSelectionIdentity intel{0x8086, 0xe223, {0, 3, 0, 0}};
    const GpuSelectionIdentity amd{0x1002, 0x164e, {0, 9, 0, 0}};
    const GpuSelectionIdentity amd_other{0x1002, 0x164e, {0, 10, 0, 0}};
    bool ok = true;
    const auto check = [&](std::string_view requested, std::string_view pci,
                           const GpuSelectionIdentity& actual,
                           std::string_view expected_error) {
        const auto error = heaven::requested_gpu_error(requested, pci, actual);
        const bool pass = expected_error.empty() ? !error
            : error && error->find(expected_error) != std::string::npos;
        if (!pass) {
            std::printf("[unit_gpu_selection] requested '%.*s' at '%.*s': %s\n",
                static_cast<int>(requested.size()), requested.data(),
                static_cast<int>(pci.size()), pci.data(), error ? error->c_str() : "accepted");
            ok = false;
        }
    };

    check("", "", {}, "");
    check("8086:e223", "", intel, "");
    check("8086:E223", "0000:03:00.0", intel, "");
    check("1002:164e", "0000:09:00.0", amd, "");
    check("1002:164e", "", amd_other, "");
    check("1002:164e", "", intel, "did not honor GPU selection");
    check("8086:164e", "", intel, "did not honor GPU selection");
    check("1002:164e", "0000:09:00.0", amd_other, "physical GPU selection");
    check("1002:164e", "0000:0a:00.0", amd_other, "");
    check("1002:164e", "", {}, "identity is unavailable");
    check("1002:164e", "0000:09:00.0", {0x1002, 0x164e}, "PCI identity is unavailable");
    check("1002:164e", "0000:09:00.0", {0x1002, 0x164e, {0, 9, 0, -1}}, "PCI identity is unavailable");
    // Complete PCI availability has precedence over an earlier component
    // mismatch, and numeric identity has precedence over malformed PCI text.
    check("1002:164e", "0000:09:00.0", {0x1002, 0x164e, {0, 10, 0, -1}}, "PCI identity is unavailable");
    check("1002:164e", "not-a-pci-id", intel, "did not honor GPU selection");
    check("1002:0000", "", {0x1002, 0}, "");
    for (const std::string_view id : {"", "1002:164e!", "1002:164z", "0x1002:164e",
                                     "1002:164e\n", "1002:164", "0000:164e"})
        check(id, "0000:09:00.0", amd, "Invalid requested GPU identity");
    for (const std::string_view pci : {"0000:09:00", "0000:09:20.0", "0000:09:00.8",
                                      "0000:09:00.z", "0000:09:00.0junk", "0000:gg:00.0"})
        check("1002:164e", pci, amd, "Invalid requested GPU PCI identity");
    return ok;
}

const heaven::testing::Register register_gpu_selection(unit_gpu_selection);

} // namespace
