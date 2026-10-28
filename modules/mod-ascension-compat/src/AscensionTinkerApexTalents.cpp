/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
//
// Tinker Apex (class 28, specialization 102), PTR only: the stance buttons and the C++ half of the talents of
// levels 31 to 60. Ids are frozen by sortie-apex/talents60/plan.json; the human reading is
// docs/PLAN-apex-talents-31-60.md, and docs/CONCEPTION-apex-attaques-par-posture.md for the buttons.
//
// Script names (contract C-SCRIPTS, written to spell_script_names by exploitation/apex/apex-talents60-ptr.sql):
//   9300011 spell_apex_rocket_pounce          Canopy Drop
//   9300012, 9300108, 9300116 spell_apex_cannon_gate   Cannon Offline refuses the cannon (and Plasma Frenzy holds it)
//   9300013 spell_apex_plasma_bolt            Patient Ambush: the shot spends Charged Caster
//   9300018 spell_apex_skull_and_spine        Claim the Skull; the killed one's body becomes its bones (28/10)
//   9300036 aura_apex_trophy_hunter           Trophy Hunter v1, through GrantApexTrophies (cap of Trophy Rack)
//   9300055 spell_apex_mimicked_voice         creatures walk to the voice
//   9300062 spell_apex_grapple_snatch         the pack stays silent 3 s
//   9300046 spell_apex_smart_disc             the slow disc that cuts through everything on its line, out and back to
//                                             the hand (27/10 16:00; return since 27/10 evening)
//   9300090 spell_apex_razor_disc             same disc, plus the interrupt and Disc Overcharge
//   9300102 aura_apex_patient_ambush          charges the cannon while still under camouflage
//   9300106 aura_apex_plasma_splash           50% of the real damage around the target
//   9300107 spell_apex_plasma_splash_damage   ADDED to the plan's contract: leaves the main target out
//   9300116 aura_apex_plasma_frenzy           Plasma Frenzy (node 138407, replaces Plasma Barrage): random shots
//   9300117-9300119 spell_apex_plasma_frenzy_shot   caps of Plasma Frenzy (5 enemies a blast, 8 blasts an enemy)
//   creature 9300000 npc_apex_hunting_drone   Falconer's Eye, COAAPEX messages to the addon, circling flight 45-100 m
//                                             from the Apex, reveals hidden enemies around it (9300132, 27/10 evening)
// Without a spell_script_names row: AscensionApexDiscSpellInfoScript (GlobalScript, OnLoadSpellCustomAttr) gives the
// disc cuts 9300091, 9300092 and 9300130 their armor and absorption piercing (no Spell.dbc field carries those).
// Without a script name (hooks of this file): Arc Discharge, Never Saw It Coming, Blood Frenzy, Danger Seeker,
// Sentry Protocol, Worthy Prey, Self-Destruct Protocol, Three Red Dots, the stance buttons, and the login clean-up of
// a node ultimate no node grants any more (Plasma Barrage since Plasma Frenzy took node 138407).

#include "AscensionTinkerApex.h"

#include "AllSpellScript.h"
#include "AscensionCoATalentData.h"
#include "AscensionSpecialization.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Containers.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GlobalScript.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "UnitScript.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// WorldMapArea.dbc (DBCStores.cpp): DBCStores.h keeps it out of reach and only offers Map2ZoneCoordinates, but
// the drone needs the size of the zone map itself, the client places the marks with its own position.
extern DBCStorage<WorldMapAreaEntry> sWorldMapAreaStore;

using namespace AscensionTinkerApex;

namespace
{
constexpr uint32 SPEC_APEX = 102;
constexpr uint8 APEX_LEVEL = 10;

enum ApexSpells : uint32
{
    // in service (talents 1-30, outils/apex-spe-dbc.py)
    SPELL_ROCKET_POUNCE = 9300011,
    SPELL_PLASMA_CASTER = 9300012,
    SPELL_SKULL_AND_SPINE = 9300018,
    SPELL_UNMASKED = 9300028,
    SPELL_TROPHY_HUNTER = 9300036,
    SPELL_TROPHIES = 9300037,
    SPELL_CAMOUFLAGE = 9300039,
    SPELL_POUNCE_STUN = 20253,          // Intercept stun, triggered by Rocket Pounce effect 1
    // talents 31-60 (plan.json « sorts »)
    SPELL_CANOPY_DROP = 9300063,
    SPELL_CANOPY_DROP_IMPACT = 9300064,
    SPELL_NSIC = 9300066,
    SPELL_NSIC_WINDOW = 9300067,
    SPELL_NSIC_TERROR = 9300069,
    SPELL_NSIC_DREAD = 9300070,
    SPELL_ARC_DISCHARGE = 9300071,
    SPELL_ARC_DISCHARGE_BLAST = 9300072,
    SPELL_BLOOD_FRENZY = 9300074,
    SPELL_FRENZY = 9300075,
    SPELL_FRENZY_PROC = 9300076,
    SPELL_DANGER_SEEKER = 9300077,
    SPELL_DANGER_SEEKER_BUFF = 9300078,
    SPELL_TROPHY_RACK_1 = 9300079,
    SPELL_TROPHY_RACK_2 = 9300080,
    SPELL_TROPHY_HEAL = 9300081,
    SPELL_CLAIM_THE_SKULL = 9300082,
    SPELL_SELF_DESTRUCT = 9300083,
    SPELL_SELF_DESTRUCT_BLAST = 9300084,
    SPELL_CANNON_OFFLINE = 9300085,
    SPELL_SMART_DISC_CUT = 9300130,      // 27/10 16:00: a cut of Smart Disc (one per enemy the disc crosses, per leg)
    SPELL_DISC_RETURN_MISSILE = 9300131, // 27/10 evening: the disc seen flying back to the hand (DUMMY, no effect)
    SPELL_DRONE_REVEAL = 9300132,        // 27/10 evening: Falconer's Eye spotted a hidden enemy (MOD_STALKED, 3 s)
    SPELL_CAMOUFLAGE_MEND = 9300133,     // 27/10 evening: Optical Camouflage mends the Apex (HEAL_PCT, set here)
    SPELL_SKULL_TROPHY = 9300134,        // 28/10: the torn skull and spine in the Apex's hand (hidden dummy aura, 3 s)
    SPELL_RAZOR_DISC_PASS = 9300091,
    SPELL_RAZOR_DISC_RETURN = 9300092,
    SPELL_DISC_OVERCHARGE = 9300093,
    SPELL_DISC_RELEASE = 9300094,
    SPELL_THREE_RED_DOTS = 9300099,
    SPELL_LOCKED_ON = 9300100,
    SPELL_RED_DOTS_MARK = 9300101,
    SPELL_CHARGED_CASTER = 9300103,
    SPELL_PLASMA_SPLASH_DAMAGE = 9300107,
    SPELL_PLASMA_BARRAGE = 9300108,      // node 138407 until 27/10, no node since: taken back at login
    SPELL_SENTRY_PROTOCOL = 9300110,
    SPELL_SENTRY_SHOT = 9300111,
    SPELL_WORTHY_PREY = 9300114,
    // Plasma Frenzy (docs/CONCEPTION-apex-frenesie-canon.md): node 138407 since 27/10, in place of Plasma Barrage
    SPELL_PLASMA_FRENZY = 9300116,
    SPELL_FRENZY_SHOT_ORB = 9300117,     // three shots, same damage, three explosions (orb, ring, mushroom)
    SPELL_FRENZY_SHOT_RING = 9300118,
    SPELL_FRENZY_SHOT_MUSHROOM = 9300119,
};

// Numbers of the plan (docs/PLAN-apex-talents-31-60.md § 4.2).
constexpr int32 BLOOD_FRENZY_EXTENSION_MS = 5000;
constexpr uint32 DANGER_SEEKER_PERIOD_MS = 1000;
constexpr float DANGER_SEEKER_RANGE = 5.0f;
constexpr uint8 DANGER_SEEKER_MAX = 5;
constexpr uint32 ARC_DISCHARGE_COOLDOWN_MS = 30000;
constexpr uint32 SENTRY_LOCK_MS = 8000;
constexpr uint32 SENTRY_SEEN_MS = 60000;
constexpr float NSIC_RADIUS = 10.0f;
constexpr int32 CANOPY_STUN_MS = 5000;
constexpr int32 CANOPY_BASE_STUN_MS = 3000;   // 20253, duration index 27
constexpr float CANOPY_MIN_HEIGHT = 8.0f;
constexpr int32 RED_DOTS_MS = 1500;
constexpr uint32 PLASMA_SPLASH_PCT = 50;
constexpr int32 SELF_DESTRUCT_PCT_PER_TROPHY = 25;
constexpr int32 SELF_DESTRUCT_MAX_HEALTH_PCT = 50;   // 27/10 evening (sponsor): the blast is 50% of the Apex's max health
constexpr int32 SELF_DESTRUCT_HEALTH_PCT = 10;
constexpr uint32 WORTHY_PREY_PCT = 108;
// Optical Camouflage mends (27/10 evening, sponsor): under the camouflage, below 30% of maximum health, 5% of it every
// second until 80%, then nothing until the Apex falls below 30% again (still or newly camouflaged).
constexpr uint32 CAMO_MEND_START_PCT = 30;
constexpr uint32 CAMO_MEND_STOP_PCT = 80;
constexpr uint32 CAMO_MEND_TICK_PCT = 5;
constexpr uint32 CAMO_MEND_PERIOD_MS = 1000;
// Plasma Frenzy: 21 ticks (250 ms over 5 s, one at the start). Damage bounds: 5 enemies a blast, 8 blasts an enemy
// (8 x 105 = 840 at level 60, Plasma Barrage dealt 3 x 288 = 864 to its single target).
constexpr float FRENZY_RANGE = 25.0f;
constexpr float FRENZY_STRAY_MIN = 6.0f;
constexpr float FRENZY_BLAST_RADIUS = 5.0f;             // radius index 8 of the shots
constexpr int32 FRENZY_AIMED_PCT = 70;                  // with an enemy in reach; the rest are stray shots
constexpr int32 FRENZY_MUSHROOM_PCT = 35;               // of the stray shots: the big one lands where nobody stands
constexpr uint8 FRENZY_HITS_PER_ENEMY = 8;
constexpr std::size_t FRENZY_ENEMIES_PER_BLAST = 5;
constexpr uint8 FRENZY_STRAY_TRIES = 4;            // each probe is a collision check (Map::CheckCollisionAndGetValidCoords)
constexpr float FRENZY_SHOT_RANGE = 29.0f;         // SpellRange 4 = 30 m, checked in 3D on the target's CENTRE even when triggered
// Smart Disc and Razor Disc (27/10 16:00, sponsor): one slow disc on a straight line that cuts every enemy it
// crosses, once per leg, through armor, absorption shields and invulnerabilities (Spell.dbc COUPE_PERCANTE and
// AscensionApexDiscSpellInfoScript below). 27/10 evening (sponsor): BOTH discs come back to the hand, wherever the
// Apex stands, and cut each enemy again on the way back.
constexpr float DISC_RANGE = 35.0f;
constexpr float DISC_HALF_WIDTH = 1.5f;              // 3 m blade path, plus the combat reach of the unit
constexpr float DISC_DEFAULT_SPEED = 14.0f;          // Speed of 9300046 / 9300090 (VITESSE_DISQUE), read from the spell
constexpr float DISC_HEIGHT = 1.2f;                  // above the feet of the Apex at the throw
constexpr float DISC_VERTICAL_REACH = 5.0f;          // feet of a unit at most 5 m above or below the disc
constexpr uint32 DISC_TICK_MS = 100;
constexpr float DISC_CATCH_DISTANCE = 2.0f;          // back in the hand
constexpr uint32 DISC_RETURN_GRACE_MS = 4000;        // a fleeing Apex is not chased for ever: the disc is then lost
constexpr float VOICE_STOP_DISTANCE = 3.0f;
constexpr Milliseconds VOICE_HOLD = 4s;
constexpr Milliseconds GRAPPLE_SILENCE = 3s;
constexpr uint32 POINT_VOICE = 0x41504501;
constexpr uint32 POINT_VOICE_HOME = 0x41504502;
constexpr uint32 POINT_DRONE_HOVER = 0x41504503;

bool IsTinker(Unit const* unit)
{
    return unit && unit->IsPlayer() && unit->getClass() == CLASS_TINKER;
}

/// Per-character state of the talents, in the character's own CustomData: read and written from the hooks of
/// its map only (one map thread per player), never shared.
struct ApexTalentState : public DataMap::Base
{
    uint32 dangerSeekerTimer = 0;
    uint32 arcDischargeAt = 0;                              // getMSTime of the last discharge, 0 never
    bool arcDischargeUsed = false;
    std::unordered_set<ObjectGuid> ripostes;                // Never Saw It Coming: who hit the Apex in the window
    std::unordered_map<ObjectGuid, uint32> sentryLock;      // Sentry Protocol: last shot per target
    std::unordered_map<ObjectGuid, uint32> sentrySeen;      // Sentry Protocol: first hit of a player
    std::unordered_map<ObjectGuid, uint8> frenzyHits;       // Plasma Frenzy: blasts per enemy, emptied at each start
    bool camoMending = false;                               // Optical Camouflage: mending, until 80%
    uint32 camoMendTimer = 0;                               // ms until the next 5%
};

ApexTalentState& State(Player* player)
{
    return *player->CustomData.GetDefault<ApexTalentState>("coa.apex.talents");
}

/// Drops the entries older than `ttl`; called before an insert, the maps stay a handful of entries long.
void Prune(std::unordered_map<ObjectGuid, uint32>& stamps, uint32 now, uint32 ttl)
{
    for (auto itr = stamps.begin(); itr != stamps.end();)
        itr = getMSTimeDiff(itr->second, now) >= ttl ? stamps.erase(itr) : std::next(itr);
}

/// Casting from inside a damage, combat or aura hook would re-enter Unit::DealDamage or the aura lists: the
/// cast runs at the next update of the Apex instead (its own event list: the event dies with the player).
template <typename Fn>
void Defer(Player* player, Fn&& fn)
{
    player->m_Events.AddEventAtOffset([player, fn = std::forward<Fn>(fn)]() mutable
    {
        if (player->IsInWorld())
            fn(player);
    }, 1ms);
}

/// Same test as Trophy Hunter v2 (AscensionApexTrophyScript::WorthyPrey): a player (bots included), an elite or a
/// rare. Pets, totems and critters never.
bool IsWorthyPrey(Unit const* unit)
{
    if (!unit)
        return false;
    if (unit->IsPlayer())
        return true;
    Creature const* creature = unit->ToCreature();
    if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsCritter())
        return false;
    return creature->isElite() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_RARE;
}

