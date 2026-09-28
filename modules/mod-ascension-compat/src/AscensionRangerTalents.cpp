/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionRangerTalents.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <limits>

namespace
{
enum RangerTalentSpells : uint32
{
    SPELL_LIGHT_ARROWS = 681292,
    SPELL_KNOCKOUT_INCAPACITATE = 706762,
    SPELL_STONEMASONS_SECRET = 524654,
    SPELL_DIRTY_BLADES = 680276,
    SPELL_ADVANTAGE = 804329,
    SPELL_EXTEND_DIRTY_BLADES = 524653,
    SPELL_SNATCH = 803115,
    SPELL_SNATCH_DISARM = 803123,
    // Nettoyage du 2026-09-28.
    SPELL_DIRTY_BLADES_LEECH = 520571,
    SPELL_PILFERING = 705087,
    SPELL_DIRTY_BLADES_HEAL = 520880,
    SPELL_NEUROTOXIN_SILENCE = 500616,
    SPELL_SERRATIONS_1 = 705050,
    SPELL_SERRATIONS_2 = 707850,
    SPELL_SKIRMISHERS_QUIVER = 801069,
    SPELL_SHARPSHOOTER = 582855,
    SPELL_WILD_MAN = 560344,
    SPELL_BACKSTEP = 800685,
    SPELL_BACKSTEP_BONUS = 800684,
    SPELL_WOODLAND_STALKER = 705034,
    SPELL_RAPID_STRIKE = 560342,
    SPELL_PIERCED_BLEED = 782754,
    SPELL_INCENDIARY_SHOT = 524870,
    SPELL_INCENDIARY_SHOT_HELPER = 524868,
    SPELL_INCENDIARY_EXPLOSION = 570182,
    SPELL_ELUDE = 801345,
    SPELL_WOODLAND_ADEPT = 570015,
    SPELL_SLY = 560339,
    SPELL_COMMAND_AURA_DAMAGE = 537249,
    SPELL_SERRATED_SHOT = 500073,
    // Nettoyage du 2026-09-28, second passage.
    SPELL_CORROSIVE_SHOT = 572372,
    SPELL_CORROSIVE_SHOT_DAMAGE = 572419,
    SPELL_DAZED = 1604,
    SPELL_BARBED_QUILLS = 800077,
    SPELL_BARBED_QUILLS_BLEED = 801472,
    SPELL_BARBED_QUILLS_SPREAD = 560965,
    SPELL_BARBED_QUILLS_EXTEND = 561161,
    SPELL_BARBED_QUILLS_EXTEND_2 = 561168,
    SPELL_ELUDE_TALENTS = 524969,
    SPELL_BOUNTY = 560722,
    // Nettoyage du 2026-09-28, troisième passage.
    SPELL_ONSLAUGHT = 801951,
    SPELL_SKIRMISH = 802039,
    SPELL_SKIRMISH_BRUTAL_SHOT = 803335,
    SPELL_WOODSMANS_ADAPTATION_1 = 800266
};

bool IsRanger(Unit const* unit)
{
    return unit && unit->IsPlayer() && unit->getClass() == CLASS_RANGER;
}

int32 ClampToInt32(uint64 value)
{
    return int32(std::min<uint64>(value, uint64(std::numeric_limits<int32>::max())));
}

// Common filter of the owner's own damaging hits on a hostile unit.
bool IsOwnHostileHit(Unit* owner, ProcEventInfo& event)
{
    Unit* victim = event.GetActionTarget();
    DamageInfo const* damage = event.GetDamageInfo();
    return owner && owner->IsAlive() && event.GetActor() == owner && victim && victim != owner &&
        !owner->IsFriendlyTo(victim) && damage && damage->GetDamage() > 0;
}

bool MainHandIs(Unit* unit, uint32 subClass)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    Item* weapon = player ? player->GetWeaponForAttack(BASE_ATTACK, true) : nullptr;
    return weapon && weapon->GetTemplate()->SubClass == subClass;
}

class spell_ascension_ranger_light_arrows : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_light_arrows);
    int32 _bonus = 0;

    bool Load() override { return GetCaster()->IsPlayer() && GetCaster()->getClass() == CLASS_RANGER; }

    void Snapshot()
    {
        Unit* target = GetExplTargetUnit();
        if (target && GetCaster()->GetExactDist(target) >= 40.0f)
            if (AuraEffect const* talent = GetCaster()->GetAuraEffect(SPELL_LIGHT_ARROWS, EFFECT_0))
                _bonus = std::max(0, talent->GetAmount());
    }

    void Damage()
    {
        // All native base/AP terms have been calculated; retain subsequent crit,
        // armor, absorption and resistance. Distance was captured at cast launch.
        if (_bonus && GetHitDamage() > 0)
            SetHitDamage(int32(std::min<int64>(int64(GetHitDamage()) * (int64(100) + _bonus) / 100,
                std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_ranger_light_arrows::Snapshot);
        OnHit += SpellHitFn(spell_ascension_ranger_light_arrows::Damage);
    }
};

class spell_ascension_ranger_knockout : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_knockout);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_KNOCKOUT_INCAPACITATE}); }
    bool Load() override { return GetCaster()->IsPlayer() && GetCaster()->getClass() == CLASS_RANGER; }

    void Incapacitate(SpellEffIndex)
    {
        if (Unit* target = GetHitUnit())
            GetCaster()->CastSpell(target, SPELL_KNOCKOUT_INCAPACITATE, true);
    }

    void Register() override
    {
        // Effect 0 has already made the victim execute the native bleed/poison cleanser.
        OnEffectHitTarget += SpellEffectFn(spell_ascension_ranger_knockout::Incapacitate, EFFECT_1, SPELL_EFFECT_DUMMY);
    }
};

