/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionPrimalistEarthshaping.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Random.h"
#include "SpellScript.h"
#include "WorldSession.h"
#include <array>

namespace
{
constexpr uint32 PRIMALIST_FAMILY = uint32(CLASS_WILDWALKER) + 6;
constexpr uint32 SPELL_EARTHSHAPING = 680441;
constexpr uint32 SPELL_STONESHARD_MODIFIER = 680846;
constexpr uint32 SPELL_EARTHQUAKE_MODIFIER = 532562;
constexpr uint32 SPELL_ERUPTION_MODIFIER = 680450;
constexpr uint32 SPELL_BLESSING_OF_THERAZANE = 680439;
constexpr std::array<uint32, 3> EARTHSHAPING_HELPERS =
    {SPELL_STONESHARD_MODIFIER, SPELL_EARTHQUAKE_MODIFIER, SPELL_ERUPTION_MODIFIER};

bool IsEarthshapingOwner(Unit* owner)
{
    return owner && owner->IsPlayer() && owner->getClass() == CLASS_WILDWALKER;
}

void SynchronizeEarthshapingHelpers(Unit* owner, bool remove)
{
    if (!IsEarthshapingOwner(owner))
        return;

    Aura const* main = remove ? nullptr : owner->GetAura(SPELL_EARTHSHAPING, owner->GetGUID());
    for (uint32 spellId : EARTHSHAPING_HELPERS)
    {
        if (!main)
        {
            owner->RemoveAurasDueToSpell(spellId, owner->GetGUID());
            continue;
        }

        Aura* helper = owner->GetAura(spellId, owner->GetGUID());
        if (!helper)
            helper = owner->AddAura(spellId, owner);
        if (helper)
        {
            // SetStackAmount also recalculates Blessing's effect-2 modifier.
            // The visible aura alone owns expiry, including no-refresh gains.
            helper->SetStackAmount(main->GetStackAmount());
            helper->SetDuration(-1);
        }
    }
}

bool ValidateEarthshapingHelpers()
{
    SpellInfo const* stoneshard = sSpellMgr->GetSpellInfo(SPELL_STONESHARD_MODIFIER);
    SpellInfo const* earthquake = sSpellMgr->GetSpellInfo(SPELL_EARTHQUAKE_MODIFIER);
    SpellInfo const* eruption = sSpellMgr->GetSpellInfo(SPELL_ERUPTION_MODIFIER);
    if (!stoneshard || !earthquake || !eruption)
        return false;

    for (SpellInfo const* helper : {stoneshard, earthquake, eruption})
        if (!helper->HasAttribute(SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED) ||
            helper->HasAttribute(SPELL_ATTR0_CU_FORCE_AURA_SAVING))
            return false;

    // Refuse the old wildcard damage modifier if the metadata correction was
    // not installed. Every other effect keeps its native calculation path.
    return stoneshard->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
        stoneshard->Effects[EFFECT_1].IsAura(SPELL_AURA_ADD_PCT_MODIFIER) &&
        stoneshard->Effects[EFFECT_1].SpellClassMask == flag96(0, 512, 0) &&
        earthquake->Effects[EFFECT_1].IsAura(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS) &&
        earthquake->Effects[EFFECT_2].IsAura(SPELL_AURA_ADD_PCT_MODIFIER) &&
        earthquake->Effects[EFFECT_2].MiscValue == SPELLMOD_DOT &&
        earthquake->Effects[EFFECT_2].SpellClassMask == flag96(0, 0, 268435456) &&
        eruption->Effects[EFFECT_2].IsAura(SPELL_AURA_ADD_FLAT_MODIFIER) &&
        eruption->Effects[EFFECT_2].MiscValue == SPELLMOD_DURATION &&
        eruption->Effects[EFFECT_2].SpellClassMask == flag96(0, 0, 2097152);
}

// Vague 5 : talents « Generating Earthshaping ... ». Aucun ProcFlags ne dit « une pile
// d'Earthshaping gagnee » : les piles viennent du module (ResourceGainRules, GainEarthshaping,
// effets 175), jamais d'un evenement de proc. Le seul point commun a tous ces chemins est le
// changement de montant de 680441, vu ici. Une baisse (Heavy Earth, effet 175 -1) n'est pas un gain.
constexpr uint32 SPELL_LITHIC_LANCE_TALENT = 706159;   // 20 %, -> 807048
constexpr uint32 SPELL_LITHIC_LANCE_READY  = 807048;
constexpr uint32 SPELL_DREAM_TALENT        = 680452;   // a 10 piles ou plus, -> 578256 -> 578255
constexpr uint32 SPELL_DREAM               = 578255;
constexpr uint32 SPELL_CATACLYSM           = 680444;   // 10 %, -> 680445 (moitie Stoneshard en spell_proc)
constexpr uint32 SPELL_SEISMIC_RESET       = 680445;
constexpr uint8 DREAM_THRESHOLD = 10;

// ProcChance du talent, modifie par SPELLMOD_CHANCE_OF_SUCCESS (piege n. 3 de la procedure).
bool RollTalentChance(Player* player, uint32 talentId)
{
    SpellInfo const* talent = sSpellMgr->GetSpellInfo(talentId);
    if (!talent)
        return false;
    float chance = float(talent->ProcChance);
    player->ApplySpellMod(talentId, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    return roll_chance_f(chance);
}

void OnEarthshapingGained(Player* player, uint8 stacks)
{
    if (!player->IsAlive() || !player->IsInWorld())
        return;
    if (player->HasAura(SPELL_LITHIC_LANCE_TALENT) && RollTalentChance(player, SPELL_LITHIC_LANCE_TALENT))
        player->CastSpell(player, SPELL_LITHIC_LANCE_READY, true);
    // 578256 (TRIGGER_SPELL, tA 6) exige une cible unite ; 578255 vise le lanceur (tA 1).
    if (stacks >= DREAM_THRESHOLD && player->HasAura(SPELL_DREAM_TALENT))
        player->CastSpell(player, SPELL_DREAM, true);
    if (player->HasAura(SPELL_CATACLYSM) && RollTalentChance(player, SPELL_CATACLYSM))
        player->CastSpell(player, SPELL_SEISMIC_RESET, true);
}

class spell_ascension_primalist_earthshaping : public AuraScript
{
    PrepareAuraScript(spell_ascension_primalist_earthshaping);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Id == SPELL_EARTHSHAPING &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            spellInfo->Effects[EFFECT_0].IsAura(SPELL_AURA_HASTE_SPELLS) && ValidateEarthshapingHelpers();
    }

    bool Load() override
    {
        Unit* owner = GetUnitOwner();
        return IsEarthshapingOwner(owner) && GetCasterGUID() == owner->GetGUID();
    }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        SynchronizeEarthshapingHelpers(GetTarget(), false);

        uint8 current = GetStackAmount();
        bool gained = current > _previousStacks;
        _previousStacks = current;
        Player* player = GetTarget()->ToPlayer();
        // Une aura rechargee a la connexion n'est pas un gain.
        if (gained && player && player->GetSession() && !player->GetSession()->PlayerLoading())
            OnEarthshapingGained(player, current);
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        _previousStacks = 0;
        SynchronizeEarthshapingHelpers(GetTarget(), true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ascension_primalist_earthshaping::Apply,
            EFFECT_0, SPELL_AURA_HASTE_SPELLS, AURA_EFFECT_HANDLE_CHANGE_AMOUNT_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ascension_primalist_earthshaping::Remove,
            EFFECT_0, SPELL_AURA_HASTE_SPELLS, AURA_EFFECT_HANDLE_REAL);
    }

