/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

// BLOODMAGE (CLASS_SON_OF_ARUGAL = 20, SpellFamilyName 26 = class id + 6, read at field 208 of
// Spell.dbc on 2026-09-20 — field 149 reads 0 for every record in the file and is not the family).
//
// This file wires the nine talents of the list whose Spell.dbc record states a promise the core
// cannot keep on its own. Every number below was read, never guessed; what could not be read is
// named in the comments and left out rather than invented.
//
//   680680 Atherann's Anguish  accumulate 30% of the damage you deal, explode on natural expiry
//   681403 Infuse              accumulate the group's damage, unleash on natural expiry
//   570023 Thirst for Blood    Thirst 1-5 -> Sated (570024), 6-10 -> Ravenous (570025)
//   704662 Shadows In The Night  the authored -5% damage taken, but only above 75% health
//   560259 Vampyr Lord         +35% damage, restricted to the Animated Blood summons
//   706683 Festering Maw       its proc restricted to Bite Wound's heal (556234)
//   704626 Thick Pelt          the attack-power part of 556233's flat reduction
//   704115 Clotting            gate Aortic Assault's closing cone on the talent, +25% on bleeders
//   680732 Dark Essence        the periodic heal on Blood Rituals allies, and only on them
//
// ONE FIELD INDEX WORTH WRITING DOWN. Spell.dbc's EffectRadiusIndex is field 92+e, NOT 104+e
// (104+e is EffectChainTarget). Witnesses read on 2026-09-20: Arcane Explosion 1449 has
// field 92 = 13 and SpellRadius entry 13 is 10 yards; Chain Lightning 421 has field 92 = 0 and
// field 104 = 3, its three jumps. Likewise EffectBonusMultiplier is 229+e and EffectDamageMultiplier
// 216+e: Chain Lightning reads 0.571 at 229 (its stock coefficient) and 0.7 at 216 (the per-jump
// falloff). A first pass of this task read the radius at 104+e, concluded that Clotting and Dark
// Essence had no area at all, and was about to discard both; they are wired below because they do.

#include "AscensionPooledVitality.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <list>