// 680276 Dirty Blades: "your melee auto attacks deal an additional $s1% of the damage dealt as
// Nature damage and reduce healing received by $520571s2%". Effect 0 is the private aura 354
// (no core handler) toward 520571. Its amount already carries Pilfering's +10 (705087 e0,
// SPELLMOD_EFFECT1 on 680276). spell_proc 680276: PROC_FLAG_DONE_MELEE_AUTO_ATTACK.
class aura_ascension_ranger_dirty_blades : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_dirty_blades);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_DIRTY_BLADES_LEECH}); }

    bool CheckProc(ProcEventInfo& event)
    {
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) && event.GetActionTarget()->IsAlive();
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 1000)) / 100;
        if (amount)
            GetTarget()->CastCustomSpell(SPELL_DIRTY_BLADES_LEECH, SPELLVALUE_BASE_POINT0, ClampToInt32(amount),
                event.GetActionTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_dirty_blades::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ranger_dirty_blades::Proc, EFFECT_0, AuraType(354));
    }
};

// 705087 Pilfering e1 (aura 354 toward 520880): "causes its damage dealt to heal you equal to
// $s2% of the value". Read after the hit, so the heal follows the Nature damage really dealt
// (m_damage holds the post-absorb figure at AfterHit, Spell.cpp:2934).
class spell_ascension_ranger_dirty_blades_leech : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_dirty_blades_leech);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_PILFERING, SPELL_DIRTY_BLADES_HEAL}); }
    bool Load() override { return IsRanger(GetCaster()); }

    void Heal()
    {
        Unit* caster = GetCaster();
        int32 dealt = GetHitDamage();
        AuraEffect const* pilfering = caster->GetAuraEffect(SPELL_PILFERING, EFFECT_1);
        if (dealt <= 0 || !pilfering || !caster->IsAlive())
            return;
        uint64 heal = uint64(dealt) * uint32(std::clamp(pilfering->GetAmount(), 0, 100)) / 100;
        if (heal)
            caster->CastCustomSpell(SPELL_DIRTY_BLADES_HEAL, SPELLVALUE_BASE_POINT0, ClampToInt32(heal), caster, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ascension_ranger_dirty_blades_leech::Heal);
    }
};

// 707744 Highwayman: "Direct critical strikes while behind your target". The crit and the
// direct hit are spell_proc data; only the position has no column.
class aura_ascension_ranger_highwayman : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_highwayman);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        return IsRanger(owner) && IsOwnHostileHit(owner, event) &&
            event.GetDamageInfo()->GetDamageType() != DOT && event.GetActionTarget()->isInBack(owner);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_highwayman::CheckProc);
    }
};

// 807023 Tip of the Spear: "While you have a polearm equipped". The three effects are kept:
// one roll fires all three 807024, hence "3 additional times".
class aura_ascension_ranger_tip_of_the_spear : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_tip_of_the_spear);

    bool CheckProc(ProcEventInfo& event)
    {
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) &&
            MainHandIs(GetTarget(), ITEM_SUBCLASS_WEAPON_POLEARM);
    }

    // Each effect used to cast 807024, whose trigger is hard-coded to Wild Strike rank 1: the
    // extra strikes repeat the rank that procced instead. Passing the aura effect marks them as
    // triggered by 807023, so they cannot proc it again (SpellAuras.cpp:2159).
    void Strike(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        SpellInfo const* strike = event.GetSpellInfo();
        Unit* target = event.GetActionTarget();
        if (strike && target && target->IsAlive())
            GetTarget()->CastSpell(target, strike->Id, true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_tip_of_the_spear::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ranger_tip_of_the_spear::Strike,
            EFFECT_ALL, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 806973 Marked for Death: "While you have daggers equipped, direct critical strikes on enemies
// below 35% health". The crit is spell_proc data (HitMask 2); 35 is the tooltip's figure, no
// field carries it.
class aura_ascension_ranger_marked_for_death : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_marked_for_death);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        return IsRanger(owner) && IsOwnHostileHit(owner, event) &&
            event.GetDamageInfo()->GetDamageType() != DOT && MainHandIs(owner, ITEM_SUBCLASS_WEAPON_DAGGER) &&
            event.GetActionTarget()->HealthBelowPct(35);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_marked_for_death::CheckProc);
    }
};

