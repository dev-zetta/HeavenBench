/*
 * HeavenBench — RHI-aware material resolver implementation
 *
 * Reuses the same.mat parsing logic as the GL-only MaterialDatabase,
 * but loads textures through the abstract IRhiBackend interface.
 */
#include "engine/material_rhi.h"
#include "engine/glass_material_ref.h"
#include "engine/asset_usage_recorder.h"
#include "engine/number_parse.h"
#include "xml0_parser.h"

#include <cstdio>
#include <cstring>
#include <array>
#include <filesystem>
#include <optional>
#include <set>

namespace heaven {

TextureSemantic texture_semantic_for_binding(
    std::string_view suffix,
    std::string_view authored_format,
    bool auto_discovered) {
    if (authored_format == "srgb") {
        return TextureSemantic::ColorSRGB;
    }
    if (suffix == "_n" || suffix == "_dn") {
        return TextureSemantic::NormalLinear;
    }
    if (authored_format == "signed") {
        return TextureSemantic::ScalarLinear;
    }
    // Authored base/detail colors are composed in their encoded domain before
    // the original mesh shader's gamma-2 conversion. Hardware sRGB decoding
    // before that composition would move the overlay's neutral 0.5 value.
    if (suffix == "_d" || suffix == "_dd") {
        return auto_discovered ? TextureSemantic::ColorSRGB
                               : TextureSemantic::ColorLegacyGamma2;
    }
    if (auto_discovered && suffix == "_e") {
        return TextureSemantic::ColorSRGB;
    }
    return TextureSemantic::ScalarLinear;
}

void RhiMaterialDatabase::clear_resolved_cache() noexcept {
    cache_.clear();
    cache_surface_.clear();
    tex_hit_ = 0;
    tex_miss_ = 0;
    tex_misses_.clear();
}

namespace {

// ── Texture slot mapping (shared with material.cpp) ─────────────

std::string tex_slot_to_suffix(const std::string& tex_name) {
    if (tex_name == "diffuse")         return "_d";
    if (tex_name == "detail_diffuse")  return "_dd";
    if (tex_name == "normal")          return "_n";
    if (tex_name == "detail_normal")   return "_dn";
    if (tex_name == "specular")        return "_s";
    if (tex_name == "detail_specular") return "_ds";
    if (tex_name == "height" || tex_name == "parallax") return "_h";
    // Unigine differentiates `parallax` (POM-only normal-map relief)
    // from `tessellation` (real geometry displacement). Heaven's
    // `*_tesselation` materials use a `<texture name="tessellation">`
    // child whose path ends with `_t.dds` — engine_t.dds, gears_t.dds
    // and so on. Without this mapping the slot was silently dropped
    // and the CPU tessellation fell back to the inherited 16×16
    // mesh_base_parallax placeholder, leaving every gear/turbine
    // looking smooth and tooth-less.
    if (tex_name == "tessellation")    return "_t";
    if (tex_name == "emission")        return "_e";
    return "";
}

constexpr std::string_view texture_role_for_suffix(
        std::string_view suffix) noexcept {
    if (suffix == "_d" || suffix == "_dd") return "diffuse";
    if (suffix == "_n" || suffix == "_dn") return "normal";
    if (suffix == "_s" || suffix == "_ds") return "specular";
    if (suffix == "_h") return "height";
    if (suffix == "_t") return "tessellation";
    if (suffix == "_e") return "emissive";
    return "data";
}

// Normalize an on-disk asset path to a Relative_Asset_Path against the
// Source_Tree root (the canonical parent of Data_Root), mirroring how
// AssetUsageRecorder normalizes its used set. Used only on the packer tap path
// to render unresolved texture-slot targets and their referring.mat with the
// same "heaven/…" / "core/…" namespace as the used set. Never called when the
// recorder is null, so it adds no cost to a render run.
std::string to_source_rel(const std::string& on_disk, const PathResolver& paths) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(on_disk), ec);
    if (ec) canon = fs::path(on_disk).lexically_normal();
    fs::path src_root = fs::path(paths.data_root()).parent_path();
    fs::path src_canon = fs::weakly_canonical(src_root, ec);
    if (ec) src_canon = src_root.lexically_normal();
    fs::path rel = canon.lexically_relative(src_canon);
    return rel.empty() ? canon.generic_string() : rel.generic_string();
}

[[nodiscard]] std::optional<std::array<float, 4>>
parse_vec4_literal(std::string_view text) {
    const std::size_t open = text.find('(');
    if (open != std::string_view::npos) {
        text.remove_prefix(open + 1);
    }

    std::array<float, 4> values{};
    for (float& value : values) {
        const auto parsed = parse_number_prefix_with_consumed<float>(text);
        if (!parsed) {
            return std::nullopt;
        }
        value = parsed->value;
        text.remove_prefix(parsed->consumed);
        while (!text.empty() &&
               (text.front() == 'f' || text.front() == 'F' ||
                text.front() == ' ' || text.front() == ',')) {
            text.remove_prefix(1);
        }
    }
    return values;
}

// Only the two authored toggle values are meaningful. Preserve unknown on a
// missing/malformed state so ordinary material inheritance still applies.
int emission_toggle(std::string_view text) {
    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == std::string_view::npos) return -1;
    text = text.substr(0, end + 1);
    const auto parsed = parse_number_strict<int>(text);
    return parsed && (*parsed == 0 || *parsed == 1) ? *parsed : -1;
}

// ── xml0.mat parser (same as material.cpp) ─────────────────────

