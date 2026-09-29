/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionReaperTalents.h"
#include "AscensionSpellSafe.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>
#include <list>

namespace
{
enum ReaperTalentSpells : uint32
{
    SPELL_HARVESTER_AMOUNT = 500283,
    SPELL_BLOOD_HARVEST = 504565,
    SPELL_UNDERWALK = 800797,
    SPELL_FROM_THE_SHADOWS = 561099,
    SPELL_FROM_THE_SHADOWS_CRIT = 561128,
    SPELL_REAPED_SOUL = 500363,
    SPELL_SOUL_CAPTURED = 572887,
    SPELL_SOUL_SPLINTERS = 805719,
    SPELL_SOUL_SPLINTER = 805720,

    SPELL_REAP = 500357,
    SPELL_THRESH = 505170,
    SPELL_REDSHADE_THRESH = 525058,
    SPELL_DECIMATE = 500523,
    SPELL_DECIMATION_COUNT = 573289,
    SPELL_DECIMATION_WINDOW = 704215,
    SPELL_DEATHSTALKER = 804054,
    SPELL_DEATHSTALKER_SPEED = 807775,
    SPELL_ANXIETY = 807776,
    SPELL_DEATHCHASER = 805190,
    SPELL_CHASING_DEATH_AURA = 807418,
    SPELL_CHASING_DEATH_EXTEND = 807546
};

// Chasing Death 707455: "Deathchaser used against a target below 20% health". The 20 is the
// tooltip's own literal: no field of 707455, 807418 or 807546 carries it.
constexpr int32 CHASING_DEATH_HEALTH_PCT = 20;

class spell_ascension_soul_capture : public SpellScript
{
    PrepareSpellScript(spell_ascension_soul_capture);
    ObjectGuid _corpse;

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_SOUL_CAPTURED, SPELL_REAPED_SOUL});
    }

    bool Eligible(Unit* unit)
    {
        Unit* caster = GetCaster();
        return unit && unit != caster && !unit->IsAlive() && !caster->IsFriendlyTo(unit) &&
            !unit->HasAura(SPELL_SOUL_CAPTURED) && caster->IsInMap(unit) && caster->InSamePhase(unit) &&
            caster->IsWithinDistInMap(unit, GetSpellInfo()->Effects[EFFECT_0].CalcRadius(caster)) &&
            caster->IsWithinLOSInMap(unit);
    }

    SpellCastResult CheckCorpse()
    {
        _corpse.Clear();
        Unit* caster = GetCaster();
        std::list<Unit*> corpses;
        Acore::AnyDeadUnitCheck check;
        Acore::UnitListSearcher<Acore::AnyDeadUnitCheck> searcher(caster, corpses, check);
        Cell::VisitObjects(caster, searcher, GetSpellInfo()->Effects[EFFECT_0].CalcRadius(caster));
        for (Unit* corpse : corpses)
            if (Eligible(corpse))
            {
                _corpse = corpse->GetGUID();
                return SPELL_CAST_OK;
            }
        return SPELL_FAILED_NO_EDIBLE_CORPSES;
    }

    void SuppressTrigger(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
    }

    void Capture(SpellEffIndex index)
    {
        Unit* caster = GetCaster();
        Unit* corpse = ObjectAccessor::GetUnit(*caster, _corpse);
        // Recheck at impact: another cast can consume the same corpse after CheckCast.
        if (!Eligible(corpse) || !caster->AddAura(SPELL_SOUL_CAPTURED, corpse))
        {
            PreventHitDefaultEffect(index);
            return;
        }
        caster->CastSpell(caster, SPELL_REAPED_SOUL, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_soul_capture::CheckCorpse);
        OnEffectLaunch += SpellEffectFn(spell_ascension_soul_capture::SuppressTrigger,
            EFFECT_ALL, SPELL_EFFECT_TRIGGER_SPELL);
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_soul_capture::SuppressTrigger,
            EFFECT_ALL, SPELL_EFFECT_TRIGGER_SPELL);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_soul_capture::Capture, EFFECT_2, SPELL_EFFECT_HEAL_PCT);
    }
};

