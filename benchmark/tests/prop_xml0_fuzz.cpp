/*
 * HeavenBench — Property test: xml0 bounded-recursion parse safety
 *
 * Feature: codebase-hardening
 *   •: xml0 bounded-recursion parse safety
 *
 *
 * The xml0 parser (xml0_parser.h::xml0_parse_tree) reads an untrusted binary
 * .node/.world/.mat file: every on-disk length, count, index, and the element
 * nesting depth are hostile until validated. This property pins the parser's
 * bounded-recursion / out-of-bounds-safety contract
 * FOR ALL byte sequences supplied as an xml0 file,
 * xml0_parse_tree returns either a parsed tree or an empty result — without
 * overflowing the stack, without reading outside the file buffer, and without
 * terminating the process.
 *
 * xml0_parse_tree takes a filepath, so each fuzz input is written to a real
 * temp file on disk and parsed through the public entry point. The property
 * feeds several families of hostile input:
 *
 *   • Random bytes — an arbitrary buffer of arbitrary length. Almost always
 *     rejected (bad magic / too short), exercising the early-out paths.
 *   • Magic-prefixed random — "xml0" + 4 header bytes + random tail. Gets past
 *     the magic/size gate into the string-pool and tree decoders, exercising
 *     read_lps / read_index / read_byte bounds checks against ctx.size.
 *   • Truncated valid — a synthesised, fully-valid xml0 cut off at a random
 *     length, driving every short-read / end-of-buffer guard at every field
 *     boundary.
 *   • Mutated valid — a synthesised valid xml0 with a handful of random byte
 *     overwrites, perturbing pool counts, string lengths, tag/attr indices, and
 *     child counts alike.
 *   • Deeply-nested chains — a single child chain nested far past
 *     kXmlMaxDepth (256). This is the headline case for the recursion cap
 * without the cap the recursive parse_element
 *     descent would overflow the stack; with it the parse stops at the cap and
 *     xml0_parse_tree surfaces an empty result. The property also checks that a
 *     chain nested *below* the cap still parses to a non-empty tree (parity).
 *
 * The success-path assertions are intentionally light: the core property is
 * that control returns from xml0_parse_tree in a defined state for every input
 * (the no-crash / no-stack-overflow / no-OOB guarantee, enforced when the suite
 * is compiled under AddressSanitizer). xml0_parse_tree has no Result type — it
 * returns an XmlNode that is empty (default-constructed) on any failure — so the
 * test touches the returned node so the call cannot be optimised away, and adds
 * a deterministic check that a known-valid buffer parses to the expected tree.
 *
 * xml0_parse_tree is GPU-free and host-testable. This file self-registers with
 * the RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point. RapidCheck's default of 100 successful iterations
 * meets the "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "xml0_parser.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Little-endian POD appenders ────────────────────────────────

// xml0_parse_tree reads multi-byte fields with memcpy from the raw buffer, so
// we serialise the host's byte representation directly (the renderer targets
// little-endian x86).

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

// Append a strict length-prefixed string (header-pool encoding): uint32 length
// INCLUDING the null terminator, the ASCII content bytes, then the terminator.
void put_lps(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size()) + 1u);
    b.insert(b.end(), s.begin(), s.end());
    b.push_back('\0');
}

// ── Header-pool emitter ────────────────────────────────────────

// Emit the xml0 prologue shared by every synthesised buffer: the "xml0" magic,
// a 4-byte (ignored) header word, the strict tag-string pool, then the
// attr-name and value pools. The tag-read loop in xml0_parse_tree consumes
// strict LPS entries until one fails to decode; setting num_attr_names to a
// small value (0) makes the next 4 bytes decode as an invalid LPS length, which
// cleanly terminates the tag loop without the parser ever mistaking a count for
// a tag. Keeping every pool ≤ 255 entries holds the index widths at one byte.
void put_header(std::vector<std::uint8_t>& b,
                const std::vector<std::string>& tags,
                const std::vector<std::string>& attr_names,
                const std::vector<std::string>& values) {
    b.push_back('x');
    b.push_back('m');
    b.push_back('l');
    b.push_back('0');
    put_u32(b, 0); // header word, ignored by the parser

    for (const std::string& t : tags) put_lps(b, t);

    put_u32(b, static_cast<std::uint32_t>(attr_names.size()));
    for (const std::string& a : attr_names) put_lps(b, a);

    put_u32(b, static_cast<std::uint32_t>(values.size()));
    for (const std::string& v : values) put_lps(b, v);
}

// ── Valid-file synthesiser ─────────────────────────────────────

// Build a small, fully-valid xml0 image whose tree is:
//   <node>
//     <mesh>model.mesh</mesh>
//     <surface/>
//   </node>
// Encoded with no attributes or values (both pools empty) so the tag loop
// terminates deterministically. Each element child is wrapped in optional 0
// separator bytes, exactly as the parser's child loop expects.
std::vector<std::uint8_t> build_valid_xml0() {
    std::vector<std::uint8_t> b;
    put_header(b, {"node", "mesh", "surface"}, {}, {});

    // Root <node>: tag_idx=0, num_attrs=0, separator, num_data_children=0,
    // num_element_children=2.
    b.push_back(0); // tag_idx → "node"
    b.push_back(0); // num_attrs
    b.push_back(0); // separator
    b.push_back(0); // num_data_children
    b.push_back(2); // num_element_children

    // Child 1 — <mesh>model.mesh</mesh>: a leading 0 separator, the element
    // (tag_idx=1, 0 attrs, separator, num_data_children=1 + one data string),
    // then a trailing 0 separator.
    b.push_back(0); // leading separator
    b.push_back(1); // tag_idx → "mesh"
    b.push_back(0); // num_attrs
    b.push_back(0); // separator
    b.push_back(1); // num_data_children
    put_lps(b, "model.mesh"); // data string (non-strict LPS, same encoding)
    b.push_back(0); // trailing separator

    // Child 2 — <surface/>: leading 0, element (tag_idx=2, 0 attrs, separator,
    // num_data_children=0, num_element_children=0), trailing 0.
    b.push_back(0); // leading separator
    b.push_back(2); // tag_idx → "surface"
    b.push_back(0); // num_attrs
    b.push_back(0); // separator
    b.push_back(0); // num_data_children
    b.push_back(0); // num_element_children
    b.push_back(0); // trailing separator

    return b;
}

// ── Deeply-nested synthesiser ──────────────────────────────────

// Build an xml0 image whose tree is a single element-child chain `levels` deep:
//   <node><node>…<node/>…</node></node>
// The root sits at recursion depth 0 and the leaf at depth `levels`, so a
// `levels` of 256 or more drives parse_element to its kXmlMaxDepth cap. A single
// "node" tag keeps the tag pool tiny (one-byte indices).
std::vector<std::uint8_t> build_nested_xml0(int levels) {
    std::vector<std::uint8_t> b;
    put_header(b, {"node"}, {}, {});

    // Opener for each nesting level: element header with one element child plus
    // that child's leading 0 separator.
    for (int i = 0; i < levels; ++i) {
        b.push_back(0); // tag_idx → "node"
        b.push_back(0); // num_attrs
        b.push_back(0); // separator
        b.push_back(0); // num_data_children
        b.push_back(1); // num_element_children
        b.push_back(0); // leading separator for the single child
    }

    // Leaf element: no children.
    b.push_back(0); // tag_idx → "node"
    b.push_back(0); // num_attrs
    b.push_back(0); // separator
    b.push_back(0); // num_data_children
    b.push_back(0); // num_element_children

    // One trailing 0 separator per opened level.
    for (int i = 0; i < levels; ++i) b.push_back(0);

    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// xml0_parse_tree opens a path, so each fuzz input is written to a
// uniquely-named file under the system temp directory and removed once the
// parser returns.
struct TempFile {
    std::filesystem::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_xml0_fuzz_{}.bin", id);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
            }
            std::fclose(f);
            ok = true;
        }
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec); // best-effort cleanup
    }
};

// Parse a fuzz buffer and assert the safety contract: control returns in a
// defined state (the no-crash / no-stack-overflow / no-OOB guarantee under
// ASan). Touch the returned node so the call cannot be optimised away. Returns
// the parsed node so callers can add case-specific structural assertions.
XmlNode parse_safely(const std::vector<std::uint8_t>& bytes) {
    TempFile tf(bytes);
    RC_ASSERT(tf.ok); // temp file must be writable, else the test is invalid

    XmlNode root = xml0_parse_tree(tf.path.string().c_str());

    // Force observation of the result. An empty (failure) node has an empty tag
    // and no children; a parsed node may have either. Either outcome is
    // acceptable — the property is that we got here at all.
    [[maybe_unused]] volatile std::size_t sink =
        root.tag.size() + root.children.size();
    return root;
}

bool run_p6_random_bytes() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — arbitrary bytes (Validates: Requirements 3.4, 3.5, 22.1)",
        [] {
            const auto bytes =
                *rc::gen::container<std::vector<std::uint8_t>>(
                    rc::gen::arbitrary<std::uint8_t>());
            parse_safely(bytes);
        });
}

bool run_p6_magic_prefixed_random() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — magic-prefixed random tail (Validates: Requirements 3.4, "
        "3.5, 22.1)",
        [] {
            std::vector<std::uint8_t> bytes = {'x', 'm', 'l', '0', 0, 0, 0, 0};
            const auto tail =
                *rc::gen::container<std::vector<std::uint8_t>>(
                    rc::gen::arbitrary<std::uint8_t>());
            bytes.insert(bytes.end(), tail.begin(), tail.end());
            parse_safely(bytes);
        });
}

bool run_p6_truncated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — truncated valid file (Validates: Requirements 3.4, 3.5, "
        "22.1)",
        [] {
            const std::vector<std::uint8_t> full = build_valid_xml0();
            const std::size_t cut =
                *rc::gen::inRange<std::size_t>(0, full.size() + 1);
            const std::vector<std::uint8_t> truncated(full.begin(),
                full.begin() + static_cast<std::ptrdiff_t>(cut));
            parse_safely(truncated);
        });
}

bool run_p6_mutated_valid() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — mutated valid file (Validates: Requirements 3.4, 3.5, 22.1)",
        [] {
            std::vector<std::uint8_t> bytes = build_valid_xml0();
            const int mutations = *rc::gen::inRange(1, 8);
            for (int i = 0; i < mutations && !bytes.empty(); ++i) {
                const std::size_t pos =
                    *rc::gen::inRange<std::size_t>(0, bytes.size());
                bytes[pos] = *rc::gen::arbitrary<std::uint8_t>();
            }
            parse_safely(bytes);
        });
}

bool run_p6_deeply_nested_over_cap() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — chain nested past kXmlMaxDepth (Validates: Requirements 3.1, "
        "3.2, 3.5, 22.1)",
        [] {
            // Well past the 256 cap: the recursion-depth guard must stop the
            // descent and surface an empty result rather than overflowing the
            // stack. Use a band strictly above the cap to avoid the exact
            // boundary.
            const int levels =
                *rc::gen::inRange(heaven::loadval::kXmlMaxDepth + 1,
                                  heaven::loadval::kXmlMaxDepth + 256);
            const std::vector<std::uint8_t> bytes = build_nested_xml0(levels);
            const XmlNode root = parse_safely(bytes);

            // Past the cap, parse_element flags ctx.error and xml0_parse_tree
            // returns an empty node.
            RC_ASSERT(root.tag.empty());
            RC_ASSERT(root.children.empty());
        });
}

bool run_p6_deeply_nested_under_cap() {
    return rc::check(
        "Feature: codebase-hardening, Property 6: xml0 bounded-recursion parse "
        "safety — chain nested below kXmlMaxDepth parses (Validates: "
        "Requirements 3.5, 22.1)",
        [] {
            // Below the cap the deep-but-valid chain parses to a non-empty tree
            // (parity: the cap only rejects genuinely over-deep input). The
            // upper bound stays safely under 256.
            const int levels =
                *rc::gen::inRange(1, heaven::loadval::kXmlMaxDepth - 5);
            const std::vector<std::uint8_t> bytes = build_nested_xml0(levels);
            const XmlNode root = parse_safely(bytes);

            // The root is the first element of the chain → tag "node".
            RC_ASSERT(root.tag == "node");
        });
}

// Deterministic guard: the synthesised valid buffer must parse to the expected
// tree, confirming the synthesiser matches the on-disk layout (so the fuzzing
// mutations actually start from a parseable baseline).
bool run_valid_buffer_guard() {
    const std::vector<std::uint8_t> bytes = build_valid_xml0();
    TempFile tf(bytes);
    if (!tf.ok) {
        std::printf("[prop_xml0_fuzz] valid-buffer guard: temp file not writable\n");
        return false;
    }

    const XmlNode root = xml0_parse_tree(tf.path.string().c_str());
    bool ok = true;
    if (root.tag != "node") {
        std::printf("[prop_xml0_fuzz] valid-buffer guard: root tag '%s' != 'node'\n",
                    root.tag.c_str());
        ok = false;
    }
    if (root.children.size() != 2u) {
        std::printf("[prop_xml0_fuzz] valid-buffer guard: root has %zu children, "
                    "expected 2\n", root.children.size());
        ok = false;
    } else {
        if (root.children[0].tag != "mesh" ||
            root.children[0].text != "model.mesh") {
            std::printf("[prop_xml0_fuzz] valid-buffer guard: child 0 tag '%s' "
                        "text '%s' (expected mesh/model.mesh)\n",
                        root.children[0].tag.c_str(),
                        root.children[0].text.c_str());
            ok = false;
        }
        if (root.children[1].tag != "surface") {
            std::printf("[prop_xml0_fuzz] valid-buffer guard: child 1 tag '%s' "
                        "(expected surface)\n", root.children[1].tag.c_str());
            ok = false;
        }
    }
    return ok;
}

bool run_xml0_fuzz_properties() {
    bool ok = true;
    ok = run_valid_buffer_guard() && ok;
    ok = run_p6_random_bytes() && ok;
    ok = run_p6_magic_prefixed_random() && ok;
    ok = run_p6_truncated_valid() && ok;
    ok = run_p6_mutated_valid() && ok;
    ok = run_p6_deeply_nested_over_cap() && ok;
    ok = run_p6_deeply_nested_under_cap() && ok;
    return ok;
}

const heaven::testing::Register g_register_xml0_fuzz{&run_xml0_fuzz_properties};

} // namespace