std::map<std::string, MaterialDef> xml0_parse_materials_rhi(
        const char* filepath, AssetFileSystemRef assets) {
    XmlNode root = xml0_parse_tree(filepath, assets);
    std::map<std::string, MaterialDef> materials;
    std::vector<const XmlNode*> mat_nodes;
    root.find_all("material", mat_nodes);

    for (const auto* m : mat_nodes) {
        MaterialDef def;
        def.name = m->get("name"); def.parent = m->get("parent");
        if (def.name.empty()) continue;
        for (const auto& child : m->children) {
            if (child.tag == "options") {
                if (child.get("alpha_test") == "1") def.alpha_test = 1;
                if (child.get("two_sided") == "1")  def.two_sided = 1;
                if (child.get("depth_mask") == "0") def.hidden = 1;
            }
            if (child.tag == "state" && child.get("name") == "detail" && !child.text.empty())
                def.detail_mode =
                    parse_number_prefix<int>(child.text).value_or(0);
            if (child.tag == "state" && child.get("name") == "emission") {
                def.emission_enabled = emission_toggle(child.text);
            }
            if (child.tag == "parameter" && child.get("name") == "detail_transform" && !child.text.empty()) {
                // detail_transform is the per-material UV transform
                // for the *detail* overlay map. See parse_vec4 for
                // why we don't use sscanf.
                if (const auto values = parse_vec4_literal(child.text)) {
                    std::ranges::copy(*values, def.detail_transform);
                }
            }
            if (child.tag == "parameter" && child.get("name") == "base_transform" && !child.text.empty()) {
                // base_transform scales/offsets UV0 before sampling
                // diffuse / normal / specular / height. Authored on
                // many Heaven materials (ruins_merlon: vec4(2,1,-1,0),
                // airship_rigging: vec4(2,1,0,0), fort_stucko:
                // vec4(0.6,0.6,0,0)). Without this, surfaces using
                // those materials sample the wrong region of the
                // texture and look "stretched" or "wrong colour".
                if (const auto values = parse_vec4_literal(child.text)) {
                    std::ranges::copy(*values, def.base_transform);
                }
            }
            // Per-material tessellation parameters (see types.h MaterialDef).
            if (child.tag == "parameter" && !child.text.empty()) {
                const auto& pn = child.get("name");
                if (pn == "tessellation_length") {
                    def.tess_length =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_length_set = true;
                } else if (pn == "tessellation_power") {
                    def.tess_power =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_power_set = true;
                } else if (pn == "tessellation_scale") {
                    def.tess_scale_param =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_scale_set = true;
                } else if (pn == "tessellation_factor") {
                    def.tess_factor =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_factor_set = true;
                } else if (pn == "tessellation_distance") {
                    def.tess_distance_far =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_distance_set = true;
                } else if (pn == "tessellation_threshold") {
                    def.tess_threshold =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_threshold_set = true;
                } else if (pn == "tessellation_attenuation") {
                    def.tess_attenuation =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                    def.tess_attenuation_set = true;
                } else if (pn == "detail_diffuse")
                    def.detail_diffuse =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                else if (pn == "detail_normal")
                    def.detail_normal =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                else if (pn == "detail_specular")
                    def.detail_specular =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                else if (pn == "emission_scale")
                    def.emission_scale =
                        parse_number_prefix<float>(child.text).value_or(0.0f);
                else if (pn == "emission_color") {
                    float r = 1, g = 1, b = 1, a = 1;
                    if (std::sscanf(child.text.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3) {
                        def.emission_color[0] = r;
                        def.emission_color[1] = g;
                        def.emission_color[2] = b;
                    }
                }
            }
            if (child.tag == "texture" && !child.text.empty()) {
                if (child.text.size() > 4 && child.text.substr(child.text.size()-4) == ".dds") {
                    auto suffix = tex_slot_to_suffix(child.get("name"));
                    if (!suffix.empty() && !def.textures.contains(suffix)) {
                        def.textures[suffix] = child.text;
                        const std::string format = child.get("format");
                        if (!format.empty()) def.texture_formats[suffix] = format;
                        const std::string wrap = child.get("wrap");
                        if (!wrap.empty()) def.texture_wraps[suffix] = wrap;
                    }
                }
            }
        }
        materials[def.name] = def;
    }
    // Per-file inheritance pre-pass: merge missing texture slots from each
    // material's local-file parent. The global pass in build() walks the
    // full cross-file parent chain afterwards.
    for (auto& [n,d] : materials) {
        std::string pn = d.parent; std::set<std::string> vis; vis.insert(d.name);
        while (!pn.empty() && !vis.contains(pn)) {
            vis.insert(pn);
            auto pi = materials.find(pn);
            if (pi == materials.end()) break;
            for (const auto& [suffix, path] : pi->second.textures) {
                if (!d.textures.contains(suffix)) {
                    d.textures[suffix] = path;
                    if (const auto fi = pi->second.texture_formats.find(suffix);
                        fi != pi->second.texture_formats.end()) {
                        d.texture_formats[suffix] = fi->second;
                    }
                    if (const auto wi = pi->second.texture_wraps.find(suffix);
                        wi != pi->second.texture_wraps.end()) {
                        d.texture_wraps[suffix] = wi->second;
                    }
                }
            }
            pn = pi->second.parent;
        }
    }
    return materials;
}

// ── Plain XML.mat parser (same as material.cpp) ────────────────

std::map<std::string, MaterialDef> xml_parse_materials_rhi(
        const char* filepath, AssetFileSystemRef assets) {
    std::map<std::string, MaterialDef> materials;
    auto loaded = read_asset_text(filepath, assets);
    if (!loaded) return materials;
    std::string content = std::move(*loaded);

    std::string current_mat; MaterialDef current_def;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t ts = content.find('<', pos); if (ts == std::string::npos) break;
        size_t te = content.find('>', ts);  if (te == std::string::npos) break;
        std::string tag = content.substr(ts+1, te-ts-1); pos = te+1;
        if (tag[0]=='?'||tag[0]=='!') continue;
        if (tag[0]=='/') { if (tag.find("material")!=std::string::npos && !current_mat.empty()) { materials[current_mat]=current_def; current_mat.clear(); } continue; }
        bool sc = (!tag.empty() && tag.back()=='/'); if (sc) tag=tag.substr(0,tag.size()-1);
        size_t sp = tag.find(' ');
        std::string tn = (sp!=std::string::npos)?tag.substr(0,sp):tag;
        std::string as = (sp!=std::string::npos)?tag.substr(sp+1):"";
        auto ga = [&](const std::string& n)->std::string {
            std::string nd=n+"=\""; auto p=as.find(nd); if(p==std::string::npos){nd=n+"='";p=as.find(nd);} if(p==std::string::npos)return"";
            size_t vs=p+nd.size(); char q=as[vs-1]; size_t ve=as.find(q,vs); return(ve==std::string::npos)?"":as.substr(vs,ve-vs);
        };
        if (tn=="material") { current_mat=ga("name"); current_def=MaterialDef{}; current_def.name=current_mat; current_def.parent=ga("parent"); }
        if (current_mat.empty()) continue;
        if (tn=="options") { if(ga("alpha_test")=="1")current_def.alpha_test=1; if(ga("two_sided")=="1")current_def.two_sided=1; if(ga("depth_mask")=="0")current_def.hidden=1; }
        if (tn=="state") { auto sn=ga("name"); if(sn=="detail") { size_t end=content.find('<',pos); if(end!=std::string::npos) { auto val=content.substr(pos,end-pos); while(!val.empty()&&(val.front()==' '||val.front()=='\n'))val.erase(val.begin()); if(!val.empty()) current_def.detail_mode=parse_number_prefix<int>(val).value_or(0); } } }
        if (tn == "state" && ga("name") == "emission" && !sc) {
            const auto end = content.find('<', pos);
            if (end != std::string::npos) {
                current_def.emission_enabled = emission_toggle(
                    std::string_view(content).substr(pos, end - pos));
            }
        }
        if (tn=="parameter") { auto pn=ga("name"); if(pn=="detail_transform") { size_t end=content.find('<',pos); if(end!=std::string::npos) { auto val=content.substr(pos,end-pos); if (const auto values = parse_vec4_literal(val)) { std::ranges::copy(*values, current_def.detail_transform); } } }
            // base_transform parsing — same shape as detail_transform
            // (vec4(...) literal in element text). See material_rhi.h's
            // base_transform comment for why it matters.
            if (pn == "base_transform") {
                size_t end = content.find('<', pos);
                if (end != std::string::npos) {
                    auto val = content.substr(pos, end - pos);
                    if (const auto values = parse_vec4_literal(val)) {
                        std::ranges::copy(*values, current_def.base_transform);
                    }
                }
            }
            // Per-material tessellation parameters from core.mat (plain XML).
            if (pn == "tessellation_length" || pn == "tessellation_power" ||
                pn == "tessellation_scale"  || pn == "tessellation_factor" ||
                pn == "tessellation_distance" || pn == "tessellation_threshold" ||
                pn == "tessellation_attenuation" ||
                pn == "detail_diffuse"      || pn == "detail_normal" ||
                pn == "detail_specular") {
                size_t end = content.find('<', pos);
                if (end != std::string::npos) {
                    auto val = content.substr(pos, end - pos);
                    while (!val.empty() && (val.front()==' '||val.front()=='\n'||val.front()=='\t'||val.front()=='\r'))
                        val.erase(val.begin());
                    if (!val.empty()) {
                        const float parsed_value =
                            parse_number_prefix<float>(val).value_or(0.0f);
                        if (pn == "tessellation_length") {
                            current_def.tess_length = parsed_value; current_def.tess_length_set = true;
                        } else if (pn == "tessellation_power") {
                            current_def.tess_power = parsed_value; current_def.tess_power_set = true;
                        } else if (pn == "tessellation_scale") {
                            current_def.tess_scale_param = parsed_value; current_def.tess_scale_set = true;
                        } else if (pn == "tessellation_factor") {
                            current_def.tess_factor = parsed_value; current_def.tess_factor_set = true;
                        } else if (pn == "tessellation_distance") {
                            current_def.tess_distance_far = parsed_value; current_def.tess_distance_set = true;
                        } else if (pn == "tessellation_threshold") {
                            current_def.tess_threshold = parsed_value; current_def.tess_threshold_set = true;
                        } else if (pn == "tessellation_attenuation") {
                            current_def.tess_attenuation = parsed_value; current_def.tess_attenuation_set = true;
                        }
                        else if (pn == "detail_diffuse")        current_def.detail_diffuse   = parsed_value;
                        else if (pn == "detail_normal")         current_def.detail_normal    = parsed_value;
                        else if (pn == "detail_specular")       current_def.detail_specular  = parsed_value;
                    }
                }
            }
        }
        if (tn=="texture"&&!sc) { auto tname=ga("name"); auto format=ga("format"); auto wrap=ga("wrap"); size_t end=content.find('<',pos); if(end!=std::string::npos){auto p=content.substr(pos,end-pos); if(p.size()>4&&p.substr(p.size()-4)==".dds"){auto s=tex_slot_to_suffix(tname); if(!s.empty()&&!current_def.textures.contains(s)){current_def.textures[s]=p;if(!format.empty())current_def.texture_formats[s]=format;if(!wrap.empty())current_def.texture_wraps[s]=wrap;}}}}
    }
    return materials;
}

} // namespace