/// True when the Apex already fights someone other than `newcomer` (or the newcomer's master).
bool EngagedWithAnother(Player* apex, Unit* newcomer)
{
    ObjectGuid const self = newcomer->GetGUID();
    ObjectGuid const master = newcomer->GetCharmerOrOwnerGUID();
    auto other = [&](ObjectGuid const& guid) { return guid != self && (!master || guid != master); };
    CombatManager const& combat = apex->GetCombatManager();
    for (auto const& [guid, ref] : combat.GetPvECombatRefs())
        if (other(guid))
            return true;
    for (auto const& [guid, ref] : combat.GetPvPCombatRefs())
        if (other(guid))
            return true;
    return false;
}

void CastDeferred(Player* apex, ObjectGuid targetGuid, uint32 spellId)
{
    Defer(apex, [targetGuid, spellId](Player* player)
    {
        if (!player->IsAlive())
            return;
        Unit* target = targetGuid == player->GetGUID() ? player : ObjectAccessor::GetUnit(*player, targetGuid);
        if (!target || !target->IsAlive() || (target != player && !player->IsValidAttackTarget(target)))
            return;
        player->CastSpell(target, spellId, true);
    });
}

// ---- Arc Discharge (redefined 27/10): the camouflage short-circuits when it is pierced ----

void TryArcDischarge(Player* apex)
{
    if (!apex->HasAura(SPELL_ARC_DISCHARGE) || !sSpellMgr->GetSpellInfo(SPELL_ARC_DISCHARGE_BLAST))
        return;
    ApexTalentState& state = State(apex);
    uint32 const now = getMSTime();
    if (state.arcDischargeUsed && getMSTimeDiff(state.arcDischargeAt, now) < ARC_DISCHARGE_COOLDOWN_MS)
        return;
    state.arcDischargeUsed = true;
    state.arcDischargeAt = now;
    CastDeferred(apex, apex->GetGUID(), SPELL_ARC_DISCHARGE_BLAST);
}

// ---- Sentry Protocol ----

void TrySentry(Player* apex, Unit* newcomer)
{
    if (!apex->IsAlive() || !newcomer->IsAlive() || !apex->HasAura(SPELL_SENTRY_PROTOCOL) ||
        !EngagedWithAnother(apex, newcomer) || !sSpellMgr->GetSpellInfo(SPELL_SENTRY_SHOT))
        return;
    ApexTalentState& state = State(apex);
    uint32 const now = getMSTime();
    auto itr = state.sentryLock.find(newcomer->GetGUID());
    if (itr != state.sentryLock.end() && getMSTimeDiff(itr->second, now) < SENTRY_LOCK_MS)
        return;
    Prune(state.sentryLock, now, SENTRY_LOCK_MS);
    state.sentryLock[newcomer->GetGUID()] = now;
    CastDeferred(apex, newcomer->GetGUID(), SPELL_SENTRY_SHOT);
}

// ---- Self-Destruct Protocol ----

void SelfDestruct(Player* apex)
{
    if (!apex->IsAlive())
        return;
    SpellInfo const* blast = sSpellMgr->GetSpellInfo(SPELL_SELF_DESTRUCT_BLAST);
    if (!blast)
        return;
    uint32 const trophies = apex->GetAuraCount(SPELL_TROPHIES);
    // 27/10 evening (sponsor: « 20 Fire is ridiculous »): 50% of the Apex's MAXIMUM health, read now (buffs included),
    // then the Trophy bonus of the talent on top (+25% per Trophy consumed, kept). 64-bit until the clamp.
    int64 const base = int64(apex->CountPctFromMaxHealth(SELF_DESTRUCT_MAX_HEALTH_PCT));
    int64 const damage = base * (100 + SELF_DESTRUCT_PCT_PER_TROPHY * int64(trophies)) / 100;
    // Spell.dbc gives the blast no die and no per-level part (outils/apex-spe-dbc.py, « explosion »): CalcValue applies
    // the amount as passed. The per-level part, if a later DBC brought one back, is still taken off (CalcValue adds it
    // again on top of a custom base point, review of 27/10).
    int64 const levelPart = int64(blast->Effects[EFFECT_0].CalcValue(apex)) - blast->Effects[EFFECT_0].CalcValue(nullptr);
    int32 const amount = int32(std::clamp<int64>(damage - levelPart, 1, std::numeric_limits<int32>::max()));
    apex->CastCustomSpell(SPELL_SELF_DESTRUCT_BLAST, SPELLVALUE_BASE_POINT0, amount, apex, true);
    apex->RemoveAurasDueToSpell(SPELL_TROPHIES);
    // Decision D-5 of the plan, left to the sponsor: the health is SET to 10% (a 3% Apex is healed to 10%).
    if (apex->IsAlive())
        apex->SetHealth(std::max<uint32>(1, apex->CountPctFromMaxHealth(SELF_DESTRUCT_HEALTH_PCT)));
    if (sSpellMgr->GetSpellInfo(SPELL_CANNON_OFFLINE))
        apex->AddAura(SPELL_CANNON_OFFLINE, apex);
}

// ---- Optical Camouflage mends the Apex ----

/// Once per player update while the camouflage holds (OnPlayerUpdate). Starts below CAMO_MEND_START_PCT, then heals
/// CAMO_MEND_TICK_PCT of the maximum health every second (the first at once) until CAMO_MEND_STOP_PCT; the last tick
/// only tops up to it. The heal is a triggered cast of 9300133 (HEAL_PCT): healing taken modifiers apply (Mortal
/// Strike and the like reduce it, a unit that cannot be healed is not), it shows in the combat log, it breaks no
/// stealth (triggered: Spell::prepare skips the CAST interrupt flags; ATTR1 0x20 too) and gives no threat (ATTR1
/// 0x400). Nothing outside the camouflage: OnAuraRemove of the camouflage stops it (broken, cancelled, death).
void CamouflageMend(Player* apex, uint32 diff)
{
    if (!apex->IsAlive() || !apex->HasAura(SPELL_CAMOUFLAGE))
        return;
    ApexTalentState& state = State(apex);
    uint32 const maxHealth = apex->GetMaxHealth();
    if (!maxHealth)
        return;
    uint64 const health = apex->GetHealth();
    if (!state.camoMending)
    {
        if (health * 100 >= uint64(maxHealth) * CAMO_MEND_START_PCT)
            return;
        state.camoMending = true;
        state.camoMendTimer = 0;
    }
    uint64 const stop = uint64(maxHealth) * CAMO_MEND_STOP_PCT / 100;
    if (health >= stop)
    {
        state.camoMending = false;
        return;
    }
    if (state.camoMendTimer > diff)
    {
        state.camoMendTimer -= diff;
        return;
    }
    state.camoMendTimer = CAMO_MEND_PERIOD_MS;
    if (!sSpellMgr->GetSpellInfo(SPELL_CAMOUFLAGE_MEND))
        return;
    // Percent left to 80%, rounded up, between 1 and 5.
    uint64 const missingPct = ((stop - health) * 100 + maxHealth - 1) / maxHealth;
    int32 const pct = int32(std::clamp<uint64>(missingPct, 1, CAMO_MEND_TICK_PCT));
    apex->CastCustomSpell(SPELL_CAMOUFLAGE_MEND, SPELLVALUE_BASE_POINT0, pct, apex, true);
}

// ---- Plasma Frenzy: the cannon fires wildly, at enemies and at the ground ----

/// True when `unit` already fights the Apex or a member of its group/raid (a neutral the group is killing counts).
bool FightsApexSide(Player* apex, Unit* unit)
{
    if (unit->IsInCombatWith(apex))
        return true;
    Unit* victim = unit->GetVictim();
    Player* side = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return side && apex->IsInSameRaidWith(side) && unit->IsInCombatWith(side);
}

/// One tick of Plasma Frenzy: a shot at a random prey in reach (FRENZY_AIMED_PCT of the time when one is there),
/// otherwise a stray shot at a random point of the ground between 6 and 25 m. Prey = attackable, seen, in line of
/// sight, under its cap of blasts, and either a player (PvP rules: IsValidAttackTarget) or a creature that already
/// fights the Apex's side (review 27/10: a hostile pack at rest is NOT shot at, the frenzy pulls nothing in a
/// dungeon). Every other attackable unit around (a creature at rest, hostile or neutral) is spared: no shot, aimed or
/// stray, lands within blast reach of it (the blasts hit like Plasma Splash, TARGET_UNIT_DEST_AREA_ENEMY, so only the
/// choice of the point spares them). Critters are neither (AnyUnfriendlyUnitInObjectRangeCheck leaves them out): a
/// stray blast may hit one. Triggered casts (TRIGGERED_FULL_MASK): no cost, no cooldown, no proc of Plasma Splash
/// (the shots carry no family flag).
void FirePlasmaFrenzyShot(Player* apex)
{
    float const scan = FRENZY_RANGE + FRENZY_BLAST_RADIUS + 2.0f;
    std::list<Unit*> around;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(apex, apex, scan);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(apex, around, check);
    Cell::VisitObjects(apex, searcher, scan);

    ApexTalentState& state = State(apex);
    std::vector<Unit*> prey;
    std::vector<Unit*> spared;
    for (Unit* unit : around)
    {
        if (unit == apex || !unit->IsAlive() || !apex->IsValidAttackTarget(unit))
            continue;
        if (!unit->IsPlayer() && !FightsApexSide(apex, unit))
        {
            spared.push_back(unit);
            continue;
        }
        if (!apex->IsWithinDist(unit, FRENZY_RANGE) || apex->GetExactDist(unit) > FRENZY_SHOT_RANGE ||
            !apex->CanSeeOrDetect(unit) || !apex->IsWithinLOSInMap(unit))
            continue;
        auto const hits = state.frenzyHits.find(unit->GetGUID());
        if (hits != state.frenzyHits.end() && hits->second >= FRENZY_HITS_PER_ENEMY)
            continue;
        prey.push_back(unit);
    }
    auto const nearBystander = [&spared](Position const& point)
    {
        return std::any_of(spared.begin(), spared.end(), [&point](Unit const* unit)
        {
            return unit->GetExactDist(&point) < FRENZY_BLAST_RADIUS + unit->GetCombatReach() + 1.0f;
        });
    };
    prey.erase(std::remove_if(prey.begin(), prey.end(), [&nearBystander](Unit const* unit)
    {
        return nearBystander(unit->GetPosition());
    }), prey.end());

    uint32 const blast = urand(0, 1) ? SPELL_FRENZY_SHOT_ORB : SPELL_FRENZY_SHOT_RING;
    if (!prey.empty() && roll_chance_i(FRENZY_AIMED_PCT))
    {
        Unit* target = Acore::Containers::SelectRandomContainerElement(prey);
        apex->CastSpell(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), blast, true);
        return;
    }
    for (uint8 i = 0; i < FRENZY_STRAY_TRIES; ++i)
    {
        Position point = apex->GetPosition();
        // First collision on the way (walls, slopes), ground height included; a failed probe leaves the point on the
        // Apex, refused below.
        apex->MovePositionToFirstCollision(point, frand(FRENZY_STRAY_MIN, FRENZY_RANGE), frand(0.0f, 2.0f * float(M_PI)));
        if (apex->GetExactDist2d(&point) < FRENZY_STRAY_MIN * 0.5f)
            continue;
        if (nearBystander(point))
            continue;
        apex->CastSpell(point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(),
                        roll_chance_i(FRENZY_MUSHROOM_PCT) ? SPELL_FRENZY_SHOT_MUSHROOM : blast, true);
        return;
    }
}

// ---- Smart Disc and Razor Disc: one slow disc that cuts through everything on its line ----

/// A disc in flight. Owned by the events of its Apex (Player::m_Events, run on the Apex's map thread): it dies with
/// the player and stops on a change of map or instance, a death or a logout (the last sweep is simply not done).
struct ApexDiscFlight
{
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    float StartX = 0.0f, StartY = 0.0f, StartZ = 0.0f;
    float DirX = 1.0f, DirY = 0.0f;
    float Slope = 0.0f;                     // height gained per metre: the line goes through the aimed unit's chest
    float Length = 0.0f;                    // 2D, from the hand to the end of the line (or the first wall)
    float Speed = DISC_DEFAULT_SPEED;
    ObjectGuid Aim;                         // the enemy aimed at: never spared
    uint32 CutSpell = 0;
    uint32 ReturnSpell = 0;                 // the disc flies back to the hand, cutting again (0: it stops at the end)
    bool Returning = false;
    float X = 0.0f, Y = 0.0f, Z = 0.0f;     // where the disc was at the last sweep (Z: kept on the way back only)
    uint32 LaunchMs = 0;
    uint32 LastMs = 0;
    uint32 MaxMs = 0;
    std::unordered_set<ObjectGuid> HitOut;  // once per leg: an enemy is cut once on the way out, once on the way back
    std::unordered_set<ObjectGuid> HitBack;
};

/// Height of the disc's line `along` metres from the hand.
float DiscHeightAt(ApexDiscFlight const& flight, float along)
{
    return flight.StartZ + flight.Slope * along;
}

