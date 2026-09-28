/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "CellImpl.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
enum TimeSpells : uint32
{
    Epoch = 801270,
    Recovery = 800857,
    RenewalAeon = 806290,
    ResilienceAeon = 806291,
    ProtectionAeon = 806292,
    OblivionAeon = 806293,
    Renewal = 560355,
    Protection = 560374,
    Oblivion = 583921,
    ResilienceSpread = 560373,
    Sands = 804488,
    EndlessSandsTalent = 806338,
    EndlessSands = 806728,
    KeepAccelerating = 520042,
    KeepAcceleratingSpread = 520118,
    CadenceTalent = 560397,
    Cadence = 560398,
    OrderlyTalent = 806162,
    Orderly = 704482,
    TimelineTether = 804505,
    TetherCooldown = 804506,
    Fortify = 804491,
    Chronicler = 706134,
    EpicRecovery = 706098,
    EpicRecoveryTargets = 542725,
    Accelerate = 572633,
    Decelerate = 572632,
    TimeOut = 802229,
    TimeOutRankTwo = 803896,
    TimeOutRankThree = 803897,
    TimeOutStasis = 802228,
    TitansGaze = 804462,
    NozdormusGaze = 807691,
    NozdormusEnergize = 504838,
    ReverseWound = 801303,
    FabricOfTime = 806299,
    OrderlySls = 707602,
    InfinitePower = 92118,
    InfinitePowerValue = 528312,
    TemporalConvergence = 807588,
    AeonRend = 583248,
    TimeIsACircle = 806206,
    TimeIsACircleExtension = 524962,
    CheatingTime = 524945,
    CheatingTimeExtension = 524944,
    RollBack = 804490,
    RollBackBuff = 807771,
    Unmake = 804418,
    InfiniteKeeper = 806312,
    InfiniteKeeperTrigger = 806314,
    AnomalySpike = 503826,
    ChromaticShardStack = 503829,
    BiekoCooldownReduction = 707312,
    WavesOfTimeKnockback = 802600,
    CorrectTheMistake = 572352,
    ConvergeTheInfinite = 520840,
    ConvergeCooldownReduction = 520931,
    EchoFragments = 92120,
    BuyTimeStasis = 520186,
    BuyTimeRemover = 807310,
    SingularityCore = 804438,
    EchoFragment = 804455,
    Slipstream = 707555,
    SlipstreamPlace = 707566
};

constexpr uint32 ChronomancerFamily = 28;
// Effect 192 names a spell and a percentage of its remaining cooldown; the core maps it to EffectNULL.
constexpr uint32 AscensionReduceCooldownPctEffect = 192;
// Family 28 flags, each exclusive to one spell in Spell.dbc (all ranks, chained or not).
flag96 const WandOfTimeFlags(0, 0x100000, 0);
flag96 const ShatterEchoFlags(0, 0, 0x14);
flag96 const ChromaticShardFlags(0, 0, 0x2000000);
flag96 const TimerendFlags(0, 0x1000000, 0);
// Infinite Power: Timerend, Melt Reality and Chromatic Shard.
flag96 const InfinitePowerTargets(0, 0x1000000 | 0x200, 0x2000000);
// The nine Chromatic Shard rows; rank 9 (567544) is outside spell_ranks.
constexpr uint32 ChromaticShardRanks[] = {801292, 501836, 501837, 501838, 501839, 501840, 574346, 567543, 567544};
// Artificer's Wand, five rows outside spell_ranks; flags2 0x400 is shared with Clocked In.
constexpr uint32 ArtificersWandRanks[] = {561284, 561354, 561355, 561356, 561357};
// Slipstream: "choose a new location to place that enemy within 20 yds" (text only, no data field).
constexpr float SlipstreamPlaceRange = 20.0f;

Player* Chronomancer(Unit* caster)
{
    Player* player = caster ? caster->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_CHRONOMANCER ? player : nullptr;
}

bool IsRank(uint32 spell, uint32 root)
{
    return sSpellMgr->GetFirstSpellInChain(spell) == root;
}

int32 Amount(uint32 spell, uint8 index = EFFECT_0, Unit const* caster = nullptr)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    return info ? info->Effects[index].CalcValue(caster) : 0;
}

bool HasFamilyFlags(SpellInfo const* info, flag96 const& flags)
{
    return info && info->SpellFamilyName == ChronomancerFamily && (info->SpellFamilyFlags & flags);
}

// The talent's DBC ProcChance, through the modifiers other talents put on it.
bool RollTalentChance(Player* player, uint32 talent)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(talent);
    if (!info)
        return false;
    int32 chance = int32(info->ProcChance);
    player->ApplySpellMod(talent, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    return chance > 0 && roll_chance_i(chance);
}