// 560341 Rapid Strikes: "auto attacks now have a $h% chance to strike an additional time for
// $s1% of the damage dealt". Effect 0 is aura 354 toward 560342; the chance and the one-second
// cooldown are spell_proc data. 560342 carries SPELL_ATTR3_IGNORE_CASTER_MODIFIERS.
class aura_ascension_ranger_rapid_strikes : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_rapid_strikes);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_RAPID_STRIKE}); }

    bool CheckProc(ProcEventInfo& event)
    {
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) && event.GetActionTarget()->IsAlive();
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        if (amount)
            GetTarget()->CastCustomSpell(SPELL_RAPID_STRIKE, SPELLVALUE_BASE_POINT0, ClampToInt32(amount),
                event.GetActionTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_rapid_strikes::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ranger_rapid_strikes::Proc, EFFECT_0, AuraType(354));
    }
};

// 705033 Pierced: critical Skullpiercer / Precision Shot make the enemy bleed (782754). Effect 0
// is aura 354 worth 15, the only figure the DBC carries: the whole 15% is spread over the ticks
// of 782754 (IGNORE_CASTER_MODIFIERS). The tooltip's extra "35% over $d" has no field.
class aura_ascension_ranger_pierced : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_pierced);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_PIERCED_BLEED}); }

    bool CheckProc(ProcEventInfo& event)
    {
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) && event.GetActionTarget()->IsAlive();
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        SpellInfo const* bleed = sSpellMgr->GetSpellInfo(SPELL_PIERCED_BLEED);
        int32 amplitude = bleed->Effects[EFFECT_0].Amplitude;
        int32 duration = bleed->GetMaxDuration();
        if (amplitude <= 0 || duration < amplitude)
            return;
        uint64 total = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        uint64 perTick = total / uint64(duration / amplitude);
        if (perTick)
            GetTarget()->CastCustomSpell(SPELL_PIERCED_BLEED, SPELLVALUE_BASE_POINT0, ClampToInt32(perTick),
                event.GetActionTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_pierced::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ranger_pierced::Proc, EFFECT_0, AuraType(354));
    }
};

// 524869 Incendiary Arrows: "your next 3 direct damage Advantage spenders deal $524869s1% of their
// damage to nearby enemies". Effect 0 is aura 354 toward 570182; the record has no charges, so the
// three spenders are counted here, once per Spell (an area spender raises one event per target,
// and every target of a counted spender explodes). 3 is the tooltip's figure.
class aura_ascension_ranger_incendiary_arrows : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_incendiary_arrows);

    static constexpr uint8 Spenders = 3;
    Spell const* _last = nullptr;
    uint8 _counted = 0;

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_INCENDIARY_EXPLOSION}); }

    void Reset(AuraEffect const*, AuraEffectHandleModes)
    {
        _last = nullptr;
        _counted = 0;
    }

    bool CheckProc(ProcEventInfo& event)
    {
        SpellInfo const* spell = event.GetSpellInfo();
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) && spell && event.GetProcSpell() &&
            spell->SpellFamilyName == 27 && spell->CasterAuraSpell == SPELL_ADVANTAGE &&
            spell->Id != SPELL_INCENDIARY_SHOT && spell->Id != SPELL_INCENDIARY_SHOT_HELPER &&
            event.GetDamageInfo()->GetDamageType() != DOT;
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        if (event.GetProcSpell() != _last)
        {
            if (_counted >= Spenders)
                return;
            _last = event.GetProcSpell();
            ++_counted;
            // Expires on the next aura update, after the other targets of this same spender.
            if (_counted >= Spenders)
                GetAura()->SetDuration(1);
        }
        Unit* victim = event.GetActionTarget();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        SpellInfo const* explosion = sSpellMgr->GetSpellInfo(SPELL_INCENDIARY_EXPLOSION);
        if (!amount || !explosion)
            return;
        // TARGET_UNIT_SRC_AREA_ENEMY: centred on the struck enemy, not on the Ranger.
        SpellCastTargets targets;
        targets.SetUnitTarget(victim);
        targets.SetSrc(*victim);
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, ClampToInt32(amount));
        GetTarget()->CastSpell(targets, explosion, &values, TRIGGERED_FULL_MASK, nullptr, effect);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_incendiary_arrows::Reset,
            EFFECT_0, AuraType(354), AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_incendiary_arrows::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ranger_incendiary_arrows::Proc, EFFECT_0, AuraType(354));
    }
};

// 560339 Sly: "Your Woodland Adept and Elude now trigger a $s1% reduced cooldown for each stack of
// Advantage active when cast". Its SPELLMOD_EFFECT2 targets the hidden 802612, which nothing casts
// (and whose effect 0 would also feed Skullpiercer): the reduction is applied here instead.
// Woodland Adept starts its cooldown at cast. Elude carries SPELL_ATTR0_COOLDOWN_ON_EVENT: its
// cooldown starts when the aura leaves (SpellAuras.cpp:484-488, before the effect removal hooks),
// so the pile is kept on the aura at cast and read at removal. Triggered Eludes (Woodland Adept
// effect 2) take no reduction.
constexpr uint32 SlySnapshotKey = SPELL_SLY;