/// Plans the line of a throw: from the hand towards the aimed unit (the facing without one), through its chest
/// height (slope bounded to 45 degrees: a target on a ledge or below a bridge is reached), DISC_RANGE long and cut
/// short by the first wall, building or gameobject (vmaps, LOS checks of spells: trees and other M2 doodads are
/// ignored; the terrain is not tested, the disc flies over a ridge). Returns the end of the line, where the client
/// sends the disc.
Position PlanDiscFlight(Player* apex, Unit* aim, ApexDiscFlight& flight)
{
    float const angle = aim && aim != apex ? apex->GetAngle(aim) : apex->GetOrientation();
    flight.MapId = apex->GetMapId();
    flight.InstanceId = apex->GetInstanceId();
    flight.StartX = apex->GetPositionX();
    flight.StartY = apex->GetPositionY();
    flight.StartZ = apex->GetPositionZ() + DISC_HEIGHT;
    flight.DirX = std::cos(angle);
    flight.DirY = std::sin(angle);
    flight.Aim = aim ? aim->GetGUID() : ObjectGuid::Empty;
    flight.Slope = 0.0f;
    if (aim && aim != apex)
        if (float const flat = apex->GetExactDist2d(aim); flat > 1.0f)
            flight.Slope = std::clamp((aim->GetPositionZ() + aim->GetCollisionHeight() * 0.5f - flight.StartZ) / flat,
                                      -1.0f, 1.0f);
    Map* map = apex->GetMap();
    auto const clear = [&](float length)
    {
        return map->isInLineOfSight(flight.StartX, flight.StartY, flight.StartZ, flight.StartX + flight.DirX * length,
                                    flight.StartY + flight.DirY * length, DiscHeightAt(flight, length),
                                    apex->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
    };
    // Spell::CheckRange accepts the aimed unit a little beyond 35 m (combat reaches, leeway): the line reaches it.
    float const reach = aim && aim != apex ? apex->GetExactDist2d(aim) + aim->GetCombatReach() : 0.0f;
    float length = std::max(DISC_RANGE, reach);
    if (!clear(length))
    {
        float open = 0.0f, blocked = length;       // 6 halvings: the wall is found to 0.7 m at most
        for (uint8 i = 0; i < 6; ++i)
        {
            float const middle = (open + blocked) * 0.5f;
            (clear(middle) ? open : blocked) = middle;
        }
        length = open;
    }
    flight.Length = std::max(length, 0.5f);
    flight.X = flight.StartX;
    flight.Y = flight.StartY;
    flight.Z = flight.StartZ;
    return Position(flight.StartX + flight.DirX * flight.Length, flight.StartY + flight.DirY * flight.Length,
                    DiscHeightAt(flight, flight.Length), angle);
}

/// Body of a unit across the disc's path from (AX, AY) to (AX + DX, AY + DY), feet within DISC_VERTICAL_REACH of Z (the
/// ground under the disc at the throw: disc height minus DISC_HEIGHT). Geometry only; the rules of the hit are in
/// SweepDisc.
struct DiscPathCheck
{
    float AX, AY, DX, DY, LengthSq, Z;

    bool operator()(Unit* unit) const
    {
        if (!unit || !unit->IsAlive() || std::fabs(unit->GetPositionZ() - Z) > DISC_VERTICAL_REACH)
            return false;
        float const ux = unit->GetPositionX() - AX;
        float const uy = unit->GetPositionY() - AY;
        // Behind the start of the segment: already tested by the previous one, or behind the hand at the throw.
        float const projected = LengthSq > 0.0f ? (ux * DX + uy * DY) / LengthSq : 0.0f;
        if (projected < 0.0f)
            return false;
        float const along = std::min(projected, 1.0f);
        float const ox = ux - DX * along;
        float const oy = uy - DY * along;
        float const reach = DISC_HALF_WIDTH + unit->GetCombatReach();
        return ox * ox + oy * oy <= reach * reach;
    }
};

/// Cuts every enemy whose body the disc crossed from (fromX, fromY) to (toX, toY), at height z, once per leg.
/// An enemy: a unit the Apex may attack (IsValidAttackTarget: friends, neutral guards of a sanctuary, own pets,
/// players outside PvP rules never), that it sees or detects, in line of sight of the DISC (not of the Apex). Spared:
/// a creature at rest that is not hostile (a neutral critter or a yellow beast on the way is not pulled), unless it
/// is the aimed unit; and a creature that no player controls under a damage immunity (scripted invulnerable phases,
/// raid adds, event creatures stay invulnerable: the piercing is meant for players and their pets, Divine Shield,
/// Ice Block...; review of 27/10).
void SweepDisc(Player* apex, ApexDiscFlight const& flight, float fromX, float fromY, float toX, float toY, float z,
               std::unordered_set<ObjectGuid>& hit, uint32 spellId)
{
    DiscPathCheck const check{ fromX, fromY, toX - fromX, toY - fromY,
                               (toX - fromX) * (toX - fromX) + (toY - fromY) * (toY - fromY), z - DISC_HEIGHT };
    float const radius = std::sqrt(check.LengthSq) * 0.5f + DISC_HALF_WIDTH + 10.0f;   // 10: widest combat reach searched
    std::list<Unit*> around;
    Acore::UnitListSearcher<DiscPathCheck const> searcher(apex, around, check);
    Cell::VisitObjects((fromX + toX) * 0.5f, (fromY + toY) * 0.5f, apex->GetMap(), searcher, radius);

    std::vector<ObjectGuid> cuts;
    for (Unit* unit : around)
    {
        if (unit == apex || hit.count(unit->GetGUID()) || !apex->IsValidAttackTarget(unit) || !apex->CanSeeOrDetect(unit))
            continue;
        if (Creature* creature = unit->ToCreature())
        {
            if (unit->GetGUID() != flight.Aim && !creature->IsInCombat() && !creature->IsHostileTo(apex))
                continue;
            if (!creature->IsCharmedOwnedByPlayerOrPlayer() &&
                creature->IsImmunedToDamageOrSchool(SPELL_SCHOOL_MASK_NORMAL))
                continue;
        }
        if (!apex->GetMap()->isInLineOfSight(toX, toY, z, unit->GetPositionX(), unit->GetPositionY(),
                                             unit->GetPositionZ() + unit->GetCollisionHeight() * 0.5f,
                                             apex->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2))
            continue;
        hit.insert(unit->GetGUID());
        cuts.push_back(unit->GetGUID());
    }
    // Cast after the walk over the grid: a cut may kill (Unit::Kill) and the next one find its target gone.
    for (ObjectGuid const& guid : cuts)
        if (Unit* unit = ObjectAccessor::GetUnit(*apex, guid); unit && unit->IsAlive())
            apex->CastSpell(unit, spellId, true);
}

void ScheduleDiscTick(Player* apex, std::shared_ptr<ApexDiscFlight> flight);

constexpr Milliseconds DISC_RETURN_MISSILE_DELAY = 100ms;   // the client must have the trigger before its missile

/// The disc seen flying back (27/10 evening): an invisible trigger (WORLD_TRIGGER, the Apex's faction) left where the
/// disc turned sends the disc's missile (9300131: DUMMY, no effect, the throw's visual and speed) at the Apex. The
/// client follows a missile aimed at a unit, as the server's return follows the Apex. Visual only: the cuts of the
/// return are SweepDisc's. The trigger lives as long as the longest return (MaxMs), plus a second.
void ShowDiscReturn(Player* apex, ApexDiscFlight const& flight, float z, uint32 lifeMs)
{
    if (!sSpellMgr->GetSpellInfo(SPELL_DISC_RETURN_MISSILE))
        return;
    Creature* trigger = apex->SummonTrigger(flight.X, flight.Y, z, 0.0f, lifeMs + 1000, true);
    if (!trigger)
        return;
    ObjectGuid const apexGuid = apex->GetGUID();
    trigger->m_Events.AddEventAtOffset([trigger, apexGuid]()
    {
        Player* target = ObjectAccessor::GetPlayer(*trigger, apexGuid);
        if (target && target->IsAlive())
            trigger->CastSpell(target, SPELL_DISC_RETURN_MISSILE, true);
    }, DISC_RETURN_MISSILE_DELAY);
}

/// One step of a disc: out along its line at its speed (time measured from the throw, so the cuts follow the
/// missile the client draws), then back to the hand at the same speed, wherever the Apex is (Smart Disc and Razor
/// Disc alike since 27/10 evening).
void DiscTick(Player* apex, std::shared_ptr<ApexDiscFlight> const& flight)
{
    if (!apex->IsInWorld() || !apex->IsAlive() || apex->GetMapId() != flight->MapId ||
        apex->GetInstanceId() != flight->InstanceId)
        return;
    uint32 const now = getMSTime();
    if (!flight->Returning)
    {
        // The client flies the missile in 3D to the destination (Spell::CalculateDelayMomentForDst): on a slope the
        // flat progress is the flown distance divided by sqrt(1 + slope^2).
        float const along = std::min(flight->Length, flight->Speed * float(getMSTimeDiff(flight->LaunchMs, now)) /
                                                         1000.0f / std::sqrt(1.0f + flight->Slope * flight->Slope));
        float const x = flight->StartX + flight->DirX * along;
        float const y = flight->StartY + flight->DirY * along;
        SweepDisc(apex, *flight, flight->X, flight->Y, x, y, DiscHeightAt(*flight, along), flight->HitOut,
                  flight->CutSpell);
        flight->X = x;
        flight->Y = y;
        flight->LastMs = now;
        if (along < flight->Length)
        {
            ScheduleDiscTick(apex, flight);
            return;
        }
        if (!flight->ReturnSpell || !sSpellMgr->GetSpellInfo(flight->ReturnSpell))
            return;
        flight->Returning = true;
        flight->Z = DiscHeightAt(*flight, flight->Length);   // the return starts at the height the disc turned
        uint32 const returnMs = uint32(flight->Length / flight->Speed * 1000.0f) + DISC_RETURN_GRACE_MS;
        flight->MaxMs = getMSTimeDiff(flight->LaunchMs, now) + returnMs;
        ShowDiscReturn(apex, *flight, DiscHeightAt(*flight, flight->Length), returnMs);
        ScheduleDiscTick(apex, flight);
        return;
    }
    float const step = flight->Speed * float(getMSTimeDiff(flight->LastMs, now)) / 1000.0f;
    float const dx = apex->GetPositionX() - flight->X;
    float const dy = apex->GetPositionY() - flight->Y;
    float const distance = std::sqrt(dx * dx + dy * dy);
    bool const caught = distance <= step + DISC_CATCH_DISTANCE;
    float const x = caught ? apex->GetPositionX() : flight->X + dx / distance * step;
    float const y = caught ? apex->GetPositionY() : flight->Y + dy / distance * step;
    // The height goes from where the disc turned to the Apex's hand in proportion to the flat way flown (review of
    // 27/10 evening: swept at the hand's height all along, a return from a ledge or a slope missed what it crossed).
    float const hand = apex->GetPositionZ() + DISC_HEIGHT;
    float const z = caught ? hand : flight->Z + (hand - flight->Z) * std::min(1.0f, step / distance);
    SweepDisc(apex, *flight, flight->X, flight->Y, x, y, z, flight->HitBack, flight->ReturnSpell);
    flight->X = x;
    flight->Y = y;
    flight->Z = z;
    flight->LastMs = now;
    if (!caught && getMSTimeDiff(flight->LaunchMs, now) < flight->MaxMs)
        ScheduleDiscTick(apex, flight);
}

void ScheduleDiscTick(Player* apex, std::shared_ptr<ApexDiscFlight> flight)
{
    apex->m_Events.AddEventAtOffset([apex, flight]() { DiscTick(apex, flight); }, Milliseconds(DISC_TICK_MS));
}

/// Throws the planned disc: the first sweep runs one tick after the throw.
void LaunchDisc(Player* apex, ApexDiscFlight const& planned, float speed, uint32 cutSpell, uint32 returnSpell)
{
    if (!sSpellMgr->GetSpellInfo(cutSpell))
        return;
    auto flight = std::make_shared<ApexDiscFlight>(planned);
    flight->Speed = speed > 0.0f ? speed : DISC_DEFAULT_SPEED;
    flight->CutSpell = cutSpell;
    flight->ReturnSpell = returnSpell;
    flight->LaunchMs = flight->LastMs = getMSTime();
    flight->MaxMs = uint32(flight->Length * std::sqrt(1.0f + flight->Slope * flight->Slope) / flight->Speed * 1000.0f) +
        DISC_TICK_MS;
    ScheduleDiscTick(apex, flight);
}

// ---- Skull and Spine (28/10, docs/ETUDE-apex-skull-spine-animation.md § 5-6: « the body becomes its bones ») ----
// The killed one's body bursts (flesh and a spray of bones, no animation forced), and 0.3 s later, inside the burst,
// the body itself takes the display of a lying skeleton of its kind, scaled to its size. One object: the loot, the
// skinning, the « Dead » tooltip and the normal decay stay the body's. Creature::Respawn gives the native model and
// scale back (Creature.cpp:2097-2105, skipped while a transform aura is on: hence no bones on a transformed body).
// The lying skeletons are doodad M2s given a CreatureDisplayInfo (CDI = CMD = same id, CDI scale 1.0) by the Apex client
// patch; Length = length of the lying skeleton along the M2's X axis, in m (sortie-apex/etude-skull-spine/
// deathskeletons.json, § 5 R3; sortie-apex/gisants/gisants.json may correct them: tune here).
struct ApexBoneBed
{
    uint32 Display;
    float Length;
};

constexpr ApexBoneBed BONE_BEDS_UPRIGHT[] =   // humanoid, undead, demon, giant: chosen by the prey's HEIGHT
{
    { 9300001, 1.13f },   // Gnome M
    { 9300002, 1.31f },   // Goblin M
    { 9300003, 1.85f },   // Dwarf M
    { 9300004, 2.25f },   // Human M
    { 9300005, 2.43f },   // Orc M
    { 9300006, 2.66f },   // Tauren M
    { 9300007, 5.06f },   // Vrykul M
};
constexpr ApexBoneBed BONE_BED_COW = { 9300008, 5.69f };       // BlastedLandsBlastedCow: a stocky quadruped
constexpr ApexBoneBed BONE_BED_MULE = { 9300009, 5.82f };      // BlastedLandsBlastedMule: a slender quadruped
constexpr ApexBoneBed BONE_BED_REPTILE = { 9300010, 5.77f };   // DragonFootSoldierBones: tail, reptile or dragonkin
constexpr uint32 BONE_BED_FIRST = 9300001;
constexpr uint32 BONE_BED_LAST = 9300010;

constexpr uint32 KIT_FLESH_BURST = 11644;   // Ghoul_Explode (effect 4282), AnimID -1: the body is not stood up
constexpr uint32 KIT_BONE_SPRAY = 11245;    // WorldEffect 4260 DeathKnight_CorpseExplosion (x2), StartAnim/Anim -1
constexpr Milliseconds BONE_BED_DELAY = 300ms;    // the bones come in the burst's cloud
constexpr Milliseconds BARE_BODY_LIFE = 45s;      // a body with nothing to loot nor skin: its bones lie 45 s
constexpr float BONE_BED_MIN_SCALE = 0.35f;
constexpr float BONE_BED_MAX_SCALE = 3.0f;
// Fallback when the display is not in coa_apex_display_taille: UNIT_FIELD_COMBATREACH (model's combat_reach x object
// scale, Creature::SetDisplayId) times this. Combat reach is 1.5 for most humanoid and beast models, for about 2 m
// of height or length; the bounding radius is a half width, the wrong axis. [S] a rough rule, bounded by the clamp.
constexpr float SIZE_PER_COMBAT_REACH = 1.3f;

struct ApexPreySize
{
    float Length;
    float Height;
};

/// coa_apex_display_taille (world database), by display id: written once at startup (AscensionApexSkullSpineWorldScript,
/// before any map updates), only read afterwards from the map threads.
std::unordered_map<uint32, ApexPreySize> ApexPreySizes;

void LoadApexPreySizes()
{
    ApexPreySizes.clear();
    if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND "
                             "TABLE_NAME = 'coa_apex_display_taille'"))
    {
        LOG_INFO("module", "apex skull and spine: no table coa_apex_display_taille, sizes from the combat reach");
        return;
    }
    uint32 rejected = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT display_id, longueur, hauteur FROM coa_apex_display_taille"))
    {
        do
        {
            Field* fields = result->Fetch();
            float const length = fields[1].Get<float>();
            float const height = fields[2].Get<float>();
            if (!std::isfinite(length) || !std::isfinite(height) || length < 0.0f || height < 0.0f)
            {
                ++rejected;
                continue;
            }
            ApexPreySizes[fields[0].Get<uint32>()] = { length, height };
        } while (result->NextRow());
    }
    LOG_INFO("module", "apex skull and spine: {} display sizes loaded, {} rejected", ApexPreySizes.size(), rejected);
}