namespace
{
enum BloodmageEventSpells : uint32
{
    // "Unleash a hemoplague upon the target area, accumulating 30% of the damage you deal to
    // affected enemies for $d, and then exploding for that amount of Shadow damage."
    SPELL_ANGUISH = 680680,
    SPELL_ANGUISH_DAMAGE = 680681,
    // "Accumulates damage dealt to the target by you, your party or raid members, and their
    // minions for $d, then unleashes the stored amount as Shadow damage."
    SPELL_INFUSE = 681403,
    SPELL_INFUSE_DAMAGE = 681404,
    // Thirst: the Bloodmage resource, 10 stacks maximum (Spell.dbc StackAmount, and the
    // DisplayMaximum of the {20, 706613} row of AscensionCompatData::ResourceDisplays).
    SPELL_THIRST = 706613,
    SPELL_THIRST_FOR_BLOOD = 570023,
    SPELL_SHADOWS_IN_THE_NIGHT = 704662,
    SPELL_VAMPYR_LORD = 560259,
    SPELL_FESTERING_MAW = 706683,
    SPELL_BITE_WOUND_HEAL = 556234,
    SPELL_THICK_PELT = 704626,
    SPELL_THICK_PELT_GUARD = 556233,
    SPELL_CLOTTING = 704115,
    SPELL_CLOTTING_DAMAGE = 806214,
    SPELL_DARK_ESSENCE = 680732,
    SPELL_DARK_ESSENCE_HEAL = 681036,
    // "Blood Rituals / Target Applied Aura", the mark Dark Liturgy leaves on allies.
    SPELL_BLOOD_RITUALS_MARK = 706623,
    // The second of the two "Cursed Form" markers every Cursed Form ability gates its cast on
    // through CasterAuraSpell; the first is AscensionBloodmage::CursedFormCheck (524861).
    // AscensionBloodmageTalents.cpp owns and documents that pair. Read back from Spell.dbc
    // field 24 on 2026-09-20: Lunge 500126 carries 525031, Rotclaw 804197 and Aortic Assault
    // 806212 carry 524861.
    SPELL_CURSED_FORM_MARKER = 525031
};

// Aortic Assault has seven ranks — 806212, then 807780-807785, which AscensionSpellProgressionData.h
// lists as its rank-ups at levels 24, 32, 40, 48, 56 and 60. Effect 2 of all seven was read and is
// the same SPELL_AURA_PERIODIC_TRIGGER_SPELL of amplitude 3000 on 806214. They are named in the
// companion SQL, not here: the script keys on that effect's shape, not on a list of ids.

constexpr uint32 BloodmageFamily = 26;

// Script-value keys on an accumulator mark. The key space belongs to the aura instance and is
// arbitrary; the payload id carries the running total, and this one the "already paying out"
// flag. Neither can collide: nothing else scripts these two auras.
constexpr uint32 KEY_UNLEASHING = 1;

// Every creature Animated Blood can leave behind: worms, parasites and the rank 3 amalgam.
// Duplicated from AscensionBloodmageTalents.cpp, which owns the list and which this task is not
// allowed to modify; the two copies must stay in step.
constexpr uint32 AnimatedBloodSummons[] = {325301, 335301, 315301};

bool IsAnimatedBlood(uint32 entry)
{
    return std::find(std::begin(AnimatedBloodSummons), std::end(AnimatedBloodSummons), entry) !=
        std::end(AnimatedBloodSummons);
}

// How many accumulator marks are alive in the whole realm. UNITHOOK_ON_DAMAGE runs for every
// damage event of every one of the ~700 units in world, and the Infuse branch cannot filter on
// the attacker's class (a party member of any class, or their minion, feeds the mark). This
// counter turns the common case into one relaxed atomic load. Maps are updated from several
// threads, so a plain int here would be a data race.
std::atomic<uint32> ActiveInfuse{0};
std::atomic<uint32> ActiveAnguish{0};

// Suppresses accumulation while an unleash is resolving. Atherann's Anguish marks a whole pack:
// several marks expire in the same Unit::_UpdateSpells pass, one at a time, so the explosion paid
// out on the first enemy would otherwise be accumulated by the marks still alive on the others
// and paid again. A map is updated by a single thread at a time, and the triggered cast resolves
// inside the same call, so a thread_local depth is the right scope.
thread_local uint32 UnleashDepth = 0;

struct UnleashGuard
{
    UnleashGuard() { ++UnleashDepth; }
    ~UnleashGuard() { --UnleashDepth; }
    UnleashGuard(UnleashGuard const&) = delete;
    UnleashGuard& operator=(UnleashGuard const&) = delete;
};

// fetch_sub on zero wraps to UINT32_MAX and would leave the realm-wide gate permanently open,
// so the decrement never goes below zero.
void ReleaseMark(std::atomic<uint32>& counter)
{
    uint32 seen = counter.load(std::memory_order_relaxed);
    while (seen && !counter.compare_exchange_weak(seen, seen - 1, std::memory_order_relaxed))
        ;
}

bool IsBloodmage(Unit const* unit)
{
    return unit && unit->IsPlayer() && unit->getClass() == CLASS_SON_OF_ARUGAL;
}

// Custom base points travel through native float arithmetic before returning to int32, the same
// clamp AscensionBloodmageSecondary.cpp and AscensionBloodmageVitality.cpp already use.
int32 ClampBasePoints(uint64 amount)
{
    uint32 const maximum = uint32(std::nextafter(float(std::numeric_limits<int32>::max()), 0.0f));
    return int32(std::min<uint64>(amount, maximum));
}

// The shared body of the two accumulators: pay out what the mark stored, once, on natural expiry.
// `percent` is the share the record authors; 100 means "the stored amount", which is what Infuse's
// description says and all its data allows (see the Infuse note in the metadata pass below).
void UnleashMark(AuraScript* script, uint32 payloadId, int32 percent)
{
    Aura* mark = script->GetAura();
    Unit* caster = script->GetCaster();
    Unit* target = script->GetTarget();
    AuraApplication const* application = script->GetTargetApplication();
    // Natural expiry only: a dispel, a death, an early removal or a refresh pays nothing. Same
    // test as Genesis (AscensionRunemasterGenesis.cpp) and Firebrand.
    if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
        return;
    if (!caster || !target || !target->IsAlive() || !caster->IsInWorld() ||
        !caster->IsValidAttackTarget(target))
        return;
    uint64 const stored = mark->GetScriptValue(payloadId);
    if (!stored)
        return;
    // Re-entrancy guard on the aura itself, set and never cleared: this handler only runs on the
    // removal that destroys the aura, and the payload deals damage to this very target while the
    // mark may still sit in the target's applied-aura map.
    if (mark->GetScriptValue(KEY_UNLEASHING))
        return;
    mark->SetScriptValue(KEY_UNLEASHING, 1);
    uint64 const payout = stored * uint64(std::clamp(percent, 0, 100)) / 100;
    if (!payout)
        return;
    UnleashGuard guard;
    caster->CastCustomSpell(payloadId, SPELLVALUE_BASE_POINT0, ClampBasePoints(payout), target,
        TRIGGERED_FULL_MASK);
}

// ---------------------------------------------------------------------------------------------
// Accumulation, and the two flat damage reductions that also need Unit::DealDamage.
//
// UNITHOOK_ON_DAMAGE rather than an AllSpellScript: the tooltips say "the damage you deal" and
// "damage dealt to the target" without qualifying it, and Unit::DealDamage is the single point
// every kind of damage passes through — melee swings, spell hits, periodic ticks — with the
// amount already mitigated. ALLSPELLHOOK_ON_HIT_RESULT would miss auto attacks entirely.
//
// The filter is ordered cheapest first on purpose: two relaxed atomic loads and a pointer test
// before anything touches an aura map.
class bloodmage_damage_events : public UnitScript
{
public:
    bloodmage_damage_events() : UnitScript("bloodmage_damage_events", true, {UNITHOOK_ON_DAMAGE}) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim)
            return;

