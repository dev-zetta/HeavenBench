#include "engine/render_test_scene.h"

#include "engine/mesh.h"
#include "engine/render_test_scene_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <set>
#include <tuple>

namespace heaven {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::string_view kTextureRoot = "render-tests/v1/textures/";
using Pixel = std::array<std::uint8_t, 4>;

std::string texture_path(std::string_view name) {
    return std::string(kTextureRoot) + std::string(name) + ".dds";
}

// A conventional RGBA8 DDS with a complete, deterministic mip chain. These
// buffers are mounted into AssetFileSystem; TextureStore still owns decoding,
// semantic selection, upload and cache lifetime.
SharedAssetBuffer make_dds(const std::function<Pixel(int, int)>& sample) {
    constexpr int side = 64;
    constexpr std::uint32_t levels = 7;
    std::array<std::uint32_t, 32> header{};
    header[0] = 0x20534444u;
    header[1] = 124;
    header[2] = 0x0002100fu;
    header[3] = side;
    header[4] = side;
    header[5] = side * 4;
    header[7] = levels;
    header[19] = 32;
    header[20] = 0x41;
    header[22] = 32;
    header[23] = 0x000000ff;
    header[24] = 0x0000ff00;
    header[25] = 0x00ff0000;
    header[26] = 0xff000000;
    header[27] = 0x00401008;
    auto bytes = std::make_shared<AssetBuffer>();
    for (const auto value : header) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes->push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    std::vector<Pixel> pixels(static_cast<std::size_t>(side) * side);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) pixels[y * side + x] = sample(x, y);
    }
    for (int width = side; width > 0; width /= 2) {
        for (const auto& p : pixels) bytes->insert(bytes->end(), p.begin(), p.end());
        if (width == 1) break;
        const int next_width = width / 2;
        std::vector<Pixel> next(static_cast<std::size_t>(next_width) * next_width);
        for (int y = 0; y < next_width; ++y) {
            for (int x = 0; x < next_width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            sum += pixels[(2 * y + dy) * width + 2 * x + dx][c];
                        }
                    }
                    next[y * next_width + x][c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        pixels = std::move(next);
    }
    return bytes;
}

Vertex vertex(glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    Vertex v{};
    v.px = p.x; v.py = p.y; v.pz = p.z;
    v.nx = n.x; v.ny = n.y; v.nz = n.z;
    v.u = v.u1 = uv.x; v.v = v.v1 = uv.y;
    v.displacement_normal_scale = 1.0f;
    v.tx = 1.0f; v.tw = 1.0f;
    return v;
}

glm::vec3 position(const Vertex& v) { return {v.px, v.py, v.pz}; }

void surface_tangents(Surface& surface) {
    std::vector<glm::vec3> tangents(surface.vertices.size(), glm::vec3(0));
    std::vector<glm::vec3> bitangents(surface.vertices.size(), glm::vec3(0));
    for (std::size_t i = 0; i < surface.indices.size(); i += 3) {
        const auto a = surface.indices[i], b = surface.indices[i + 1], c = surface.indices[i + 2];
        const auto& va = surface.vertices[a];
        const auto& vb = surface.vertices[b];
        const auto& vc = surface.vertices[c];
        const glm::vec2 du{vb.u - va.u, vb.v - va.v}, dv{vc.u - va.u, vc.v - va.v};
        const float determinant = du.x * dv.y - du.y * dv.x;
        if (std::abs(determinant) < 1e-10f) continue;
        const auto e1 = position(vb) - position(va), e2 = position(vc) - position(va);
        const auto t = (e1 * dv.y - e2 * du.y) / determinant;
        const auto bt = (e2 * du.x - e1 * dv.x) / determinant;
        for (const auto index : {a, b, c}) { tangents[index] += t; bitangents[index] += bt; }
    }
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        auto& v = surface.vertices[i];
        const glm::vec3 n{v.nx, v.ny, v.nz};
        auto t = tangents[i] - n * glm::dot(n, tangents[i]);
        if (glm::dot(t, t) < 1e-12f) {
            t = glm::cross(std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0), n);
        }
        t = glm::normalize(t);
        v.tx = t.x; v.ty = t.y; v.tz = t.z;
        v.tw = glm::dot(glm::cross(n, t), bitangents[i]) < 0 ? -1 : 1;
    }
}

void bounds(Mesh& mesh) {
    mesh.bbox_min = glm::vec3(std::numeric_limits<float>::max());
    mesh.bbox_max = glm::vec3(std::numeric_limits<float>::lowest());
    for (auto& surface : mesh.surfaces) {
        surface_tangents(surface);
        for (const auto& v : surface.vertices) {
            mesh.bbox_min = glm::min(mesh.bbox_min, position(v));
            mesh.bbox_max = glm::max(mesh.bbox_max, position(v));
        }
    }
}