void ApplySly(Player* player, uint32 spellId, uint32 stacks)
{
    AuraEffect const* sly = player->GetAuraEffect(SPELL_SLY, EFFECT_0);
    if (!sly || !stacks)
        return;
    uint32 pct = std::min<uint32>(100, stacks * uint32(std::abs(sly->GetAmount())));
    uint32 delay = player->GetSpellCooldownDelay(spellId);
    if (int32 cut = int32(uint64(delay) * pct / 100))
        player->ModifySpellCooldown(spellId, -cut);
}

class spell_ascension_ranger_sly : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_sly);

    bool Load() override { return IsRanger(GetCaster()); }

    void Cast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (GetSpell()->IsTriggered() || !player->HasAura(SPELL_SLY))
            return;
        Aura const* advantage = player->GetAura(SPELL_ADVANTAGE, player->GetGUID());
        uint32 stacks = advantage ? advantage->GetStackAmount() : 0;
        if (GetSpellInfo()->Id == SPELL_WOODLAND_ADEPT)
            ApplySly(player, SPELL_WOODLAND_ADEPT, stacks);
        else if (Aura* elude = player->GetAura(SPELL_ELUDE, player->GetGUID()))
            elude->SetScriptValue(SlySnapshotKey, stacks);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_ranger_sly::Cast);
    }
};

class aura_ascension_ranger_sly : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_sly);

    void Removed(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = GetTarget()->ToPlayer();
        if (IsRanger(player) && GetCaster() == player)
            ApplySly(player, SPELL_ELUDE, uint32(GetAura()->GetScriptValue(SlySnapshotKey)));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_sly::Removed,
            EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// 800685 Backstep: the bonus text is shown only "$?s705034", so only that talent's holders build
// the stacks of 800684. The leap (effect 1, periodic trigger every 100 ms) stops feeding once a
// shot has consumed the bonus, and landing removes what is left.
constexpr uint32 BackstepSpentKey = SPELL_BACKSTEP_BONUS;

class aura_ascension_ranger_backstep : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_backstep);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_BACKSTEP_BONUS}); }

    void Tick(AuraEffect const*)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!IsRanger(player) || !player->HasSpell(SPELL_WOODLAND_STALKER) || GetAura()->GetScriptValue(BackstepSpentKey))
            PreventDefaultAction();
    }

    void Land(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_BACKSTEP_BONUS, GetTarget()->GetGUID());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_ranger_backstep::Tick,
            EFFECT_1, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_backstep::Land,
            EFFECT_1, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

// 800684 is consumed by spell_proc (REQ_SPELLMOD, one charge); this marks the leap as spent.
class aura_ascension_ranger_backstep_bonus : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_backstep_bonus);

    void Spent(ProcEventInfo&)
    {
        if (Aura* leap = GetTarget()->GetAura(SPELL_BACKSTEP, GetTarget()->GetGUID()))
            leap->SetScriptValue(BackstepSpentKey, 1);
    }

    void Register() override
    {
        AfterProc += AuraProcFn(aura_ascension_ranger_backstep_bonus::Spent);
    }
};

// 524600 Command Aura puts 537248 on allies: "allied players deal ${$RAP*0.35} additional damage
// as Stormstrike Damage when they deal direct damage". Effect 0 is PROC_TRIGGER_SPELL_WITH_VALUE
// toward 537249, whose amount is the aura's (SpellAuraEffects.cpp:7108). The Ranger's ranged
// attack power is snapshot here; 0.35 is the tooltip's coefficient, the only figure restated.
// Same pattern as aura_ascension_stormbringer_surge_of_might.
class aura_ascension_ranger_command_aura : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_command_aura);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_COMMAND_AURA_DAMAGE}); }

    void CalcAmount(AuraEffect const*, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        amount = IsRanger(caster) ? std::max(0, int32(caster->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.35f)) : 0;
    }

    bool CheckProc(ProcEventInfo& event)
    {
        AuraEffect const* effect = GetEffect(EFFECT_0);
        return effect && effect->GetAmount() > 0 && IsOwnHostileHit(GetTarget(), event) &&
            event.GetDamageInfo()->GetDamageType() != DOT;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_ranger_command_aura::CalcAmount,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE);
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_command_aura::CheckProc);
    }
};

// 561109 A Quick Demise: effect 177 of the core looks up the exact id 500073 on the target, from
// any caster (SpellEffects.cpp:464-482). Serrated Shot has seven ranks chained in spell_ranks, so
// the extension follows the chain and only this Ranger's bleed. The core effect is left alone.
class spell_ascension_ranger_quick_demise : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_quick_demise);

    bool Load() override { return IsRanger(GetCaster()); }

    void Extend(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        Unit* target = GetHitUnit();
        if (!target)
            return;
        if (Aura* serrated = target->GetAuraOfRankedSpell(SPELL_SERRATED_SHOT, GetCaster()->GetGUID()))
        {
            int32 duration = serrated->GetDuration() + GetEffectValue();
            if (duration > 0)
                serrated->SetDuration(duration);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_ranger_quick_demise::Extend,
            EFFECT_0, SPELL_EFFECT_ASCENSION_MODIFY_AURA_DURATION);
    }
};

