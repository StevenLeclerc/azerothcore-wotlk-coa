/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Log.h"
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
constexpr uint32 AdvantageCompanions[] = {704337, 801429, 801700};

class aura_ascension_ranger_advantage : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_advantage);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({704337, 801429, 801700}); }

    bool Load() override
    {
        return GetCaster() && GetCaster() == GetUnitOwner() && GetCaster()->IsPlayer() &&
            GetCaster()->getClass() == CLASS_RANGER;
    }

    void Sync(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* player = GetTarget();
        for (uint32 id : AdvantageCompanions)
        {
            Aura* companion = player->GetAura(id, player->GetGUID());
            if (!companion)
                companion = player->AddAura(id, player);
            if (companion && companion->GetStackAmount() != GetStackAmount())
                companion->SetStackAmount(GetStackAmount());
        }
    }

    void Clear(AuraEffect const*, AuraEffectHandleModes)
    {
        for (uint32 id : AdvantageCompanions)
            GetTarget()->RemoveAurasDueToSpell(id, GetTarget()->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ranger_advantage::Sync,
            EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ranger_advantage::Clear,
            EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

class ranger_secondary_hits : public AllSpellScript
{
public:
    ranger_secondary_hits() : AllSpellScript("ranger_secondary_hits",
        {ALLSPELLHOOK_ON_BEFORE_EFFECTS, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        // Viper's Bite 803104: "If the target is poisoned, each strike deals Nature damage instead".
        // Each strike is a cast of 803105 (school Physical); set its school before the effects run,
        // so armor, resistances and the combat log all see Nature.
        if (info->Id == 803105 && caster->IsPlayer() && caster->getClass() == CLASS_RANGER)
            if (Unit* target = spell->m_targets.GetUnitTarget())
                if (target->HasAuraState(AURA_STATE_ASCENSION_POISONED))
                    spell->SetSpellSchoolMask(SPELL_SCHOOL_MASK_NATURE);
        if (caster->IsPlayer() && caster->getClass() == CLASS_RANGER && info->SpellFamilyName == 27 &&
            !spell->IsTriggered() && sSpellMgr->GetFirstSpellInChain(info->Id) == 804712)
            if (Aura const* advantage = caster->GetAura(804329, caster->GetGUID()))
            {
                // The projectile must retain the resource count from launch. "$/1000;801700s1 sec
                // for each stack" is the Advantage companion 801700 e0 (DURATION on the flare, x
                // stacks, Sharpshooter included); 2 s per stack only if the companion is missing.
                int32 duration = 2000 * advantage->GetStackAmount();
                if (AuraEffect const* share = caster->GetAuraEffect(801700, EFFECT_0, caster->GetGUID()))
                    if (share->GetAmount() > 0)
                        duration = share->GetAmount();
                spell->SetScriptValue(801935, uint64(duration));
            }
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32, uint32, bool) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        if (!player || player->getClass() != CLASS_RANGER || spell->GetSpellInfo()->SpellFamilyName != 27 ||
            miss != SPELL_MISS_NONE || !target || target == player || player->IsFriendlyTo(target))
            return;
        if (!spell->IsTriggered() && sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id) == 804712 &&
            !spell->GetScriptValue(804712))
        {
            spell->SetScriptValue(804712, 1);
            if (uint64 duration = spell->GetScriptValue(801935))
                if (SpellInfo const* flare = sSpellMgr->GetSpellInfo(801935))
                {
                    SpellCastTargets targets;
                    targets.SetUnitTarget(target);
                    targets.SetDst(target->GetPosition());
                    CustomSpellValues values;
                    values.AddSpellMod(SPELLVALUE_AURA_DURATION, int32(duration));
                    player->CastSpell(targets, flare, &values, TRIGGERED_FULL_MASK);
                }
        }
        if (spell->GetSpellInfo()->Id != 803105 || !target->IsAlive() ||
            !target->HasAuraState(AURA_STATE_BLEEDING) || spell->GetScriptValue(803106))
            return;
        // Each of the channel's four native weapon strikes owns its own bonus.
        // A fully absorbed strike still landed; misses cannot trigger the bonus.
        spell->SetScriptValue(803106, 1);
        player->CastSpell(target, 803106, true);
    }
};

// Toxic Dart (chain 807237, eight ranks): "Periodic damage dealt by this benefits from your ranged
// haste". AuraEffect::CalculatePeriodic only knows spell haste (SPELL_ATTR5_SPELL_HASTE_AFFECTS_
// PERIODIC, UNIT_MOD_CAST_SPEED); the record has attr5 0x200 (EXTRA_INITIAL_PERIOD) only. The
// hook runs before SPELLMOD_ACTIVATION_TIME (SpellAuraEffects.cpp:702-716), which still applies.
// The duration is untouched: more ticks over the same time.
class aura_ascension_ranger_toxic_dart_haste : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_toxic_dart_haste);

    void CalcPeriodic(AuraEffect const*, bool& isPeriodic, int32& amplitude)
    {
        Unit* caster = GetCaster();
        if (!isPeriodic || amplitude <= 0 || !caster || !caster->IsPlayer() || caster->getClass() != CLASS_RANGER)
            return;
        float const haste = caster->m_modAttackSpeedPct[RANGED_ATTACK];
        if (haste > 0.0f && haste < 1.0f)
            amplitude = std::max(1, int32(amplitude * haste));
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_ranger_toxic_dart_haste::CalcPeriodic,
            EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// 680469 Masterful Archery: "While in Elude, gain a stack of Masterful Archery every $T1 sec."
// The passive ticks 573450 -> 680471 at all times; nothing in the records ties it to Elude.
// (680471's consumption by the next Precision Shot is spell_proc data.)
class aura_ascension_ranger_masterful_archery : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_masterful_archery);

    static constexpr uint32 Elude = 801345;
    static constexpr uint32 Onslaught = 801951; // "granting the benefits of Elude without entering stealth"

    void Tick(AuraEffect const*)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(Elude, target->GetGUID()) && !target->HasAura(Onslaught, target->GetGUID()))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_ranger_masterful_archery::Tick,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 800358 Hawkeye: "You now fire an arrow every $t1 sec at enemies within $a1 yds affected by your