Surface grid(glm::vec2 min, glm::vec2 max, int columns, int rows,
             float z = 0.0f, bool curved = false) {
    Surface s;
    for (int y = 0; y <= rows; ++y) {
        for (int x = 0; x <= columns; ++x) {
            const glm::vec2 uv{float(x) / float(columns), float(y) / float(rows)};
            const auto xy = glm::mix(min, max, uv);
            float height = z;
            glm::vec3 normal{0, 0, 1};
            if (curved) {
                const float k = 2.2f;
                height += 0.16f * std::sin(k * xy.x) * std::cos(k * xy.y);
                normal = glm::normalize(glm::vec3{
                    -0.16f * k * std::cos(k * xy.x) * std::cos(k * xy.y),
                    0.16f * k * std::sin(k * xy.x) * std::sin(k * xy.y), 1});
            }
            s.vertices.push_back(vertex({xy, height}, normal, uv));
        }
    }
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const auto a = static_cast<std::uint32_t>(y * (columns + 1) + x);
            const auto b = a + 1;
            const auto c = a + columns + 1;
            const auto d = c + 1;
            s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
        }
    }
    return s;
}

Surface card(float width, float height, int columns = 1, int rows = 1) {
    auto s = grid({-width / 2, 0}, {width / 2, height}, columns, rows);
    for (auto& v : s.vertices) {
        v.pz = v.py; v.py = 0;
        v.nx = 0; v.ny = -1; v.nz = 0;
    }
    return s;
}

// The rounded cube preserves planar centres and adds actual bevel geometry.
Surface box(glm::vec3 half, float bevel = 0.0f) {
    Surface s;
    const int divisions = bevel > 0 ? 8 : 1;
    for (int face = 0; face < 6; ++face) {
        const int axis = face / 2;
        const float sign = face % 2 ? 1.0f : -1.0f;
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        const auto base = static_cast<std::uint32_t>(s.vertices.size());
        for (int y = 0; y <= divisions; ++y) {
            for (int x = 0; x <= divisions; ++x) {
                glm::vec3 p{};
                p[axis] = sign * half[axis];
                p[u] = (2.0f * float(x) / float(divisions) - 1.0f) * half[u];
                p[v] = (2.0f * float(y) / float(divisions) - 1.0f) * half[v];
                glm::vec3 n{};
                n[axis] = sign;
                if (bevel > 0) {
                    const auto inner = glm::clamp(p, -half + bevel, half - bevel);
                    n = glm::normalize(p - inner);
                    p = inner + bevel * n;
                }
                s.vertices.push_back(vertex(p, n, {float(x) / float(divisions), float(y) / float(divisions)}));
            }
        }
        for (int y = 0; y < divisions; ++y) {
            for (int x = 0; x < divisions; ++x) {
                const auto a = base + y * (divisions + 1) + x;
                const auto b = a + 1, c = a + divisions + 1, d = c + 1;
                if (sign > 0) s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
                else s.indices.insert(s.indices.end(), {a, d, b, a, c, d});
            }
        }
    }
    return s;
}

Surface sphere(float radius, int segments = 32, int rings = 16) {
    Surface s;
    s.vertices.push_back(vertex({0, 0, radius}, {0, 0, 1}, {0.5f, 0}));
    for (int ring = 1; ring < rings; ++ring) {
        const float theta = kPi * float(ring) / float(rings);
        for (int segment = 0; segment <= segments; ++segment) {
            const float phi = 2 * kPi * float(segment) / float(segments);
            const glm::vec3 n{std::sin(theta) * std::cos(phi),
                              std::sin(theta) * std::sin(phi), std::cos(theta)};
            s.vertices.push_back(vertex(radius * n, n,
                {float(segment) / float(segments), float(ring) / float(rings)}));
        }
    }
    const auto south = static_cast<std::uint32_t>(s.vertices.size());
    s.vertices.push_back(vertex({0, 0, -radius}, {0, 0, -1}, {0.5f, 1}));
    for (int segment = 0; segment < segments; ++segment) {
        s.indices.insert(s.indices.end(), {0, 1u + segment, 2u + segment});
    }
    for (int ring = 0; ring < rings - 2; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const auto a = static_cast<std::uint32_t>(1 + ring * (segments + 1) + segment);
            const auto b = a + 1, c = a + segments + 1, d = c + 1;
            s.indices.insert(s.indices.end(), {a, c, d, a, d, b});
        }
    }
    const auto last = static_cast<std::uint32_t>(1 + (rings - 2) * (segments + 1));
    for (int segment = 0; segment < segments; ++segment) {
        s.indices.insert(s.indices.end(), {last + segment, south, last + segment + 1});
    }
    return s;
}

glm::mat4 translation(glm::vec3 p) { return glm::translate(glm::mat4(1), p); }

struct Builder {
    RenderTestSceneDefinition definition;

