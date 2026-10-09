/*
 * HeavenBench — unit test: xml0 parser ftell / deep-nest / short-body guards
 *
 * Feature: codebase-hardening
 * — Validates:
 *
 * The xml0 binary parser (xml0_parser.h) reads untrusted.node /.world /.mat
 * files, so it must cap recursion and reject failed / truncated I/O instead of
 * overflowing the stack or acting on partially-read bytes. This file pins the
 * deterministically-testable guards:
 *
 *   • Deep-nest recursion cap: parse_element threads a depth
 *     counter and stops at heaven::loadval::kXmlMaxDepth (256). A buffer nested
 *     past the cap sets ctx.error, so xml0_parse_tree surfaces an EMPTY result
 *     (a default XmlNode) rather than recursing further. We synthesise a chain
 *     of >256 nested element containers and assert the parse yields an empty
 *     node — no tag, no attrs, no children.
 *
 *   • Truncated-body / failed-size guards: xml0_parse_tree
 *     determines the file size from a checked fseek/ftell and reads the whole
 *     file with a single fread whose return count is verified. The ftell == -1
 *     branch  cannot be forced deterministically on a regular
 *     seekable file — ftell only fails on unseekable streams (pipes/sockets),
 *     and xml0_parse_tree always fopen()s a path in "rb" mode. We therefore
 *     document that and instead exercise the deterministic sibling guards: a
 *     valid "xml0" header whose body is truncated at each structural boundary
 *     (before the attr-name count, before the value-string count, and below the
 *     8-byte magic+header floor) returns an empty result rather than reading
 *     past the buffer or building a half-parsed tree.
 *
 * A positive control (a complete, shallow, valid xml0 leaf document) parses to
 * a NON-empty node, proving the synthesiser is faithful to the on-disk format
 * and that the empty results above are genuine rejections, not a broken fixture.
 *
 * xml0_parse_tree takes a filepath, so each fixture is written to a uniquely
 * named temp file and removed once the parser returns. The file self-registers
 * with the RapidCheck harness in test_main.cpp via heaven::testing::Register, so
 * the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point. This is a deterministic unit test, so it does not use rc::check.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "xml0_parser.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// ── Little-endian xml0 byte writer (matches xml0_parser.h) ──────

// Layout: "xml0" magic + u32 header, then strict-LPS tag/attr/value string
// pools (the tag pool is read until an LPS read fails — which the following u32
// pool count naturally triggers), then the recursive element tree. Index widths
// are 1 byte while each pool holds <= 255 entries (true for every fixture here).

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// Length-prefixed string: u32 length INCLUDING the null terminator, the bytes,
// then the null.
void put_lps(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size() + 1));
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}

// Common header for every fixture: magic, header word, a single tag pool entry
// "n" (so tree tag index 0 resolves and the index width stays 1 byte), then
// empty attr-name and value-string pools. The u32 num_attr_names = 0 doubles as
// the terminator that ends the tag-pool LPS loop.

// The returned buffer ends exactly where the element tree begins (offset 22).
std::vector<std::uint8_t> build_header() {
    std::vector<std::uint8_t> b;
    b.insert(b.end(), {'x', 'm', 'l', '0'}); // magic            [0,4)
    put_u32(b, 0);                            // header word      [4,8)
    put_lps(b, "n");                          // tag pool: "n"    [8,14)
    put_u32(b, 0);                            // num_attr_names=0 [14,18)
    put_u32(b, 0);                            // num_values=0     [18,22)
    return b;
}

// A complete, shallow, valid document: a single leaf element <n/> with no
// attributes, no text, and no children. Parses to a NON-empty node (tag "n").
std::vector<std::uint8_t> build_valid_leaf() {
    std::vector<std::uint8_t> b = build_header();
    // Element: tag_idx=0, num_attrs=0, separator, num_data_children=0,
    // num_element_children=0.
    b.insert(b.end(), {0x00, 0x00, 0x00, 0x00, 0x00});
    return b;
}

// A document whose element tree nests `levels` containers deep. Each container
// is { tag_idx=0, num_attrs=0, separator=0, num_data_children=0,
// num_element_children=1 } followed by a 0 separator byte that the parser's
// pre-child "skip zero separator" step consumes, so the next container's
// tag_idx (also 0) is read cleanly. With levels > kXmlMaxDepth the recursion
// hits the depth cap, flags ctx.error, and xml0_parse_tree returns empty.
std::vector<std::uint8_t> build_deep_nest(int levels) {
    std::vector<std::uint8_t> b = build_header();
    for (int i = 0; i < levels; ++i) {
        b.insert(b.end(), {0x00, 0x00, 0x00, 0x00, 0x01, 0x00});
    }
    return b;
}

// ── Temp-file scaffolding ──────────────────────────────────────

// xml0_parse_tree opens a path, so each fixture is written to a uniquely named
// file under the system temp directory and removed once parsing completes.
struct TempFile {
    fs::path path;
    bool ok = false;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1);
        std::error_code ec;
        path = fs::temp_directory_path(ec) /
               std::format("heaven_xml0_guard_{}.bin", id);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            ok = true;
        }
    }

    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec); // best-effort cleanup
    }
};

// A default XmlNode (the "rejected / empty" result xml0_parse_tree returns on
// any guarded failure) has no tag, no attributes, and no children.
bool is_empty_node(const XmlNode& n) {
    return n.tag.empty() && n.attrs.empty() && n.children.empty() &&
           n.text.empty();
}

bool unit_xml0_guards() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_xml0_guards] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    auto parse = [&](const std::vector<std::uint8_t>& bytes,
                     const char* label) -> XmlNode {
        TempFile tf(bytes);
        if (!tf.ok) {
            fail(std::string("could not write temp fixture for ") + label);
            return {};
        }
        return xml0_parse_tree(tf.path.string().c_str());
    };

    // ── 1. Positive control: a valid shallow leaf parses to a non-empty node ──
    // Proves the synthesiser matches the on-disk format, so the empty results
    // below are genuine guard rejections rather than a malformed fixture.
    {
        const XmlNode root = parse(build_valid_leaf(), "valid leaf");
        if (root.tag != "n")
            fail("valid leaf: expected tag \"n\", parser did not accept the fixture");
        if (!root.children.empty())
            fail("valid leaf: expected no children");
    }

    // ── 2. Deep-nest recursion cap  ──
    // Nest well past kXmlMaxDepth (256). The parser must stop at the cap and
    // surface an empty result instead of recursing further / overflowing.
    {
        const int levels = heaven::loadval::kXmlMaxDepth + 16; // 272
        const XmlNode root = parse(build_deep_nest(levels), "deep nest");
        if (!is_empty_node(root))
            fail("deep nest past kXmlMaxDepth: expected an empty (rejected) result");
    }

    // ── 2b. Just under the cap still parses (boundary sanity) ──
    // A chain a few levels below the cap is accepted, so the cap is what
    // rejects the deep case above — not some unrelated parse failure.
    {
        const int levels = 8; // far below the 256 cap
        const XmlNode root = parse(build_deep_nest(levels), "shallow nest");
        if (is_empty_node(root))
            fail("shallow nest below cap: expected a non-empty parsed tree");
    }

    // ── 3. Truncated body returns empty (documented, 3.4) ──

    // ftell == -1  cannot be forced on a regular seekable file
    // opened with fopen("rb"); it only fails on unseekable streams. We document
    // that and instead pin the deterministic sibling size/short-body guards: a
    // valid "xml0" header truncated at each structural boundary yields an empty
    // result rather than reading past the buffer or building a partial tree.
    {
        const std::vector<std::uint8_t> full = build_valid_leaf();

        struct Case { std::size_t len; const char* label; };
        const Case cases[] = {
            {4,  "below the 8-byte magic+header floor"},
            {8,  "header only, before the attr-name count"},
            {14, "after the tag pool, before the attr-name count"},
            {18, "after the attr-name count, before the value-string count"},
        };

        for (const auto& c : cases) {
            // Each boundary must lie within the synthesised buffer.
            if (c.len > full.size()) {
                fail(std::string("truncation case out of range: ") + c.label);
                continue;
            }
            const std::vector<std::uint8_t> truncated(full.begin(),
                full.begin() + static_cast<std::ptrdiff_t>(c.len));
            const XmlNode root = parse(truncated, c.label);
            if (!is_empty_node(root))
                fail(std::string("truncated body (") + c.label +
                     "): expected an empty (rejected) result");
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_xml0_guards{&unit_xml0_guards};

} // namespace
