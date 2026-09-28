/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Log.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"

namespace
{
enum AirElementalSpells : uint32
{
    SPELL_SUMMON_AIR_ELEMENTAL = 804019,
    SPELL_AIR_ELEMENTAL_PASSIVE = 806010,
    SPELL_INVIGORATION_PROC = 806020,
    SPELL_GENERATE_INVIGORATION = 500348,
    SPELL_INVIGORATION = 680918,
    SPELL_AURAT_SCRIPTS = 712431,
    SPELL_AURAT_SCRIPTS_PROC = 712488,
    SPELL_AURAT_GALE = 500019,
    SPELL_FLURRY_READY = 807465,
    SPELL_FLURRY_DOT = 807555,
    SPELL_FLURRY_DEBUFF = 807464,
    // 804830 Skyfall force-casts 807134 on the pet: "The next instance of damage will unleash
    // an Aether Strike" (807133). 807134 is an aura 42 with ProcFlags 0 and no spell_proc
    // row, so SpellMgr skips it (P-045); the pet's damage proc below supplies the event.
    SPELL_SKYFALL_READY = 807134,
    SPELL_SKYFALL_STRIKE = 807133
};

enum AirElementalEntries : uint32
{
    NPC_AIR_ELEMENTAL = 500941
};

// Value promised by 807464's aura tooltip ("Magic Damage taken increased by 2%").
constexpr int32 FLURRY_MAGIC_DAMAGE_TAKEN_PCT = 2;

Player* AirElementalOwner(Unit* unit)
{
    Pet* pet = unit ? unit->ToPet() : nullptr;
    if (!pet || pet->GetEntry() != NPC_AIR_ELEMENTAL)
        return nullptr;
    Player* owner = pet->GetOwner();
    return owner && owner->getClass() == CLASS_STORMBRINGER && owner->GetPet() == pet &&
        owner->HasActiveSpell(SPELL_SUMMON_AIR_ELEMENTAL) ? owner : nullptr;
}

class stormbringer_pet_lifecycle : public PlayerScript
{
public:
    stormbringer_pet_lifecycle() : PlayerScript("stormbringer_pet_lifecycle",
        {PLAYERHOOK_ON_BEFORE_GUARDIAN_INIT_STATS_FOR_LEVEL, PLAYERHOOK_ON_UPDATE}) { }

    void OnPlayerBeforeGuardianInitStatsForLevel(Player* player, Guardian* guardian, CreatureTemplate const*,
        PetType& type) override
    {
        if (player->getClass() == CLASS_STORMBRINGER && guardian && guardian->GetEntry() == NPC_AIR_ELEMENTAL)
            type = SUMMON_PET;
    }