    explicit Builder(std::string_view id) {
        definition.id = id;
        const std::array<std::pair<std::string_view, Pixel>, 13> colors{{
            {"gray", {145, 145, 145, 255}}, {"white", {235, 235, 235, 255}},
            {"pure-white", {255, 255, 255, 255}},
            {"black", {14, 17, 20, 255}}, {"red", {205, 42, 35, 255}},
            {"green", {45, 170, 65, 255}}, {"blue", {35, 72, 205, 255}},
            {"gold", {194, 133, 37, 255}}, {"orange", {235, 103, 22, 255}},
            {"cloth", {68, 35, 150, 255}}, {"spec-matte", {0, 45, 0, 255}},
            {"spec-gloss", {0, 235, 0, 255}}, {"normal", {128, 128, 255, 255}},
        }};
        for (const auto& [name, color] : colors) {
            const auto semantic = name == "normal" ? TextureSemantic::NormalLinear
                : name.starts_with("spec-") ? TextureSemantic::ScalarLinear
                : TextureSemantic::ColorSRGB;
            add_texture(name, semantic, [color](int, int) { return color; });
        }
        add_texture("checker", TextureSemantic::ColorSRGB, [](int x, int y) {
            const auto c = static_cast<std::uint8_t>(((x / 8) ^ (y / 8)) & 1 ? 215 : 42);
            return Pixel{c, c, c, 255};
        });
        add_texture("stripes", TextureSemantic::ColorSRGB, [](int x, int) {
            const auto c = static_cast<std::uint8_t>(x % 8 < 4 ? 235 : 24);
            return Pixel{c, c, c, 255};
        });
        add_texture("normal-wave", TextureSemantic::NormalLinear, [](int x, int y) {
            const auto n = glm::normalize(glm::vec3{
                0.35f * std::cos(2 * kPi * (float(x) + 0.5f) / 16),
                0.25f * std::sin(2 * kPi * (float(y) + 0.5f) / 16), 1});
            return Pixel{static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255),
                         static_cast<std::uint8_t>((n.y * 0.5f + 0.5f) * 255),
                         static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255), 255};
        });
        add_texture("leaf", TextureSemantic::ColorSRGB, [](int x, int y) {
            const float u = (float(x) + 0.5f - 32) / 24.0f;
            const float v = (float(y) + 0.5f - 32) / 30.0f;
            const auto a = static_cast<std::uint8_t>(std::clamp((1.0f - u * u - v * v) * 12, 0.0f, 1.0f) * 255);
            return Pixel{38, static_cast<std::uint8_t>(125 + (x % 8) * 7), 32, a};
        });
        add_texture("shadow-leaf", TextureSemantic::ColorSRGB, [](int x, int y) {
            const float u = (float(x) + 0.5f - 32) / 24.0f;
            const float v = (float(y) + 0.5f - 32) / 30.0f;
            const float edge = std::clamp((1.0f - u * u - v * v) * 12, 0.0f, 1.0f);
            // Above the visible mask threshold inside the leaf, while the
            // shadow-only smoothstep(.40,.85) has fractional coverage.
            const float interior = 0.55f + 0.30f * (float(x) + 0.5f) / 64;
            const auto alpha = static_cast<std::uint8_t>(edge * interior * 255);
            return Pixel{38, 160, 32, alpha};
        });
        add_texture("window", TextureSemantic::ColorSRGB, [](int x, int y) {
            const bool frame = x < 5 || x >= 59 || y < 5 || y >= 59 || x % 32 < 3 || y % 32 < 3;
            return frame ? Pixel{75, 48, 30, 255} : Pixel{130, 180, 200, 100};
        });
        add_texture("height", TextureSemantic::ScalarLinear, [](int x, int y) {
            const float h = 0.5f + 0.35f * std::sin(2 * kPi * (float(x) + 0.5f) / 16)
                                          * std::cos(2 * kPi * (float(y) + 0.5f) / 32);
            const auto c = static_cast<std::uint8_t>(std::clamp(h * 255, 0.0f, 255.0f));
            return Pixel{c, c, c, 255};
        });
    }

    void add_texture(std::string_view name, TextureSemantic semantic,
                     const std::function<Pixel(int, int)>& sample) {
        definition.textures.push_back({texture_path(name), make_dds(sample), semantic});
    }

    RenderTestMaterial material(std::string_view label, std::string_view diffuse = "gray",
                                bool glossy = false) {
        RenderTestMaterial m;
        m.surface_name = "render_test_" + std::string(label);
        m.material.material_name = m.surface_name;
        m.material.parent_chain = {"mesh_base"};
        m.diffuse_texture = texture_path(diffuse);
        m.normal_texture = texture_path("normal");
        m.specular_texture = texture_path(glossy ? "spec-gloss" : "spec-matte");
        m.material.dedicated_spec = true;
        return m;
    }

    int add(std::string_view label, Surface surface, RenderTestMaterial material,
            glm::mat4 transform = glm::mat4(1)) {
        Mesh mesh;
        mesh.name = label;
        mesh.path = "render-tests/meshes/" + definition.id + "/" + std::string(label);
        surface.name = material.surface_name;
        mesh.surfaces.push_back(std::move(surface));
        bounds(mesh);
        const auto index = static_cast<int>(definition.meshes.size());
        definition.meshes.push_back(std::move(mesh));
        definition.instances.push_back({index, transform});
        definition.materials.push_back(std::move(material));
        return index;
    }

    int add_box(std::string_view label, glm::vec3 centre, glm::vec3 half,
                std::string_view color = "gray", float bevel = 0.0f) {
        return add(label, box(half, bevel), material(label, color), translation(centre));
    }

    void add_sphere(std::string_view label, glm::vec3 centre, float radius,
                    RenderTestMaterial m) {
        add(label, sphere(radius), std::move(m), translation(centre));
    }

    void ground(glm::vec2 min = {-6, -2}, glm::vec2 max = {6, 8}, bool glossy = false) {
        const auto texture = definition.id == "shadow-cascade" ? "white" : "checker";
        add("ground", grid(min, max, 4, 4), material("ground", texture, glossy));
        definition.grounds.push_back({0, min, max, 0});
    }

    void light(std::string_view label, glm::vec3 p, glm::vec3 color,
               SceneLightKind kind = SceneLightKind::Omni) {
        SceneLight l;
        l.name = "render_test_" + std::string(label);
        l.kind = kind; l.world_pos = l.local_pos = p; l.color = color;
        l.multiplier = definition.id == "lighting" || definition.id == "bloom" ? 64 : 4;
        l.radius = 12;
        l.direction = glm::normalize(glm::vec3(0, 2, 0) - p);
        l.inner_cone_rad = 0.3f; l.outer_cone_rad = 0.7f;
        definition.lights.push_back(std::move(l));
    }

    void lamp(std::string_view label, glm::vec3 p, glm::vec3 color, float scale = 8) {
        auto m = material(label, "white", true);
        m.emission_texture = texture_path("white");
        m.material.emission_scale = scale;
        std::copy_n(&color.x, 3, m.material.emission_color);
        // The explicit SceneLight gives this fixture a stable light origin;
        // no asset-name emission heuristic is needed.
        m.material.emission_light_source = false;
        add_sphere(label, p, 0.18f, std::move(m));
        light(label, p, color);
    }

    void emitter(std::string_view name, std::string_view material_name, glm::vec3 p) {
        ParticleEmitter e;
        e.name = name; e.material_name = material_name;
        e.transform = translation(p); e.emitter_size = {0.12f, 0.12f, 0};
        e.spawn_rate = 12; e.life_mean = 3; e.life_spread = 0.5f;
        e.velocity_mean = 0.6f; e.radius_mean = 0.2f; e.radius_spread = 0.05f;
        e.growth_mean = 0.18f; e.max_particles = 64;
        definition.particle_emitters.push_back(std::move(e));
    }
};

