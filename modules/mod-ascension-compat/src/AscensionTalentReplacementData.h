/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#ifndef ASCENSION_TALENT_REPLACEMENT_DATA_H
#define ASCENSION_TALENT_REPLACEMENT_DATA_H

#include <array>
#include <cstdint>

namespace AscensionCompatData
{
struct ReplacementRank
{
    std::uint32_t SpellId;
    std::uint8_t RequiredLevel;
};

struct TalentReplacement
{
    std::uint8_t ClassId;
    std::uint32_t SpecId;
    std::uint32_t ParentSpellId;
    std::uint32_t OriginalSpellId;
    // Ten, not nine: Aeroblast and Torrential Wrath each carry ten ranks in Spell.dbc
    // (Echo of Nozdormu, the third ten-rank chain, is handled in AscensionPyromancer.cpp).
    // Unused slots stay value-initialised to { 0, 0 } and are skipped by
    // SynchronizeTalentReplacements' `if (!rank.SpellId) continue;`.
    std::array<ReplacementRank, 10> Ranks;
};

// Explicit transformation clauses from the class audits. The parent unlocks the
// base replacement; higher ranks use the captured class trainer's level gates.
// Only ranks whose outstanding requirement was the missing root are included.
// Transient procs and equipment-dependent selectors need separate policies.
//
// OriginalSpellId must be the ROOT of the original's rank chain: the loop below
// compares it to sSpellMgr->GetFirstSpellInChain(id). Ranks must be declared by
// ascending RequiredLevel: the loop keeps the LAST qualifying entry, not the
// highest one.
inline constexpr std::array<TalentReplacement, 16> TalentReplacements =
{{
    // Warbringer stays at rank 1 ON PURPOSE. Spell.dbc does carry 802582@18 .. 802586@52,
    // but none of these replacement chains is declared in acore_world.spell_ranks or in
    // Talent.dbc, so GetFirstSpellInChain(802582) == 802582 and AscensionXoroth.cpp's
    // Named(info, 802581) is false for every rank above the first. Granting ranks 2-6
    // would therefore silently drop, from level 18 on, the Mortal-Wound cast (803240,
    // the spell's whole tooltip), the +18%-per-Demonfire damage factor
    // (AscensionXorothAbilities.cpp:235) and Spender() status (AscensionXoroth.cpp:76).
    // Prerequisite for the ranks: declare the chain in acore_world.spell_ranks.
    { 17, 18, 570727, 801059, {{ { 802581, 0 } }} }, // Flames of Xoroth -> Warbringer
    // Tempest Calling: Conjure Storm -> Updraft; Call Lightning -> Aeroblast.
    { 16, 13, 707615, 800227, {{
        { 802354, 0 }, { 570161, 18 }, { 570162, 26 }, { 570163, 34 },
        { 570164, 42 }, { 570165, 50 }, { 570166, 58 }
    }} },
    { 16, 13, 707615, 500040, {{
        { 801839, 0 }, { 501450, 20 }, { 501451, 30 }, { 501452, 38 },
        { 501453, 46 }, { 501454, 54 }, { 501455, 58 }, { 501456, 66 },
        { 501457, 74 }, { 501458, 80 }
    }} },
    // Tempest Sovereign: Shock -> Brine; Call Lightning -> Torrential Wrath.
    { 16, 14, 560020, 804020, {{
        { 807105, 0 }, { 807106, 20 }, { 807107, 24 }, { 807108, 34 },
        { 807109, 44 }, { 807110, 54 }, { 807111, 58 }
    }} },
    { 16, 14, 560020, 500040, {{
        { 804017, 0 }, { 503352, 16 }, { 503353, 22 }, { 503354, 28 }, { 503355, 34 },
        { 503356, 40 }, { 503357, 46 }, { 503358, 52 }, { 503359, 58 }, { 503360, 64 }
    }} },
    { 16, 13, 704222, 526362, {{ { 704201, 0 } }} }, // Whirlpool -> Raging Zephyr
    // Blood Curse's three specialization forms.
    { 20, 25, 505188, 562720, {{ { 680692, 0 } }} }, // Sanguine Essence
    { 20, 26, 504728, 562720, {{ { 801076, 0 } }} }, // Transgression
    { 20, 27, 504710, 562720, {{ { 562572, 0 } }} }, // Accursed Form
    // Wand of Time -> Timerend; Unmake -> Artificer's Wand.
    { 22, 32, 707430, 520175, {{
        { 801291, 0 }, { 501831, 14 }, { 501832, 22 }, { 501833, 30 },
        { 501834, 38 }, { 501835, 46 }, { 572578, 52 }
    }} },
    { 22, 33, 804478, 804418, {{
        { 561284, 0 }, { 561354, 22 }, { 561355, 32 }, { 561356, 42 }, { 561357, 52 }
    }} },
    { 30, 56, 805708, 500376, {{ { 572382, 0 } }} }, // Murder -> Shudder Scythe
    { 30, 56, 504269, 803985, {{ { 807234, 0 } }} }, // Ghost Claw -> Wraith Claw
    // Knight of Xoroth. These three were only expressed in the local table of
    // AscensionXoroth.cpp, which calls SetTemporarySpellReplacement without ever
    // teaching the target: the call returned silently and the button never changed
    // (P-044). They live here now and were removed from that table.
    // Spec 17 is DEFIANCE and spec 18 is WAR (ChrSpecs.dbc records 17 and 18,
    // class token FLESHWARDEN). 801016, 801059 and 500904 are rank-1 roots in
    // acore_world.spell_ranks.
    // Shieldgore: rank 1 only, same measured reason as Warbringer above. Spell.dbc has
    // 806869@18 .. 806874@58, but they are in no rank chain, so Infernal() and the
    // id == 804353 tests of AscensionXorothAbilities.cpp (Torn Flesh 800341, the 704997
    // burst, the 706563 crit effect, the 680729 curse shield) would all stop matching.
    { 17, 17, 301302, 801016, {{ { 804353, 0 } }} }, // Infernal Strike -> Shieldgore
    // Hellfire Bellows and Brimstone Bludgeon have a single castable spell each in
    // Spell.dbc (no "Rank N" siblings): one rank, no invented level gate.
    { 17, 17, 807587, 801059, {{ { 520292, 0 } }} }, // Flames of Xoroth -> Hellfire Bellows
    { 17, 18, 800710, 500904, {{ { 520005, 0 } }} }  // Sever -> Brimstone Bludgeon
}};
}

#endif
