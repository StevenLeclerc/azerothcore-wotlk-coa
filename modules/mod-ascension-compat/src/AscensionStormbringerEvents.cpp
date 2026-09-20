/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"

namespace
{
enum StormbringerEventSpells : uint32
{
    // Talent passives repaired here. All three are SPELL_ATTR0_PASSIVE in Spell.dbc,
    // so the learnt talent is always present as an aura on its owner.
    SPELL_GIFT_OF_AIR = 705715,
    SPELL_ENVELOPING_WINDS = 707546,
    SPELL_TITANSTORM = 801869,

    // Authored helpers the talents point at, each record read in Spell.dbc.
    SPELL_TAILWIND = 804035,             // 15 s self buff, also a raid area aura
    SPELL_TAILWIND_EXTENSION = 583254,   // effect 177, MiscValue 804035, +1500 ms
    SPELL_TITANSTORM_REDUCTION = 801854, // effects 165, MiscValue 801847 / 560030, -1500 ms

    // Rank-one anchors of the chains involved, taken from `spell_ranks`.
    SPELL_GALE_RANK_1 = 804036,
    SPELL_CALL_LIGHTNING_RANK_1 = 500040,
    SPELL_ELECTROCUTE_RANK_1 = 801844
};

enum StormbringerEventEntries : uint32
{
    NPC_AIR_ELEMENTAL = 500941
};

// Field 208 of Spell.dbc for every Stormbringer record read while writing this file.
constexpr uint32 STORMBRINGER_SPELL_FAMILY = 22;

// `spell_ranks` holds at most thirteen ranks for this class; the bound only exists so a
// corrupted chain cannot spin forever.
constexpr uint8 MAX_SPELL_CHAIN_WALK = 32;

// The Air Elemental, and only when it is really this player's live, loaded summon.
Pet* LiveAirElemental(Player* player)
{
    Pet* pet = player ? player->GetPet() : nullptr;
    if (!pet || pet->GetEntry() != NPC_AIR_ELEMENTAL || pet->isBeingLoaded() || !pet->IsAlive() ||
        !pet->IsInWorld() || pet->GetMap() != player->GetMap() || !player->InSamePhase(pet))
        return nullptr;
    return pet;
}

// SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN names one single spell id, and 801854 names
// Arm of Thorim *rank 1* (801847). A levelled Stormbringer casts rank 9 (567520), whose
// cooldown entry is a different key, so the authored effect would miss it. Walk the chain.
//
// MEASURED COST, accepted on purpose. Arm of Thorim has a CATEGORY cooldown (Spell.dbc
// field 1 Category = 981, field 30 CategoryRecoveryTime = 20000 on every rank), and
// Player::AddSpellAndCategoryCooldowns (Player.cpp:11360, category block 11443-11491)
// stores an entry for every spell of that category in the same family, whether or not the
// player ever casts it. All nine ranks of the chain (`spell_ranks`
// first_spell_id 801847: 801847, 501433-501437, 567518-567520) therefore hold a live
// cooldown entry at once, and this walk sends up to nine SMSG_MODIFY_COOLDOWN
// (Player.cpp:11549-11562) or SMSG_CLEAR_COOLDOWN (Player.cpp:3754-3760) packets per
// Call Lightning / Electrocute cast while Arm of Thorim is recharging.
// Not filtered on what the player knows: Player::HasSpell (Player.cpp:4133-4137) does NOT
// test PlayerSpell::Active, the flag that marks a superseded lower rank (Player.h:131), so
// it would still answer true for the eight idle ranks and save nothing. Player::HasActiveSpell
// (Player.cpp:4145-4149) does test it, but nothing here has measured that the Ascension
// class spellbook keeps exactly one rank Active; getting that wrong would skip the rank
// really on cooldown and kill the talent in silence — the P-045 / P-051 / P-053 family.
void ReduceCooldownOverChain(Player* player, uint32 spellId, int32 delta)
{
    if (!player || !spellId || delta >= 0)
        return;

    uint32 reduction = uint32(-int64(delta));
    uint32 current = sSpellMgr->GetFirstSpellInChain(spellId);
    for (uint8 step = 0; current && step < MAX_SPELL_CHAIN_WALK; ++step)
    {
        // Same shape as the core's own ModifyAscensionCooldown: ModifySpellCooldown only
        // shifts the stored end time, with no floor, so a reduction at least as long as
        // what is left has to clear the entry instead of wrapping it.
        if (uint32 remaining = player->GetSpellCooldownDelay(current))
        {
            if (reduction >= remaining)
                player->RemoveSpellCooldown(current, true);
            else
                player->ModifySpellCooldown(current, delta);
        }
        current = sSpellMgr->GetNextSpellInChain(current);
    }
}

// 707546 "Enveloping Winds", first sentence: "Casting Gale now causes your Air Elemental
// to cast Gale." Its effect 0 is a SPELL_AURA_DUMMY (aura 4) whose EffectTriggerSpell
// (707547, the same record effect 1 points at) is never read by the dummy handler, so
// nothing in the data can express it. The second sentence (the raid armour aura) is
// already carried natively by
// effect 1, a SPELL_AURA_PERIODIC_TRIGGER_SPELL firing 707547 every 5 s, and is untouched.
class stormbringer_event_casts : public AllSpellScript
{
public:
    stormbringer_event_casts() : AllSpellScript("stormbringer_event_casts", {ALLSPELLHOOK_ON_CAST}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        // This hook runs for every cast in the world, so the cheapest scalar test is first.
        if (!spellInfo || spellInfo->SpellFamilyName != STORMBRINGER_SPELL_FAMILY)
            return;

        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || !spell || spell->IsTriggered() ||
            sSpellMgr->GetFirstSpellInChain(spellInfo->Id) != SPELL_GALE_RANK_1 ||
            !player->HasAura(SPELL_ENVELOPING_WINDS))
            return;