void add_animation(Builder& b) {
    const int shared = b.add("instanced_bevel", box({0.28f, 0.28f, 0.28f}, 0.08f),
        b.material("instanced_bevel", "gold", true), translation({-3, 1, 0.3f}));
    for (int i = 1; i < 12; ++i) {
        b.definition.instances.push_back({shared,
            translation({-3.0f + float(i % 4) * 0.7f, 1.0f + float(i / 4) * 0.7f, 0.3f})});
    }
    RenderTestSkinnedObject object;
    object.transform = translation({1.3f, 2, 0.1f});
    auto& sm = object.mesh;
    sm.name = "two_bone_ribbon";
    sm.path = "render-tests/meshes/animation/two_bone_ribbon";
    sm.bones = {{"root", -1}, {"hinge", 0}};
    sm.num_frames = 60;
    for (int frame = 0; frame < sm.num_frames; ++frame) {
        sm.frames.push_back({{0, 0, 0, 1}, glm::quat(1, 0, 0, 0)});
        sm.frames.push_back({{0, 0, 1.2f, 1},
            glm::angleAxis(0.65f * std::sin(2 * kPi * float(frame) / float(sm.num_frames)), glm::vec3(0, 1, 0))});
    }
    const auto ribbon = card(0.8f, 2.4f, 4, 24);
    auto m = b.material("two_bone_ribbon", "stripes");
    m.material.two_sided = true;
    SkinnedSurface surface;
    surface.name = m.surface_name;
    surface.indices = ribbon.indices;
    for (const auto& v : ribbon.vertices) {
        SkinnedVertex sv{};
        sv.px = v.px; sv.py = v.py; sv.pz = v.pz;
        sv.nx = v.nx; sv.ny = v.ny; sv.nz = v.nz;
        sv.u = sv.u1 = v.u; sv.v = sv.v1 = v.v;
        const float upper = std::clamp((v.pz - 0.8f) / 0.8f, 0.0f, 1.0f);
        sv.bone_indices[1] = 1;
        sv.bone_weights[0] = 1 - upper; sv.bone_weights[1] = upper;
        surface.vertices.push_back(sv);
    }
    sm.surfaces.push_back(std::move(surface));
    b.definition.materials.push_back(std::move(m));
    b.definition.skinned_objects.push_back(std::move(object));

    auto flag_surface = card(1.4f, 1, 14, 10);
    auto flag_material = b.material("cloth_flag", "cloth");
    flag_material.material.two_sided = true;
    const auto transform = translation({-0.4f, 3.8f, 1.2f});
    const int flag_index = b.add("cloth_flag", std::move(flag_surface),
        std::move(flag_material), transform);
    const auto& flag = b.definition.meshes[flag_index].surfaces.front();
    ClothBody cloth;
    cloth.node_name = "render_test_cloth_flag";
    cloth.mesh_index = flag_index; cloth.world_transform = transform;
    cloth.wind_velocity = cloth.wind_velocity_world = {0, 3.5f, 0.6f};
    cloth.bind_vertices = flag.vertices; cloth.bind_indices = flag.indices;
    for (std::size_t i = 0; i < flag.vertices.size(); ++i) {
        const auto p = position(flag.vertices[i]);
        cloth.rest_pos.push_back(p); cloth.position.push_back(p); cloth.prev_position.push_back(p);
        const bool anchor = (i % 15) == 0;
        cloth.inv_mass.push_back(anchor ? 0 : 1);
        cloth.mv_to_particle.push_back(static_cast<int>(i));
        if (anchor) { cloth.anchor_idx.push_back(static_cast<int>(i)); cloth.anchor_local.push_back(p); }
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::size_t i = 0; i < flag.indices.size(); i += 3) {
        const auto a = flag.indices[i], c = flag.indices[i + 1], d = flag.indices[i + 2];
        cloth.triangles.push_back({static_cast<int>(a), static_cast<int>(c), static_cast<int>(d)});
        for (const auto pair : {std::pair{a, c}, std::pair{c, d}, std::pair{d, a}}) {
            edges.insert(std::minmax(pair.first, pair.second));
        }
    }
    for (const auto [a, c] : edges) {
        cloth.linear.push_back({static_cast<int>(a), static_cast<int>(c),
            glm::length(cloth.rest_pos[c] - cloth.rest_pos[a])});
    }
    b.definition.cloths.push_back(std::move(cloth));
    b.add_box("flag_pole", {-1.1f, 3.8f, 1.1f}, {0.035f, 0.035f, 1.1f}, "black");
}

