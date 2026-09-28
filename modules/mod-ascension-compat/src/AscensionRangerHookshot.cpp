/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <map>
#include <mutex>

namespace
{
constexpr uint32 HookReady = 803857;
constexpr uint32 Hooked = 803852;
constexpr uint32 HookRoot = 800360;

Player* HookRanger(Unit* caster)
{
    Player* player = caster ? caster->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_RANGER ? player : nullptr;
}

Unit* HookTarget(Player* player)
{
    if (!player || !player->IsAlive() || !player->IsInWorld())
        return nullptr;
    Aura* ready = player->GetAura(HookReady, player->GetGUID());
    if (!ready || !player->HasActiveSpell(uint32(ready->GetScriptValue(HookRoot))))
        return nullptr;
    Unit* target = ObjectAccessor::GetUnit(*player, ObjectGuid(ready->GetScriptValue(Hooked)));
    return target && target->IsAlive() && player->InSamePhase(target) &&
        player->IsValidAttackTarget(target) ? target : nullptr;
}

class spell_ascension_hookshot : public SpellScript
{
    PrepareSpellScript(spell_ascension_hookshot);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({HookReady, Hooked}); }
    bool Load() override { return HookRanger(GetCaster()) != nullptr; }

    void Latch(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        Player* player = GetCaster()->ToPlayer();
        Unit* target = GetHitUnit();
        if (!target || !target->IsAlive() || !player->IsValidAttackTarget(target) ||
            !target->HasAuraState(AURA_STATE_BLEEDING))
            return;
        player->RemoveAurasDueToSpell(HookReady, player->GetGUID());
        if (Aura* ready = player->AddAura(HookReady, player))
        {
            uint32 original = GetSpellInfo()->Id;
            ready->SetScriptValue(HookRoot, original);
            ready->SetScriptValue(Hooked, target->GetGUID().GetRawValue());
            // Unannounced: the client sees Hooked only in the rewritten Hookshot slot (P-144).
            if (player->GetSpellMap().find(Hooked) == player->GetSpellMap().end())
                player->LearnTemporarySpellReplacementTarget(Hooked);
            player->SetTemporarySpellReplacement(original, Hooked);
            if (player->GetTemporarySpellReplacement(original) != Hooked)
                player->RemoveAurasDueToSpell(HookReady, player->GetGUID());
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_hookshot::Latch, EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

class aura_ascension_hookshot_ready : public AuraScript
{
    PrepareAuraScript(aura_ascension_hookshot_ready);

    void Clear(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = HookRanger(GetTarget());
        if (!player || GetCaster() != player)
            return;
        if (uint32 original = uint32(GetAura()->GetScriptValue(HookRoot)))
            player->SetTemporarySpellReplacement(original, 0);
        player->removeSpell(Hooked, SPEC_MASK_ALL, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_hookshot_ready::Clear,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class ranger_hookshot_casts : public AllSpellScript
{
public:
    ranger_hookshot_casts() : AllSpellScript("ranger_hookshot_casts",
        {ALLSPELLHOOK_CAN_PREPARE, ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_BEFORE_EFFECTS}) { }

    bool IsFollowup(Spell* spell) const
    {
        return spell->GetSpellInfo()->Id == Hooked && spell->GetSpellInfo()->SpellFamilyName == 27;
    }

    bool CanPrepare(Spell* spell, SpellCastTargets const*, AuraEffect const*) override
    {
        if (!IsFollowup(spell))
            return true;
        // Never refuse here: a false return ends the cast in Spell::prepare with no cast result
        // sent and the Spell never freed. OnSpellCheckCast refuses with BAD_TARGETS instead.
        if (Unit* target = HookTarget(HookRanger(spell->GetCaster())))
            spell->m_targets.SetUnitTarget(target);
        return true;
    }

    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        if (!IsFollowup(spell))
            return;
        Unit* target = HookTarget(HookRanger(spell->GetCaster()));
        if (!target || target != spell->m_targets.GetUnitTarget())
            result = SPELL_FAILED_BAD_TARGETS;
    }

    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const*) override
    {
        if (IsFollowup(spell))
            caster->RemoveAurasDueToSpell(HookReady, caster->GetGUID());
    }
};

// 800093 Skewer: "You may reactivate the ability to pull all marked enemies to your location. Each
// enemy pulled reduces the cooldown of your Hookshot by 3 sec." The reactivation is 800153
// "Skewer (Pull)", complete in the DBC (TRIGGER_SPELL -> 800148 on enemies within 40 yd, which
// requires the 800095 mark, grips, cuts Hookshot 800360 by 3 s and removes the mark): nothing put
// it on the bar. Same pattern as Hookshot above: after a Skewer that marked an enemy, the Skewer
// slot shows 800153 until it is used or the marks' duration (800095, read) has passed.
constexpr uint32 Skewer = 800093;
constexpr uint32 SkewerPull = 800153;
constexpr uint32 SkewerMark = 800095;

std::mutex skewerMutex;
std::map<ObjectGuid, Milliseconds> skewerExpiry;

void ClearSkewer(Player* player)
{
    {
        std::lock_guard<std::mutex> lock(skewerMutex);
        skewerExpiry.erase(player->GetGUID());
    }
    if (player->GetTemporarySpellReplacement(Skewer) == SkewerPull)
        player->SetTemporarySpellReplacement(Skewer, 0);
    player->removeSpell(SkewerPull, SPEC_MASK_ALL, true);
}

void ArmSkewer(Player* player)
{
    SpellInfo const* mark = sSpellMgr->GetSpellInfo(SkewerMark);
    int32 duration = mark ? mark->GetMaxDuration() : 0;
    if (duration <= 0 || !player->HasActiveSpell(Skewer))
        return;
    // Unannounced: the client sees Skewer (Pull) only in the rewritten Skewer slot (P-144).
    if (player->GetSpellMap().find(SkewerPull) == player->GetSpellMap().end())
        player->LearnTemporarySpellReplacementTarget(SkewerPull);
    player->SetTemporarySpellReplacement(Skewer, SkewerPull);
    if (player->GetTemporarySpellReplacement(Skewer) != SkewerPull)
    {
        ClearSkewer(player);
        return;
    }
    std::lock_guard<std::mutex> lock(skewerMutex);
    skewerExpiry[player->GetGUID()] = GameTime::GetGameTimeMS() + Milliseconds(duration);
}

class spell_ascension_ranger_skewer : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_skewer);

    bool _armed = false;

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SkewerPull, SkewerMark}); }
    bool Load() override { return HookRanger(GetCaster()) != nullptr; }

    void Mark(SpellEffIndex /*effIndex*/)
    {
        // The native trigger (LAUNCH_TARGET) has already put the mark; this only arms the slot.
        Player* player = GetCaster()->ToPlayer();
        Unit* target = GetHitUnit();
        if (_armed || !target || !target->IsAlive() || !player->IsValidAttackTarget(target))
            return;
        _armed = true;
        ArmSkewer(player);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_ranger_skewer::Mark, EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

class spell_ascension_ranger_skewer_pull : public SpellScript
{
    PrepareSpellScript(spell_ascension_ranger_skewer_pull);

    bool Load() override { return HookRanger(GetCaster()) != nullptr; }

    void Used()
    {
        ClearSkewer(GetCaster()->ToPlayer());
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_ranger_skewer_pull::Used);
    }
};