// Spell and category cooldown, cleared on the client too. Only used for Wand of Time
// (category 292) and Chromatic Shard (888), whose categories hold nothing else.
void ResetCooldown(Player* player, SpellInfo const* info)
{
    player->RemoveSpellCooldown(info->Id, true);
    if (uint32 category = info->GetCategory())
        player->RemoveCategoryCooldown(category);
}

// "reduces ... by X% of their remaining cooldown"; a cut that leaves nothing clears it.
void ReduceRemainingCooldown(Player* player, uint32 spell, int32 percent)
{
    uint32 remaining = player->GetSpellCooldownDelay(spell);
    if (!remaining || percent <= 0)
        return;
    uint64 cut = uint64(remaining) * uint64(std::min(percent, 100)) / 100;
    if (cut >= remaining)
        player->RemoveSpellCooldown(spell, true);
    else if (cut)
        player->ModifySpellCooldown(spell, -int32(cut));
}

// "cannot be extended beyond their maximum duration"
void ExtendOwnAura(Player* player, uint32 spell, int32 extension)
{
    Aura* aura = player->GetAura(spell, player->GetGUID());
    if (!aura || extension <= 0 || aura->GetMaxDuration() <= 0)
        return;
    aura->SetDuration(std::min(aura->GetDuration() + extension, aura->GetMaxDuration()));
}

bool DealsDamage(SpellInfo const* info)
{
    if (info->IsPositive())
        return false;
    for (SpellEffectInfo const& effect : info->Effects)
        switch (effect.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_HEALTH_LEECH:
                return true;
            case SPELL_EFFECT_APPLY_AURA:
                if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                    effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH ||
                    effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
                    return true;
                break;
            default:
                break;
        }
    return false;
}

// Infinite Power (92118): "Casting damaging spells now reduces your damaging spell cooldowns by
// $528312s1% of their remaining cooldown." Its trigger 503946 carries effect 192 (EffectNULL)
// and names rank 1 only, while Timerend has no rank chain: the targets are read by family flags.
void ApplyInfinitePower(Player* player, SpellInfo const* cast)
{
    if (!player->HasAura(InfinitePower) || !DealsDamage(cast))
        return;
    int32 percent = Amount(InfinitePowerValue, EFFECT_0, player);
    uint32 category = cast->GetCategory();
    for (auto const& [id, state] : player->GetSpellMap())
    {
        if (id == cast->Id || !player->HasSpell(id))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        // The ranks sharing the cast spell's category carry its own, just started, cooldown.
        if (!HasFamilyFlags(info, InfinitePowerTargets) || (category && info->GetCategory() == category))
            continue;
        ReduceRemainingCooldown(player, id, percent);
    }
}

bool HasOwnTimerend(Unit* target, ObjectGuid caster)
{
    for (auto const& [id, application] : target->GetAppliedAuras())
        if (application->GetBase()->GetCasterGUID() == caster &&
            HasFamilyFlags(application->GetBase()->GetSpellInfo(), TimerendFlags))
            return true;
    return false;
}

bool KnowsChromaticShard(Player* player)
{
    for (uint32 id : ChromaticShardRanks)
        if (player->HasSpell(id))
            return true;
    return false;
}