/// Creature types with a skeleton (CreatureType, SharedDefines.h). None: elemental, critter, mechanical, not specified,
/// totem, non-combat pet, gas cloud; their body only bursts.
bool HasSkeleton(uint32 creatureType)
{
    switch (creatureType)
    {
        case CREATURE_TYPE_BEAST:
        case CREATURE_TYPE_DRAGONKIN:
        case CREATURE_TYPE_DEMON:
        case CREATURE_TYPE_GIANT:
        case CREATURE_TYPE_UNDEAD:
        case CREATURE_TYPE_HUMANOID:
            return true;
        default:
            return false;
    }
}

/// The lying skeleton of a creature's kind, nullptr for none. `upright`: the prey's size is its height (else its length).
ApexBoneBed const* ChooseBoneBed(Creature const* body, bool& upright)
{
    upright = false;
    uint32 const type = body->GetCreatureType();
    if (!HasSkeleton(type))
        return nullptr;
    if (type == CREATURE_TYPE_DRAGONKIN)
        return &BONE_BED_REPTILE;
    if (type != CREATURE_TYPE_BEAST)
    {
        upright = true;
        return nullptr;   // the caller picks by height among BONE_BEDS_UPRIGHT
    }
    switch (body->GetCreatureTemplate()->family)
    {
        case CREATURE_FAMILY_CROCOLISK:
        case CREATURE_FAMILY_RAPTOR:
        case CREATURE_FAMILY_TURTLE:
        case CREATURE_FAMILY_WIND_SERPENT:
        case CREATURE_FAMILY_DRAGONHAWK:
        case CREATURE_FAMILY_SERPENT:
        case CREATURE_FAMILY_CHIMAERA:
        case CREATURE_FAMILY_DEVILSAUR:
        case CREATURE_FAMILY_WORM:
            return &BONE_BED_REPTILE;
        case CREATURE_FAMILY_WOLF:
        case CREATURE_FAMILY_CAT:
        case CREATURE_FAMILY_TALLSTRIDER:
        case CREATURE_FAMILY_HYENA:
        case CREATURE_FAMILY_SPIRIT_BEAST:
            return &BONE_BED_MULE;
        default:
            return &BONE_BED_COW;   // bear, boar, spider, insects, no family...
    }
}

/// The prey's size in m: the table's value for its NATIVE display (the CDI's own scale included) times the object
/// scale (OBJECT_FIELD_SCALE_X = creature_template_model.DisplayScale, Creature::InitEntry; the client multiplies
/// both). Fallback: the combat reach, which carries the object scale already.
float PreySize(Creature const* body, bool upright)
{
    if (auto itr = ApexPreySizes.find(body->GetNativeDisplayId()); itr != ApexPreySizes.end())
    {
        float const size = (upright ? itr->second.Height : itr->second.Length) * body->GetObjectScale();
        if (std::isfinite(size) && size > 0.0f)
            return size;
    }
    return body->GetCombatReach() * SIZE_PER_COMBAT_REACH;
}

/// 0.3 s after the burst: the body becomes its bones. Run from the map's events, so neither the Apex nor the body need
/// to exist any more: the body is looked up again, and left alone if it is gone, alive again (respawned: same GUID),
/// no longer a corpse, or no longer shows the display it died with (already bones, a script or an aura changed it).
void LayBoneBed(Map* map, ObjectGuid bodyGuid, uint32 deathDisplay)
{
    Creature* body = map->GetCreature(bodyGuid);
    if (!body || !body->IsInWorld() || body->getDeathState() != DeathState::Corpse ||
        body->GetDisplayId() != deathDisplay || !body->GetAuraEffectsByType(SPELL_AURA_TRANSFORM).empty())
        return;
    bool upright = false;
    ApexBoneBed const* bed = ChooseBoneBed(body, upright);
    if (!bed && !upright)
        return;
    float const size = PreySize(body, upright);
    if (!bed)
    {
        bed = &BONE_BEDS_UPRIGHT[0];
        for (ApexBoneBed const& candidate : BONE_BEDS_UPRIGHT)
            if (std::fabs(candidate.Length - size) < std::fabs(bed->Length - size))
                bed = &candidate;
    }
    float scale = size / bed->Length;
    if (!std::isfinite(scale))
        scale = 1.0f;
    body->SetDisplayId(bed->Display, std::clamp(scale, BONE_BED_MIN_SCALE, BONE_BED_MAX_SCALE));
    if (!body->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) && !body->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        body->DespawnOrUnsummon(BARE_BODY_LIFE);
}

/// A killing blow of Skull and Spine: the body bursts at once; 0.3 s later it lies as its skeleton, and the Apex holds
/// the torn skull and spine (9300134, 3 s, hidden dummy aura carrying the visual). Kept as they are, burst only: a
/// player's body (where the ghost comes back, what a resurrection targets), a pet or guardian (its owner's), and a
/// dungeon or world boss (encounter scripts may still use the corpse). Nothing for a totem or a critter.
void TearOutSkeleton(Player* apex, Unit* victim)
{
    if (!victim || !victim->IsInWorld() || victim->IsAlive())
        return;
    Creature* body = victim->ToCreature();
    if (body && (body->IsTotem() || body->IsCritter()))
        return;
    uint32 const deathDisplay = victim->GetDisplayId();
    if (deathDisplay >= BONE_BED_FIRST && deathDisplay <= BONE_BED_LAST)
        return;   // already its bones (a second Skull and Spine on the same body)
    victim->SendPlaySpellVisual(KIT_FLESH_BURST);
    victim->SendPlaySpellVisual(KIT_BONE_SPRAY);
    bool const trophy = HasSkeleton(victim->GetCreatureType());
    bool const bones = body && !(body->IsPet() || body->IsGuardian() || body->IsCharmedOwnedByPlayerOrPlayer() ||
                                 body->IsDungeonBoss() || body->isWorldBoss());
    if (!trophy && !bones)
        return;
    Map* map = victim->GetMap();
    map->Events.AddEventAtOffset([map, apexGuid = apex->GetGUID(), bodyGuid = victim->GetGUID(), deathDisplay, trophy,
                                  bones]()
    {
        if (trophy)
            if (Player* hunter = ObjectAccessor::GetPlayer(map, apexGuid);
                hunter && hunter->IsAlive() && sSpellMgr->GetSpellInfo(SPELL_SKULL_TROPHY))
                hunter->CastSpell(hunter, SPELL_SKULL_TROPHY, true);
        if (bones)
            LayBoneBed(map, bodyGuid, deathDisplay);
    }, BONE_BED_DELAY);
}

/// Loads coa_apex_display_taille once, after the world database is up and before the maps update.
class AscensionApexSkullSpineWorldScript : public WorldScript
{
public:
    AscensionApexSkullSpineWorldScript() : WorldScript("AscensionApexSkullSpineWorldScript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override { LoadApexPreySizes(); }
};

// ==================================================================================================
// Spell and aura scripts
// ==================================================================================================

// Canopy Drop (138389): a Rocket Pounce fired from the camouflage and from 8 m or more above the target deals
// 9300064 on impact, and its stun (20253) lasts 5 s instead of 3. The camouflage is gone by the impact (the
// cast breaks it), hence the condition is taken at CheckCast and kept for the impact.
class spell_apex_rocket_pounce : public SpellScript
{
    PrepareSpellScript(spell_apex_rocket_pounce);

    bool _fromCanopy = false;

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (caster && target && caster->HasAura(SPELL_CANOPY_DROP) && caster->HasAura(SPELL_CAMOUFLAGE) &&
            caster->GetPositionZ() - target->GetPositionZ() >= CANOPY_MIN_HEIGHT)
            _fromCanopy = true;   // never reset: the second CheckCast (Spell::cast) may run without camouflage
        return SPELL_CAST_OK;
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!_fromCanopy || !caster || !target || !target->IsAlive())
            return;
        // 3 s -> 5 s, in proportion: the stun already carries its diminishing returns (3 s, 1.5 s, 0.75 s).
        if (Aura* stun = target->GetAura(SPELL_POUNCE_STUN, caster->GetGUID()))
            if (stun->GetMaxDuration() > 0)
            {
                int32 const duration = stun->GetMaxDuration() * CANOPY_STUN_MS / CANOPY_BASE_STUN_MS;
                stun->SetMaxDuration(duration);
                stun->SetDuration(duration);
            }
        if (sSpellMgr->GetSpellInfo(SPELL_CANOPY_DROP_IMPACT))
            caster->CastSpell(target, SPELL_CANOPY_DROP_IMPACT, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_apex_rocket_pounce::CheckCast);
        AfterHit += SpellHitFn(spell_apex_rocket_pounce::HandleAfterHit);
    }
};

// Plasma Caster (9300012), Plasma Barrage (9300108) and Plasma Frenzy (9300116): no shot while the cannon is offline
// (Self-Destruct), and no Plasma Caster while Plasma Frenzy fires.
// EXCLUDE_CASTER_AURA already holds Unmasked, and a spell has only one.
class spell_apex_cannon_gate : public SpellScript
{
    PrepareSpellScript(spell_apex_cannon_gate);

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return SPELL_CAST_OK;
        if (caster->HasAura(SPELL_CANNON_OFFLINE))
            return SPELL_FAILED_CASTER_AURASTATE;
        // Plasma Frenzy holds the cannon for its 5 s: no aimed shot meanwhile.
        if (GetSpellInfo()->Id == SPELL_PLASMA_CASTER && caster->HasAura(SPELL_PLASMA_FRENZY))
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_apex_cannon_gate::CheckCast);
    }
};

// Plasma Bolt (9300013): Patient Ambush's charge is spent by the shot. Its damage was computed at launch with the
// charge (SPELLMOD_DAMAGE on CANON); the charge goes on impact.
class spell_apex_plasma_bolt : public SpellScript
{
    PrepareSpellScript(spell_apex_plasma_bolt);

    void HandleAfterHit()
    {
        if (Unit* caster = GetCaster())
            caster->RemoveAurasDueToSpell(SPELL_CHARGED_CASTER);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_apex_plasma_bolt::HandleAfterHit);
    }
};

// A killing blow of Skull and Spine tears the skeleton out (TearOutSkeleton, 28/10). Claim the Skull (138396):
// it also gives one more Trophy and resets Rocket Pounce.
class spell_apex_skull_and_spine : public SpellScript
{
    PrepareSpellScript(spell_apex_skull_and_spine);

    void HandleAfterHit()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Unit* target = GetHitUnit();
        if (!player || !target || target->IsAlive())
            return;
        TearOutSkeleton(player, target);
        if (!player->HasAura(SPELL_CLAIM_THE_SKULL))
            return;
        GrantApexTrophies(player, 1);
        player->RemoveSpellCooldown(SPELL_ROCKET_POUNCE, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_apex_skull_and_spine::HandleAfterHit);
    }
};

