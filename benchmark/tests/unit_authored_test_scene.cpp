#include "engine/asset_file_system.h"
#include "engine/material_rhi.h"
#include "engine/path_resolver.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/scene_rhi.h"
#include "engine/zip_archive.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
using namespace heaven;
using Bytes = std::vector<std::uint8_t>;

template<class T> void pod(Bytes& b, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    b.insert(b.end(), p, p + sizeof(value));
}
void lps(Bytes& b, const std::string& s) {
    pod(b, static_cast<std::uint32_t>(s.size() + 1));
    b.insert(b.end(), s.begin(), s.end()); b.push_back(0);
}
struct Element {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;
    std::vector<Element> children;
};
struct XmlWriter {
    std::vector<std::string> tags, attrs, values;
    Bytes tree;
    static std::uint8_t intern(std::vector<std::string>& pool, const std::string& s) {
        const auto it = std::find(pool.begin(), pool.end(), s);
        if (it != pool.end()) return static_cast<std::uint8_t>(it - pool.begin());
        pool.push_back(s); return static_cast<std::uint8_t>(pool.size() - 1);
    }
    void write(const Element& e) {
        tree.push_back(intern(tags, e.tag));
        tree.push_back(static_cast<std::uint8_t>(e.attrs.size())); tree.push_back(0);
        for (const auto& [k,v] : e.attrs) {
            tree.push_back(intern(attrs, k)); tree.push_back(intern(values, v));
        }
        tree.push_back(e.text.empty() ? 0 : 1);
        if (!e.text.empty()) lps(tree, e.text);
        else {
            tree.push_back(static_cast<std::uint8_t>(e.children.size()));
            for (const auto& child : e.children) { tree.push_back(0); write(child); tree.push_back(0); }
        }
    }
    Bytes finish(const Element& e) {
        write(e); Bytes result{'x','m','l','0'}; pod(result, std::uint32_t{0});
        for (const auto& s : tags) lps(result, s);
        pod(result, static_cast<std::uint32_t>(attrs.size()));
        for (const auto& s : attrs) lps(result, s);
        pod(result, static_cast<std::uint32_t>(values.size()));
        for (const auto& s : values) lps(result, s);
        result.insert(result.end(), tree.begin(), tree.end()); return result;
    }
};
Bytes mesh_bytes() {
    Bytes b{'m','i','0','8'};
    for (int i=0; i<10; ++i) pod(b, 0.0f);
    pod(b, std::int32_t{1});
    const std::string name = "fixture_surface";
    pod(b, static_cast<std::int32_t>(name.size()+1));
    b.insert(b.end(), name.begin(), name.end()); b.push_back(0);
    for (int i=0; i<10; ++i) pod(b, 0.0f);
    pod(b, std::int32_t{3});
    for (const auto p : {glm::vec3{0,0,0}, glm::vec3{1,0,0}, glm::vec3{0,1,0}}) {
        pod(b,p.x); pod(b,p.y); pod(b,p.z);
        pod(b,std::uint16_t{32768}); pod(b,std::uint16_t{32768}); pod(b,std::uint16_t{65535});
    }
    pod(b,std::int32_t{3});
    for (const auto uv : {glm::vec2{0,0},glm::vec2{1,0},glm::vec2{0,1}}) { pod(b,uv.x); pod(b,uv.y); }
    pod(b,std::int32_t{0}); pod(b,std::int32_t{1});
    pod(b,std::uint16_t{0}); pod(b,std::uint16_t{1}); pod(b,std::uint16_t{2}); return b;
}
constexpr auto kMesh = "heaven/meshes/fixture.mesh";
constexpr auto kChild = "heaven/nodes/fixture_child.node";
Element object() {
    return {"node",{{"type","ObjectMesh"},{"name","fixture"}},"",{
        {"mesh",{},kMesh,{}},
        {"surface",{{"name","fixture_surface"},{"material","fixture_material"}},"",{}},
        {"transform",{},"1 0 0 0 0 1 0 0 0 0 1 0 1 2 3 1",{}}}};
}
Element reference(const std::string& path) {
    return {"node",{{"type","NodeReference"}},"",{{"reference",{},path,{}}}};
}
Bytes node(std::vector<Element> children) { return XmlWriter{}.finish({"nodes",{},"",std::move(children)}); }
using Files = std::map<std::string, Bytes>;
Files fixture_files() {
    Files files;
    const auto roots = render_test_scene_asset_nodes("asset-contact-shadow");
    files[std::string(roots[0])] = node({reference(kChild)});
    auto light = Element{"node",{{"type","LightOmni"},{"name","fixture_light"}},"",{
        {"transform",{},"1 0 0 0 0 1 0 0 0 0 1 0 0 0 1 1",{}},
        {"shadow",{},"0",{}}}};
    files[std::string(roots[1])] = node({object(), light});
    files[kChild] = node({object()}); files[kMesh] = mesh_bytes();
    // Broken unrelated world/bridge data must never be opened by a compact fixture.
    files["heaven/heaven.world"] = {'b','a','d'};
    files["heaven/nodes/bridges.node"] = {'b','a','d'};
    files["heaven/materials/fixture.mat"] = XmlWriter{}.finish({"materials",{},"",{
        {"material",{{"name","fixture_material"}},"",{}}}});
    return files;
}
class UploadBackend final : public IRhiBackend {
public:
    bool fail_upload = false;
    std::set<std::uint64_t> live_handles;
    std::size_t releases = 0;
    GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    std::string device_name() const override { return "authored fixture host test"; }
    std::string api_version() const override { return {}; }
    RendererFeatureStats renderer_feature_stats() const override { return {}; }
    GpuMesh upload_mesh(const Mesh& m) override {
        GpuMesh g; g.path=m.path; g.name=m.name;
        if (!fail_upload) for (const auto& s : m.surfaces) {
            const auto handle = next_handle_++;
            live_handles.insert(handle);
            g.surfaces.push_back({handle,static_cast<std::uint32_t>(s.indices.size()),s.name});
        }
        return g;
    }
    void release_mesh(const GpuMesh& mesh) override {
        for (const auto& surface : mesh.surfaces) releases += live_handles.erase(surface.handle);
    }
    GpuMesh upload_skinned(const SkinnedMesh& m) override {
        Mesh bind; bind.name=m.name; bind.path=m.path;
        for (int si=0; std::cmp_less(si, m.surfaces.size()); ++si)
            bind.surfaces.push_back({m.surfaces[si].name,m.skin_frame(si,0),m.surfaces[si].indices});
        return upload_mesh(bind);
    }
    std::uint64_t load_texture(const std::string& /*path*/, TextureSemantic /*semantic*/) override { return 1; }
    Result<void> init_pipelines(const AppConfig& /*config*/) override { return {}; }
    FrameResult render_frame(std::span<const Instance> /*instances*/, std::span<const int> /*dirty_instance_indices*/,
        std::span<const GpuMesh> /*meshes*/, const MaterialResolver& /*resolve*/, const Camera& /*camera*/,
        const LightingParams& /*lighting*/, const WeatherState& /*weather*/, const TonemapParams& /*tonemap*/,
        int /*width*/, int /*height*/) override { return {}; }
    void present() override {}
    Result<void> update_mesh_vertices(std::uint64_t /*surface_handle*/, const Vertex* /*vertices*/,
        std::uint32_t /*vertex_count*/) override { return {}; }
private:
    std::uint64_t next_handle_ = 1;
};
AppConfig config_for(const AssetFileSystem& assets) {
    AppConfig c; c.data_dir=assets.data_root(); c.render_test_scene="asset-contact-shadow";
    c.tessellation.enabled=false; return c;
}
Result<AssetFileSystem> memory(const Files& files) {
    auto a=AssetFileSystem::create_memory("authored_fixture_memory");
    if (!a) return a;
    for (const auto& [path,bytes] : files)
        if (auto mounted=a->mount_memory(path,std::make_shared<const AssetBuffer>(bytes)); !mounted)
            return std::unexpected(mounted.error());
    return a;
}
bool check(bool ok,const char* message) {
    if (!ok) std::fprintf(stderr,"authored scene test: %s\n",message);
    return ok;
}
bool validate_scene(const RhiScene& s) {
    const auto roots=render_test_scene_asset_nodes("asset-contact-shadow");
    const auto shift=render_test_scene_asset_translation("asset-contact-shadow",roots[1]);
    return check(s.instances.size()==2 && s.cpu_meshes.size()==1 && s.total_triangles==1,
                 "selected roots must retain instances and deduplicate static mesh") &&
        check(s.source_node_files==std::vector<std::string>{std::string(roots[0]),kChild,std::string(roots[1])},
              "provenance must contain only parsed roots and nested nodes") &&
        check(glm::length(glm::vec3(s.instances[1].transform[3])-glm::vec3(1+shift[0],2+shift[1],3+shift[2]))<1e-6f,
              "catalog parent translation must preserve authored local transform") &&
        check(s.lights.size()==1 && glm::length(s.lights[0].world_pos-glm::vec3(shift[0],shift[1],1+shift[2]))<1e-6f,
              "light must share the root placement") &&
        check(!s.lights[0].shadow_eligible,
              "authored shadow=0 must survive scene loading");
}
bool authored_memory_and_material_validation() {
    auto assets=memory(fixture_files());
    if (!assets) { std::fprintf(stderr,"authored scene memory setup: %s\n",assets.error().message.c_str()); return false; }
    UploadBackend b; auto c=config_for(*assets);
    auto base=load_scene_rhi(c,b,nullptr,std::cref(*assets));
    if (!base || !validate_scene(*base)) return false;
    const auto existing_handles = b.live_handles;
    RhiMaterialDatabase missing;
    for (int retry = 0; retry < 12; ++retry) {
        auto invalid=load_scene_rhi(c,b,missing,"",nullptr,std::cref(*assets));
        if (!check(!invalid && invalid.error().message.find("material is unresolved")!=std::string::npos,
                   "unresolved authored material must stop fixture") ||
            !check(b.live_handles == existing_handles,
                   "failed replacement loads must retire new uploads while preserving the active scene")) return false;
    }
    if (!check(b.releases == 12, "each failed replacement must release its uploaded mesh")) return false;
    RhiMaterialDatabase materials; const auto paths=PathResolver::create(c.data_dir);
    materials.build(*paths,nullptr,std::cref(*assets));
    auto valid=load_scene_rhi(c,b,materials,"",nullptr,std::cref(*assets));
    return check(valid.has_value(),"binary node material mapping must resolve through normal database") &&
           check(b.live_handles.size() == existing_handles.size() + 1,
                 "successful replacement load must keep its handles until the caller retires the old scene") &&
           validate_scene(*valid);
}
bool authored_strict_failures_and_legacy_node() {
    const auto roots=render_test_scene_asset_nodes("asset-contact-shadow");
    const auto original=fixture_files();
    for (int failure=0; failure<7; ++failure) {
        auto files=original;
        if (failure==0) files.erase(std::string(roots[0]));
        if (failure==1) files[kChild]={'b','a','d'};
        if (failure==2) files.erase(kMesh);
        if (failure==3) { files[std::string(roots[0])]=node({}); files[std::string(roots[1])]=node({}); }
        if (failure==4) files[kChild]=node({reference(std::string(roots[0]))});
        if (failure==5) files[kChild]=node({{"node",{{"type","ObjectMesh"}},"",{}}});
        auto a=memory(files);
        if (!a) { std::fprintf(stderr,"authored scene negative setup: %s\n",a.error().message.c_str()); return false; }
        auto c=config_for(*a); UploadBackend b; b.fail_upload=failure==6;
        auto result=load_scene_rhi(c,b,nullptr,std::cref(*a));
        if (!check(!result && result.error().message.find("Asset fixture")!=std::string::npos,
                   "missing root/reference/mesh/empty/cycle/upload must fail explicitly")) return false;
        if (failure==2) {
            c.render_test_scene.clear(); c.node_file=c.data_dir+"/"+std::string(roots[0]);
            auto legacy=load_scene_rhi(c,b,nullptr,std::cref(*a));
            if (!check(legacy.has_value() && legacy->instances.empty() && legacy->source_node_files.empty(),
                       "ordinary --node must preserve best-effort missing mesh behavior")) return false;
        }
    }
    return true;
}
bool authored_directory_and_zip_equivalence() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/std::format("heaven_authored_scene_{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p,e); } } cleanup{root};
    const auto data=root/"Data_Root"; const auto files=fixture_files();
    for (const auto& [path,bytes] : files) {
        fs::create_directories((data/path).parent_path());
        std::ofstream f(data/path,std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    const auto archive=root/"assets.zip"; auto writer=ZipArchiveWriter::create(archive,false);
    const std::string manifest=R"({"schema_version":1,"data_root_prefix":"Data_Root"})";
    if (!writer || !writer->add_bytes(std::string(kAssetArchiveManifest),std::as_bytes(std::span(manifest)),ZipCompression::Store)) return false;
    for (const auto& [path,bytes] : files)
        if (!writer->add_bytes("Data_Root/"+path,std::as_bytes(std::span(bytes)),ZipCompression::Deflate)) return false;
    if (!writer->finish()) return false;
    for (const auto& location : {data.string(),archive.string()}) {
        auto a=AssetFileSystem::open(location); if (!a) return false;
        UploadBackend b; auto c=config_for(*a);
        auto result=load_scene_rhi(c,b,nullptr,std::cref(*a));
        if (!check(result.has_value(),"same authored roots must load from directory and compressed ZIP") ||
            !validate_scene(*result)) return false;
    }
    return true;
}
const heaven::testing::Register memory_test(authored_memory_and_material_validation);
const heaven::testing::Register errors_test(authored_strict_failures_and_legacy_node);
const heaven::testing::Register storage_test(authored_directory_and_zip_equivalence);
}
