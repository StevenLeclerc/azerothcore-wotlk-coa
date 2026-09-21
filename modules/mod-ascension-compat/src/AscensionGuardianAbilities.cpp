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
            player->CastSpell(player, 807140, true);
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
}

void AddAscensionGuardianAbilityScripts()
{
    RegisterSpellScript(spell_ascension_guardian_ability);
    RegisterSpellScript(aura_ascension_guardian_advance);
}
