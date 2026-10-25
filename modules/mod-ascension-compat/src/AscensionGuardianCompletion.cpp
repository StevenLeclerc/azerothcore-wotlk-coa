/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionGuardianCompletion.h"
#include "AscensionContract.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
// G-28: a Guardian rewrite whose Spell.dbc layout moved is journalled once and counted against the
// "Guardian" contract of AscensionContract::Report(), instead of being applied blind or skipped in silence.
bool GuardianRefuse(SpellInfo const* info, uint8 slot, char const* check)
{
    if (AscensionContract::Detail::FirstFailure("Guardian",
            AscensionContract::Detail::Key("Guardian", info->Id, slot, check)))
        LOG_ERROR("module.ascension_compat",
            "Ascension contract Guardian: spell {} slot {} is effect {} aura {} misc {} trigger {}; "
            "its '{}' rewrite is not applied.", info->Id, slot, uint32(info->Effects[slot].Effect),
            uint32(info->Effects[slot].ApplyAuraName), info->Effects[slot].MiscValue,
            info->Effects[slot].TriggerSpell, check);
    return false;
}

bool GuardianExpect(SpellInfo const* info, uint8 slot, uint32 effect, uint32 aura, char const* check)
{
    SpellEffectInfo const& current = info->Effects[slot];
    return (current.Effect == effect && current.ApplyAuraName == aura) || GuardianRefuse(info, slot, check);
}

Player* GuardianPlayer(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_GUARDIAN ? player : nullptr;
}

float ShieldValue(Player const* player)
{
    Item const* shield = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    return shield && !shield->IsBroken() && shield->GetTemplate()->InventoryType == INVTYPE_SHIELD ?
        float(player->GetShieldBlockValue()) : 0.0f;
}

uint32 Nearby(Unit* unit, bool allies, float range)
{
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(unit, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(unit, units, check);
    Cell::VisitObjects(unit, searcher, range);
    return uint32(std::count_if(units.begin(), units.end(), [=](Unit* target)
    {
        if (target == unit || !target->IsAlive() || !unit->InSamePhase(target))
            return false;
        // G-22 Strength in Unison (300860), the only caller with allies = true: "for each ally". Pets,
        // totems and the Guardian's own standards are player-controlled units too, and each one used
        // to add 1% haste. Only players (bots included) count.
        return allies ? target->IsPlayer() && unit->IsFriendlyTo(target) && target->IsControlledByPlayer() :
            unit->IsValidAttackTarget(target);
    }));
}

bool Dynamic(uint32 id)
{
    return id == 300860 || id == 505199 || id == 705381 || id == 807794 || id == 707662;
}

int32 DynamicAmount(Unit* owner, uint32 id)
{
    Player* player = GuardianPlayer(owner);
    if (!player)
        return 0;
    switch (id)
    {
        case 300860:
            return int32(std::min(20u, Nearby(owner, true,
                sSpellMgr->GetSpellInfo(id)->Effects[EFFECT_0].CalcRadius(owner))));
        case 505199:
            return -int32(std::min(sSpellMgr->GetSpellInfo(id)->MaxAffectedTargets, Nearby(owner, false,
                sSpellMgr->GetSpellInfo(id)->Effects[EFFECT_1].CalcRadius(owner))));
        case 705381: return int32(player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_BLOCK));
        case 807794: return int32(player->GetStat(STAT_STAMINA) * 0.2f);
        // 707662 Footman's Call. The flat 15 is sourced: Spell.dbc effect 0 has
        // BasePoints 14 and DieSides 1. The 0.3 coefficient is NOT sourced anywhere --
        // not in 707662's tooltip ("Damage dealt by Pulverize now scales with your
        // block rating"), not in 705376's ("... plus an additional amount scaling with
        // your block value"), not in any DBC field, and not upstream (707662 and 705376
        // each have zero entries in the upstream issue tracker). It came in with the
        // single-commit tree import, so git cannot attribute it either. And with the
        // previous (4, 0, 0) class mask below the amount reached nothing, so this
        // coefficient has never been live: MEASURE IT IN GAME BEFORE RELYING ON IT.
        // The two tooltips also name different quantities. ShieldValue() reads
        // GetShieldBlockValue(), i.e. block *value*, which follows 705376, the talent
        // that puts this aura on the player; 705381 above reads CR_BLOCK, i.e. block
        // *rating*, for a neighbouring case. Not arbitrated here.
        case 707662: return 15 + int32(ShieldValue(player) * 0.3f);
        default: return 0;
    }
}