// Hunting Shot". Effect 0 is an enemy area aura (129) carrying the 801192 periodic trigger, and the
// core fills it with every attackable unit in range. Only this Ranger's Hunting Shot marks
// (chain 801191, eight ranks) qualify; the owner's own application is always kept.
class aura_ascension_ranger_hawkeye : public AuraScript
{
    PrepareAuraScript(aura_ascension_ranger_hawkeye);

    static constexpr uint32 HuntingShot = 801191;

    bool CheckTarget(Unit* target)
    {
        return target == GetUnitOwner() || (target && target->GetAuraOfRankedSpell(HuntingShot, GetCasterGUID()));
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(aura_ascension_ranger_hawkeye::CheckTarget);
    }
};

// 806342 Whipvine Arrow: "Hits 1 additional nearby target per stack of Advantage". The record jumps
// to ChainTarget - 1 targets whatever the stacks (raised to 6 below, the MaxAffectedTargets of the
// record, so 5 stacks reach 1 + 5). The jump list excludes the primary target (Spell.cpp:1905), and
// target selection runs before the Advantage is spent (SelectSpellTargets precedes OnSpellCast).
// For TARGET_UNIT_TARGET_ENEMY the chain is selected effect by effect (Spell.cpp:1880), so the
// filter runs once per effect list: EFFECT_ALL trims each list to the same stack count.
class spell_ascension_ranger_whipvine_arrow : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_whipvine_arrow);

    static constexpr uint32 Advantage = 804329;

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->getClass() == CLASS_RANGER;
    }

    void FilterJumps(std::list<WorldObject*>& targets)
    {
        Aura const* advantage = GetCaster()->GetAura(Advantage, GetCaster()->GetGUID());
        std::size_t const stacks = advantage ? advantage->GetStackAmount() : 0;
        if (targets.size() > stacks)
            targets.resize(stacks);
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ascension_ranger_whipvine_arrow::FilterJumps,
            EFFECT_ALL, TARGET_UNIT_TARGET_ENEMY);
    }
};