std::list<Unit*> Nearby(Player* player, Unit* center, uint32 helper, bool friendly)
{
    std::list<Unit*> targets;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(helper);
    if (!info || !center || !center->IsInWorld())
        return targets;
    float radius = info->Effects[EFFECT_0].CalcRadius(player);
    Acore::AnyUnitInObjectRangeCheck check(center, radius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> search(center, targets, check);
    Cell::VisitObjects(center, search, radius);
    targets.remove_if([player, center, friendly](Unit* target)
    {
        return target == center || !target->IsAlive() || !target->InSamePhase(player) ||
            !center->IsWithinLOSInMap(target) || !player->IsWithinLOSInMap(target) ||
            !(friendly ? player->IsValidAssistTarget(target) : player->IsValidAttackTarget(target));
    });
    targets.sort([center](Unit* first, Unit* second)
    {
        float left = center->GetDistance(first), right = center->GetDistance(second);
        return left == right ? first->GetGUID() < second->GetGUID() : left < right;
    });
    return targets;
}

void SpreadRecovery(Player* player, Unit* source, uint32 helper)
{
    Aura* original = source->GetAuraOfRankedSpell(Recovery, player->GetGUID());
    if (!original)
        return;
    uint32 spell = original->GetId();
    int32 remaining = original->GetDuration();
    for (Unit* target : Nearby(player, source, helper, true))
    {
        if (target->GetAuraOfRankedSpell(Recovery, player->GetGUID()))
            continue;
        if (player->CastSpell(target, spell, true) != SPELL_CAST_OK)
            continue;
        if (Aura* copy = target->GetAura(spell, player->GetGUID()))
            copy->SetDuration(std::min(copy->GetDuration(), remaining));
        break;
    }
}

void ExtendRecovery(Player* player, Unit* target)
{
    Aura* recovery = target->GetAuraOfRankedSpell(Recovery, player->GetGUID());
    if (!recovery)
        return;
    int32 extension = Amount(Fortify, EFFECT_1);
    int32 limit = Amount(Fortify, EFFECT_2);
    if (player->HasAura(Chronicler))
    {
        extension += Amount(Chronicler, EFFECT_0);
        limit += Amount(Chronicler, EFFECT_1);
    }
    int32 used = int32(std::min<uint64>(recovery->GetScriptValue(Fortify), INT32_MAX));
    extension = std::clamp(extension, 0, std::max(0, limit - used));
    if (extension)
    {
        recovery->SetScriptValue(Fortify, used + extension);
        recovery->SetMaxDuration(recovery->GetMaxDuration() + extension);
        recovery->SetDuration(recovery->GetDuration() + extension);
    }
}

void ApplyEpochAeon(Player* player, Unit* target, uint32 healing)
{
    if (player->HasAura(ResilienceAeon))
        SpreadRecovery(player, target, ResilienceSpread);
    if (!healing)
        return;
    if (player->HasAura(RenewalAeon))
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(Renewal);
        if (!info)
            return;
        // Time Beacon lengthens the aura natively (SPELLMOD_DURATION); the tick is sized on the
        // base duration so the extra seconds add ticks instead of spreading the same total.
        int32 duration = player->CalcSpellDuration(info);
        uint32 ticks = std::max(1, duration / int32(std::max(1u, info->Effects[EFFECT_0].Amplitude)));
        int32 tick = int32(std::min<uint64>(uint64(healing) * 75 / 100 / ticks, INT32_MAX));
        player->CastCustomSpell(Renewal, SPELLVALUE_BASE_POINT0, tick, target, true);
    }
    else if (player->HasAura(ProtectionAeon))
        player->CastCustomSpell(Protection, SPELLVALUE_BASE_POINT0,
            int32(std::min<uint64>(uint64(healing) * Amount(ProtectionAeon) / 100, INT32_MAX)), target, true);
    else if (player->HasAura(OblivionAeon))
    {
        auto enemies = Nearby(player, target, Oblivion, false);
        if (!enemies.empty())
            player->CastCustomSpell(Oblivion, SPELLVALUE_BASE_POINT0,
                int32(std::min<uint32>(healing, INT32_MAX)), enemies.front(), true);
    }
}

void CastEpicRecovery(Player* player, Unit* primary)
{
    if (!player->HasAura(EpicRecovery))
        return;
    SpellInfo const* highest = nullptr;
    for (auto const& [id, state] : player->GetSpellMap())
        if (player->HasSpell(id) && IsRank(id, Epoch))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                if (!highest || info->GetRank() > highest->GetRank())
                    highest = info;
    SpellInfo const* helper = sSpellMgr->GetSpellInfo(EpicRecoveryTargets);
    if (!highest || !helper)
        return;
    uint32 count = 0;
    for (Unit* target : Nearby(player, primary, EpicRecoveryTargets, true))
    {
        if (!target->GetAuraOfRankedSpell(Recovery, player->GetGUID()))
            continue;
        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        Spell* echo = new Spell(player, highest, TRIGGERED_FULL_MASK);
        echo->SetScriptValue(EpicRecovery, Amount(EpicRecovery));
        if (echo->prepare(&targets) == SPELL_CAST_OK && ++count >= helper->MaxAffectedTargets)
            break;
    }
}

