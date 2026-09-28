/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionPooledVitality.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <vector>

namespace
{
enum BloodmageSecondarySpells : uint32
{
    SPELL_VEINBURST = 504260,
    SPELL_REAVE = 800490,
    // Reave's three copies, named after the condition each one pays. Read in Spell.dbc on 2026-09-28:
    // 805612 "Reave / Target Bleeding" carries TargetAuraState 18 (bleeding), 802883 "Reave / Behind
    // Target" carries none, 802884 "Reave / Target Low HP" carries 13. The two first were swapped
    // until then: the "behind" bit cast 805612, which the core rejects on a target that does not bleed
    // (TargetAuraState is checked on triggered casts too, Spell.cpp:2363), so "behind only" paid nothing.
    SPELL_REAVE_BLEED = 805612,
    SPELL_REAVE_EXECUTE = 802884,
    SPELL_REAVE_BACK = 802883,
    SPELL_HEMOBURST = 572855,
    SPELL_HEMOTURGY = 572856,
    SPELL_CURSED_FORM = 562720,
    SPELL_ACCURSED_FORM = 562572,
    SPELL_VAMPYRS_KISS = 504275,
    SPELL_VAMPYRS_KISS_COPY = 504785,
    SPELL_BLACK_HEART = 680731,
    SPELL_BLACK_HEART_ENERGIZE = 681031, // "Vampyr's Kiss (Energize)": ENERGIZE_PCT 20 on Rage
    SPELL_NIGHT_HUNTER = 704659,
    SPELL_BLOOD_FEAST_RESTORE = 706608,
    SPELL_ROTCLAW = 804197,
    SPELL_ROTCLAW_ENERGIZE = 805352, // Ravenous Strike (Energize): 30..70 internal, i.e. 3 to 7 Rage

    // Nettoyage du 2026-09-28.
    SPELL_LUNGE = 500126,
    SPELL_LUNGE_ENERGIZE = 804864,      // ENERGIZE 150 = 15 Rage
    SPELL_BLOODFANG_BITE = 800156,      // chain of ten ranks in spell_ranks
    SPELL_BITE_WOUND = 706654,
    SPELL_BITE_WOUND_SHARE = 532612,    // carries the 50 % of 706654's tooltip
    SPELL_BITE_WOUND_HEAL = 556234,
    SPELL_TRANSGRESSION = 801076,
    SPELL_VAMPIRIC_FEAST = 804934,
    SPELL_SANGUINE_ESSENCE = 680692,
    SPELL_SANGUINE_ESSENCE_HEAL = 680693,
    SPELL_SANGUINE_ESSENCE_DAMAGE = 681399,
    SPELL_SANGUINE_MEND = 802310,
    SPELL_BLOODMOON_BLAST = 500125,
    SPELL_AORTIC_ASSAULT_STRIKE = 806502,
    SPELL_RELENTLESS_BUFF = 807357,
    SPELL_RELENTLESS_COOLDOWN = 807358,
    SPELL_RELENTLESS_EXTEND = 807359,   // carries the 2000 ms
    SPELL_TALDARAMS_TORMENT = 800772,
    SPELL_INHUMANE = 807488,
    SPELL_BLOOD_SHARDS = 804849,        // Ferocity level 30 passive
    SPELL_BLOOD_SHARDS_SPEC = 807787,   // "Level 20 Passive" of the same name
    SPELL_BATTLEWEAVER = 801963,
    SPELL_BLOODCHASER = 523721,
    SPELL_EVERLASTING_HUNT = 804686,
    SPELL_EVERLASTING_HUNT_COOLDOWN = 802497,
    SPELL_BLOOD_SHARD = 504115,
    SPELL_BLOOD_SHARD_COUNTER = 505366, // "Gore Barrage Stack counter", StackAmount 8
    SPELL_BLOOD_SHARD_WINDOW = 505349,  // carries the $505349d of the tooltips
    SPELL_CURSED_FORM_MARKER = 525031,
    SPELL_CURSED_FORM_MARKER_2 = 524861,
    SPELL_CARDIAC_ARREST = 806944,
    SPELL_CARDIAC_ARREST_LEECH = 806946,
    SPELL_WAVES_OF_BLOOD = 681427,
    SPELL_WAVES_OF_BLOOD_HEAL = 807652,
    SPELL_CURSED_BLOOD = 707435,
    SPELL_CURSED_BLOOD_DAMAGE = 707708,
    SPELL_VAMPIRIC_FANG = 804726,
    SPELL_VAMPIRIC_FANG_HEAL_PASSIVE = 572373, // aura 354, 100 %, the value Dominion of Blood modifies
    SPELL_VAMPIRIC_FANG_HEAL = 572374,
    SPELL_THIRST = 706613,
    SPELL_THIRST_CLEANSER = 504109,     // "Thirst/Insatiable Cleanser": REMOVE_AURA 706613 and 706621
    SPELL_TORTURE = 504071,
    SPELL_TORTURE_EXTENDER = 561152,    // +5000 ms on Transgression 801076
    SPELL_ENDURING = 300585,
    SPELL_NIGHT_FEAST = 563736,
    SPELL_KILLER_INSTINCT = 704690,
    SPELL_APPETITE_FOR_BLOOD = 560479,
    SPELL_BLOOD_PLAGUE = 575335,
    SPELL_GORE_TOME = 807788,
    SPELL_CURSED_GROUND = 561195,
    SPELL_CURSED_GROUND_TRIGGER = 561304,
    SPELL_CURSED_GROUND_STACKER = 561205, // StackAmount 5: the "at least 5 enemies" of the tooltips

    // Troisieme passe du 2026-09-28.
    SPELL_BLOODSORE = 805592,           // e0 aura 271 on Sanguine Rupture / Bloodmoon Blast (word 1 0x202000)
    SPELL_WRETCHED = 504284,            // Essence Harvester's Rage and Thirst, "Vampiric Fang removes this effect"
    SPELL_BLOOD_FEAST = 706605,
    SPELL_ARTERIAL_BIND = 681077,
    SPELL_LIFES_ESSENCE = 520312,       // "and then causes you to burst, healing party and raid members"

