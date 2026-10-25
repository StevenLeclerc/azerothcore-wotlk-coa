/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionGuardianCompletion.h"
#include "EventProcessor.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "ThreatManager.h"
#include <algorithm>
#include <limits>

namespace
{
// Broad Sweep (805150 and its six higher ranks) is replaced by the Ballad of the
// Dragonslayer while Inspiring Leader (505344) is carried; the ballad is the spell the
// player actually casts, so the Broad Sweep callbacks have to recognise it. The same
// substitution is already read this way for Pulverize and the Ballad of the Conqueror
// in AscensionClassMechanics.cpp (Guardbreaker).
bool IsBroadSweep(Player const* player, uint32 id)
{
    if (sSpellMgr->GetFirstSpellInChain(id) == 805150)
        return true;
    return player && player->HasAura(505344) && AscensionGuardian::BalladOfTheDragonslayer(id);
}

class GuardianLanding : public BasicEvent
{
public:
    GuardianLanding(Player* owner, Position const& destination) : _owner(owner->GetGUID()),
        _map(owner->GetMapId()), _instance(owner->GetInstanceId()), _destination(destination) { }

    bool Execute(uint64 time, uint32 /*diff*/) override
    {
        Player* player = ObjectAccessor::FindPlayer(_owner);
        if (!player || !player->IsAlive() || !player->IsInWorld() || player->GetMapId() != _map ||
            player->GetInstanceId() != _instance || ++_attempts > 100)
            return true;
        if (!player->movespline->Finalized())
        {
            player->m_Events.AddEvent(this, time + 50);
            return false;
        }
        if (player->GetExactDist2d(&_destination) <= 3.0f)
            player->CastSpell(_destination.GetPositionX(), _destination.GetPositionY(),
                _destination.GetPositionZ(), 802874, true);
        return true;
    }

private:
    ObjectGuid _owner;
    uint32 _map;
    uint32 _instance;
    Position _destination;
    uint32 _attempts = 0;
};

class spell_ascension_guardian_ability : public SpellScript
{
    PrepareSpellScript(spell_ascension_guardian_ability);
    uint32 _energy = 0;
    uint32 _perilStacks = 0;

    bool Load() override
    {
        return GetCaster()->IsPlayer() && GetCaster()->getClass() == CLASS_GUARDIAN;
    }

    void Before()
    {
        _energy = GetCaster()->GetPower(POWER_ENERGY);
    }

    SpellCastResult Check()
    {
        if (GetSpellInfo()->Id == 500673)
        {
            bool ready = false;
            for (auto const& pair : GetCaster()->GetAppliedAuras())
            {
                Aura const* aura = pair.second->GetBase();
                if (AscensionGuardian::Advance(aura->GetId()) && aura->GetMaxDuration() - aura->GetDuration() >= 500)
                    ready = true;
            }
            if (!ready && !GetSpell()->IsTriggered())
                return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
        }
        if (GetSpellInfo()->Id == 802629 && !GetCaster()->HasAura(707138))
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
        if (GetSpellInfo()->Id == 802629)
            for (auto const& pair : GetCaster()->ToPlayer()->GetSpellMap())
                if (AscensionGuardian::HeavyBlow(pair.first) && GetCaster()->ToPlayer()->GetSpellCooldownDelay(pair.first))
                    return SPELL_FAILED_NOT_READY;
        return SPELL_CAST_OK;
    }