    void OnPlayerUpdate(Player* player, uint32) override
    {
        if (player->getClass() != CLASS_STORMBRINGER)
            return;
        Pet* pet = player->GetPet();
        if (!pet || pet->GetEntry() != NPC_AIR_ELEMENTAL || pet->isBeingLoaded())
            return;
        if (!player->HasActiveSpell(SPELL_SUMMON_AIR_ELEMENTAL))
        {
            player->RemovePet(pet, PET_SAVE_NOT_IN_SLOT);
            return;
        }
        // Pet loading is asynchronous. Apply the captured owner-to-pet passive only
        // after native spell/aura loading, including restoration after a resummon.
        if (player->IsAlive() && pet->IsAlive() && player->IsInWorld() && pet->IsInWorld() &&
            player->GetMap() == pet->GetMap() && player->InSamePhase(pet) &&
            (!pet->HasAura(SPELL_AIR_ELEMENTAL_PASSIVE, player->GetGUID()) ||
                !pet->HasAura(SPELL_INVIGORATION_PROC, pet->GetGUID())))
            player->CastSpell(pet, SPELL_AIR_ELEMENTAL_PASSIVE, true);
    }
};

class aura_ascension_air_invigoration : public AuraScript
{
    PrepareAuraScript(aura_ascension_air_invigoration);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_GENERATE_INVIGORATION, SPELL_AURAT_SCRIPTS_PROC, SPELL_AURAT_GALE,
            SPELL_FLURRY_READY, SPELL_FLURRY_DOT, SPELL_SKYFALL_READY, SPELL_SKYFALL_STRIKE});
    }

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* pet = GetTarget();
        Player* owner = AirElementalOwner(pet);
        Unit* victim = event.GetActionTarget();
        return owner && owner->IsAlive() && owner->IsInWorld() && pet->IsAlive() && pet->IsInWorld() &&
            owner->GetMap() == pet->GetMap() && owner->InSamePhase(pet) && event.GetActor() == pet &&
            victim && victim != pet && !pet->IsFriendlyTo(victim) &&
            event.GetDamageInfo() && event.GetDamageInfo()->GetDamage();
    }

    void Invigorate(AuraEffect const*, ProcEventInfo& event)
    {
        PreventDefaultAction();
        GetTarget()->CastSpell(GetTarget(), SPELL_GENERATE_INVIGORATION, true);
        // This aura already receives each successful owned-pet damage event at 100% chance.
        // The companion Aurat record retains the authored chance and empowered Gale helper.
        Player* owner = AirElementalOwner(GetTarget());
        if (owner && GetTarget()->HasAura(SPELL_FLURRY_READY, GetTarget()->GetGUID()))
        {
            // The pet's existing successful-damage proc supplies the missing
            // "next instance of damage" event for Flurry's zero-flag aura.
            GetTarget()->RemoveAurasDueToSpell(SPELL_FLURRY_READY, GetTarget()->GetGUID());
            int32 amount = sSpellMgr->GetSpellInfo(SPELL_FLURRY_DOT)->Effects[EFFECT_0].CalcValue(owner) +
                int32(owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE) * 0.2f);
            GetTarget()->CastCustomSpell(SPELL_FLURRY_DOT, SPELLVALUE_BASE_POINT0, amount,
                event.GetActionTarget(), true);
        }
        if (owner && GetTarget()->HasAura(SPELL_SKYFALL_READY, GetTarget()->GetGUID()))
        {
            // Removed first: the strike's own hit comes back here and must not consume again.
            GetTarget()->RemoveAurasDueToSpell(SPELL_SKYFALL_READY, GetTarget()->GetGUID());
            // "${$807133m1+$807133ppl1+$spa*1}": the owner's spell power is snapshot (coefficient 1,
            // the tooltip's), as for Flurry. 807133 carries no coefficient of its own, so the
            // pet's spell power is not added a second time. The triggered cast ignores its
            // CasterAuraSpell 573249, which the pet does not have.
            // CalcValue(nullptr): the per-level part (ppl 15 above BaseLevel 59) is added once,
            // by the pet's own cast (SpellInfo.cpp:463-482 applies it even to a custom base).
            SpellInfo const* strike = sSpellMgr->GetSpellInfo(SPELL_SKYFALL_STRIKE);
            int32 amount = strike->Effects[EFFECT_0].CalcValue(nullptr) +
                int32(owner->SpellBaseDamageBonusDone(strike->GetSchoolMask()) * 1.0f);
            GetTarget()->CastCustomSpell(SPELL_SKYFALL_STRIKE, SPELLVALUE_BASE_POINT0, amount,
                event.GetActionTarget(), true);
        }
        if (owner && owner->HasActiveSpell(SPELL_AURAT_SCRIPTS) &&
            roll_chance_i(sSpellMgr->GetSpellInfo(SPELL_AURAT_SCRIPTS_PROC)->ProcChance))
        {
            // 500019's only target is TARGET_UNIT_MASTER (27), resolved through
            // m_caster->GetCharmerOrOwner(): cast by the player himself it found no target and
            // the empowerment never existed (same trap as Xoroth 500605/803254). AddAura skips
            // target selection and places it on the player with the player as caster.
            owner->AddAura(SPELL_AURAT_GALE, owner);
            if (Aura* empowerment = owner->GetAura(SPELL_AURAT_GALE, owner->GetGUID()))
                empowerment->SetCharges(1); // Native spell modifiers consume one charge for the whole Gale cast.
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_air_invigoration::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_air_invigoration::Invigorate,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

class spell_ascension_air_invigoration_duration : public SpellScript
{
    PrepareSpellScript(spell_ascension_air_invigoration_duration);
    int32 remaining = 0;

    bool Load() override { return AirElementalOwner(GetCaster()) != nullptr; }

    void SnapshotDuration()
    {
        if (Aura* aura = GetCaster()->GetAura(SPELL_INVIGORATION, GetCaster()->GetGUID()))
            remaining = aura->GetDuration();
    }

    void RestoreDuration()
    {
        // ModStackAmount refreshes native timers, including at the ten-stack cap.
        // Preserve this aura's original expiry for every source of additional stacks.
        if (remaining > 0)
            if (Aura* aura = GetCaster()->GetAura(SPELL_INVIGORATION, GetCaster()->GetGUID()))
                aura->SetDuration(remaining);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_air_invigoration_duration::SnapshotDuration);
        AfterCast += SpellCastFn(spell_ascension_air_invigoration_duration::RestoreDuration);
    }
};

class stormbringer_pet_contracts : public GlobalScript
{
public:
    stormbringer_pet_contracts() : GlobalScript("stormbringer_pet_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info && info->Id == SPELL_FLURRY_DEBUFF)
        {
            // Spell.dbc stores the magic-damage slot as -3 over one die side, i.e. -2%,
            // while the aura tooltip promises "Magic Damage taken increased by 2%".
            // Normalise it to the promised +2%.
            //
            // The former guard also demanded SpellFamilyName == 22 (Stormbringer), while
            // the record carries 18 (Barbarian) in both client files we hold — the
            // 2026-09-19 one and the one it replaced. So this normalisation was already
            // dead before that import and is not one of its casualties. The spell id is
            // unique, so no family test is needed here; what the record must still satisfy
            // is that slot 1 really is the damage-taken aura, and that is now reported.
            SpellEffectInfo& magicTaken = info->Effects[EFFECT_1];
            if (magicTaken.Effect == SPELL_EFFECT_APPLY_AURA &&
                magicTaken.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)
            {
                magicTaken.BasePoints = magicTaken.CalcBaseValue(FLURRY_MAGIC_DAMAGE_TAKEN_PCT);
            }
            else
            {
                LOG_ERROR("module.ascension_compat",
                    "Flurry debuff {}: effect 1 is no longer a damage-taken aura (effect {}, aura {}), "
                    "the +{}% normalisation was skipped",
                    info->Id, magicTaken.Effect, uint32(magicTaken.ApplyAuraName),
                    FLURRY_MAGIC_DAMAGE_TAKEN_PCT);
            }
        }
        if (info && info->Id == SPELL_FLURRY_DOT && info->SpellFamilyName == 22)
        {
            // The proc snapshots the owner's Nature power. Keep native pet
            // percentage and target modifiers without adding the pet's SP again.
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
    }
};
}

void AddSC_AscensionStormbringerPet()
{
    new stormbringer_pet_lifecycle();
    new stormbringer_pet_contracts();
    RegisterSpellScript(aura_ascension_air_invigoration);
    RegisterSpellScript(spell_ascension_air_invigoration_duration);
}