class ranger_skewer_lifecycle : public PlayerScript
{
public:
    ranger_skewer_lifecycle() : PlayerScript("ranger_skewer_lifecycle",
        {PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT}) { }

    void OnPlayerUpdate(Player* player, uint32) override
    {
        if (player->getClass() != CLASS_RANGER || player->GetTemporarySpellReplacement(Skewer) != SkewerPull)
            return;
        bool expired = true;
        {
            std::lock_guard<std::mutex> lock(skewerMutex);
            auto itr = skewerExpiry.find(player->GetGUID());
            if (itr != skewerExpiry.end())
                expired = GameTime::GetGameTimeMS() >= itr->second;
        }
        if (expired || !player->IsAlive())
            ClearSkewer(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(skewerMutex);
        skewerExpiry.erase(player->GetGUID());
    }
};

class ranger_hookshot_contracts : public GlobalScript
{
public:
    ranger_hookshot_contracts() : GlobalScript("ranger_hookshot_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info && info->Id == HookReady && info->SpellFamilyName == 27)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
    }
};
}

void AddSC_AscensionRangerHookshot()
{
    RegisterSpellScript(spell_ascension_hookshot);
    RegisterSpellScript(aura_ascension_hookshot_ready);
    new ranger_hookshot_casts();
    new ranger_hookshot_contracts();
    RegisterSpellScript(spell_ascension_ranger_skewer);
    RegisterSpellScript(spell_ascension_ranger_skewer_pull);
    new ranger_skewer_lifecycle();
}