    void After()
    {
        Player* player = GetCaster()->ToPlayer();
        uint32 id = GetSpellInfo()->Id;
        if (id == 802871)
        {
            if (WorldLocation const* dest = GetExplTargetDest())
                player->m_Events.AddEvent(new GuardianLanding(player, *dest), player->m_Events.CalculateTime(50));
            return;
        }
        if (id == 500673)
        {
            std::vector<uint32> remove;
            for (auto const& pair : player->GetAppliedAuras())
                if (AscensionGuardian::Advance(pair.first))
                    remove.push_back(pair.first);
            for (uint32 aura : remove)
                player->RemoveAurasDueToSpell(aura);
            player->RemoveAurasDueToSpell(801160);
            return;
        }
        if (GetSpell()->IsTriggered())
            return;
        if (AscensionGuardian::Ram(id))
        {
            if (player->HasAura(520650))
                player->CastSpell(player, 520651, true);
        }
        if ((AscensionGuardian::Ram(id) || AscensionGuardian::Pulverize(id)) && player->HasAura(807297))
            player->CastSpell(player, 705381, true);
        if (AscensionGuardian::HeavyBlow(id))
            AscensionGuardian::AddParagon(player, 3);
        if (id == 802629)
        {
            player->RemoveAurasDueToSpell(707138);
            for (auto const& pair : player->GetSpellMap())
                if (AscensionGuardian::HeavyBlow(pair.first) && player->HasActiveSpell(pair.first))
                    player->AddSpellCooldown(pair.first, 0, GetSpellInfo()->RecoveryTime, true);
        }
        // Earthsplitter. Only half of 806081 can reach a ballad. Its effect 1
        // (ADD_PCT_MODIFIER, SPELLMOD_COST, -10%, class mask (16, 0, 0)) applies,
        // because the nine Ballad of the Dragonslayer ranks carry Broad Sweep's family
        // flag A = 16. Its effect 2 does not: ApplyAdditionalTargetContracts turns it
        // into SPELL_AURA_MOD_MAX_AFFECTED_TARGETS, and the core only sums that aura
        // inside `if (uint32 maxTargets = m_spellValue->MaxAffectedTargets)`
        // (Spell.cpp:1283 and 1377, its only two readers). The ballads have Spell.dbc
        // MaxAffectedTargets = 0 where Broad Sweep has 8, so the target half stays dead
        // on the ballad. No tooltip, DBC field or upstream tracker entry gives the
        // ballad a target count, so none is invented here.
        if (player->HasAura(805155) && IsBroadSweep(player, id))
            player->CastSpell(player, 806081, true);
        if (id == 803963)
        {
            // G-05 Liberation: "Dispels $807140s3 harmful magic effects from the ALLY". 807140 effect 2
            // (DISPEL, implicit target 21 TARGET_UNIT_TARGET_ALLY) used to be cast on the Guardian
            // himself. The ally is the explicit target of 803963 (also target 21); the Guardian stays
            // the fallback when the target is gone or no longer friendly.
            Unit* ally = GetExplTargetUnit();
            if (!ally || !ally->IsInWorld() || !ally->IsAlive() || !player->IsFriendlyTo(ally))
                ally = player;
            player->CastSpell(ally, 807140, true);
        }
        if (_perilStacks)
        {
            uint32 remaining = player->GetSpellCooldownDelay(300983);
            player->ModifySpellCooldown(300983, -int32(uint64(remaining) * std::min(10u, _perilStacks) / 10));
        }
    }

