/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionChronomancerTalents.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <vector>

namespace
{
enum ChronomancerTalentSpells : uint32
{
    SPELL_SHIMMERING_SHARD = 806302,
    SPELL_SHIMMER = 806303,
    SPELL_AEON_RENEWAL = 806290,
    SPELL_AEON_RESILIENCE = 806291,
    SPELL_AEON_PROTECTION = 806292,
    SPELL_AEON_OBLIVION = 806293,
    SPELL_DIMENSIONAL_DIVERGENCE = 802790,
    SPELL_DIVERGENCE_SLOW = 803301,
    SPELL_DIVERGENCE_SPEED = 803703,
    SPELL_INCARNATION_OF_CHAOS = 570067,
    SPELL_CHAOS_FUSION_COOLDOWN = 807755,
    SPELL_CHROMATIC_SHARD = 801292,
    SPELL_FRAY_MAGIC = 510236,
    SPELL_FRAYED = 510237,
    SPELL_DESTABILIZE_TIME = 680971,
    SPELL_DESTABILIZE_SLOW = 570761,
    SPELL_CHAOTIC_TIME = 583245,
    SPELL_REVERSE_WOUND_EXTENSION = 804679,
    SPELL_PARADOX = 680947,
    SPELL_CONVERGE_THE_INFINITE = 520840,
    SPELL_INFINITE_SHIELD = 520457,
    SPELL_CHAOS_INCARNATE_TRANSFORM = 560323,
    SPELL_PARADOX_CANNON = 806203,
    SPELL_SPLITTING_TIME = 706130,
    SPELL_CHRONOLOGICAL_CDR = 542728,
    SPELL_FABRIC_OF_TIME_BUFF = 706774,
    SPELL_TIMEWALKING = 572527
};

// Reverse Wound, the nine rows (801303 chain). Listed so the contract does not depend on spell_ranks.
constexpr uint32 ReverseWoundRanks[] = {801303, 501805, 501806, 501807, 501808, 501809, 501810, 501811, 572628};

// Chromatic Shard's category (888 in Spell.dbc) holds its nine rows and nothing else.
uint32 ChromaticShardCategory()
{
    SpellInfo const* shard = sSpellMgr->GetSpellInfo(SPELL_CHROMATIC_SHARD);
    return shard ? shard->GetCategory() : 0;
}

// Destabilize Time: "increasing by $570761s1% every time they cast. Gaining stacks does not
// extend the duration of the spell." The slow is kept on the mark's remaining duration.
void SyncDestabilize(Aura* slow, Aura const* mark)
{
    if (!slow || !mark || mark->GetDuration() <= 0)
        return;
    slow->SetMaxDuration(mark->GetMaxDuration());
    slow->SetDuration(mark->GetDuration());
}

void AddDestabilizeStacks(Unit* target)
{
    std::vector<ObjectGuid> casters;
    auto range = target->GetAppliedAuras().equal_range(SPELL_DESTABILIZE_TIME);
    for (auto itr = range.first; itr != range.second; ++itr)
        casters.push_back(itr->second->GetBase()->GetCasterGUID());
    for (ObjectGuid const& guid : casters)
    {
        Aura* mark = target->GetAura(SPELL_DESTABILIZE_TIME, guid);
        Unit* caster = mark ? mark->GetCaster() : nullptr;
        if (!caster || !caster->IsInWorld() || caster->GetMap() != target->GetMap())
            continue;
        Aura* slow = target->GetAura(SPELL_DESTABILIZE_SLOW, guid);
        if (slow)
            slow->ModStackAmount(1);
        else
            slow = caster->AddAura(SPELL_DESTABILIZE_SLOW, target);
        SyncDestabilize(slow, mark);
    }
}

bool IsAeonActivation(uint32 id)
{
    return id == SPELL_AEON_RENEWAL || id == SPELL_AEON_RESILIENCE ||
        id == SPELL_AEON_PROTECTION || id == SPELL_AEON_OBLIVION;
}

bool CanSwapPlayers(Player* player, Player* target)
{
    if (!player || !target || player == target || player->getClass() != CLASS_CHRONOMANCER ||
        !player->IsAlive() || !target->IsAlive() || !player->IsInWorld() || !target->IsInWorld() ||
        player->GetMap() != target->GetMap() || !player->InSamePhase(target))
        return false;
    if (player->IsBeingTeleported() || target->IsBeingTeleported() || player->IsInFlight() || target->IsInFlight() ||
        player->GetTransport() || target->GetTransport() || player->GetVehicle() || target->GetVehicle())
        return false;
    return player->IsWithinLOSInMap(target) &&
        (player->IsValidAttackTarget(target) || player->IsValidAssistTarget(target));
}

class spell_ascension_dimensional_divergence : public SpellScript
{
    PrepareSpellScript(spell_ascension_dimensional_divergence);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_DIVERGENCE_SLOW, SPELL_DIVERGENCE_SPEED});
    }

    SpellCastResult CheckSwap()
    {
        Unit* target = GetExplTargetUnit();
        return CanSwapPlayers(GetCaster()->ToPlayer(), target ? target->ToPlayer() : nullptr)
            ? SPELL_CAST_OK : SPELL_FAILED_BAD_TARGETS;
    }

    void Swap(SpellEffIndex)
    {
        Player* player = GetCaster()->ToPlayer();
        Unit* hit = GetHitUnit();
        Player* target = hit ? hit->ToPlayer() : nullptr;
        if (!CanSwapPlayers(player, target))
            return;
        // Snapshot both live positions before either native teleport changes one.
        Position origin = player->GetPosition();
        Position destination = target->GetPosition();
        bool hostile = player->IsValidAttackTarget(target);
        target->NearTeleportTo(origin);
        player->NearTeleportTo(destination, true);
        if (hostile)
        {
            player->CastSpell(target, SPELL_DIVERGENCE_SLOW, true);
            if (target->HasAura(SPELL_DIVERGENCE_SLOW, player->GetGUID()))
                player->CastSpell(player, SPELL_DIVERGENCE_SPEED, true);
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_dimensional_divergence::CheckSwap);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_dimensional_divergence::Swap, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Destabilize Time's own aura: effect 0 is an aura 42 with no trigger (made DUMMY by contract).
// The real slow, 570761, is posed by nothing in the data.
class aura_ascension_destabilize_time : public AuraScript
{
    PrepareAuraScript(aura_ascension_destabilize_time);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_DESTABILIZE_SLOW}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !target->IsAlive())
            return;
        Aura* slow = target->GetAura(SPELL_DESTABILIZE_SLOW, caster->GetGUID());
        if (!slow)
            slow = caster->AddAura(SPELL_DESTABILIZE_SLOW, target);
        SyncDestabilize(slow, GetAura());
    }

    void Clear(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_DESTABILIZE_SLOW, GetCasterGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_destabilize_time::Apply,
            EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_destabilize_time::Clear,
            EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Incarnation of Chaos: "allowing you to cast it while moving". The DBC gives that to the hidden
// 560323 (aura 313 on Chromatic Shard, flags1 0x1000), which nothing poses. It follows 570067 here:
// applied, refreshed and reloaded at login through OnAuraApply, removed with it in any way.
class chronomancer_incarnation_transform : public UnitScript
{
public:
    chronomancer_incarnation_transform() : UnitScript("chronomancer_incarnation_transform", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || aura->GetId() != SPELL_INCARNATION_OF_CHAOS || aura->GetOwner() != unit ||
            aura->GetCasterGUID() != unit->GetGUID() || !unit->IsPlayer() ||
            unit->getClass() != CLASS_CHRONOMANCER || !unit->IsAlive())
            return;
        Aura* transform = unit->AddAura(SPELL_CHAOS_INCARNATE_TRANSFORM, unit);
        if (!transform)
            transform = unit->GetAura(SPELL_CHAOS_INCARNATE_TRANSFORM, unit->GetGUID());
        // Durations are final here: Chaos Fusion's SPELLMOD_DURATION was applied at creation.
        if (transform && aura->GetDuration() > 0)
        {
            transform->SetMaxDuration(aura->GetMaxDuration());
            transform->SetDuration(aura->GetDuration());
        }
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode) override
    {
        if (unit && application && application->GetBase()->GetId() == SPELL_INCARNATION_OF_CHAOS &&
            unit->IsPlayer() && unit->getClass() == CLASS_CHRONOMANCER)
            unit->RemoveAurasDueToSpell(SPELL_CHAOS_INCARNATE_TRANSFORM, unit->GetGUID());
    }
};