// "(8 sec vs players)": Blackjack 520568 (sap), Whipvine Arrow 806342 (root) and Befriend Beast 562301
// (possess) last 40 s, and the core's generic crowd control limit cuts them to 10 s against a player
// or a player's pet. Same target test as Unit::ApplyDiminishingToDuration and Templar 850010; the
// diminishing returns then apply to the 8 s. Creatures keep the full duration. An aura the Ranger
// carries himself (Befriend Beast e2, dummy on the caster) is left alone: its holder is a player,
// but it follows the possession of a creature, which lasts 40 s.
class ranger_crowd_control_duration : public AllSpellScript
{
public:
    ranger_crowd_control_duration() : AllSpellScript("ranger_crowd_control_duration",
        {ALLSPELLHOOK_ON_CALC_MAX_DURATION}) { }

    void OnCalcMaxDuration(Aura const* aura, int32& duration) override
    {
        if (!aura || aura->GetType() != UNIT_AURA_TYPE || duration <= 0)
            return;
        uint32 const id = aura->GetId();
        if (id != 520568 && id != 806342 && id != 562301)
            return;
        Unit* caster = aura->GetCaster();
        Player* player = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!player || player->getClass() != CLASS_RANGER)
            return;
        Unit const* victim = aura->GetUnitOwner();
        if (!victim || victim == caster || victim == player)
            return;
        if (Unit const* owner = victim->GetOwner(); (owner ? owner : victim)->IsPlayer())
            duration = std::min(duration, 8000);
    }
};

class ranger_secondary_contracts : public GlobalScript
{
public:
    ranger_secondary_contracts() : GlobalScript("ranger_secondary_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != 27)
            return;
        if (info->Id == 560805)
            // The native negative exclusion applies Branded on a landed hit and
            // rejects another brand until its independent 90-second timer ends.
            info->ExcludeTargetAuraSpell = 570167;
        if (info->Id == 801935)
            info->UseRangedAttackPowerForDamage = true;
        if (info->Id == 586212)
            // Quel'dorei Poison tick: "+$RAP*.15" (spell_bonus_data ap_dot_bonus). DmgClass 1 without
            // the ranged slot: IsRangedWeaponSpell is false and melee AP would be read.
            info->UseRangedAttackPowerForDamage = true;
        if (info->Id == 806342)
        {
            // Whipvine Arrow: 1 + one target per Advantage stack, filtered by
            // spell_ascension_ranger_whipvine_arrow. ChainTarget 5 capped 5 stacks at 5 targets.
            SpellEffectInfo& root = info->Effects[EFFECT_0];
            SpellEffectInfo& damage = info->Effects[EFFECT_1];
            if (root.ChainTarget == 5 && damage.ChainTarget == 5 && info->MaxAffectedTargets == 6)
                root.ChainTarget = damage.ChainTarget = 6;
            else if (root.ChainTarget != 6 || damage.ChainTarget != 6)
                LOG_ERROR("module.ascension_compat", "Skipped unexpected Whipvine Arrow record {}", info->Id);
        }
        for (uint32 id : AdvantageCompanions)
            if (info->Id == id)
            {
                // Recreated from the resource itself, including its saved stack count.
                info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
                info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            }
    }
};
}

void AddSC_AscensionRangerSecondary()
{
    RegisterSpellScript(aura_ascension_ranger_advantage);
    RegisterSpellScript(aura_ascension_ranger_toxic_dart_haste);
    RegisterSpellScript(aura_ascension_ranger_masterful_archery);
    RegisterSpellScript(aura_ascension_ranger_hawkeye);
    RegisterSpellScript(spell_ascension_ranger_whipvine_arrow);
    new ranger_crowd_control_duration();
    new ranger_secondary_hits();
    new ranger_secondary_contracts();
}