    // Quatrieme passe du 2026-09-28.
    SPELL_DARK_LITURGY = 800781,        // spell_ranks head of its six ranks
    SPELL_BLOOD_RITUALS_MARK = 706623,  // the mark each Dark Liturgy rank leaves (effect 1, TRIGGER_SPELL)
    SPELL_BLOOD_RITUALS_HEAL = 704119,  // HEAL, TARGET_SRC_CASTER / TARGET_UNIT_SRC_AREA_ALLY, 50 yd
    SPELL_BLOOD_PRINCES_COMMAND = 704641 // e0 aura 107 SPELLMOD_EFFECT2 +100 on Taldaram's Torment
};

// Family 26 SpellFamilyFlags, counted on 2026-09-28 (each bit covers exactly the named ranks):
constexpr uint32 MASK0_CRIMSON_TIDE = 0x4000;       // word 0: 504282, 504129-504136
constexpr uint32 MASK0_VALANARS_VENGEANCE = 0x40000; // word 0: 560315 and its six ranks
constexpr uint32 MASK1_BLOODBOLT = 0x20000;         // word 1: the eight Bloodbolts
constexpr uint32 MASK1_RAVENOUS_STRIKE = 0x1;       // word 1: the ten Ravenous Strikes
constexpr uint32 MASK2_CLAW_SWEEP = 0x4000;         // word 2: the eight Claw Sweeps
constexpr uint32 MASK2_TALDARAMS_TORMENT = 0x200000; // word 2: the eight Taldaram's Torments

bool RankOf(uint32 id, uint32 root)
{
    return id == root || sSpellMgr->GetFirstSpellInChain(id) == root;
}

// Any rank of a spell_ranks chain known (a higher rank does not remove the lower one from the spell map).
bool KnowsAnyRank(Player const* player, uint32 root)
{
    for (uint32 id = root; id; id = sSpellMgr->GetNextSpellInChain(id))
        if (player->HasSpell(id))
            return true;
    return false;
}

Player* Bloodmage(Spell* spell)
{
    Player* player = spell->GetCaster()->ToPlayer();
    return player && player->getClass() == CLASS_SON_OF_ARUGAL &&
        spell->GetSpellInfo()->SpellFamilyName == 26 ? player : nullptr;
}

// Custom basepoints pass through native float arithmetic before conversion back to int32.
int32 ClampBasePoints(uint64 amount)
{
    uint32 maximum = uint32(std::nextafter(float(std::numeric_limits<int32>::max()), 0.0f));
    return int32(std::min<uint64>(amount, maximum));
}

void CopyDamage(Player* player, Unit* target, uint32 id, uint32 damage)
{
    if (!damage)
        return;
    player->CastCustomSpell(id, SPELLVALUE_BASE_POINT0, ClampBasePoints(damage), target, true);
}

// A percentage roll that a talent can raise: ProcChance read on the record, then the caster's
// SPELLMOD_CHANCE_OF_SUCCESS (the Xoroth trap of procedure-nettoyage-classe.md section 3).
bool RollTalent(Player* player, SpellInfo const* talent)
{
    if (!talent)
        return false;
    float chance = float(talent->ProcChance);
    player->ApplySpellMod(talent->Id, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    return roll_chance_f(chance);
}

// ---------------------------------------------------------------------------------------------
// Blood Shards (804849, 807787, 801963, 807488, 523721, 804686). Until 2026-09-28 the system did
// not exist: 506640 "Generate 1" and 506639 "Consume 1" are bare DUMMY effects no script handles.
// The data carries the pieces, and they are used as they are:
//   - the counter is 505366 "Gore Barrage Stack counter" (StackAmount 8, the "up to 8");
//   - its window is 505349's duration ($505349d in every tooltip, 600 s). 505366 itself has no
//     duration index, so the window is written onto the aura after each change;
//   - one shard is one 504115 "Blood Shard", cast straight on the target (505357-505364, the
//     "Trigger Attack" missiles, only relay 504115 and are not needed to deal the hit).
// The "$AP*x+$SP*y" terms of the tooltips are carried by no field (504115 has EffectBonusMultiplier 0 and
// no spell_bonus_data row, so the core adds no stat at all): each launching talent passes its own pair,
// read in its tooltip on 2026-09-28, and the shard is cast with base points + those terms. No
// NO_DONE_BONUS: the percentage done modifiers must still apply, and without a row nothing counts twice.
struct ShardCoefficients { float ap; float sp; };
constexpr ShardCoefficients ShardsLow = {0.05f, 0.1f};   // 804849 (Veinburst) and Battleweaver 801963
constexpr ShardCoefficients ShardsHigh = {0.15f, 0.35f}; // 807787 (Veinburst), Inhumane 807488, Bloodchaser 523721
void AddBloodShards(Player* player, uint32 count)
{
    SpellInfo const* counter = sSpellMgr->GetSpellInfo(SPELL_BLOOD_SHARD_COUNTER);
    SpellInfo const* window = sSpellMgr->GetSpellInfo(SPELL_BLOOD_SHARD_WINDOW);
    if (!count || !counter || !window || !player->IsAlive() || !player->IsInWorld())
        return;
    int32 const duration = window->GetMaxDuration();
    if (duration <= 0)
        return;
    uint32 const maximum = std::max<uint32>(counter->StackAmount, 1);
    Aura* aura = player->GetAura(SPELL_BLOOD_SHARD_COUNTER, player->GetGUID());
    uint32 stacks = aura ? aura->GetStackAmount() : 0;
    if (!aura)
    {
        aura = player->AddAura(SPELL_BLOOD_SHARD_COUNTER, player);
        if (!aura)
            return;
        stacks = aura->GetStackAmount();
        --count;
    }
    uint32 const wanted = std::min(stacks + count, maximum);
    if (wanted != aura->GetStackAmount())
        aura->SetStackAmount(uint8(wanted));
    aura->SetMaxDuration(duration);
    aura->SetDuration(duration);
}

uint32 TakeBloodShards(Player* player)
{
    Aura* aura = player->GetAura(SPELL_BLOOD_SHARD_COUNTER, player->GetGUID());
    if (!aura)
        return 0;
    uint32 const stacks = aura->GetStackAmount();
    aura->Remove();
    return stacks;
}

void LaunchBloodShards(Player* player, Unit* target, uint32 count, ShardCoefficients coefficients)
{
    SpellInfo const* shard = sSpellMgr->GetSpellInfo(SPELL_BLOOD_SHARD);
    if (!shard || !count)
        return;
    // CalcValue adds the die and the per-level gain on top of these base points, as for the stock record.
    double const bonus = double(player->GetTotalAttackPowerValue(BASE_ATTACK)) * coefficients.ap +
        double(player->SpellBaseDamageBonusDone(shard->GetSchoolMask())) * coefficients.sp;
    double const points = double(shard->Effects[EFFECT_0].BasePoints) + std::max(bonus, 0.0);
    int32 const basePoints = std::isfinite(points) ? ClampBasePoints(uint64(std::max(points, 0.0))) :
        shard->Effects[EFFECT_0].BasePoints;
    // The first shard can kill the target: stop as soon as it is gone.
    ObjectGuid const guid = target->GetGUID();
    for (uint32 i = 0; i < count; ++i)
    {
        Unit* victim = ObjectAccessor::GetUnit(*player, guid);
        if (!victim || !victim->IsAlive() || !player->IsValidAttackTarget(victim))
            return;
        player->CastCustomSpell(SPELL_BLOOD_SHARD, SPELLVALUE_BASE_POINT0, basePoints, victim, TRIGGERED_FULL_MASK);
    }
}

// Bloodsore (805592, applied by Bloodsores 805591): "Increases the damage of your NEXT Sanguine Rupture and
// Bloodmoon Blast against the target". Its aura 271 is a plain multiplier with ProcCharges 0: nothing ever
// took it away, so each stack amplified every hit for 8 s. It is spent here by the hit it has just
// amplified (the damage is computed before the hit hook). Blood Lord (704636, 707866): "a $s1% chance to
// provide its benefits without being consumed", a SPELLMOD_CHANCE_OF_SUCCESS of -15 / -30 on word 0 0x10,
// i.e. 805592; the amount is negative (procedure section 3), hence the sign. ApplySpellMod is called
// without the Spell so that a modifier with charges is not spent. Per target: Bloodsore is per target.
void ConsumeBloodsore(Player* player, Unit* target, SpellInfo const* info, uint32 damage)
{
    if (!damage || !target || target == player || !player->IsValidAttackTarget(target))
        return;
    Aura* sore = target->GetAura(SPELL_BLOODSORE, player->GetGUID());
    AuraEffect const* bonus = sore ? sore->GetEffect(EFFECT_0) : nullptr;
    if (!bonus || !bonus->IsAffectedOnSpell(info))
        return;
    float keep = 0.0f;
    player->ApplySpellMod(SPELL_BLOODSORE, SPELLMOD_CHANCE_OF_SUCCESS, keep);
    keep = std::clamp(-keep, 0.0f, 100.0f);
    if (!roll_chance_f(keep))
        sore->Remove();
}

bool HasBloodShards(Player const* player)
{
    return player->HasAura(SPELL_BLOOD_SHARDS) || player->HasAura(SPELL_BLOOD_SHARDS_SPEC);
}

// The target carries a disease of any caster ("against Diseased targets").
bool IsDiseased(Unit const* target)
{
    for (auto const& pair : target->GetAppliedAuras())
        if (pair.second->GetBase()->GetSpellInfo()->Dispel == DISPEL_DISEASE)
            return true;
    return false;
}

void ScaleHit(TargetInfo& hit, int32 pct)
{
    if (pct <= 0)
        return;
    uint64 const damage = uint64(hit.damage) * uint64(100 + pct) / 100;
    uint64 const before = uint64(hit.damageBeforeTakenMods) * uint64(100 + pct) / 100;
    hit.damage = int32(std::min<uint64>(damage, std::numeric_limits<int32>::max()));
    hit.damageBeforeTakenMods = int32(std::min<uint64>(before, std::numeric_limits<int32>::max()));
}

// Gore Tome (807788): "For the first 5 sec after activating a Cursed Form, your Bloodfang Bite is
// now guaranteed to critically strike." The activation time is written by
// AscensionBloodmageTalents.cpp on this very aura (script value key 807788). The 5 s is the
// tooltip's: 807788 has no duration and no other field states it.
constexpr uint64 GoreTomeWindowMs = 5000;

bool GoreTomeActive(Player const* player)
{
    Aura const* tome = player->GetAura(SPELL_GORE_TOME, player->GetGUID());
    uint64 const armed = tome ? tome->GetScriptValue(SPELL_GORE_TOME) : 0;
    uint64 const now = uint64(GameTime::GetGameTimeMS().count());
    return armed && now >= armed && now - armed <= GoreTomeWindowMs;
}

class bloodmage_secondary_casts : public AllSpellScript
{
public:
    bloodmage_secondary_casts() : AllSpellScript("bloodmage_secondary_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CALCULATED_TARGET, ALLSPELLHOOK_ON_CRIT_CHANCE,
            ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellCast(Spell* spell, Unit*, SpellInfo const* info, bool) override
    {
        Player* player = Bloodmage(spell);
        if (!player || spell->IsTriggered())
            return;
        // Black Heart (680731): "Vampyr's Kiss ... instantly regenerate $805912s1% of your maximum
        // Rage", once per cast. 805912 has no effect at all; the Energize record of the talent is
        // 681031 (ENERGIZE_PCT 20, Rage). It was paid on EVERY periodic tick copied by the curse
        // (bloodmage_kiss_periodic), i.e. six times per Kiss and on allies' ticks as well.
        if (RankOf(info->Id, SPELL_VAMPYRS_KISS) && player->HasAura(SPELL_BLACK_HEART))
            player->CastSpell(player, SPELL_BLACK_HEART_ENERGIZE, true);
        // Battleweaver (801963): "Casting Cursed Form abilities now has a $h% chance to generate 1
        // Blood Shard". Cursed Form abilities are the ones gated on either marker (the test of
        // bloodmage_dark_essence_casts, AscensionBloodmageEvents.cpp).
        if ((info->CasterAuraSpell == SPELL_CURSED_FORM_MARKER || info->CasterAuraSpell == SPELL_CURSED_FORM_MARKER_2))
            if (Aura const* weaver = player->GetAura(SPELL_BATTLEWEAVER, player->GetGUID()))
                if (RollTalent(player, weaver->GetSpellInfo()))
                    AddBloodShards(player, 1);
        // Dark Liturgy (800781, six ranks) marks the allies it heals with 706623, "Healed whenever the
        // Bloodmage uses Bloodmoon Blast", and its description ends on 706624's "Causes Bloodmoon Blast to
        // heal marked allies". The only record chain to that heal (680683 -> 704118 -> 704119) belongs to
        // another talent that nothing grants, and 706624 is granted by nothing: the heal had no engine. It is
        // cast here, once per cast ("whenever the Bloodmage uses"); spell_ascension_bloodmage_blood_rituals_heal
        // keeps only the allies marked by THIS Bloodmage. No spell_proc row: it would count twice.
        if (RankOf(info->Id, SPELL_BLOODMOON_BLAST) && KnowsAnyRank(player, SPELL_DARK_LITURGY))
            player->CastSpell(player, SPELL_BLOOD_RITUALS_HEAL, true);
        if (!player->HasAura(SPELL_NIGHT_HUNTER))
            return;
        if (RankOf(info->Id, SPELL_VEINBURST))
        {
            if (roll_chance_i(40))
                player->RemoveSpellCooldown(info->Id, true);
        }
        else if (AscensionBloodmage::GetEmpowerment(info->Id) == AscensionBloodmage::Bloodbolt &&
            info->PowerType == POWER_RAGE && spell->GetPowerCost() > 0 && roll_chance_i(40))
            player->ModifyPower(POWER_RAGE, spell->GetPowerCost() / 2);
    }

    void OnSpellCritChance(Spell* spell, Unit* target, float& chance) override
    {
        Player* player = Bloodmage(spell);
        if (!player || !target)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (RankOf(info->Id, SPELL_HEMOBURST) && target->HasAuraState(AURA_STATE_BLEEDING))
            chance = 100;
        if (spell->IsTriggered())
            return;
        // Gore Tome (807788).
        if (RankOf(info->Id, SPELL_BLOODFANG_BITE) && GoreTomeActive(player))
            chance = 100;
        // Killer Instinct (704690): "Each point of Rage now increases the critical strike chance of
        // Veinburst and Reave by $/1000;S1%, up to a maximum of 50%." Aura 112 with the private
        // selector 704690 has no engine; the per-point rate is its amount (500 = 0.5 %), the cap its
        // MiscValueB (50), and its mask (word 0 0x600000) names the two spells.
        if (AuraEffect const* instinct = player->GetAuraEffect(SPELL_KILLER_INSTINCT, EFFECT_0, player->GetGUID()))
            if (instinct->IsAffectedOnSpell(info))
            {
                float const rage = float(player->GetPower(POWER_RAGE)) / 10.0f;
                float const cap = float(std::max(instinct->GetMiscValueB(), 0));
                chance += std::min(rage * float(std::max(instinct->GetAmount(), 0)) / 1000.0f, cap);
            }
        // Blood Plague (575335) effect 0: "Increases the critical strike chance ... of Ravenous Strike
        // and Claw Sweep by $s1% against Diseased targets." The TOOLTIP is followed: the record's mask
        // (word 1 0x800001) names Ravenous Strike and Bloodfang Bite, not Claw Sweep.
        if (AuraEffect const* plague = player->GetAuraEffect(SPELL_BLOOD_PLAGUE, EFFECT_0, player->GetGUID()))
            if (((info->SpellFamilyFlags[1] & MASK1_RAVENOUS_STRIKE) || (info->SpellFamilyFlags[2] & MASK2_CLAW_SWEEP)) &&
                IsDiseased(target))
                chance += float(std::max(plague->GetAmount(), 0));
    }

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& hit) override
    {
        Player* player = Bloodmage(spell);
        if (!player || !target || hit.missCondition != SPELL_MISS_NONE)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        // Blood Prince's Command (704641): "Your Bloodbolt is now guaranteed to critically strike on enemies
        // affected by Taldaram's Torment." Its +100 lands on Torment's effect 1, an aura 308 the core sums
        // without reading its class mask (every spell and white hit of the Bloodmage then crit): that effect
        // is disarmed at load (bloodmage_secondary_contracts) and the crit granted here, after the roll
        // (Spell.cpp: roll_chance_f, then this hook; targetInfo.crit is read at hit time). Before the damage
        // test below: Bloodbolt is a HEALTH_LEECH, resolved at hit, so its launch damage is still 0 here.
        if (!hit.crit && (info->SpellFamilyFlags[1] & MASK1_BLOODBOLT) && target != player &&
            !info->HasAttribute(SPELL_ATTR2_CANT_CRIT) && info->IsCritCapable() &&
            player->HasAura(SPELL_BLOOD_PRINCES_COMMAND) &&
            target->GetAuraOfRankedSpell(SPELL_TALDARAMS_TORMENT, player->GetGUID()))
            hit.crit = true;
        if (hit.damage <= 0)
            return;
        uint32 id = info->Id;
        if (RankOf(id, SPELL_VEINBURST) && target->HasAuraState(AURA_STATE_BLEEDING))
        {
            hit.damage = uint32(hit.damage * 1.25f);
            hit.damageBeforeTakenMods = uint32(hit.damageBeforeTakenMods * 1.25f);
        }
        if (!spell->IsTriggered() && RankOf(id, SPELL_REAVE))
        {
            uint32 conditions = target->HasAuraState(AURA_STATE_BLEEDING) ? 1 : 0;
            if (target->HealthBelowPct(35))
                conditions |= 2;
            if (!target->HasInArc(float(M_PI), player))
                conditions |= 4;
            // Reave has one victim. Snapshot its conditions before the initial hit changes health.
            spell->SetScriptValue(SPELL_REAVE, conditions);
        }
        if (spell->IsTriggered())
            return;
        // Enduring (300585) and Night Feast (563736) scale Bloodbolt in bloodmage_kiss_periodic::
        // ModifySpellDamageTaken: Bloodbolt is a HEALTH_LEECH whose damage only exists at hit
        // (SpellEffects.cpp EffectHealthLeech), so this launch hook returned above with damage 0.
        // Appetite for Blood (560479): "+$s1% against Humanoids and Beasts" for the spells of its mask
        // (Bloodfang Bite, Claw Sweep); the creature-type mask 65 is its MiscValueB.
        if (AuraEffect const* appetite = player->GetAuraEffect(SPELL_APPETITE_FOR_BLOOD, EFFECT_0, player->GetGUID()))
            if (appetite->IsAffectedOnSpell(info) &&
                (target->GetCreatureTypeMask() & uint32(std::max(appetite->GetMiscValueB(), 0))))
                ScaleHit(hit, appetite->GetAmount());
        // Blood Plague (575335) effect 1: "... and critical damage ... by $s1%". Read as "a critical
        // strike deals $s1% more"; the crit is already rolled here (Spell.cpp:8541).
        if (hit.crit)
            if (AuraEffect const* plague = player->GetAuraEffect(SPELL_BLOOD_PLAGUE, EFFECT_1, player->GetGUID()))
                if (((info->SpellFamilyFlags[1] & MASK1_RAVENOUS_STRIKE) || (info->SpellFamilyFlags[2] & MASK2_CLAW_SWEEP)) &&
                    IsDiseased(target))
                    ScaleHit(hit, plague->GetAmount());
    }

    // Hits of triggered spells that talents watch: the strikes of Aortic Assault (806502, excludes
    // Clotting 806214, which shares its family bit) and the Blood Shard itself (504115).
    void OnTriggeredHit(Player* player, Unit* target, SpellInfo const* info, uint32 damage)
    {
        if (!damage || target == player || !player->IsValidAttackTarget(target))
            return;
        // Sanguine Rupture's damage record 800775 is triggered. Before the alive test: a killing hit has
        // used the Bloodsore too (the aura goes with the dead target anyway).
        ConsumeBloodsore(player, target, info, damage);
        if (!target->IsAlive())
            return;
        uint32 const id = info->Id;
        if (id == SPELL_AORTIC_ASSAULT_STRIKE)
        {
            // Relentless (807357): "each strike now extends the duration of Taldaram's Torment by
            // $/1000;807359s1 sec". 807359 resolves its aura with GetAura(800772), rank 1 only
            // (SpellEffects.cpp:464-482); any rank of the caster's own Torment is found here through
            // the spell_ranks chain, and the 2000 is read on 807359.
            if (player->HasAura(SPELL_RELENTLESS_BUFF, player->GetGUID()))
                if (Aura* torment = target->GetAuraOfRankedSpell(SPELL_TALDARAMS_TORMENT, player->GetGUID()))
                    if (SpellInfo const* extend = sSpellMgr->GetSpellInfo(SPELL_RELENTLESS_EXTEND))
                        if (int32 bonus = extend->Effects[EFFECT_0].CalcValue(player); bonus > 0)
                        {
                            int32 const duration = torment->GetDuration() + bonus;
                            if (duration > torment->GetMaxDuration())
                                torment->SetMaxDuration(duration);
                            torment->SetDuration(duration);
                        }
            // Inhumane (807488): "Dealing damage with Aortic Assault will now launch 1 Blood Shard".
            if (player->HasAura(SPELL_INHUMANE))
                LaunchBloodShards(player, target, 1, ShardsHigh);
        }
        // Everlasting Hunt (804686): "Damage dealt by Blood Shards now reduces the cooldown of
        // Veinburst and Reave by $/1000;802497S2 sec." 802497 carries both reductions.
        if (id == SPELL_BLOOD_SHARD && player->HasAura(SPELL_EVERLASTING_HUNT))
            player->CastSpell(player, SPELL_EVERLASTING_HUNT_COOLDOWN, true);
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32 healing, bool) override
    {
        Player* player = Bloodmage(spell);
        if (!player || !target || miss != SPELL_MISS_NONE)
            return;
        uint32 id = spell->GetSpellInfo()->Id;
        if (spell->IsTriggered())
        {
            OnTriggeredHit(player, target, spell->GetSpellInfo(), damage);
            return;
        }
        SpellInfo const* info = spell->GetSpellInfo();
        if (target == player && (id == SPELL_CURSED_FORM || id == SPELL_ACCURSED_FORM))
            // Native duration modifiers have already extended this application.
            player->RemoveAurasDueToSpell(SPELL_HEMOTURGY);

        // Sanguine Essence (680692): "While active, you replicate $s1% of healing or damage done by
        // Sanguine Mend and Bloodmoon Blast on the target every $680693t1 sec for $680693d." Effect 0
        // is aura 354 (no handler): the share is its amount (so the +50% of Unbearable Rage 801070
        // applies), spread over the copy's ticks (duration / amplitude, read). Placed before the
        // friendly-target return, since Sanguine Mend heals allies. The copies are triggered and never
        // come back here. The 20 % is read as the TOTAL of the three ticks: the tooltip does not say.
        {
            bool const mend = RankOf(id, SPELL_SANGUINE_MEND);
            bool const blast = !mend && RankOf(id, SPELL_BLOODMOON_BLAST);
            uint32 const done = mend ? healing : (blast ? damage : 0);
            if (done && target->IsAlive())
                if (AuraEffect const* essence = player->GetAuraEffect(SPELL_SANGUINE_ESSENCE, EFFECT_0, player->GetGUID()))
                    if (SpellInfo const* copy = sSpellMgr->GetSpellInfo(mend ? SPELL_SANGUINE_ESSENCE_HEAL : SPELL_SANGUINE_ESSENCE_DAMAGE))
                        if (!blast || player->IsValidAttackTarget(target))
                        {
                            int32 const amplitude = copy->Effects[EFFECT_0].Amplitude;
                            int32 const duration = copy->GetMaxDuration();
                            uint32 const ticks = (amplitude > 0 && duration >= amplitude) ? uint32(duration / amplitude) : 1;
                            uint64 const total = uint64(done) * uint64(std::max(essence->GetAmount(), 0)) / 100;
                            if (uint64 const perTick = total / ticks)
                                player->CastCustomSpell(copy->Id, SPELLVALUE_BASE_POINT0, ClampBasePoints(perTick), target, true);
                        }
        }

        if (target == player || player->IsFriendlyTo(target))
            return;
        // Bloodmoon Blast (not triggered) spends its Bloodsore here, as Sanguine Rupture does in OnTriggeredHit.
        ConsumeBloodsore(player, target, info, damage);
        if (RankOf(id, SPELL_HEMOBURST) && !spell->GetScriptValue(SPELL_HEMOTURGY))
        {
            spell->SetScriptValue(SPELL_HEMOTURGY, 1);
            player->CastSpell(player, SPELL_HEMOTURGY, true);
        }
        // Rotclaw's description promises the income ("dealing ... Shadow damage, generating Rage and
        // infecting their wounds"), but none of its three effects is an energize and no companion
        // "Rotclaw (Energize)" record exists, unlike Ravenous Strike, Bloodmoon Blast and Sanguine
        // Rupture, which carry effect 142 pointing at their own Energize spell. Unit::DealDamage only
        // pays Rage for weapon damage, so the ability granted none. The amount is in no source - not
        // Spell.dbc, not spell_proc/spell_linked_spell, not the 2026-09-13 exiles-db export - so the
        // Rage is paid with Ravenous Strike's own Energize record rather than a new number, and once per
        // cast: Rotclaw hits up to 25 enemies.
        if (RankOf(id, SPELL_ROTCLAW) && !spell->GetScriptValue(SPELL_ROTCLAW_ENERGIZE))
        {
            spell->SetScriptValue(SPELL_ROTCLAW_ENERGIZE, 1);
            player->CastSpell(player, SPELL_ROTCLAW_ENERGIZE, true);
        }
        // Lunge (500126): "dazing them for $/1000;s3 sec, and generating $/10;804864s1 Rage". Lunge has
        // NO effect 142 (contrary to what the comment above said until 2026-09-28): its effect 2 is the
        // unhandled 178 (EffectNULL, SpellEffects.cpp:250) carrying Dazed 50259, and nothing cast its
        // Energize 804864. Paid on hit, once per cast, as Rotclaw.
        if (id == SPELL_LUNGE && !spell->GetScriptValue(SPELL_LUNGE))
        {
            spell->SetScriptValue(SPELL_LUNGE, 1);
            if (uint32 daze = info->Effects[EFFECT_2].TriggerSpell)
                player->CastSpell(target, daze, true);
            player->CastSpell(player, SPELL_LUNGE_ENERGIZE, true);
        }
        // Bloodfang Bite (800156, ten ranks): "... and creates a Bite Wound." No rank carries it; 706654
        // was reachable only through 706655/532612, which nothing grants. The heal of the wound is
        // aura_ascension_bloodmage_bite_wound below.
        if (RankOf(id, SPELL_BLOODFANG_BITE) && target->IsAlive())
            player->CastSpell(target, SPELL_BITE_WOUND, true);
        if (!damage)
            return;
        if (RankOf(id, SPELL_REAVE) && !spell->GetScriptValue(SPELL_REAVE_BLEED))
        {
            spell->SetScriptValue(SPELL_REAVE_BLEED, 1);
            uint32 conditions = uint32(spell->GetScriptValue(SPELL_REAVE));
            if (conditions & 1)
                CopyDamage(player, target, SPELL_REAVE_BLEED, damage);
            if (conditions & 2)
                CopyDamage(player, target, SPELL_REAVE_EXECUTE, damage);
            if (conditions & 4)
                CopyDamage(player, target, SPELL_REAVE_BACK, damage);
        }
        // Transgression (801076): "While active, direct spell damage triggers a Vampiric Feast, dealing
        // $801076s3% additional Shadow damage to enemies, healing you equal to 50% of the damage dealt."
        // Effect 2 is aura 354: its amount is the share. 804934 is a HEALTH_LEECH of multiplier 0.5
        // (the native 50 % heal) that receives an already-resolved amount (contract below), so the
        // only caster modifier re-applied is its own spell modifier: Torture 504071, "+50% leech".
        if (info->DmgClass == SPELL_DAMAGE_CLASS_MAGIC)
            if (AuraEffect const* transgression = player->GetAuraEffect(SPELL_TRANSGRESSION, EFFECT_2, player->GetGUID()))
            {
                uint32 feast = uint32(uint64(damage) * uint64(std::max(transgression->GetAmount(), 0)) / 100);
                player->ApplySpellMod(SPELL_VAMPIRIC_FEAST, SPELLMOD_DAMAGE, feast);
                CopyDamage(player, target, SPELL_VAMPIRIC_FEAST, feast);
            }
        // Cardiac Arrest (806944): "Your Valanar's Vengeance now deals an additional 50% of its initial
        // damage dealt over $806946d, healing you for 50% of the damage dealt." The amount (13) is the
        // per-tick share: 806946 ticks every 500 ms for 2000 ms, 4 x 13 = 52 %. The heal is the leech's
        // native multiplier (0.5).
        if (info->SpellFamilyFlags[0] & MASK0_VALANARS_VENGEANCE)
            if (AuraEffect const* arrest = player->GetAuraEffect(SPELL_CARDIAC_ARREST, EFFECT_0, player->GetGUID()))
                if (uint64 const perTick = uint64(damage) * uint64(std::max(arrest->GetAmount(), 0)) / 100)
                    player->CastCustomSpell(SPELL_CARDIAC_ARREST_LEECH, SPELLVALUE_BASE_POINT0, ClampBasePoints(perTick),
                        target, true);
        // Vampiric Fang (804726, eight ranks): "Steal ... health from an enemy." The ranks only deal
        // damage; the steal is the hidden passive 572373 (aura 354, 100 %) that nothing grants and no
        // engine reads. Its value is taken through ApplyEffectModifiers, so Dominion of Blood 680716
        // (SPELLMOD_EFFECT1 +50 on word 1 0x40000000, i.e. 572373) makes it 150 %. 572373 is not given
        // to the player: its aura 354 would count twice the day someone wires it.
        if (RankOf(id, SPELL_VAMPIRIC_FANG))
            if (SpellInfo const* steal = sSpellMgr->GetSpellInfo(SPELL_VAMPIRIC_FANG_HEAL_PASSIVE))
            {
                float const pct = player->ApplyEffectModifiers(steal, EFFECT_0, float(steal->Effects[EFFECT_0].CalcValue(player)));
                if (std::isfinite(pct) && pct > 0.0f)
                    if (uint64 const heal = uint64(double(damage) * double(pct) / 100.0))
                        player->CastCustomSpell(SPELL_VAMPIRIC_FANG_HEAL, SPELLVALUE_BASE_POINT0, ClampBasePoints(heal),
                            player, TRIGGERED_FULL_MASK);
            }
        // Vampiric Fang: "Expends Thirst Stacks ... Clears Insatiable and Thirst stacks." Its damage
        // bonus per stack is already in the hit (680753, computed at launch). The clear is 504109,
        // which nothing cast: without it Thirst, now generated by 92112, would pin the Bloodmage in
        // Insatiable. Torture (504071): "Consuming at least 9 stacks of Thirst with Vampiric Fang now
        // extends the duration of Transgression by $/1000;561152s1 sec." The 9 is the tooltip's.
        if (RankOf(id, SPELL_VAMPIRIC_FANG) && !spell->GetScriptValue(SPELL_THIRST_CLEANSER))
        {
            spell->SetScriptValue(SPELL_THIRST_CLEANSER, 1);
            // Wretched (504284, Essence Harvester): "Vampiric Fang removes this effect." Removed first, and
            // even with no Thirst to clear, so that no Thirst tick lands after the clear.
            player->RemoveAurasDueToSpell(SPELL_WRETCHED, player->GetGUID());
            Aura const* thirst = player->GetAura(SPELL_THIRST);
            uint32 const consumed = thirst ? thirst->GetStackAmount() : 0;
            if (consumed >= 9 && player->HasAura(SPELL_TORTURE))
                player->CastSpell(player, SPELL_TORTURE_EXTENDER, true);
            if (consumed)
                player->CastSpell(player, SPELL_THIRST_CLEANSER, true);
        }
        // Blood Shards.
        if (RankOf(id, SPELL_VEINBURST) && HasBloodShards(player) && !spell->GetScriptValue(SPELL_BLOOD_SHARD_COUNTER))
        {
            // "Dealing damage with Veinburst will now expend all stacks of Blood Shards".
            spell->SetScriptValue(SPELL_BLOOD_SHARD_COUNTER, 1);
            // DECISION: both passives describe the same spend with two coefficient pairs; with both known,
            // the higher one (807787's) is used.
            LaunchBloodShards(player, target, TakeBloodShards(player),
                player->HasAura(SPELL_BLOOD_SHARDS_SPEC) ? ShardsHigh : ShardsLow);
        }
        else if ((info->SpellFamilyFlags[1] & MASK1_BLOODBOLT) && player->HasAura(SPELL_BATTLEWEAVER) &&
            !spell->GetScriptValue(SPELL_BATTLEWEAVER))
        {
            // Battleweaver: "Dealing damage with Bloodbolt will launch 1 Blood Shard". The tooltip does
            // not say a shard is spent, so none is.
            spell->SetScriptValue(SPELL_BATTLEWEAVER, 1);
            LaunchBloodShards(player, target, 1, ShardsLow);
        }
        else if (RankOf(id, SPELL_BLOODFANG_BITE) && player->HasAura(SPELL_BLOODCHASER, player->GetGUID()))
            // Bloodchaser (523721): "While active, Bloodfang Bite now launches 1 Blood Shard".
            LaunchBloodShards(player, target, 1, ShardsHigh);
    }
};

// 704119 Blood Rituals heal: the allies of its area that carry THIS Bloodmage's mark (706623 is cast by the
// Dark Liturgy caster, effect 64), and no one else. No marked ally: an empty list, the heal lands nowhere.
class spell_ascension_bloodmage_blood_rituals_heal : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_blood_rituals_heal);

