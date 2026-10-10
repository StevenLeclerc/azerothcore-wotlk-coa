/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionXoroth.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
namespace
{
using namespace AscensionXoroth;
bool First(AuraEffect const* effect)
{
    for (uint8 i = 0; i < effect->GetEffIndex(); ++i)
        if (effect->GetBase()->HasEffect(i))
            return false;
    return true;
}
bool Mark(SpellInfo const* info)
{
    // Greater Mark of Rivendare 803730 carries the Mark bit but SpellFamilyName 36 in Spell.dbc.
    return info && (info->Id == 803730 || (info->SpellFamilyName == 23 && (info->SpellFamilyFlags[1] & 2147483648u)));
}
class aura_ascension_xoroth_lifecycle : public AuraScript
{
    PrepareAuraScript(aura_ascension_xoroth_lifecycle);
    void Calculate(AuraEffect const* effect, int32& amount, bool&)
    {
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetId();
        if (id == 520294 && effect->GetEffIndex() == EFFECT_1)
            amount = int32(amount * (1 + .2f * State(player).blood));
        if (id == 805680 && effect->GetEffIndex() == EFFECT_0)
        {
            // $800999s3 per Demon's Blood stack (its native 317 is dummied), plus Hellwrath's $s2.
            float per = float(Amount(800999, EFFECT_2)) + (player->HasAura(804014) ? float(Amount(804014, EFFECT_1)) : 0.0f);
            amount = int32(amount * (1 + per / 100.0f * State(player).blood));
        }
        if (id == 803889)
            amount = int32(amount * (1 + .2f * State(player).fire));
    }
    void Apply(AuraEffect const* effect, AuraEffectHandleModes)
    {
        // Xorothian Warsteed applies its mounted speed (effect 0) before it mounts (effect 1), when the speed
        // update still reads the unmounted modifiers. Update the run speed again once mounted.
        if (GetId() == 804703 && effect->GetEffIndex() == EFFECT_1)
            GetTarget()->UpdateSpeed(MOVE_RUN, true);
        if (!First(effect))
            return;
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetId();
        if (Pestilence(id) || Mark(GetSpellInfo()))
        {
            std::vector<Aura*> old;
            for (auto const& pair : GetTarget()->GetAppliedAuras())
            {
                Aura* aura = pair.second->GetBase();
                if (aura != GetAura() && aura->GetCasterGUID() == player->GetGUID() &&
                    (Pestilence(id) ? Pestilence(aura->GetId()) : Mark(aura->GetSpellInfo())))
                    old.push_back(aura);
            }
            for (Aura* aura : old)
                aura->Remove();
        }
        if (GetTarget() != player)
            return;
        GetAura()->SetScriptValue(500906, ++State(player).sequence);
        if (id == 681184)
            GetAura()->SetScriptValue(id, 2);
        if (id == 524913)
            GetAura()->SetScriptValue(id, 5);
        if (id == 524920)
            // Burning Swings 707632 is dummied (its SPELLMOD_CHARGES would end Burning Blade after 4 attacks).
            GetAura()->SetScriptValue(id, 6 + State(player).fire +
                                              (player->HasAura(707632) ? uint32(std::max(0, Amount(707632))) : 0));
        if (id == 712294)
            Replace(player, 800340, 504581);
        if (id == 800999 || id == 92104)
            player->UpdateMaxHealth();
        if (id == 804703)
            State(player).timers.RescheduleEvent(id, 3s);
        if (id == 805679)
            return;
        Refresh(player);
    }
    void Tick(AuraEffect const* effect)
    {
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetId();
        if (id == 804703 && effect->GetEffIndex() == EFFECT_2)
        {
            PreventDefaultAction();
            uint32 maximum = player->GetLevel() >= 40 ? 5 : player->GetLevel() >= 30 ? 3 : 2;
            if (!State(player).timers.HasTimeUntilEvent(id) && !player->HasAura(805746) &&
                Count(player, 804787) < maximum)
                Cast(player, player, 804787);
        }
        if (Pestilence(id) && effect->GetEffIndex() == EFFECT_0)
        {
            PreventDefaultAction();
            uint32 child = id == 801053 ? 801071 : id == 802344 ? 802347 : id == 804786 ? 804801 : 0;
            // The child's own radius: Conquest $801071a1 = 12 yd, Famine 10 yd, Apocalypse $804801a3 = 8 yd.
            float radius = 0.0f;
            if (SpellInfo const* info = child ? sSpellMgr->GetSpellInfo(child) : nullptr)
                for (auto const& fx : info->Effects)
                    radius = std::max(radius, fx.CalcRadius(player));
            if (radius <= 0.0f)
                radius = 10.0f;
            if (child)
                for (Unit* enemy : Nearby(player, radius))
                    if (player->IsValidAttackTarget(enemy))
                        if (Aura* aura = player->AddAura(child, enemy))
                            aura->SetDuration(1500);
        }
    }
    void Remove(AuraEffect const* effect, AuraEffectHandleModes)
    {
        if (!First(effect))
            return;
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetId();
        auto mode = GetTargetApplication()->GetRemoveMode();
        if (Named(GetSpellInfo(), 806965) && mode == AURA_REMOVE_BY_EXPIRE && player->HasAura(802619) &&
            GetTarget()->IsAlive())
        {
            uint64 amount = GetAura()->GetScriptValue(802619) * std::max(0, Amount(802619)) / 100;
            if (amount)
                for (Unit* target : Nearby(GetTarget(), 8))
                    if (player->IsValidAttackTarget(target))
                        Copy(player, target, 802620, uint32(std::min<uint64>(INT32_MAX, amount)));
        }
        if (GetTarget() != player)
            return;
        if (id == 681184 && player->IsAlive() && (mode == AURA_REMOVE_BY_DEFAULT || mode == AURA_REMOVE_BY_EXPIRE))
            Cast(player, player, Highest(player, 800340));
        if (id == 712294)
            Replace(player, 800340, 0);
        if (id == 520295)
            player->RemoveAurasDueToSpell(520752);
        if (id == 800999 || id == 92104)
            player->UpdateMaxHealth();
        if (id == 805746 && mode == AURA_REMOVE_BY_EXPIRE && !player->HasAura(804785))
            player->RemoveAurasDueToSpell(804787);
        if (id == 804703)
        {
            player->RemoveAurasDueToSpell(804787);
            player->RemoveAurasDueToSpell(805746);
        }
        // P-194 (10/10/2026) : jamais de Refresh synchrone depuis un retrait d'aura. Refresh repose
        // des auras (707666 depuis P-162, 805916, les auras d'echelle) ; appele pendant une boucle de
        // retrait du coeur (Unit::RemoveArenaAuras a l'entree en arene, qui repart du debut apres
        // chaque retrait), il remettait l'aura que la boucle venait d'oter : boucle sans fin, fil
        // monde fige. On rend l'evenement 1 echu : OnPlayerUpdate fait le Refresh au tick suivant.
        State(player).timers.CancelEvent(1);
    }
    void Register() override
    {
        DoEffectCalcAmount +=
            AuraEffectCalcAmountFn(aura_ascension_xoroth_lifecycle::Calculate, EFFECT_ALL, SPELL_AURA_ANY);
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_xoroth_lifecycle::Apply, EFFECT_ALL, SPELL_AURA_ANY,
                                              AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_xoroth_lifecycle::Tick, EFFECT_ALL, SPELL_AURA_ANY);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_xoroth_lifecycle::Remove, EFFECT_ALL, SPELL_AURA_ANY,
                                                AURA_EFFECT_HANDLE_REAL);
    }
};
class aura_ascension_xoroth_block : public AuraScript
{
    PrepareAuraScript(aura_ascension_xoroth_block);
    void Calculate(AuraEffect const*, int32& amount, bool&)
    {
        amount = -1;
    }
    void Absorb(AuraEffect*, DamageInfo& damage, uint32& amount)
    {
        amount = damage.GetBlock() ? CalculatePct(damage.GetDamage(), 20) : 0;
    }
    void Register() override
    {
        DoEffectCalcAmount +=
            AuraEffectCalcAmountFn(aura_ascension_xoroth_block::Calculate, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_xoroth_block::Absorb, EFFECT_0);
    }
};
// Hellknight 800703: its movement speed is read in Unit::UpdateSpeed from the GCD effect's amount (-5 per stack).
// Refresh the run speed on apply, on every stack change (REAPPLY, amount already updated) and on removal (the
// application is already out of m_appliedAuras in AfterEffectRemove, so the bonus is dropped).
class aura_ascension_xoroth_hellknight : public AuraScript
{
    PrepareAuraScript(aura_ascension_xoroth_hellknight);
    void Refresh(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Unit* target = GetTarget())
            target->UpdateSpeed(MOVE_RUN, true);
    }
    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_xoroth_hellknight::Refresh, EFFECT_0,
                                              SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_xoroth_hellknight::Refresh, EFFECT_0,
                                                SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};
} // namespace
void AddSC_AscensionXorothAuras()
{
    RegisterSpellScript(aura_ascension_xoroth_lifecycle);
    RegisterSpellScript(aura_ascension_xoroth_block);
    RegisterSpellScript(aura_ascension_xoroth_hellknight);
}
