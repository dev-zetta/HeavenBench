/*
 * HeavenBench —: Node-file resolution rejects empty or
 * whitespace-only filenames.
 *
 * Feature: asset-path-resolution,
 *
 *
 * PathResolver::node_path(filename) (src/engine/path_resolver.cpp) is the only
 * fallible scene-file resolver: it rejects an empty or whitespace-only
 * filename with an Error and emits no partial path, and otherwise returns the
 * verbatim concatenation Data_Root + "/heaven/nodes/" + filename.
 *
 * This property exercises both branches across many inputs:
 *
 *   Blank branch  — for any filename that is empty or made up solely of
 *                   whitespace, node_path returns an error result
 *                   (!result.has_value()). The Result is the error variant, so
 *                   "no partial path emitted" is asserted by the absence of a
 *                   value: there is no string to inspect on the error branch.
 *
 *   Value branch  — for any filename containing at least one non-whitespace
 *                   character, node_path returns a value byte-for-byte equal to
 *                   an INDEPENDENT reference concatenation
 *                   (data_root + "/heaven/nodes/" + filename) that does not
 *                   share code with the resolver.
 *
 * Data_Root is always non-blank so PathResolver::create succeeds; the test is
 * about the filename argument, not construction (covers that).
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

// Generate a blank filename: empty, or composed solely of whitespace
// characters (space, tab, newline, carriage return, vertical tab, form feed).
// container<> can yield the empty string, so the empty-filename case is
// covered alongside arbitrary whitespace mixes like " ", "  ", "\t", "\n\t ".
rc::Gen<std::string> blank_filename() {
    static const std::string ws = " \t\n\r\v\f";
    return rc::gen::container<std::string>(rc::gen::elementOf(ws));
}

// Generate a non-blank filename: a string guaranteed to contain at least one
// non-whitespace character. We splice a literal 'x' between two arbitrary
// strings so the result always has a non-whitespace char regardless of what
// the arbitrary parts contain. The concrete regression anchor "bridges.node"
// is mixed in so the real call-site input is exercised too.
rc::Gen<std::string> nonblank_filename() {
    auto spliced = rc::gen::map(
        rc::gen::pair(rc::gen::arbitrary<std::string>(),
                      rc::gen::arbitrary<std::string>()),
        [](const std::pair<std::string, std::string>& parts) {
            return parts.first + "x" + parts.second;
        });
    return rc::gen::oneOf(std::move(spliced),
                          rc::gen::just<std::string>("bridges.node"));
}

// Generate a non-blank Data_Root so PathResolver::create always succeeds. Same
// splice trick: an arbitrary prefix/suffix around a guaranteed non-whitespace
// char. Includes the realistic default "extracted/heaven" as one alternative.
rc::Gen<std::string> nonblank_data_root() {
    auto spliced = rc::gen::map(
        rc::gen::pair(rc::gen::arbitrary<std::string>(),
                      rc::gen::arbitrary<std::string>()),
        [](const std::pair<std::string, std::string>& parts) {
            return parts.first + "d" + parts.second;
        });
    return rc::gen::oneOf(std::move(spliced),
                          rc::gen::just<std::string>("extracted/heaven"));
}

// Returns true when every RapidCheck case
// passes. Two checks of 100 cases each (RapidCheck's default maxSuccess) cover
// the blank-rejection branch and the value branch, so the property is
// exercised over >= 100 iterations.
bool property_6_node_rejects_blank_filenames() {
    // Blank branch: empty or whitespace-only filename must be rejected with an
    // error result and no partial path. The error variant carries no string,
    // so checking !has_value() is the assertion that no partial path leaked.
    bool ok = rc::check(
        "Property 6: node_path rejects empty/whitespace-only filenames",
        [] {
            const std::string data_root = *nonblank_data_root();
            const std::string filename = *blank_filename();

            auto resolver = heaven::PathResolver::create(data_root);
            RC_ASSERT(resolver.has_value());

            auto result = resolver->node_path(filename);
            RC_ASSERT(!result.has_value());
        });

    // Value branch: a filename with at least one non-whitespace char yields a
    // value equal to an independent reference concatenation.
    ok = rc::check(
             "Property 6: node_path returns data_root + \"/heaven/nodes/\" + filename",
             [] {
                 const std::string data_root = *nonblank_data_root();
                 const std::string filename = *nonblank_filename();

                 auto resolver = heaven::PathResolver::create(data_root);
                 RC_ASSERT(resolver.has_value());

                 auto result = resolver->node_path(filename);
                 RC_ASSERT(result.has_value());

                 // Independent oracle: raw concatenation, no shared code with
                 // the resolver.
                 const std::string expected =
                     data_root + "/heaven/nodes/" + filename;
                 RC_ASSERT(*result == expected);
             }) &&
         ok;

    return ok;
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_6{&property_6_node_rejects_blank_filenames};

} // namespace