class guardian_scaling : public UnitScript
{
public:
    guardian_scaling() : UnitScript("guardian_scaling", true, { UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE }) { }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        if (caster && info->Id == 800629 && !index)
        {
            Player const* owner = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (owner && owner->getClass() == CLASS_GUARDIAN && owner->HasAura(802296))
                value -= 15.0f;
            return;
        }
        Player const* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_GUARDIAN || info->SpellFamilyName != 24)
            return;
        uint32 id = info->Id;
        if (!index && AscensionGuardian::Ram(id))
            value += ShieldValue(player);
        else if (!index && (AscensionGuardian::HeavyBlow(id) || id == 802629))
            value += player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_DEFENSE_SKILL) * 0.5f;
        else if (!index && id == 573212)
            value += player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_DEFENSE_SKILL) * 0.2f;
        else if (!index && id == 803131)
            value += player->GetStat(STAT_STAMINA) * 0.0375f;
        else if (!index && id == 707140)
            value += player->GetStat(STAT_STAMINA) * 0.1f + player->GetTotalAttackPowerValue(BASE_ATTACK) * 0.11f;
        else if (!index && id == 520651)
            value += player->GetTotalAttackPowerValue(BASE_ATTACK) * 0.1f;
        value = std::clamp(value, -float(std::numeric_limits<int32>::max() / 2),
            float(std::numeric_limits<int32>::max() / 2));
    }
};

class aura_ascension_guardian_lifecycle : public AuraScript
{
    PrepareAuraScript(aura_ascension_guardian_lifecycle);

    void Calculate(AuraEffect const* effect, int32& amount, bool& recalculate)
    {
        if (effect->GetEffIndex() == EFFECT_0 && Dynamic(GetId()))
        {
            amount = DynamicAmount(GetUnitOwner(), GetId());
            recalculate = false;
        }
    }

    void Periodic(AuraEffect const* effect, bool& periodic, int32& amplitude)
    {
        if (effect->GetEffIndex() == EFFECT_0 && Dynamic(GetId()))
        {
            periodic = true;
            amplitude = 1000;
        }
    }

    void Tick(AuraEffect const* effect)
    {
        if (effect->GetEffIndex() != EFFECT_0)
            return;
        if (Dynamic(GetId()))
        {
            PreventDefaultAction();
            int32 amount = DynamicAmount(GetTarget(), GetId());
            if (effect->GetAmount() != amount)
                GetEffect(EFFECT_0)->ChangeAmount(amount);
        }
        else if (GetId() == 504139 && !GetTarget()->IsInCombat())
        {
            PreventDefaultAction();
            GetTarget()->RemoveAurasDueToSpell(504145);
        }
    }