class aura_ascension_harvester : public AuraScript
{
    PrepareAuraScript(aura_ascension_harvester);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_HARVESTER_AMOUNT, SPELL_BLOOD_HARVEST});
    }

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        return owner->IsPlayer() && owner->getClass() == CLASS_REAPER && owner->IsAlive() &&
            event.GetActor() == owner && victim && victim != owner && !owner->IsFriendlyTo(victim) &&
            event.GetDamageInfo() && event.GetDamageInfo()->GetDamage();
    }

    void Heal(AuraEffect const*, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget();
        // Meme famille de defaut que les quinze deref nus deja repris ailleurs dans le module :
        // sSpellMgr->GetSpellInfo() rend nullptr des que l'id sort du Spell.dbc (P-047), et ce
        // chemin est un proc, donc un fil de MapUpdate. Le repli 15 est la valeur lue au
        // Spell.dbc en service le 2026-09-21 (500283 "Harvester", effet 0 BasePoints 14 /
        // DieSides 1, soit val=15) : degrader la passive de spec vaut mieux que l'eteindre.
        int32 percent = AscensionSpellSafe::EffectValue(SPELL_HARVESTER_AMOUNT, EFFECT_0, owner, 15);
        DamageInfo* damageInfo = event.GetDamageInfo();
        if (!damageInfo)
            return;
        uint64 amount = uint64(damageInfo->GetDamage()) * std::clamp(percent, 0, 100) / 100;
        if (amount)
            owner->CastCustomSpell(SPELL_BLOOD_HARVEST, SPELLVALUE_BASE_POINT0,
                int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), owner, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_harvester::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_harvester::Heal, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The Jailer's Call: attacks against enemies below 20% health trigger its extra Shadow damage.
class aura_ascension_jailers_call : public AuraScript
{
    PrepareAuraScript(aura_ascension_jailers_call);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* victim = event.GetActionTarget();
        return victim && victim != GetTarget() && victim->IsAlive() && victim->HealthBelowPct(20);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_jailers_call::CheckProc);
    }
};

// Reap is transformed by two auras, and the client only swaps a button when the server tells
// it (SMSG_SUPERCEDED_SPELL through Player::SetTemporarySpellReplacement, the Outmaneuver
// pattern, AscensionRangerOutmaneuver.cpp). TalentReplacements is NOT the tool: it swaps for
// as long as the talent is owned (AscensionCompat.cpp SynchronizeTalentReplacements), while
// these last 10 and 15 s. No TalentReplacements row targets the Reap chain.
//   Redshade 524735 -> 525058 (10 s): "Using Reap now transforms your Reap into Thresh for
//     10 sec." Thresh's own e2 (effect 183 -> 525303, REMOVE_AURA 525058) ends it on cast;
//     its family mask (0,0,16384) lacks Reap's bit, so it cannot re-proc Redshade.
//   Decimation 704193 -> 704215 (15 s): "Every 6th cast of Reap transforms your next Reap
//     into Decimate for 15 sec." DECISION: Reap, as the passive the player reads says; the
//     helper auras' own tooltips say Soul Strike. Decimate wins when both are up.
// Both targets are learned unannounced (P-144) and dropped with removeSpell(..., true), which
// only ever removes a PLAYERSPELL_TEMPORARY copy and turns the client slot back itself.
uint32 ActiveReapRank(Player* player)
{
    uint32 best = 0;
    uint8 bestRank = 0;
    for (auto const& [id, spell] : player->GetSpellMap())
    {
        if (!spell || sSpellMgr->GetFirstSpellInChain(id) != SPELL_REAP || !player->HasActiveSpell(id))
            continue;
        uint8 rank = sSpellMgr->GetSpellRank(id);
        if (!best || rank > bestRank)
        {
            best = id;
            bestRank = rank;
        }
    }
    return best;
}