// ── Build ───────────────────────────────────────────────────────

void RhiMaterialDatabase::build(const PathResolver& paths,
                                AssetUsageRecorder* rec,
                                AssetFileSystemRef assets) {
    assets_ = assets;
    const std::string& data_dir = paths.data_root();
    std::string core_dir = data_dir + "/../core";
    std::vector<std::string> core_files;
    scan_dir_recursive(core_dir, ".mat", core_files, assets_);
    for (const auto& mf : core_files) {
        for (auto& [n,d] : xml_parse_materials_rhi(mf.c_str(), assets_))
            if (!all_materials_.contains(n)) {
                all_materials_[n] = d;
                // Remember which.mat defined this material so resolve() can
                // report provenance of the used subset. Only when a recorder is
                // supplied (packer mode) — render runs pay nothing.
                if (rec) material_source_file_[n] = mf;
            }
    }
    std::printf("  Core material definitions: %zu (from %zu .mat files)\n", all_materials_.size(), core_files.size());

    std::vector<std::string> mat_files;
    scan_dir_recursive(data_dir, ".mat", mat_files, assets_);
    for (const auto& mf : mat_files) {
        for (auto& [n,d] : xml0_parse_materials_rhi(mf.c_str(), assets_))
            if (!all_materials_.contains(n)) {
                all_materials_[n] = d;
                if (rec) material_source_file_[n] = mf;
            }
    }

    // Resolve inheritance — per-texture-slot merge through the full parent chain.
    // A child may define some texture slots (e.g. detail_diffuse) and inherit
    // others (e.g. diffuse, normal) from its parent. Walk the parent chain and
    // fill in any slot the child doesn't already have.
    for (auto& [n,d] : all_materials_) {
        std::string pn = d.parent; std::set<std::string> vis; vis.insert(d.name);
        while (!pn.empty() && !vis.contains(pn)) {
            vis.insert(pn);
            auto pi = all_materials_.find(pn);
            if (pi == all_materials_.end()) break;
            const auto& pd = pi->second;
            for (const auto& [suffix, path] : pd.textures) {
                if (!d.textures.contains(suffix)) {
                    d.textures[suffix] = path;
                    if (const auto fi = pd.texture_formats.find(suffix);
                        fi != pd.texture_formats.end()) {
                        d.texture_formats[suffix] = fi->second;
                    }
                    if (const auto wi = pd.texture_wraps.find(suffix);
                        wi != pd.texture_wraps.end()) {
                        d.texture_wraps[suffix] = wi->second;
                    }
                }
            }
            pn = pd.parent;
        }
    }
    for (auto& [n,d] : all_materials_) {
        std::string pn = d.parent; std::set<std::string> vis; vis.insert(d.name);
        while (!pn.empty()&&!vis.contains(pn)) { vis.insert(pn);
            auto pi=all_materials_.find(pn); if(pi==all_materials_.end())break; const auto& pd=pi->second;
            if(d.alpha_test<0&&pd.alpha_test>=0) d.alpha_test=pd.alpha_test;
            if(d.detail_mode<0&&pd.detail_mode>=0) d.detail_mode=pd.detail_mode;
            if (d.emission_enabled < 0 && pd.emission_enabled >= 0)
                d.emission_enabled = pd.emission_enabled;
            if(d.two_sided<0&&pd.two_sided>=0) d.two_sided=pd.two_sided;
            if(d.hidden<0&&pd.hidden>=0) d.hidden=pd.hidden;
            if(d.detail_transform[0]==0&&d.detail_transform[1]==0&&(pd.detail_transform[0]!=0||pd.detail_transform[1]!=0))
                std::memcpy(d.detail_transform,pd.detail_transform,sizeof(d.detail_transform));
            if (d.detail_diffuse < 0.0f && pd.detail_diffuse >= 0.0f)
                d.detail_diffuse = pd.detail_diffuse;
            if (d.detail_normal < 0.0f && pd.detail_normal >= 0.0f)
                d.detail_normal = pd.detail_normal;
            if (d.detail_specular < 0.0f && pd.detail_specular >= 0.0f)
                d.detail_specular = pd.detail_specular;
            // Inherit base_transform when the child still has the
            // default identity. The parent's authored transform
            // (e.g. ruins_merlon_tesselation: vec4(2,1,-1,0)) cascades
            // down to leaf materials like fort_merlon_t which only
            // override textures.
            if (d.base_transform[0] == 1.0f && d.base_transform[1] == 1.0f &&
                d.base_transform[2] == 0.0f && d.base_transform[3] == 0.0f &&
                (pd.base_transform[0] != 1.0f || pd.base_transform[1] != 1.0f ||
                 pd.base_transform[2] != 0.0f || pd.base_transform[3] != 0.0f)) {
                std::memcpy(d.base_transform, pd.base_transform, sizeof(d.base_transform));
            }
            // Inherit tessellation parameters when the child didn't set them.
            if (!d.tess_length_set && pd.tess_length_set) {
                d.tess_length = pd.tess_length; d.tess_length_set = true;
            }
            if (!d.tess_power_set && pd.tess_power_set) {
                d.tess_power = pd.tess_power; d.tess_power_set = true;
            }
            if (!d.tess_scale_set && pd.tess_scale_set) {
                d.tess_scale_param = pd.tess_scale_param; d.tess_scale_set = true;
            }
            if (!d.tess_factor_set && pd.tess_factor_set) {
                d.tess_factor = pd.tess_factor; d.tess_factor_set = true;
            }
            if (!d.tess_distance_set && pd.tess_distance_set) {
                d.tess_distance_far = pd.tess_distance_far; d.tess_distance_set = true;
            }
            if (!d.tess_threshold_set && pd.tess_threshold_set) {
                d.tess_threshold = pd.tess_threshold; d.tess_threshold_set = true;
            }
            if (!d.tess_attenuation_set && pd.tess_attenuation_set) {
                d.tess_attenuation = pd.tess_attenuation;
                d.tess_attenuation_set = true;
            }
            // Inherit emission tint / scale when the child didn't set them.
            if (d.emission_scale < 0.0f && pd.emission_scale >= 0.0f)
                d.emission_scale = pd.emission_scale;
            if (d.emission_color[0] == 1.0f && d.emission_color[1] == 1.0f &&
                d.emission_color[2] == 1.0f &&
                (pd.emission_color[0] != 1.0f || pd.emission_color[1] != 1.0f ||
                 pd.emission_color[2] != 1.0f)) {
                std::memcpy(d.emission_color, pd.emission_color, sizeof(d.emission_color));
            }
            pn=pd.parent;
        }
        d.alpha_test = std::max(d.alpha_test, 0);
        d.detail_mode = std::max(d.detail_mode, 0);
        d.two_sided = std::max(d.two_sided, 0);
        d.hidden = std::max(d.hidden, 0);
        if (d.detail_diffuse < 0.0f) d.detail_diffuse = 0.5f;
        if (d.detail_normal < 0.0f) d.detail_normal = 0.5f;
        if (d.detail_specular < 0.0f) d.detail_specular = 0.5f;
    }
    std::printf("  Material definitions: %zu (from %zu game .mat files)\n", all_materials_.size(), mat_files.size());

    // ── Metallic flag from parent chain ───────────────────────

    // Heaven separates metal vs dielectric materials by giving
    // them distinct base parents in the engine's `unigine_*.mat`
    // libraries. Anything that descends from
    // `mesh_paint_reflection_cube_base` or its tessellation
    // sibling renders with cubemap reflections (the metal
    // pipeline); everything descending from plain `mesh_base`,
    // `mesh_tessellation_base`, etc. is matte dielectric.

    // We can't infer this from the spec map alone — Heaven's
    // plaster (`building_wall_stucko`) ships a bright spec
    // texture for crisp ambient highlights, but the material is
    // still authored against the dielectric pipeline. Walking
    // the parent chain gives us the authoritative answer.
    auto is_metallic_base = [](const std::string& name) {
        return name == "mesh_paint_reflection_cube_base"
            || name == "mesh_tessellation_paint_reflection_cube_base";
    };
    for (auto& [n, d] : all_materials_) {
        std::string pn = d.name;  // start at self so direct match counts
        std::set<std::string> vis;
        while (!pn.empty() && !vis.contains(pn)) {
            vis.insert(pn);
            if (is_metallic_base(pn)) { d.is_metallic = true; break; }
            auto pi = all_materials_.find(pn);
            if (pi == all_materials_.end()) break;
            pn = pi->second.parent;
        }
    }

    // ── Tessellation flag from parent chain ───────────────────

    // Only materials descending from a `*_tessellation*` base run the
    // engine's control/evaluate (TCS/TES) stages and geometrically
    // displace their surface (see core/shaders/.../evaluate_base.shader).
    // Plain `*_base` materials carry a `parallax`/`height` texture purely
    // for parallax-occlusion mapping in the fragment shader and never
    // tessellate. We can only tell these apart by the parent chain — the
    // texture set alone is ambiguous because both pipelines bind a height
    // map. Marking the chain here lets CPU tessellation skip the POM-only
    // surfaces the engine renders flat.
    for (auto& [n, d] : all_materials_) {
        std::string pn = d.name;  // start at self so direct match counts
        std::set<std::string> vis;
        while (!pn.empty() && !vis.contains(pn)) {
            vis.insert(pn);
            if (pn.find("tessellation") != std::string::npos) {
                d.is_tessellation = true;
                break;
            }
            auto pi = all_materials_.find(pn);
            if (pi == all_materials_.end()) break;
            pn = pi->second.parent;
        }
    }

    // Surface→material pairs. Two maps are populated:
    //   * mesh_surface_to_material_: (mesh_path, surface_name) → material
    //                                  Disambiguates placeholder names that
    //                                  recur across multiple meshes (e.g.
    //                                  Object04 in airship_cannons vs
    //                                  weathercock — without this scope they
    //                                  would inherit each other's material).
    //   * surface_to_material_: surface_name → material (fallback when
    //                                  the mesh-scoped lookup misses, e.g.
    //                                  surfaces declared at world-level).
    std::vector<std::string> node_files;
    scan_dir_recursive(data_dir, ".node", node_files, assets_);
    for (const auto& nf : node_files) {
        // Mesh-scoped extractor: also populates global map with names
        // that have a unique mesh binding within this file.
        xml0_extract_mesh_surface_materials(
            nf.c_str(), mesh_surface_to_material_, surface_to_material_,
            assets_);
        xml0_extract_mesh_inactive_surfaces(
            nf.c_str(), inactive_surfaces_, assets_);
        // Plain extractor: gathers any other `<surface>` declarations
        // (e.g. world-level overrides not under an ObjectMesh node).
        for (auto& [s, m] :
             xml0_extract_surface_materials(nf.c_str(), assets_))
            if (!surface_to_material_.contains(s))
                surface_to_material_[s] = m;
    }
    {
        std::string world = paths.world_path();
        xml0_extract_mesh_surface_materials(
            world.c_str(), mesh_surface_to_material_, surface_to_material_,
            assets_);
        xml0_extract_mesh_inactive_surfaces(
            world.c_str(), inactive_surfaces_, assets_);
        for (auto& [s, m] :
             xml0_extract_surface_materials(world.c_str(), assets_))
            if (!surface_to_material_.contains(s))
                surface_to_material_[s] = m;
    }

    std::printf(
                "  Surface->material pairs: %zu (mesh-scoped: %zu, "
                "inactive/far-LOD surfaces: %zu)\n",
                surface_to_material_.size(),
                mesh_surface_to_material_.size(),
                inactive_surfaces_.size());
}