        Pet* pet = LiveAirElemental(player);
        if (!pet)
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target || target == pet || !target->IsInWorld() || pet->GetMap() != target->GetMap() ||
            !pet->IsValidAttackTarget(target))
            return;

        // The elemental repeats the very rank the player cast; the copy is triggered, so
        // it is neither a player cast nor a charge spend, and cannot re-enter this hook.
        pet->CastSpell(target, spellInfo->Id, true);
    }
};

// 705715 "Gift of Air", second sentence: "your critical strikes now extend the duration of
// your Tailwind by 1.5 sec". The talent's own spell_proc line is already spent on the first
// sentence (Kiss of the Clouds -> 804033 on the pet), and 583254, the record that carries
// the extension, is referenced by exactly two spells — 706537 "Gift of Air / Passive SLS"
// and 706123 "Cursed ID - Procs wont work on it", both as EffectTriggerSpell[0] of an
// aura 42 (full sweep of the 234 fields of all 209510 Spell.dbc records). Neither can ever
// fire it: their ProcFlags in the DBC are 0 and neither has a `spell_proc` row, so
// SpellMgr::LoadSpellProcs skips them ("Skip if no proc flags in DBC", SpellMgr.cpp:2248)
// — P-045. 706537 is besides absent from Talent.dbc and SkillLineAbility.dbc, so no player
// learns it. The proc is therefore hung on Tailwind itself, which also keeps it off every
// unit that is not buffed.
class aura_ascension_stormbringer_tailwind : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_tailwind);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({SPELL_TAILWIND_EXTENSION});
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        // Tailwind is a raid area aura: every allied application shares this same Aura
        // object, so the buffed allies reach this hook too. The owner is the one whose
        // application target is the aura's caster; this test is the only thing keeping an
        // ally's critical strike from extending someone else's Tailwind.
        return player && player->getClass() == CLASS_STORMBRINGER &&
            GetCasterGUID() == player->GetGUID() && eventInfo.GetActor() == player &&
            player->HasAura(SPELL_GIFT_OF_AIR);
    }

    void Extend(ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget();
        if (!owner || !owner->IsInWorld())
            return;

        // 583254 holds the authored amount and names the aura it lengthens; nothing is
        // restated here. It is READ, not cast, and this is the whole point:
        //   - its effect 0 carries EffectImplicitTargetB = 30 TARGET_UNIT_SRC_AREA_ALLY
        //     (SharedDefines.h:1527) with EffectRadiusIndex 10 (SpellRadius.dbc = 30 yd)
        //     and MaxAffectedTargets 25, so casting it picks every ally in 30 yards, not
        //     the caster alone — SpellInfo::GetMissingTargetMask (SpellInfo.cpp:630-651)
        //     clears TARGET_FLAG_UNIT_MASK as soon as a target type supplies a unit, and
        //     Spell::SelectEffectTypeImplicitTargets (Spell.cpp:2037-2040) then returns
        //     at once, so the explicit target is never the fallback;
        //   - Tailwind is an area aura, so every buffed ally hands
        //     Spell::EffectAscensionModifyAuraDuration (SpellEffects.cpp:464-482) the SAME
        //     Aura object (Unit::GetAura returns AuraApplication::GetBase(),
        //     Unit.cpp:6178-6182). One cast therefore added +1500 ms once PER ALLY: right
        //     solo, up to +37.5 s per critical strike in a 25-man raid, on a 15 s buff.
        // GetAura() is the very Tailwind that procced, and CheckProc has already confirmed
        // its caster is this player, so no other Stormbringer's Tailwind can be reached.
        Aura* tailwind = GetAura();
        SpellInfo const* extension = sSpellMgr->GetSpellInfo(SPELL_TAILWIND_EXTENSION);
        if (!tailwind || !extension)
            return;

        // The record must still be the one that names this aura, and a permanent aura has
        // no duration to lengthen (adding to -1 would give it a finite one).
        if (uint32(extension->Effects[EFFECT_0].MiscValue) != tailwind->GetId() || tailwind->IsPermanent())
            return;

        int32 bonus = extension->Effects[EFFECT_0].CalcValue(owner);
        if (bonus <= 0)
            return;

        tailwind->SetDuration(tailwind->GetDuration() + bonus);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_tailwind::CheckProc);
        OnProc += AuraProcFn(aura_ascension_stormbringer_tailwind::Extend);
    }
};

