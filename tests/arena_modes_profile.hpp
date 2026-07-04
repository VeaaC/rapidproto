#pragma once

// Test-support: THE field-modes profile for the arena_modes corpus, shared by every suite that
// consumes it (layout dump golden, generated-header golden, decode tests) so they can never
// drift apart. Must stay in lockstep with tests/corpus/arena_modes.modes, the same selection's
// CLI spelling (the golden comparison fails if the two diverge); mirrors the corpus comments.

#include <initializer_list>
#include <utility>

#include "catch_amalgamated.hpp"
#include "rapidproto/arenagen/modes.hpp"
#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"

namespace rapidproto::test {

inline arenagen::FieldModes arena_modes_profile(const ResolvedFileSet& set,
                                                const SymbolTable& symbols) {
    arenagen::FieldModesSpec spec;
    spec.profile_name = "lean";
    for (const auto& [mode, name] :
         std::initializer_list<std::pair<arenagen::FieldMode, const char*>>{
             {arenagen::FieldMode::Drop, "fm.Holder.debug"},
             {arenagen::FieldMode::Drop, "fm.Holder.extra"},
             {arenagen::FieldMode::Drop, "fm.Holder.old_ids"},
             {arenagen::FieldMode::Raw, "fm.Blob"},  // covers blob/blobs/req_blob; skips the map
             {arenagen::FieldMode::Raw, "fm.Holder.grp"}}) {
        spec.entries.push_back({mode, name, "test-profile"});
    }
    auto resolved = arenagen::resolve_field_modes(spec, set, symbols);
    REQUIRE(resolved.is_ok());
    return std::move(resolved).value();
}

}  // namespace rapidproto::test