void SyncReapTransform(Player* player)
{
    if (!player || player->getClass() != CLASS_REAPER)
        return;
    bool leaving = !player->IsInWorld() || !player->GetSession() || player->GetSession()->PlayerLogout();
    uint32 wanted = 0;
    if (!leaving && player->IsAlive())
    {
        if (player->HasAura(SPELL_DECIMATION_WINDOW, player->GetGUID()) && sSpellMgr->GetSpellInfo(SPELL_DECIMATE))
            wanted = SPELL_DECIMATE;
        else if (player->HasAura(SPELL_REDSHADE_THRESH, player->GetGUID()) && sSpellMgr->GetSpellInfo(SPELL_THRESH))
            wanted = SPELL_THRESH;
    }
    uint32 reap = ActiveReapRank(player);
    if (reap && wanted)
    {
        player->LearnTemporarySpellReplacementTarget(wanted);
        player->SetTemporarySpellReplacement(reap, wanted);
    }
    else if (reap)
        player->SetTemporarySpellReplacement(reap, 0);
    // A temporary spell is never saved: on the way out, dropping the replacement is enough.
    if (leaving)
        return;
    for (uint32 target : {SPELL_DECIMATE, SPELL_THRESH})
        if (target != wanted)
            player->removeSpell(target, SPEC_MASK_ALL, true);
}

// 525058 and 704215: both are a bare aura 4 on the Reaper. Not saved (reaper_secondary_metadata).
class aura_ascension_reaper_reap_transform : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_reap_transform);

    void Sync(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Unit* owner = GetTarget())
            SyncReapTransform(owner->ToPlayer());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_reap_transform::Sync,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_reaper_reap_transform::Sync,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 573289, the Decimation counter: spell_proc 704193 adds one stack per Reap cast (StackAmount
// 6). At the cap it becomes the 15 s window 704215. AddAura, not CastSpell: 704215 e1 is an
// effect 183 toward 707457, a remover of this very counter, which is removed here already.
// Deferred by one event: this handler runs inside Aura::SetStackAmount, which goes on using
// the aura's applications after it returns (SpellAuras.cpp), so the aura must not go now.
class aura_ascension_reaper_decimation_count : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_decimation_count);

    void Counted(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        uint32 full = GetSpellInfo()->CalcMaxAuraStacks(GetCaster());
        if (!player || player->getClass() != CLASS_REAPER || !player->IsInWorld() || !player->IsAlive() ||
            !full || GetStackAmount() < full)
            return;
        player->m_Events.AddEventAtOffset([player, full]()
        {
            if (!player->IsInWorld() || !player->IsAlive())
                return;
            Aura* count = player->GetAura(SPELL_DECIMATION_COUNT, player->GetGUID());
            if (!count || count->GetStackAmount() < full)
                return;
            count->Remove();
            if (sSpellMgr->GetSpellInfo(SPELL_DECIMATION_WINDOW))
                player->AddAura(SPELL_DECIMATION_WINDOW, player);
        }, 1ms);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_decimation_count::Counted,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }
};

// Deathstalker 804054 (level 22): "Mark an enemy target and increase your speed by
// $807775s1% while within $807776a2 yds of them for $d. Dealing damage will break the effect.
// ... Stalking a Humanoid will apply Anxiety to them." The DBC applies all three halves
// unconditionally (e0 -> 807775 on self, e1 -> 807776 on the target, AuraInterruptFlags 0).
//   Humanoid: e1 is prevented on any other creature type (players are humanoid).
//   Range: 807775's detection effect is made periodic, at Anxiety's own e1 amplitude, and
//     the speed amount follows the distance to the mark, radius read from 807776 e1. The
//     stealth detection is not range-bound in the tooltip and stays.
//   Damage: spell_proc 807775 (direct damage done, one charge) removes the whole aura.
class spell_ascension_reaper_deathstalker : public SpellScript
{
    PrepareSpellScript(spell_ascension_reaper_deathstalker);

    bool Humanoid(Unit* target) const
    {
        return target && target->GetCreatureType() == CREATURE_TYPE_HUMANOID;
    }

    void AnxietyLaunch(SpellEffIndex index)
    {
        if (!Humanoid(GetExplTargetUnit()))
            PreventHitDefaultEffect(index);
    }

    void AnxietyLaunchTarget(SpellEffIndex index)
    {
        if (!Humanoid(GetHitUnit()))
            PreventHitDefaultEffect(index);
    }