class chronomancer_time_casts : public AllSpellScript
{
public:
    chronomancer_time_casts() : AllSpellScript("chronomancer_time_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT, ALLSPELLHOOK_ON_CALCULATED_TARGET,
         ALLSPELLHOOK_ON_CRIT_CHANCE}) { }

    // Wand of Time: "guaranteed to critically strike against Undead and Demons". Target-side
    // reductions (SpellTakenCritChance) still apply afterwards, as for Exorcism.
    void OnSpellCritChance(Spell* spell, Unit* target, float& chance) override
    {
        if (target && Chronomancer(spell->GetCaster()) && HasFamilyFlags(spell->GetSpellInfo(), WandOfTimeFlags) &&
            (target->GetCreatureTypeMask() & CREATURE_TYPEMASK_DEMON_OR_UNDEAD))
            chance = 100.0f;
    }

    void OnSpellCalculatedTarget(Spell* spell, Unit*, TargetInfo& target) override
    {
        if (uint64 percent = spell->GetScriptValue(EpicRecovery); percent &&
            Chronomancer(spell->GetCaster()) && IsRank(spell->GetSpellInfo()->Id, Epoch))
        {
            target.damage = int32(int64(target.damage) * int64(std::min<uint64>(percent, 100)) / 100);
            target.damageBeforeTakenMods =
                int32(int64(target.damageBeforeTakenMods) * int64(std::min<uint64>(percent, 100)) / 100);
        }
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = Chronomancer(caster);
        if (!player || spell->IsTriggered())
            return;
        // Artificer's Wand: "Generates 1 Echo Fragment" (Clocked In adds its own through spell_proc).
        // Same requirement as the Wand of Time gain: the spec 33 passive Echo Fragments 92120.
        if (std::find(std::begin(ArtificersWandRanks), std::end(ArtificersWandRanks), info->Id) !=
            std::end(ArtificersWandRanks) && player->HasAura(EchoFragments))
            player->CastSpell(player, EchoFragment, true);
        ApplyInfinitePower(player, info);
        // Endless Sands: "Cast time of your next Reverse Wounds ... is reduced". The cast time was
        // fixed at prepare, so this cast keeps its reduction.
        if (IsRank(info->Id, ReverseWound))
            player->RemoveAurasDueToSpell(EndlessSands, player->GetGUID());
        // Nozdormu's Gaze: "casting it now instantly restores an additional $504838s1% of your maximum mana".
        if (IsRank(info->Id, TimeOut) && player->HasAura(NozdormusGaze))
            player->CastSpell(player, NozdormusEnergize, true);
        // Orderly Protector: 707602 (never granted) is the designed carrier of the per-stack
        // CHANCE_OF_SUCCESS of 704482; read it here, after SendSpellCooldown (Spell.cpp:3983).
        if (IsRank(info->Id, FabricOfTime) && player->HasAura(Orderly))
        {
            float chance = 0.0f;
            player->ApplySpellMod(OrderlySls, SPELLMOD_CHANCE_OF_SUCCESS, chance);
            if (chance > 0.0f && roll_chance_f(chance))
                player->RemoveSpellCooldown(info->Id, true);
        }
        if (HasFamilyFlags(info, WandOfTimeFlags))
        {
            // Temporal Convergence: "a $h% chance to reset its own cooldown". 807756 names rank 1 only.
            if (player->HasAura(TemporalConvergence) && RollTalentChance(player, TemporalConvergence))
                ResetCooldown(player, info);
            // Time Is A Circle: 524962 lists the Continuum auras and the extension. Its three effects
            // name Paradox Cannon, Flux Emitter and Aether Compression; the fourth Continuum, Singularity
            // Core, does not fit and takes effect 0's extension. One Continuum at a time (spell_group).
            if (player->HasAura(TimeIsACircle))
                if (SpellInfo const* extension = sSpellMgr->GetSpellInfo(TimeIsACircleExtension))
                {
                    for (SpellEffectInfo const& effect : extension->Effects)
                        if (effect.Effect == SPELL_EFFECT_ASCENSION_MODIFY_AURA_DURATION && effect.MiscValue > 0)
                            ExtendOwnAura(player, uint32(effect.MiscValue), effect.CalcValue(player));
                    if (extension->Effects[EFFECT_0].Effect == SPELL_EFFECT_ASCENSION_MODIFY_AURA_DURATION)
                        ExtendOwnAura(player, SingularityCore, extension->Effects[EFFECT_0].CalcValue(player));
                }
        }
        // Cheating Time: Shatter Echo extends Flux Emitter, within its maximum duration.
        if (HasFamilyFlags(info, ShatterEchoFlags) && player->HasAura(CheatingTime))
            if (SpellInfo const* extension = sSpellMgr->GetSpellInfo(CheatingTimeExtension))
                if (extension->Effects[EFFECT_0].MiscValue > 0)
                    ExtendOwnAura(player, uint32(extension->Effects[EFFECT_0].MiscValue),
                        extension->Effects[EFFECT_0].CalcValue(player));
        // Aeon Rend: "a $h% chance to strike again and incur no cooldown". The DBC recast names rank 1.
        if (HasFamilyFlags(info, ChromaticShardFlags) && player->HasAura(AeonRend) &&
            RollTalentChance(player, AeonRend))
        {
            ResetCooldown(player, info);
            if (Unit* target = spell->m_targets.GetUnitTarget();
                target && target != player && target->IsAlive() && target->IsInWorld() &&
                player->IsValidAttackTarget(target))
                player->CastSpell(target, info->Id, true);
        }
        // Buy Time: "Casting Unmake on a target removes this effect". 807310, the designed remover
        // (REMOVE_AURA 520186, ignores immunities), is cast by nothing in the data. Allies cannot be
        // targeted by Unmake, so only the enemy half (520186) is reachable.
        if (IsRank(info->Id, Unmake))
            if (Unit* target = spell->m_targets.GetUnitTarget();
                target && target != player && target->IsInWorld() && target->HasAura(BuyTimeStasis, player->GetGUID()))
                player->CastSpell(target, BuyTimeRemover, true);
        for (uint32 aeon : {RenewalAeon, ResilienceAeon, ProtectionAeon, OblivionAeon})
            if (info->Id == aeon)
                for (uint32 other : {RenewalAeon, ResilienceAeon, ProtectionAeon, OblivionAeon})
                    if (other != aeon)
                        player->RemoveAurasDueToSpell(other);
        if (!IsRank(info->Id, Epoch))
            return;
        Aura* sands = player->GetAura(Sands);
        if (sands && sands->GetStackAmount() >= 5)
            sands->Remove();
        else
            player->CastSpell(player, Sands, true);
        if (player->HasAura(EndlessSandsTalent))
            player->CastSpell(player, EndlessSands, true);
        if (Unit* target = spell->m_targets.GetUnitTarget())
            CastEpicRecovery(player, target);
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32 healing, bool critical) override
    {
        Player* player = Chronomancer(spell->GetCaster());
        if (!player || !target || miss != SPELL_MISS_NONE)
            return;
        uint32 id = spell->GetSpellInfo()->Id;
        // Chromatic Shard: "Dealing damage with Anomaly Spikes reduces the cast time of this ability".
        // 503829 is carried only by two hidden auras nothing grants. The spike is always triggered.
        if (id == AnomalySpike)
        {
            if (damage && KnowsChromaticShard(player))
                player->CastSpell(player, ChromaticShardStack, true);
            return;
        }
        // Infinite Keeper: Unmake on an enemy affected by your Timerend (7 unchained rows).
        if (IsRank(id, Unmake) && !spell->IsTriggered() && target != player && target->IsAlive() &&
            player->HasAura(InfiniteKeeper) && player->IsValidAttackTarget(target) &&
            HasOwnTimerend(target, player->GetGUID()))
            player->CastSpell(target, InfiniteKeeperTrigger, true);
        if (IsRank(id, Recovery))
        {
            if (Aura* aura = target->GetAuraOfRankedSpell(Recovery, player->GetGUID()))
                aura->SetScriptValue(Fortify, 0);
            if (!spell->IsTriggered() && player->HasAura(KeepAccelerating))
                SpreadRecovery(player, target, KeepAcceleratingSpread);
        }
        else if (IsRank(id, Fortify))
            ExtendRecovery(player, target);
        else if (IsRank(id, CorrectTheMistake))
        {
            // Converge the Infinite: "Successfully healing an ally in this way reduces the cooldown of
            // Fortify Timeline by $520931s1%" (520931 is effect 192, EffectNULL). Read here: the charge
            // of 520840 is spent by RemoveSpellMods after the hits (Spell.cpp:4599), before OnSpellCast.
            // The spell marker keeps it to one reduction per cast when several allies are healed.
            if (!spell->IsTriggered() && healing && !spell->GetScriptValue(ConvergeCooldownReduction) &&
                player->HasAura(ConvergeTheInfinite, player->GetGUID()))
            {
                spell->SetScriptValue(ConvergeCooldownReduction, 1);
                int32 percent = Amount(ConvergeCooldownReduction, EFFECT_0, player);
                for (auto const& [sid, state] : player->GetSpellMap())
                    if (player->HasSpell(sid) && IsRank(sid, Fortify))
                        ReduceRemainingCooldown(player, sid, percent);
            }
        }
        else if (IsRank(id, Epoch))
        {
            ApplyEpochAeon(player, target, healing);
            if (critical && player->HasAura(CadenceTalent))
                player->CastSpell(player, Cadence, true);
            if (healing && player->HasAura(OrderlyTalent))
                player->CastSpell(player, Orderly, true);
        }
        else if (IsRank(id, Accelerate) || IsRank(id, Decelerate))
        {
            uint32 opposite = IsRank(id, Accelerate) ? Decelerate : Accelerate;
            while (Aura* aura = target->GetAuraOfRankedSpell(opposite))
                aura->Remove();
        }
    }
};