// Trophy Hunter v1 (data: 9300036 procs on every kill that yields experience or honor). The proc goes through
// GrantApexTrophies, which holds the cap of Trophy Rack: the DBC stack amount becomes 7 (R3), the C++ keeps 5,
// 6 or 7. v2 (AscensionApexTrophyScript) calls the same function once the data drops the proc.
class aura_apex_trophy_hunter : public AuraScript
{
    PrepareAuraScript(aura_apex_trophy_hunter);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            GrantApexTrophies(player, 1);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_apex_trophy_hunter::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Mimicked Voice (138385): the Distract of the core turns the units (players too, like the rogue's); the
// creatures that are not fighting then walk to the voice, stop 3 m short, wait, and walk back home.
class spell_apex_mimicked_voice : public SpellScript
{
    PrepareSpellScript(spell_apex_mimicked_voice);

    void HandleAfterHit()
    {
        Creature* creature = GetHitCreature();
        WorldLocation const* voice = GetExplTargetDest();
        if (!creature || !voice || !creature->IsAlive() || creature->IsInCombat() || creature->IsInEvadeMode() ||
            creature->IsPet() || creature->IsTotem() || creature->IsCharmedOwnedByPlayerOrPlayer() ||
            creature->isWorldBoss() || creature->IsDungeonBoss() ||
            creature->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            return;
        float const dx = voice->GetPositionX() - creature->GetPositionX();
        float const dy = voice->GetPositionY() - creature->GetPositionY();
        float const dz = voice->GetPositionZ() - creature->GetPositionZ();
        float const distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= VOICE_STOP_DISTANCE)
            return;
        float const part = (distance - VOICE_STOP_DISTANCE) / distance;
        Position const stop(creature->GetPositionX() + dx * part, creature->GetPositionY() + dy * part,
                            creature->GetPositionZ() + dz * part);
        // After the effects: the core's MoveDistract is replaced by the walk (same active slot).
        creature->SetWalk(true);
        creature->GetMotionMaster()->MovePoint(POINT_VOICE, stop);
        creature->m_Events.AddEventAtOffset([creature]()
        {
            if (!creature->IsAlive() || creature->IsInCombat() || creature->IsInEvadeMode())
                return;
            // Back to its home point; once there, the idle generator (random, waypoints) takes over again.
            creature->GetMotionMaster()->MovePoint(POINT_VOICE_HOME, creature->GetHomePosition());
        }, VOICE_HOLD);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_apex_mimicked_voice::HandleAfterHit);
    }
};

// Grapple Snatch (138388): the snatched creature does not call its pack for 3 s. Set before the hit, so the
// engagement the hit starts finds the call already spent. Known limit: a formation follows its member anyway
// (CreatureGroup::MemberEngagingTarget, Creature.cpp:2917).
class spell_apex_grapple_snatch : public SpellScript
{
    PrepareSpellScript(spell_apex_grapple_snatch);

    void HandleBeforeHit(SpellMissInfo missInfo)
    {
        Creature* creature = GetHitCreature();
        if (missInfo != SPELL_MISS_NONE || !creature || creature->IsPet() || creature->IsCharmedOwnedByPlayerOrPlayer())
            return;
        creature->SetNoCallAssistance(true);
        creature->SetNoCallForHelp(true);
        creature->m_Events.AddEventAtOffset([creature]()
        {
            creature->SetNoCallAssistance(false);
            creature->SetNoCallForHelp(false);
        }, GRAPPLE_SILENCE);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_apex_grapple_snatch::HandleBeforeHit);
    }
};

// Smart Disc (138379, 27/10 16:00): the throw only aims. Effect 0 (DUMMY on TARGET_DEST_TARGET_ENEMY) is moved to the
// end of the line (PlanDiscFlight), where the client sends the spinning disc at the spell's Speed; once cast, the
// disc cuts (9300130) every enemy it crosses, one sweep every 100 ms, each enemy once per leg: once on the way out,
// once more on the way back to the hand (27/10 evening, sponsor: « the Apex gets his disc back »).
class spell_apex_smart_disc : public SpellScript
{
    PrepareSpellScript(spell_apex_smart_disc);

    ApexDiscFlight _flight;
    bool _planned = false;

    void HandleDestination(SpellDestination& dest)
    {
        if (Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr)
        {
            dest.Relocate(PlanDiscFlight(player, GetExplTargetUnit(), _flight));
            _planned = true;
        }
    }

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        if (!_planned)
            PlanDiscFlight(player, GetExplTargetUnit(), _flight);
        LaunchDisc(player, _flight, GetSpellInfo()->Speed, SPELL_SMART_DISC_CUT, SPELL_SMART_DISC_CUT);
    }

    void Register() override
    {
        OnDestinationTargetSelect += SpellDestinationTargetSelectFn(spell_apex_smart_disc::HandleDestination, EFFECT_0,
                                                                    TARGET_DEST_TARGET_ENEMY);
        AfterCast += SpellCastFn(spell_apex_smart_disc::HandleAfterCast);
    }
};

// Razor Disc (138399, replaces Smart Disc on its button): the same slow disc, out and back (cuts 9300091 out, 9300092
// back, each as strong as a cut of Smart Disc since 27/10 evening), which also interrupts the aimed enemy when it
// reaches it (effect 1 of the spell). Disc Overcharge (138400): the throw also frees the Apex (9300094).
class spell_apex_razor_disc : public SpellScript
{
    PrepareSpellScript(spell_apex_razor_disc);

    ApexDiscFlight _flight;
    bool _planned = false;

    void HandleDestination(SpellDestination& dest)
    {
        if (Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr)
        {
            dest.Relocate(PlanDiscFlight(player, GetExplTargetUnit(), _flight));
            _planned = true;
        }
    }

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        if (player->HasAura(SPELL_DISC_OVERCHARGE) && sSpellMgr->GetSpellInfo(SPELL_DISC_RELEASE))
            player->CastSpell(player, SPELL_DISC_RELEASE, true);
        if (!_planned)
            PlanDiscFlight(player, GetExplTargetUnit(), _flight);
        LaunchDisc(player, _flight, GetSpellInfo()->Speed, SPELL_RAZOR_DISC_PASS, SPELL_RAZOR_DISC_RETURN);
    }

    void Register() override
    {
        OnDestinationTargetSelect += SpellDestinationTargetSelectFn(spell_apex_razor_disc::HandleDestination, EFFECT_0,
                                                                    TARGET_DEST_TARGET_ENEMY);
        AfterCast += SpellCastFn(spell_apex_razor_disc::HandleAfterCast);
    }
};

