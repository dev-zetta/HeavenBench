/*
 * HeavenBench — Property test for tone-mapper name parsing
 *
 * Feature: advanced-materials-and-grading,: Tone-mapper parse maps known names and rejects unknowns to ACES
 *
 *
 * lets the user pick the SDR tone-mapping operator by name
 * (ACES, AgX, PBR_Neutral, GT7, Generic); 9.4 requires an unrecognized name to
 * fall back to the ACES default while naming the rejected selection in a
 * diagnostic; 9.3 keeps ACES the flagless default. The grading / camera knob
 * groups share the same pure-helper-plus-caller-diagnostic discipline (10.6,
 * the parse is a pure function (no I/O), so the CLI owns the warning.
 *
 * The contract lives in the header-only `heaven::parse_tone_mapper`
 * (engine/types.h), which normalizes a name (lowercase, dropping spaces /
 * dashes / underscores) and returns a `ToneMapperParse { ToneMapperSelection
 * value; bool rejected; std::string rejected_name; }`:
 *
 *   - A recognized name (case-insensitive, separator-insensitive) yields the
 *     matching ToneMapperSelection with `rejected == false` and an empty
 *     `rejected_name`. Recognized keys after normalization: "aces", "agx",
 *     "pbrneutral" and the "neutral" alias, "gt7", "generic".
 *
 *   - Any unrecognized string (including the empty string) returns
 *     ToneMapperSelection::ACES with `rejected == true` and the ORIGINAL name
 *     bytes preserved verbatim in `rejected_name` for the diagnostic.
 *
 * pins both branches over many inputs:
 *
 *   - Known branch: a canonical operator name, decorated with random letter
 *     case and random spaces / dashes / underscores spliced between characters,
 *     still parses to the correct enum, is not rejected, and carries no
 *     rejected name. This exercises the case-/separator-insensitive
 *     normalization.
 *
 *   - Unknown branch: a random string whose normalized key is NOT a recognized
 *     operator (the empty string included) parses to ACES, is rejected, and
 *     retains its original bytes in rejected_name.
 *
 * The unknown generator computes the same normalized key the function does and
 * discards (RC_PRE) any draw that lands on a recognized key, so a random string
 * can never be mistaken for a known operator.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidcheck.h>

#include "engine/types.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Canonical recognized operator names paired with their expected selection.
// The "neutral" alias is included so the alias branch is exercised too. Each
// string is already in normalized form (lowercase, no separators), so a known
// variant generator can decorate it freely and still normalize back to it.
struct KnownName {
    std::string_view key;
    heaven::ToneMapperSelection expected;
};

constexpr std::array<KnownName, 6> kKnownNames = {{
    {"aces",       heaven::ToneMapperSelection::ACES},
    {"agx",        heaven::ToneMapperSelection::AgX},
    {"pbrneutral", heaven::ToneMapperSelection::PBRNeutral},
    {"neutral",    heaven::ToneMapperSelection::PBRNeutral},
    {"gt7",        heaven::ToneMapperSelection::GT7},
    {"generic",    heaven::ToneMapperSelection::Generic},
}};

// Normalize exactly as parse_tone_mapper does: lowercase, dropping spaces,
// dashes and underscores. Used to decide whether a randomly generated string is
// actually an (accidentally) recognized operator.
std::string normalize(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (char ch : name) {
        if (ch == ' ' || ch == '-' || ch == '_') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return key;
}

bool is_known_key(const std::string& key) {
    return std::ranges::any_of(kKnownNames, [&key](const KnownName& k) { return key == k.key; });
}

// Decorate a normalized canonical key with random letter case and random
// spaces / dashes / underscores spliced between characters. The result still
// normalizes back to `key`, so it must parse to the same operator. Separators
// may also appear at the start / end, which normalization strips. Written in
// the imperative `*rc::gen::...` style (like the sibling prop tests), so it is
// called from within a property body's generation context.
std::string gen_known_variant(std::string_view key) {
    constexpr std::array<char, 3> kSeparators = {' ', '-', '_'};
    std::string out;

    auto emit_separators = [&]() {
        const int count = *rc::gen::inRange<int>(0, 3);
        for (int i = 0; i < count; ++i) {
            out.push_back(kSeparators[*rc::gen::inRange<std::size_t>(
                0, kSeparators.size())]);
        }
    };

    emit_separators();
    for (char ch : key) {
        // Randomize case for alphabetic characters; digits pass through.
        const bool upper = *rc::gen::arbitrary<bool>();
        out.push_back(upper
            ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
            : ch);
        emit_separators();
    }
    return out;
}

bool run_tone_mapper_parse_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Property 10: tone-mapper parse maps known names and rejects unknowns "
        "to ACES (Validates: Requirements 9.1, 9.3, 9.4, 10.6, 11.6)",
        [] {
            // Straddle both branches every run so coverage never collapses onto
            // only the known or only the unknown path.
            const bool exercise_known = *rc::gen::arbitrary<bool>();

            if (exercise_known) {
                // ── Recognized name  ──
                // Pick a canonical operator, decorate it with random case and
                // separators, and assert it round-trips to the right enum with
                // no rejection.
                const std::size_t idx =
                    *rc::gen::inRange<std::size_t>(0, kKnownNames.size());
                const KnownName& known = kKnownNames[idx];
                const std::string input = gen_known_variant(known.key);

                // Sanity: the decorated input really does normalize to the key.
                RC_ASSERT(normalize(input) == std::string(known.key));

                const heaven::ToneMapperParse parsed =
                    heaven::parse_tone_mapper(input);
                RC_ASSERT(parsed.value == known.expected);
                RC_ASSERT(!parsed.rejected);
                RC_ASSERT(parsed.rejected_name.empty());
            } else {
                // ── Unrecognized name  ──
                // Draw an arbitrary string and keep only those whose normalized
                // key is NOT a recognized operator, so the case can never be a
                // disguised known name. The empty string is a valid unknown.
                const std::string input = *rc::gen::arbitrary<std::string>();
                RC_PRE(!is_known_key(normalize(input)));

                const heaven::ToneMapperParse parsed =
                    heaven::parse_tone_mapper(input);

                // Falls back to the ACES default, is flagged rejected, and
                // preserves the original bytes for the diagnostic.
                RC_ASSERT(parsed.value == heaven::ToneMapperSelection::ACES);
                RC_ASSERT(parsed.rejected);
                RC_ASSERT(parsed.rejected_name == input);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_tone_mapper_parse{
    run_tone_mapper_parse_property};

} // namespace