// ── Resolve ─────────────────────────────────────────────────────

bool RhiMaterialDatabase::try_load(GpuMaterial& mat, const MaterialDef& def,
                                    const PathResolver& paths, IRhiBackend& backend,
                                    AssetUsageRecorder* rec,
                                    const std::string& referring_mat,
                                    bool auto_discovered) {
    std::string diffuse_path, specular_path;
    for (const auto& [suffix, tex_path] : def.textures) {
        std::string full = paths.relative_path(tex_path);
        auto exists = [&](const std::string& candidate) {
            return asset_exists(candidate, assets_);
        };
        // Core material defaults author paths such as
        // "core/textures/mesh_base_diffuse.dds". The extracted source tree
        // keeps those below the sibling root "core/core/..."; resolve that
        // namespace explicitly so directory and ZIP mounts share the same
        // logical path and packaged releases do not silently use placeholders.
        if (!exists(full) && std::string_view(tex_path).starts_with("core/")) {
            const std::string core_candidate =
                paths.data_root() + "/../core/" + tex_path;
            if (exists(core_candidate)) full = core_candidate;
        }
        if (!exists(full)) {
            // The slot names a texture that does not exist on disk — record it
            // as an unresolved reference against the.mat that bound it
            // and skip it, exactly as the renderer does.
            if (rec)
                rec->record_unresolved(to_source_rel(full, paths), referring_mat);
            continue;
        }
        const auto format = def.texture_formats.find(suffix);
        const TextureSemantic semantic = texture_semantic_for_binding(
            suffix,
            format != def.texture_formats.end()
                ? std::string_view(format->second)
                : std::string_view{},
            auto_discovered);
        uint64_t h = backend.load_texture(full, semantic);
        // Record every texture file that exists and loads, including the
        // convention-auto-discovered slots.
        if (rec) {
            const auto wrap = def.texture_wraps.find(suffix);
            rec->record_texture(
                full, texture_role_for_suffix(suffix),
                wrap == def.texture_wraps.end()
                    ? std::string_view{"repeat"}
                    : std::string_view{wrap->second});
        }
        if (suffix == "_d") {
            mat.diffuse = h;
            mat.diffuse_semantic = semantic;
            diffuse_path = tex_path;
        }
        if (suffix == "_dd") {
            mat.detail_diffuse = h;
            mat.detail_diffuse_semantic = semantic;
        }
        if (suffix == "_dn") mat.detail_normal = h;
        if (suffix == "_ds") mat.detail_specular = h;
        if (suffix == "_n")  mat.normal = h;
        if (suffix == "_s")  { mat.specular = h; specular_path = tex_path; }
        if (suffix == "_e") {
            mat.emission = h;
            // Keep the bound map/inventory intact. The core engine supplies a
            // neutral placeholder even with its emission state disabled; that
            // placeholder must not synthesize a light on every metal/wall.
            const bool default_map = tex_path == "core/textures/mesh_base_emission.dds";
            mat.emission_light_source = h != 0 &&
                (def.emission_enabled == 1 ||
                 (def.emission_enabled < 0 && !default_map));
        }
        if (suffix == "_h")  mat.height = h;
        if (suffix == "_t")  mat.tessellation = h;
    }
    // The spec slot is "dedicated" when it points to a different
    // file than the diffuse. Many Heaven materials (fort_merlon_t,
    // various building stones) alias the diffuse as the spec, in
    // which case the spec map is just the diffuse luminance and we
    // shouldn't classify metallic from it.
    mat.dedicated_spec = !specular_path.empty()
                      && !diffuse_path.empty()
                      && specular_path != diffuse_path;
    if (def.alpha_test > 0) mat.alpha_ref = 0.5f;
    // Auto-detect foliage materials that need alpha testing
    // (inheritance chain from mesh_leaf_base may not be fully resolved)
    if (def.name.find("leaf") != std::string::npos ||
        def.name.find("leaves") != std::string::npos ||
        def.name.find("branch") != std::string::npos ||
        def.name.find("fir") != std::string::npos) {
        if (mat.alpha_ref <= 0.0f) mat.alpha_ref = 0.5f;
        mat.two_sided = true;
    }
    // Auto-detect thin rope/cable/rigging materials and stop them casting the
    // directional sun shadow. The airship rigging cables and the various ropes
    // (bridge/pier/island/fort) are sub-texel-thin casters; in the sun shadow
    // map at a low grazing sun they produce an aliased, light-leaking streak
    // that swims across nearby walls (confirmed: the streak vanishes at night
    // when the sun shadow is off, and no post-effect toggle removes it). Name-
    // based, mirroring the foliage/glass auto-detection above — no
    // material_map.txt. The surfaces still render and still receive shadows.
    if (def.name.find("rope")    != std::string::npos ||
        def.name.find("rigging") != std::string::npos ||
        def.name.find("cable")   != std::string::npos ||
        def.name.find("wire")    != std::string::npos ||
        def.name.find("chain")   != std::string::npos) {
        mat.casts_shadow = false;
    }
    // Auto-detect glass surfaces (windows, airship glass, lab
    // equipment) the same name-based way as foliage above, but
    // delegating the markers to the pure, host-testable predicate
    // in glass_material_ref.h (— classification
    // driven EXCLUSIVELY from the xml0-derived material name and
    // its parent="..." chain; no material_map.txt). Walk the parent
    // chain from all_materials_ so leaf variants like
    // building_glass_on inherit the marker from building_glass.
    {
        std::vector<std::string> parent_chain;
        std::string pn = def.parent;
        std::set<std::string> vis;
        vis.insert(def.name);
        while (!pn.empty() && !vis.contains(pn)) {
            vis.insert(pn);
            parent_chain.push_back(pn);
            auto pi = all_materials_.find(pn);
            if (pi == all_materials_.end()) break;
            pn = pi->second.parent;
        }
        if (glass_ref::is_glass_material(def.name, parent_chain))
            mat.glass = true;

        // Carry the xml0 material identity + parent chain to the GPU side so
        // the data-driven Material_Model_Selector  can run in
        // MaterialInstanceCache::get_or_create from the same name+parent-chain
        // discipline the glass classifier above uses — no material_map.txt.
        mat.material_name = def.name;
        mat.parent_chain  = std::move(parent_chain);
    }
    mat.detail_mode = def.detail_mode;
    if (def.detail_mode >= 1 && def.detail_mode <= 4 && mat.detail_diffuse)
        mat.has_detail = true;
    if (def.two_sided > 0) mat.two_sided = true;
    if (def.hidden > 0) mat.hidden = true;
    if (def.detail_transform[0]!=0||def.detail_transform[1]!=0)
        std::memcpy(mat.detail_transform, def.detail_transform, sizeof(mat.detail_transform));
    mat.detail_strength[0] = def.detail_diffuse;
    mat.detail_strength[1] = def.detail_normal;
    mat.detail_strength[2] = def.detail_specular;
    mat.geometry_displacement = def.is_tessellation && def.tess_length > 0.0f &&
                                mat.height != 0;
    mat.tess_length = def.tess_length_set ? def.tess_length : 0.0f;
    mat.tess_power = def.tess_power_set ? def.tess_power : 2.0f;
    mat.tess_distance = def.tess_distance_set ? def.tess_distance_far : 10.0f;
    mat.tess_threshold = def.tess_threshold_set ? def.tess_threshold : 0.0f;
    mat.tess_attenuation = def.tess_attenuation_set ? def.tess_attenuation : 2.0f;
    mat.tess_factor = def.tess_factor_set ? def.tess_factor : 1.0f;
    mat.tess_scale = def.tess_scale_set ? def.tess_scale_param : 0.5f;

    // Copy base_transform unconditionally — it defaults to identity
    // {1,1,0,0} in MaterialDef, so blank materials still produce a
    // sensible no-op transform. Materials that authored a different
    // value get it propagated to the GPU side.
    std::memcpy(mat.base_transform, def.base_transform, sizeof(mat.base_transform));

    // Authoritative metallic class from the parent chain.
    mat.is_metallic = def.is_metallic;

    // Per-material emission tint and scale (only meaningful when the
    // material actually has an emission texture). Default scale to 1.0
    // when unset; emission_color defaults to identity.
    std::memcpy(mat.emission_color, def.emission_color, sizeof(mat.emission_color));
    mat.emission_scale = (def.emission_scale > 0.0f) ? def.emission_scale : 1.0f;

    return mat.diffuse||mat.normal||mat.specular||mat.detail_diffuse;
}