Result<void> validate(const RenderTestSceneDefinition& definition) {
    std::set<std::string_view> names;
    for (const auto& m : definition.materials) {
        if (!names.insert(m.surface_name).second) {
            return std::unexpected(Error("Duplicate procedural material: " + m.surface_name));
        }
    }
    for (const auto& mesh : definition.meshes) {
        for (const auto& s : mesh.surfaces) {
            if (!names.contains(s.name) || s.vertices.empty() || s.indices.empty() || s.indices.size() % 3 != 0) {
                return std::unexpected(Error("Invalid procedural surface: " + s.name));
            }
            for (const auto& v : s.vertices) {
                const auto p = position(v);
                const glm::vec3 n{v.nx, v.ny, v.nz};
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                    !std::isfinite(glm::dot(n, n)) || std::abs(glm::dot(n, n) - 1) > 0.001f) {
                    return std::unexpected(Error("Non-finite procedural vertex: " + s.name));
                }
            }
            for (std::size_t i = 0; i < s.indices.size(); i += 3) {
                const auto a = s.indices[i], c = s.indices[i + 1], d = s.indices[i + 2];
                if (a >= s.vertices.size() || c >= s.vertices.size() || d >= s.vertices.size()) {
                    return std::unexpected(Error("Procedural index outside vertex buffer: " + s.name));
                }
                const auto cross = glm::cross(position(s.vertices[c]) - position(s.vertices[a]),
                                               position(s.vertices[d]) - position(s.vertices[a]));
                if (glm::dot(cross, cross) < 1e-15f) {
                    return std::unexpected(Error("Degenerate procedural triangle: " + s.name));
                }
            }
        }
    }
    return {};
}

} // namespace

