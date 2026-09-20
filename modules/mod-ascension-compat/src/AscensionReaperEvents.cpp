/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>

namespace
{
enum ReaperEventSpells : uint32
{
    // Reaped Soul is the class resource, not an ability: its stack count is owned by
    // ResourceGainRules / AscensionCompat.cpp. Nothing here ever changes that count,
    // it only reacts to it. See docs/CONCEPTION-evenements-de-classe.md section 5.2.
    SPELL_REAPED_SOUL = 500363,
    SPELL_SOUL_INFUSION = 803031,

    SPELL_FATESEALER = 705442,
    SPELL_FATESEALER_WARD = 705443,
    SPELL_SPIRIT_CULLING = 301986,
    SPELL_SPIRIT_CULLING_SCYTHE = 500576,
    SPELL_EATER_OF_SOULS = 805181,
    SPELL_EATER_OF_SOULS_WARD = 805182,
    SPELL_DOMINION = 803999,
    SPELL_DOMINION_ARMOR = 804000,
    SPELL_WEAKENED_SOULS = 92146,
    SPELL_WEAKENED_SOUL = 803433
};

// Every talent whose tooltip reads "generating a Reaped Soul ..." reacts here, on the
// resource aura itself. Two producers exist and neither is a proc: the declarative one
// (AscensionCompat.cpp ModifyAuraStacks -> HandleAscensionReaperResource -> ModStackAmount)
// and the direct casts of 500363 (soul capture, Final Requiem 680338, Sinister Litany).
// Both end in Aura::SetStackAmount or Unit::_ApplyAura, and both therefore reach an aura
// effect handler: SetStackAmount calls AuraEffect::ChangeAmount(onStackOrReapply = true),
// which calls HandleEffect with AURA_EFFECT_HANDLE_REAPPLY, which calls the apply hooks
// (SpellAuraEffects.cpp AuraEffect::ChangeAmount / AuraEffect::HandleEffect). Hooking the
// resource is what keeps the soul count single-sourced.
class aura_ascension_reaper_soul_events : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_soul_events);

    // Per-aura state: the script object lives exactly as long as the Reaped Soul aura.
    // Spending the last soul removes the aura, so the next application legitimately
    // restarts from zero, which is a real gain.
    uint8 _seen = 0;
    bool _running = false;

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_FATESEALER_WARD, SPELL_SPIRIT_CULLING_SCYTHE,
            SPELL_EATER_OF_SOULS_WARD, SPELL_SPIRIT_CULLING});
    }

    // A cast issued below could, through no-stack rules, take the resource aura away under
    // this handler. Re-check before every further cast rather than assume.
    bool Still() const
    {
        Aura const* aura = GetAura();
        return aura && !aura->IsRemoved();
    }

    // KNOWN GAP, measured, not guessed: at the ceiling the three talents in this handler are dead.
    // Crossing 3 souls does NOT spend them (AscensionCompat.cpp:3148-3152 only casts Soul
    // Infusion), so 3/3 is the resting state. A further award reaches Aura::ModStackAmount,
    // which clamps stackAmount to maxStackAmount (SpellAuras.cpp:972-981) and then calls
    // SetStackAmount(3) anyway (SpellAuras.cpp:1002); this handler therefore runs with
    // current == previous and leaves on the comparison at its top. Fatesealer, Spirit Culling and
    // Eater of Souls pay nothing for every "generating a Reaped Soul" that lands on a full
    // bar, which no tooltip conditions. Repairing it belongs to the PRODUCER side
    // (HandleAscensionReaperResource / ApplyGainRule in AscensionCompat.cpp), which would
    // have to signal a clamped award, or move the talents onto an award hook instead of the
    // stack. Both files are outside this file's scope, so the gap is recorded, not patched.
    void Harvested(AuraEffect const*, AuraEffectHandleModes)
    {
        uint8 previous = _seen;
        uint8 current = GetStackAmount();
        // Record first: a spend, a capped award and a reapply are not a harvest, and a
        // nested change must not be replayed by the outer frame.
        _seen = current;
        if (_running || current <= previous)
            return;

        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        // IsInWorld() also rules out the login replay, and the order above matters for it:
        // PlayerStorage.cpp:5908-5909 calls SetLoadedState before ApplyForTargets, so this
        // handler runs once with the saved stack already in place while the player is not
        // yet on a map. Recording the count before the guard is what stops a character
        // logging in on three souls from being paid as if it had just harvested them.
        if (!player || player->getClass() != CLASS_REAPER || !player->IsInWorld() || !player->IsAlive())
            return;

        _running = true;

        // Fatesealer: "Generating a Reaped Soul now reduces damage taken ..., stacking u times."
        // 705443 carries its own stack cap and duration; casting it is the whole talent.
        if (player->HasAura(SPELL_FATESEALER))
            player->CastSpell(player, SPELL_FATESEALER_WARD, true);

        // Spirit Culling: "... now has a $h% chance to summon a Spectral Scythe". $h is the
        // talent's own DBC ProcChance; 500576 is the Spirit Culling variant of the summon and
        // carries its own duration. The damage half of the talent is a native ADD_FLAT_MODIFIER.
        // 500576 is listed in REAPER_ALL_SOUL_CONSUMERS (AscensionCompat.cpp:274), so the
        // cast MUST stay triggered = true. ConsumeReaperSouls (AscensionCompat.cpp:3014, la liste testee l.3028)
        // would otherwise RemoveAurasDueToSpell(500363) from inside Aura::SetStackAmount,
        // which goes on iterating its application list and calls SetNeedClientUpdateForTargets
        // on a removed aura (SpellAuras.cpp:943-966). Only the triggered test at the top of
        // OnSpellCast (AscensionCompat.cpp:2456) keeps that out today, and Still() does not
        // cover it: the damage would happen in the core after this handler returns.
        if (Still() && player->HasAura(SPELL_SPIRIT_CULLING))
            if (SpellInfo const* culling = sSpellMgr->GetSpellInfo(SPELL_SPIRIT_CULLING))
                if (roll_chance_i(int32(std::min<uint32>(culling->ProcChance, 100))))
                    player->CastSpell(player, SPELL_SPIRIT_CULLING_SCYTHE, true);

        // Eater of Souls: "Reaching 3 Reaped Souls ... This will not trigger while already
        // active." The 3 of the tooltip is Reaped Soul's own ceiling, read here rather than
        // written down, and read the way the core reads it: Aura::ModStackAmount clamps on
        // CalcMaxAuraStacks(GetCaster()) (SpellAuras.cpp:972), not on the raw DBC StackAmount,
        // so a modifier that ever raised the cap is followed for free. Today both give 3.
        // No "previous < full" here. Measured, not assumed: that test changes NOTHING today.
        // Reaching this line already means current > previous (the guard at the top) and
        // current >= full, and current cannot exceed the ceiling, so previous < full always
        // holds. What actually stops the ward from ever coming back is the ceiling gap
        // documented above, not this test. The test is dropped anyway, for two reasons: it
        // states a condition the tooltip does not ("This will not trigger while already
        // active" is exactly the ward check below, nothing more), and it is the test that
        // WOULD block rearming the day the ceiling gap is repaired producer-side, since any
        // such repair has to let a clamped award through with previous == current == full.
        // 805182 lasts 10 s (DurationIndex 1 = 10000 ms), so that day it must be able to
        // come back.
        uint32 full = GetSpellInfo()->CalcMaxAuraStacks(GetCaster());
        if (Still() && full && current >= full && player->HasAura(SPELL_EATER_OF_SOULS) &&
            !player->HasAura(SPELL_EATER_OF_SOULS_WARD))
            player->CastSpell(player, SPELL_EATER_OF_SOULS_WARD, true);

        _running = false;
    }

    void Register() override
    {
        // EFFECT_ALL / SPELL_AURA_ANY on purpose: 500363 carries a single aura effect today
        // (effect 1, ADD_FLAT_MODIFIER), and a hook bound to a stale index would be silently
        // dead. The stack comparison makes repeated calls idempotent.
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_soul_events::Harvested,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
    }
};

