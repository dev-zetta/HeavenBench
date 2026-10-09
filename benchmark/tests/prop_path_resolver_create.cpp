/*
 * HeavenBench — asset-path-resolution
 *
 * Feature: asset-path-resolution,: Construction rejects an empty or
 * whitespace-only Data_Root.
 *
 *
 *
 * For any Data_Root string that is empty or consists solely of whitespace,
 * PathResolver::create returns an error indicating the Data_Root is missing
 * and constructs no resolver (has_value() == false). For any string containing
 * at least one non-whitespace character, create succeeds and the resulting
 * resolver reports that exact value, verbatim, as its data_root() — with NO
 * trimming, separator handling, or case change (a non-blank Data_Root with
 * leading/trailing whitespace such as "  x  " must succeed and round-trip
 * byte-for-byte).
 *
 * The blank generator emits empty and whitespace-only combinations (spaces,
 * tabs, newlines, and mixes); the non-blank generator emits the concrete
 * "extracted/heaven" anchor plus arbitrary strings carrying at least one
 * non-whitespace character, including ones padded with leading/trailing
 * whitespace to confirm there is no trimming.
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, exactly like the other benchmark test TUs.
 */
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is defined
// there (external linkage); Register is an identical inline definition. Each
// test TU appends a callable returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Empty-or-all-whitespace test, used only to constrain the non-blank generator
// (the resolver itself owns the production definition). Mirrors the resolver's
// construction guard: an empty string is blank, and a string made up entirely
// of whitespace characters is blank.
bool is_blank(std::string_view s) {
    return std::ranges::all_of(s, [](unsigned char c) { return std::isspace(c) != 0; });
}

// ── Generators ─────────────────────────────────────────────────────────

// A blank Data_Root: empty or built solely from whitespace characters. Pieces
// cover single spaces, runs of spaces, tabs, newlines, and carriage returns;
// concatenating zero or more of them yields "", " ", "  ", "\t", "\n",
// "\t \n", and other whitespace-only combinations. All must be rejected.
rc::Gen<std::string> blank_root_gen() {
    auto ws_piece = rc::gen::elementOf(std::vector<std::string>{
        " ", "  ", "\t", "\n", "\r", "\t \n", "\f", "\v",
    });
    return rc::gen::map(
        rc::gen::container<std::vector<std::string>>(ws_piece),
        [](const std::vector<std::string>& parts) {
            std::string s;  // empty when parts is empty — also a blank root
            for (const auto& p : parts)
                s += p;
            return s;
        });
}

// A non-blank Data_Root: at least one non-whitespace character. Includes the
// concrete "extracted/heaven" anchor and arbitrary strings (filtered to be
// non-blank), plus a variant padded with surrounding whitespace so the
// no-trimming, verbatim round-trip is exercised ("  x  " must succeed AND
// data_root() must return "  x  " unchanged).
rc::Gen<std::string> nonblank_root_gen() {
    auto arbitrary_nonblank = rc::gen::suchThat(
        rc::gen::arbitrary<std::string>(),
        [](const std::string& s) { return !is_blank(s); });

    auto padded = rc::gen::map(arbitrary_nonblank, [](const std::string& s) {
        return "  " + s + "  ";  // still non-blank; leading/trailing whitespace
    });

    return rc::gen::oneOf(
        rc::gen::element<std::string>("extracted/heaven"),
        arbitrary_nonblank,
        padded);
}

// Returns true when every RapidCheck case passes. Two checks of
// 100 cases each (RapidCheck's default maxSuccess), so the property is
// exercised over >= 100 iterations.
bool property_7_construction_validation() {
    // Rejection branch: any empty or whitespace-only Data_Root is rejected,
    // and no resolver is constructed (has_value() == false).
    bool ok = rc::check(
        "Property 7: construction rejects an empty or whitespace-only Data_Root",
        [] {
            const std::string data_root = *blank_root_gen();
            auto created = heaven::PathResolver::create(data_root);
            RC_ASSERT(!created.has_value());
        });

    // Success branch: any Data_Root with at least one non-whitespace character
    // is accepted, and the resolver reports it back verbatim (no trimming,
    // separator handling, or case change).
    ok = rc::check(
             "Property 7: construction accepts a non-blank Data_Root and reports "
             "it verbatim",
             [] {
                 const std::string data_root = *nonblank_root_gen();
                 auto created = heaven::PathResolver::create(data_root);
                 RC_ASSERT(created.has_value());
                 RC_ASSERT(created->data_root() == data_root);
             }) &&
         ok;

    return ok;
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_7{&property_7_construction_validation};

} // namespace