Result<RenderTestSceneDefinition> make_render_test_scene(std::string_view id) {
    if (render_test_scene_uses_assets(id)) {
        return std::unexpected(Error("Authored render test scene requires Heaven assets: " + std::string(id)));
    }
    if (std::ranges::none_of(render_test_scene_catalog(), [id](const auto& s) { return s.id == id; })) {
        return std::unexpected(Error("Unknown render test scene: " + std::string(id)));
    }
    Builder b(id);
    if (id == "shadow-cascade") b.ground({-8, -2}, {8, 80});
    else if (id == "depth-of-field") b.ground({-5, -4}, {5, 30});
    else if (id == "atmosphere") b.ground({-6, -2}, {6, 24});
    else if (id == "foliage") b.ground({-4, -2}, {4, 6});
    else b.ground({-6, -2}, {6, 8}, id == "reflections" || id == "rain");

    if (id == "contact-shadow") {
        b.add("curved_receiver", grid({-2, 0}, {1, 3}, 24, 24, 0.18f, true), b.material("curved_receiver", "white"));
        b.add_box("bevel_caster", {1.5f, 1.6f, 0.7f}, {0.65f, 0.65f, 0.7f}, "red", 0.18f);
        auto contact_only = b.material("thin_blocker", "black");
        contact_only.material.casts_shadow = false;
        b.add("thin_blocker", box({0.6f, 0.025f, 0.12f}), std::move(contact_only), translation({-1, 1.5f, 0.45f}));
        b.add_box("near_contact", {0.2f, 3.5f, 0.1f}, {0.65f, 0.25f, 0.1f}, "blue", 0.06f);
        b.add_sphere("smooth_stone", {-2.7f, 2.2f, 0.55f}, 0.55f, b.material("smooth_stone", "gray"));
    } else if (id == "shadow-cascade") {
        for (int i = 0; i < 6; ++i) {
            b.add_box("tower_" + std::to_string(i), {-2.5f + float(i % 2) * 5.0f, 2.0f + float(i) * 11.0f, 2},
                {0.6f, 0.6f, 2}, i % 2 ? "red" : "white");
        }
        auto leaf = b.material("leaf_shadow_card", "shadow-leaf");
        leaf.material.alpha_ref = 0.5f; leaf.material.two_sided = true;
        b.add("leaf_shadow_card", card(2.3f, 2.6f), std::move(leaf), translation({1.5f, 2.5f, 0.7f}));
    } else if (id == "material-models") {
        const std::array<std::string_view, 6> labels{"matte", "chrome", "lacquer", "brushed", "cloth", "leaf"};
        for (int i = 0; i < 6; ++i) {
            auto m = b.material(labels[i], i == 2 ? "red" : i == 4 ? "cloth" : i == 5 ? "leaf" : "gray", i != 0 && i != 4 && i != 5);
            m.material.is_metallic = i == 1 || i == 3;
            if (i == 0) {
                m.material.has_detail = true;
                m.material.detail_mode = 4;
                m.detail_diffuse_texture = texture_path("checker");
                m.detail_normal_texture = texture_path("normal-wave");
                m.detail_specular_texture = texture_path("spec-gloss");
                const std::array<float, 4> base{0.75f, 1.25f, 0.125f, 0.25f};
                const std::array<float, 4> detail{3, 2, 0.125f, -0.25f};
                std::copy(base.begin(), base.end(), m.material.base_transform);
                std::copy(detail.begin(), detail.end(), m.material.detail_transform);
            }
            if (i == 1 || i == 3) m.normal_texture = texture_path("normal-wave");
            if (i == 5) { m.material.alpha_ref = 0.5f; m.material.two_sided = true; }
            b.add_sphere(labels[i], {-2.8f + float(i % 3) * 2.2f, 1.2f + float(i / 3) * 2.5f, 0.85f}, 0.85f, std::move(m));
        }
        b.add_box("pure_white_guard", {3.6f, 3.7f, 0.5f}, {0.5f, 0.5f, 0.5f}, "pure-white");
    } else if (id == "transparency") {
        b.add_box("behind_glass", {-2, 3.2f, 0.8f}, {0.7f, 0.6f, 0.8f}, "red", 0.1f);
        auto glass = b.material("glass", "white", true); glass.material.glass = true;
        b.add("glass", box({0.85f, 0.07f, 1.2f}, 0.04f), std::move(glass), translation({-2, 1.5f, 1.2f}));
        auto window = b.material("window", "window"); window.material.alpha_ref = 0.5f; window.material.two_sided = true;
        b.add("window", card(1.8f, 2.4f), std::move(window), translation({0.4f, 1.5f, 0}));
        b.add_box("behind_window", {0.4f, 3.5f, 0.8f}, {0.7f, 0.6f, 0.8f}, "blue");
        auto leaf = b.material("leaf_cutout", "leaf"); leaf.material.alpha_ref = 0.5f; leaf.material.two_sided = true;
        b.add("leaf_cutout", card(1.5f, 2.4f), std::move(leaf), translation({2.8f, 2, 0}));
    } else if (id == "displacement") {
        for (int i = 0; i < 2; ++i) {
            const std::string label = i == 0 ? "coarse_height" : "dense_ground_relief";
            auto m = b.material(label, "checker");
            m.height_texture = texture_path("height");
            m.material.geometry_displacement = true;
            m.material.ground_relief_shading = i == 1;
            m.material.displaced_surface_normal = i == 1;
            m.material.tess_length = 0.2f; m.material.tess_scale = 0.2f;
            m.material.tess_threshold = 80; m.material.tess_distance = 100; m.material.tess_power = 1;
            const int density = i == 0 ? 9 : 48;
            b.add(label, grid({-1.4f, 0}, {1.4f, 3.5f}, density, density, 0.16f),
                std::move(m), translation({i == 0 ? -1.8f : 1.8f, 0, 0}));
        }
        b.add_box("height_blocker", {0, 3.5f, 0.45f}, {0.12f, 0.4f, 0.45f}, "red");
        auto pom = b.material("pom_height", "checker");
        pom.height_texture = texture_path("height");
        b.add("pom_height", grid({-1.4f, 4.4f}, {1.4f, 6.6f}, 1, 1, 0.16f), std::move(pom));
    } else if (id == "anti-aliasing") {
        for (int i = 0; i < 7; ++i) {
            auto transform = translation({-2.4f + float(i) * 0.24f, 2, 1.4f});
            transform = glm::rotate(transform, glm::radians(17.0f * float(i)), glm::vec3(0, 1, 0));
            const auto label = "slat_" + std::to_string(i);
            b.add(label, box({0.012f + float(i) * 0.004f, 0.04f, 1.3f}), b.material(label, "black"), transform);
        }
        auto m = b.material("specular_sphere", "gray", true); m.material.is_metallic = true;
        m.normal_texture = texture_path("normal-wave");
        b.add_sphere("specular_sphere", {1.5f, 2.5f, 1}, 1, std::move(m));
        auto leaf = b.material("aa_cutout", "leaf"); leaf.material.alpha_ref = 0.5f; leaf.material.two_sided = true;
        b.add("aa_cutout", card(1.4f, 2), std::move(leaf), translation({3.1f, 3, 0}));
    } else if (id == "ambient-occlusion") {
        b.add_box("corner_back", {0, 4, 1.4f}, {2.5f, 0.18f, 1.4f}, "white");
        b.add_box("corner_side", {-2.4f, 2.3f, 1.4f}, {0.18f, 1.8f, 1.4f}, "white");
        b.add_box("contact_box", {-1.4f, 2.8f, 0.55f}, {0.6f, 0.6f, 0.55f}, "gray", 0.12f);
        b.add_box("gap_box", {0.1f, 2.8f, 0.55f}, {0.6f, 0.6f, 0.55f}, "gray", 0.12f);
        b.add_sphere("open_sphere", {2.1f, 1.4f, 0.65f}, 0.65f, b.material("open_sphere", "white"));
    } else if (id == "reflections") {
        auto chrome = b.material("chrome", "gray", true); chrome.material.is_metallic = true;
        b.add_sphere("chrome", {0, 2, 1}, 1, std::move(chrome));
        b.add_box("red_wall", {-3.3f, 3, 1.5f}, {0.12f, 2, 1.5f}, "red");
        b.add_box("green_wall", {3.3f, 3, 1.5f}, {0.12f, 2, 1.5f}, "green");
        b.lamp("emission_strip", {0, 4, 2.6f}, {1, 0.65f, 0.25f}, 15);
    } else if (id == "lighting" || id == "bloom") {
        b.add_box("light_receiver", {0, 3.5f, 1.2f}, {3.5f, 0.16f, 1.2f}, "white");
        b.add_box("light_blocker", {0, 1.7f, 0.8f}, {0.4f, 0.4f, 0.8f}, "gray", 0.1f);
        b.lamp("red_lamp", {-2.8f, 1, 2.1f}, {1, 0.15f, 0.08f}, id == "bloom" ? 35 : 8);
        b.lamp("blue_lamp", {2.8f, 1, 2.1f}, {0.1f, 0.25f, 1}, id == "bloom" ? 18 : 8);
        b.light("spot", {0, -0.7f, 4}, {0.75f, 0.95f, 1}, SceneLightKind::Spot);
        if (id == "bloom") {
            b.add_box("black_reference", {-2, 3, 0.45f}, {0.45f, 0.4f, 0.45f}, "black");
            b.add_box("gray_reference", {2, 3, 0.45f}, {0.45f, 0.4f, 0.45f}, "gray");
        }
    } else if (id == "depth-of-field") {
        const std::array<float, 3> depths{-6.5f, 12, 25};
        for (int i = 0; i < 3; ++i) {
            const auto label = "focus_" + std::to_string(i);
            b.add(label, card(i == 0 ? 0.24f : 2.2f, i == 0 ? 0.54f : 3.5f),
                b.material(label, "checker"), translation({i == 0 ? -0.39f : i == 2 ? 1.8f : 0,
                    depths[i], i == 0 ? 1.52f : 0.4f}));
        }
    } else if (id == "atmosphere") {
        b.add_box("near_tower", {-3, 5, 2}, {0.7f, 0.7f, 2}, "gray");
        b.add_box("far_tower", {4, 18, 5}, {1.3f, 1.3f, 5}, "gray");
        // At 08h this lies close to the catalog's Sun-facing diagnostic ray.
        b.add_box("god_ray_blocker", {7.145f, -7.112f, 8}, {0.9f, 0.18f, 0.9f}, "black");
        b.lamp("fog_lamp", {1.5f, 1.5f, 2.5f}, {1, 0.65f, 0.3f});
        b.light("fog_spot", {-2, 2, 3}, {0.3f, 0.55f, 1}, SceneLightKind::Spot);
    } else if (id == "foliage") {
        b.add_box("grass_caster", {-1, 2.7f, 0.9f}, {0.6f, 0.6f, 0.9f}, "white");
        for (int i = 0; i < 3; ++i) {
            const auto label = "leaf_card_" + std::to_string(i);
            auto m = b.material(label, "leaf"); m.material.two_sided = true; m.material.alpha_ref = 0.5f;
            b.add(label, card(1.2f, 1.7f), std::move(m), translation({0.6f + float(i) * 0.9f, 3, 0.3f}));
        }
    } else if (id == "particles") {
        auto background = b.material("particle_background", "stripes");
        background.emission_texture = texture_path("stripes");
        background.material.emission_scale = 8;
        background.material.emission_light_source = false;
        b.add("particle_background", card(11, 7), std::move(background), translation({0, 4.5f, 0}));
        b.add_box("chimney", {-1.5f, 2, 0.6f}, {0.4f, 0.4f, 0.6f}, "gray");
        b.emitter("render_test_smoke", "fire_smoke", {-1.5f, 2, 1.2f});
        b.emitter("render_test_flame", "fire_flame", {1, 2, 0.25f});
        b.emitter("render_test_glow", "fire_glow", {1, 2, 0.35f});
        b.emitter("render_test_flies", "flies", {2.5f, 2, 1.5f});
        b.lamp("flame_light", {1, 2, 0.5f}, {1, 0.35f, 0.08f}, 12);
    } else if (id == "rain") {
        auto slope = translation({-1.6f, 2, 0.4f});
        slope = glm::rotate(slope, glm::radians(15.0f), glm::vec3(1, 0, 0));
        b.add("wet_slope", box({1.3f, 1.6f, 0.1f}), b.material("wet_slope", "blue", true), slope);
        b.add_sphere("rain_sculpture", {1.6f, 2.5f, 0.9f}, 0.9f, b.material("rain_sculpture", "gray", true));
        b.add_box("rain_reference", {2.8f, 4, 0.5f}, {0.6f, 0.6f, 0.5f}, "white");
    } else if (id == "animation") {
        add_animation(b);
    } else if (id == "output") {
        const std::array<std::string_view, 6> colors{"red", "green", "blue", "gold", "white", "gray"};
        for (int i = 0; i < 6; ++i) {
            const auto label = "reference_" + std::to_string(i);
            b.add_box(label, {-2.5f + float(i % 3) * 2.2f, 1.2f + float(i / 3) * 2.3f, 0.55f},
                {0.7f, 0.65f, 0.55f}, colors[i], 0.1f);
        }
        b.lamp("hdr_white", {0, 4.5f, 2.6f}, {1, 1, 1}, 30);
        auto chrome = b.material("output_chrome", "gray", true); chrome.material.is_metallic = true;
        b.add_sphere("output_chrome", {3.2f, 2.3f, 1}, 0.8f, std::move(chrome));
    }
    if (auto result = validate(b.definition); !result) return std::unexpected(result.error());
    return std::move(b.definition);
}

