/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionPrimalistSpiritBeast.h"
#include "Creature.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <limits>

namespace
{
constexpr uint32 SPELL_HARNESS_ANIMAL_SPIRIT = 574301;
constexpr uint32 SPELL_HARNESS_TAME_HELPER = 574304;
constexpr uint32 PRIMALIST_SPELL_FAMILY = 37;
constexpr uint32 HARNESS_DURATION = 20000;
constexpr uint32 HARNESS_OLD_PERIOD = 19000;

bool IsHarnessLayout(SpellInfo const* info)
{
    return info && info->Id == SPELL_HARNESS_ANIMAL_SPIRIT &&
        info->SpellFamilyName == PRIMALIST_SPELL_FAMILY && !info->SpellFamilyFlags &&
        info->IsChanneled() && info->GetDuration() == HARNESS_DURATION && info->TargetCreatureType == 1 &&
        info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
        info->Effects[EFFECT_0].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY &&
        !info->Effects[EFFECT_0].TargetB.GetTarget() &&
        info->Effects[EFFECT_1].IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL) &&
        info->Effects[EFFECT_1].TriggerSpell == SPELL_HARNESS_TAME_HELPER &&
        info->Effects[EFFECT_1].TargetA.GetTarget() == TARGET_UNIT_CASTER &&
        !info->Effects[EFFECT_1].TargetB.GetTarget() && !info->Effects[EFFECT_2].Effect &&
        !info->HasAttribute(SPELL_ATTR5_EXTRA_INITIAL_PERIOD) &&
        (info->Effects[EFFECT_1].Amplitude == HARNESS_OLD_PERIOD ||
            info->Effects[EFFECT_1].Amplitude == HARNESS_DURATION);
}

bool IsTameHelperLayout(SpellInfo const* info)
{
    return info && info->Id == SPELL_HARNESS_TAME_HELPER && !info->SpellFamilyName &&
        !info->SpellFamilyFlags && info->TargetCreatureType == 1 &&
        info->Effects[EFFECT_0].Effect == SPELL_EFFECT_TAMECREATURE &&
        info->Effects[EFFECT_0].TargetA.GetTarget() == TARGET_UNIT_CHANNEL_TARGET &&
        !info->Effects[EFFECT_0].TargetB.GetTarget() &&
        !info->Effects[EFFECT_1].Effect && !info->Effects[EFFECT_2].Effect;
}

bool HasHarnessContract()
{
    SpellInfo const* parent = sSpellMgr->GetSpellInfo(SPELL_HARNESS_ANIMAL_SPIRIT);
    return IsHarnessLayout(parent) && parent->Effects[EFFECT_1].Amplitude == HARNESS_DURATION &&
        IsTameHelperLayout(sSpellMgr->GetSpellInfo(SPELL_HARNESS_TAME_HELPER));
}

SpellCastResult CheckHarnessTarget(Player* player, Unit* unitTarget)
{
    if (!player || !IsAscensionPrimalistTameEligible(player) || !HasHarnessContract())
    {
        if (player)
            player->SendTameFailure(PET_TAME_UNITS_CANT_TAME);
        return SPELL_FAILED_DONT_REPORT;
    }

    Creature* target = unitTarget ? unitTarget->ToCreature() : nullptr;
    if (!player->IsAlive() || !target || !target->IsAlive() || !target->GetCreatureTemplate())
    {
        player->SendTameFailure(PET_TAME_INVALID_CREATURE);
        return SPELL_FAILED_DONT_REPORT;
    }

    // Same target and stable restrictions as native spell_hun_tame_beast.
    // Repeat at the selected helper hit to cover changes during the channel.
    if (target->GetLevel() > player->GetLevel())
    {
        player->SendTameFailure(PET_TAME_TOO_HIGHLEVEL);
        return SPELL_FAILED_DONT_REPORT;
    }
    if (target->GetCreatureTemplate()->IsExotic() && !player->CanTameExoticPets())
    {
        player->SendTameFailure(PET_TAME_CANT_CONTROL_EXOTIC);
        return SPELL_FAILED_DONT_REPORT;
    }
    if (!target->GetCreatureTemplate()->IsTameable(player->CanTameExoticPets()))
    {
        player->SendTameFailure(PET_TAME_NOT_TAMEABLE);
        return SPELL_FAILED_DONT_REPORT;
    }
    if (player->GetPetGUID())
        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
    if (PetStable const* stable = player->GetPetStable())
    {
        if (stable->CurrentPet)
            return SPELL_FAILED_ALREADY_HAVE_SUMMON;
        if (stable->GetUnslottedHunterPet())
        {
            player->SendTameFailure(PET_TAME_TOO_MANY);
            return SPELL_FAILED_DONT_REPORT;
        }
    }
    if (player->GetCharmGUID())
    {
        player->SendTameFailure(PET_TAME_ANOTHER_SUMMON_ACTIVE);
        return SPELL_FAILED_DONT_REPORT;
    }
    if (target->GetOwnerGUID() || target->IsPet())
    {
        player->SendTameFailure(PET_TAME_CREATURE_ALREADY_OWNED);
        return SPELL_FAILED_DONT_REPORT;
    }
    return SPELL_CAST_OK;
}