    void Hit()
    {
        Player* player = GetCaster()->ToPlayer();
        Unit* target = GetHitUnit();
        uint32 id = GetSpellInfo()->Id;
        if (!target)
            return;
        if (id == 572904 && player->IsFriendlyTo(target) && target != player)
        {
            auto threats = target->GetThreatMgr().GetThreatenedByMeList();
            for (auto const& entry : threats)
            {
                ThreatReference* reference = entry.second;
                Unit* enemy = reference->GetOwner();
                float amount = reference->GetThreat() * 0.5f;
                enemy->GetThreatMgr().ModifyThreatByPercent(target, -50);
                enemy->GetThreatMgr().AddThreat(player, amount, GetSpellInfo(), true, true);
            }
            return;
        }
        if (GetSpell()->IsTriggered() || target == player || player->IsFriendlyTo(target))
            return;
        if (id == 500258 && !target->IsAlive())
            player->RemoveSpellCooldown(id, true);
        if (!target->IsAlive() || GetHitDamage() <= 0)
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(id);
        // Calculated Strike is a real cast, up to 200% weapon damage, and this hook runs
        // once per target struck. Broad Sweep is capped at 8 targets (Spell.dbc
        // MaxAffectedTargets = 8); the Ballad of the Dragonslayer that replaces it has 0,
        // i.e. no cap at all, so on a wide pull this fires once per living enemy in the
        // 8 yd radius. No cap is added here because no source gives one; bounding it, or
        // restricting Calculated Strike to the 805150 chain, is an operator decision.
        if (player->HasAura(705341) &&
            (IsBroadSweep(player, id) || root == 800316 || root == 500463))
            player->CastCustomSpell(705342, SPELLVALUE_BASE_POINT0, int32(std::min(_energy, 1000u) * 2), target,
                TRIGGERED_FULL_MASK);
        if (AscensionGuardian::Pulverize(id) || id == 801776 || (id >= 501068 && id <= 501074) || id == 574340)
        {
            if (Aura* peril = target->GetAura(524610, player->GetGUID()))
            {
                uint8 stacks = peril->GetStackAmount();
                _perilStacks += stacks;
                target->RemoveAurasDueToSpell(524610, player->GetGUID());
                player->CastCustomSpell(524608, SPELLVALUE_BASE_POINT0,
                    int32(player->GetTotalAttackPowerValue(BASE_ATTACK) * 0.1f * stacks), target, TRIGGERED_FULL_MASK);
            }
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_guardian_ability::Check);
        BeforeCast += SpellCastFn(spell_ascension_guardian_ability::Before);
        AfterCast += SpellCastFn(spell_ascension_guardian_ability::After);
        AfterHit += SpellHitFn(spell_ascension_guardian_ability::Hit);
    }
};

class aura_ascension_guardian_advance : public AuraScript
{
    PrepareAuraScript(aura_ascension_guardian_advance);

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_ROOT, true);
        GetTarget()->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SNARE, true);
        GetTarget()->RemoveAurasWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE), AURA_REMOVE_BY_DEFAULT);
    }

    void Removed(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        Unit* owner = GetTarget();
        owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_ROOT, false);
        owner->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SNARE, false);
        if (GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE && owner->IsAlive() && owner->IsInWorld())
            owner->CastSpell(owner, 500673, true, nullptr, effect);
        owner->RemoveAurasDueToSpell(801160);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_guardian_advance::Apply,
            EFFECT_1, SPELL_AURA_FORCE_MOVE_FORWARD, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_guardian_advance::Removed,
            EFFECT_1, SPELL_AURA_FORCE_MOVE_FORWARD, AURA_EFFECT_HANDLE_REAL);
    }
};

// Hold the Line (803830) promises immunity to incapacitate, disorient, knockback and grip, but its
// three effects only carry mechanics 6 (grip) and 2 (disorient) and the knockback effect. The
// unused "Hold the Line | SLS" twin 803939 shows the missing part: mechanics 14 and 30.
class aura_ascension_guardian_hold_the_line : public AuraScript
{
    PrepareAuraScript(aura_ascension_guardian_hold_the_line);

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_KNOCKOUT, true);
        target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SAPPED, true);
        if (GetSpellInfo()->HasAttribute(SPELL_ATTR1_IMMUNITY_PURGES_EFFECT))
            target->RemoveAurasWithMechanic((1ULL << MECHANIC_KNOCKOUT) | (1ULL << MECHANIC_SAPPED),
                AURA_REMOVE_BY_DEFAULT, GetId());
    }

    // Apply and Removed both hang on the LAST effect (symmetric: same HasEffect mask).
    // Unit::_UnapplyAura removes effects 0..2 in order, so the core handlers of all three
    // effects have already run when Removed is called.
    void Removed(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        // ApplySpellImmune(false) erases every entry of this spell id, including those of another
        // Guardian's Hold the Line still covering the target (this application is already out of
        // the applied list here). Keep ours, and give back the native DBC immunities that the core
        // handlers (ApplyAllSpellImmunitiesTo, HandleAuraModEffectImmunity) have just erased.
        if (target->HasAura(GetId()))
        {
            for (SpellEffectInfo const& eff : GetSpellInfo()->Effects)
            {
                if (!eff.IsAura())
                    continue;
                if (eff.ApplyAuraName == SPELL_AURA_MECHANIC_IMMUNITY && eff.MiscValue > 0 && eff.MiscValue < MAX_MECHANIC)
                    target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, uint32(eff.MiscValue), true);
                else if (eff.ApplyAuraName == SPELL_AURA_EFFECT_IMMUNITY && eff.MiscValue > 0)
                    target->ApplySpellImmune(GetId(), IMMUNITY_EFFECT, uint32(eff.MiscValue), true);
            }
            return;
        }
        target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_KNOCKOUT, false);
        target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, MECHANIC_SAPPED, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_guardian_hold_the_line::Apply,
            EFFECT_2, SPELL_AURA_MECHANIC_IMMUNITY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_guardian_hold_the_line::Removed,
            EFFECT_2, SPELL_AURA_MECHANIC_IMMUNITY, AURA_EFFECT_HANDLE_REAL);
    }
};

