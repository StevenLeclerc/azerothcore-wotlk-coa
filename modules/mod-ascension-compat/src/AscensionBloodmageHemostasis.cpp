/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include <algorithm>
#include <vector>

namespace
{
enum HemostasisSpells : uint32
{
    SPELL_HEMOSTASIS = 681304,
    SPELL_HEMOSTASIS_READY = 302895,
    SPELL_BLOOD_BURST = 803326,
    // Troisieme passe du 2026-09-28.
    SPELL_HEMAL_EXCISION = 803681,       // DISPEL curse (20) on an ally
    SPELL_EXCISION = 803733,             // e0 DUMMY on the enemy, e1 REMOVE_AURA 803734
    SPELL_HEMAL_EXCISION_BUFF = 803734   // the 10 s window
};

// Hemal Excision: "siphoning all curse effects from them. You may reactivate this ability again within
// $803734d to place these curse effects on an enemy." The dispel worked; the second half had no engine
// (803682, 803733 and the buff were reached by nothing). The siphoned curses are kept on the window aura
// itself (script values, gone with it), one packed entry each: spell id, stacks, remaining milliseconds.
constexpr uint32 KEY_EXCISION_COUNT = 1;
constexpr uint32 KEY_EXCISION_FIRST = 2;
constexpr uint32 ExcisionMaxCurses = 20;  // 803681 effect 0 dispels at most 20 (bp 19 + 1)

uint64 PackCurse(uint32 id, uint32 stacks, int32 duration)
{
    uint64 const ms = duration > 0 ? std::min<uint64>(uint64(duration), 0xFFFFFF) : 0;
    return (uint64(id) << 32) | (uint64(std::min<uint32>(stacks, 0xFF)) << 24) | ms;
}

Player* HemostasisCaster(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_SON_OF_ARUGAL ? player : nullptr;
}

Unit* HemostasisTarget(Player* player)
{
    if (!player || !player->IsAlive() || !player->IsInWorld() || !player->HasActiveSpell(SPELL_HEMOSTASIS))
        return nullptr;
    Aura* window = player->GetAura(SPELL_HEMOSTASIS_READY, player->GetGUID());
    if (!window)
        return nullptr;
    Unit* victim = ObjectAccessor::GetUnit(*player, ObjectGuid(window->GetScriptValue(SPELL_HEMOSTASIS)));
    return victim && victim->IsAlive() && player->InSamePhase(victim) &&
        player->IsValidAttackTarget(victim) && victim->HasAura(SPELL_HEMOSTASIS, player->GetGUID()) ? victim : nullptr;
}

class aura_ascension_hemostasis : public AuraScript
{
    PrepareAuraScript(aura_ascension_hemostasis);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_HEMOSTASIS_READY, SPELL_BLOOD_BURST}); }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = HemostasisCaster(GetCaster());
        if (!player || !player->HasActiveSpell(SPELL_HEMOSTASIS))
            return;
        if (Aura* window = player->AddAura(SPELL_HEMOSTASIS_READY, player))
        {
            window->SetScriptValue(SPELL_HEMOSTASIS, GetTarget()->GetGUID().GetRawValue());
            // Preserve any independent permanent or other-spec ownership of the helper.
            // Unannounced: the client sees Blood Burst only in the rewritten Hemostasis slot (P-144).
            if (player->GetSpellMap().find(SPELL_BLOOD_BURST) == player->GetSpellMap().end())
                player->LearnTemporarySpellReplacementTarget(SPELL_BLOOD_BURST);
            player->SetTemporarySpellReplacement(SPELL_HEMOSTASIS, SPELL_BLOOD_BURST);
            if (player->GetTemporarySpellReplacement(SPELL_HEMOSTASIS) != SPELL_BLOOD_BURST)
                player->RemoveAurasDueToSpell(SPELL_HEMOSTASIS_READY, player->GetGUID());
        }
    }

    void OnRemove(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Player* player = HemostasisCaster(GetCaster()))
            if (Aura* window = player->GetAura(SPELL_HEMOSTASIS_READY, player->GetGUID()))
                if (window->GetScriptValue(SPELL_HEMOSTASIS) == GetTarget()->GetGUID().GetRawValue())
                    player->RemoveAurasDueToSpell(SPELL_HEMOSTASIS_READY, player->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_hemostasis::Apply,
            EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_hemostasis::OnRemove,
            EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_hemostasis_ready : public AuraScript
{
    PrepareAuraScript(aura_ascension_hemostasis_ready);

    void OnRemove(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Player* player = HemostasisCaster(GetTarget()))
        {
            player->SetTemporarySpellReplacement(SPELL_HEMOSTASIS, 0);
            player->removeSpell(SPELL_BLOOD_BURST, SPEC_MASK_ALL, true);
        }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_hemostasis_ready::OnRemove,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class hemostasis_burst_target : public AllSpellScript
{
public:
    hemostasis_burst_target() : AllSpellScript("hemostasis_burst_target",
        {ALLSPELLHOOK_CAN_PREPARE, ALLSPELLHOOK_ON_SPELL_CHECK_CAST}) { }

    bool CanPrepare(Spell* spell, SpellCastTargets const*, AuraEffect const*) override
    {
        if (spell->GetSpellInfo()->Id != SPELL_BLOOD_BURST || spell->GetSpellInfo()->SpellFamilyName != 26)
            return true;
        Unit* victim = HemostasisTarget(HemostasisCaster(spell->GetCaster()));
        if (!victim)
            return false;
        // InitExplicitTargets already ran. Retarget before native range/LOS checks,
        // even if the player selected another unit or cleared their selection.
        spell->m_targets.SetUnitTarget(victim);
        return true;
    }

    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        if (spell->GetSpellInfo()->Id != SPELL_BLOOD_BURST || spell->GetSpellInfo()->SpellFamilyName != 26)
            return;
        Unit* victim = HemostasisTarget(HemostasisCaster(spell->GetCaster()));
        if (!victim || victim != spell->m_targets.GetUnitTarget())
            result = SPELL_FAILED_BAD_TARGETS;
    }
};

class spell_ascension_blood_burst : public SpellScript
{
    PrepareSpellScript(spell_ascension_blood_burst);

    void Release(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        // Native effect 164 removes every caster's copy of the named aura.
        if (Unit* victim = GetHitUnit())
            victim->RemoveAurasDueToSpell(SPELL_HEMOSTASIS, GetCaster()->GetGUID());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_blood_burst::Release, EFFECT_2, SPELL_EFFECT_REMOVE_AURA);
    }
};