// 572372 Corrosive Shot: "dealing $572419s1% Ranged Weapon damage as Nature damage, doubled
// against Poisoned targets". The doubling is effect 0, an OVERRIDE_CLASS_SCRIPTS 20007 no code reads
// outside aura 303 (and its spell_proc row keeps it out of the proc). Applied on the weapon damage
// already computed, before crit, armour and resistance; the factor is the talent's own amount.
class spell_ascension_ranger_corrosive_shot : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_corrosive_shot);

    bool Load() override { return IsRanger(GetCaster()); }

    void Damage()
    {
        Unit* target = GetHitUnit();
        AuraEffect const* talent = GetCaster()->GetAuraEffect(SPELL_CORROSIVE_SHOT, EFFECT_0);
        if (!target || !talent || GetHitDamage() <= 0 || !target->HasAuraState(AURA_STATE_ASCENSION_POISONED))
            return;
        int64 pct = std::clamp(talent->GetAmount(), 0, 1000);
        SetHitDamage(int32(std::min<int64>(int64(GetHitDamage()) * (100 + pct) / 100,
            std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_ranger_corrosive_shot::Damage);
    }
};

// 807820 Dust Toss: "dazing them for $d". Effect 1 is effect 178 (TriggerSpell 1604 Dazed), which the
// core serves with EffectNULL. Dazed keeps its own duration, the $d of the tooltip (index 35 on both).
class spell_ascension_ranger_dust_toss : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_dust_toss);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_DAZED}); }
    bool Load() override { return IsRanger(GetCaster()); }

    void Daze(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        Unit* target = GetHitUnit();
        uint32 daze = GetSpellInfo()->Effects[index].TriggerSpell;
        if (target && target->IsAlive() && daze)
            GetCaster()->CastSpell(target, daze, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_ranger_dust_toss::Daze, EFFECT_1, SPELL_EFFECT_ANY);
    }
};

// 800077 Barbed Quills: "Damage dealt by Wild Strike, Flank and Quills will now tear into enemies,
// causing them to bleed [801472] [...]. Damage dealt by Quills now spreads your periodic effects to
// $560965i enemies within $560965a yds and extends their duration by $/1000;561161s1 sec."
// Its three auras 42 had no proc engine; the bleed is carried by none of them (803856 holds it, and
// nothing applies it). Their triggers cannot be used as they are: 560965 is effect 169 (EffectNULL in
// the core) and 561161/561168 are effect 177, which reads the exact rank-1 id on any caster. So the
// proc (spell_proc 800077: Wild Strike, Flank, Quills) does the work here: the ids, radius, target
// count and extension are read from those three records, ranks follow spell_ranks, and only this
// Ranger's auras are touched.
class aura_ascension_ranger_barbed_quills : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_barbed_quills);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_BARBED_QUILLS_BLEED, SPELL_BARBED_QUILLS_SPREAD,
            SPELL_BARBED_QUILLS_EXTEND, SPELL_BARBED_QUILLS_EXTEND_2});
    }

    bool CheckProc(ProcEventInfo& event)
    {
        return IsRanger(GetTarget()) && IsOwnHostileHit(GetTarget(), event) && event.GetSpellInfo() &&
            event.GetActionTarget()->IsAlive() && event.GetDamageInfo()->GetDamageType() != DOT;
    }

    static void Extend(Unit* owner, Unit* victim)
    {
        for (uint32 id : {uint32(SPELL_BARBED_QUILLS_EXTEND), uint32(SPELL_BARBED_QUILLS_EXTEND_2)})
        {
            SpellInfo const* extender = sSpellMgr->GetSpellInfo(id);
            for (SpellEffectInfo const& effect : extender->Effects)
            {
                if (effect.Effect != SPELL_EFFECT_ASCENSION_MODIFY_AURA_DURATION || effect.MiscValue <= 0)
                    continue;
                Aura* aura = victim->GetAuraOfRankedSpell(uint32(effect.MiscValue), owner->GetGUID());
                int32 bonus = effect.CalcValue(owner);
                if (!aura || aura->GetMaxDuration() <= 0 || bonus <= 0)
                    continue;
                int32 duration = aura->GetDuration() + bonus;
                if (duration > aura->GetMaxDuration())
                    aura->SetMaxDuration(duration);
                aura->SetDuration(duration);
            }
        }
    }

    static void Spread(Unit* owner, Unit* victim)
    {
        SpellInfo const* spread = sSpellMgr->GetSpellInfo(SPELL_BARBED_QUILLS_SPREAD);
        float radius = spread->Effects[EFFECT_0].CalcRadius(owner);
        uint32 count = spread->MaxAffectedTargets;
        if (radius <= 0.0f || !count)
            return;
        std::list<Unit*> candidates;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(victim, owner, radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(victim, candidates, check);
        Cell::VisitObjects(victim, searcher, radius);
        candidates.remove_if([&](Unit* unit)
        {
            return !unit || unit == victim || unit == owner || !unit->IsAlive() ||
                !owner->IsValidAttackTarget(unit) || !victim->IsWithinLOSInMap(unit);
        });
        candidates.sort([&](Unit const* left, Unit const* right)
        {
            float l = victim->GetExactDist(left);
            float r = victim->GetExactDist(right);
            return l == r ? left->GetGUID() < right->GetGUID() : l < r;
        });
        if (candidates.size() > count)
            candidates.resize(count);
        for (SpellEffectInfo const& effect : spread->Effects)
        {
            if (effect.Effect != SPELL_EFFECT_ASCENSION_SPREAD_AURA || !effect.TriggerSpell)
                continue;
            Aura const* source = victim->GetAuraOfRankedSpell(effect.TriggerSpell, owner->GetGUID());
            if (!source || source->GetDuration() <= 0)
                continue;
            for (Unit* unit : candidates)
            {
                if (unit->GetAuraOfRankedSpell(effect.TriggerSpell, owner->GetGUID()))
                    continue;
                // The rank held, the time left and the stacks of the source.
                if (Aura* copy = owner->AddAura(source->GetId(), unit))
                {
                    copy->SetMaxDuration(source->GetMaxDuration());
                    copy->SetDuration(source->GetDuration());
                    if (source->GetStackAmount() > 1)
                        copy->SetStackAmount(source->GetStackAmount());
                }
            }
        }
    }

    void Proc(ProcEventInfo& event)
    {
        // The three auras 42 would cast 560965/561161/561168 with their dead effects.
        PreventDefaultAction();
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        bool const quills = event.GetSpellInfo()->SpellFamilyFlags[1] & 0x4;
        if (quills)
            Extend(owner, victim);
        owner->CastSpell(victim, SPELL_BARBED_QUILLS_BLEED, true, nullptr, GetEffect(EFFECT_0));
        // Once per Quills cast, from the first enemy struck, not once per dart.
        Spell* spell = const_cast<Spell*>(event.GetProcSpell());
        if (!quills || !spell || spell->GetScriptValue(SPELL_BARBED_QUILLS) || !victim->IsAlive())
            return;
        spell->SetScriptValue(SPELL_BARBED_QUILLS, 1);
        Spread(owner, victim);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ranger_barbed_quills::CheckProc);
        OnProc += AuraProcFn(aura_ascension_ranger_barbed_quills::Proc);
    }
};

