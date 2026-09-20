/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>

namespace
{
enum RangerEventSpells : uint32
{
    // Talent auras the player carries.
    SPELL_WINGMAN = 705098,
    SPELL_PHOENIX_PLUMES = 705074,
    SPELL_SWIFTSHOT = 705028,
    // Companion counters. Measured, not assumed: before the SQL half of this fix
    // NOTHING cast either record. No field of any of the 209510 Spell.dbc records
    // holds 680278 or 681394, no acore_world row held them, and no other source file
    // names them. The SQL beside this one hands 680278 to creature 50393 through
    // creature_template_addon, so each live War Falcon carries it and pushes its two
    // area-aura effects onto its master (UnitAura::FillTargetMap, SpellAuras.cpp:2875
    // -2881; radius index 12 = 100 yards). Two falcons make two separate applications
    // on the master: Aura::CanStackWith takes the "different caster" path and returns
    // true on effect 1 being SPELL_AURA_PERIODIC_ENERGIZE with no area implicit target,
    // and no spell_group row constrains these ids. Unit::GetAuraCount (Unit.cpp:6279)
    // adds Aura::GetStackAmount() per application; it does NOT read the DBC StackAmount
    // field (which is 0 here). Aura::m_stackAmount is initialised to 1 (SpellAuras.cpp:
    // 358) and nothing stacks these auras, so the "else" branch runs and the count is
    // one per application.
    // Effect 2 is NOT a per-companion figure parked there for convenience. It is
    // SPELL_AURA_ADD_FLAT_MODIFIER (107) with EffectMiscValue 23 = SPELLMOD_EFFECT3
    // (SpellDefines.h:99) and EffectSpellClassMask (0x00080000, 0, 0), which matches
    // 705098's SpellFamilyFlags exactly: it is the DBC's own -2-per-companion wiring
    // onto Wingman's effect index 2. It stays on the pet, because only area-aura
    // effects travel to the owner, and it is inert there since only Players carry
    // spell mods. The script below therefore computes the amount instead of the DBC,
    // and does not double it. Should a later fix ever put the counter on the player,
    // SpellEffectInfo::CalcValue would apply that spellmod first (AuraEffect::
    // CalculateAmount, SpellAuraEffects.cpp:506, before the script hook at :628) and
    // the plain assignment in CalcAmount would silently hide it.
    // Its -2 is what both tooltips quote: 705098 says "$680278s3%", and 680278 reads
    // "You have a War Falcon active! Generating 1 Focus per second.$?s705098[ Reduces
    // damage taken by ${$w3}%.][]" -- that Focus line is effect 1, value 1 per second.
    SPELL_WAR_FALCON_COUNT = 680278,
    // Dead branch today, and deliberately kept. The only Ranger SPELL_EFFECT_SUMMON
    // aimed at a "Dragonhawk" is 573058 "Dragonhawk Tamer", EffectMiscValue 52393, and
    // creature_template has no 52393 (50264 and 50393 both exist, both named "War
    // Falcon"). No creature can carry 681394, so this counter is always absent. The
    // call below costs one failed lookup and becomes right the day the creature exists.
    SPELL_DRAGONHAWK_COUNT = 681394,
    // Resource and payloads.
    SPELL_ADVANTAGE = 804329,
    SPELL_PHOENIX_PLUMES_PROC = 520558,
    SPELL_PRECISION_SHOT_WEAKNESS = 800578
};

// 804329 carries StackAmount 5 in the client DBC, and the Phoenix Plumes text says
// "used with 5 stacks of Advantage". AscensionClassMechanics.cpp's Stonemason branch
// tests the same full pile at the same moment of the cast.
constexpr uint8 RANGER_ADVANTAGE_FULL_STACK = 5;

// Each counter is read with its own effect 2 rather than one shared figure: both hold
// -2 today, but nothing forces the two records to keep agreeing.
int64 RangerCompanionShare(Unit const* master, uint32 counterId)
{
    SpellInfo const* counter = sSpellMgr->GetSpellInfo(counterId);
    return counter ? int64(counter->Effects[EFFECT_2].CalcValue()) * int64(master->GetAuraCount(counterId)) : 0;
}

// Negative, because SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN is read as a multiplier
// (100 + amount) / 100 by Unit::SpellDamageBonusTaken and Unit::MeleeDamageBonusTaken.
int32 RangerWingmanAmount(Unit const* master)
{
    int64 amount = RangerCompanionShare(master, SPELL_WAR_FALCON_COUNT) +
        RangerCompanionShare(master, SPELL_DRAGONHAWK_COUNT);
    // A multiplier below zero would turn damage into healing. Nothing else is capped,
    // and that hole is stated here rather than passed over: with N companions the
    // talent gives -2N%, so 50 birds reach -100%, i.e. full immunity -- Unit.cpp:9351
    // (spells) and Unit.cpp:10856 (melee) both fold this aura in as (100 + amount)/100.
    // 806341 "Falcon Dive" already summons TWO birds at once, for 12 s (DurationIndex
    // 29 = 12000 ms). No ceiling is invented, because nothing states one: the only two
    // candidates in the DBC are 705098's unimplemented effects 0 and 1, both DUMMY,
    // worth 20 and 5 (BasePoints 19 and 4, DieSides 1). 5 x 2 = 10 is not 20, so
    // nothing ties them together as "5 companions max" / "-20% max". Left for
    // arbitration; see the SQL beside this file.
    return int32(std::clamp<int64>(amount, -100, 100));
}

void RefreshRangerWingman(Unit* unit)
{
    Player* master = unit ? unit->ToPlayer() : nullptr;
    if (!master || master->getClass() != CLASS_RANGER)
        return;
    if (AuraEffect* wingman = master->GetAuraEffect(SPELL_WINGMAN, EFFECT_2))
        wingman->RecalculateAmount();
}

// 705098 "Wingman": "Reduces all damage taken by $680278s3% for each War Falcon or
// Dragonhawk you have active." Its effect 2 is the empty container the client reads:
// SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN, school mask 127, amount 0 in the DBC.
class aura_ascension_ranger_wingman : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_wingman);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_WAR_FALCON_COUNT, SPELL_DRAGONHAWK_COUNT});
    }

    void CalcAmount(AuraEffect const*, int32& amount, bool& canBeRecalculated)
    {
        // Kept recalculable: every companion that appears or leaves calls
        // RecalculateAmount below, which refuses to run once this flag is cleared.
        canBeRecalculated = true;
        Unit* master = GetUnitOwner();
        amount = master && master->IsPlayer() && master->getClass() == CLASS_RANGER
            ? RangerWingmanAmount(master) : 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_ranger_wingman::CalcAmount,
            EFFECT_2, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    }
};