void RhiMaterialDatabase::record_material_chain(const std::string& mat_name,
                                                AssetUsageRecorder* rec) const {
    if (!rec) return;
    // Walk the resolved parent-inheritance chain, recording the defining.mat of
    // the material and of every ancestor. The visited set
    // guards against the cyclic parent="" links the build pass already tolerates.
    std::string pn = mat_name;
    std::set<std::string> vis;
    while (!pn.empty() && !vis.contains(pn)) {
        vis.insert(pn);
        if (auto sf = material_source_file_.find(pn); sf != material_source_file_.end())
            rec->record_used(sf->second);
        auto pi = all_materials_.find(pn);
        if (pi == all_materials_.end()) break;
        pn = pi->second.parent;
    }
}

namespace {
int detail_count_ = 0;
} // namespace

// Mesh-scoped resolve. Looks up the material via
// `mesh_surface_to_material_` first (so placeholder names like
// `Object04` resolve correctly per-mesh), falls back to the global
// surface→material map and the auto-discovery path used by the
// plain surface-name overload.
GpuMaterial RhiMaterialDatabase::resolve(const std::string& mesh_path,
                                          const std::string& surface_name,
                                          const PathResolver& paths,
                                          IRhiBackend& backend,
                                          AssetUsageRecorder* rec) {
    // Hot path. This runs once per (mesh, surface) per frame at
    // 3000+ FPS. Previous code did `mesh_path + "::" + surface_name`
    // which heap-allocates a fresh std::string on every call —
    // millions of allocations per second. We hash the pair directly
    // (PairHash in the header) and store views into the input
    // strings; the cache values are GpuMaterial PODs.
    auto key = std::pair<std::string, std::string>(mesh_path, surface_name);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;

    GpuMaterial mat;
    if (inactive_surfaces_.contains(key)) {
        mat.hidden = true;
        cache_.emplace(std::move(key), mat);
        return mat;
    }

    // 1. Mesh-scoped map (preferred — disambiguates shared names).
    if (auto it = mesh_surface_to_material_.find({mesh_path, surface_name});
        it != mesh_surface_to_material_.end()) {
        if (auto md = all_materials_.find(it->second); md != all_materials_.end()) {
            // The referring.mat for any unresolved texture slot is this
            // material's defining file (empty unless the packer is recording).
            std::string referring;
            if (rec) {
                if (auto sf = material_source_file_.find(it->second);
                    sf != material_source_file_.end())
                    referring = to_source_rel(sf->second, paths);
            }
            if (md->second.hidden > 0) {
                record_material_chain(it->second, rec);
                mat.hidden = true;
                tex_hit_++; cache_.emplace(std::move(key), mat); return mat;
            }
            if (try_load(mat, md->second, paths, backend, rec, referring)) {
                record_material_chain(it->second, rec);
                if (mat.has_detail) detail_count_++;
                tex_hit_++; cache_.emplace(std::move(key), mat); return mat;
            }
        }
        if (it->second.find("dummy")  != std::string::npos ||
            it->second.find("hidden") != std::string::npos) {
            mat.hidden = true;
            tex_hit_++; cache_.emplace(std::move(key), mat); return mat;
        }
    }

    // 2. Fall back to the surface-only resolver. The result is
    // cached under our mesh-scoped key (NOT under the legacy
    // surface-only key) so subsequent lookups for this exact mesh
    // hit the right entry.
    GpuMaterial fallback = resolve(surface_name, paths, backend, rec);
    cache_.emplace(std::move(key), fallback);
    return fallback;
}