class aura_ascension_timeline_tether : public AuraScript
{
    PrepareAuraScript(aura_ascension_timeline_tether);

    bool CheckProc(ProcEventInfo& event)
    {
        return Chronomancer(GetTarget()) && event.GetActor() == GetTarget() &&
            (event.GetTypeMask() & PROC_FLAG_DONE_PERIODIC) &&
            ((event.GetHealInfo() && event.GetHealInfo()->GetHeal()) ||
             (event.GetDamageInfo() && event.GetDamageInfo()->GetDamage()));
    }

    void ReduceCooldown(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        // The copied helper names only rank one. Apply its delta to every learned Fortify rank.
        for (auto const& [id, state] : player->GetSpellMap())
            if (player->HasSpell(id) && IsRank(id, Fortify))
                player->ModifySpellCooldown(id, Amount(TetherCooldown));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_timeline_tether::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_timeline_tether::ReduceCooldown,
            EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Time Out! stasis: "any direct damage taken may end the effect" (ProcChance of the rank), unless
// Titan's Gaze: "no longer breaks on damage". 802228 has no AuraInterruptFlags and no proc of its own.
class aura_ascension_time_out_stasis : public AuraScript
{
    PrepareAuraScript(aura_ascension_time_out_stasis);

    bool CheckBreak(ProcEventInfo& event)
    {
        Player* player = Chronomancer(GetTarget());
        DamageInfo* damage = event.GetDamageInfo();
        if (!player || !damage || !damage->GetDamage() || (event.GetTypeMask() & PROC_FLAG_TAKEN_PERIODIC) ||
            player->HasAura(TitansGaze))
            return false;
        uint32 rank = TimeOut;
        for (uint32 id : {TimeOutRankThree, TimeOutRankTwo, TimeOut})
            if (player->HasAura(id, player->GetGUID()))
            {
                rank = id;
                break;
            }
        // The roll happens here once; the spell_proc row carries Chance 100.
        return RollTalentChance(player, rank);
    }

    void Break(ProcEventInfo&)
    {
        PreventDefaultAction();
        Remove(AURA_REMOVE_BY_ENEMY_SPELL);
    }

    void End(AuraEffect const*, AuraEffectHandleModes)
    {
        // A broken stasis also ends the regeneration; an expired one leaves it alone.
        if (GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE)
            return;
        Unit* target = GetTarget();
        for (uint32 id : {TimeOut, TimeOutRankTwo, TimeOutRankThree})
            target->RemoveAurasDueToSpell(id, target->GetGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_time_out_stasis::CheckBreak);
        OnProc += AuraProcFn(aura_ascension_time_out_stasis::Break);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_time_out_stasis::End,
            EFFECT_1, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
    }
};

// Time Out! ranks: Expediting Time shortens the regeneration (SPELLMOD_DURATION on 0x8000) but
// not 802228. When the regeneration runs out, the stasis ends with it.
class aura_ascension_time_out : public AuraScript
{
    PrepareAuraScript(aura_ascension_time_out);

    void End(AuraEffect const*, AuraEffectHandleModes)
    {
        if (GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE)
            GetTarget()->RemoveAurasDueToSpell(TimeOutStasis, GetTarget()->GetGUID());
    }

    void Register() override
    {
        // SPELL_AURA_ANY: the contract rewrites effect 1 into OBS_MOD_POWER.
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_time_out::End,
            EFFECT_1, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Roll Back: "dispelling the last harmful effect placed on an ally or a positive effect placed on an
// enemy. Subsequent uses increases the mana cost of this ability". Its only effect is a bare
// SCRIPT_EFFECT. "Dispelling" is read as: auras that carry a dispel type.
class spell_ascension_roll_back : public SpellScript
{
    PrepareSpellScript(spell_ascension_roll_back);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({RollBackBuff}); }

    void Dispel(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !target->IsAlive())
            return;
        bool ally = caster->IsFriendlyTo(target);
        AuraApplication* latest = nullptr;
        int64 latestAge = 0;
        int64 now = int64(GameTime::GetGameTime().count());
        for (auto const& [id, application] : target->GetAppliedAuras())
        {
            Aura const* aura = application->GetBase();
            if (aura->IsPassive() || application->IsPositive() == ally ||
                aura->GetSpellInfo()->Dispel == DISPEL_NONE)
                continue;
            int64 age = aura->GetMaxDuration() > 0 ? int64(aura->GetMaxDuration()) - aura->GetDuration()
                : (now - int64(aura->GetApplyTime())) * IN_MILLISECONDS;
            if (!latest || age < latestAge)
            {
                latest = application;
                latestAge = age;
            }
        }
        if (latest)
            target->RemoveAura(latest, AURA_REMOVE_BY_ENEMY_SPELL);
    }

    void RaiseCost()
    {
        // The Time Skip recast (804470) is triggered and does not stack the cost again.
        if (!GetSpell()->IsTriggered())
            GetCaster()->CastSpell(GetCaster(), RollBackBuff, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_roll_back::Dispel, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        AfterCast += SpellCastFn(spell_ascension_roll_back::RaiseCost);
    }
};

// Timerend: "Haste does not decrease the duration but increases the total instances of damage".
// No rank carries SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC, which would also shorten the duration.
// Melt Reality (806335, 7 ranks) makes the same promise on the same effect 0 and is bound here too,
// through its spell_ranks chain (-806335, 2026_09_28_41).
class aura_ascension_timerend : public AuraScript
{
    PrepareAuraScript(aura_ascension_timerend);

    void Haste(AuraEffect const*, bool& isPeriodic, int32& amplitude)
    {
        Unit* caster = GetCaster();
        if (!isPeriodic || !caster || amplitude <= 0)
            return;
        float speed = caster->GetFloatValue(UNIT_MOD_CAST_SPEED);
        if (speed > 0.0f)
            amplitude = std::max(1, int32(amplitude * speed));
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_timerend::Haste,
            EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// The Bieko Effect: 712371 triggers 707312 every 2 s, whose three effects are effect 192
// (EffectNULL): "cooldowns ... are reduced by $707312s1%". Done here from 707312's own data.
class aura_ascension_bieko_effect : public AuraScript
{
    PrepareAuraScript(aura_ascension_bieko_effect);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({BiekoCooldownReduction}); }

    void Tick(AuraEffect const*)
    {
        PreventDefaultAction();
        Player* player = Chronomancer(GetTarget());
        SpellInfo const* reduction = sSpellMgr->GetSpellInfo(BiekoCooldownReduction);
        if (!player || !reduction)
            return;
        for (SpellEffectInfo const& effect : reduction->Effects)
            if (effect.Effect == AscensionReduceCooldownPctEffect && effect.MiscValue > 0 &&
                player->HasSpell(uint32(effect.MiscValue)))
                ReduceRemainingCooldown(player, uint32(effect.MiscValue), effect.CalcValue(player));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_bieko_effect::Tick,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// Waves of Time: "knocking them back slightly. The knockback repeats 1 additional time after 2 sec."
// Effect 0 only schedules the delayed knockback (effect 183) and effect 1 is a visual: the first
// knockback is cast here, from the caster like the delayed one (802600 is a cone, target 24).
class spell_ascension_waves_of_time : public SpellScript
{
    PrepareSpellScript(spell_ascension_waves_of_time);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({WavesOfTimeKnockback}); }

    void Knockback()
    {
        GetCaster()->CastSpell(GetCaster(), WavesOfTimeKnockback, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_waves_of_time::Knockback);
    }
};

void ApplyTimeContracts(SpellInfo* info)
{
    if (!info || info->SpellFamilyName != 28)
        return;
    if (IsRank(info->Id, Fortify))
        info->Effects[EFFECT_1].Effect = 0;
    if (info->Id == Sands)
        info->StackAmount = 5;
    if (info->Id == TimeOut || info->Id == TimeOutRankTwo || info->Id == TimeOutRankThree)
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_OBS_MOD_POWER;
    if (info->Id == NozdormusGaze)
    {
        // "regenerates $s1% less mana per tick": the regeneration is effect 1 of Time Out
        // (SPELLMOD_EFFECT2); the row names EFFECT1, the damage reduction. The instant mana
        // of effect 1 is cast in chronomancer_time_casts.
        SpellEffectInfo& regen = info->Effects[EFFECT_0];
        if (regen.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER && regen.MiscValue == SPELLMOD_EFFECT1)
            regen.MiscValue = SPELLMOD_EFFECT2;
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[EFFECT_1].TriggerSpell = 0;
    }
    for (uint32 id : {RenewalAeon, ResilienceAeon, ProtectionAeon, KeepAccelerating, CadenceTalent,
        OrderlyTalent, EndlessSandsTalent, EpicRecovery, TimelineTether})
        if (info->Id == id)
        {
            info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
            info->Effects[EFFECT_0].TriggerSpell = 0;
        }
    if (info->Id == OblivionAeon)
    {
        info->Effects[EFFECT_2].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[EFFECT_2].TriggerSpell = 0;
    }
    if (info->Id == EchoFragments && !info->Effects[EFFECT_2].Effect)
    {
        // Shatter Echo: "$s2% Ranged Weapon damage plus $m1 + 0.3 x spell power Arcane damage". It goes
        // through EffectWeaponDmg and MeleeDamageBonusDone, which read no spell_bonus_data; their only
        // spell power entry is the flat coefficient aura (Unit.cpp:10748). 92120, the spec 33 passive
        // every Shatter Echo user holds, carries it: 30 = 0.30 of arcane spell power, added before the
        // per-fragment SPELLMOD_DAMAGE and outside the 90% weapon share. Flags2 0x14: the six ranks only.
        SpellEffectInfo& scaling = info->Effects[EFFECT_2];
        scaling.Effect = SPELL_EFFECT_APPLY_AURA;
        scaling.ApplyAuraName = SPELL_AURA_OVERRIDE_CLASS_SCRIPTS;
        scaling.MiscValue = ASCENSION_SPELL_POWER_COEFFICIENT_FLAT;
        scaling.MiscValueB = 0;
        scaling.BasePoints = 29;
        scaling.DieSides = 1;
        scaling.RealPointsPerLevel = 0.0f;
        scaling.Amplitude = 0;
        scaling.TriggerSpell = 0;
        scaling.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        scaling.TargetB = SpellImplicitTargetInfo(0);
        scaling.SpellClassMask = flag96(0, 0, 0x14);
        info->_InitializeExplicitTargetMask();
    }
    if (info->Id == Renewal || info->Id == Protection || info->Id == Oblivion)
    {
        // These copy an already modified Epoch heal. The periodic heal cannot critically hit again.
        info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
        info->AttributesEx6 |= SPELL_ATTR6_IGNORE_HEALTH_MODIFIERS;
        info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        // Protection (560374) carries 1.0 point per level on top of the passed amount: "$s1% of the
        // amount healed" and nothing more. Renewal and Oblivion are already at 0.
        info->Effects[EFFECT_0].RealPointsPerLevel = 0.0f;
    }
    if (info->Id == SlipstreamPlace && info->Effects[EFFECT_0].Effect == SPELL_EFFECT_SUMMON)
    {
        // Effect 0 summons creature 52119, absent from creature_template, and effect 1 lays the
        // Pullers' area aura. Kept: the destination (target 87) the client asks for; the placement is
        // done by spell_ascension_slipstream_place.
        info->Effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
        info->Effects[EFFECT_0].MiscValue = 0;
        info->Effects[EFFECT_1].Effect = 0;
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_NONE;
        info->_InitializeExplicitTargetMask();
    }
}

// Slipstream: "Activate this ability again before the duration expires to choose a new location to
// place that enemy within 20 yds." The designed chain (707566 summons creature 52119, which does not
// exist, for the Pullers 707568/712436) cannot play. While the banishment lasts, 707566 takes the
// slot of 707555 (P-144: learned unannounced); its cast places the banished enemy.
void ClearSlipstream(Player* player)
{
    player->SetTemporarySpellReplacement(Slipstream, 0);
    player->removeSpell(SlipstreamPlace, SPEC_MASK_ALL, true); // temporary copy only
}

Unit* FindSlipstreamTarget(Player* player)
{
    // A search bound, not a gameplay radius: the banished enemy is where 707555 reached it.
    float const radius = 100.0f;
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(player, radius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> search(player, units, check);
    Cell::VisitObjects(player, search, radius);
    for (Unit* unit : units)
        if (unit != player && unit->IsAlive() && unit->HasAura(Slipstream, player->GetGUID()))
            return unit;
    return nullptr;
}

class aura_ascension_slipstream : public AuraScript
{
    PrepareAuraScript(aura_ascension_slipstream);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SlipstreamPlace}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = Chronomancer(GetCaster());
        if (!player || !player->IsInWorld() || GetTarget() == player)
            return;
        player->LearnTemporarySpellReplacementTarget(SlipstreamPlace);
        player->SetTemporarySpellReplacement(Slipstream, SlipstreamPlace);
    }

    void End(AuraEffect const*, AuraEffectHandleModes)
    {
        // A caster gone (logout, other map) keeps nothing: replacements and temporary spells are not saved.
        if (Player* player = Chronomancer(GetCaster()))
            ClearSlipstream(player);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_slipstream::Apply,
            EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_slipstream::End,
            EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_ascension_slipstream_place : public SpellScript
{
    PrepareSpellScript(spell_ascension_slipstream_place);

    void Place(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = Chronomancer(GetCaster());
        WorldLocation const* dest = GetHitDest();
        if (!player || !dest)
            return;
        if (Unit* banished = FindSlipstreamTarget(player); banished && dest->GetMapId() == banished->GetMapId())
        {
            // Toward the chosen point, at most 20 yd from where the enemy is, stopped by collisions.
            float distance = std::min(banished->GetExactDist2d(dest), SlipstreamPlaceRange);
            if (distance > 0.5f)
            {
                Position pos = banished->GetPosition();
                banished->MovePositionToFirstCollision(pos, distance, banished->GetRelativeAngle(dest));
                banished->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                    banished->GetOrientation());
            }
        }
        // One placement per banishment: Slipstream takes its slot back (also when no target is left).
        player->SetTemporarySpellReplacement(Slipstream, 0);
    }

    void Register() override
    {
        // SPELL_EFFECT_ANY: the contract turns the summon into a dummy.
        OnEffectHit += SpellEffectFn(spell_ascension_slipstream_place::Place, EFFECT_0, SPELL_EFFECT_ANY);
    }
};

class chronomancer_time_contracts : public GlobalScript
{
public:
    chronomancer_time_contracts() : GlobalScript("chronomancer_time_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        ApplyTimeContracts(info);
    }
};
}

void AddSC_AscensionChronomancerTime()
{
    new chronomancer_time_casts();
    new chronomancer_time_contracts();
    RegisterSpellScript(aura_ascension_timeline_tether);
    RegisterSpellScript(aura_ascension_time_out_stasis);
    RegisterSpellScript(aura_ascension_time_out);
    RegisterSpellScript(spell_ascension_roll_back);
    RegisterSpellScript(aura_ascension_timerend);
    RegisterSpellScript(aura_ascension_bieko_effect);
    RegisterSpellScript(spell_ascension_waves_of_time);
    RegisterSpellScript(aura_ascension_slipstream);
    RegisterSpellScript(spell_ascension_slipstream_place);
}
