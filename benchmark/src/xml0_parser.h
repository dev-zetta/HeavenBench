/*
 * Unigine xml0 binary format — proper recursive tree parser
 *
 * Format layout:
 *   "xml0" magic (4 bytes) + uint32 header (4 bytes)
 *   Tag name strings (LPS sequence, terminated when LPS read fails)
 *   uint32 num_attr_names + attr name strings (LPS)
 *   uint32 num_value_strings + value strings (LPS)
 *   Tree section: recursive element encoding
 *
 * Element encoding:
 *   tag_idx(W_tag), num_attrs(u8), separator(u8),
 *   [attr_name_idx(W_attr), attr_val_idx(W_val)]* num_attrs,
 *   num_data_children(u8),
 *   if ndc > 0: [LPS(text)]* ndc   (element has text, no element children)
 *   if ndc == 0: num_element_children(u8), [0, child_element, 0]* nec
 *
 * Index widths (W_tag, W_attr, W_val):
 *   If pool_size > 255: indices are uint16 little-endian (2 bytes)
 *   If pool_size <= 255: indices are uint8 (1 byte)
 *
 * LPS (length-prefixed string):
 *   uint32 length (includes null terminator), char[length] data
 *   Header strings are limited to 65536 bytes with strict ASCII validation.
 *   Data strings (element text content) can be much larger (up to 10MB).
 */
#ifndef HEAVENBENCH_SRC_XML0_PARSER_H_
#define HEAVENBENCH_SRC_XML0_PARSER_H_

#include "engine/asset_file_system.h"
#include "engine/number_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <filesystem>
#include <iterator>

#include "engine/loader_validation.h"

// ── XML node structure ──────────────────────────────────────────

struct XmlNode {
    std::string tag;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<XmlNode> children;

    std::string get(const std::string& attr, const std::string& def = "") const {
        auto it = attrs.find(attr);
        return it != attrs.end() ? it->second : def;
    }

    void find_all(const std::string& tag_name, std::vector<const XmlNode*>& out) const {
        if (tag == tag_name) out.push_back(this);
        for (const auto& c : children) c.find_all(tag_name, out);
    }
};

// ── Internal parsing state ──────────────────────────────────────

namespace xml0_detail {

struct ParseContext {
    const uint8_t* data;
    size_t         size;
    size_t         pos;

    std::vector<std::string> tags;
    std::vector<std::string> attr_names;
    std::vector<std::string> values;

    int tag_width;
    int attr_width;
    int val_width;

    // Set true when parse_element exceeds the recursion-depth cap or an
    // unrecoverable parse failure occurs, so xml0_parse_tree surfaces an
    // empty result rather than acting on a half-parsed tree.
    bool error = false;
};

static bool read_lps(const uint8_t* data, size_t len, size_t& off,
                     std::string& out, bool strict) {
    if (off + 4 > len) return false;
    uint32_t slen;
    memcpy(&slen, data + off, 4);

    size_t max_len = strict ? 65536u : 10000000u;
    uint32_t min_len = strict ? 2u : 1u;

    if (slen < min_len || slen > max_len || off + 4 + slen > len) return false;
    if (data[off + 4 + slen - 1] != 0) return false;

    if (strict) {
        for (uint32_t i = 0; i < slen - 1; i++) {
            if (data[off + 4 + i] < 32 || data[off + 4 + i] >= 127) return false;
        }
    }

    out.assign(reinterpret_cast<const char*>(data + off + 4), slen - 1);
    off += 4 + slen;
    return true;
}

static uint8_t read_byte(ParseContext& ctx) {
    if (ctx.pos >= ctx.size) return 0;
    return ctx.data[ctx.pos++];
}

static uint16_t read_index(ParseContext& ctx, int width) {
    if (width == 2) {
        if (ctx.pos + 2 > ctx.size) return 0;
        uint16_t v;
        memcpy(&v, ctx.data + ctx.pos, 2);
        ctx.pos += 2;
        return v;
    }
    return read_byte(ctx);
}

static uint8_t peek_byte(const ParseContext& ctx) {
    if (ctx.pos >= ctx.size) return 0xFF;
    return ctx.data[ctx.pos];
}

static std::string read_data_string(ParseContext& ctx) {
    std::string s;
    if (read_lps(ctx.data, ctx.size, ctx.pos, s, false))
        return s;
    return {};
}

static XmlNode parse_element(ParseContext& ctx, int depth) {
    XmlNode node;

    // Recursion-depth cap: a corrupt or maliciously
    // deep file must not overflow the stack. At the cap we stop descending,
    // flag the parse as failed, and unwind without recursing further. Valid
    // scene files nest far below kXmlMaxDepth, so parity is preserved.
    if (depth >= heaven::loadval::kXmlMaxDepth) {
        ctx.error = true;
        return node;
    }

    uint16_t tag_idx = read_index(ctx, ctx.tag_width);
    if (tag_idx < ctx.tags.size())
        node.tag = ctx.tags[tag_idx];

    uint8_t na = read_byte(ctx);
    read_byte(ctx); // separator

    for (int i = 0; std::cmp_less(i , na); i++) {
        uint16_t an = read_index(ctx, ctx.attr_width);
        uint16_t av = read_index(ctx, ctx.val_width);
        std::string aname = (an < ctx.attr_names.size()) ? ctx.attr_names[an] : "";
        std::string aval  = (av < ctx.values.size())     ? ctx.values[av]     : "";
        if (!aname.empty())
            node.attrs[aname] = aval;
    }

    uint8_t ndc = read_byte(ctx);
    for (int i = 0; std::cmp_less(i , ndc); i++) {
        std::string s = read_data_string(ctx);
        if (!s.empty()) node.text = std::move(s);
    }

    if (ndc == 0) {
        uint8_t nec = read_byte(ctx);
        node.children.reserve(nec);
        for (int i = 0; std::cmp_less(i , nec); i++) {
            if (peek_byte(ctx) == 0) ctx.pos++;
            node.children.push_back(parse_element(ctx, depth + 1));
            // Propagate a depth-cap failure straight up the call chain so we
            // stop touching the buffer once the tree is rejected.
            if (ctx.error) return node;
            if (peek_byte(ctx) == 0) ctx.pos++;
        }
    }

    return node;
}

} // namespace xml0_detail