class spell_ascension_primalist_harness : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_harness);

    bool Validate(SpellInfo const* info) override
    {
        return info->Id == SPELL_HARNESS_ANIMAL_SPIRIT &&
            ValidateSpellInfo({ SPELL_HARNESS_ANIMAL_SPIRIT, SPELL_HARNESS_TAME_HELPER });
    }

    SpellCastResult CheckCast()
    {
        return CheckHarnessTarget(GetCaster()->ToPlayer(), GetExplTargetUnit());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_primalist_harness::CheckCast);
    }
};

class spell_ascension_primalist_harness_tame : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_harness_tame);

    bool Validate(SpellInfo const* info) override
    {
        return info->Id == SPELL_HARNESS_TAME_HELPER &&
            ValidateSpellInfo({ SPELL_HARNESS_ANIMAL_SPIRIT, SPELL_HARNESS_TAME_HELPER });
    }

    void ValidateTame(SpellEffIndex effIndex)
    {
        SpellInfo const* trigger = GetSpell()->GetTriggeredByAuraSpellInfo();
        // Natural channel completion clears the live channel GUID before the
        // aura's last tick. Use the native stored channel target, not that GUID.
        if (!trigger || trigger->Id != SPELL_HARNESS_ANIMAL_SPIRIT ||
            GetSpell()->GetTriggeredByAuraTickNumber() != 1 ||
            CheckHarnessTarget(GetCaster()->ToPlayer(), GetHitUnit()) != SPELL_CAST_OK)
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_harness_tame::ValidateTame,
            EFFECT_0, SPELL_EFFECT_TAMECREATURE);
    }
};

// C-20 / C-21 : 34902 Hunter Pet Scaling 01, porte par chaque familier de chasseur (HUNTER_PET), y
// compris ceux d'un Primalist. spell_hun_generic_scaling (spell_hunter.cpp) donne au familier 22 % de
// la PA A DISTANCE du maitre ; classe 31 mappee DRUID (SharedDefines.h), c'est Agilite - 10, sans
// Force. Ici, pour un maitre Primalist seulement, la meme part (22 %, Wild Hunt compris) de sa PA de
// MELEE. Ampleur non mesuree.
// ORDRE : ce script est un second script de 34902. Les crochets DoEffectCalcAmount passent dans
// l'ordre de chargement des scripts, qui est celui de la lecture de spell_script_names : table sans
// cle primaire, index unique (spell_id, ScriptName) donc lu dans l'ordre alphabetique des noms.
// « spell_hun_primalist_pet_scaling » passe APRES « spell_hun_generic_scaling » et ecrase sa valeur ;
// d'ou ce nom, hors convention du module. Renommer ce script casse le correctif.
// Bonheur : un Primalist n'a aucun moyen de nourrir son familier (aucun Feed Pet), qui finit
// UNHAPPY (-25 % de degats, Pet.cpp). Choix fidele a des « esprits » : bonheur tenu au maximum, a
// la pose et a chaque tick de 2 s que spell_hun_generic_scaling impose a ce sort.
class spell_hun_primalist_pet_scaling : public AuraScript
{
    PrepareAuraScript(spell_hun_primalist_pet_scaling);

    static Player* PrimalistMaster(Unit* unit)
    {
        Pet* pet = unit ? unit->ToPet() : nullptr;
        if (!pet || pet->getPetType() != HUNTER_PET)
            return nullptr;
        Unit* owner = pet->GetOwner();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        return player && player->getClass() == CLASS_WILDWALKER ? player : nullptr;
    }