    void Select(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        ObjectGuid const guid = caster ? caster->GetGUID() : ObjectGuid::Empty;
        targets.remove_if([guid](WorldObject* object)
        {
            Unit* unit = object ? object->ToUnit() : nullptr;
            return !unit || guid.IsEmpty() || !unit->HasAura(SPELL_BLOOD_RITUALS_MARK, guid);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(
            spell_ascension_bloodmage_blood_rituals_heal::Select, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
    }
};

class bloodmage_kiss_periodic : public UnitScript
{
public:
    bloodmage_kiss_periodic() : UnitScript("bloodmage_kiss_periodic", true,
        {UNITHOOK_ON_PERIODIC_DAMAGE_RESULT, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN}) { }

    void OnPeriodicDamageResult(Unit* target, Unit* attacker, uint32 damage, SpellInfo const* info) override
    {
        if (!target || !target->IsAlive() || !damage)
            return;

        // Talents that read the Bloodmage's own ticks.
        if (Player* player = attacker ? attacker->ToPlayer() : nullptr;
            player && player->getClass() == CLASS_SON_OF_ARUGAL && info && info->SpellFamilyName == 26 &&
            player->IsAlive() && player->IsInWorld() && player != target)
        {
            // Waves of Blood (681427): "Each tick of Crimson Tide now has a $681427h% chance to heal a
            // nearby ally for $681427s1% of the damage dealt." Aura 354, ProcFlags 0: no engine. The
            // heal 807652 picks its one ally itself (MaxAffectedTargets 1).
            if (info->SpellFamilyFlags[0] & MASK0_CRIMSON_TIDE)
                if (AuraEffect const* waves = player->GetAuraEffect(SPELL_WAVES_OF_BLOOD, EFFECT_0, player->GetGUID()))
                    if (uint64 const heal = uint64(damage) * uint64(std::max(waves->GetAmount(), 0)) / 100)
                        if (RollTalent(player, waves->GetSpellInfo()))
                            player->CastCustomSpell(SPELL_WAVES_OF_BLOOD_HEAL, SPELLVALUE_BASE_POINT0, ClampBasePoints(heal),
                                player, true);
            // Cursed Blood (707435): "Damage dealt by Taldaram's Torment now has a $h% chance to
            // internally rupture, causing it to deal its damage an additional time to all enemies near
            // the target." The chance is the talent's (20), not the ProcChance of 707708 (33); the
            // share is effect 0's amount (100). 707708 resolves its area around the target (tA 6 /
            // tB 16, SUPPOSE, to be seen in game).
            if (RankOf(info->Id, SPELL_TALDARAMS_TORMENT) && player->IsValidAttackTarget(target))
                if (AuraEffect const* cursed = player->GetAuraEffect(SPELL_CURSED_BLOOD, EFFECT_0, player->GetGUID()))
                    if (uint64 const copy = uint64(damage) * uint64(std::max(cursed->GetAmount(), 0)) / 100)
                        if (RollTalent(player, cursed->GetSpellInfo()))
                            CopyDamage(player, target, SPELL_CURSED_BLOOD_DAMAGE, uint32(std::min<uint64>(copy, std::numeric_limits<uint32>::max())));
            if (!target->IsAlive())
                return;
        }

        // The active curse copies all periodic damage to its victim, including allied casters' ticks.
        // Collect owner GUIDs before casting, since a copy can kill the target and remove its auras.
        std::vector<ObjectGuid> owners;
        for (auto const& pair : target->GetAppliedAuras())
        {
            Aura* aura = pair.second->GetBase();
            if (RankOf(aura->GetId(), SPELL_VAMPYRS_KISS) &&
                std::find(owners.begin(), owners.end(), aura->GetCasterGUID()) == owners.end())
                owners.push_back(aura->GetCasterGUID());
        }
        for (ObjectGuid const& guid : owners)
            if (Unit* unit = ObjectAccessor::GetUnit(*target, guid))
                if (Player* player = unit->ToPlayer(); player && player->getClass() == CLASS_SON_OF_ARUGAL &&
                    player->IsAlive() && player->IsInWorld() && player->InSamePhase(target) && target->IsAlive() &&
                    player->IsValidAttackTarget(target))
                    // Black Heart used to pay 20 % of maximum Rage here, on every copied tick. It is now
                    // paid once per Kiss cast (bloodmage_secondary_casts::OnSpellCast).
                    CopyDamage(player, target, SPELL_VAMPYRS_KISS_COPY, damage / 4);
    }

    // Enduring (300585): "Increases the damage of Bloodbolt by $s2%, plus an additional $s2% while below
    // 35% health." The permanent half is native (effect 1, aura 108). The conditional half sits on an aura
    // 112 with the raw selector 20007 that nothing honours; the tooltip names the Bloodmage's own health,
    // so it is tested on the caster, with effect 1's amount.
    // Night Feast (563736): "Each point of Rage now increases the damage of Bloodbolt by 1%". The 1 is
    // effect 0's amount; the Rage cost penalty (effect 1, aura 107) stays native.
    // Both used to sit in bloodmage_secondary_casts::OnSpellCalculatedTarget, after its `damage <= 0`
    // return: Bloodbolt's effect 0 is a HEALTH_LEECH resolved at hit (EffectHealthLeech), so its launch
    // damage is 0 and neither bonus ever applied. Here (Unit::CalculateSpellDamageTaken, before crit and
    // mitigation) the leech damage exists, and the heal is taken from the final damage (Spell.cpp).
    // DECISION: this hook carries no Spell, so triggered Bloodbolts (none known) would be scaled too.
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* info) override
    {
        if (damage <= 0 || !info || !target || info->SpellFamilyName != 26 || !(info->SpellFamilyFlags[1] & MASK1_BLOODBOLT))
            return;
        Player* player = attacker ? attacker->ToPlayer() : nullptr;
        if (!player || player == target || player->getClass() != CLASS_SON_OF_ARUGAL || !player->IsAlive())
            return;
        // Multiplicative, as the two ScaleHit calls were. Rage is read at hit, after the cost is paid.
        uint64 value = uint64(damage);
        if (player->HealthBelowPct(35))
            if (AuraEffect const* enduring = player->GetAuraEffect(SPELL_ENDURING, EFFECT_1, player->GetGUID()))
                value = value * uint64(100 + std::max(enduring->GetAmount(), 0)) / 100;
        if (AuraEffect const* feast = player->GetAuraEffect(SPELL_NIGHT_FEAST, EFFECT_0, player->GetGUID()))
            value = value * (100 + uint64(player->GetPower(POWER_RAGE) / 10) * uint64(std::max(feast->GetAmount(), 0))) / 100;
        damage = int32(std::min<uint64>(value, std::numeric_limits<int32>::max()));
    }
};

class bloodmage_secondary_contracts : public GlobalScript
{
public:
    bloodmage_secondary_contracts() : GlobalScript("bloodmage_secondary_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != 26)
            return;
        // Shares of damage that has already been resolved once against this same target: they must
        // not be mitigated, buffed or critted a second time. 804934 (Vampiric Feast), 806946 (Cardiac
        // Arrest) and 707708 (Cursed Blood) joined the Reave and Kiss copies on 2026-09-28.
        if (info->Id == SPELL_REAVE_BLEED || info->Id == SPELL_REAVE_EXECUTE || info->Id == SPELL_REAVE_BACK ||
            info->Id == SPELL_VAMPYRS_KISS_COPY || info->Id == SPELL_VAMPIRIC_FEAST ||
            info->Id == SPELL_CARDIAC_ARREST_LEECH || info->Id == SPELL_CURSED_BLOOD_DAMAGE)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        // The Blood Shard counter is a transient resource with a written-in window; a copy restored
        // from `character_aura` would come back without it. Same idiom (and same three-flag overlap,
        // SpellInfo.h:206/188/207) as AscensionBloodmageEvents.cpp.
        if (info->Id == SPELL_BLOOD_SHARD_COUNTER)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
        // Gore Tome (807788): its only effect is +150% VALUE_MULTIPLIER on Bloodbolt's leech, which no
        // tooltip mentions; the tooltip promises a guaranteed Bloodfang Bite critical strike (handled in
        // bloodmage_secondary_casts). The record is followed where the player reads it.
        // Blood Prince's Command, the other half (see OnSpellCalculatedTarget): the eight Torments' effect 1,
        // aura 308 with Bloodbolt's mask, bp -1. Only 704641 raises it; left armed, Unit::SpellDoneCritChance
        // and the melee crit roll (Unit.cpp) add it to every attack against the target.
        if (info->SpellFamilyFlags[2] & MASK2_TALDARAMS_TORMENT)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_CRIT_CHANCE_FOR_CASTER &&
                effect.SpellClassMask == flag96(0, MASK1_BLOODBOLT, 0))
                effect.ApplyAuraName = SPELL_AURA_DUMMY;
        }
        if (info->Id == SPELL_GORE_TOME)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER &&
                effect.MiscValue == SPELLMOD_VALUE_MULTIPLIER && effect.SpellClassMask == flag96(0, MASK1_BLOODBOLT, 0))
                effect.ApplyAuraName = SPELL_AURA_DUMMY;
        }
    }
};