// Attached to 680278 and 681394. Effect 0 of both is an area aura on the owner, so
// these handlers run on the master's application, never on the companion's own
// (the companion only carries effect 2, which is not an area aura effect).
// Unit::_UnapplyAura erases the application from m_appliedAuras before unapplying its
// effects, so GetAuraCount already excludes the departing companion at removal time.
// Only the 680278 line can fire today: nothing can carry 681394, see its enum note.
class aura_ascension_ranger_companion_count : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_companion_count);

    void Apply(AuraEffect const*, AuraEffectHandleModes) { RefreshRangerWingman(GetTarget()); }
    void Remove(AuraEffect const*, AuraEffectHandleModes) { RefreshRangerWingman(GetTarget()); }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_companion_count::Apply,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_companion_count::Remove,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Copy of DidRangerAdvantageConsumerSucceed, AscensionClassMechanics.cpp:899. The copy
// is forced, not an oversight: that definition sits inside the anonymous namespace
// opened at AscensionClassMechanics.cpp:64 and closed at :972, so it has internal
// linkage and no declaration, in a header or here, can reach it from this file. Moving
// it out of that namespace and into a header would remove the duplicate; until then the
// two bodies have to be kept in step by hand.
// The rule: the pile is only treated as spent when at least one external target was
// reached, or when the cast had no external target at all. Delayed missiles already hold
// their launch-time miss condition, and SelectSpellTargets has filled the list long
// before AfterCast.
bool RangerConsumerSucceeded(Spell* spell, Player const* master)
{
    bool hasExternalTarget = false;
    for (TargetInfo const& target : *spell->GetUniqueTargetInfo())
    {
        if (target.targetGUID == master->GetGUID())
            continue;
        hasExternalTarget = true;
        if (target.missCondition == SPELL_MISS_NONE)
            return true;
    }
    return !hasExternalTarget;
}