// Patient Ambush (138404): every second spent still under camouflage charges the cannon (Charged Caster, 5 stacks;
// moving removes it by its AuraInterruptFlags).
class aura_apex_patient_ambush : public AuraScript
{
    PrepareAuraScript(aura_apex_patient_ambush);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (player && player->IsAlive() && player->HasAura(SPELL_CAMOUFLAGE) && !player->isMoving() &&
            sSpellMgr->GetSpellInfo(SPELL_CHARGED_CASTER))
            player->CastSpell(player, SPELL_CHARGED_CASTER, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_apex_patient_ambush::HandlePeriodic, EFFECT_0,
                                                 SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Plasma Splash (138406): a cannon shot bursts on impact, 50% of the damage it really dealt to the enemies within
// 5 m of the target (9300107, which carries no family flag: no proc on itself).
class aura_apex_plasma_splash : public AuraScript
{
    PrepareAuraScript(aura_apex_plasma_splash);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!caster || !damage || !victim || victim == caster || !damage->GetDamage() ||
            !sSpellMgr->GetSpellInfo(SPELL_PLASMA_SPLASH_DAMAGE))
            return;
        int32 const splash = int32(damage->GetDamage() * PLASMA_SPLASH_PCT / 100);
        if (splash > 0)
            caster->CastCustomSpell(SPELL_PLASMA_SPLASH_DAMAGE, SPELLVALUE_BASE_POINT0, splash, victim, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_apex_plasma_splash::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 9300107: the burst spares the target the shot already hit (its explicit target).
class spell_apex_plasma_splash_damage : public SpellScript
{
    PrepareSpellScript(spell_apex_plasma_splash_damage);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        if (Unit* main = GetExplTargetUnit())
            targets.remove(main);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_apex_plasma_splash_damage::FilterTargets,
                                                                  EFFECT_ALL, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

// Plasma Frenzy (node 138407, 9300116): an aura on the Apex, PERIODIC_DUMMY every 250 ms for 5 s (one tick at the
// start, ATTR5 0x200). Each tick fires one shot (FirePlasmaFrenzyShot). No shot while the Apex is dead, out of the
// world, controlled, mounted, on a taxi, in a vehicle, silenced or pacified; the frenzy ends when the cannon becomes unavailable (Unmasked,
// Cannon Offline).
class aura_apex_plasma_frenzy : public AuraScript
{
    PrepareAuraScript(aura_apex_plasma_frenzy);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            State(player).frenzyHits.clear();
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Player* apex = GetTarget()->ToPlayer();
        if (!apex || !apex->IsInWorld() || !apex->IsAlive())
            return;
        if (apex->HasAura(SPELL_UNMASKED) || apex->HasAura(SPELL_CANNON_OFFLINE))
        {
            // Not from inside the aura's own tick: at the Apex's next update.
            Defer(apex, [](Player* player) { player->RemoveAurasDueToSpell(SPELL_PLASMA_FRENZY); });
            return;
        }
        // A triggered cast skips these checks: the cannon holds fire while the Apex is controlled, mounted, on a taxi,
        // in a vehicle, silenced or pacified (review 27/10).
        if (apex->HasUnitState(UNIT_STATE_CONTROLLED) || apex->IsCharmed() || apex->IsMounted() || apex->IsInFlight() ||
            apex->GetVehicle() || apex->HasUnitFlag(UnitFlags(UNIT_FLAG_SILENCED | UNIT_FLAG_PACIFIED)))
            return;
        FirePlasmaFrenzyShot(apex);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_apex_plasma_frenzy::HandleApply, EFFECT_0,
                                              SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_apex_plasma_frenzy::HandlePeriodic, EFFECT_0,
                                                 SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The three shots of Plasma Frenzy (9300117-9300119): a blast hits at most 5 enemies (random ones beyond), and an enemy
// at most 8 blasts per frenzy (counted at launch, in the caster's state). The core cuts MaxAffectedTargets AFTER this
// hook (Spell::SelectImplicitAreaTargets), hence the data says 0 and the cut is made here, before the count.
class spell_apex_plasma_frenzy_shot : public SpellScript
{
    PrepareSpellScript(spell_apex_plasma_frenzy_shot);

    void FilterTargets(std::list<WorldObject*>& targets)
    {
        Player* apex = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!apex)
        {
            Acore::Containers::RandomResize(targets, FRENZY_ENEMIES_PER_BLAST);
            return;
        }
        ApexTalentState& state = State(apex);
        targets.remove_if([&state](WorldObject* object)
        {
            auto const hits = state.frenzyHits.find(object->GetGUID());
            return hits != state.frenzyHits.end() && hits->second >= FRENZY_HITS_PER_ENEMY;
        });
        Acore::Containers::RandomResize(targets, FRENZY_ENEMIES_PER_BLAST);
        for (WorldObject* object : targets)
            ++state.frenzyHits[object->GetGUID()];
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_apex_plasma_frenzy_shot::FilterTargets,
                                                                  EFFECT_0, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

// ==================================================================================================
// Falconer's Eye: the hunting drone and the COAAPEX messages (contract C-ADDON of the plan)
// ==================================================================================================
//
// Server -> client only, an addon whisper of the character to itself (CHAT_MSG_WHISPER + LANG_ADDON, GUID
// overload of BuildChatPacket, as AscensionCompat.cpp's ASC_LOCAL_CAD): the client sees CHAT_MSG_ADDON,
// prefix "COAAPEX", text "<i>/<n>|<lines joined by ^>", at most 200 bytes per message, a line is never split.
//   V|1                                     version
//   D|seq|left_s|zone|width_yd|height_yd|dx|dy
//        seq 0-65535; left_s whole seconds the drone still lives; zone = the PLAYER's zone (AreaTable id), 0 when
//        the zone has no map (instance: no T line then); width_yd = |LocLeft - LocRight| of WorldMapArea (the
//        map's horizontal axis), height_yd = |LocTop - LocBottom| (vertical axis); dx dy = drone on the zone map,
//        in 1/10000 (0 = left / top edge), as Map2ZoneCoordinates puts it.
//   T|id|x|y|level|hp|kind|rank|hostile|hidden
//        id opaque 1-255, stable for the drone's life (never a guid); x y in 1/10000 of the zone map; level -1 when
//        10 or more above the player ("??"); hp % rounded to 5; kind P (character: player or bot, never told
//        apart) or C (creature); rank 0 normal, 1 elite, 2 rare, 3 rare elite, 4 boss; hostile 1, neutral 0;
//        hidden (27/10 evening, appended: an addon that reads 9 fields ignores it) 1 = stealthed or invisible, seen
//        only because the drone spotted it (RevealHidden), 0 otherwise.
//   F                                       end of the salvo
//   X                                       the drone is gone: fade the marks out (10 s)
// One salvo per second while the drone lives. Never a name, a class, a guid, nor a unit outside the player's zone
// or that the player cannot see (CanSeeOrDetect). At most 20 units, the nearest to the drone, within 40 m of it.
//
// 27/10 evening (sponsor: « it stays too close; it is a scout, the player already sees what is around him »):
// the round circles a centre 60 to 85 m from the Apex (DRONE_CENTRE_MIN/MAX, radius 15 m), so the drone flies
// 45 to 100 m away and its 40 m scan covers 5 to 140 m. 100 m = Visibility.Distance.Continents of the PTR: the drone,
// and the hidden enemies it reveals, stay within the distance at which the client draws units. The centre follows
// the Apex (leash): beyond 85 m, or once the Apex is within 25 m of it, it is placed again 60 m from him.
// It also spots the hidden (27/10 evening): an enemy within 40 m of the drone under stealth or invisibility gets
// 9300132 from the Apex (SPELL_AURA_MOD_STALKED, 3 s, renewed every salvo): Unit::IsAlwaysDetectableFor then lets the
// Apex (and his pets, CanDetect uses the master's sight) see and target it, at the usual visibility distance, as
// Hunter's Mark does; the stalked flag also tracks it on his minimap. Only the Apex: neither his group nor anyone
// else. GMs and spectators stay hidden (checked before detection in CanSeeOrDetect).

constexpr char APEX_ADDON_PREFIX[] = "COAAPEX\t";
constexpr std::size_t APEX_ADDON_MAX = 200;
constexpr float DRONE_SCAN_RADIUS = 40.0f;
constexpr std::size_t DRONE_MAX_TARGETS = 20;
constexpr uint32 DRONE_PERIOD_MS = 1000;
constexpr float DRONE_HOVER = 5.0f;                  // 27/10 16:00: inspection round 5 m above its centre
constexpr float DRONE_ORBIT_RADIUS = 15.0f;          // 27/10 evening (8 before): around its centre
constexpr float DRONE_ORBIT_SPEED = 6.0f;            // m/s: a round of 94 m in about 16 s, two rounds in 30 s
constexpr float DRONE_CENTRE_MIN = 60.0f;            // 27/10 evening: centre of the round 60-85 m from the Apex
constexpr float DRONE_CENTRE_MAX = 85.0f;            //   (+ radius 15 = 100 m, the continents' visibility distance)
constexpr float DRONE_CENTRE_NEAR = 25.0f;           // the Apex walked onto the round: the scout goes ahead again
constexpr float DRONE_EYE_HEIGHT = 2.0f;             // line of sight from the Apex's head to the centre
constexpr float DRONE_CENTRE_MOVE = 10.0f;           // the leash moves the centre only by more than this
constexpr float DRONE_LEASH_STEP = 10.0f;            // ... and is worked out again only after the Apex moved this much
constexpr float DRONE_GROUND_SEARCH_UP = 50.0f;      // ground under the round looked for from 50 m above it (hills)
constexpr float DRONE_GROUND_SEARCH = 150.0f;        //   down to 100 m below it
constexpr float DRONE_HEADING_MIN = 3.0f;            // the Apex moved this much since the last salvo: his heading
constexpr float DRONE_CATCH_UP_S = 4.0f;             // a distanced drone flies fast enough to reach its round in 4 s
constexpr float DRONE_MAX_SPEED = 50.0f;             //   up to 50 m/s (a flying mount runs at 38 at most)
constexpr float DRONE_TELEPORT_DISTANCE = 200.0f;    //   beyond 200 m (a teleport of the Apex), it is moved there
constexpr uint8 DRONE_ORBIT_POINTS = 24;             // 15 degrees a vertex
constexpr float DRONE_GROUND_CLEARANCE = 2.5f;       // a vertex over a slope rises to stay 2.5 m above the ground

void SendApexLines(Player* player, std::vector<std::string> const& lines)
{
    if (!player || !player->GetSession())
        return;
    std::size_t const prefixed = sizeof(APEX_ADDON_PREFIX) - 1 + 8;  // prefix + "nn/nn|"
    std::vector<std::string> chunks(1);
    for (std::string const& line : lines)
    {
        std::string& current = chunks.back();
        if (!current.empty() && prefixed + current.size() + 1 + line.size() > APEX_ADDON_MAX)
            chunks.emplace_back();
        std::string& target = chunks.back();
        if (!target.empty())
            target += '^';
        target += line;
    }
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        std::string const message = std::string(APEX_ADDON_PREFIX) + std::to_string(index + 1) + "/" +
            std::to_string(chunks.size()) + "|" + chunks[index];
        WorldPacket packet;
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
                                     message, 0, player->GetName(), player->GetName(), 0, false);
        player->SendDirectMessage(&packet);
    }
}

/// Zone-map coordinates in 1/10000 (Map2ZoneCoordinates gives percents, x along the horizontal axis).
std::pair<int32, int32> ZoneMapPosition(WorldObject const* object, uint32 zone)
{
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    Map2ZoneCoordinates(x, y, zone);
    return { int32(std::lround(x * 100.0f)), int32(std::lround(y * 100.0f)) };
}

uint32 PreyRank(Unit const* unit)
{
    Creature const* creature = unit->ToCreature();
    if (!creature)
        return 0;
    if (creature->isWorldBoss() || creature->IsDungeonBoss())
        return 4;
    switch (creature->GetCreatureTemplate()->rank)
    {
        case CREATURE_ELITE_ELITE: return 1;
        case CREATURE_ELITE_RARE: return 2;
        case CREATURE_ELITE_RAREELITE: return 3;
        case CREATURE_ELITE_WORLDBOSS: return 4;
        default: return 0;
    }
}

/// True when the creature is invisible for good (an aura of SPELL_AURA_MOD_INVISIBILITY without end: spirits seen
/// only with a quest item, invisible event creatures): the drone leaves those to their design. Same test as the core
/// (WorldObject::CanDetectInvisibilityOf, isPermInvisibleCreature).
bool PermanentlyInvisible(Unit const* unit)
{
    for (AuraEffect const* effect : unit->GetAuraEffectsByType(SPELL_AURA_MOD_INVISIBILITY))
        if (effect->GetSpellInfo() && effect->GetSpellInfo()->GetMaxDuration() == -1)
            return true;
    return false;
}

/// Falconer's Eye spots a hidden enemy (27/10 evening): `unit` is under stealth or invisibility and within the drone's
/// scan. It gets (or keeps for 3 more seconds) 9300132 from the Apex, and true is returned while it carries it.
/// Spotted: characters and what they control (players, bots, their pets), always; a creature of the world only when
/// it could be fought (not a trigger, not unselectable nor unattackable, not immune to players) and is not invisible
/// for good (PermanentlyInvisible). Not Unit::IsValidAttackTarget: it refuses a unit the Apex cannot see or detect
/// (Unit::_IsValidAttackTarget), that is every hidden one (review of 27/10 evening). The scan already left out
/// friends (AnyUnfriendlyUnitInObjectRangeCheck) and the dead.
bool RevealHidden(Player* apex, Unit* unit)
{
    if (!unit->m_stealth.GetFlags() && !unit->m_invisibility.GetFlags())
        return false;
    if (!unit->IsCharmedOwnedByPlayerOrPlayer())
    {
        Creature const* creature = unit->ToCreature();
        if (!creature || creature->IsTrigger() || creature->IsImmuneToPC() || PermanentlyInvisible(creature) ||
            creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE))
            return false;
    }
    if (Aura* mark = unit->GetAura(SPELL_DRONE_REVEAL, apex->GetGUID()))
    {
        mark->RefreshDuration();
        return true;
    }
    if (!sSpellMgr->GetSpellInfo(SPELL_DRONE_REVEAL))
        return false;
    return apex->AddAura(SPELL_DRONE_REVEAL, unit) != nullptr;
}

class npc_apex_hunting_drone : public CreatureAI
{
public:
    explicit npc_apex_hunting_drone(Creature* creature) : CreatureAI(creature) { }

    void InitializeAI() override
    {
        me->SetReactState(REACT_PASSIVE);
        me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
        me->SetImmuneToAll(true);
    }

    void IsSummonedBy(WorldObject* summoner) override
    {
        if (!summoner || !summoner->IsPlayer())
            return;
        _owner = summoner->GetGUID();
        me->SetDisableGravity(true);
        // 27/10 16:00 (sponsor): no longer static. The drone climbs to the first vertex of its round, then circles its
        // centre for good (UpdateAI starts the cyclic spline once the climb is over). Not steerable.
        // 27/10 evening: the centre is the point aimed with Falconer's Eye when it lies 60 to 85 m from the Apex;
        // otherwise the point on the same line at 60 m (aimed closer) or 85 m (aimed farther), short of the first wall.
        Player* apex = summoner->ToPlayer();
        float const aimed = apex->GetExactDist2d(me);
        _lastOwner = apex->GetPosition();
        if (aimed >= DRONE_CENTRE_MIN && aimed <= DRONE_CENTRE_MAX)
        {
            float const ground = GroundUnder(me->GetPositionX(), me->GetPositionY(),
                                             std::max(me->GetPositionZ(), apex->GetPositionZ()));
            _centre.Relocate(me->GetPositionX(), me->GetPositionY(), ground > INVALID_HEIGHT ? ground : me->GetPositionZ());
        }
        else
            _centre = ScoutCentre(apex, aimed > 1.0f ? apex->GetAngle(me) : apex->GetOrientation(),
                                  std::clamp(aimed, DRONE_CENTRE_MIN, DRONE_CENTRE_MAX), me->GetPositionZ());
        FlyToOrbit();
        _timer = 0;   // first salvo at once
    }

    // Starting the spline here would be undone at once: the point generator is finalized after this call and the
    // idle generator that follows stops the unit (IdleMovementGenerator::Reset -> StopMoving). UpdateAI starts it.
    void MovementInform(uint32 type, uint32 id) override
    {
        if (type == POINT_MOTION_TYPE && id == POINT_DRONE_HOVER)
            _orbitPending = true;
    }

    void MoveInLineOfSight(Unit* /*who*/) override { }
    void AttackStart(Unit* /*who*/) override { }
    void EnterEvadeMode(EvadeReason /*why*/) override { }

    void JustDied(Unit* /*killer*/) override { SendEnd(); }

    void UpdateAI(uint32 diff) override
    {
        if (_ended || !_owner)
            return;
        UpdateOrbit();
        TempSummon* summon = me->ToTempSummon();
        uint32 const left = summon ? summon->GetTimer() : 0;
        if (left > diff + 100)
            _timed = true;
        // The summon has no despawn hook for its AI: the end is told while the drone still exists, at its last
        // update (TempSummon::Update runs the AI before it counts the time down and unsummons).
        if (_timed && left <= diff + 100)
        {
            SendEnd();
            return;
        }
        if (_timer > diff)
        {
            _timer -= diff;
            return;
        }
        _timer = DRONE_PERIOD_MS;
        if (Player* owner = Owner(); owner && owner->IsAlive())
            KeepLeash(owner);
        SendSalvo(left);
    }

private:
    /// Ground under (x, y), looked for from DRONE_GROUND_SEARCH_UP metres above `from` (Map::GetHeight ignores the
    /// terrain above its search point: a hill between the Apex and the round would hide under it, review of 27/10
    /// evening) and down to DRONE_GROUND_SEARCH below that. INVALID_HEIGHT when there is none (void, edge of the map).
    /// Indoors, the first floor found from above may be a roof: the centre is then under the roof line of sight test
    /// (ScoutCentre) and the round rises over the building.
    float GroundUnder(float x, float y, float from) const
    {
        return me->GetMapHeight(x, y, from + DRONE_GROUND_SEARCH_UP, true, DRONE_GROUND_SEARCH);
    }

    /// The point `distance` metres from the Apex towards `angle`, brought back short of the first wall, building or
    /// gameobject between the Apex's head and the round's height (vmaps; trees and other M2 doodads are ignored), on
    /// the ground found there (GroundUnder, from the higher of `z` and the Apex), or at `z` without ground.
    Position ScoutCentre(Player* apex, float angle, float distance, float z) const
    {
        float const ax = apex->GetPositionX();
        float const ay = apex->GetPositionY();
        float const az = apex->GetPositionZ() + DRONE_EYE_HEIGHT;
        float const dx = std::cos(angle);
        float const dy = std::sin(angle);
        Map* map = apex->GetMap();
        auto const clear = [&](float length)
        {
            return map->isInLineOfSight(ax, ay, az, ax + dx * length, ay + dy * length, z + DRONE_HOVER,
                                        apex->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
        };
        float length = distance;
        if (!clear(length))
        {
            float open = 0.0f, blocked = length;       // 7 halvings: the wall is found to 0.7 m at most
            for (uint8 i = 0; i < 7; ++i)
            {
                float const middle = (open + blocked) * 0.5f;
                (clear(middle) ? open : blocked) = middle;
            }
            length = open;
        }
        float const cx = ax + dx * length;
        float const cy = ay + dy * length;
        float const ground = GroundUnder(cx, cy, std::max(z, apex->GetPositionZ()));
        return Position(cx, cy, ground > INVALID_HEIGHT ? ground : z, angle);
    }

    /// The leash, once a salvo: the centre is placed again DRONE_CENTRE_MIN metres AHEAD of the Apex (his heading over
    /// the last salvo when he moved, where he faces otherwise: the scout precedes him, review of 27/10 evening) when he
    /// is farther than DRONE_CENTRE_MAX from it or within DRONE_CENTRE_NEAR of it. Worked out again only once the Apex
    /// moved DRONE_LEASH_STEP since the last try (each try costs up to 8 line of sight tests). Same map only (Owner).
    void KeepLeash(Player* owner)
    {
        Position const here = owner->GetPosition();
        float const moved = here.GetExactDist2d(&_lastOwner);
        float const heading = moved >= DRONE_HEADING_MIN ? _lastOwner.GetAngle(&here) : owner->GetOrientation();
        _lastOwner = here;
        float const distance = owner->GetExactDist2d(_centre.GetPositionX(), _centre.GetPositionY());
        if (distance <= DRONE_CENTRE_MAX && distance >= DRONE_CENTRE_NEAR)
            return;
        if (_leashTried && here.GetExactDist2d(&_leashFrom) < DRONE_LEASH_STEP)
            return;
        _leashTried = true;
        _leashFrom = here;
        Position const centre = ScoutCentre(owner, heading, DRONE_CENTRE_MIN, owner->GetPositionZ());
        // A wall close ahead brings the centre back within 25 m of the Apex: placed again on the same spot every
        // second, the drone would never start its round (review of 27/10 evening). Kept when it would barely move.
        if (centre.GetExactDist2d(&_centre) < DRONE_CENTRE_MOVE)
            return;
        _centre = centre;
        FlyToOrbit();
    }

    /// Plans the round around _centre and flies to its first vertex (the one nearest the drone); the cyclic round
    /// starts on arrival (MovementInform, UpdateOrbit).
    void FlyToOrbit()
    {
        _orbiting = false;
        _orbitPending = false;
        _start = 0;
        PlanOrbit();
        G3D::Vector3 const& first = _orbit.front();
        // Outrun by a mounted Apex: the drone flies fast enough to reach its round in DRONE_CATCH_UP_S (its own speed
        // when that is enough, at most DRONE_MAX_SPEED), so it stays a scout ahead instead of trailing behind; across
        // a teleport of the Apex within the map, it is moved straight there (review of 27/10 evening).
        float const away = me->GetExactDist(first.x, first.y, first.z);
        if (away > DRONE_TELEPORT_DISTANCE)
        {
            me->NearTeleportTo(first.x, first.y, first.z, me->GetOrientation());
            _orbitPending = true;
            return;
        }
        float const cruise = me->GetSpeed(MOVE_RUN);
        float const speed = std::min(away / DRONE_CATCH_UP_S, DRONE_MAX_SPEED);
        me->GetMotionMaster()->MovePoint(POINT_DRONE_HOVER, first.x, first.y, first.z, FORCED_MOVEMENT_NONE,
                                         speed > cruise ? speed : 0.0f, 0.0f, false);
    }

    /// The round: DRONE_ORBIT_POINTS vertices on a circle of DRONE_ORBIT_RADIUS around _centre, DRONE_HOVER above
    /// it, each raised to keep DRONE_GROUND_CLEARANCE over the ground below it (map height, vmaps included). The
    /// first vertex is the one on the drone's side.
    void PlanOrbit()
    {
        _orbit.clear();
        float const cx = _centre.GetPositionX();
        float const cy = _centre.GetPositionY();
        float const cz = _centre.GetPositionZ() + DRONE_HOVER;
        float const start = std::atan2(me->GetPositionY() - cy, me->GetPositionX() - cx);
        for (uint8 i = 0; i < DRONE_ORBIT_POINTS; ++i)
        {
            float const angle = start + 2.0f * float(M_PI) * float(i) / float(DRONE_ORBIT_POINTS);
            float const x = cx + DRONE_ORBIT_RADIUS * std::cos(angle);
            float const y = cy + DRONE_ORBIT_RADIUS * std::sin(angle);
            float const ground = GroundUnder(x, y, cz);   // from well above: a slope rising over the round is found
            float const z = ground > INVALID_HEIGHT ? std::max(cz, ground + DRONE_GROUND_CLEARANCE) : cz;
            _orbit.emplace_back(x, y, z);
        }
    }

    /// Starts the cyclic round once the climb is over, and starts it again if anything stopped it (a knockback, a
    /// movement generator reset): the spline is then finalized while the round should run.
    void UpdateOrbit()
    {
        if (_orbit.empty())
            return;
        if (_orbiting && !_orbitPending && me->movespline->Finalized())
        {
            // Stopped (a knockback, a reset): back to the nearest vertex first, the round starts again from there.
            // Launching the cycle from elsewhere would put that point in the server's loop for good, while the client
            // drops the first vertex of a flying cyclic spline after one round (MovementPacketBuilder).
            _orbiting = false;
            _start = 0;
            for (std::size_t i = 1; i < _orbit.size(); ++i)
                if (me->GetExactDistSq(_orbit[i].x, _orbit[i].y, _orbit[i].z) <
                    me->GetExactDistSq(_orbit[_start].x, _orbit[_start].y, _orbit[_start].z))
                    _start = i;
            G3D::Vector3 const& back = _orbit[_start];
            me->GetMotionMaster()->MovePoint(POINT_DRONE_HOVER, back.x, back.y, back.z, FORCED_MOVEMENT_NONE, 0.0f,
                                             0.0f, false);
            return;
        }
        if (!_orbitPending)
            return;
        _orbitPending = false;
        // The drone stands on vertex _start: path[0] (replaced by the current position in Launch) is that vertex.
        Movement::PointsArray path;
        path.reserve(_orbit.size());
        for (std::size_t i = 0; i < _orbit.size(); ++i)
            path.push_back(_orbit[(_start + i) % _orbit.size()]);
        Movement::MoveSplineInit init(me);
        init.MovebyPath(path);
        init.SetFly();
        init.SetCyclic();
        init.SetVelocity(DRONE_ORBIT_SPEED);
        _orbiting = init.Launch() > 0;
    }

    Player* Owner() const
    {
        Player* owner = ObjectAccessor::GetPlayer(*me, _owner);
        return owner && owner->IsInWorld() && owner->GetMapId() == me->GetMapId() ? owner : nullptr;
    }

    void SendEnd()
    {
        if (_ended)
            return;
        _ended = true;
        if (Player* owner = Owner())
            SendApexLines(owner, { "X" });
    }

    uint32 OpaqueId(ObjectGuid const& guid)
    {
        auto itr = _ids.find(guid);
        if (itr != _ids.end())
            return itr->second;
        if (_nextId > 255)
            return 0;
        _ids.emplace(guid, _nextId);
        return _nextId++;
    }

    void SendSalvo(uint32 leftMs)
    {
        Player* owner = Owner();
        if (!owner)
            return;
        uint32 const zone = owner->GetZoneId();
        WorldMapAreaEntry const* map = sWorldMapAreaStore.LookupEntry(zone);
        bool const mapped = map && map->map_id == owner->GetMapId() && map->y2 != map->y1 && map->x2 != map->x1;

        std::vector<std::string> lines;
        lines.reserve(DRONE_MAX_TARGETS + 3);
        lines.emplace_back("V|1");
        auto const drone = mapped ? ZoneMapPosition(me, zone) : std::pair<int32, int32>{ 0, 0 };
        lines.push_back(Acore::StringFormat("D|{}|{}|{}|{}|{}|{}|{}", uint32(_seq++ & 0xFFFF), leftMs / 1000,
            mapped ? zone : 0, mapped ? uint32(std::lround(std::fabs(map->y1 - map->y2))) : 0,
            mapped ? uint32(std::lround(std::fabs(map->x1 - map->x2))) : 0, drone.first, drone.second));

        // The scan and the spotting of the hidden run on every map (an instance has no zone map, so no T line).
        std::list<Unit*> around;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(me, owner, DRONE_SCAN_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(me, around, check);
        Cell::VisitObjects(me, searcher, DRONE_SCAN_RADIUS);
        // A dead Apex (ghost) spots nothing: the ghost would otherwise see stealthed enemies around the drone.
        std::unordered_set<ObjectGuid> spotted;
        if (owner->IsAlive())
            for (Unit* unit : around)
                if (unit != owner && unit != me && !unit->IsTotem() && RevealHidden(owner, unit))
                    spotted.insert(unit->GetGUID());

        if (mapped)
        {
            std::vector<Unit*> seen;
            for (Unit* unit : around)
                if (unit != owner && unit != me && !unit->IsTotem() && unit->GetZoneId() == zone &&
                    owner->CanSeeOrDetect(unit))
                    seen.push_back(unit);
            std::sort(seen.begin(), seen.end(), [this](Unit const* left, Unit const* right)
            {
                return me->GetExactDist2dSq(left) < me->GetExactDist2dSq(right);
            });
            if (seen.size() > DRONE_MAX_TARGETS)
                seen.resize(DRONE_MAX_TARGETS);

            for (Unit* unit : seen)
            {
                uint32 const id = OpaqueId(unit->GetGUID());
                if (!id)
                    continue;
                auto const position = ZoneMapPosition(unit, zone);
                int32 const level = unit->GetLevel() >= owner->GetLevel() + 10 ? -1 : int32(unit->GetLevel());
                uint32 const health = std::min<uint32>(100, uint32(std::lround(unit->GetHealthPct() / 5.0f)) * 5);
                bool const character = unit->IsPlayer();
                lines.push_back(Acore::StringFormat("T|{}|{}|{}|{}|{}|{}|{}|{}|{}", id, position.first,
                    position.second, level, health, character ? "P" : "C", PreyRank(unit), unit->IsHostileTo(owner) ? 1 : 0,
                    spotted.count(unit->GetGUID()) ? 1 : 0));
            }
        }
        lines.emplace_back("F");
        SendApexLines(owner, lines);
    }

    ObjectGuid _owner;
    uint32 _timer = 0;
    uint32 _seq = 0;
    uint32 _nextId = 1;
    bool _ended = false;
    bool _timed = false;
    std::unordered_map<ObjectGuid, uint32> _ids;
    std::vector<G3D::Vector3> _orbit;
    Position _centre;                       // centre of the round (27/10 evening: 60-85 m from the Apex, leashed)
    Position _lastOwner;                    // the Apex at the last salvo (his heading)
    Position _leashFrom;                    // the Apex at the last leash try
    bool _leashTried = false;
    std::size_t _start = 0;                 // vertex the round starts from (the one the drone climbed or came back to)
    bool _orbitPending = false;
    bool _orbiting = false;
};

// ==================================================================================================
// Hooks without a script name
// ==================================================================================================

// Three Red Dots (138403): a Plasma Caster aimed from the camouflage locks on (Locked On: assured critical on
// the next cannon shot) and paints three red dots on the prey for 1.5 s. At the start of the aim: the
// camouflage still holds (the aim does not break it, ATTR1 0x20).
class AscensionApexTalentSpellScript : public AllSpellScript
{
public:
    AscensionApexTalentSpellScript() : AllSpellScript("AscensionApexTalentSpellScript",
        { ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

    // The aim (1 s) cancelled or interrupted: no shot, so no assured critical left for the next one.
    void OnSpellCastCancel(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*bySelf*/) override
    {
        if (spellInfo && spellInfo->Id == SPELL_PLASMA_CASTER && IsTinker(caster))
            caster->RemoveAurasDueToSpell(SPELL_LOCKED_ON, caster->GetGUID());
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!spell || !spellInfo || spellInfo->Id != SPELL_PLASMA_CASTER || !IsTinker(caster) ||
            !caster->HasAura(SPELL_THREE_RED_DOTS) || !caster->HasAura(SPELL_CAMOUFLAGE))
            return;
        if (sSpellMgr->GetSpellInfo(SPELL_LOCKED_ON))
            caster->AddAura(SPELL_LOCKED_ON, caster);
        Unit* prey = spell->m_targets.GetUnitTarget();
        if (prey && prey != caster && sSpellMgr->GetSpellInfo(SPELL_RED_DOTS_MARK))
            if (Aura* dots = caster->AddAura(SPELL_RED_DOTS_MARK, prey))
            {
                dots->SetMaxDuration(RED_DOTS_MS);
                dots->SetDuration(RED_DOTS_MS);
            }
    }
};

// Smart Disc and Razor Disc cuts (27/10 16:00): « ignores armor and every kind of shield ». Spell.dbc carries the
// immunity piercing (ATTR0 NO_IMMUNITIES, ATTR2 NO_SCHOOL_IMMUNITIES) and the sure hit (ATTR3 ALWAYS_HIT); no DBC field
// carries the two others, set here once the spell store is loaded (SpellMgr::LoadSpellInfoCustomAttributes calls the
// hook for every spell, after the core's own corrections):
//   SPELL_ATTR0_CU_IGNORE_ARMOR: Unit::IsDamageReducedByArmor returns false (CalculateSpellDamageTaken skips armor);
//   AscensionIgnoreAbsorbAndResistance: Unit::CalcAbsorbResist returns at once (no absorb, no split, no resist), as
//   for the Templar's Norgannon's Wrath and the Pyromancer's 802173.
class AscensionApexDiscSpellInfoScript : public GlobalScript
{
public:
    AscensionApexDiscSpellInfoScript() : GlobalScript("AscensionApexDiscSpellInfoScript",
        { GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR }) { }

    void OnLoadSpellCustomAttr(SpellInfo* spellInfo) override
    {
        if (!spellInfo || (spellInfo->Id != SPELL_SMART_DISC_CUT && spellInfo->Id != SPELL_RAZOR_DISC_PASS &&
                           spellInfo->Id != SPELL_RAZOR_DISC_RETURN))
            return;
        spellInfo->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
        spellInfo->AscensionIgnoreAbsorbAndResistance = true;
    }
};

class AscensionApexTalentUnitScript : public UnitScript
{
public:
    AscensionApexTalentUnitScript() : UnitScript("AscensionApexTalentUnitScript", true,
        { UNITHOOK_ON_DAMAGE, UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
          UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE,
          UNITHOOK_ON_UNIT_ENTER_COMBAT }) { }

    // Called by Unit::DealDamage BEFORE the damage removes the camouflage (Unit.cpp:1006, then 1039-1042).
    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override
    {
        if (!attacker || !victim || attacker == victim || !IsTinker(victim))
            return;
        Player* apex = victim->ToPlayer();
        if (!apex->IsAlive())
            return;

        // Arc Discharge, trigger A: wounded under camouflage.
        if (apex->HasAura(SPELL_CAMOUFLAGE))
            TryArcDischarge(apex);

        // Never Saw It Coming: whoever strikes the Apex, under camouflage (the hit that breaks it) or in the window,
        // spoils the ambush (its master too). The list is emptied when a new camouflage starts.
        if (apex->HasAura(SPELL_NSIC) && (apex->HasAura(SPELL_NSIC_WINDOW) || apex->HasAura(SPELL_CAMOUFLAGE)))
        {
            ApexTalentState& state = State(apex);
            state.ripostes.insert(attacker->GetGUID());
            if (ObjectGuid const master = attacker->GetCharmerOrOwnerGUID())
                state.ripostes.insert(master);
        }

        // Sentry Protocol, players: the first hit of a player is its arrival in the fight.
        if (attacker->IsPlayer() && apex->HasAura(SPELL_SENTRY_PROTOCOL))
        {
            ApexTalentState& state = State(apex);
            uint32 const now = getMSTime();
            auto itr = state.sentrySeen.find(attacker->GetGUID());
            if (itr == state.sentrySeen.end() || getMSTimeDiff(itr->second, now) >= SENTRY_SEEN_MS)
            {
                Prune(state.sentrySeen, now, SENTRY_SEEN_MS);
                state.sentrySeen[attacker->GetGUID()] = now;
                TrySentry(apex, attacker);
            }
            else
                itr->second = now;   // still engaged: the mark lasts 60 s after the LAST hit
        }
    }

    // Sentry Protocol, creatures: a creature engages the Apex (Creature::AtEngage, Creature.cpp:2920).
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (!unit || !victim || unit == victim || !IsTinker(victim) || !unit->IsCreature())
            return;
        TrySentry(victim->ToPlayer(), unit);
    }

    // Worthy Prey (free, level 40): +8% against a player, an elite or a rare.
    static bool WorthyPreyBonus(Unit const* target, Unit const* attacker)
    {
        return attacker && target && attacker != target && IsTinker(attacker) &&
            attacker->HasAura(SPELL_WORTHY_PREY) && !attacker->IsFriendlyTo(target) && IsWorthyPrey(target);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (damage && WorthyPreyBonus(target, attacker))
            damage = damage * WORTHY_PREY_PCT / 100;
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage > 0 && WorthyPreyBonus(target, attacker))
            damage = int32(int64(damage) * WORTHY_PREY_PCT / 100);
    }

    // Also called for periodic HEALS (SpellAuraEffects.cpp:6827): IsFriendlyTo in WorthyPreyBonus keeps them out.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (damage && (!spellInfo || !spellInfo->IsPositive()) && WorthyPreyBonus(target, attacker))
            damage = damage * WORTHY_PREY_PCT / 100;
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!aura || !IsTinker(unit))
            return;
        Player* player = unit->ToPlayer();
        switch (aura->GetId())
        {
            case SPELL_WRISTBLADE_STANCE:
            case SPELL_SPEAR_STANCE:
                SynchronizeApexStanceAttacks(player);
                break;
            case SPELL_CAMOUFLAGE:
                // Never Saw It Coming: a new ambush starts with a clean slate (OnDamage records the hits).
                if (player->HasAura(SPELL_NSIC))
                    State(player).ripostes.clear();
                break;
            case SPELL_UNMASKED:
                // Blood Frenzy: Unmasked lasts 5 s more, and every claw or spear hit meanwhile gives Frenzy.
                // Once per aura: a refresh (OnAuraApply also runs for a found aura, Unit.cpp:5039) or an aura
                // reloaded at login with its extended duration must not be extended again.
                if (player->HasAura(SPELL_BLOOD_FRENZY) && !aura->GetScriptValue(SPELL_BLOOD_FRENZY) &&
                    aura->GetMaxDuration() > 0 && aura->GetMaxDuration() <= aura->GetSpellInfo()->GetMaxDuration())
                {
                    aura->SetScriptValue(SPELL_BLOOD_FRENZY, 1);
                    int32 const duration = aura->GetMaxDuration() + BLOOD_FRENZY_EXTENSION_MS;
                    aura->SetMaxDuration(duration);
                    aura->SetDuration(duration);
                    if (sSpellMgr->GetSpellInfo(SPELL_FRENZY_PROC))
                        if (Aura* proc = player->AddAura(SPELL_FRENZY_PROC, player))
                        {
                            proc->SetMaxDuration(duration);
                            proc->SetDuration(duration);
                        }
                }
                break;
            default:
                break;
        }
    }

    // Runs after the aura left the applied list (Unit::_UnapplyAura): HasAura(id) is already false.
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!aurApp || !aurApp->GetBase() || !IsTinker(unit))
            return;
        Player* player = unit->ToPlayer();
        switch (aurApp->GetBase()->GetId())
        {
            case SPELL_WRISTBLADE_STANCE:
            case SPELL_SPEAR_STANCE:
                SynchronizeApexStanceAttacks(player);
                break;
            case SPELL_CAMOUFLAGE:
                // Optical Camouflage mends only under it: the next camouflage starts over (below 30% again).
                State(player).camoMending = false;
                if (mode == AURA_REMOVE_BY_DEATH || !player->IsAlive())
                    break;
                // Never Saw It Coming: 10 s of ambush once out of the camouflage, whatever ended it.
                if (player->HasAura(SPELL_NSIC) && sSpellMgr->GetSpellInfo(SPELL_NSIC_WINDOW))
                    player->AddAura(SPELL_NSIC_WINDOW, player);
                // Arc Discharge, trigger B: unmasked by an enemy spell (dispel, reveal). Damage is trigger A.
                if (mode == AURA_REMOVE_BY_ENEMY_SPELL)
                    TryArcDischarge(player);
                break;
            case SPELL_UNMASKED:
                player->RemoveAurasDueToSpell(SPELL_FRENZY_PROC);
                player->RemoveAurasDueToSpell(SPELL_FRENZY);
                break;
            case SPELL_SELF_DESTRUCT:
                // Only the end of the countdown blows; a death (or a dispel) during the countdown does nothing.
                if (mode == AURA_REMOVE_BY_EXPIRE && player->IsAlive())
                    Defer(player, [](Player* apex) { SelfDestruct(apex); });
                break;
            default:
                break;
        }
    }
};