Result<void> mount_render_test_scene_assets(const RenderTestSceneDefinition& definition,
                                          AssetFileSystem& assets) {
    for (const auto& texture : definition.textures) {
        if (auto result = assets.mount_memory(texture.path, texture.bytes); !result) {
            return std::unexpected(Error("Cannot mount procedural texture " + texture.path + ": " + result.error().message));
        }
    }
    return {};
}

GpuMaterial RenderTestScene::resolve_material(const std::string& /*unused*/,
                                             const std::string& surface_name) const {
    if (const auto it = materials.find(surface_name); it != materials.end()) return it->second;
    GpuMaterial missing;
    missing.hidden = true;
    return missing;
}

Result<RenderTestScene> upload_render_test_scene(RenderTestSceneDefinition definition,
                                               IRhiBackend& backend,
                                               const AppConfig& config) {
    RenderTestScene result;
    for (auto& record : definition.materials) {
        auto& m = record.material;
        const std::array<std::tuple<const std::string*, TextureSemantic, std::uint64_t*>, 8> slots{{
            {&record.diffuse_texture, TextureSemantic::ColorSRGB, &m.diffuse},
            {&record.normal_texture, TextureSemantic::NormalLinear, &m.normal},
            {&record.specular_texture, TextureSemantic::ScalarLinear, &m.specular},
            {&record.detail_diffuse_texture, TextureSemantic::ColorSRGB, &m.detail_diffuse},
            {&record.detail_normal_texture, TextureSemantic::NormalLinear, &m.detail_normal},
            {&record.detail_specular_texture, TextureSemantic::ScalarLinear, &m.detail_specular},
            {&record.height_texture, TextureSemantic::ScalarLinear, &m.height},
            {&record.emission_texture, TextureSemantic::ColorSRGB, &m.emission},
        }};
        for (const auto& [path, semantic, handle] : slots) {
            if (path->empty()) continue;
            *handle = backend.load_texture(*path, semantic);
            if (!*handle) return std::unexpected(Error("Cannot upload procedural texture: " + *path));
        }
        const bool ground_enabled = !m.ground_relief_shading || config.features.ground_relief;
        m.geometry_displacement = m.geometry_displacement && config.tessellation.enabled && ground_enabled;
        m.displaced_surface_normal = m.displaced_surface_normal && ground_enabled;
        m.ground_relief_shading = m.ground_relief_shading && config.features.ground_relief;
        // The backend applies global scale/distance exactly once, just as it
        // does for resolved authored materials. Keep these authored values.
        result.materials.emplace(record.surface_name, std::move(m));
    }
    result.scene.cpu_meshes = std::move(definition.meshes);
    result.scene.instances = std::move(definition.instances);
    for (const auto& mesh : result.scene.cpu_meshes) {
        auto gpu = backend.upload_mesh(mesh);
        gpu.path = mesh.path;
        result.scene.gpu_meshes.push_back(std::move(gpu));
    }
    for (auto& object : definition.skinned_objects) {
        Mesh bind;
        bind.name = object.mesh.name; bind.path = object.mesh.path;
        for (int i = 0; std::cmp_less(i, object.mesh.surfaces.size()); ++i) {
            bind.surfaces.push_back({object.mesh.surfaces[i].name,
                object.mesh.skin_frame(i, 0), object.mesh.surfaces[i].indices});
        }
        bounds(bind);
        const auto mesh_index = static_cast<int>(result.scene.cpu_meshes.size());
        const Instance instance{mesh_index, object.transform};
        auto gpu = config.features.gpu_skinning ? backend.upload_skinned(object.mesh) : backend.upload_mesh(bind);
        gpu.path = bind.path;
        AnimatedObject animated;
        animated.gpu_mesh = gpu; animated.instance = instance;
        animated.mesh_index = mesh_index; animated.anim_speed = object.frames_per_second;
        animated.last_frame_f.assign(object.mesh.surfaces.size(), -1e30f);
        animated.smesh = std::move(object.mesh);
        result.scene.cpu_meshes.push_back(std::move(bind));
        result.scene.gpu_meshes.push_back(std::move(gpu));
        result.scene.instances.push_back(instance);
        result.scene.animated.push_back(std::move(animated));
    }
    result.scene.lights = std::move(definition.lights);
    result.scene.particle_emitters = std::move(definition.particle_emitters);
    result.scene.cloths = std::move(definition.cloths);
    result.grounds = std::move(definition.grounds);
    for (const auto& ground : result.grounds) {
        if (auto registered = backend.set_render_test_ground(ground.instance_index,
            ground.local_min, ground.local_max, ground.z); !registered) {
            for (const auto& mesh : result.scene.gpu_meshes) backend.release_mesh(mesh);
            return std::unexpected(registered.error());
        }
    }
    for (const auto& mesh : result.scene.cpu_meshes) {
        for (const auto& surface : mesh.surfaces) {
            result.scene.total_vertices += surface.vertices.size();
            result.scene.total_triangles += surface.indices.size() / 3;
        }
    }
    result.scene.base_vertices = result.scene.total_vertices;
    result.scene.base_triangles = result.scene.total_triangles;
    return result;
}

} // namespace heaven