constexpr uint8 GUARDIAN_CHAIN_WALK = 32;

// Ascension's cooldown effects (165: MiscValue = spell, value = ms, 0 = reset; 195: reset) name ONE id,
// the rank 1, while the cooldown sits under the id of the rank actually cast (Player::
// AddSpellAndCategoryCooldowns). The core handlers (SpellEffects.cpp, ModifyAscensionCooldown and
// ResetAscensionCooldown) only touch that id, so from rank 2 on nothing was shortened. Same walk as
// ReduceCooldownOverChain (AscensionPrimalistTalents.cpp): every rank of the spell_ranks chain that has
// a running cooldown; a cut at least equal to what remains clears the entry (and the category when
// MiscValueB asks for it). A spell outside any chain is its own one-element chain: native behaviour.
void ReduceGuardianCooldownOverChain(Player* player, uint32 spellId, bool resetCategory, uint64 cut)
{
    uint32 current = sSpellMgr->GetFirstSpellInChain(spellId);
    for (uint8 step = 0; current && step < GUARDIAN_CHAIN_WALK; ++step)
    {
        if (uint32 remaining = player->GetSpellCooldownDelay(current))
        {
            if (cut >= remaining)
            {
                player->RemoveSpellCooldown(current, true);
                if (resetCategory)
                    if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(current))
                        if (uint32 category = rank->GetCategory())
                            player->RemoveCategoryCooldown(category);
            }
            else if (cut)
                player->ModifySpellCooldown(current, -int32(cut));
        }
        current = sSpellMgr->GetNextSpellInChain(current);
    }
}

// G-01 No Escape 806544 (Battle Rush 802197, -10 s), G-02 Wreck Formation 803127 effect 0 (reset of
// Hammer of the Law 704418), and the same defect on 520638 Battlemaster: Axes (Battle Rush, Advance),
// 802893 Death Ram Reset and 705363 Iron Guardian (Ram), 712369 Colossal Reprise (Broad Sweep).
class spell_ascension_guardian_cooldown_chain : public SpellScript
{
    PrepareSpellScript(spell_ascension_guardian_cooldown_chain);

    void Handle(SpellEffIndex effIndex)
    {
        Unit* hit = GetHitUnit();
        Player* player = hit ? hit->ToPlayer() : nullptr;
        SpellEffectInfo const& effect = GetSpellInfo()->Effects[effIndex];
        if (effect.Effect != SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN &&
            effect.Effect != SPELL_EFFECT_ASCENSION_RESET_COOLDOWN)
            return; // 803127 effects 1 and 2 (177, aura duration) stay native.
        bool const reset = effect.Effect == SPELL_EFFECT_ASCENSION_RESET_COOLDOWN;
        int32 const delta = reset ? 0 : GetEffectValue();
        // Anything this script does not model (another class, a lengthening) keeps the core handler.
        if (!player || player->getClass() != CLASS_GUARDIAN || effect.MiscValue <= 0 || delta > 0)
            return;
        PreventHitDefaultEffect(effIndex);
        uint64 const cut = delta ? uint64(-int64(delta)) : std::numeric_limits<uint64>::max();
        ReduceGuardianCooldownOverChain(player, uint32(effect.MiscValue), effect.MiscValueB != 0, cut);
    }

    void Register() override
    {
        // One hook on any effect: two hooks (165, 195) would each fail script validation, with an
        // Errors.log line at startup, on the spells that carry only the other one.
        OnEffectHitTarget += SpellEffectFn(spell_ascension_guardian_cooldown_chain::Handle, EFFECT_ALL,
            SPELL_EFFECT_ANY);
    }
};
}

void AddAscensionGuardianAbilityScripts()
{
    RegisterSpellScript(spell_ascension_guardian_ability);
    RegisterSpellScript(aura_ascension_guardian_advance);
    RegisterSpellScript(aura_ascension_guardian_hold_the_line);
    RegisterSpellScript(spell_ascension_guardian_cooldown_chain);
}