class chronomancer_talent_casts : public AllSpellScript
{
public:
    chronomancer_talent_casts() : AllSpellScript("chronomancer_talent_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_SUCCESSFUL_INTERRUPT}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        if (!caster || spell->IsTriggered())
            return;
        // Any caster, player or creature, marked by a Chronomancer's Destabilize Time.
        if (caster->HasAura(SPELL_DESTABILIZE_TIME))
            AddDestabilizeStacks(caster);
        Player* player = caster->ToPlayer();
        if (!player || player->getClass() != CLASS_CHRONOMANCER || info->SpellFamilyName != 28)
            return;
        if (IsAeonActivation(info->Id) && player->HasAura(SPELL_SHIMMERING_SHARD))
            player->CastSpell(player, SPELL_SHIMMER, true);
        // Incarnation of Chaos: "Chromatic Shard incurs no cooldown". Its effects only carry value-0
        // modifiers for Melt Reality; Chromatic Shard's cooldown is the category one.
        // SendSpellCooldown (Spell.cpp:3983) has already run.
        uint32 shardCategory = ChromaticShardCategory();
        if (info->Id == SPELL_INCARNATION_OF_CHAOS)
        {
            if (shardCategory)
                player->RemoveCategoryCooldown(shardCategory);
            // Chaos Fusion: its 807755 (Melt Reality without cooldown) keeps DurationIndex 8 while
            // the talent lengthens Incarnation; both were applied before this callback.
            if (Aura* noCooldown = player->GetAura(SPELL_CHAOS_FUSION_COOLDOWN, player->GetGUID()))
                if (Aura const* incarnation = player->GetAura(SPELL_INCARNATION_OF_CHAOS, player->GetGUID()))
                    if (incarnation->GetDuration() > 0)
                    {
                        noCooldown->SetMaxDuration(incarnation->GetMaxDuration());
                        noCooldown->SetDuration(incarnation->GetDuration());
                    }
        }
        else if (shardCategory && info->GetCategory() == shardCategory && player->HasAura(SPELL_INCARNATION_OF_CHAOS))
            player->RemoveCategoryCooldown(shardCategory);
    }