    void Mark()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster)
            return;
        if (Aura* speed = caster->GetAura(SPELL_DEATHSTALKER_SPEED, caster->GetGUID()))
            speed->SetScriptValue(SPELL_DEATHSTALKER, target->GetGUID().GetRawValue());
    }

    void Register() override
    {
        OnEffectLaunch += SpellEffectFn(spell_ascension_reaper_deathstalker::AnxietyLaunch,
            EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_reaper_deathstalker::AnxietyLaunchTarget,
            EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
        AfterHit += SpellHitFn(spell_ascension_reaper_deathstalker::Mark);
    }
};

class aura_ascension_reaper_deathstalker_speed : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_deathstalker_speed);

    int32 _speed = 0;

    void Remember(AuraEffect const* effect, AuraEffectHandleModes)
    {
        _speed = effect->GetAmount();
    }

    void MakePeriodic(AuraEffect const*, bool& isPeriodic, int32& amplitude)
    {
        isPeriodic = true;
        amplitude = int32(std::max<uint32>(100, AscensionSpellSafe::EffectAmplitude(SPELL_ANXIETY, EFFECT_1, 500)));
    }

    void Track(AuraEffect const*)
    {
        Unit* owner = GetTarget();
        ObjectGuid markGuid(GetAura()->GetScriptValue(SPELL_DEATHSTALKER));
        // Mark not recorded: keep the native behaviour rather than guess a target.
        if (!owner || markGuid.IsEmpty())
            return;
        Unit* mark = ObjectAccessor::GetUnit(*owner, markGuid);
        if (!mark || !mark->IsAlive() || !mark->IsInMap(owner))
        {
            // The stalked enemy is gone: so is the stalking. Expires on the next update.
            GetAura()->SetDuration(0);
            return;
        }
        AuraEffect* speed = GetAura()->GetEffect(EFFECT_0);
        if (!speed)
            return;
        float radius = AscensionSpellSafe::EffectRadius(SPELL_ANXIETY, EFFECT_1, owner, 20.0f);
        int32 wanted = owner->IsWithinDistInMap(mark, radius) ? _speed : 0;
        if (speed->GetAmount() != wanted)
            speed->ChangeAmount(wanted);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_deathstalker_speed::Remember,
            EFFECT_0, SPELL_AURA_MOD_INCREASE_SPEED, AURA_EFFECT_HANDLE_REAL);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_reaper_deathstalker_speed::MakePeriodic,
            EFFECT_1, SPELL_AURA_DETECT_STEALTH);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_reaper_deathstalker_speed::Track,
            EFFECT_1, SPELL_AURA_DETECT_STEALTH);
    }
};

// Soulforged Weaponry 561127 / 561340 (two ranks, not chained): "While dual-wielding, your auto
// attacks now have a $h% chance to make your next Murder free of cost." spell_proc on both
// ranks (auto attacks, e0 out of the proc); the dual-wield half is this check. 705964 is
// consumed by its own spell_proc row (one charge, REQ_SPELLMOD on Murder).
class aura_ascension_reaper_soulforged_weaponry : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_soulforged_weaponry);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        Unit* victim = event.GetActionTarget();
        return player && player->getClass() == CLASS_REAPER && player->IsAlive() && player->IsInWorld() &&
            event.GetActor() == player && !event.GetSpellInfo() && player->haveOffhandWeapon() &&
            victim && victim != player && !player->IsFriendlyTo(victim);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_soulforged_weaponry::CheckProc);
    }
};

// Chasing Death 707455 (e0 aura 42 -> 807418 on the target): "Deathchaser used against a target
// below 20% health will now reapply itself at the end of its duration until they die or until
// $807418d passes." spell_proc 707455 (Deathchaser's family mask, cast hit) is the engine; the
// health threshold is this check. The native aura 42 then puts 807418 on the proc target.
class aura_ascension_reaper_chasing_death : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_chasing_death);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        Unit* victim = event.GetActionTarget();
        SpellInfo const* source = event.GetSpellInfo();
        return player && player->getClass() == CLASS_REAPER && player->IsAlive() && player->IsInWorld() &&
            event.GetActor() == player && source && sSpellMgr->GetFirstSpellInChain(source->Id) == SPELL_DEATHCHASER &&
            victim && victim != player && victim->IsAlive() && !player->IsFriendlyTo(victim) &&
            victim->HealthBelowPct(CHASING_DEATH_HEALTH_PCT);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_chasing_death::CheckProc);
    }
};