        // Shadows In The Night (704662): "While above 75% health, your damage taken is reduced by
        // $s2%." Effect 1 authors a permanent SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN of -5 on every
        // school (MiscValue 127), i.e. the reduction applies at ALL times, which is not what the
        // talent promises. The metadata pass below turns that effect into a DUMMY so the core
        // stops applying it, and the condition is enforced here instead. The 5 is read back from
        // the very effect that was disarmed, never hard-coded.
        if (damage && IsBloodmage(victim))
            if (AuraEffect const* shadows = victim->GetAuraEffect(SPELL_SHADOWS_IN_THE_NIGHT, EFFECT_1))
                if (victim->HealthAbovePct(75))
                {
                    int32 const pct = std::clamp(std::abs(shadows->GetAmount()), 0, 100);
                    damage -= CalculatePct(damage, pct);
                }

        if (!damage || !attacker || attacker == victim || UnleashDepth)
            return;

        // Atherann's Anguish: "the damage YOU deal to affected enemies". Caster-scoped, so two
        // Bloodmages plaguing the same pack each feed their own mark and never someone else's.
        if (ActiveAnguish.load(std::memory_order_relaxed) && IsBloodmage(attacker))
            if (Aura* mark = victim->GetAura(SPELL_ANGUISH, attacker->GetGUID()))
                if (!mark->GetScriptValue(KEY_UNLEASHING))
                    mark->SetScriptValue(SPELL_ANGUISH_DAMAGE,
                        mark->GetScriptValue(SPELL_ANGUISH_DAMAGE) + damage);

        if (!ActiveInfuse.load(std::memory_order_relaxed))
            return;
        // Infuse: "damage dealt to the target by you, your party or raid members, and their
        // minions". The attacker can be anyone, so resolve it to the player behind it once and
        // only after the marks have been found.
        auto const bounds = victim->GetAppliedAuras().equal_range(SPELL_INFUSE);
        if (bounds.first == bounds.second)
            return;
        Player* source = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!source)
            return;
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            Aura* mark = itr->second->GetBase();
            if (mark->GetScriptValue(KEY_UNLEASHING))
                continue;
            Unit* caster = mark->GetCaster();
            Player* owner = caster ? caster->ToPlayer() : nullptr;
            if (!owner || (owner != source && !owner->IsInSameRaidWith(source)))
                continue;
            mark->SetScriptValue(SPELL_INFUSE_DAMAGE, mark->GetScriptValue(SPELL_INFUSE_DAMAGE) + damage);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// 680680 Atherann's Anguish. The running total lives on the aura instance, so it dies with the