// 801869 "Titanstorm": "Casting Call Lightning or Electrocute now reduces the cooldown of
// Arm of Thorim and Lightning Cage by 1.5 sec." The 96 SpellFamilyFlags bits of family 22
// cannot separate Call Lightning (0,16,32) from Aeroblast (8388608,16,32), so the two
// chains are named here instead of in a family mask.
class aura_ascension_stormbringer_titanstorm : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_titanstorm);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({SPELL_TITANSTORM_REDUCTION});
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || eventInfo.GetActor() != player)
            return false;

        SpellInfo const* triggering = eventInfo.GetSpellInfo();
        if (!triggering || triggering->SpellFamilyName != STORMBRINGER_SPELL_FAMILY)
            return false;

        uint32 first = sSpellMgr->GetFirstSpellInChain(triggering->Id);
        return first == SPELL_CALL_LIGHTNING_RANK_1 || first == SPELL_ELECTROCUTE_RANK_1;
    }

    void Reduce(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        // 801854 would be cast here by the native handler. It is prevented because its
        // effect names Arm of Thorim rank 1 only; the amount and the target spells are
        // still read from that same record, so no number is restated.
        PreventDefaultAction();

        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        SpellInfo const* reduction = sSpellMgr->GetSpellInfo(SPELL_TITANSTORM_REDUCTION);
        if (!player || !reduction)
            return;

        for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
        {
            SpellEffectInfo const& effect = reduction->Effects[index];
            if (effect.Effect != SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN || effect.MiscValue <= 0)
                continue;

            ReduceCooldownOverChain(player, uint32(effect.MiscValue), effect.CalcValue(player));
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_titanstorm::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_stormbringer_titanstorm::Reduce,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};
} // namespace

void AddSC_AscensionStormbringerEvents()
{
    new stormbringer_event_casts();
    RegisterSpellScript(aura_ascension_stormbringer_tailwind);
    RegisterSpellScript(aura_ascension_stormbringer_titanstorm);
}