// 807418 (aura 23, periodic trigger of 807546, effect 177). 807546's MiscValue is 805190, the
// RANK 1 of Deathchaser only, and the native effect reads GetAura(misc) with no caster: ranks 2-8
// (560428 ... 573053) were never extended, and another Reaper's rank 1 could be. The extension is
// therefore done here, on the caster's own Deathchaser of any rank, with 807546's value and the
// native rule (duration + value), capped at the aura's own maximum.
class aura_ascension_reaper_chasing_death_extend : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_chasing_death_extend);

    void Extend(AuraEffect const*)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        ObjectGuid casterGuid = GetCasterGUID();
        if (!target || !target->IsAlive() || casterGuid.IsEmpty())
            return;
        Aura* chase = target->GetAuraOfRankedSpell(SPELL_DEATHCHASER, casterGuid);
        if (!chase || chase->IsRemoved() || chase->GetMaxDuration() <= 0)
            return;
        int32 add = AscensionSpellSafe::EffectValue(SPELL_CHASING_DEATH_EXTEND, EFFECT_0, GetCaster(), 1000);
        if (add <= 0)
            return;
        int32 duration = int32(std::min<int64>(int64(chase->GetDuration()) + add, chase->GetMaxDuration()));
        if (duration > chase->GetDuration())
            chase->SetDuration(duration);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_reaper_chasing_death_extend::Extend,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

class reaper_talent_events : public UnitScript
{
public:
    reaper_talent_events() : UnitScript("reaper_talent_events", true,
        {UNITHOOK_ON_AURA_REMOVE, UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE}) { }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !application || !player->IsAlive() ||
            !player->IsInWorld() || mode == AURA_REMOVE_BY_DEATH)
            return;
        Aura* aura = application->GetBase();
        if (aura->GetId() == SPELL_UNDERWALK && aura->GetCasterGUID() == player->GetGUID() &&
            player->HasAura(SPELL_FROM_THE_SHADOWS))
            player->CastSpell(player, SPELL_FROM_THE_SHADOWS_CRIT, true);
    }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        if (caster && caster->IsPlayer() && caster->getClass() == CLASS_REAPER && info->Id == SPELL_SOUL_SPLINTER &&
            info->SpellFamilyName == 36 && index == EFFECT_0 && info->Effects[index].IsAura(SPELL_AURA_PERIODIC_DAMAGE))
            value += std::max(0.0f, caster->GetStat(STAT_STAMINA)) * 0.035f;
    }
};
}

bool HandleAscensionReaperResource(Player* player, uint32 spellId, int32 amount)
{
    if (player->getClass() != CLASS_REAPER || spellId != SPELL_REAPED_SOUL)
        return false;
    Aura* aura = player->GetAura(spellId, player->GetGUID());
    uint8 previous = aura ? aura->GetStackAmount() : 0;
    if (aura)
        aura->ModStackAmount(amount);
    else if (amount > 0)
        if (Aura* created = player->AddAura(spellId, player); created && amount > 1)
            created->ModStackAmount(amount - 1);
    // Re-resolve: spending the last stack removes the aura. A capped award,
    // loss, load or refresh is not a newly harvested soul.
    aura = player->GetAura(spellId, player->GetGUID());
    if (aura && aura->GetStackAmount() > previous && player->IsAlive() && player->HasAura(SPELL_SOUL_SPLINTERS))
        player->CastSpell(player, SPELL_SOUL_SPLINTER, true);
    return true;
}

void AddSC_AscensionReaperTalents()
{
    RegisterSpellScript(spell_ascension_soul_capture);
    RegisterSpellScript(aura_ascension_harvester);
    RegisterSpellScript(aura_ascension_jailers_call);
    RegisterSpellScript(aura_ascension_reaper_reap_transform);
    RegisterSpellScript(aura_ascension_reaper_decimation_count);
    RegisterSpellScript(spell_ascension_reaper_deathstalker);
    RegisterSpellScript(aura_ascension_reaper_deathstalker_speed);
    RegisterSpellScript(aura_ascension_reaper_soulforged_weaponry);
    RegisterSpellScript(aura_ascension_reaper_chasing_death);
    RegisterSpellScript(aura_ascension_reaper_chasing_death_extend);
    new reaper_talent_events();
}