    uint8 _previousStacks = 0;
};

class spell_ascension_primalist_therazane_update : public AuraScript
{
    PrepareAuraScript(spell_ascension_primalist_therazane_update);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Id == SPELL_BLESSING_OF_THERAZANE &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            spellInfo->Effects[EFFECT_0].IsAura(SPELL_AURA_ADD_FLAT_MODIFIER) &&
            spellInfo->Effects[EFFECT_0].MiscValue == SPELLMOD_EFFECT2 &&
            spellInfo->Effects[EFFECT_0].SpellClassMask == flag96(0, 65536, 0) &&
            ValidateEarthshapingHelpers();
    }

    bool Load() override
    {
        Unit* owner = GetUnitOwner();
        return IsEarthshapingOwner(owner) && GetCasterGUID() == owner->GetGUID();
    }

    void Update(AuraEffect const*, AuraEffectHandleModes)
    {
        SynchronizeEarthshapingHelpers(GetTarget(), false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_ascension_primalist_therazane_update::Update,
            EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_CHANGE_AMOUNT_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ascension_primalist_therazane_update::Update,
            EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_CHANGE_AMOUNT_MASK);
    }
};
}

void ApplyAscensionPrimalistEarthshapingContracts(SpellInfo* spellInfo)
{
    if (!spellInfo || spellInfo->SpellFamilyName != PRIMALIST_FAMILY)
        return;

    for (uint32 spellId : EARTHSHAPING_HELPERS)
        if (spellInfo->Id == spellId && spellInfo->StackAmount == 15)
            spellInfo->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;

    if (spellInfo->Id != SPELL_STONESHARD_MODIFIER)
        return;

    SpellEffectInfo& effect = spellInfo->Effects[EFFECT_0];
    if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
        (effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER || effect.ApplyAuraName == SPELL_AURA_DUMMY) &&
        effect.BasePoints == 1 && effect.DieSides == 1 && effect.MiscValue == SPELLMOD_DAMAGE &&
        effect.SpellClassMask == flag96(0, 0, 0))
        effect.ApplyAuraName = SPELL_AURA_DUMMY;
}

void AddSC_AscensionPrimalistEarthshaping()
{
    RegisterSpellScript(spell_ascension_primalist_earthshaping);
    RegisterSpellScript(spell_ascension_primalist_therazane_update);
}
