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

// Weakened Soul lasts 15 s. This number is a DESIGN DECISION taken by the realm owner on
// 2026-09-21, not a reading: 803433 carries DurationIndex 21, and line 21 of
// SpellDuration.dbc is (-1, 0, -1) — infinite. No source writes a duration for it (neither
// tooltip, nor the client DBC, nor the upstream world dump coa-world-20260912.zip, nor
// baseline.json, nor the 4 436 entries of the upstream issue tracker; the Wayback archives
// of db.ascension.gg were offline when checked). It is applied here rather than through a
// `spell_dbc` row because that table needs a complete 234-column row and is read at startup
// only, while the module already owns this cast.
constexpr int32 WEAKENED_SOUL_DURATION = 15 * IN_MILLISECONDS;

// Every talent whose tooltip reads "generating a Reaped Soul ..." reacts here, on the
// resource aura itself. Two producers exist and neither is a proc: the declarative one
// (ModifyAuraStacks, AscensionCompat.cpp:2939 -> HandleAscensionReaperResource,
// AscensionReaperTalents.cpp:176 -> ModStackAmount) and the direct casts of 500363
// (soul capture, Final Requiem 680338, Sinister Litany).
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

    // CEILING GAP, measured, not guessed. This is NOT a corner case: it is the resting
    // state. Nothing spends a soul when the bar reaches 3 — SynchronizeThresholdResources
    // (AscensionCompat.cpp:3103, the threshold block at :3148-3153) only casts Soul
    // Infusion — so a Reaper sits at 3/3 between two spenders. Every award landing in that
    // window is clamped by Aura::ModStackAmount (SpellAuras.cpp:972-981) and still ends in
    // SetStackAmount(3) (SpellAuras.cpp:1002), so this handler runs with current ==
    // previous and leaves on the comparison at its top. Fatesealer and Spirit Culling
    // therefore pay NOTHING over a large share of playing time, which no tooltip
    // conditions, and they must NOT be described as repaired until this is closed.
    // The repair belongs to the PRODUCER, HandleAscensionReaperResource
    // (AscensionReaperTalents.cpp:176): it already reads `previous` and re-reads the final
    // stack, so it can signal an award that was clamped, i.e. one where the stack did not
    // move. That file is outside this file's scope, so the gap is recorded, not patched.
    // Eater of Souls is not affected the same way: its tooltip is a threshold and its ward
    // check below is what gates it.
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

        // Scope guard, not a pair of assignments: any early return added below by a later
        // hand, or anything travelling up out of Spell::prepare, would otherwise leave
        // _running at true and kill this script for the whole life of the aura, silently.
        struct RunGuard
        {
            bool& Flag;
            explicit RunGuard(bool& flag) : Flag(flag) { Flag = true; }
            ~RunGuard() { Flag = false; }
        } runGuard(_running);

        // One award can be worth several souls, and it arrives here as ONE call: two
        // ResourceGainRules grant 3 at once (AscensionCustomResourceData.h:421-424,
        // Wraithblade 805258 and Sinister Litany 806818), and both travel through a single
        // ModifyAuraStacks -> ModStackAmount -> SetStackAmount. The talents whose tooltip
        // reads "generating a Reaped Soul" are therefore paid once per soul, not once per
        // event, which is what lets Fatesealer reach its own stack cap.
        uint8 const gained = uint8(current - previous);
        for (uint8 i = 0; i < gained && Still(); ++i)
        {
            // Fatesealer: "Generating a Reaped Soul now reduces damage taken ..., stacking
            // u times." 705443 carries its own stack cap and duration; casting it is the
            // whole talent, and one cast per soul is what makes the stacking reachable.
            if (player->HasAura(SPELL_FATESEALER))
                player->CastSpell(player, SPELL_FATESEALER_WARD, true);

            // Spirit Culling: "... now has a $h% chance to summon a Spectral Scythe". $h is
            // the talent's own DBC ProcChance, rolled once per soul. 500576 is the Spirit
            // Culling variant of the summon and carries its own duration. The damage half of
            // the talent is a native ADD_FLAT_MODIFIER.
            // 500576 is listed in REAPER_ALL_SOUL_CONSUMERS (AscensionCompat.cpp:274), so the
            // cast MUST stay triggered = true. ConsumeReaperSouls (AscensionCompat.cpp:3014,
            // the list tested at l.3028) would otherwise RemoveAurasDueToSpell(500363) from
            // inside Aura::SetStackAmount, which goes on iterating its application list and
            // calls SetNeedClientUpdateForTargets on a removed aura (SpellAuras.cpp:943-966).
            // Only the triggered test at the top of OnSpellCast (AscensionCompat.cpp:2456)
            // keeps that out today, and Still() does not cover it: the damage would happen in
            // the core after this handler returns.
            if (Still() && player->HasAura(SPELL_SPIRIT_CULLING))
                if (SpellInfo const* culling = sSpellMgr->GetSpellInfo(SPELL_SPIRIT_CULLING))
                    if (roll_chance_i(int32(std::min<uint32>(culling->ProcChance, 100))))
                        player->CastSpell(player, SPELL_SPIRIT_CULLING_SCYTHE, true);
        }

        // Eater of Souls stays OUT of the loop on purpose: its tooltip reads "Reaching 3
        // Reaped Souls", a threshold, not a gain; "... This will not trigger while
        // already active." The 3 of the tooltip is Reaped Soul's own ceiling, read here rather than
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
//
// WIRED ON 2026-09-21 (2026_09_21_07_ascension_reaper_weakened_souls.sql). It had been
// withheld since 2026-09-20 because 803433 was unfit to ship on two counts. One is now
// settled, the other is knowingly accepted:
//
//   SETTLED — the debuff was PERMANENT. The realm owner set it to 15 s, and Weaken()
//   applies that duration on every proc (see WEAKENED_SOUL_DURATION above). It is a
//   decision, not a reading: no source anywhere writes a duration for this spell.
//
//   ACCEPTED, NOT FIXED — it is still NOT restricted to Shadow and Frost. That
//   restriction is NOT EXPRESSIBLE for aura 271: both of its consumers filter on the
//   caster GUID and IsAffectedOnSpell only, and never read MiscValue (measured, see the
//   block in Weaken()). Consequence, measured on Spell.dbc: family 36 holds 1 018 spells,
//   of which 529 touch neither Shadow nor Frost (487 of them physical). The +10% reaches
//   those too. Narrowing would mean inventing a mask the tooltip does not write.
//
// The startup line LOG_ERROR "Script named 'aura_ascension_weakened_souls' is not
// assigned in the database." (ScriptMgr.h:921-926) disappears with this wiring; its
// absence is now the marker that the two rows are in place.
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
        // TWO MEASURED DIVERGENCES FROM THE TOOLTIP. The first is settled, the second is
        // accepted knowingly — see the block above the class.
        //
        // 1. SETTLED. The debuff WAS permanent: 803433 carries DurationIndex 21 and line 21
        //    of SpellDuration.dbc is (-1, 0, -1), i.e. infinite; on a player it lasted until
        //    death or a dispel, on an NPC until the evade. It is now capped at
        //    WEAKENED_SOUL_DURATION right after the cast below. Still a decision and not a
        //    reading: no source writes a duration for this spell.
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
        //    row. The only non-invented value available is (0,8192,0), Soulrend's own
        //    SpellFamilyFlags, which frames the debuff on Soulrend alone rather than on the
        //    Shadow-and-Frost the tooltip claims; that is a design call, not a reading, so
        //    it is not made here. For contrast, 573320 "Soulrend / aura"
        //    carries the same aura 271 with mask (2,0,0) and is correctly framed.
        owner->CastSpell(victim, SPELL_WEAKENED_SOUL, true);
        // Both calls are required and in this order: Aura::IsPermanent() is
        // GetMaxDuration() == -1 (SpellAuras.h:163), so the aura stays permanent until the
        // max duration is overwritten. Re-applied on every proc, which refreshes the timer.
        if (Aura* debuff = victim->GetAura(SPELL_WEAKENED_SOUL, owner->GetGUID()))
        {
            debuff->SetMaxDuration(WEAKENED_SOUL_DURATION);
            debuff->SetDuration(WEAKENED_SOUL_DURATION);
        }
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