// 705074 "Phoenix Plumes": "Your Skullpiercers and Woodland Arrows used with 5 stacks of
// Advantage now restore $520784s3% Focus and summon a War Falcon for $520558d."
// 520558 carries both halves: SPELL_EFFECT_SUMMON of creature 50393 (War Falcon) for its
// own 6 second duration, and SPELL_EFFECT_ENERGIZE_PCT of 3 Focus percent, the same
// figure 520784 effect 2 holds. Placement, so it is not rediscovered: effect 0 of 520558
// has EffectImplicitTargetA 65 = TARGET_DEST_TARGET_BACK (SharedDefines.h:1562), and the
// cast below passes the master as its own target, so the falcon lands behind the player.
// 520588, the summon Falconstrike uses, holds the same creature and SummonProperties 61
// but EffectImplicitTargetA 53 = TARGET_DEST_TARGET_ENEMY (SharedDefines.h:1550): the two
// summons are not interchangeable in where they put the bird. Left as the DBC asks. Bound to the two ability chains, so the hook costs
// nothing to the other classes. AfterCast runs at Spell.cpp:4102, before the Advantage
// decrement at Spell.cpp:4129, so the pile is still whole here. All ten Skullpiercer and
// all seven Woodland Arrow records carry CasterAuraSpell 804329, so this fires on the
// very cast that spends the pile, and the pile is empty for the next one.
class spell_ascension_ranger_phoenix_plumes : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_phoenix_plumes);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_PHOENIX_PLUMES_PROC, SPELL_ADVANTAGE});
    }

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->getClass() == CLASS_RANGER;
    }

    void Summon()
    {
        Player* master = GetCaster()->ToPlayer();
        Spell* spell = GetSpell();
        if (!spell || spell->IsTriggered() || !master->IsAlive() || !master->IsInWorld() ||
            !master->HasAura(SPELL_PHOENIX_PLUMES))
            return;
        // ">=" and not "==" as the Stonemason branch writes it: SpellInfo::CalcMaxAuraStacks
        // is modifiable, and a pile capped above five would otherwise disarm the talent
        // in silence. Five is the DBC StackAmount of 804329 and the figure the text names.
        Aura const* advantage = master->GetAura(SPELL_ADVANTAGE);
        if (!advantage || advantage->GetStackAmount() < RANGER_ADVANTAGE_FULL_STACK)
            return;
        if (!RangerConsumerSucceeded(spell, master))
            return;
        master->CastSpell(master, SPELL_PHOENIX_PLUMES_PROC, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_ranger_phoenix_plumes::Summon);
    }
};

// 705028 "Swiftshot": "Damage dealt by Precision Shot now increases enemy Physical damage
// taken by $800578s1% for $800578d." 800578 is a plain SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN
// of 4, misc value 1 (physical), 12 seconds. Bound to the Precision Shot chain only.
class spell_ascension_ranger_swiftshot : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_swiftshot);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_PRECISION_SHOT_WEAKNESS}); }
    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->getClass() == CLASS_RANGER;
    }

    void Weaken()
    {
        Player* master = GetCaster()->ToPlayer();
        Unit* target = GetHitUnit();
        // GetHitDamage is the amount rolled before absorption; a fully absorbed shot
        // still dealt damage, a fully resisted or immune one did not.
        if (!target || target == master || !target->IsAlive() || master->IsFriendlyTo(target) ||
            GetHitDamage() <= 0 || !master->IsAlive() || !master->IsInWorld() ||
            !master->HasAura(SPELL_SWIFTSHOT))
            return;
        master->CastSpell(target, SPELL_PRECISION_SHOT_WEAKNESS, true);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_ranger_swiftshot::Weaken);
    }
};
}

void AddSC_AscensionRangerEvents()
{
    RegisterSpellScript(aura_ascension_ranger_wingman);
    RegisterSpellScript(aura_ascension_ranger_companion_count);
    RegisterSpellScript(spell_ascension_ranger_phoenix_plumes);
    RegisterSpellScript(spell_ascension_ranger_swiftshot);
}