GpuMaterial RhiMaterialDatabase::resolve(const std::string& surface_name,
                                          const PathResolver& paths,
                                          IRhiBackend& backend,
                                          AssetUsageRecorder* rec) {
    const std::string& data_dir = paths.data_root();
    if (auto it = cache_surface_.find(surface_name); it != cache_surface_.end()) return it->second;
    GpuMaterial mat;

    // Helper: the referring.mat (source-relative) for a resolved material name,
    // used to attribute unresolved texture slots. Empty unless packer-recording.
    auto referring_for = [&](const std::string& mat_name) -> std::string {
        if (!rec) return "";
        if (auto sf = material_source_file_.find(mat_name); sf != material_source_file_.end())
            return to_source_rel(sf->second, paths);
        return "";
    };

    // 1. Try surface→material mapping from node/world files
    if (auto sm = surface_to_material_.find(surface_name); sm != surface_to_material_.end()) {
        if (auto md = all_materials_.find(sm->second); md != all_materials_.end()) {
            if (md->second.hidden > 0) {
                record_material_chain(sm->second, rec);
                mat.hidden = true;
                tex_hit_++; cache_surface_[surface_name] = mat; return mat;
            }
            if (try_load(mat, md->second, paths, backend, rec, referring_for(sm->second))) {
                record_material_chain(sm->second, rec);
                if (mat.has_detail) detail_count_++;
                tex_hit_++; cache_surface_[surface_name] = mat; return mat;
            }
        }
        if (sm->second.find("dummy") != std::string::npos ||
            sm->second.find("hidden") != std::string::npos) {
            mat.hidden = true;
            tex_hit_++; cache_surface_[surface_name] = mat; return mat;
        }
    }

    // 2. Try using surface name directly as material name
    if (auto md = all_materials_.find(surface_name); md != all_materials_.end())
        if (try_load(mat, md->second, paths, backend, rec, referring_for(surface_name))) {
            record_material_chain(surface_name, rec);
            tex_hit_++; cache_surface_[surface_name] = mat; return mat;
        }

    // 3. Auto-discover textures by convention for any material name we have
    //    (from surface_to_material_ or material_map.txt)
    {
        std::vector<std::string> mat_names_to_try;
        if (auto sm = surface_to_material_.find(surface_name); sm != surface_to_material_.end())
            mat_names_to_try.push_back(sm->second);
        mat_names_to_try.push_back(surface_name);

        const auto& tex_dirs = paths.texture_search_dirs();

        for (const auto& mat_name : mat_names_to_try) {
            MaterialDef auto_def;
            auto_def.name = mat_name;
            for (const auto& dir : tex_dirs) {
                for (const auto& suffix : PathResolver::texture_suffixes()) {
                    std::string path = paths.texture_candidate(dir, mat_name, suffix);
                    if (asset_exists(path, assets_)) {
                        auto_def.textures[std::string(suffix)] = path.substr(data_dir.size() + 1);
                    }
                }
            }
            if (!auto_def.textures.empty()) {
                // Auto-discovered slots have no defining.mat; try_load still
                // records the texture files themselves as used.
                if (try_load(mat, auto_def, paths, backend, rec, "",
                             /*auto_discovered=*/true)) {
                    tex_hit_++; cache_surface_[surface_name] = mat; return mat;
                }
            }
        }
    }

    tex_miss_++; tex_misses_.push_back(surface_name); cache_surface_[surface_name] = mat; return mat;
}