class AscensionApexTalentPlayerScript : public PlayerScript
{
public:
    AscensionApexTalentPlayerScript() : PlayerScript("AscensionApexTalentPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_CREATURE_KILL,
          PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

    // A node ultimate held for good that no node of the loaded CharacterAdvancement.dbc grants any more goes: Plasma
    // Barrage since Plasma Frenzy took node 138407 (27/10), and Plasma Frenzy itself if a later DBC drops it. The
    // talent state is read from the spellbook (AscensionCoATalentState::KnownRank): an orphan would stay castable
    // and cost nothing. Never on an empty catalog (a DBC that failed to load would take everything back).
    void OnPlayerLogin(Player* player) override
    {
        if (!player || player->getClass() != CLASS_TINKER || AscensionCompatData::CoATalentEntries.empty())
            return;
        for (uint32 const spellId : { uint32(SPELL_PLASMA_BARRAGE), uint32(SPELL_PLASMA_FRENZY) })
        {
            auto const owned = player->GetSpellMap().find(spellId);
            if (owned == player->GetSpellMap().end() || !owned->second ||
                owned->second->State == PLAYERSPELL_REMOVED || owned->second->State == PLAYERSPELL_TEMPORARY)
                continue;
            bool const granted = std::any_of(AscensionCompatData::CoATalentEntries.begin(),
                AscensionCompatData::CoATalentEntries.end(), [spellId](AscensionCompatData::CoATalentEntry const& entry)
                {
                    return entry.ClassId == CLASS_TINKER &&
                        std::find(entry.SpellIds.begin(), entry.SpellIds.end(), spellId) != entry.SpellIds.end();
                });
            if (granted)
                continue;
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
            LOG_INFO("module.ascension_compat", "Apex: {} lost {}, granted by no node any more", player->GetName(), spellId);
        }
    }

    // Optical Camouflage mends (CamouflageMend). Danger Seeker: once a second, the crit buff follows the enemies at
    // melee contact that attack the Apex.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!player || player->getClass() != CLASS_TINKER)
            return;
        CamouflageMend(player, diff);
        bool const talent = player->HasAura(SPELL_DANGER_SEEKER);
        if (!talent && !player->HasAura(SPELL_DANGER_SEEKER_BUFF))
            return;
        ApexTalentState& state = State(player);
        if (state.dangerSeekerTimer > diff)
        {
            state.dangerSeekerTimer -= diff;
            return;
        }
        state.dangerSeekerTimer = DANGER_SEEKER_PERIOD_MS;

        uint32 count = 0;
        if (talent && player->IsAlive())
            for (Unit* attacker : player->getAttackers())
                if (attacker && attacker->IsAlive() && player->IsWithinDist(attacker, DANGER_SEEKER_RANGE) &&
                    ++count >= DANGER_SEEKER_MAX)
                    break;
        Aura* buff = player->GetAura(SPELL_DANGER_SEEKER_BUFF);
        if (!count)
        {
            if (buff)
                player->RemoveAura(buff);
            return;
        }
        if (!buff && sSpellMgr->GetSpellInfo(SPELL_DANGER_SEEKER_BUFF))
            buff = player->AddAura(SPELL_DANGER_SEEKER_BUFF, player);
        if (buff && buff->GetStackAmount() != count)
            buff->SetStackAmount(uint8(count));
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override { NeverSawItComing(killer, killed); }
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { NeverSawItComing(killer, killed); }

    // Never Saw It Coming: a prey killed in the window without having struck the Apex gives one more Trophy (the
    // kill's own Trophy comes from Trophy Hunter), and its kin within 10 m of the body flee in terror for 3 s; a
    // player is only slowed. Decision D-6: kin = every enemy of the Apex there.
    static void NeverSawItComing(Player* killer, Unit* killed)
    {
        if (!killer || !killed || killer == killed || killer->getClass() != CLASS_TINKER || !killer->IsAlive() ||
            !killer->HasAura(SPELL_NSIC) || !killer->HasAura(SPELL_NSIC_WINDOW) ||
            killed->GetCharmerOrOwnerPlayerOrPlayerItself() == killer || !killer->isHonorOrXPTarget(killed))
            return;
        if (State(killer).ripostes.count(killed->GetGUID()))
            return;
        GrantApexTrophies(killer, 1);

        std::list<Unit*> around;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(killed, killer, NSIC_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(killed, around, check);
        Cell::VisitObjects(killed, searcher, NSIC_RADIUS);
        for (Unit* unit : around)
        {
            if (unit == killed || unit == killer || !unit->IsAlive() || !killer->IsValidAttackTarget(unit) ||
                (!unit->IsHostileTo(killer) && !unit->IsInCombatWith(killer)))
                continue;
            CastDeferred(killer, unit->GetGUID(), unit->IsPlayer() ? SPELL_NSIC_DREAD : SPELL_NSIC_TERROR);
        }
    }

    // The protocol is server -> client only: a COAAPEX whisper sent by a client is consumed, never forwarded.
    bool OnPlayerCanUseChat(Player* /*player*/, uint32 type, uint32 language, std::string& message,
                            Player* /*receiver*/) override
    {
        return !(type == CHAT_MSG_WHISPER && language == LANG_ADDON && message.starts_with(APEX_ADDON_PREFIX));
    }
};

uint8 TrophyCap(Player const* player)
{
    if (player->HasAura(SPELL_TROPHY_RACK_2))
        return 7;
    if (player->HasAura(SPELL_TROPHY_RACK_1))
        return 6;
    return 5;
}

int32 TrophyRackHealPct(Player const* player)
{
    if (player->HasAura(SPELL_TROPHY_RACK_2))
        return 6;
    if (player->HasAura(SPELL_TROPHY_RACK_1))
        return 3;
    return 0;
}
} // namespace

void SynchronizeApexStanceAttacks(Player* player)
{
    if (!player || player->getClass() != CLASS_TINKER)
        return;
    // learnSpell and removeSpell below run the learn/forget and aura hooks, which call back here.
    static thread_local bool running = false;
    if (running)
        return;
    running = true;

    bool const apex = player->GetLevel() >= APEX_LEVEL && GetAscensionActiveSpecialization(player) == SPEC_APEX;
    for (StancePair const& pair : STANCE_PAIRS)
    {
        bool const open = apex && player->GetLevel() >= pair.Level && player->HasActiveSpell(pair.Original) &&
            sSpellMgr->GetSpellInfo(pair.Target);
        if (!open)
        {
            player->SetTemporarySpellReplacement(pair.Original, 0);
            // Only the temporary copy: a target still granted for good (a node of an older DBC) stays.
            player->removeSpell(pair.Target, SPEC_MASK_ALL, true);
            continue;
        }
        // Kept learned in either stance (see the header): only the button changes. A tombstone (permanent copy
        // removed by ReconcileTinkerApex, not saved yet) is left alone: learning over it would resurrect it.
        if (player->GetSpellMap().find(pair.Target) == player->GetSpellMap().end())
            player->LearnTemporarySpellReplacementTarget(pair.Target);
        player->SetTemporarySpellReplacement(pair.Original, player->HasAura(pair.StanceAura) ? pair.Target : 0);
    }
    running = false;
}

uint32 GrantApexTrophies(Player* player, uint32 count)
{
    if (!player || !count || player->getClass() != CLASS_TINKER || !player->IsAlive() ||
        !player->HasSpell(SPELL_TROPHY_HUNTER) || !sSpellMgr->GetSpellInfo(SPELL_TROPHIES))
        return 0;
    uint8 const cap = TrophyCap(player);
    uint32 gained = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        Aura* trophies = player->GetAura(SPELL_TROPHIES);
        uint8 const before = trophies ? trophies->GetStackAmount() : 0;
        if (before >= cap)
        {
            if (trophies && before > cap)
                trophies->SetStackAmount(cap);
            break;
        }
        if (trophies)
            trophies->ModStackAmount(1);   // bounded by the DBC stack amount as well (5 until R3's DBC)
        else
            player->CastSpell(player, SPELL_TROPHIES, true);
        trophies = player->GetAura(SPELL_TROPHIES);
        if (!trophies)
            break;
        if (trophies->GetStackAmount() > cap)
            trophies->SetStackAmount(cap);
        if (trophies->GetStackAmount() <= before)
            break;
        gained += trophies->GetStackAmount() - before;
    }
    if (int32 const pct = TrophyRackHealPct(player); gained && pct && sSpellMgr->GetSpellInfo(SPELL_TROPHY_HEAL))
        player->CastCustomSpell(SPELL_TROPHY_HEAL, SPELLVALUE_BASE_POINT0, pct * int32(gained), player, true);
    return gained;
}