// ── Public API: parse xml0 binary file into tree ────────────────

static XmlNode xml0_parse_tree(
        const char* filepath,
        heaven::AssetFileSystemRef assets = std::nullopt) {
    auto loaded = heaven::read_asset_file(filepath, assets);
    if (!loaded) return {};
    auto data = std::move(*loaded);
    const size_t sz = data.size();

    if (sz < 8 || memcmp(data.data(), "xml0", 4) != 0) return {};

    using namespace xml0_detail;
    ParseContext ctx;
    ctx.data = data.data();
    ctx.size = sz;
    ctx.pos  = 8;

    std::string s;
    while (read_lps(ctx.data, ctx.size, ctx.pos, s, true))
        ctx.tags.push_back(s);

    if (ctx.pos + 4 > ctx.size) return {};
    uint32_t na;
    memcpy(&na, ctx.data + ctx.pos, 4);
    ctx.pos += 4;
    for (uint32_t i = 0; i < na; i++) {
        if (!read_lps(ctx.data, ctx.size, ctx.pos, s, true)) break;
        ctx.attr_names.push_back(s);
    }

    if (ctx.pos + 4 > ctx.size) return {};
    uint32_t nv;
    memcpy(&nv, ctx.data + ctx.pos, 4);
    ctx.pos += 4;
    for (uint32_t i = 0; i < nv; i++) {
        if (!read_lps(ctx.data, ctx.size, ctx.pos, s, true)) break;
        ctx.values.push_back(s);
    }

    ctx.tag_width  = (ctx.tags.size()       > 255) ? 2 : 1;
    ctx.attr_width = (ctx.attr_names.size() > 255) ? 2 : 1;
    ctx.val_width  = (ctx.values.size()     > 255) ? 2 : 1;

    // Parse the tree from depth 0. A depth-cap breach (or other unrecoverable
    // failure) sets ctx.error; surface that as an empty result rather than
    // returning a partially-built tree.
    XmlNode root = parse_element(ctx, 0);
    if (ctx.error) return {};
    return root;
}

// ── Extract surface → material pairs from.node/.world files ────

static std::map<std::string, std::string> xml0_extract_surface_materials(
        const char* filepath,
        heaven::AssetFileSystemRef assets = std::nullopt) {
    XmlNode root = xml0_parse_tree(filepath, assets);
    std::map<std::string, std::string> pairs;
    std::vector<const XmlNode*> surfaces;
    root.find_all("surface", surfaces);
    for (const auto* s : surfaces) {
        std::string name = s->get("name");
        std::string mat  = s->get("material");
        if (!name.empty() && !mat.empty())
            pairs[name] = mat;
    }
    return pairs;
}

// ── Mesh-scoped surface → material extractor ─────────────────────

// Like xml0_extract_surface_materials but pairs each surface with
// the mesh path it belongs to. This matters because surface names
// in the original Heaven assets are *not* unique across meshes —
// many sub-meshes share placeholder names like "Object04",
// "Box05", "surface_0" etc. A flat global map collapses them to
// whichever node loaded first (so e.g. weathercock's "Object04"
// would inherit airship_cannons' rigging material). Returning
// `(mesh_path, surface_name) → material` lets the resolver pick
// the correct material per mesh.