// 803681 Hemal Excision: the curses are read before the dispel and compared after it, so that only what the
// dispel actually took (a resisted dispel takes nothing) is siphoned.
class spell_ascension_bloodmage_hemal_excision : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_hemal_excision);

    struct Curse { uint32 id; ObjectGuid caster; uint32 stacks; int32 duration; };
    std::vector<Curse> _before;

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_EXCISION, SPELL_HEMAL_EXCISION_BUFF});
    }

    void Snapshot(SpellMissInfo missInfo)
    {
        _before.clear();
        Unit* ally = GetHitUnit();
        if (missInfo != SPELL_MISS_NONE || !ally)
            return;
        for (auto const& pair : ally->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            if (aura->GetSpellInfo()->Dispel == DISPEL_CURSE && !pair.second->IsPositive() && !aura->IsPassive())
                _before.push_back({aura->GetId(), aura->GetCasterGUID(), std::max<uint32>(aura->GetStackAmount(), 1),
                    aura->GetDuration()});
        }
    }

    void Siphon()
    {
        Player* player = HemostasisCaster(GetCaster());
        Unit* ally = GetHitUnit();
        if (!player || !ally || _before.empty() || !player->IsAlive())
            return;
        std::vector<uint64> taken;
        for (Curse const& curse : _before)
        {
            if (taken.size() >= ExcisionMaxCurses)
                break;
            Aura const* left = ally->GetAura(curse.id, curse.caster);
            uint32 const remaining = left ? std::max<uint32>(left->GetStackAmount(), 1) : 0;
            if (remaining < curse.stacks)
                taken.push_back(PackCurse(curse.id, curse.stacks - remaining, curse.duration));
        }
        _before.clear();
        if (taken.empty())
            return;
        player->CastSpell(player, SPELL_HEMAL_EXCISION_BUFF, TRIGGERED_FULL_MASK);
        Aura* window = player->GetAura(SPELL_HEMAL_EXCISION_BUFF, player->GetGUID());
        if (!window)
            return;
        window->SetScriptValue(KEY_EXCISION_COUNT, taken.size());
        for (uint32 i = 0; i < taken.size(); ++i)
            window->SetScriptValue(KEY_EXCISION_FIRST + i, taken[i]);
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ascension_bloodmage_hemal_excision::Snapshot);
        AfterHit += SpellHitFn(spell_ascension_bloodmage_hemal_excision::Siphon);
    }
};

