/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#ifndef ASCENSION_TINKER_APEX_H
#define ASCENSION_TINKER_APEX_H

#include "Define.h"

#include <array>

class Player;

// Tinker Apex (specialization 102), talents 31-60 and the stance buttons (PTR, 27/10/2026).
// Plan: sortie-apex/talents60/plan.json (ids), docs/PLAN-apex-talents-31-60.md, and
// docs/CONCEPTION-apex-attaques-par-posture.md for the stance buttons. Code: AscensionTinkerApexTalents.cpp.
namespace AscensionTinkerApex
{
// Smart Disc (class-tab node 138379, spell 9300046) and Razor Disc (spec node 138399, marker 9300089):
// Razor Disc replaces Smart Disc on its button (TalentReplacements) and is never held without it.
constexpr uint32 ENTRY_SMART_DISC = 138379;
constexpr uint32 ENTRY_RAZOR_DISC = 138399;
constexpr uint32 SPELL_SMART_DISC = 9300046;
constexpr uint32 SPELL_RAZOR_DISC_TALENT = 9300089;

// One button per stance pair: the original stays in the spellbook, the target is shown in its slot while the
// stance aura is up. Levels of a pair are aligned (decision D3 of 27/10). The last row is the single stance
// button (D4): in Wristblade Stance, the Wristblade Stance button shows Spear Stance.
struct StancePair
{
    uint32 Original;
    uint32 Target;
    uint32 StanceAura;
    uint8 Level;
};

constexpr uint32 SPELL_WRISTBLADE_STANCE = 9300001;
constexpr uint32 SPELL_SPEAR_STANCE = 9300002;

// Both stances are base abilities from level 10 (free nodes 138352 and 138356), each on its own button: no
// stance button pair (D4 dropped on 27/10).
constexpr std::array<StancePair, 3> STANCE_PAIRS =
{{
    { 9300003, 9300008, SPELL_SPEAR_STANCE, 10 },      // key 1: Wristblade Flurry / Spear Thrust
    { 9300006, 9300010, SPELL_SPEAR_STANCE, 14 },      // key 2: Rending Claws / Pinning Strike
    { 9300009, 9300007, SPELL_WRISTBLADE_STANCE, 16 }, // key 3: Spear Throw / Shredding Whirl
}};

// Support trunk (28/10, docs/apex-tronc-revue/talents.json v3): the three buffs of the Apex are learned for free by level,
// like the buffs of every other class, and never through a node (a free node of the class tab would go to the three
// other Tinker specializations too). Single target 10/14/20, Greater 56/58/60. Reserved to an Apex of level 10 or more;
// taken back outside Apex (SynchronizeProgression, AscensionCompat.cpp). One buff per Apex on a target: spell_group
// 2128001-2128004 (exploitation/apex/apex-tronc-soutien-ptr.sql).
struct ApexBuff
{
    uint8 Level;
    uint32 SpellId;
};

constexpr std::array<ApexBuff, 6> APEX_BUFFS =
{{
    { 10, 9300140 },   // Acid Mark: critical strike chance
    { 14, 9300142 },   // Elder's Gift: attack and casting speed
    { 20, 9300144 },   // Cooling Bloom: maximum health and healing received
    { 56, 9300141 },   // Greater Acid Mark
    { 58, 9300143 },   // Greater Elder's Gift
    { 60, 9300145 },   // Greater Cooling Bloom
}};

/// An original of STANCE_PAIRS (a spell whose learn or loss changes the buttons).
constexpr bool IsStanceOriginal(uint32 spellId)
{
    for (StancePair const& pair : STANCE_PAIRS)
        if (pair.Original == spellId)
            return true;
    return false;
}

/// The original a stance target is shown on, 0 when spellId is no target.
constexpr uint32 StanceOriginalOf(uint32 spellId)
{
    for (StancePair const& pair : STANCE_PAIRS)
        if (pair.Target == spellId)
            return pair.Original;
    return 0;
}
} // namespace AscensionTinkerApex

/// Idempotent: reads the class, the active specialization, the level, the originals held and the stance auras,
/// and makes each pair's button show what they call for. Every target stays learned (temporary, unannounced)
/// while its pair is open to the character, whatever the stance: removing it would also remove the auras it
/// owns (Player::removeSpell -> RemoveOwnedAura), Spear Stance itself included. It is removed once the pair
/// is closed (other specialization, original lost). Safe to call from learn/forget hooks of the ORIGINALS only.
void SynchronizeApexStanceAttacks(Player* player);

/// Every trophy source goes through here (Trophy Hunter v1 and v2, Never Saw It Coming, Claim the Skull):
/// adds up to `count` Trophies (9300037) under the cap 5, 6 or 7 (Trophy Rack 9300079/9300080) and heals 3 or 6%
/// per Trophy gained with Trophy Rack. Needs Trophy Hunter (9300036). Returns the Trophies actually gained.
uint32 GrantApexTrophies(Player* player, uint32 count);

#endif