    void CalculateAttackPower(AuraEffect const*, int32& amount, bool&)
    {
        Player* owner = PrimalistMaster(GetUnitOwner());
        if (!owner)
            return;
        int32 modifier = 22;
        if (AuraEffect const* wildHunt = GetUnitOwner()->GetDummyAuraEffect(SPELLFAMILY_PET, 3748, EFFECT_1))
            AddPct(modifier, wildHunt->GetAmount());
        float const ap = std::clamp(owner->GetTotalAttackPowerValue(BASE_ATTACK), 0.0f,
            float(std::numeric_limits<int32>::max() / 2));
        amount = CalculatePct(int32(ap), modifier);
    }

    void KeepHappy()
    {
        Unit* unit = GetUnitOwner();
        if (!PrimalistMaster(unit) || !unit->IsAlive())
            return;
        uint32 const maximum = unit->GetMaxPower(POWER_HAPPINESS);
        if (maximum && unit->GetPower(POWER_HAPPINESS) < maximum)
            unit->SetPower(POWER_HAPPINESS, maximum);
    }

    void Applied(AuraEffect const*, AuraEffectHandleModes) { KeepHappy(); }
    void Tick(AuraEffect const*) { KeepHappy(); }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_hun_primalist_pet_scaling::CalculateAttackPower,
            EFFECT_ALL, SPELL_AURA_MOD_ATTACK_POWER);
        AfterEffectApply += AuraEffectApplyFn(spell_hun_primalist_pet_scaling::Applied, EFFECT_0,
            SPELL_AURA_MOD_STAT, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_hun_primalist_pet_scaling::Tick, EFFECT_0,
            SPELL_AURA_MOD_STAT);
    }
};
}

bool HasAscensionPrimalistHunterPetContext(Player const* player)
{
    if (!player || player->getClass() != CLASS_WILDWALKER)
        return false;
    if (IsAscensionPrimalistTameEligible(player))
        return true;
    PetStable const* stable = player->GetPetStable();
    if (!stable)
        return false;

    // Native ownership survives respec and login before CAD confirmation.
    // No mutation or CreatedBySpell restriction: normal recall changes that ID.
    if (stable->CurrentPet && stable->CurrentPet->Type == HUNTER_PET)
        return true;
    for (auto const& pet : stable->StabledPets)
        if (pet && pet->Type == HUNTER_PET)
            return true;
    return std::any_of(stable->UnslottedPets.begin(), stable->UnslottedPets.end(),
        [](PetStable::PetInfo const& pet) { return pet.Type == HUNTER_PET; });
}

void ApplyAscensionPrimalistSpiritBeastContract(SpellInfo* spellInfo)
{
    if (IsHarnessLayout(spellInfo))
        spellInfo->Effects[EFFECT_1].Amplitude = HARNESS_DURATION;

    // C-30 573365 Spirit Beast Communion (passif du niveau 40) : son infobulle dit seulement « You learn
    // Beastmaster's Recall and Spirit Stable ». Ses deux effets sont pourtant les modificateurs de
    // Beastmaster's Recall 573364 (incantation -100 %, cout -50 %), en permanence, sur Recall Animal
    // Spirit 500860 (SpellClassMask mot 1 = 0x1, lu aux champs 122+3e..124+3e, P-064) : 573364 ne
    // servait plus a rien. Les deux effets deviennent des DUMMY sans masque ; l'aura reste (la
    // spe et les enseignements TaughtAbilities lisent le sort appris, pas ses effets).
    if (spellInfo->Id == 573365 && spellInfo->SpellFamilyName == PRIMALIST_SPELL_FAMILY)
        for (uint8 i : {EFFECT_0, EFFECT_1})
        {
            SpellEffectInfo& effect = spellInfo->Effects[i];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER &&
                (effect.MiscValue == SPELLMOD_CASTING_TIME || effect.MiscValue == SPELLMOD_COST) &&
                effect.SpellClassMask[0] == 0 && effect.SpellClassMask[1] == 0x1 && effect.SpellClassMask[2] == 0)
            {
                effect.ApplyAuraName = SPELL_AURA_DUMMY;
                effect.SpellClassMask = flag96();
            }
        }
}

void AddSC_AscensionPrimalistSpiritBeast()
{
    RegisterSpellScript(spell_ascension_primalist_harness);
    RegisterSpellScript(spell_ascension_primalist_harness_tame);
    RegisterSpellScript(spell_hun_primalist_pet_scaling);
}