    void Apply(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        for (uint8 i = 0; i < effect->GetEffIndex(); ++i)
            if (GetEffect(i))
                return;
        Unit* owner = GetTarget();
        if (GetId() == 503634)
            owner->CastSpell(owner, 573255, true, nullptr, effect);
        else if (GetId() == 524610)
        {
            if (GetStackAmount() >= 5)
                if (Unit* caster = GetCaster())
                    caster->CastSpell(owner, 524609, true, nullptr, effect);
        }
        else if (GetId() == 801774 && owner == GetCaster())
            owner->CastSpell(owner, 803524, true, nullptr, effect);
        else if (GetId() == 705376)
            owner->CastSpell(owner, 707662, true, nullptr, effect);
        else if (GetId() == 803738)
            owner->CastSpell(owner, 807794, true, nullptr, effect);
        else if (GetId() == 300542 || GetId() == 803130)
            owner->UpdateSpeed(MOVE_RUN, true);
        else if (GetId() == 804691 && GetCaster() && GetCaster()->HasAura(503631))
        {
            owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_ROOT, true);
            owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SNARE, true);
            owner->RemoveAurasWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE), AURA_REMOVE_BY_DEFAULT);
        }
    }

    void Removed(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        for (uint8 i = 0; i < effect->GetEffIndex(); ++i)
            if (GetEffect(i))
                return;
        Unit* owner = GetTarget();
        AuraRemoveMode mode = GetTargetApplication()->GetRemoveMode();
        switch (GetId())
        {
            case 803956:
                if (mode == AURA_REMOVE_BY_EXPIRE && owner->IsAlive() && owner->IsInWorld())
                    owner->CastSpell(owner, 572329, true, nullptr, effect);
                break;
            case 801774: owner->RemoveAurasDueToSpell(803524, owner->GetGUID()); break;
            case 705376: owner->RemoveAurasDueToSpell(707662); break;
            case 803738: owner->RemoveAurasDueToSpell(807794); break;
            case 807297: owner->RemoveAurasDueToSpell(705381); break;
            case 804891: owner->RemoveAurasDueToSpell(572158); break;
            case 706514:
                owner->RemoveAurasDueToSpell(706516);
                owner->RemoveAurasDueToSpell(707138);
                owner->RemoveAurasDueToSpell(803432);
                break;
            case 504139: owner->RemoveAurasDueToSpell(504145); break;
            case 300542:
            case 803130:
                owner->UpdateSpeed(MOVE_RUN, true);
                break;
            case 804691:
                owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_ROOT, false);
                owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SNARE, false);
                break;
            default:
                break;
        }
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_guardian_lifecycle::Calculate,
            EFFECT_ALL, SPELL_AURA_ANY);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_guardian_lifecycle::Periodic,
            EFFECT_ALL, SPELL_AURA_ANY);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_guardian_lifecycle::Tick, EFFECT_ALL, SPELL_AURA_ANY);
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_guardian_lifecycle::Apply,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_guardian_lifecycle::Removed,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};
}

void AscensionGuardian::AddParagon(Player* player, std::uint8_t amount)
{
    if (!GuardianPlayer(player) || !player->HasAura(706514) || player->HasAura(707138))
        return;
    for (uint8 i = 0; i < amount; ++i)
        player->CastSpell(player, 706516, true);
    Aura const* stacks = player->GetAura(706516);
    if (stacks && stacks->GetStackAmount() >= 10)
    {
        player->RemoveAurasDueToSpell(706516);
        player->CastSpell(player, 707138, true);
    }
}