// mark, cannot leak between two applications and cannot be shared between two enemies of the pack.
class aura_ascension_bloodmage_anguish : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_anguish);
    // One increment per script instance at most, so the realm-wide gate can never drift upwards
    // if the core ever handled the effect twice.
    bool _counted = false;

    bool Validate(SpellInfo const* info) override
    {
        // Keyed on the id: the ids are what this logic depends on, and a SpellFamilyName guard
        // that ever stopped matching would disarm the script without logging a line (P-055).
        return info && info->Id == SPELL_ANGUISH &&
            info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_ANGUISH_DAMAGE &&
            ValidateSpellInfo({SPELL_ANGUISH_DAMAGE});
    }

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
            return;
        _counted = true;
        ActiveAnguish.fetch_add(1, std::memory_order_relaxed);
    }

    void Unleash(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        // Release first and unconditionally: every path out of Unleash must leave the counter
        // balanced, including the early returns for a dispel or a dead target.
        if (_counted)
        {
            _counted = false;
            ReleaseMark(ActiveAnguish);
        }
        // The rate is effect 1's MiscValueB — the 30 the description quotes, and the only 30 the
        // record carries. Accumulating the raw damage and taking the share once loses less than
        // taking 30% of every single hit.
        UnleashMark(this, SPELL_ANGUISH_DAMAGE, GetSpellInfo()->Effects[EFFECT_1].MiscValueB);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_anguish::Apply, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_anguish::Unleash, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 681403 Infuse. Same shape, one difference: the record authors no share at all (effect 1's
// MiscValueB is 0, where Atherann's Anguish carries its 30 and Genesis its 50), and no percentage
// appears in its description. "Unleashes the stored amount" is therefore taken literally: 100%.
class aura_ascension_bloodmage_infuse : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_infuse);
    bool _counted = false;

    bool Validate(SpellInfo const* info) override
    {
        return info && info->Id == SPELL_INFUSE &&
            info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_INFUSE_DAMAGE &&
            ValidateSpellInfo({SPELL_INFUSE_DAMAGE});
    }

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
            return;
        _counted = true;
        ActiveInfuse.fetch_add(1, std::memory_order_relaxed);
    }

    void Unleash(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
        {
            _counted = false;
            ReleaseMark(ActiveInfuse);
        }
        UnleashMark(this, SPELL_INFUSE_DAMAGE, 100);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_infuse::Apply, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_infuse::Unleash, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---------------------------------------------------------------------------------------------
// 570023 Thirst for Blood. "Receive a bonus based on your Thirst level. Between 1-5 stacks of
// Thirst: Spell haste increased by $570024s1%. Between 6-10 stacks of Thirst: The bonus damage
// dealt by spell critical strikes is increased by $570025s1%."
//
// Both halves exist and work on their own: 570024 "Sated" is a SPELL_AURA_HASTE_SPELLS of 10, and
// 570025 "Ravenous" is a SPELL_AURA_MOD_CRIT_DAMAGE_BONUS of 100 on the magic schools (MiscValue
// 126) whose effect 1 is a SPELL_EFFECT_REMOVE_AURA on 570024. Nothing ever granted either of
// them: 570023's own two effects are inert DUMMYs, and neither id appears anywhere in the module
// or in spell_proc / spell_linked_spell / spell_script_names.
//
// The tier is derived from the live Thirst stack, never stored. Both tier spells are read out of
// the talent's own record — effect 0 triggers the high tier (570025), effect 1 the low one
// (570024) — so a DBC import that repoints them is followed and no id is written twice.
void SyncThirstTiers(Player* player, uint32 stacks)
{
    if (!IsBloodmage(player))
        return;
    SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_THIRST_FOR_BLOOD);
    uint32 const high = talent ? talent->Effects[EFFECT_0].TriggerSpell : 0;
    uint32 const low = talent ? talent->Effects[EFFECT_1].TriggerSpell : 0;
    if (!high || !low)
        return;
    // "Between 1-5" and "Between 6-10" are the only boundaries written anywhere; 10 is also
    // Thirst's StackAmount in Spell.dbc and the DisplayMaximum of its ResourceDisplays row. A
    // count above the authored maximum still reads as the high tier.
    bool const eligible = player->IsAlive() && player->IsInWorld() &&
        player->HasAura(SPELL_THIRST_FOR_BLOOD);
    uint32 const grant = !eligible || !stacks ? 0 : stacks <= 5 ? low : high;
    for (uint32 tier : {low, high})
        if (tier != grant)
            player->RemoveAurasDueToSpell(tier, player->GetGUID());
    if (grant && !player->HasAura(grant, player->GetGUID()))
        player->CastSpell(player, grant, true);
}

