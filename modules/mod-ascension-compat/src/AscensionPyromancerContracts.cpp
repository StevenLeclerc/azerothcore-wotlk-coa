/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionContract.h"
#include "AscensionPyromancer.h"
#include "AscensionPyromancerData.h"
#include "DBCStores.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include <algorithm>
namespace AscensionPyromancer
{
void ApplyContracts(SpellInfo* info)
{
    if (!AscensionContract::MatchesFamily(info, 30, "Pyromancer"))
        return;
    uint32 id = info->Id;
    auto dummy = [info](uint8 slot)
    {
        // Journals once, without changing the write, when the slot cannot carry an
        // aura: the ApplyAuraName below is then inert (P-049).
        AscensionContract::ExpectAuraSlot(info, slot, "Pyromancer");
        info->Effects[slot].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[slot].TriggerSpell = 0;
    };
    auto mod = [info](uint8 slot, AuraType type, int32 value, uint32 operation, flag96 mask)
    {
        auto& e = info->Effects[slot];
        e.Effect = SPELL_EFFECT_APPLY_AURA;
        e.ApplyAuraName = type;
        e.BasePoints = value - 1;
        e.DieSides = 1;
        e.MiscValue = operation;
        e.SpellClassMask = mask;
        e.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        e.TargetB = SpellImplicitTargetInfo();
        e.TriggerSpell = 0;
    };
    for (auto const& list : {std::pair(PyromancerEvents, std::size(PyromancerEvents)),
                             std::pair(PyromancerDrivers, std::size(PyromancerDrivers))})
        for (size_t n = 0; n < list.second; ++n)
            if (list.first[n] == id)
            {
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL ||
                        info->Effects[i].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_DAMAGE ||
                        info->Effects[i].ApplyAuraName == 347 || info->Effects[i].ApplyAuraName == 354)
                        dummy(i);
                info->ProcFlags = info->ProcCharges = 0;
            }
    for (uint32 copy : PyromancerCopies)
        if (id == copy)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
            info->AscensionInheritsResolvedAmount = id != 520826;
            bool helpful = id == 704274 || id == 707892 || id == 707595 || id == 807403;
            info->Effects[0].TargetA =
                SpellImplicitTargetInfo(helpful ? TARGET_UNIT_TARGET_ALLY : TARGET_UNIT_TARGET_ENEMY);
            info->Effects[0].TargetB = SpellImplicitTargetInfo();
        }
    if (id == 503864)
    {
        info->Effects[0].Effect = SPELL_EFFECT_SCHOOL_DAMAGE;
        // Aspect() reads the echo's damage right after casting it, to chain the next one and to
        // feed Lifebringer's Blessing: the hit must land inside that cast, not on a later tick.
        info->Speed = 0.0f;
    }
    if (id == 520868)
        info->Effects[0].Effect = SPELL_EFFECT_DUMMY;
    if (id == 572806)
        info->Effects[1].Effect = info->Effects[2].Effect = 0;
    if (Named(info, 802174))
        info->Effects[1].Effect = info->Effects[2].Effect = 0;
    if (Named(info, 805500))
        dummy(1), dummy(2); // Persist next-Blaze critical snapshots in the existing zero-valued slots.
    if (Any(info, {800791, 706874}))
        dummy(1); // Persist Stoke's consumed extension budget through normal aura saves.
    if (id == 520927)
        dummy(1);
    if (id == 524707)
        info->ProcCharges = 3; // Saved native charges; only Finish consumes them, with no proc row.
    if (id == 680962 || id == 807403 || id == 520826)
        for (uint8 slot = 1; slot < MAX_SPELL_EFFECTS; ++slot)
        {
            auto& effect = info->Effects[slot];
            effect.Effect = SPELL_EFFECT_APPLY_AURA;
            dummy(slot);
            effect.BasePoints = -1;
            effect.DieSides = 1;
            effect.Amplitude = 0;
            effect.SpellClassMask = flag96();
            effect.TargetA = info->Effects[0].TargetA;
            effect.TargetB = SpellImplicitTargetInfo();
        }
    if (id == 680367 || id == 681366)
    {
        info->Effects[id == 680367 ? 0 : 1].ApplyAuraName = SPELL_AURA_MOD_HEALING_DONE_PERCENT;
        if (id == 680367)
            info->Effects[1].Effect = 0;
    }
    if (id == 807402)
        info->Effects[1].Effect = 0;
    if (id == 807768)
        info->Effects[0].Effect = SPELL_EFFECT_DUMMY;
    if (id == 802120 || id == 680369)
    {
        info->Effects[0].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
        info->Effects[0].TriggerSpell = 0;
    }
    if (id == 573220)
    {
        // Transform uses a creature template, not a display ID.
        info->Effects[1].MiscValue = 21362;
    }
    if (id == 706650)
        dummy(0), info->Effects[0].BasePoints = 29;
    if (id == 706238)
        dummy(0), info->Effects[1].Effect = 0;
    if (id == 706877 || id == 706889 || id == 300751)
        dummy(0);
    if (id == 805474)
        dummy(0); // Target's caster-owned modifier is evaluated at heal time.
    if (id == 807542)
    {
        mod(0, SPELL_AURA_ADD_PCT_MODIFIER, 0, SPELLMOD_CASTING_TIME, flag96(2097152, 0, 0));
        info->StackAmount = 1;
    }
    if (id == 900755)
        AscensionContract::SetDuration(info, 21, "Pyromancer");
    if (id == 806783)
        info->Effects[0].SpellClassMask = flag96(2, 0, 1048576);
    if (id == 525059)
        info->Effects[1].SpellClassMask = flag96(0, 0, 2048);
    if (id == 520823)
    {
        info->Effects[1].Effect = 0;
        mod(2, SPELL_AURA_ADD_PCT_MODIFIER, -100, SPELLMOD_COST, flag96(134217728, 0, 8192));
    }
    if (id == 680387)
        dummy(1); // Retaliation scales from the owner's Spirit/SP once.
    if (id == 807944)
        for (uint8 i = 0; i < 2; ++i)
        {
            info->Effects[i].ApplyAuraName = SPELL_AURA_PERIODIC_DUMMY;
            info->Effects[i].TriggerSpell = 0;
            info->Effects[i].Amplitude = i ? 1000 : 5000;
        }
    if (id == 807540)
        info->Effects[1].Effect = 0;
    if (id == 680372)
        info->Effects[2].Effect = 0;
    if (id == 803380)
        info->Effects[0].Effect = 0; // Obsolete Overheat remover points to absent legacy ID 802565.
    if (id == 806148 || id == 503229)
    {
        // Gaze of Ysera: "Direct damage dealt may interrupt this effect". Both records carry the
        // same 40 s (DurationIndex 64) MECHANIC_SLEEP stun on effect 0 (SPELL_AURA_MOD_STUN) plus a
        // SPELL_AURA_DUMMY on effect 1.
        // The two records differ, and only one of the two statements below is about both:
        //   806148 (SkillLineAbility 89592, skill 86): ProcFlags 0x222A8 (melee, ranged and negative
        //     spell hits taken, deliberately without PROC_FLAG_TAKEN_PERIODIC), ProcChance 100,
        //     RecoveryTime 90000, AuraInterruptFlags 0x00480000.
        //   503229 (rank "Sleep", SkillLineAbility 87635, skill 85): ProcFlags already 0,
        //     ProcChance 80, RecoveryTime 30000, AuraInterruptFlags 0.
        // The break is a proc of the sleep itself, from a spell_proc row
        // (2026_09_28_30_ascension_pyromancer_gaze_of_ysera.sql): the same taken-hit flags as
        // 806148, direct damage only (SpellTypeMask DAMAGE, no periodic flag), triggered hits
        // included, one charge, Chance 0 so the record's ProcChance applies (100 and 80), and
        // DisableEffectsMask 6 so only the stun effect decides (it needs non-zero damage: a fully
        // absorbed hit does not break). That SQL must be applied BEFORE this binary starts.
        // Aura::CalcProcChance then applies the caster's SPELLMOD_CHANCE_OF_SUCCESS, which is
        // Ysera's Blessing 704820 (-10). Until 2026-09-28 the break was AURA_INTERRUPT_FLAG_DIRECT_DAMAGE:
        // certain, with no chance field, so 503229's 80% and Ysera's Blessing were both lost.
        // ProcFlags stays 0 on the record: the explicit row carries the flags, and a CC aura with
        // DBC ProcFlags would get a damage cap (AuraEffect::CalculateAmount) instead of breaking.
        // Pets and combat AI still attack a sleeping target, as before.
        info->ProcFlags = 0;
    }
    if (id == 706893)
        mod(1, SPELL_AURA_ADD_FLAT_MODIFIER, -500, SPELLMOD_DURATION, flag96(0, 0, 8388608));
    if (id == 807224)
        info->StackAmount = 5;
    if (id == 803712)
        info->StackAmount = 3;
    if (id == 707480 || id == 704823)
    {
        flag96 mask;
        for (uint32 root : {800792, 802174, 801915, 805500, 800818, 520019, 680369, 704278, 706854, 802791})
            if (SpellInfo const* spender = sSpellMgr->GetSpellInfo(root))
                mask |= spender->SpellFamilyFlags;
        info->Effects[0].SpellClassMask = mask;
    }
    if (id == 704277)
        AscensionContract::SetDuration(info, 1, "Pyromancer");
    if (id == 802173 || id == 520826 || id == 680370 || id == 680371)
    {
        info->AscensionIgnoreAbsorbAndResistance = true;
    }
    if (id == 680370 || id == 680371)
        info->Attributes |= SPELL_ATTR0_NO_IMMUNITIES;
    for (uint32 heal : {707110, 706856, 806742, 806743, 806749, 712482})
        if (id == heal)
        {
            info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
            info->Effects[0].TargetB = SpellImplicitTargetInfo();
        }
    if (id == 803704 || id == 680366)
    {
        info->Effects[0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        info->Effects[0].TargetB = SpellImplicitTargetInfo();
    }
    if (id == 803704)
    {
        info->Effects[1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
        info->Effects[1].TargetB = SpellImplicitTargetInfo();
    }
    info->_InitializeExplicitTargetMask();
}
} // namespace AscensionPyromancer
namespace
{
using namespace AscensionPyromancer;
class pyromancer_scaling : public UnitScript
{
  public:
    pyromancer_scaling()
        : UnitScript("pyromancer_scaling", true,
                     {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                      UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_AURA_APPLY,
                      UNITHOOK_ON_PERIODIC_DAMAGE_RESULT})
    {
    }
    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        Player* player = Owner(caster);
        if (!player || !info)
            return;
        for (auto const& row : PyromancerCoefficients)
            if (row.spell == info->Id && row.effect == index)
                value += row.sp * std::max(0, player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE)) +
                         row.spirit * player->GetStat(STAT_SPIRIT) +
                         row.healing * std::max(0, player->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_FIRE));
        if ((info->Id == 680370 || info->Id == 680371) && !index)
            value *= 1 + State(player).ignis * Amount(680382) / 100.0f;
        value = std::clamp(value, float(INT32_MIN / 2), float(INT32_MAX / 2));
    }
    float Factor(Unit* target, Unit* caster, SpellInfo const* info)
    {
        Player* player = Owner(caster);
        if (!player || !info || !target || Derived(info))
            return 1;
        float factor = 1;
        if (Any(info, {800792, 801915}) && player->HasAura(706889) && Burning(player, target))
            factor *= 1 + Amount(706889) / 100.0f;
        if (Named(info, 805500) && player->HasAura(706238) && target->GetHealthPct() < 35)
            factor *= 1 + Amount(706238) / 100.0f;
        if (Named(info, 800792) && player->HasAura(520884))
            factor *= 1 + Burning(player, target) * .15f;
        // Cataclysmic Power: $s1% more Flare Bolt damage per periodic effect on the target. The DBC
        // route (SPELLMOD_EFFECT2 on the DoTs' aura 271 slot) lands in slots the contract turned
        // into storage dummies. Ranks are not chained (P-046): each id is tested.
        if (Named(info, 800790))
            for (uint32 rank : {807912u, 807882u, 804617u})
                if (player->HasAura(rank))
                {
                    if (uint32 dots = Burning(player, target))
                        factor *= 1 + dots * std::max(0, Amount(rank)) / 100.0f;
                    break;
                }
        return factor;
    }
    void ModifySpellDamageTaken(Unit* target, Unit* caster, int32& damage, SpellInfo const* info) override
    {
        damage = int32(damage * Factor(target, caster, info));
    }
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* caster, uint32& damage, SpellInfo const* info) override
    {
        damage = uint32(damage * Factor(target, caster, info));
    }
    void ModifyHealReceived(Unit* target, Unit* caster, uint32& healing, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (!player || !target || !info || info->SpellFamilyName != 30 || Derived(info))
            return;
        if (Aura* aura = target->GetAura(805474, player->GetGUID()))
            AddPct(healing, aura->GetStackAmount() * Amount(805474));
    }
    void OnAuraApply(Unit* target, Aura* aura) override
    {
        Player* player = Owner(target);
        if (player == target && player && aura->GetSpellInfo()->HasAura(SPELL_AURA_MOD_STUN) && player->HasAura(524818))
            Cast(player, player, 524819);
    }
    void OnPeriodicDamageResult(Unit*, Unit* caster, uint32 damage, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (player && info && info->Id == 807540 && damage)
        {
            Resource(player, HeatAura, 1);
            Mana(player, (player->GetMaxPower(POWER_MANA) - player->GetPower(POWER_MANA)) * 2 / 100, 807402);
        }
    }
};
} // namespace
void AddSC_AscensionPyromancerContracts()
{
    new pyromancer_scaling();
}