// In the xml0 binary, mesh objects live under generic `node`
// elements whose `type` attribute is `ObjectMesh` or
// `ObjectMeshSkinned`. Each such node has a `<mesh>` child whose
// text is the asset path, plus zero or more `<surface>` children
// (with `name` and `material` attributes) that override the
// material for a specific sub-surface of that mesh.
static void xml0_extract_mesh_surface_materials(
    const char* filepath,
    std::map<std::pair<std::string, std::string>, std::string>& mesh_surface_to_material,
    std::map<std::string, std::string>& global_surface_to_material,
    heaven::AssetFileSystemRef assets = std::nullopt)
{
    XmlNode root = xml0_parse_tree(filepath, assets);

    auto is_mesh_node = [](const XmlNode& n) {
        if (n.tag != "node") return false;
        std::string t = n.get("type");
        // ObjectMeshDynamic (cloth bodies, e.g. flags) also carries
        // <surface name=... material=...> children that we want to
        // pick up — without this the flag surface ends up with no
        // resolved material and renders untextured.
        return t == "ObjectMesh" ||
               t == "ObjectMeshSkinned" ||
               t == "ObjectMeshDynamic";
    };

    // Recursive walker — when we hit a mesh node, pair its surfaces
    // with the mesh path. Continue descending into non-mesh nodes
    // (e.g. `nodes`, `NodeReference`, layer wrappers) so deeply
    // nested mesh objects are still picked up.
    std::function<void(const XmlNode&)> walk = [&](const XmlNode& node) {
        if (is_mesh_node(node)) {
            std::string mesh_path;
            for (const auto& c : node.children) {
                if (c.tag == "mesh" && !c.text.empty()) {
                    mesh_path = c.text;
                    break;
                }
            }
            for (const auto& c : node.children) {
                if (c.tag != "surface") continue;
                std::string sname = c.get("name");
                std::string mname = c.get("material");
                if (sname.empty() || mname.empty()) continue;
                if (!mesh_path.empty()) {
                    mesh_surface_to_material[{mesh_path, sname}] = mname;
                }
                if (!global_surface_to_material.contains(sname)) {
                    global_surface_to_material[sname] = mname;
                }
            }
            // Don't descend further — surface children of a mesh
            // are direct children (not nested inside other mesh
            // objects).
            return;
        }
        for (const auto& c : node.children) walk(c);
    };
    walk(root);
}

// HeavenBench deliberately keeps the authored near/high-detail geometry active
// at every distance instead of switching mesh-surface LODs at runtime. Cull
// surfaces which are explicitly disabled or whose authored visibility interval
// starts away from the camera; otherwise near and far surfaces overlap and the
// depth winner changes with view direction.

// `folded` is editor/UI state, not render visibility. Some genuine LOD0
// tessellation surfaces carry folded="1", so it must not participate here.
static bool xml0_surface_inactive_in_high_detail_mode(const XmlNode& surface) {
    if (surface.get("enabled") == "0") return true;

    const std::string min_distance = surface.get("min_visible_distance");
    if (min_distance.empty()) return false;
    const auto value = heaven::parse_number_strict<float>(min_distance);
    return value && *value > 0.0f;
}

static void xml0_extract_mesh_inactive_surfaces(
    const char* filepath,
    std::set<std::pair<std::string, std::string>>& inactive_surfaces,
    heaven::AssetFileSystemRef assets = std::nullopt)
{
    XmlNode root = xml0_parse_tree(filepath, assets);

    std::function<void(const XmlNode&)> walk = [&](const XmlNode& node) {
        if (node.tag == "node") {
            const std::string type = node.get("type");
            if (type == "ObjectMesh" || type == "ObjectMeshSkinned" ||
                type == "ObjectMeshDynamic") {
                std::string mesh_path;
                for (const auto& child : node.children) {
                    if (child.tag == "mesh" && !child.text.empty()) {
                        mesh_path = child.text;
                        break;
                    }
                }
                if (!mesh_path.empty()) {
                    for (const auto& child : node.children) {
                        if (child.tag != "surface" ||
                            !xml0_surface_inactive_in_high_detail_mode(child)) {
                            continue;
                        }
                        const std::string surface_name = child.get("name");
                        if (!surface_name.empty()) {
                            inactive_surfaces.emplace(mesh_path, surface_name);
                        }
                    }
                }
                return;
            }
        }
        for (const auto& child : node.children) walk(child);
    };
    walk(root);
}

// ── Filesystem helper: recursive directory scan ─────────────────

static void scan_dir_recursive(const std::string& dir, const std::string& ext,
                                std::vector<std::string>& out,
                                heaven::AssetFileSystemRef assets = std::nullopt) {
    if (assets) {
        auto found = assets->get().find(dir, ext);
        out.insert(out.end(),
                   std::make_move_iterator(found.begin()),
                   std::make_move_iterator(found.end()));
        return;
    }
    namespace fs = std::filesystem;

    // Recurse with the standard library iterator. Default options do NOT
    // follow directory symlinks, so a symlink cycle in the tree cannot trap
    // the scan (the prior stat()-based walk followed symlinks and could).
    // skip_permission_denied keeps an unreadable subdirectory from aborting
    // the whole scan, mirroring the old "stat fails -> skip entry" behaviour.
    std::error_code ec;
    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        // Match files (symlinks to regular files included, as stat did) whose
        // name ends with the requested extension, exactly as before.
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.size() > ext.size() &&
            name.substr(name.size() - ext.size()) == ext)
            out.push_back(it->path().string());
    }
}

#endif // HEAVENBENCH_SRC_XML0_PARSER_H_