uint32 ThirstStacks(Player const* player)
{
    Aura const* thirst = player ? player->GetAura(SPELL_THIRST) : nullptr;
    return thirst ? thirst->GetStackAmount() : 0;
}

// Riding on Thirst itself. Thirst already carries `aura_ascension_resource_talent_refresh`, which
// proves the idiom: an AfterEffectApply registered with AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK is
// re-entered on every stack change, because Aura::SetStackAmount calls AuraEffect::ChangeAmount
// with onStackOrReapply = true, which re-handles the effect (SpellAuras.cpp, Aura::SetStackAmount).
class aura_ascension_bloodmage_thirst_tiers : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_thirst_tiers);

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        SyncThirstTiers(GetTarget()->ToPlayer(), GetStackAmount());
    }

    void Remove(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        SyncThirstTiers(GetTarget()->ToPlayer(), 0);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_thirst_tiers::Apply, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_thirst_tiers::Remove, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The talent can be learned, unlearned or lost to a spec change while Thirst is already up, so the
// tier has to be re-derived at both ends. The removal path clears unconditionally rather than
// going through SyncThirstTiers, whose HasAura(talent) test is not reliable while the talent aura
// is itself being unapplied.
class aura_ascension_bloodmage_thirst_for_blood : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_thirst_for_blood);

    void Refresh(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        SyncThirstTiers(player, ThirstStacks(player));
    }

    void Clear(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* player = GetTarget();
        SpellInfo const* talent = GetSpellInfo();
        for (uint8 index : {uint8(EFFECT_0), uint8(EFFECT_1)})
            if (uint32 tier = talent->Effects[index].TriggerSpell)
                player->RemoveAurasDueToSpell(tier, player->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_thirst_for_blood::Refresh,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_thirst_for_blood::Clear,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---------------------------------------------------------------------------------------------
// 560259 Vampyr Lord: "Increases the damage dealt by your Animated Blood by $s1%."
//
// Effect 0 is SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS (190), which this core does implement
// (SpellEffects.cpp maps it to Spell::EffectApplyAreaAura, and UnitAura::FillTargetMap gives it
// its own branch that collects every summon in Unit::m_Controlled). So the +35
// SPELL_AURA_MOD_DAMAGE_PERCENT_DONE on every school (MiscValue 127) is already granted by the
// data — to EVERY summon the Bloodmage owns, not only to Animated Blood. Read in the core, not
// observed in game: the talent is not silent, it is too generous, and the repair is a target
// filter, not a bonus. Adding a second +35% here would double the talent.
//
// Aura::CanBeAppliedOn consults DoCheckAreaTarget for every candidate including the aura's own
// owner, so the owner must be let through or effect 1 would stop applying to the Bloodmage.
class aura_ascension_bloodmage_vampyr_lord : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_vampyr_lord);

    bool CheckAreaTarget(Unit* target)
    {
        // GetAura()->GetOwner() rather than GetUnitOwner(): the latter asserts the aura is a unit
        // aura, and this hook must stay safe whatever the record is later made to create.
        if (!target || target == GetAura()->GetOwner())
            return true;
        Creature* summon = target->ToCreature();
        return summon && IsAnimatedBlood(summon->GetEntry());
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(aura_ascension_bloodmage_vampyr_lord::CheckAreaTarget);
    }
};

