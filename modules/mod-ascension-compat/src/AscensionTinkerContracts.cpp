/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionContract.h"
#include "AscensionScalingBaseData.h"
#include "AscensionTinker.h"
#include "AscensionTinkerData.h"
#include "DBCStores.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include <algorithm>
#include <cmath>
namespace AscensionTinker
{
uint32 SummonVulnerability(Player* player, Unit* attacker, Unit* target)
{
    if (!player || !target || !attacker || !attacker->IsCreature() || !Owned(player,attacker))
        return 0;
    uint32 percent = 0;
    for (auto const& pair : target->GetAppliedAuras())
        if (Aura* aura = pair.second->GetBase(); aura->GetId() == 805657 &&
            aura->GetScriptValue(805657) == player->GetGUID().GetRawValue())
            percent = std::max(percent,uint32(std::max(0,Amount(805657,1))));
    return percent;
}
namespace
{
// The layout a rewrite below assumes, read in Spell.dbc on 2026-10-23. A client import that moves it
// is journalled once instead of being rewritten blind (T-31), and the rewrite is then skipped.
bool ExpectEffect(SpellInfo const* info, uint8 slot, uint32 effect, uint32 aura)
{
    SpellEffectInfo const& current = info->Effects[slot];
    if (current.Effect == effect && current.ApplyAuraName == aura)
        return true;
    if (AscensionContract::Detail::FirstFailure("Tinker",
            AscensionContract::Detail::Key("Tinker", info->Id, slot, "layout")))
        LOG_ERROR("module.ascension_compat",
            "Ascension contract Tinker: spell {} slot {} is effect {} aura {}, expected effect {} aura {}; "
            "its rewrite is not applied.", info->Id, slot, uint32(current.Effect), uint32(current.ApplyAuraName),
            effect, aura);
    return false;
}
// Family 18 records that only the Tinker uses. MatchesFamily(34) used to reject them, so their
// branches below never ran (T-30).
bool TinkerForeign(uint32 id)
{
    return id == 807499 || id == 807500 || id == 807635;
}
}
void ApplyContracts(SpellInfo* info)
{
    if (!AscensionContract::MatchesFamily(info, 34, "Tinker") && !(info && TinkerForeign(info->Id)))
        return;
    uint32 id = info->Id;
    // Mechsuit was a mount (803451 effect 0, aura 78): the client dismounted at the first attack, which
    // removed 803451 and ejected the Tinker, and the server refused auto attack and every non-Tinker spell
    // while mounted (P-314, T-01/T-02/T-27). A transform into the same creature keeps the look without the
    // mount. HandleAuraTransform reads MiscValue as a creature_template entry: 229921 "Build:Mechsuit",
    // display 916645 (GnomeSpiderTank02).
    if (id == 803451)
    {
        if (ExpectEffect(info, 0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOUNTED) && info->Effects[0].MiscValue == 229921)
        {
            info->Effects[0].ApplyAuraName = SPELL_AURA_TRANSFORM;
            // Display 916645 has no ExtendedDisplayInfo, so Unit::IsInDisallowedMountForm held the transformed
            // Tinker: no taxi, every mount refused. This attribute is the core's own exemption there; the
            // mount then removes 801384 through its MOUNT interrupt flag, which ejects the Tinker (T-27).
            info->Attributes |= SPELL_ATTR0_ALLOW_WHILE_MOUNTED;
        }
        // Form 49 "Mechsuit: Shredder" has bonusActionBar 1 and no model (SpellShapeshiftForm.dbc): the client
        // swapped action bar 1 to the empty slots 72-83, hiding Gatling Gun. No Tinker spell requires form 49.
        if (ExpectEffect(info, 2, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_SHAPESHIFT) && info->Effects[2].MiscValue == 49)
        {
            info->Effects[2].Effect = 0;
            info->Effects[2].ApplyAuraName = SPELL_AURA_NONE;
        }
    }
    // Same value, now outside a mount: aura 32 only counts while mounted (Unit::UpdateSpeed).
    if (id == 803329 && ExpectEffect(info, 0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED))
        info->Effects[0].ApplyAuraName = SPELL_AURA_MOD_INCREASE_SPEED;
    // Overcharged is a beacon-only one-use guard. The native exclusion field also applies it on hit.
    if (info->ExcludeTargetAuraSpell == 560711 &&
        (id == 801707 || (id >= 502573 && id <= 502581) || id == 574152 || id == 529288))
        info->ExcludeTargetAuraSpell = 0;
    if (id == 707495)
    {
        // The former duplicate pet Synergy payload is now supplied by 707278.
        // This hidden, normally saved dummy retains the selected Module across login.
        info->Effects[0].Effect = SPELL_EFFECT_APPLY_AURA;
        info->Effects[0].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        info->Effects[0].TargetB = SpellImplicitTargetInfo();
        info->Effects[0].BasePoints = 0;
        info->Effects[0].DieSides = 0;
        info->Effects[1].Effect = info->Effects[2].Effect = 0;
        info->SpellFamilyFlags = flag96();
        info->StackAmount = 1;
        AscensionContract::SetDuration(info, 21, "Tinker");
        info->Attributes |= SPELL_ATTR0_DO_NOT_DISPLAY;
    }
    auto dummy = [info](uint8 slot)
    {
        // Journals once, without changing the write, when the slot cannot carry an
        // aura: the ApplyAuraName below is then inert (P-049).
        AscensionContract::ExpectAuraSlot(info, slot, "Tinker");
        info->Effects[slot].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[slot].TriggerSpell = 0;
        if (info->Effects[slot].Effect == 190)
            info->Effects[slot].Effect = SPELL_EFFECT_APPLY_AURA;
    };
    for (auto list : {std::pair(TinkerEvents,std::size(TinkerEvents)),
                     std::pair(TinkerDrivers,std::size(TinkerDrivers)),
                     std::pair(TinkerFinite,std::size(TinkerFinite))})
        for (size_t n = 0; n < list.second; ++n)
            if (list.first[n] == id)
            {
                for (uint8 slot = 0; slot < MAX_SPELL_EFFECTS; ++slot)
                    if (info->Effects[slot].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL ||
                        info->Effects[slot].ApplyAuraName == 354)
                        dummy(slot);
                info->ProcFlags = info->ProcCharges = 0;
            }
    for (uint32 finite : TinkerFinite)
        if (id == finite)
        {
            info->ProcCharges = id == 707261 ? 2 : 1;
            info->StackAmount = 1;
        }
    if (id == 707261)
        info->Effects[1].SpellClassMask = flag96(16,0,0);
    if (id == 707272)
        info->Effects[0].Effect = SPELL_EFFECT_DUMMY;
    if (id == 504594)
    {
        info->SpellFamilyFlags |= flag96(0,536870912,0);
        info->MaxCharges = 2;
        info->ChargeRecoveryTime = 8000;
        info->ChargeRecoveryKey = 504527;
        info->ChargeCategoryId = 323;
        if (auto* base = sSpellMgr->GetSpellInfo(504527))
        {
            info->ManaCost = base->ManaCost;
            info->ManaCostPercentage = base->ManaCostPercentage;
        }
    }
    if (Named(info,500232) || id == 504667 || id == 500612)
    {
        info->Effects[1].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        info->Effects[1].TriggerSpell = 0;
        if (id == 504667 || id == 500612)
        {
            info->Effects[1].BasePoints = info->Effects[0].BasePoints;
            info->Effects[1].DieSides = info->Effects[0].DieSides;
            info->Effects[1].RealPointsPerLevel = info->Effects[0].RealPointsPerLevel;
            info->Effects[0].Effect = SPELL_EFFECT_DUMMY;
        }
    }
    if (id == 806780)
    {
        dummy(0);
        info->ProcFlags = 0;
    }
    if (Named(info,801709))
        info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
    if (id == 573054)
    {
        // The lifecycle tick selects enemies around the recipient; damage belongs
        // to the Tinker, so its coefficient and threat redirection use that caster.
        info->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        info->Effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
    }
    if (id == 524935)
    {
        info->Effects[0].Effect = SPELL_EFFECT_APPLY_AURA;
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
        AscensionContract::SetDuration(info, 21, "Tinker");
    }
    if (Named(info,504519))
        info->Effects[1].Effect = 0; // The owned turret distributes the summon damage field.
    if (id == 560757)
        for (auto& effect : info->Effects)
            if (effect.Effect)
            {
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    if (id == 801384)
    {
        info->Effects[2].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        info->Effects[2].TriggerSpell = 0;
    }
    if (id == 807635)
    {
        info->Effects[0].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        info->Effects[0].TriggerSpell = 0;
        dummy(1);
    }
    if (id == 653232)
        info->Effects[0].MiscValue = 21003; // Existing native creature-type critical chance consumer.
    if (id == 680999)
    {
        info->Effects[0].MiscValue = SPELL_SCHOOL_MASK_NORMAL;
        AscensionContract::SetDuration(info, 21, "Tinker");
    }
    if (id == 801744)
        for (auto& effect : info->Effects)
            if (effect.Effect)
                effect.Effect = SPELL_EFFECT_DUMMY; // The delayed location snapshot owns both phases.
    if (id == 707244 || id == 707256)
        for (uint8 slot = 0; slot < MAX_SPELL_EFFECTS; ++slot)
            if (info->Effects[slot].Effect)
                dummy(slot);
    if (Named(info,500232))
        dummy(2); // The old blanket caster critical aura is not Rocket Launcher's conditional rule.
    if (Named(info,805351))
        info->Effects[2].Effect = 0;
    if (id == 807966 || id == 704475)
        info->Effects[0].SpellClassMask |= flag96(1073741824,1073741824,2684354560u);
    if (id == 805657)
        dummy(1);
    if (id == 707290)
    {
        dummy(1);
        dummy(2);
    }
    if (id == 805315)
    {
        dummy(0);
        info->Effects[1].ApplyAuraName = SPELL_AURA_MOD_HEALING_PCT;
        info->Effects[1].BasePoints = -3;
        info->Effects[1].DieSides = 0;
        info->Effects[1].TriggerSpell = 0;
        info->Effects[1].MiscValue = SPELL_SCHOOL_MASK_ALL;
        dummy(2);
    }
    if (id == 707278 || id == 578323 || id == 805519 || id == 706698)
        for (auto& effect : info->Effects)
        {
            if (!effect.Effect)
                continue;
            if (effect.Effect == 190)
                effect.Effect = SPELL_EFFECT_APPLY_AURA;
            effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
            effect.TargetB = SpellImplicitTargetInfo();
        }
    if (id == 805519 || id == 706698)
    {
        dummy(0);
        info->Effects[0].Amplitude = 0;
        info->ProcFlags = 0;
    }
    if (id == 808008)
    {
        info->Effects[0].Effect = SPELL_EFFECT_APPLY_AURA;
        info->Effects[0].ApplyAuraName = SPELL_AURA_MOD_HEALING_DONE_PERCENT;
        info->Effects[1].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_DONE;
        for (auto& effect : info->Effects)
            effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
    }
    if (id == 706692)
    {
        info->Effects[1].Effect = 0; // Retain native turret appearance until an exact upgraded display is recovered.
        // TargetAuraSpell 750001 (Sentry Turret Hidden Identifier) is applied by nothing, so CheckTarget
        // refused every Upgrade! cast on the turret. Abilities.cpp already selects turrets by entry.
        info->TargetAuraSpell = 0;
    }
    if (id == 524979)
        info->TargetAuraSpell = 0; // 806626 is applied by nothing: the learned passive never reached the player.
    if (id == 675204)
    {
        // Anti-Cloaking Device is Flare's layout (dispel immunity 5 and 6 from a persistent area) without
        // Flare's purge bit: already stealthed enemies stayed hidden, only a new stealth was prevented.
        // Known limits, same as Flare: custom stealths without Dispel 5 (803833, 806179, 573038, 705427...)
        // are untouched; pure invisibility (Dispel 6) is purged only on a target the area already detects.
        bool layout = true;
        for (uint8 slot : {uint8(0),uint8(1)})
            layout = layout && info->Effects[slot].Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA &&
                info->Effects[slot].ApplyAuraName == SPELL_AURA_DISPEL_IMMUNITY &&
                info->Effects[slot].MiscValue == (slot ? DISPEL_INVISIBILITY : DISPEL_STEALTH);
        if (layout)
            info->AttributesEx |= SPELL_ATTR1_IMMUNITY_PURGES_EFFECT;
        else if (AscensionContract::Detail::FirstFailure("Tinker",
                     AscensionContract::Detail::Key("Tinker", id, 0, "anti-cloaking")))
            LOG_ERROR("module.ascension_compat",
                "Ascension contract Tinker: spell 675204 is no longer two persistent dispel immunities 5 and 6; "
                "the stealth purge is not applied.");
    }
    if (id == 705792)
    {
        info->Effects[0].Effect = 0; // Device crit/hit inheritance is applied explicitly with owner scope.
        info->Effects[1].ApplyAuraName = SPELL_AURA_MOD_CRIT_PCT;
        info->Effects[2].ApplyAuraName = SPELL_AURA_ASCENSION_MOD_HIT_CHANCE_ALL_PCT;
    }
    if (id == 560742)
    {
        info->Effects[1].ApplyAuraName = SPELL_AURA_MOD_HIT_CHANCE;
        info->Effects[2].ApplyAuraName = SPELL_AURA_MOD_SPELL_HIT_CHANCE;
        for (uint8 slot : {uint8(1),uint8(2)})
        {
            info->Effects[slot].BasePoints = 100;
            info->Effects[slot].DieSides = 0;
        }
    }
    if (id == 503535 || id == 504822 || id == 560786 || id == 561267 || id == 560709 || id == 560710 ||
        id == 706829 || id == 560746 || id == 681513 || id == 705847 || id == 706255 ||
        id == 560711 || id == 705787 || id == 573269 || id == 578323 || id == 560774 ||
        id == 707698 || id == 560742 || id == 705792)
        for (auto& effect : info->Effects)
            if (effect.Effect)
            {
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    // Shield Beacon pulses its armor helper at each ally. Persistent area auras need a dynamic object the beacon
    // never creates, so the helper applied nothing; apply the aura to the ally directly.
    if (id == 801256 || (id >= 803804 && id <= 803808))
        for (auto& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            {
                effect.Effect = SPELL_EFFECT_APPLY_AURA;
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    if (id == 560709 || id == 560710)
        info->Effects[1].Effect = 0; // The exact beacon, not the player, stores its one-use guard.
    if (id == 561267)
        info->Effects[1].Effect = 0; // The explicit pet mana restoration is independent of heal modifiers.
    if (id == 524903)
    {
        info->Effects[1].Effect = 0; // Consume only the triggering owner's mark.
        // DeviceEvent removes the mark before casting: TargetAuraSpell 524904 then refused every cast (T-10).
        info->TargetAuraSpell = 0;
    }
    // Turret Upgrade Z-15: "increase critical strike chance of the turret by $573269s1%". The data's aura
    // 197 raised the crit chance of the turret's attackers instead (T-14). 706700 effect 1 also triggered
    // 573269 on the enemy; the turret stack is applied by DeviceEvent alone.
    if (id == 573269 && ExpectEffect(info, 0, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE))
        info->Effects[0].ApplyAuraName = SPELL_AURA_MOD_CRIT_PCT;
    if (id == 706700 && ExpectEffect(info, 1, SPELL_EFFECT_TRIGGER_SPELL, 0) && info->Effects[1].TriggerSpell == 573269)
        info->Effects[1].Effect = 0;
    // Rocket Bombardment: effect 1 (aura 271) names class mask (16777216,0,0), which no Tinker spell carried,
    // so "increased by $s2% for each subsequent Rocket Cluster" never applied (T-24). No other family 34
    // record or mask uses that bit (Spell.dbc, spell_proc).
    if (id == 706696 && ExpectEffect(info, 1, SPELL_EFFECT_APPLY_AURA, SPELL_AURA_MOD_DAMAGE_FROM_CASTER) &&
        info->Effects[1].SpellClassMask == flag96(16777216,0,0) && !info->SpellFamilyFlags)
        info->SpellFamilyFlags = flag96(16777216,0,0);
    if (id == 578335)
        info->Attributes |= SPELL_ATTR0_NO_IMMUNITIES;
    if (id == 801982 || id == 802477 || id == 500601 || id == 806074 || id == 706648 || id == 802336)
    {
        info->Speed = 0;
        info->Attributes |= SPELL_ATTR0_ALLOW_CAST_WHILE_DEAD;
    }
    // Its party target also took the Tinker's turret, beacons and pets; Explode (50045) throws 804306 at
    // the owner and his party's players instead (T-08).
    if (id == 802336 && ExpectEffect(info, 2, SPELL_EFFECT_TRIGGER_SPELL, 0) && info->Effects[2].TriggerSpell == 804306)
        info->Effects[2].Effect = 0;
    // The device AI selects the complete explosion set once. Each helper is one selected hit.
    // 802336 (Blast Mine) keeps its own area targeting: one cast from the mine (T-06). Split per enemy it
    // drew one flare missile per target, and its effect 2 threw the allied knockback 804306 at the enemies
    // instead of the party.
    if (id == 801982 || id == 802477 || id == 500601 || id == 806074 || id == 706648 ||
        id == 712680 || id == 561269)
        for (auto& effect : info->Effects)
            if (effect.Effect)
            {
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
                effect.TargetB = SpellImplicitTargetInfo();
            }
    for (uint32 copy : TinkerCopies)
        if (id == copy)
        {
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AscensionInheritsResolvedAmount = true;
        }
    info->_InitializeExplicitTargetMask();
}
} // namespace AscensionTinker
namespace
{
using namespace AscensionTinker;
// True when ascension_scaling_base (AscensionScalingBase.cpp) already multiplies this (spell, effect)
// by $scalingbp, because AscensionScalingBaseData.h lists it.
bool TinkerScaledByScalingBase(uint32 id, uint8 index)
{
    auto const entry = std::lower_bound(AscensionCompatData::ScalingBaseSpells.begin(),
        AscensionCompatData::ScalingBaseSpells.end(), id,
        [](AscensionCompatData::ScalingBaseEntry const& row, uint32 spell) { return row.Spell < spell; });
    return entry != AscensionCompatData::ScalingBaseSpells.end() && entry->Spell == id &&
        index < 8 && (entry->EffectMask & (1u << index));
}
class tinker_scaling : public UnitScript
{
public:
    tinker_scaling() : UnitScript("tinker_scaling",true,
        {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE,UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,UNITHOOK_MODIFY_MELEE_DAMAGE}) { }
    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        Player* player = Owner(caster);
        if (!player || !info || info->SpellFamilyName != 34 || Derived(info) || !std::isfinite(value))
            return;
        for (auto const& row : TinkerCoefficients)
            if (row.spell == info->Id && row.effect == index)
            {
                // $scalingbp once. 500601, 560709, 706689, 706700, 706829 and 802176 e0 are also in
                // AscensionScalingBaseData.h, so ascension_scaling_base already applied the curve and
                // applying it here squared it (x2.9 too low at level 30). 520375 is not in that table
                // and keeps its curve here. The SP/AP terms below stay unscaled only because
                // ascension_scaling_base is registered before this script (MP_loader.cpp).
                if (row.curve && !TinkerScaledByScalingBase(row.spell, row.effect))
                {
                    double level = player->GetLevel();
                    value *= float(.0267291844060354 + .0048541098014737 * level + .0001859597762293 * level * level);
                }
                auto school = row.school ? SpellSchoolMask(row.school) : info->GetSchoolMask();
                value += row.sp * std::max(0,player->SpellBaseDamageBonusDone(school)) +
                    row.ap * player->GetTotalAttackPowerValue(BASE_ATTACK) +
                    row.rap * player->GetTotalAttackPowerValue(RANGED_ATTACK) +
                    row.healing * std::max(0,player->SpellBaseHealingBonusDone(info->GetSchoolMask()));
            }
        value = std::clamp(value,float(INT32_MIN / 2),float(INT32_MAX / 2));
    }
    void ModifyMeleeDamage(Unit* target, Unit* caster, uint32& damage) override
    {
        AddPct(damage,SummonVulnerability(Owner(caster),caster,target));
    }
    void ModifySpellDamageTaken(Unit* target, Unit* caster, int32& damage, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (!player || !target || !info || Derived(info) || info->AscensionInheritsResolvedAmount)
            return;
        AddPct(damage,SummonVulnerability(player,caster,target));
        if (Aura* oil = target->GetAura(707290,player->GetGUID()); oil &&
            (caster == player || Owned(player,caster)))
        {
            if (info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE)
                AddPct(damage,Amount(707290,1));
            if ((info->SpellFamilyFlags & flag96(65536,0,0)) || info->Id == 500601)
                AddPct(damage,Amount(707290,2));
        }
        if (caster == player && (info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE))
            if (Aura* aura = target->GetAura(Napalm,player->GetGUID()))
                AddPct(damage,Amount(Napalm) * aura->GetStackAmount());
        if (player->HasAura(707256) && (Shot(info) || info->Id == 500601))
            if (Aura* aura = target->GetAura(Napalm,player->GetGUID()))
                AddPct(damage,Amount(707256) * aura->GetStackAmount());
    }
};
}
void AddSC_AscensionTinkerContracts()
{
    new tinker_scaling();
}
