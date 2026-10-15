#!/usr/bin/env python3
"""Compile the actual readback selector and isolated historical/flag-policy negatives."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
HEADER = HERE / "frame_generation_capture_memory_type.h"
CAPTURE = HERE / "frame_generation_capture.cpp"
SIGNATURE = "inline uint32_t select_frame_generation_readback_memory_type("
# Exact former allocate() selection loop, with its requirements mask bound to
# the helper argument. No Vulkan device, memory allocation or queue is involved.
OLD_POLICY = r'''
    const VkMemoryRequirements requirements{0, 0, memory_type_bits};
    uint32_t selected = UINT32_MAX;
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if (memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
            VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)) continue;
        if (!(requirements.memoryTypeBits & (1u << type)) ||
                !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        if (selected == UINT32_MAX) selected = type;
        if (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            selected = type; break;
        }
    }
    return selected;
'''
FIXTURE = r'''
#define VK_NO_PROTOTYPES
#include "frame_generation_capture_memory_type.h"
#include <cstdio>
using heaven::select_frame_generation_readback_memory_type;
constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
bool check(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits, uint32_t expected, const char* reason) {
    if (select_frame_generation_readback_memory_type(memory, bits) == expected) return true;
    std::fprintf(stderr, "%s\n", reason);
    return false;
}
int main() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount = 6;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[2].propertyFlags = visible | coherent;
    memory.memoryTypes[5].propertyFlags = visible | coherent | cached;
    if (!check(memory, 0x7ad, 5, "Cached coherent readback was not preferred")) return 1;
    memory = {}; memory.memoryTypeCount = 3;
    memory.memoryTypes[0].propertyFlags = visible | coherent;
    memory.memoryTypes[2].propertyFlags = visible | cached;
    if (!check(memory, 5, 2, "Cached noncoherent readback was not preferred")) return 1;
    if (!check(memory, 1, 0, "Disallowed cached memory selected")) return 1;
    memory = {}; memory.memoryTypeCount = 3;
    memory.memoryTypes[0].propertyFlags = coherent | cached;
    memory.memoryTypes[1].propertyFlags = visible | coherent;
    memory.memoryTypes[2].propertyFlags = visible | coherent | cached;
    if (!check(memory, 3, 1, "Non-visible memory selected")) return 1;
    memory.memoryTypes[0].propertyFlags = visible | coherent | cached | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
    memory.memoryTypes[1].propertyFlags = visible | coherent | cached | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
    memory.memoryTypes[2].propertyFlags = visible | coherent;
    if (!check(memory, 7, 2, "Special AMD memory was not excluded")) return 1;
    if (!check(memory, 3, UINT32_MAX, "No-candidate selection did not fail closed")) return 1;
    memory = {}; memory.memoryTypeCount = 2;
    memory.memoryTypes[0].propertyFlags = visible | cached;
    memory.memoryTypes[1].propertyFlags = visible | coherent | cached;
    if (!check(memory, 7, 1, "Coherence tie-break was not retained")) return 1;
    memory.memoryTypeCount = 3;
    memory.memoryTypes[2].propertyFlags = visible | coherent | cached;
    memory.memoryTypes[0].propertyFlags = visible | coherent | cached;
    if (!check(memory, 7, 0, "Equal-ranked selection was not deterministic")) return 1;
    memory = {}; memory.memoryTypeCount = 2;
    memory.memoryTypes[0].propertyFlags = visible;
    memory.memoryTypes[1].propertyFlags = visible | coherent;
    if (!check(memory, 3, 1, "Uncached coherent fallback was not retained")) return 1;
    if (!check(memory, 1, 0, "Uncached noncoherent fallback was not retained")) return 1;
    if (!check(memory, 0, UINT32_MAX, "Empty eligible mask did not fail closed")) return 1;
    memory.memoryTypeCount = 0;
    if (!check(memory, UINT32_MAX, UINT32_MAX, "Empty memory table did not fail closed")) return 1;
    std::puts("Readback memory selector PASS: cached/coherence order, eligibility, AMD exclusions, empty candidates, deterministic ties and fallbacks; no GPU API calls");
}
'''


def replace_body(header, body):
    start = header.index("{", header.index(SIGNATURE))
    depth = 1
    for end in range(start + 1, len(header)):
        depth += (header[end] == "{") - (header[end] == "}")
        if depth == 0:
            return header[:start + 1] + body + header[end:]
    raise ValueError("Selector body is unbalanced")


def source_contract(source):
    compact = "".join(source.split())
    checks = {
        "production_uses_tested_selector": "select_frame_generation_readback_memory_type(memory,requirements.memoryTypeBits)" in compact,
        "coherence_uses_selected_flags": "buffer.coherent=(buffer.property_flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;" in compact,
        "noncoherent_invalidate_retained": "if(!buffers[index].coherent){" in compact and "vkInvalidateMappedMemoryRanges(device,1,&range)" in compact and "range.size=VK_WHOLE_SIZE;" in compact,
        "actual_allocation_cap_retained": "requirements.size>capture_limit-allocated_bytes" in compact and "capture_limit=128ull*1024*1024" in compact,
        "default_four_pair_limit_retained": "uint32_tpair_limit=4;" in compact,
        "configured_pair_limit_bounded": "impl_->pairs<impl_->pair_limit" in compact and
            "impl_->pair_limit=parse_pair_limit(limit)" in compact and "parsed>32/10" in compact and "if(parsed>32)" in compact,
        "same_two_or_four_buffers": "constsize_tcount=ui_requested?4:2;" in compact,
    }
    order = [source.index(token) for token in (
        "const auto wait_begin = now_ns();", "vkWaitForFences(p.device", "const auto completed = now_ns();",
        "p.write_image(1,", "const auto images_export_completed = now_ns();", "const auto metadata =")]
    checks["wait_and_image_export_times_separate"] = order == sorted(order)
    fields = ("readback_wait_begin_cpu_monotonic_ns", "images_export_completed_cpu_monotonic_ns",
              "readback_wait_duration_ns", "images_export_duration_ns", "readback_buffers",
              "memory_type_index", "property_flags", "memory_type_bits", "allocation_bytes")
    checks["metadata_discloses_timing_and_memory"] = all(field in source for field in fields)
    if not all(checks.values()):
        raise ValueError("Capture source contract changed: " + str(checks))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--vulkan-include", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    header, capture = HEADER.read_text(), CAPTURE.read_text()
    checks = source_contract(capture)
    variants = {
        "positive": (header, None),
        "old-first-coherent": (replace_body(header, OLD_POLICY), "Cached coherent readback was not preferred"),
        "ignore-eligibility": (header.replace("!(memory_type_bits & (1u << type))", "false"), "Disallowed cached memory selected"),
        "ignore-visibility": (header.replace("!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)", "false"), "Non-visible memory selected"),
        "ignore-amd-exclusions": (header.replace("flags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |\n                VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)", "false"), "Special AMD memory was not excluded"),
        "ignore-coherence-tie": (header.replace("(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 1u : 0u)", "0u"), "Coherence tie-break was not retained"),
        "replace-equal-ranked": (header.replace("rank > best_rank", "rank >= best_rank"), "Equal-ranked selection was not deterministic"),
    }
    args.work_dir.mkdir(parents=True, exist_ok=True)
    records = []
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe")
    for name, (variant, expected) in variants.items():
        if expected and variant == header:
            raise ValueError("Negative mutation no longer applies: " + name)
        directory = args.work_dir / name
        directory.mkdir(exist_ok=True)
        (directory / HEADER.name).write_text(variant)
        source = directory / "test.cpp"
        source.write_text(FIXTURE)
        executable = directory / ("test.exe" if msvc else "test")
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", "/I" + str(args.vulkan_include),
                    str(source), "/Fe:" + str(executable), "/Fo:" + str(directory / "test.obj")]
                   if msvc else [args.cxx, "-std=c++20", "-I" + str(args.vulkan_include), str(source), "-o", str(executable)])
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (directory / "compile.log").write_text(compiled.stdout)
        if compiled.returncode:
            raise ValueError(name + " failed to compile; not a valid negative control")
        result = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (directory / "run.log").write_text(result.stdout)
        passed = result.returncode == (1 if expected else 0) and (not expected or expected in result.stdout)
        if not passed:
            raise ValueError(name + " did not meet its specific oracle: " + result.stdout)
        records.append({"case": name, "exit_code": result.returncode, "expected_failure": expected, "passed": True})
    (args.work_dir / "receipt.json").write_text(json.dumps({
        "schema": 1, "source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (HEADER, CAPTURE, Path(__file__))},
        "compiler": args.cxx, "vulkan_include": str(args.vulkan_include), "source_contract": checks,
        "compiled_cases": records, "GPU_API_calls": 0,
        "limits": "CPU-only actual-header policy and isolated source negatives. No native allocation/timing/continuity or driver qualification."}, indent=2) + "\n")
    print("Capture memory contract PASS: actual selector +six compiled negative controls; disabled-path/cap/invalidate/timing source guards; no GPU API calls")


if __name__ == "__main__":
    main()
