#include "engine/camera_collision.h"

#include <cstdio>
#include <functional>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
heaven::Mesh wall(float x) {
    heaven::Mesh mesh;
    heaven::Surface surface;
    surface.name="wall";
    for (const auto p : {glm::vec3{x,-10,-10},glm::vec3{x,10,-10},
                         glm::vec3{x,10,10},glm::vec3{x,-10,10}}) {
        heaven::Vertex v{};
        v.px=p.x; v.py=p.y; v.pz=p.z; v.nx=-1;
        surface.vertices.push_back(v);
    }
    surface.indices={0,1,2,0,2,3};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}
bool check(bool value,const char* message) {
    if (!value) std::fprintf(stderr,"collision rebuild: %s\n",message);
    return value;
}
bool collision_rebuild_replaces_geometry_and_body_ids() {
    using namespace heaven;
    CameraCollision collision;
    collision.set_radius(.25f);
    collision.set_enabled(false);
    std::vector<Mesh> meshes{wall(0)};
    glm::mat4 translated{1}; translated[3].x=10;
    std::vector<Instance> instances{{0,glm::mat4{1}},{0,translated}};
    if (!check(collision.build(meshes,instances) && collision.body_count()==2,
               "first world must have both instances") ||
        !check(!collision.enabled() && collision.radius()==.25f,
               "build must preserve the collision choice and radius")) return false;
    collision.set_enabled(true);
    const auto first=collision.resolve({-2,0,0},{2,0,0});
    const auto second=collision.resolve({8,0,0},{12,0,0});
    if (!check(first.x<-.24f && first.x>-.36f && second.x<9.76f && second.x>9.64f,
               "original walls must stop actual sphere sweeps")) return false;

    // Reuse mesh/instance index zero for different geometry and remove index one.
    meshes[0]=wall(3);
    instances.resize(1);
    if (!check(collision.build(meshes,instances) && collision.body_count()==1,
               "rebuild must discard the removed body ID")) return false;
    const auto old_wall=collision.resolve({-2,0,0},{2,0,0});
    const auto new_wall=collision.resolve({1,0,0},{5,0,0});
    const auto removed_wall=collision.resolve({8,0,0},{12,0,0});
    if (!check(glm::length(old_wall-glm::vec3{2,0,0})<1e-5f &&
               glm::length(removed_wall-glm::vec3{12,0,0})<1e-5f,
               "old shape and removed instance must no longer collide") ||
        !check(new_wall.x>2.64f && new_wall.x<2.76f,
               "same-index replacement geometry must collide at its new plane") ||
        !check(collision.enabled() && collision.radius()==.25f,
               "rebuild must preserve enabled state and radius")) return false;

    collision.set_enabled(false);
    instances.clear();
    if (!check(!collision.build(meshes,instances) && collision.body_count()==0,
               "empty rebuild must retire every old body") ||
        !check(!collision.enabled() && collision.radius()==.25f,
               "empty rebuild must preserve settings")) return false;
    collision.set_enabled(true);
    return check(glm::length(collision.resolve({1,0,0},{5,0,0})-glm::vec3{5,0,0})<1e-5f,
                 "empty rebuilt world must allow motion through the previous plane");
}
const heaven::testing::Register test(collision_rebuild_replaces_geometry_and_body_ids);
}