// 803734, the window: Hemal Excision's slot shows Excision 803733 while it lasts (Hemostasis idiom, P-144).
class aura_ascension_bloodmage_hemal_excision_window : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_hemal_excision_window);

    void Open(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = HemostasisCaster(GetTarget());
        if (!player || !player->HasActiveSpell(SPELL_HEMAL_EXCISION))
            return;
        if (player->GetSpellMap().find(SPELL_EXCISION) == player->GetSpellMap().end())
            player->LearnTemporarySpellReplacementTarget(SPELL_EXCISION);
        player->SetTemporarySpellReplacement(SPELL_HEMAL_EXCISION, SPELL_EXCISION);
    }

    void Close(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Player* player = HemostasisCaster(GetTarget()))
        {
            if (player->GetTemporarySpellReplacement(SPELL_HEMAL_EXCISION) == SPELL_EXCISION)
                player->SetTemporarySpellReplacement(SPELL_HEMAL_EXCISION, 0);
            // Only a temporary copy is removed (removeSpell's last argument).
            player->removeSpell(SPELL_EXCISION, SPEC_MASK_ALL, true);
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_hemal_excision_window::Open,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_hemal_excision_window::Close,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 803733 Excision: "Apply all siphoned curses onto an enemy." The Bloodmage becomes their caster, with the
// stacks and the time they had left, and the window closes. A miss or an immune target keeps the window
// (and the curses) until it expires.
class spell_ascension_bloodmage_excision : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_excision);

    void Place(SpellEffIndex /*index*/)
    {
        Player* player = HemostasisCaster(GetCaster());
        Unit* enemy = GetHitUnit();
        if (!player || !enemy || !enemy->IsAlive() || !player->IsValidAttackTarget(enemy))
            return;
        Aura* window = player->GetAura(SPELL_HEMAL_EXCISION_BUFF, player->GetGUID());
        if (!window)
            return;
        uint32 const count = uint32(std::min<uint64>(window->GetScriptValue(KEY_EXCISION_COUNT), ExcisionMaxCurses));
        window->SetScriptValue(KEY_EXCISION_COUNT, 0);
        for (uint32 i = 0; i < count && enemy->IsAlive(); ++i)
        {
            uint64 const packed = window->GetScriptValue(KEY_EXCISION_FIRST + i);
            uint32 const id = uint32(packed >> 32);
            uint32 const stacks = uint32((packed >> 24) & 0xFF);
            int32 const duration = int32(packed & 0xFFFFFF);
            if (!id)
                continue;
            Aura* curse = player->AddAura(id, enemy);
            if (!curse)
                continue;
            if (duration > 0)
            {
                if (duration > curse->GetMaxDuration())
                    curse->SetMaxDuration(duration);
                curse->SetDuration(duration);
            }
            if (stacks > 1 && curse->GetStackAmount() < stacks)
                curse->SetStackAmount(uint8(stacks));
        }
        // Closed here rather than by effect 1: the caster's own target entry can be resolved before the
        // enemy's (a travel time, or the target order), and would take the record away first.
        window->Remove();
    }

    void KeepWindow(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_bloodmage_excision::Place, EFFECT_0, SPELL_EFFECT_DUMMY);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_bloodmage_excision::KeepWindow, EFFECT_1,
            SPELL_EFFECT_REMOVE_AURA);
    }
};

class hemostasis_contracts : public GlobalScript
{
public:
    hemostasis_contracts() : GlobalScript("hemostasis_contracts", {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info && info->Id == SPELL_HEMOSTASIS_READY && info->SpellFamilyName == 26)
        {
            // The four-second replacement and its target GUID are transient.
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
        }
        // Hemal Excision's window holds the siphoned curses in memory only: a copy restored from
        // `character_aura` would reopen the button with nothing to place.
        if (info && info->Id == SPELL_HEMAL_EXCISION_BUFF && info->SpellFamilyName == 26)
        {
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
        }
    }
};
}

void AddSC_AscensionBloodmageHemostasis()
{
    RegisterSpellScript(aura_ascension_hemostasis);
    RegisterSpellScript(aura_ascension_hemostasis_ready);
    RegisterSpellScript(spell_ascension_blood_burst);
    new hemostasis_burst_target();
    new hemostasis_contracts();
    // Troisieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 5-7).
    RegisterSpellScript(spell_ascension_bloodmage_hemal_excision);
    RegisterSpellScript(aura_ascension_bloodmage_hemal_excision_window);
    RegisterSpellScript(spell_ascension_bloodmage_excision);
}