// 804849 Blood Shards (Ferocity level 30) and 807787 (its level 20 namesake): "Shadow Damage dealt
// now generates 1 Blood Shard, plus 1 additional if it critically strikes" (807787: "Damage dealt by
// Bloodmoon Blast"). The filters are the spell_proc rows (school 32 / Bloodmoon Blast mask); the
// native trigger 506640 is an empty DUMMY and is replaced by the counter above.
class aura_ascension_bloodmage_blood_shards : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_blood_shards);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BLOOD_SHARD_COUNTER, SPELL_BLOOD_SHARD_WINDOW, SPELL_BLOOD_SHARD});
    }

    bool Check(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return eventInfo.GetActor() == GetTarget() && damage && damage->GetDamage();
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            AddBloodShards(player, (eventInfo.GetHitMask() & PROC_HIT_CRITICAL) ? 2 : 1);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_blood_shards::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_blood_shards::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 704652 Relentless (Ferocity level 40): "Physical critical strikes now reduce the cooldown of Aortic
// Assault by $/1000;807358s1 sec and grant you Relentless." The spell_proc row lets the native proc
// grant 807357; 807358 has no caster at all, so it is cast after it.
class aura_ascension_bloodmage_relentless : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_relentless);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_RELENTLESS_COOLDOWN}); }

    void Reduce(AuraEffect const* /*effect*/, ProcEventInfo& /*eventInfo*/)
    {
        GetTarget()->CastSpell(GetTarget(), SPELL_RELENTLESS_COOLDOWN, true);
    }

    void Register() override
    {
        AfterEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_relentless::Reduce, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 706654 Bite Wound, held by the enemy: "Dealing damage to this target with auto attacks now restores
// health to you equal to $532612s1% of the damage dealt." The spell_proc row reacts to
// TAKEN_MELEE_AUTO_ATTACK; only the wound's own caster is healed. The heal goes through 556234
// "Bite Wound | Heal", so Festering Maw 706683 (filtered on 556234) finally has something to see.
// DECISION: 556235/556234's own texts describe a flat heal on "Cursed Fang or Cursed Rage", two
// spells that do not exist in Spell.dbc; the 50 % of the damage of 706654's tooltip is the one applied.
class aura_ascension_bloodmage_bite_wound : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_bite_wound);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BITE_WOUND_SHARE, SPELL_BITE_WOUND_HEAL});
    }

    bool Check(ProcEventInfo& eventInfo)
    {
        Unit* caster = GetCaster();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return caster && caster->IsAlive() && eventInfo.GetActor() == caster &&
            eventInfo.GetActionTarget() == GetTarget() && damage && damage->GetDamage();
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetCaster();
        SpellInfo const* share = sSpellMgr->GetSpellInfo(SPELL_BITE_WOUND_SHARE);
        if (!caster || !share)
            return;
        int32 const pct = std::clamp(share->Effects[EFFECT_0].CalcValue(caster), 0, 100);
        if (uint64 const heal = uint64(eventInfo.GetDamageInfo()->GetDamage()) * uint64(pct) / 100)
            caster->CastCustomSpell(SPELL_BITE_WOUND_HEAL, SPELLVALUE_BASE_POINT0, ClampBasePoints(heal), caster,
                TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_bite_wound::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_bite_wound::Proc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Sanguine Rupture, all eight ranks (spell_script_names -800774). Effect 1 is a TRIGGER_SPELL on
// 561304 "Cursed Ground | Trigger" for every enemy hit, so EVERY Bloodmage bled every target for
// 4 ticks, talent or not. The tooltips of the talent 561195 and of 561304 both say "when you strike at
// least 5 enemies": the effect now goes through only with the talent and at least that many targets.
// The 5 is 561205 "Cursed Ground | Stacker"'s StackAmount, the only field that holds it.
class spell_ascension_bloodmage_cursed_ground_gate : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_cursed_ground_gate);

    uint32 _struck = 0;

    void Count(std::list<WorldObject*>& targets)
    {
        _struck = uint32(targets.size());
    }

    void Gate(SpellEffIndex index)
    {
        if (GetSpellInfo()->Effects[index].TriggerSpell != SPELL_CURSED_GROUND_TRIGGER)
            return;
        SpellInfo const* stacker = sSpellMgr->GetSpellInfo(SPELL_CURSED_GROUND_STACKER);
        uint32 const needed = stacker && stacker->StackAmount ? stacker->StackAmount : 5;
        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_CURSED_GROUND) && _struck >= needed)
            return;
        PreventHitDefaultEffect(index);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ascension_bloodmage_cursed_ground_gate::Count,
            EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_bloodmage_cursed_ground_gate::Gate, EFFECT_1,
            SPELL_EFFECT_TRIGGER_SPELL);
    }
};