// 705034 Woodland Stalker ("While in Elude, your critical strike chance is increased") and 706284
// Pathfinder ("Adrenaline Rush [...] reduced cooldown when used while Elude is active") only modify
// 524969 Elude SLS3, which nothing applied: AscensionClassMechanics.cpp poses 524862 and 524886 only.
// 524969 is worth nothing without those talents (cooldown +0%, crit +0, dummy), so it follows Elude
// for every Ranger. Talent amounts are read when it is applied, i.e. at the next Elude.
class aura_ascension_ranger_elude_talents : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_elude_talents);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_ELUDE_TALENTS}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        // AddAura, not CastSpell: Elude can be re-applied from the saved auras at login, before the
        // player is in the world.
        if (IsRanger(owner) && GetCasterGUID() == owner->GetGUID() && !owner->HasAura(SPELL_ELUDE_TALENTS))
            owner->AddAura(SPELL_ELUDE_TALENTS, owner);
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        // Onslaught grants the same benefits: leaving Elude under Onslaught keeps them.
        if (GetCasterGUID() == owner->GetGUID() && !owner->HasAura(SPELL_ONSLAUGHT, owner->GetGUID()))
            owner->RemoveAurasDueToSpell(SPELL_ELUDE_TALENTS, owner->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_elude_talents::Apply,
            EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_elude_talents::Remove,
            EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// 801951 Onslaught: "granting the benefits of Elude without entering stealth for $d". Only Forest
// Dweller accepted Onslaught (AscensionClassMechanics19To25.cpp); Woodland Stalker and Pathfinder
// live on 524969, which followed Elude alone. Stealth (524862, 524886) and Elusive Character, which
// belongs to leaving Elude, are not granted. The aura being removed is already out of the owner's
// applied auras in AfterEffectRemove: each side tests the other one only.
class aura_ascension_ranger_onslaught_elude_benefits : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_onslaught_elude_benefits);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_ELUDE_TALENTS}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        if (IsRanger(owner) && GetCasterGUID() == owner->GetGUID() && !owner->HasAura(SPELL_ELUDE_TALENTS))
            owner->AddAura(SPELL_ELUDE_TALENTS, owner);
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        if (GetCasterGUID() == owner->GetGUID() && !owner->HasAura(SPELL_ELUDE, owner->GetGUID()))
            owner->RemoveAurasDueToSpell(SPELL_ELUDE_TALENTS, owner->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_onslaught_elude_benefits::Apply,
            EFFECT_0, SPELL_AURA_ABILITY_IGNORE_AURASTATE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_onslaught_elude_benefits::Remove,
            EFFECT_0, SPELL_AURA_ABILITY_IGNORE_AURASTATE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 570014 Brutal Shot: "While Skirmish is active this ability is guaranteed to critically strike". The
// +100 CRITICAL_CHANCE is 803335 Skirmish SLS, which nothing applied (no trigger, link or code). It
// follows Skirmish 802039 (effect 1, its crit aura) the way 524969 follows Elude.
class aura_ascension_ranger_skirmish_brutal_shot : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_skirmish_brutal_shot);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_SKIRMISH_BRUTAL_SHOT}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        if (IsRanger(owner) && GetCasterGUID() == owner->GetGUID() && !owner->HasAura(SPELL_SKIRMISH_BRUTAL_SHOT))
            owner->AddAura(SPELL_SKIRMISH_BRUTAL_SHOT, owner);
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        if (GetCasterGUID() == owner->GetGUID())
            owner->RemoveAurasDueToSpell(SPELL_SKIRMISH_BRUTAL_SHOT, owner->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_skirmish_brutal_shot::Apply,
            EFFECT_1, SPELL_AURA_ASCENSION_MOD_CRIT_CHANCE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_skirmish_brutal_shot::Remove,
            EFFECT_1, SPELL_AURA_ASCENSION_MOD_CRIT_CHANCE, AURA_EFFECT_HANDLE_REAL);
    }
};
}