// ---------------------------------------------------------------------------------------------
// 706683 Festering Maw: "Healing with Bite Wound now reduces the cooldown of Lunge by 1 sec."
//
// The payload already works: effect 0 is a SPELL_AURA_PROC_TRIGGER_SPELL on 706718, whose single
// effect is SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN (165) with MiscValue 500126 (Lunge) and a base
// value of -1000, and that effect has a real handler (Spell::EffectAscensionModifyCooldown ->
// ModifyAscensionCooldown, SpellEffects.cpp). The proc row added on 2026-09-20 also exists:
//   spell_proc(706683): SpellFamilyName 26, SpellFamilyMask 0, ProcFlags 1024
//                       (PROC_FLAG_DONE_SPELL_NONE_DMG_CLASS_POS), AttributesMask 2.
// With SpellFamilyMask 0 that row matches EVERY positive, dmg-class-none family 26 spell the
// Bloodmage casts, so today any heal at all shortens Lunge. This check narrows it to the one
// spell the description names: 556234 "Bite Wound / Heal", the SPELL_EFFECT_HEAL that 556235
// triggers. It takes nothing away from the existing row, which is left untouched.
class aura_ascension_bloodmage_festering_maw : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_festering_maw);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BITE_WOUND_HEAL});
    }

    bool CheckProc(ProcEventInfo& event)
    {
        SpellInfo const* info = event.GetSpellInfo();
        return info && info->Id == SPELL_BITE_WOUND_HEAL;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_festering_maw::CheckProc);
    }
};

// ---------------------------------------------------------------------------------------------
// 704626 Thick Pelt: "Physical damage taken now reduces the Physical damage taken from the next
// attack within $556233d by ${($556233m1+$AP*0.2)}."
//
// Everything but the attack-power term is already in place and was verified: 704626's effect 0 is
// a SPELL_AURA_PROC_TRIGGER_SPELL on 556233, its spell_proc row exists since 2026-09-20
// (SchoolMask 1, ProcFlags 40 = PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK | PROC_FLAG_TAKEN_SPELL_MELEE_
// DMG_CLASS), and 556233 itself is a one-charge SPELL_AURA_MOD_DAMAGE_TAKEN of -15 on the
// physical school with its own non-zero DBC ProcFlags (0x2a8) so P-045 does not apply to it.
//
// What is missing is the scaling: a flat 15 points is nothing at level 80, and both descriptions
// say the reduction scales with attack power. The two disagree on the coefficient — 556233's own
// description says "$AP*0.25", 704626's says "$AP*0.2" — so the one written on the record whose
// amount this is (556233) is the one applied, and the disagreement is reported rather than
// averaged away. No other source states a coefficient.
class bloodmage_thick_pelt_scaling : public UnitScript
{
public:
    bloodmage_thick_pelt_scaling() : UnitScript("bloodmage_thick_pelt_scaling", true,
        {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE}) { }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        if (!info || info->Id != SPELL_THICK_PELT_GUARD || index != EFFECT_0 || !IsBloodmage(caster) ||
            info->SpellFamilyName != BloodmageFamily ||
            info->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_MOD_DAMAGE_TAKEN ||
            !caster->HasAura(SPELL_THICK_PELT))
            return;
        // The authored amount is negative because it is a reduction, so the attack-power share is
        // subtracted, not added.
        double const amount = double(value) -
            double(caster->GetTotalAttackPowerValue(BASE_ATTACK)) * 0.25;
        if (std::isfinite(amount) && amount <= 0.0 &&
            double(float(amount)) >= double(std::numeric_limits<int32>::min()))
            value = float(amount);
    }
};