std::string RhiMaterialDatabase::get_displacement_path(
    const std::string& surface_name,
    const PathResolver& paths,
    const std::string& suffix) const
{
    // Find the material for this surface
    auto sit = surface_to_material_.find(surface_name);
    if (sit == surface_to_material_.end()) return "";

    // Walk the material inheritance chain looking for the texture
    std::string mat_name = sit->second;
    for (int depth = 0; depth < 10; depth++) {
        auto mit = all_materials_.find(mat_name);
        if (mit == all_materials_.end()) break;
        const auto& def = mit->second;
        auto tit = def.textures.find(suffix);
        if (tit != def.textures.end() && !tit->second.empty()) {
            return paths.displacement_path(tit->second);
        }
        if (def.parent.empty()) break;
        mat_name = def.parent;
    }
    return "";
}

const MaterialDef* RhiMaterialDatabase::get_material_for_surface(
        const std::string& surface_name) const {
    auto sit = surface_to_material_.find(surface_name);
    if (sit == surface_to_material_.end()) return nullptr;
    auto mit = all_materials_.find(sit->second);
    return (mit != all_materials_.end()) ? &mit->second : nullptr;
}

// ── Mesh-scoped helpers ─────────────────────────────────────────

// Both delegate to the surface-only lookup when no mesh-scoped
// binding exists for the (mesh, surface) pair. This is what makes
// tessellation safe for surfaces with shared names — without the
// mesh-scoped path, "Object04" in weathercock would inherit the
// `_h` heightmap declared for airship_rigging and CPU tessellation
// would displace its 154 verts against an unrelated 1024×1024
// texture, producing the spike artefacts we saw in the screenshot.