void HandleAscensionRangerStonemason(Spell* spell, Player* player)
{
    SpellInfo const* info = spell->GetSpellInfo();
    if (player->getClass() != CLASS_RANGER || info->SpellFamilyName != 27 ||
        !(info->SpellFamilyFlags[1] & (32768 | 134217728)) || !player->HasAura(SPELL_STONEMASONS_SECRET) ||
        !player->HasAura(SPELL_DIRTY_BLADES, player->GetGUID()))
        return;
    if (Aura const* advantage = player->GetAura(SPELL_ADVANTAGE); advantage && advantage->GetStackAmount() == 5)
        player->CastSpell(player, SPELL_EXTEND_DIRTY_BLADES, true);
}

void ApplyAscensionRangerTalentContracts(SpellInfo* info)
{
    if (info->Id == SPELL_KNOCKOUT_INCAPACITATE && info->SpellFamilyName == 27)
        info->AuraInterruptFlags |= AURA_INTERRUPT_FLAG_TAKE_DAMAGE;
    if (info->Id == SPELL_SNATCH_DISARM && info->SpellFamilyName == 27)
        if (SpellInfo const* parent = sSpellMgr->GetSpellInfo(SPELL_SNATCH))
            info->DurationEntry = parent->DurationEntry;

    if (info->SpellFamilyName != 27)
        return;
    switch (info->Id)
    {
        case SPELL_NEUROTOXIN_SILENCE:
            // Launched by 500071 at its launch phase, then delayed by its own speed 80: the aura
            // was created at impact, after the Advantage (and its 704337 DURATION companion) was
            // spent. At speed 0 it applies at launch, like Cutthroat 805958. Speed is visual only.
            if (info->Speed == 80.0f)
                info->Speed = 0.0f;
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Neurotoxin silence record {}", info->Id);
            break;
        case SPELL_SERRATIONS_1:
        case SPELL_SERRATIONS_2:
        {
            // "Increases your damage against bleeding or Poisoned targets". Effect 1 stayed a copied
            // OVERRIDE_CLASS_SCRIPTS 20007 that no code reads; it becomes the native per-state bonus,
            // global like the bleeding half (effect 0, aura 303, MiscValueB 0).
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            int32 const bp = info->Id == SPELL_SERRATIONS_1 ? 4 : 9;
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_OVERRIDE_CLASS_SCRIPTS &&
                effect.MiscValue == ASCENSION_CLASSMASK_AURASTATE_DAMAGE &&
                effect.MiscValueB == int32(AURA_STATE_ASCENSION_POISONED) && effect.BasePoints == bp &&
                effect.TargetA.GetTarget() == TARGET_UNIT_CASTER)
            {
                effect.ApplyAuraName = SPELL_AURA_MOD_DAMAGE_DONE_VERSUS_AURASTATE;
                effect.MiscValue = int32(AURA_STATE_ASCENSION_POISONED);
                effect.MiscValueB = 0;
            }
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Serrations record {}", info->Id);
            break;
        }
        case SPELL_SKIRMISHERS_QUIVER:
        {
            // "Increases your Physical damage and your critical damage": effect 1 held the Nature
            // school only. The field is read by the server only.
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_CRIT_DAMAGE_BONUS &&
                effect.MiscValue == int32(SPELL_SCHOOL_MASK_NATURE))
                effect.MiscValue = int32(SPELL_SCHOOL_MASK_ALL);
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Skirmisher's Quiver record {}", info->Id);
            break;
        }
        case SPELL_SHARPSHOOTER:
        {
            // "Adds $s1% to the bonuses that your stacks of Advantage provide". The flat +25 on
            // SPELLMOD_ALL_EFFECTS added 25 to every effect of 804329/801429/801700, including
            // Quills' one-target count (x stacks: 130 targets) and Rub It In's 20% chance. As a
            // percentage it adds a quarter to each bonus, and one target stays one target.
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
                effect.MiscValue == SPELLMOD_ALL_EFFECTS && effect.BasePoints == 24 &&
                effect.SpellClassMask == flag96(0, 0x200000, 0))
                effect.ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Sharpshooter record {}", info->Id);
            break;
        }
        case SPELL_WILD_MAN:
        {
            // "deals reduced damage with each enemy struck": the -10% DAMAGE_MULTIPLIER only reached
            // the off-hand 560962, which does not jump. The jumps are Wild Strike's (FF1 0x100).
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER &&
                effect.MiscValue == SPELLMOD_DAMAGE_MULTIPLIER && effect.SpellClassMask == flag96(0x40000000, 0, 0))
                effect.SpellClassMask = flag96(0x40000000, 0x100, 0);
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Wild Man record {}", info->Id);
            break;
        }
        case SPELL_BOUNTY:
        {
            // "take $s1% increased damage from your Advantage spenders": the class mask held Assault,
            // Skullpiercer, Serrated Shot and Incendiary Shot only. Added: Whipvine Arrow (FF0
            // 0x10000), Precision Shot (FF1 0x800000), Emerald Arrow (FF1 0x4000000), Quills (FF1 0x4,
            // not FF2 0x20000 which Corrosive Shot shares), Cutthroat (FF2 0x1000000), Woodland and
            // Neurotoxin Arrow (FF2 0x2000). Scanned: no other family-27 spell carries these bits.
            // Spenders whose SpellFamilyFlags are zero stay out of reach of any mask.
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_FROM_CASTER &&
                effect.SpellClassMask == flag96(0x20000040, 0x8008000, 0x2000080))
                effect.SpellClassMask = flag96(0x20010040, 0x8008000 | 0x800000 | 0x4000000 | 0x4,
                    0x2000080 | 0x1000000 | 0x2000);
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Bounty record {}", info->Id);
            break;
        }
        case SPELL_DIRTY_BLADES_LEECH:
            // Its amount is a share of damage already modified: do not apply the done bonuses twice.
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            break;
        case SPELL_BACKSTEP_BONUS:
            // DurationIndex 0 and not passive: every application expired on the next update.
            // It lives as long as the leap that feeds it.
            if (!info->DurationEntry)
                if (SpellInfo const* leap = sSpellMgr->GetSpellInfo(SPELL_BACKSTEP))
                    info->DurationEntry = leap->DurationEntry;
            break;
        case SPELL_ELUDE_TALENTS:
        case SPELL_SKIRMISH_BRUTAL_SHOT:
            // Infinite helpers owned by Elude / Onslaught and Skirmish: never saved alone, their
            // owner recreates them when it is re-applied at login.
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            if (info->Id == SPELL_SKIRMISH_BRUTAL_SHOT)
            {
                // The +100 crit mask held Brutal Shot (FF0 0x80) and FF2 0x2, i.e. every Waterskin
                // rank and Bone Reliquary Heal. The text names Brutal Shot only.
                SpellEffectInfo& effect = info->Effects[EFFECT_0];
                if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
                    effect.MiscValue == SPELLMOD_CRITICAL_CHANCE && effect.SpellClassMask == flag96(0x80, 0, 0x2))
                    effect.SpellClassMask = flag96(0x80, 0, 0);
                else
                    LOG_ERROR("module.ascension_compat", "Skipped unexpected Skirmish SLS record {}", info->Id);
            }
            break;
        case SPELL_WOODSMANS_ADAPTATION_1:
            // Every other Adaptation rank carries FF2 0x200, the bit Outlaw 706277 ("Increases the
            // effectiveness of your Adaptations by 20%") targets; rank 1 had no family flag at all.
            if (info->SpellFamilyFlags[0] == 0 && info->SpellFamilyFlags[1] == 0 && info->SpellFamilyFlags[2] == 0)
                info->SpellFamilyFlags[2] |= 0x200;
            else
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Woodsman's Adaptation rank 1 record {}", info->Id);
            break;
        default:
            break;
    }
}

