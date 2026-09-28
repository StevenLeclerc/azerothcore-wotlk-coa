/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
class spell_ascension_ranger_assault : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_assault);

    bool _scaled = false;

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && (spellInfo->Id == 803108 || (spellInfo->Id >= 503099 && spellInfo->Id <= 503105)) &&
            spellInfo->SpellFamilyName == uint32(CLASS_RANGER) + 6 &&
            spellInfo->SpellFamilyFlags == flag96(0, 32768, 0) && spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE &&
            spellInfo->Effects[EFFECT_0].Effect == SPELL_EFFECT_SCHOOL_DAMAGE &&
            spellInfo->Effects[EFFECT_2].Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE &&
            spellInfo->Effects[EFFECT_2].BasePoints == 99 && spellInfo->Effects[EFFECT_2].DieSides == 1;
    }

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->ToPlayer()->getClass() == CLASS_RANGER;
    }

    void ScaleWeaponDamage(SpellEffIndex /*effIndex*/)
    {
        if (_scaled)
            return;
        _scaled = true;

        Player* caster = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Item* weapon = caster ? caster->GetWeaponForAttack(BASE_ATTACK, true) : nullptr;
        if (!weapon || weapon->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_DAGGER)
            return;

        // Change only the weapon component, before native per-effect modifiers
        // and target damage calculations. EffectWeaponDmg recalculates this
        // value, so SetEffectValue at LAUNCH_TARGET would have no effect.
        // The separate flat damage and explicit AP coefficient stay unscaled.
        int64 const percent = (int64(GetSpellValue()->EffectBasePoints[EFFECT_2]) + 1) * 7 / 4;
        GetSpell()->SetSpellValue(SPELLVALUE_BASE_POINT2, int32(std::clamp<int64>(percent, 0, std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        OnEffectLaunch += SpellEffectFn(spell_ascension_ranger_assault::ScaleWeaponDamage, EFFECT_2, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

// Flank (chain 804940, ten ranks): "dealing $s2% Weapon Damage (${$m2*2}% if a dagger is
// equipped)". The DBC carries the doubling as 582754 "Flank Dagger Damage Setter" (EFFECT2 +100%
// on Flank's FF1 0x20000, dagger required), which nothing casts. Same hook as Assault. The
// weapon percentage also scales effect 0's flat bonus (EffectWeaponDmg), as 582754 would.
class spell_ascension_ranger_flank : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_flank);

    static constexpr uint32 FlankFirstRank = 804940;
    static constexpr uint32 FlankDaggerSetter = 582754;
    bool _scaled = false;

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && sSpellMgr->GetFirstSpellInChain(spellInfo->Id) == FlankFirstRank &&
            spellInfo->SpellFamilyName == uint32(CLASS_RANGER) + 6 &&
            spellInfo->SpellFamilyFlags == flag96(0, 131072, 0) &&
            spellInfo->Effects[EFFECT_1].Effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE &&
            spellInfo->Effects[EFFECT_1].DieSides == 1;
    }

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->ToPlayer()->getClass() == CLASS_RANGER;
    }

    void ScaleWeaponDamage(SpellEffIndex /*effIndex*/)
    {
        if (_scaled)
            return;
        _scaled = true;

        Player* caster = GetCaster()->ToPlayer();
        Item* weapon = caster->GetWeaponForAttack(BASE_ATTACK, true);
        // 582754, if anything ever casts it, already doubles: never count twice.
        if (!weapon || weapon->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_DAGGER || caster->HasAura(FlankDaggerSetter))
            return;

        // EffectBasePoints holds BasePoints (149 at rank 1, DieSides 1): the tooltip's $m2 is
        // bp + 1. SetSpellValue removes the die side and CalcValue adds it back, so the doubled
        // $m2 is passed as is (the same arithmetic gives Assault exactly x1.75).
        int64 const percent = (int64(GetSpellValue()->EffectBasePoints[EFFECT_1]) + 1) * 2;
        GetSpell()->SetSpellValue(SPELLVALUE_BASE_POINT1, int32(std::clamp<int64>(percent, 0, std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        OnEffectLaunch += SpellEffectFn(spell_ascension_ranger_flank::ScaleWeaponDamage, EFFECT_1, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE);
    }
};

// Wild Strike (chain 800083, twelve ranks) triggers 560962 "Wild Strike (Off Hand)" by effect 1
// without a value, and 560962 carries a fixed +319 (rank 10's effect 2) at every rank: at rank 1
// the off-hand strike took +319 where the main hand took +3. The off-hand now takes the fixed
// bonus of the rank cast. 560962 needs a unit target, so EffectTriggerSpell casts it in
// LAUNCH_TARGET mode (SpellEffects.cpp:1210-1215): the hook is OnEffectLaunchTarget, once per
// struck enemy, as the native trigger. Spell::CheckItems still refuses it without an off-hand.
class spell_ascension_ranger_wild_strike_offhand : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_wild_strike_offhand);

    static constexpr uint32 WildStrikeOffHand = 560962;

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Effects[EFFECT_1].Effect == SPELL_EFFECT_TRIGGER_SPELL &&
            spellInfo->Effects[EFFECT_1].TriggerSpell == WildStrikeOffHand &&
            spellInfo->Effects[EFFECT_2].Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG &&
            ValidateSpellInfo({WildStrikeOffHand});
    }

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->ToPlayer()->getClass() == CLASS_RANGER;
    }

    void TriggerOffHand(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;
        int32 const bonus = std::max(0, GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster));
        caster->CastCustomSpell(WildStrikeOffHand, SPELLVALUE_BASE_POINT0, bonus, target, true);
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_ranger_wild_strike_offhand::TriggerOffHand,
            EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
    }
};
}

void AddAscensionRangerAssaultScripts()
{
    RegisterSpellScript(spell_ascension_ranger_assault);
    RegisterSpellScript(spell_ascension_ranger_flank);
    RegisterSpellScript(spell_ascension_ranger_wild_strike_offhand);
}