    // Fray Magic: "Successfully interrupting an enemy will additionally silence them". The core
    // calls this only when the interrupt landed (SpellEffects.cpp:4115).
    void OnSpellSuccessfulInterrupt(Spell* spell, Unit* target) override
    {
        Player* player = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (player && player->getClass() == CLASS_CHRONOMANCER && target && target->IsAlive() &&
            sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id) == SPELL_FRAY_MAGIC)
            player->CastSpell(target, SPELL_FRAYED, true);
    }
};
}

void ApplyAscensionChronomancerTalentContracts(SpellInfo* info)
{
    if (info->SpellFamilyName != 28)
        return;
    if (info->Id == SPELL_DIMENSIONAL_DIVERGENCE)
    {
        // The copied client-destination and caster-front teleports do not swap
        // players. The effect-0 script uses authoritative live server positions.
        info->Effects[EFFECT_1].Effect = 0;
        info->Effects[EFFECT_2].Effect = 0;
        info->_InitializeExplicitTargetMask();
    }
    if (info->Id == SPELL_CHAOTIC_TIME)
    {
        // "While Incarnation of Chaos is active, your Melt Reality deals $s1% increased damage":
        // effect 0 targets 504727/520458 (flags2 0x400000) permanently. Pointed at Incarnation
        // (flags2 0x2000, like effect 1), it raises Incarnation's value-0 DOT modifier on Melt.
        SpellEffectInfo& damage = info->Effects[EFFECT_0];
        if (damage.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER && damage.MiscValue == SPELLMOD_EFFECT1)
            damage.SpellClassMask = flag96(0, 0, 0x2000);
    }
    for (uint32 rank : ReverseWoundRanks)
        if (info->Id == rank && info->Effects[EFFECT_1].Effect == SPELL_EFFECT_TRIGGER_SPELL &&
            info->Effects[EFFECT_1].TriggerSpell == SPELL_REVERSE_WOUND_EXTENSION)
        {
            // The tooltip grants the unbounded +2 s (804679) only with Saviour 706109, which nobody
            // can obtain. Fortify keeps its own, capped, extension.
            info->Effects[EFFECT_1].Effect = 0;
            info->_InitializeExplicitTargetMask();
        }
    if (info->Id == SPELL_DESTABILIZE_TIME)
    {
        info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[EFFECT_0].TriggerSpell = 0;
    }
    // "your next Unmake or Reverse Wound" / "your next Correct the Mistake" / Chronological "your next
    // Discordance ... is instant cast" / Fabric of Time "Your next Aeon ... with no cooldown or cost":
    // one charge, spent by the first affected cast (Spell.cpp:4068, RemoveSpellMods).
    if ((info->Id == SPELL_PARADOX || info->Id == SPELL_CONVERGE_THE_INFINITE ||
         info->Id == SPELL_CHRONOLOGICAL_CDR || info->Id == SPELL_FABRIC_OF_TIME_BUFF) && !info->ProcCharges)
        info->ProcCharges = 1;
    // Timewalking: "Reduces the cooldown of Waves of Time" (flags1 0x8). Its mask also names flags0
    // 0x1000, Wand of Time rank 1 alone, whose 5 s cooldown fell to 1 s.
    if (info->Id == SPELL_TIMEWALKING && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
        info->Effects[EFFECT_0].MiscValue == SPELLMOD_COOLDOWN)
        info->Effects[EFFECT_0].SpellClassMask[0] &= ~0x1000u;
    if (info->Id == SPELL_INFINITE_SHIELD)
        // "can only affect 1 target at a time"
        info->AttributesEx5 |= SPELL_ATTR5_LIMIT_N;
    if (info->Id == SPELL_CHAOS_INCARNATE_TRANSFORM)
    {
        // Effect 0 turns the player into creature 650251, absent from creature_template (the core
        // falls back to model 16358). Keep the scale (value 0) and the cast-while-moving aura 313.
        // Never saved: chronomancer_incarnation_transform poses it again from 570067 at login.
        if (info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_TRANSFORM)
        {
            info->Effects[EFFECT_0].Effect = 0;
            info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_NONE;
            info->_InitializeExplicitTargetMask();
        }
        info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
        info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
    }
    if (info->Id == SPELL_PARADOX_CANNON)
    {
        // Conduction Burst (SPELLMOD_EFFECT2 +20 on the four Continuum, flags2 0x8): "+20% Shatter Echo
        // damage while a Continuum is active". Flux Emitter, Aether Compression and Singularity Core
        // carry that value-0 Shatter Echo modifier in effect 1; Paradox Cannon carries Wand Mastery's
        // chance there instead, which the talent raised from +10 to +30. The chance moves to effect 2,
        // which no modifier touches, and effect 1 becomes the Flux Emitter modifier.
        SpellEffectInfo& chance = info->Effects[EFFECT_1];
        SpellEffectInfo& moved = info->Effects[EFFECT_2];
        if (!moved.Effect && chance.Effect == SPELL_EFFECT_APPLY_AURA &&
            chance.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER && chance.MiscValue == SPELLMOD_CHANCE_OF_SUCCESS)
        {
            auto conditions = moved.ImplicitTargetConditions;
            moved = chance;
            moved.EffectIndex = EFFECT_2;
            moved.ImplicitTargetConditions = conditions;
            chance.ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
            chance.MiscValue = SPELLMOD_DAMAGE;
            chance.BasePoints = -1;
            chance.DieSides = 1;
            chance.RealPointsPerLevel = 0.0f;
            chance.SpellClassMask = flag96(0, 0, 0x10);
            chance.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
            chance.TargetB = SpellImplicitTargetInfo(0);
            info->_InitializeExplicitTargetMask();
        }
    }
    if (info->Id == SPELL_SPLITTING_TIME && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
        info->Effects[EFFECT_0].MiscValue == SPELLMOD_JUMP_TARGETS && info->Effects[EFFECT_0].BasePoints == 1)
        // "Crystal Cannon now strikes $s1 additional nearby enemies": JUMP_TARGETS counts every target
        // and 560134 has ChainTarget 0, so +2 gave one extra enemy. +3 gives the two promised.
        info->Effects[EFFECT_0].BasePoints = 2;
    if (info->Id != SPELL_SHIMMER)
        return;
    // The talent promises the same percentage for damage and healing. Retain
    // the native stack cap and duration, and match healing to the displayed amount.
    info->Effects[EFFECT_1].BasePoints = info->Effects[EFFECT_0].BasePoints;
    info->Effects[EFFECT_1].DieSides = info->Effects[EFFECT_0].DieSides;
}

void AddSC_AscensionChronomancerTalents()
{
    new chronomancer_talent_casts();
    new chronomancer_incarnation_transform();
    RegisterSpellScript(spell_ascension_dimensional_divergence);
    RegisterSpellScript(aura_ascension_destabilize_time);
}