// ---------------------------------------------------------------------------------------------
// The load-time metadata pass. Everything here is guarded on the exact shape read in the DBC on
// 2026-09-20, so a later import that authors something else is left alone.
class bloodmage_event_contracts : public GlobalScript
{
public:
    bloodmage_event_contracts() : GlobalScript("bloodmage_event_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != BloodmageFamily)
            return;

        if (info->Id == SPELL_ANGUISH || info->Id == SPELL_INFUSE)
        {
            // THE TRAP OF EFFECT 1, identical in both records and identical to Genesis (500501).
            // Aura 69 is not an unknown Ascension aura: it is the stock SPELL_AURA_SCHOOL_ABSORB
            // (SpellAuraDefines.h:132) with a real implementation in Unit::CalcAbsorbResist. Left
            // as authored, the mark would put a ONE point, all-school (MiscValue 127) shield on
            // the marked enemy, and Unit.cpp removes the whole aura as soon as an absorb amount
            // reaches zero, with AURA_REMOVE_BY_ENEMY_SPELL:
            //     if (absorbAurEff->GetAmount() <= 0)
            //         absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
            // The first point of damage would delete the mark, with a removal mode that is not
            // AURA_REMOVE_BY_EXPIRE: it would never last its 10 s and never pay out. Turning the
            // effect into a DUMMY disarms the shield and keeps its MiscValueB readable — which is
            // where Atherann's Anguish's 30% lives.
            if (info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
                info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
            // The running total is in memory only. A mark restored from `character_aura` after a
            // restart would expire with nothing to pay, so it must not be saved at all.
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }

        if (info->Id == SPELL_ANGUISH_DAMAGE || info->Id == SPELL_INFUSE_DAMAGE)
        {
            // A share of damage that has already been resolved once against this same target: it
            // must not be mitigated, buffed or critted a second time. Exactly the treatment
            // AscensionBloodmageSecondary.cpp already gives Reave's and Vampyr's Kiss's copies.
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            // The stock 3.3.5a coefficient column holds 0.25 for 680681 and 0.15 for 681404; the
            // unleashed amount is the accumulated total and nothing else.
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }

        // 681404 is deliberately NOT retargeted. It is authored as TARGET_UNIT_DEST_AREA_ENEMY
        // with EffectRadiusIndex 13 (field 92, = 10 yards): the unleash splashes around the
        // marked enemy, where Atherann's payload 680681 is single target (implicit target 6).
        // Casting it at the marked unit gives Spell::InitExplicitTargets a destination to use
        // (Spell.cpp: "try to use unit target if provided"), and the 10-yard area resolves from
        // there.

        if (info->Id == SPELL_SHADOWS_IN_THE_NIGHT &&
            info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)
        {
            // Disarm the unconditional reduction; bloodmage_damage_events re-applies it only above
            // 75% health, reading the amount back from this very effect.
            info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
        }

        if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_THIRST_FOR_BLOOD))
            if (info->Id == talent->Effects[EFFECT_0].TriggerSpell ||
                info->Id == talent->Effects[EFFECT_1].TriggerSpell)
            {
                // Sated and Ravenous are re-derived from the live Thirst stack on every stack
                // change; a copy restored from `character_aura` would be a tier with no resource
                // behind it.
                info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
                info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            }
    }
};