class spell_ascension_blood_feast_corpses : public SpellScript
{
    PrepareSpellScript(spell_ascension_blood_feast_corpses);

    void Select(std::list<WorldObject*>& targets)
    {
        Unit* caster = GetCaster();
        // Filter before the native three-target cap so living units cannot take corpse slots.
        targets.remove_if([caster](WorldObject* object)
        {
            Unit* target = object ? object->ToUnit() : nullptr;
            return !target || target->IsAlive() || target == caster || caster->IsFriendlyTo(target);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ascension_blood_feast_corpses::Select,
            EFFECT_0, TARGET_UNIT_SRC_AREA_ENTRY);
    }
};

class spell_ascension_blood_feast_drain : public SpellScript
{
    PrepareSpellScript(spell_ascension_blood_feast_drain);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_BLOOD_FEAST_RESTORE}); }

    void Drain(SpellEffIndex)
    {
        Player* player = GetCaster()->ToPlayer();
        Unit* corpse = GetHitUnit();
        if (player && player->getClass() == CLASS_SON_OF_ARUGAL && player->IsAlive() && !player->IsInCombat() &&
            corpse && !corpse->IsAlive() && corpse != player && !player->IsFriendlyTo(corpse))
            player->CastSpell(player, SPELL_BLOOD_FEAST_RESTORE, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_blood_feast_drain::Drain, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// 706605 Blood Feast, the channel itself: "$?s681077[, and then causes you to burst, healing party and raid
// members within $520312a1 yds for ...]". Life's Essence 520312 was reachable only through 520313, which
// nothing applies (and whose tick would land during the channel, not "then"). A channel that runs to its
// end lets its aura expire (AURA_REMOVE_BY_EXPIRE); an interrupt or a cancel removes it with
// AURA_REMOVE_BY_CANCEL (Spell::cancel), and pays nothing.
class aura_ascension_bloodmage_blood_feast : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_blood_feast);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_LIFES_ESSENCE}); }

    void Burst(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        AuraApplication const* application = GetTargetApplication();
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || !player ||
            player != GetTarget() || player->getClass() != CLASS_SON_OF_ARUGAL || !player->IsAlive() ||
            !player->IsInWorld() || !player->HasSpell(SPELL_ARTERIAL_BIND))
            return;
        player->CastSpell(player, SPELL_LIFES_ESSENCE, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_blood_feast::Burst, EFFECT_0,
            SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};
}

void AddSC_AscensionBloodmageSecondary()
{
    new bloodmage_secondary_casts();
    new bloodmage_kiss_periodic();
    new bloodmage_secondary_contracts();
    RegisterSpellScript(spell_ascension_blood_feast_corpses);
    RegisterSpellScript(spell_ascension_blood_feast_drain);
    // Nettoyage du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql).
    RegisterSpellScript(aura_ascension_bloodmage_blood_shards);
    RegisterSpellScript(aura_ascension_bloodmage_relentless);
    RegisterSpellScript(aura_ascension_bloodmage_bite_wound);
    RegisterSpellScript(spell_ascension_bloodmage_cursed_ground_gate);
    // Troisieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 5-7).
    RegisterSpellScript(aura_ascension_bloodmage_blood_feast);
    // Quatrieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 8-11).
    RegisterSpellScript(spell_ascension_bloodmage_blood_rituals_heal);
}
