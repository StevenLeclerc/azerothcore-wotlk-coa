/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionContract.h"
#include "AscensionStarcaller.h"
#include "AscensionXoroth.h"
#include "AscensionXorothData.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include <algorithm>
namespace AscensionXoroth
{
namespace
{
enum FleshHook : uint32
{
    SPELL_FLESH_HOOK_PULL = 800605,
    SPELL_RANGE_THIRTY_YARDS = 4
};
}

void ApplyContracts(SpellInfo* info)
{
    if (!AscensionContract::MatchesFamily(info, 23, "Xoroth"))
        return;
    uint32 id = info->Id;
    if (id == SPELL_WARPATH_PROTECTION && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_MOD_MINIMUM_SPEED)
        AscensionContract::SetDuration(info, 27, "Xoroth"); // Three seconds after Unleash Pestilence.
    if (id == SPELL_FLESH_HOOK_PULL)
    {
        // The parent has already passed its range and hit checks before scheduling this helper.
        // Do not roll melee avoidance again or reject an enemy that approaches during the delay.
        // Native spell/mechanic immunities still apply to this non-damaging pull.
        info->DmgClass = SPELL_DAMAGE_CLASS_NONE;
        // SpellInfo::GetMaxRange returns 0 for a null RangeEntry (SpellInfo.cpp:2898-2901), which
        // would make the pull castable at point blank only, without a word. Keep the client row.
        if (SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(SPELL_RANGE_THIRTY_YARDS))
            info->RangeEntry = range;
        else
            LOG_ERROR("module.ascension_compat",
                "Ascension contract Xoroth: SpellRange row {} is missing, spell {} keeps its client range.",
                uint32(SPELL_RANGE_THIRTY_YARDS), id);
    }
    if (id == 520440 || id == 520441)
        for (auto& effect : info->Effects)
            effect.Effect = 0; // Legacy delayed removal must not erase Demonfire generated after reservation.
    auto dummy = [info](uint8 i) {
        // Journals once, without changing the write, when the slot cannot carry an
        // aura: the ApplyAuraName below is then inert (P-049).
        AscensionContract::ExpectAuraSlot(info, i, "Xoroth");
        info->Effects[i].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[i].TriggerSpell = 0;
    };
    for (auto const& list :
         {std::pair(XorothEvents, std::size(XorothEvents)), std::pair(XorothCastDrivers, std::size(XorothCastDrivers))})
        for (size_t n = 0; n < list.second; ++n)
            if (list.first[n] == id)
            {
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].ApplyAuraName == 42 || info->Effects[i].ApplyAuraName == 354)
                        dummy(i);
                info->ProcFlags = 0;
                info->ProcCharges = 0;
            }
    for (uint32 sid : XorothCopies)
        if (sid == id)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
            info->ProcFlags = 0;
            info->Effects[0].TargetA =
                SpellImplicitTargetInfo(id == 681206 ? TARGET_UNIT_CASTER : TARGET_UNIT_TARGET_ENEMY);
            info->Effects[0].TargetB = SpellImplicitTargetInfo();
            if (id == 680204)
                info->Effects[0].ValueMultiplier = 1;
        }
    for (uint32 sid : {500906, 704195, 704217, 707630})
        if (id == sid)
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].Effect)
                    dummy(i);
    for (uint32 sid : {680197, 680203, 681184, 520021, 802617, 802618, 524913, 560630})
        if (id == sid)
        {
            info->ProcFlags = 0;
            info->ProcCharges = 0;
        }
    if (id == 560630)
        info->StackAmount = 0; // Demon King: "+$560630s1% parry until you parry", one aura refreshed, not 10 stacks.
    if (id == 707632 || id == 707914)
        dummy(0); // SPELLMOD_CHARGES would give Burning Blade core charges (CalcMaxCharges) and end it after 4 attacks.
    if (id == 704954)
        dummy(1); // Boundless Fury: ALL_EFFECTS already carries the +30 %; SPELLMOD_DAMAGE on top gave x1.69.
    if (id == 704964)
        dummy(1); // Bloodrider: its native +50 % duration hits Unleash Death; Losing Momentum is done by hand.
    if (id == 681206)
        AscensionContract::SetDuration(info, 27, "Xoroth"); // Gorged: 3 ticks of damage/6, 50 % over 3 sec.
    if (id == 520542)
    {
        // Conqueror's Will: the critically struck target only, not every enemy around the knight.
        info->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        info->Effects[EFFECT_0].TargetB = SpellImplicitTargetInfo();
    }
    if (id == 680197)
    {
        info->Effects[0].ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
        info->Effects[0].BasePoints = -101;
        info->Effects[0].SpellClassMask = flag96(0, 768, 0);
        info->Effects[1].Effect = 0;
    }
    if (id == 802618)
        info->Effects[0].SpellClassMask = flag96(0, 768, 0);
    if (id == 802617)
        info->Effects[0].SpellClassMask = flag96(0, 1, 0);
    if (id == 524913)
    {
        dummy(0);
        dummy(1);
    }
    if (id == 92100)
        info->Effects[1].MiscValue = 127;
    if (id == 804284)
        dummy(2); // double only the weapon portion in the effect callback
    if (id == 573035)
        dummy(0);
    if (id == 573075)
    {
        AscensionContract::SetDuration(info, 21, "Xoroth");
        info->Effects[1].Effect = 0;
    }
    if (id == 302546 || id == 302573 || id == 302574 || id == 302592)
    {
        auto& e = info->Effects[0];
        e.Effect = SPELL_EFFECT_APPLY_AURA;
        e.ApplyAuraName = id == 302546   ? SPELL_AURA_MOD_BLOCK_PERCENT
                          : id == 302573 ? SPELL_AURA_MOD_PERCENT_STAT
                          : id == 302574 ? SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN
                                         : SPELL_AURA_DUMMY;
        e.MiscValue = id == 302573 ? STAT_STRENGTH : id == 302574 ? 127 : 0;
        e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        e.TargetB = SpellImplicitTargetInfo();
        info->Effects[1].Effect = 0;
        info->Effects[2].Effect = 0;
    }
    if (id == 706935)
        dummy(0);
    if (id == 800999)
        dummy(2);
    if (id == 805680)
        info->Effects[1].Effect = 0;
    if (id == 805679)
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    if (id == 302555)
    {
        info->Effects[0].Effect = SPELL_EFFECT_APPLY_AURA;
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
    }
    if (id == 804879)
    {
        info->Effects[0].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_DONE;
        info->Effects[0].MiscValue = 127;
    }
    if (id == 805678 || id == 807898)
    {
        info->Effects[1].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN;
        info->Effects[1].BasePoints = -std::abs(info->Effects[1].BasePoints + 1) - 1;
    }
    if (id == 803671 || Named(info, 707693) || id == 800443 || id == 562029 || id == 520662)
        for (auto& e : info->Effects)
            if (e.Effect)
            {
                e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
                e.TargetB = SpellImplicitTargetInfo();
            }
    if (id == 807671 || id == 707515 || id == 704988 || id == 560828 || id == 300375)
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect)
                dummy(i);
    if (id == 500003)
        info->Effects[0].MiscValue = ASCENSION_CREATURE_GLOBAL_CRIT, info->Effects[0].MiscValueB = 4;
    if (id == 805697 || id == 807899)
        info->Effects[0].MiscValue = ASCENSION_STATE_GLOBAL_CRIT_DAMAGE;
    if (id == 300374 || id == 301357 || id == 807669)
        dummy(0);
    if (id == 704987)
        dummy(1);
    if (id == 704185)
    {
        dummy(0);
        dummy(1);
    }
    if (id == 804787)
    {
        info->StackAmount = 5;
        dummy(1);
        dummy(2);
    }
    if (id == 805677)
        info->CasterAuraSpell = 0; // spell_ascension_xoroth_sacrificial_circle checks the living Hellfire Imps
    if (id == 805916)
        // Hellfire Imp owner area aura. Only the marker survives: Refresh puts it on the master while
        // imps live, because the client's Spell.dbc still makes Sacrificial Circle require it
        // (CasterAuraSpell 805916). Its cost and effect modifiers are already applied per imp by 805965
        // and 573066; BasePoints stay, Amount(805916, 2) still reads them.
        for (auto& e : info->Effects)
        {
            e.Effect = SPELL_EFFECT_APPLY_AURA;
            e.ApplyAuraName = SPELL_AURA_DUMMY;
            e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            e.TargetB = SpellImplicitTargetInfo();
            e.SpellClassMask = flag96();
        }
    if (id == 805965)
    {
        auto& e = info->Effects[0];
        e.Effect = SPELL_EFFECT_APPLY_AURA;
        e.ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
        e.MiscValue = SPELLMOD_COST;
        e.SpellClassMask = flag96(32768, 0, 0);
        e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        AscensionContract::SetDuration(info, 21, "Xoroth");
    }
    if (id == 800702)
        dummy(0);
    if (id == 801064)
        AscensionContract::SetDuration(info, 1, "Xoroth");
    if (id == 802342)
        info->Effects[1].Effect = 0;
    if (id == 803334)
        dummy(2);
    if (id == 801053 || id == 802344 || id == 804786)
    {
        auto& e = info->Effects[0];
        e.Effect = SPELL_EFFECT_APPLY_AURA;
        e.ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        e.Amplitude = 1000;
        e.TriggerSpell = 0;
        e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        e.TargetB = SpellImplicitTargetInfo();
    }
    if (id == 804786)
    {
        // The original enemy area aura was classified before conversion to the self-only driver above.
        info->AttributesCu &= ~SPELL_ATTR0_CU_NEGATIVE_EFF0;
    }
    if (id == 801055 || id == 560817 || id == 802855 || id == 802856 || id == 802857 || id == 801052)
        for (auto& e : info->Effects)
            if (e.Effect)
            {
                e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
                e.TargetB = SpellImplicitTargetInfo();
            }
    if (id == 802855)
        info->Effects[2].Effect = 0;
    if (id == 802602)
        info->Effects[2].Effect = 0;
    if (id == 520857)
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CONE_ENEMY_104);
    if (id == 520292)
    {
        // The behind helper carries the authored cost/cooldown; charge them on the player cast.
        info->ManaCost = 200;
        info->RecoveryTime = 15000;
        info->CategoryRecoveryTime = 0;
    }
    if (id == 804886 || id == 804011)
        for (auto& e : info->Effects)
            if (e.Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_SRC_AREA_ENEMY);
                e.TargetB = SpellImplicitTargetInfo();
                // A missing SpellRadius row is read by SpellEffectInfo::CalcRadius as a radius of 0
                // (SpellInfo.cpp:608-611): the area effect reaches nobody, silently. The client row
                // is kept and the refusal journalled instead of writing a null pointer.
                if (SpellRadiusEntry const* radius = sSpellRadiusStore.LookupEntry(13))
                    e.RadiusEntry = radius;
                else
                    LOG_ERROR("module.ascension_compat",
                        "Ascension contract Xoroth: SpellRadius row 13 is missing, spell {} keeps its client radius.",
                        id);
            }
    if (id == 806219)
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
    if (id == 801042)
    {
        info->AttributesEx2 |= SPELL_ATTR2_ALLOW_DEAD_TARGET;
        info->Effects[0].Effect = SPELL_EFFECT_DUMMY;
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        for (uint8 i = 1; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect)
                info->Effects[i].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    }
    if (id == 804703)
    {
        info->Effects[2].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        info->Effects[2].TriggerSpell = 0;
    }
    if (id == 804775)
    {
        info->Effects[0].ApplyAuraName = SPELL_AURA_MOUNTED;
        info->Effects[0].MiscValue = 914505;
    }
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (info->Effects[i].ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && info->Effects[i].MiscValue == 32)
            dummy(i);
    for (uint32 sid : {801071, 802347, 804801})
        if (id == sid)
            for (auto& effect : info->Effects)
                if (effect.Effect)
                {
                    effect.Effect = SPELL_EFFECT_APPLY_AURA;
                    effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
                    effect.TargetB = SpellImplicitTargetInfo();
                }
    if (id == 804801)
        dummy(2); // MOD_DAMAGE_FROM_CASTER with an empty mask took every family 23 spell; Factor keeps it to Fire.
    if (id == 680204)
        info->Effects[1].Effect = 0;
    if (id == 712294)
    {
        info->Effects[1].Effect = 0;
        info->Effects[2].Effect = 0;
    }
    info->_InitializeExplicitTargetMask();
}
} // namespace AscensionXoroth
namespace
{
using namespace AscensionXoroth;
class xoroth_scaling : public UnitScript
{
  public:
    xoroth_scaling()
        : UnitScript("xoroth_scaling", true,
                     {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_PERIODIC_DAMAGE_RESULT,
                      UNITHOOK_ON_DAMAGE, UNITHOOK_MODIFY_MELEE_DAMAGE})
    {
    }
    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        Player* player = Owner(caster);
        if (!player && caster)
            player = Owner(caster->GetOwner());
        if (!player || !info)
            return;
        if (info->Id == 630930 && index == EFFECT_0 && caster->GetEntry() == 510100)
        {
            // Burning Slap: copied SpellDescriptionVariables row 182 ($scalingbp), then AP/SP bonuses.
            double level = caster->GetLevel();
            value *= float(0.0267291844060354 + 0.0048541098014737 * level + 0.0001859597762293 * level * level);
        }
        if (info->Id == 520857 && index == EFFECT_0)
        {
            // Behind cone of Hellfire Bellows: same $scalingbp as 520292 e0 (AscensionScalingBaseData.h),
            // missed by the generator because the 520857 tooltip is the old '$m1+0'.
            double level = player->GetLevel();
            value *= float(0.0267291844060354 + 0.0048541098014737 * level + 0.0001859597762293 * level * level);
        }
        for (auto const& row : XorothCoefficients)
            if (row.spell == info->Id && row.effect == index)
            {
                float sp = float(std::max(0, player->SpellBaseDamageBonusDone(SpellSchoolMask(row.school))));
                float bonus = row.sp * sp + row.ap * player->GetTotalAttackPowerValue(BASE_ATTACK) +
                              row.strength * player->GetStat(STAT_STRENGTH) +
                              row.stamina * player->GetStat(STAT_STAMINA);
                if (Named(info, 805671) && player->HasAura(704988))
                    bonus *= 1.25f;
                value += bonus;
            }
        if (Named(info, 801016) && index == 1 && player->HasAura(92100))
            value *= 2;
        if (Named(info, 804353) && index == 1) // every Shieldgore rank: "scaling with your block value"
            value += player->GetShieldBlockValue();
        if (info->Id == 801064 && index == 0)
            value += player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_DEFENSE_SKILL) * .20f;
        if (info->Id == 805680 && index == 0 && State(player).curseShield)
            value += player->GetStat(STAT_STAMINA) * 2;
        if (info->Id == 805680 && index == 0 && player->HasAura(560828) && player->GetHealthPct() < 35)
            value *= 2;
        if (info->Id == 573075 && index == 0)
            value = player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_BLOCK) * .5f;
        if (info->Id == 573066 && index == 0)
            value += State(player).imps.size() * Amount(805916, 2);
        if (info->Id == 804788 && index == 0) // Path of Hellfire e2 (+30 % per stack, dummied): read, not 0.2
            value *= 1 + Count(player, 804787) * Amount(804787, EFFECT_2) / 100.0f;
        // 800999 e1 (Demonskin armor) is left to the native SPELLMOD_EFFECT2 of 302581: forcing it here too
        // counted the talent twice (+2 % per stack).
        if (info->Id == 801055 || info->Id == 560817 || info->Id == 802855)
            value *= State(player).unleash;
        value = std::clamp(value, float(INT32_MIN / 2), float(INT32_MAX / 2));
    }
    float Factor(Unit* target, Unit* caster, SpellInfo const* info)
    {
        Player* player = Owner(caster);
        bool pet = false;
        if (!player && caster)
            player = Owner(caster->GetOwner()), pet = true;
        float defense = 1;
        if (target && (target->GetEntry() == 50301 || target->GetEntry() == 50375))
            if (Player* master = Owner(target->GetOwner()))
                if (Aura* grit = Grit(master)) // owned only: its effects 190 never reach TempSummon imps
                    if (!target->HasAura(grit->GetId(), master->GetGUID()))
                        defense *= 1 - std::abs(Amount(grit->GetId(), 1)) / 100.0f; // e1 = +20 (bp 19, aura 79); abs is only a guard
        if (!player || !target || !info || Derived(info))
            return defense;
        float factor = defense;
        if (pet && caster->GetEntry() == 50301 && player->HasAura(804879) &&
            !caster->HasAura(804879, player->GetGUID()))
            factor *= 2.5f;
        if ((info->SchoolMask & SPELL_SCHOOL_MASK_FIRE) && target->GetHealthPct() > 80)
        {
            if (!pet)
            {
                // Invasion: rank 2 301357 is in no chain of 300374 (P-046). Both keep a DUMMY application.
                Aura* a = player->GetAura(301357);
                if (!a)
                    a = player->GetAura(300374);
                if (a)
                    factor *= 1 + Amount(a->GetId()) / 100.0f;
            }
            else if (caster->GetEntry() == 50301 && Owned(player, 807669)) // effect 190 only: owned, never applied
                factor *= 1 + Amount(807669) / 100.0f;
        }
        if (!pet && Named(info, 805671))
        {
            if (player->HasAura(707515) && player->HasAura(805696))
                factor *= 1 + Amount(707515, 1) / 100.0f;
            if (player->HasAura(807671) && target->HasAura(804801, player->GetGUID()))
                factor *= 1 + (Amount(807671) + 10 * Count(player, 500906)) / 100.0f;
        }
        // "Burning" is no aura state anyone sets (30 is not in the enum): same rule as Starcaller.
        if (!pet && Sever(info) && player->HasAura(300375) && AscensionStarcaller::Burning(target))
            factor *= 1 + Amount(300375) / 100.0f;
        // Amount() reads the DBC value of Spiked Chains whether the player owns the talent or
        // not, so the talent itself has to be tested here. Today the only caster of 806219
        // (AscensionXorothAbilities.cpp) is already gated on HasAura(704987), which is exactly
        // what made the omission invisible: any other caster -- event, bot, SQL -- would hand
        // out the bonus talentless.
        if (info->Id == 806219 && player->HasAura(704987) && target->GetCreatureType() == CREATURE_TYPE_HUMANOID)
            factor *= 1 + Amount(704987, 1) / 100.0f;
        // Pestilence of Apocalypse: "+$s3% Fire Damage from you" (e2 dummied); player as caster keeps Harbinger.
        if (!pet && (info->SchoolMask & SPELL_SCHOOL_MASK_FIRE) && target->HasAura(804801, player->GetGUID()))
            factor *= 1 + Amount(804801, EFFECT_2, player) / 100.0f;
        return factor;
    }
    void ModifyMeleeDamage(Unit* target, Unit* caster, uint32& damage) override
    {
        damage = uint32(damage * Factor(target, caster, nullptr));
    }
    void ModifySpellDamageTaken(Unit* target, Unit* caster, int32& damage, SpellInfo const* info) override
    {
        damage = int32(damage * Factor(target, caster, info));
    }
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* caster, uint32& damage, SpellInfo const* info) override
    {
        damage = uint32(damage * Factor(target, caster, info));
    }
    void OnPeriodicDamageResult(Unit* target, Unit* caster, uint32 damage, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (player && target && player->HasAura(802619) && Named(info, 806965))
            if (Aura* aura = target->GetAura(info->Id, player->GetGUID()))
                aura->SetScriptValue(802619, aura->GetScriptValue(802619) + damage);
    }
    void OnDamage(Unit* caster, Unit* target, uint32& damage) override
    {
        if (!damage)
            return;
        for (Player* player : {Owner(caster), Owner(target)})
            if (player)
            {
                State(player).timers.RescheduleEvent(804703, 3s);
                if (player == target && player->HasAura(804703) && Count(player, 804787) && !player->HasAura(804785) &&
                    // Bloodrider: "reduces the chance ... by $s1%" ($s1 = -25), so a 75 % loss chance.
                    !player->HasAura(805746) && !roll_chance_i(player->HasAura(704964) ? -Amount(704964) : 0))
                    Cast(player, player, 805746);
            }
    }
};
} // namespace
void AddSC_AscensionXorothContracts()
{
    new xoroth_scaling();
}