// ---------------------------------------------------------------------------------------------
// 704115 Clotting: "At the end of Aortic Assault's duration, all enemies in a $806214a1 yd frontal
// cone suffer ${$806214m1*$<scalingbp>+$AP*0.5} Physical damage. Damage dealt by this effect is
// increased by $s2% against bleeding targets."
//
// Aortic Assault already fires the cone: effect 2 of each of its seven ranks is a
// SPELL_AURA_PERIODIC_TRIGGER_SPELL of amplitude 3000 on 806214, on a 3000 ms aura, so it ticks
// once as the ability ends. 806214 is a real 10-yard TARGET_UNIT_CONE_ENEMY_54 hit
// (EffectRadiusIndex 13) and its base points are already level-normalised by the module's
// $scalingbp table (AscensionScalingBaseData.h carries {806214, 1}).
//
// Two things were missing, and both are the talent itself:
//   - the gate. The description reads "$?s704115[ ... ][]": the cone belongs to Clotting, yet the
//     periodic effect fires for every Bloodmage, talent or not.
//   - "$s2% against bleeding targets", the 25 that effect 1 of 704115 carries.
//
// NOT done, and reported rather than guessed: the "$AP*0.5" of the description. Nothing in the
// data expresses it — 806214's EffectBonusMultiplier (field 229) is 0 and the record has no
// attack-power term — and adding it inside CalcValue would land underneath the $scalingbp
// normaliser that already multiplies this effect's base points, i.e. it would be scaled twice.
class aura_ascension_bloodmage_aortic_assault : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_aortic_assault);

    bool Validate(SpellInfo const* info) override
    {
        return info && info->Effects[EFFECT_2].TriggerSpell == SPELL_CLOTTING_DAMAGE &&
            ValidateSpellInfo({SPELL_CLOTTING_DAMAGE});
    }

    void Clot(AuraEffect const* /*effect*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_CLOTTING))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_bloodmage_aortic_assault::Clot,
            EFFECT_2, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

class spell_ascension_bloodmage_clotting : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_clotting);

    void Bleeding()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const dealt = GetHitDamage();
        if (!caster || !target || dealt <= 0 || !target->HasAuraState(AURA_STATE_BLEEDING))
            return;
        // The 25 is effect 1 of the talent, read back, never written here. Its aura type is 112
        // SPELL_AURA_OVERRIDE_CLASS_SCRIPTS with MiscValue 20007, a class-script id this core does
        // not know, so the effect carries the number and nothing else: it has to be spent by hand.
        AuraEffect const* talent = caster->GetAuraEffect(SPELL_CLOTTING, EFFECT_1);
        if (!talent)
            return;
        int32 const pct = std::clamp(talent->GetAmount(), 0, 1000);
        if (!pct)
            return;
        int64 const bonus = int64(dealt) * pct / 100;
        SetHitDamage(int32(std::min<int64>(int64(dealt) + bonus, std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_bloodmage_clotting::Bleeding);
    }
};

// ---------------------------------------------------------------------------------------------
// 680732 Dark Essence: "Casting Cursed Form abilities or Bloodbolt will now heal allies affected
// by Blood Rituals for ${$681036m1+$681036ppl1+$BH*0.1} every $681036t1 sec for $681036d."
//
// 681036 "Dark Essence / Heal" is complete on its own: SPELL_AURA_PERIODIC_HEAL, 1500 ms
// amplitude, 3000 ms duration, TARGET_SRC_CASTER / TARGET_UNIT_SRC_AREA_ALLY at EffectRadiusIndex
// 23 (= 40 yards), EffectBonusMultiplier 1.0. Nothing ever cast it, and nothing restricted it to
// the marked allies the description names.
//
// "Cursed Form abilities" is not a guess: every one of them gates its own cast on a CasterAuraSpell
// marker, 525031 or 524861, which is the pair AscensionBloodmageTalents.cpp already documents and
// mirrors onto the real form state. Bloodbolt is the empowerment family AscensionPooledVitality.h
// already names. Both are read, not enumerated here.
//
// The tooltip's second line ("Healing from Sanguine Essence increases target's critical strike
// chance by $680693s3% for $680593d") is left alone: 680693 has a single effect, so its "s3" names
// an effect that does not exist, and no other record states that chance.
class bloodmage_dark_essence_casts : public AllSpellScript
{
public:
    bloodmage_dark_essence_casts() : AllSpellScript("bloodmage_dark_essence_casts",
        {ALLSPELLHOOK_ON_CAST}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
    {
        // Ordered cheapest first: this hook runs for every cast in the realm.
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!IsBloodmage(player) || spell->IsTriggered() || !info ||
            info->SpellFamilyName != BloodmageFamily || !player->HasAura(SPELL_DARK_ESSENCE))
            return;
        bool const cursedForm = info->CasterAuraSpell == SPELL_CURSED_FORM_MARKER ||
            info->CasterAuraSpell == AscensionBloodmage::CursedFormCheck;
        if (!cursedForm &&
            AscensionBloodmage::GetEmpowerment(info->Id) != AscensionBloodmage::Bloodbolt)
            return;
        player->CastSpell(player, SPELL_DARK_ESSENCE_HEAL, true);
    }
};

class spell_ascension_bloodmage_dark_essence : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_dark_essence);

    void Select(std::list<WorldObject*>& targets)
    {
        // "allies affected by Blood Rituals", and no one else. Whose mark it is, is not stated, so
        // the caster of 706623 is not checked.
        targets.remove_if([](WorldObject* object)
        {
            Unit* unit = object ? object->ToUnit() : nullptr;
            return !unit || !unit->HasAura(SPELL_BLOOD_RITUALS_MARK);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(
            spell_ascension_bloodmage_dark_essence::Select, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
    }
};

}

void AddSC_AscensionBloodmageEvents()
{
    new bloodmage_damage_events();
    new bloodmage_thick_pelt_scaling();
    new bloodmage_event_contracts();
    RegisterSpellScript(aura_ascension_bloodmage_anguish);
    RegisterSpellScript(aura_ascension_bloodmage_infuse);
    RegisterSpellScript(aura_ascension_bloodmage_thirst_tiers);
    RegisterSpellScript(aura_ascension_bloodmage_thirst_for_blood);
    RegisterSpellScript(aura_ascension_bloodmage_vampyr_lord);
    RegisterSpellScript(aura_ascension_bloodmage_festering_maw);
    RegisterSpellScript(aura_ascension_bloodmage_aortic_assault);
    RegisterSpellScript(spell_ascension_bloodmage_clotting);
    new bloodmage_dark_essence_casts();
    RegisterSpellScript(spell_ascension_bloodmage_dark_essence);
}