// Dominion: "Gaining Soul Infusion now increases your Armor by ... for 804000d."
// Soul Infusion is an aura, not a proc event, so the hook is its own application.
class aura_ascension_reaper_soul_infusion : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_soul_infusion);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_DOMINION_ARMOR});
    }

    void Infused(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !player->IsInWorld() ||
            !player->IsAlive() || !player->HasAura(SPELL_DOMINION))
            return;
        // 804000 carries the armour amount and the duration; it refreshes on its own.
        player->CastSpell(player, SPELL_DOMINION_ARMOR, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_soul_infusion::Infused,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Weakened Souls: "Your Soulrend now also applies Weakened Soul". The proc window is a
// spell_proc row (family 36, Soulrend's family bit, melee spell damage, hit phase); the
// row disables effect 1, a private Ascension aura 354 whose core handler is nullptr.
class aura_ascension_weakened_souls : public AuraScript
{
    PrepareAuraScript(aura_ascension_weakened_souls);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_WEAKENED_SOUL});
    }

    // No GetDamage() test on purpose: a fully absorbed hit must still apply the debuff.
    // The consequence is worth writing down, because it will be mistaken for a bug in game.
    // A training dummy zeroes the damage (npc_training_dummy::DamageTaken, the trap already
    // recorded at AscensionCompat.cpp:2533), the core then computes SpellTypeMask =
    // PROC_SPELL_TYPE_NO_DMG_HEAL (Unit.cpp:7130-7133) and the SpellTypeMask = 1 of the
    // spell_proc row for 92146 rejects it. Weakened Souls cannot be observed on a dummy.
    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        return owner && owner->IsPlayer() && owner->getClass() == CLASS_REAPER && owner->IsAlive() &&
            owner->IsInWorld() && event.GetActor() == owner && victim && victim != owner &&
            victim->IsAlive() && !owner->IsFriendlyTo(victim) && event.GetDamageInfo();
    }

    void Weaken(AuraEffect const*, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        if (!owner || !victim)
            return;
        // 803433 holds the percentage (effect 0, BasePoints 9 -> 10%). The caster is the
        // Reaper, which is what "damage taken from you" needs.
        //
        // TWO MEASURED DIVERGENCES FROM THE TOOLTIP, neither of them repairable from here:
        //
        // 1. The debuff is PERMANENT. 803433 carries DurationIndex 21 and line 21 of
        //    SpellDuration.dbc is (-1, 0, -1), i.e. infinite. On a player it lasts until
        //    death or a dispel, on an NPC until the evade. No source anywhere writes a
        //    duration for it: neither the tooltip of 92146 nor that of 803433 announces one,
        //    so none is invented here. Giving it one means a `spell_dbc` row on 803433,
        //    which is data on a spell outside this file's scope.
        //
        // 2. It is NOT limited to Shadow and Frost. Effect 0 is aura 271
        //    SPELL_AURA_MOD_DAMAGE_FROM_CASTER, and that aura never reads its MiscValue: its
        //    only two consumers filter on the caster GUID and on IsAffectedOnSpell
        //    (Unit.cpp:9365-9370 and 10869-10872), which is SpellFamilyName plus
        //    EffectSpellClassMask (SpellAuraEffects.cpp:1199-1207 -> SpellInfo::IsAffected,
        //    SpellInfo.cpp:1422-1434). The EffectSpellClassMask of effect 0 is (0,0,0), and
        //    IsAffected skips the flag test on a null mask, so the 10% applies to EVERY
        //    family-36 spell of the caster, Reap included. The MiscValue 48 (Shadow+Frost)
        //    is dead data for this aura type; only MOD_POWER_COST_SCHOOL and
        //    REFLECT_SPELLS_SCHOOL read a MiscValue as a school mask. Both consumers also
        //    require a spellProto, so automatic weapon swings are not affected. Narrowing it
        //    for real means giving effect 0 an EffectSpellClassMask through a `spell_dbc`
        //    row, again outside this file's scope. For contrast, 573320 "Soulrend / aura"
        //    carries the same aura 271 with mask (2,0,0) and is correctly framed.
        owner->CastSpell(victim, SPELL_WEAKENED_SOUL, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_weakened_souls::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_weakened_souls::Weaken, EFFECT_0, SPELL_AURA_DUMMY);
    }
};
} // namespace

void AddSC_AscensionReaperEvents()
{
    RegisterSpellScript(aura_ascension_reaper_soul_events);
    RegisterSpellScript(aura_ascension_reaper_soul_infusion);
    RegisterSpellScript(aura_ascension_weakened_souls);
}