void AddAscensionTinkerApexTalentScripts()
{
    RegisterSpellScript(spell_apex_rocket_pounce);
    RegisterSpellScript(spell_apex_cannon_gate);
    RegisterSpellScript(spell_apex_plasma_bolt);
    RegisterSpellScript(spell_apex_skull_and_spine);
    RegisterSpellScript(aura_apex_trophy_hunter);
    RegisterSpellScript(spell_apex_mimicked_voice);
    RegisterSpellScript(spell_apex_grapple_snatch);
    RegisterSpellScript(spell_apex_smart_disc);
    RegisterSpellScript(spell_apex_razor_disc);
    RegisterSpellScript(aura_apex_patient_ambush);
    RegisterSpellScript(aura_apex_plasma_splash);
    RegisterSpellScript(spell_apex_plasma_splash_damage);
    RegisterSpellScript(aura_apex_plasma_frenzy);
    RegisterSpellScript(spell_apex_plasma_frenzy_shot);
    RegisterCreatureAI(npc_apex_hunting_drone);
    new AscensionApexTalentSpellScript();
    new AscensionApexDiscSpellInfoScript();
    new AscensionApexTalentUnitScript();
    new AscensionApexTalentPlayerScript();
    new AscensionApexSkullSpineWorldScript();
}