void AddSC_AscensionRangerTalents()
{
    RegisterSpellScript(spell_ascension_ranger_light_arrows);
    RegisterSpellScript(spell_ascension_ranger_knockout);
    RegisterSpellScript(aura_ascension_ranger_dirty_blades);
    RegisterSpellScript(spell_ascension_ranger_dirty_blades_leech);
    RegisterSpellScript(aura_ascension_ranger_highwayman);
    RegisterSpellScript(aura_ascension_ranger_tip_of_the_spear);
    RegisterSpellScript(aura_ascension_ranger_marked_for_death);
    RegisterSpellScript(aura_ascension_ranger_rapid_strikes);
    RegisterSpellScript(aura_ascension_ranger_pierced);
    RegisterSpellScript(aura_ascension_ranger_incendiary_arrows);
    RegisterSpellScript(spell_ascension_ranger_sly);
    RegisterSpellScript(aura_ascension_ranger_sly);
    RegisterSpellScript(aura_ascension_ranger_backstep);
    RegisterSpellScript(aura_ascension_ranger_backstep_bonus);
    RegisterSpellScript(aura_ascension_ranger_command_aura);
    RegisterSpellScript(spell_ascension_ranger_quick_demise);
    RegisterSpellScript(spell_ascension_ranger_corrosive_shot);
    RegisterSpellScript(spell_ascension_ranger_dust_toss);
    RegisterSpellScript(aura_ascension_ranger_barbed_quills);
    RegisterSpellScript(aura_ascension_ranger_elude_talents);
    RegisterSpellScript(aura_ascension_ranger_onslaught_elude_benefits);
    RegisterSpellScript(aura_ascension_ranger_skirmish_brutal_shot);
}