void AscensionGuardian::ApplyContracts(SpellInfo* info)
{
    if (!info || info->SpellFamilyName != 24)
        return;
    uint32 id = info->Id;
    if (id == 802629)
        info->PowerType = POWER_ENERGY;
    if (id == 803738)
        for (auto& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_TITAN_GRIP)
                effect.Effect = 0; // The exact mace-and-shield permission is checked by Player.
    if (id == 706807 || id == 705379 || id == 524694 || id == 804891 || id == 801778)
        if (info->Effects[EFFECT_0].ApplyAuraName == 354)
            info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
    if (id == 706808 || id == 705380 || id == 524091 || id == 575831)
    {
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
        info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
    }
    if (id == 705377)
        info->ProcFlags = 0; // Honorable Demeanor belongs to the existing block callback.
    if (id == 802895)
    {
        if (info->Effects[EFFECT_0].Effect == SPELL_EFFECT_ASCENSION_RESET_COOLDOWN &&
            info->Effects[EFFECT_0].MiscValue == 802284)
            // Guardbreaker's "Pulverize Reset" names Ram rank 1 only; the cooldown sits on the rank
            // actually cast (802284, 573204-573211, all category 1096), so clear the category.
            info->Effects[EFFECT_0].MiscValueB = 1;
        else
            // Without this line a DBC change would silently leave Guardbreaker resetting Ram rank 1 only.
            GuardianRefuse(info, EFFECT_0, "guardbreaker-ram-category");
    }
    if (id == 803963)
    {
        // G-05 Liberation: "Instantly breaks an ally free of any stun, fear, or polymorph effects and
        // grants immunity". Its three MECHANIC_IMMUNITY auras (12 stun, 5 fear, 17 polymorph) only
        // prevented a NEW effect: without SPELL_ATTR1_IMMUNITY_PURGES_EFFECT the core
        // (SpellInfo::ApplyAllSpellImmunitiesTo, HandleModMechanicImmunity) leaves the running one.
        // Same attribute as Anti-Cloaking Device (AscensionTinkerContracts.cpp).
        bool layout = true;
        for (uint8 slot = 0; slot < 3; ++slot)
            layout = GuardianExpect(info, slot, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MECHANIC_IMMUNITY,
                "liberation-purge") && layout;
        if (layout)
            info->AttributesEx |= SPELL_ATTR1_IMMUNITY_PURGES_EFFECT;
    }
    if (id == 572060)
    {
        // G-06 Battle Drums "[SLS]": the +10% damage (aura 79) and healing (aura 136) half of 570759's
        // tooltip, added to each drummed ally by go_ascension_guardian_drum. Its effect 0 triggers Sound
        // of War, which 570759 already does with five charges: removed so it cannot fire twice. Its
        // StackAmount 2 would let a second placement double the bonus: 570759 does not stack either.
        if (GuardianExpect(info, EFFECT_0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_PROC_TRIGGER_SPELL,
                "battle-drums-sound-of-war"))
        {
            if (info->Effects[EFFECT_0].TriggerSpell == 572612)
                info->Effects[EFFECT_0].Effect = 0;
            else
                GuardianRefuse(info, EFFECT_0, "battle-drums-sound-of-war");
        }
        GuardianExpect(info, EFFECT_1, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE,
            "battle-drums-damage");
        GuardianExpect(info, EFFECT_2, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_HEALING_DONE_PERCENT,
            "battle-drums-healing");
        info->StackAmount = 0;
    }
    if (id == 583027)
        // G-23 Valiant Knight: "your next Pulverize ... free of cost and 25% increased damage". The mask
        // (0, 16, 8388608) also selects Hammer of Kings (all nine ranks) and Hold the Line (803830,
        // 803939), which took the single charge. C 8388608 is carried by the eight Pulverize ranks and
        // by nothing else in family 24 (Spell.dbc, 2026-10-25).
        for (uint8 slot = 0; slot < 2; ++slot)
        {
            if (GuardianExpect(info, slot, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_ADD_PCT_MODIFIER,
                    "valiant-knight-mask") && info->Effects[slot].SpellClassMask == flag96(0, 16, 8388608))
                info->Effects[slot].SpellClassMask = flag96(0, 0, 8388608);
            else if (info->Effects[slot].SpellClassMask != flag96(0, 0, 8388608))
                GuardianRefuse(info, slot, "valiant-knight-mask");
        }
    if (id == 92106)
        // G-32 Glory: effects 1 and 2 re-apply the hidden Axe (542238) and Mace (542265) helpers every
        // 10 s whatever the weapon. Both modify Centurion Strike only (mask C 2048), and
        // PrepareAscensionClassMechanicsCast already removes both and applies the one that fits the
        // main hand before each Centurion Strike. Effect 0 (Glory stacks, 802302) is kept.
        for (uint8 slot = 1; slot < 3; ++slot)
        {
            uint32 helper = slot == 1 ? 542238 : 542265;
            if (GuardianExpect(info, slot, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_PERIODIC_TRIGGER_SPELL,
                    "glory-weapon-helpers"))
            {
                if (info->Effects[slot].TriggerSpell == helper)
                    info->Effects[slot].Effect = 0;
                else
                    GuardianRefuse(info, slot, "glory-weapon-helpers");
            }
        }
    if (id == 706636 && info->Effects[EFFECT_0].IsAura(SPELL_AURA_MOD_DEBUFF_RESISTANCE) &&
        info->Effects[EFFECT_0].MiscValue == DISPEL_DISEASE)
        // Little Drummer: "Allies within 20 yds of you now gain 20% chance to resist
        // curses." Unit::MagicSpellHitResult matches this aura's MiscValue against the
        // incoming spell's Dispel, and Spell.dbc stores 3 (disease), so the raid aura
        // resisted the wrong school of debuff. Both tooltip lines say curses.
        info->Effects[EFFECT_0].MiscValue = DISPEL_CURSE;
    if (id == 500673)
        // Advance's recast carries the Battle/Defensive Stance mask (forms 17/18), which a Guardian never has;
        // spell_ascension_guardian_ability::Check already limits the recast to a running Advance.
        info->Stances = 0;
    if (id == 524610 && info->Effects[EFFECT_1].TriggerSpell == 524609)
        info->Effects[EFFECT_1].Effect = 0;
    if (id == 524608)
    {
        info->Effects[EFFECT_0].DieSides = 1;
        info->Effects[EFFECT_1].Effect = 0;
        info->Effects[EFFECT_2].Effect = 0;
    }
    if (id == 707662)
        // Footman's Call reads "Damage dealt by Pulverize now scales with your block
        // rating". Pulverize carries SpellFamilyFlags (0, 8196, 8388608) and Spell.dbc
        // gives this effect the mask (0, 8192, 0). The previous (4, 0, 0) selected
        // Shieldforge (802195) alone and reached Pulverize on no rank; it is kept here
        // so nothing that used to be covered stops being covered.
        // What this mask actually covers, measured against Spell.dbc family 24:
        //   - A 4    -> Shieldforge 802195 only. The modifier stays inert there: this
        //              effect is SPELLMOD_EFFECT1 (MiscValue 3), which scales effect 0,
        //              and Shieldforge's effect 0 is SPELL_EFFECT_TRIGGER_SPELL, whose
        //              base points are not propagated to the triggered spell (only
        //              SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE is, SpellEffects.cpp:1239).
        //   - B 8192 -> the eight Pulverize ranks AND Shield Toss "Damage" 500461
        //              (flags (0, 8196, 0)). Shield Toss is not collateral damage: it
        //              documents itself "Scales with modifiers to Pulverize". But it is
        //              a real SPELL_EFFECT_SCHOOL_DAMAGE on up to 5 targets, so the flat
        //              bonus lands there too, once per target. With the previous
        //              (4, 0, 0) override this amount reached nothing, so both Pulverize
        //              and Shield Toss gain a bonus that has never been live before.
        info->Effects[EFFECT_0].SpellClassMask = flag96(4, 8192, 0);
    if (id == 807794)
        // Shieldlord's Strength names Heavy Blow, Hammer of Kings and Hammer of the Law.
        // Spell.dbc already holds all three: (0, 4194320, 8192), where 4194304 is Heavy
        // Blow, 16 Hammer of Kings and the third component's 8192 Hammer of the Law
        // (704418, 707710-707715). Dropping that component left Hammer of the Law out.
        info->Effects[EFFECT_0].SpellClassMask = flag96(0, 4194320, 8192);
    if (id == 525043)
        for (uint8 i = 0; i < 2; ++i)
            info->Effects[i].BasePoints = 19;
    if (id == 525043 || id == 524981)
        for (auto& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            {
                effect.Effect = SPELL_EFFECT_APPLY_AREA_AURA_RAID;
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    if (id == 705352)
        info->ExcludeCasterAuraSpell = 0;
    if (id == 503634)
    {
        info->ExcludeTargetAuraSpell = 573255;
        // Apply exhaustion after the recipient actually receives the buff.
        info->Effects[EFFECT_1].Effect = 0;
    }
    if (id == 803721)
        info->Effects[EFFECT_1].BasePoints = 1;
    if (id == 803524)
        info->Effects[EFFECT_1].Effect = 0; // The owner's haste is already on Hero's March.
    if (id == 504145 && !info->ProcFlags)
        info->ProcCharges = 1;
    if (id == 802874)
    {
        info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
        for (auto& effect : info->Effects)
            if (effect.Effect)
            {
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_DEST_AREA_ENEMY);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    }
    if (id == 570759)
        for (auto& effect : info->Effects)
            effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
}

void AddAscensionGuardianCompletionScripts()
{
    new guardian_scaling();
    RegisterSpellScript(aura_ascension_guardian_lifecycle);
}