std::string RhiMaterialDatabase::get_displacement_path(
    const std::string& mesh_path,
    const std::string& surface_name,
    const PathResolver& paths,
    const std::string& suffix) const
{
    if (auto it = mesh_surface_to_material_.find({mesh_path, surface_name});
        it != mesh_surface_to_material_.end()) {
        std::string mat_name = it->second;
        for (int depth = 0; depth < 10; depth++) {
            auto mit = all_materials_.find(mat_name);
            if (mit == all_materials_.end()) break;
            const auto& def = mit->second;
            auto tit = def.textures.find(suffix);
            if (tit != def.textures.end() && !tit->second.empty()) {
                return paths.displacement_path(tit->second);
            }
            if (def.parent.empty()) break;
            mat_name = def.parent;
        }
        return "";
    }
    return get_displacement_path(surface_name, paths, suffix);
}

const MaterialDef* RhiMaterialDatabase::get_material_for_surface(
        const std::string& mesh_path,
        const std::string& surface_name) const {
    if (auto it = mesh_surface_to_material_.find({mesh_path, surface_name});
        it != mesh_surface_to_material_.end()) {
        auto mit = all_materials_.find(it->second);
        return (mit != all_materials_.end()) ? &mit->second : nullptr;
    }
    return get_material_for_surface(surface_name);
}

bool RhiMaterialDatabase::is_surface_active(const std::string& mesh_path,
                                          const std::string& surface_name) const {
    if (inactive_surfaces_.contains({mesh_path, surface_name})) return false;
    const auto* material = get_material_for_surface(mesh_path, surface_name);
    return material != nullptr && material->hidden <= 0;
}

} // namespace heaven
