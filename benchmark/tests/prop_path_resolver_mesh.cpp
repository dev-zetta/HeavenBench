/*
 * HeavenBench — asset-path-resolution,: Mesh names are used
 * verbatim with no extension logic and no existence check.
 *
 * Feature: asset-path-resolution,
 *
 *
 * For any non-empty Data_Root and any mesh name (including names with no
 * extension, unusual extensions like ".mesh"/".obj", or names of files that do
 * not exist on disk), PathResolver::mesh_path returns a Resolved_Path whose
 * trailing segment is the mesh name reproduced exactly — neither appending nor
 * stripping any extension — and the call performs no filesystem access and
 * never fails.
 *
 * The oracle is an INDEPENDENT reference implementation that performs the exact
 * legacy inline concatenation the grass/foliage call sites use today:
 *
 *     data_root + "/heaven/meshes/environment/" + mesh_name
 *
 * It deliberately shares no code with PathResolver, so any divergence (an
 * appended/stripped extension, a normalized separator, a missing-file guard)
 * is caught. The "never fails" facet is structural: mesh_path returns a plain
 * std::string (not a Result), so there is no error channel to exercise — the
 * test asserts totality by simply calling it on every generated input,
 * including pathological Data_Root values, and observing a well-formed result.
 *
 * Mesh names are generated WITHOUT any '/' so that "trailing segment" is
 * well-defined as the substring after the final '/': because the mesh
 * subdirectory fragment ends in '/', that trailing segment must equal the mesh
 * name byte-for-byte. Names cover the no-extension, ".mesh", ".obj", unusual,
 * and empty cases. The Data_Root generator includes empty, trailing-'/',
 * embedded ".."/".", whitespace, and non-ASCII bytes to confirm the result is
 * verbatim concatenation with no normalization.
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, exactly like the other benchmark test TUs.
 */
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is defined
// there (external linkage); Register is an identical inline definition, which
// ODR permits across translation units. Each test TU appends a callable
// returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// The mesh subdirectory fragment, reproduced here independently of
// PathResolver so the oracle does not share code with the unit under test.
constexpr const char* k_oracle_mesh_fragment = "/heaven/meshes/environment/";

// Oracle: the exact legacy inline concatenation the grass/foliage call sites
// perform today for the environment mesh subdirectory.
std::string oracle_mesh_path(const std::string& data_root,
                             const std::string& mesh_name) {
    return data_root + k_oracle_mesh_fragment + mesh_name;
}

// Generate a Data_Root spanning the pathological cases the resolver must
// reproduce verbatim: empty, single and multiple trailing separators, embedded
// "." / "..", leading/trailing whitespace, and non-ASCII bytes. No
// normalization is expected, so each is a valid base prefix to test against.
rc::Gen<std::string> data_root_gen() {
    return rc::gen::element<std::string>(
        std::string{},                  // empty Data_Root (edge)
        "extracted/heaven",             // the default
        "extracted/heaven/",            // single trailing separator
        "extracted/heaven///",          // multiple trailing separators
        "/abs/root",                    // absolute
        "a/../b/./c",                   // embedded. and..
        "  spaced/root  ",              // surrounding whitespace
        "r\xc3\xa9" "sources/h\xc3\xa9" "aven", // non-ASCII (UTF-8) bytes
        ".");                           // bare relative dir
}

// Generate a mesh name without any '/', covering: no extension, the real
// ".mesh" names, unusual extensions (".obj", ".MESH", trailing dot), an empty
// name, and arbitrary slash-free strings. The slash-free guarantee makes the
// "trailing segment" notion well-defined.
rc::Gen<std::string> mesh_name_gen() {
    auto fixed = rc::gen::element<std::string>(
        "fly_island1.mesh",  // the real grass/foliage call-site names
        "fly_island2.mesh",
        "fly_island3.mesh",
        "island",            // no extension at all
        "island.obj",        // unusual extension
        "island.MESH",       // uppercase extension (must not be touched)
        "island.",           // trailing dot, no extension chars
        "weird..name..mesh", // doubled dots
        std::string{});      // empty name

    // Arbitrary slash-free strings exercise the verbatim guarantee on names the
    // fixed list does not anticipate.
    auto arbitrary = rc::gen::suchThat(
        rc::gen::arbitrary<std::string>(),
        [](const std::string& s) { return s.find('/') == std::string::npos; });

    return rc::gen::oneOf(std::move(fixed), std::move(arbitrary));
}

// Returns true when every RapidCheck case
// passes. RapidCheck's default maxSuccess is 100, so the property runs over
// >= 100 iterations.
bool property_5_mesh_names_verbatim() {
    bool ok = rc::check(
        "Property 5: mesh names are used verbatim (no extension logic, no IO, total)",
        [] {
            const std::string data_root = *data_root_gen();
            const std::string mesh_name = *mesh_name_gen();

            // A non-empty Data_Root constructs successfully; mesh_path itself
            // is total and performs no IO, so this never fails. (We require a
            // non-blank Data_Root only to obtain a resolver — construction
            // validation is 's concern.)
            auto resolver = heaven::PathResolver::create(
                data_root.empty() ? std::string("x") : data_root);
            RC_ASSERT(resolver.has_value());

            // Use the same base the resolver was actually built with so the
            // oracle compares against the real prefix.
            const std::string base = resolver->data_root();
            const std::string resolved =
                resolver->mesh_path(heaven::MeshCategory::Environment, mesh_name);

            // (1) Byte-for-byte equal to the independent legacy oracle — this
            //     simultaneously proves no extension was appended or stripped
            //     and no separator was normalized.
            RC_ASSERT(resolved == oracle_mesh_path(base, mesh_name));

            // (2) The Resolved_Path ends with the mesh name exactly.
            RC_ASSERT(resolved.size() >= mesh_name.size());
            RC_ASSERT(resolved.compare(resolved.size() - mesh_name.size(),
                                       mesh_name.size(), mesh_name) == 0);

            // (3) The trailing segment (substring after the final '/') equals
            //     the mesh name verbatim. Because the mesh name contains no
            //     '/' and the subdirectory fragment ends in '/', the segment
            //     after the last '/' must be precisely the mesh name.
            const auto slash = resolved.find_last_of('/');
            const std::string trailing =
                (slash == std::string::npos) ? resolved
                                             : resolved.substr(slash + 1);
            RC_ASSERT(trailing == mesh_name);
        });

    return ok;
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_5_mesh{&property_5_mesh_names_verbatim};

} // namespace
