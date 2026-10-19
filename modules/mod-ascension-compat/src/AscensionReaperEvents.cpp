/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionSpellSafe.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

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
    SPELL_WEAKENED_SOUL = 803433,
    SPELL_BLOOD_HARVEST = 504565,  // e0 HEAL, Harvester's lifesteal heal (AscensionReaperTalents.cpp)

    SPELL_SOUL_HARVESTER = 804311,
    SPELL_SOUL_HARVESTER_REGEN = 804312,
    SPELL_TORMENTOR = 92147,
    SPELL_TORMENTOR_AMOUNT = 504558,
    SPELL_TORMENTED_SOULS = 500483,
    SPELL_SOUL_HARVEST = 504012,
    SPELL_SOUL_HARVEST_BLAST = 573050,
    SPELL_PAINBRINGER = 680995,
    SPELL_PAINBRINGER_TRIGGER = 520533,
    SPELL_PAINBRINGER_EXTEND = 520877,
    SPELL_MASOCHISTIC_RAGE = 570097,
    SPELL_PURGATORY = 504046,
    SPELL_PURGATORY_BUFF = 504047,
    SPELL_DAMNED = 706786,
    SPELL_DAMNED_SPEED = 560420
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

// K7 (19/10/2026, accord du commanditaire, coa-project docs/DIAG-equilibrage-classes-20261017.md
// § 3) : Weakened Souls 92146, passif de la spe Soul (55), rend 5 % des degats infliges, sur le
// modele de Harvester 500283 (15 %, aura_ascension_harvester). DECISION, pas lecture : aucun champ
// du DBC ne porte ce 5 %, d'ou la constante. Le soin passe par Blood Harvest 504565, comme Harvester.
constexpr uint32 WEAKENED_SOULS_LIFESTEAL_PCT = 5;
constexpr uint32 SPELLFAMILY_REAPER = 36;
constexpr uint32 SOULREND_FAMILY_FLAGS_1 = 8192;  // SpellFamilyFlags[1] des 9 Soulrend (Spell.dbc, lu le 19/10)

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

            // Painbringer 680995: "Generating Reaped Souls now has a $h% chance to trigger
            // Masochistic Rage for $520533s1 sec. If Masochistic Rage is already active, it
            // will instead extend the duration by $520877s1 sec." ProcFlags 0 and no spell_proc
            // row: the talent has no engine but this one, so no row may ever be added (double
            // roll). 520533 is effect 178, EffectNULL in the core (SpellEffects.cpp:250): its
            // 5 s is read here and applied to 570097. 520877 is effect 177, native
            // (EffectAscensionModifyAuraDuration). $h goes through SPELLMOD_CHANCE_OF_SUCCESS.
            if (Still() && player->HasAura(SPELL_PAINBRINGER))
                if (SpellInfo const* painbringer = sSpellMgr->GetSpellInfo(SPELL_PAINBRINGER))
                {
                    int32 chance = int32(std::min<uint32>(painbringer->ProcChance, 100));
                    player->ApplySpellMod(SPELL_PAINBRINGER, SPELLMOD_CHANCE_OF_SUCCESS, chance);
                    if (roll_chance_i(chance))
                    {
                        if (player->HasAura(SPELL_MASOCHISTIC_RAGE, player->GetGUID()))
                            player->CastSpell(player, SPELL_PAINBRINGER_EXTEND, true);
                        else if (SpellInfo const* trigger = sSpellMgr->GetSpellInfo(SPELL_PAINBRINGER_TRIGGER))
                        {
                            // K9 (19/10/2026, accord du commanditaire, docs/DIAG-equilibrage-classes-20261017.md
                            // § 3) : le proc ne pose plus que l'AURA de 570097 (effet 1, +20 % de degats).
                            // CastSpell lancait le sort entier, dont l'effet 0 (SCHOOL_DAMAGE 420 sur
                            // TARGET_UNIT_CASTER) : des degats sur soi a chaque proc, mesures au banc ia3
                            // (docs/SYNTHESE-banc-soins-ia3-20261019.md § 3). Unit::AddAura ne garde que les
                            // effets d'aura (Aura::BuildEffectMaskForOwner), donc jamais l'effet 0. Le sort
                            // actif Masochistic Rage 803030, qui declenche lui aussi 570097, n'est pas touche.
                            Aura* rage = player->AddAura(SPELL_MASOCHISTIC_RAGE, player);
                            // The maximum stays 570097's own 15 s, so the +3 s extensions keep
                            // room to grow under it on the client's timer.
                            if (rage)
                                rage->SetDuration(std::clamp(trigger->Effects[EFFECT_0].CalcValue(player), 1,
                                    std::max(1, rage->GetMaxDuration())));
                        }
                    }
                }
        }

        // Soul Harvest 504012: "Generating Reaped Souls now triggers Soul Harvest" (573050,
        // "Blasts your target ..., healing for 100% of the damage dealt"). Once per gain, not
        // once per soul: a 3-soul award (Wraithblade, Sinister Litany) strikes once. 573050
        // aims at an enemy (implicit target 6), so it goes to the Reaper's victim, never to
        // himself. Same engine rule as Painbringer: no spell_proc row on 504012, ever.
        if (Still() && player->HasAura(SPELL_SOUL_HARVEST))
            if (SpellInfo const* blast = sSpellMgr->GetSpellInfo(SPELL_SOUL_HARVEST_BLAST))
            {
                Unit* victim = player->GetVictim();
                if (!victim)
                    victim = player->GetSelectedUnit();
                if (victim && victim != player && victim->IsAlive() && player->IsValidAttackTarget(victim) &&
                    player->IsWithinDistInMap(victim, blast->GetMaxRange(false, player)) &&
                    player->IsWithinLOSInMap(victim))
                    player->CastSpell(victim, SPELL_SOUL_HARVEST_BLAST, true);
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

// Soul Infusion is an aura, not a proc event: gaining it is its own application, and
// consuming it is its removal. Every talent below reads one of the two, never a spell_proc
// row (it would be a second engine).
//   Dominion 803999: "Gaining Soul Infusion now increases your Armor by ... for 804000d."
//   Purgatory 504046: "When you gain Soul Infusion, the damage of your Soulrend and Reliquary
//     of the Lost is increased by $504047s1% for $504047d, stacking $504047u times."
//   Damned 706786: "When you gain Soul Infusion, your movement speed is increased by
//     $560420s2% for $560420d."
//   Tormentor 92147 (Domination, level 10 passive): "Consuming Soul Infusion now reduces the
//     cooldown of Tormented Souls by $/1000;504558s1 sec." Its two DBC effects point at other
//     spells (359019, 805077) and have no proc; the value lives in 504558 e0 (effect 165
//     MODIFY_COOLDOWN, misc 500483, -3000 ms), which is never cast: its e1 would also grant
//     mana. Consumption is RemoveAurasDueToSpell (AscensionCompat.cpp ConsumeReaperSouls) or
//     561290 effect 164 (EffectRemoveAura): both remove with AURA_REMOVE_BY_DEFAULT. Expiry,
//     death and a cancel are not a consumption. Tormented Souls consumes Soul Infusion itself
//     before its own cooldown is set: the remaining delay is then 0 and nothing happens.
// The new ids are deliberately NOT added to Validate(): a missing one would destroy the whole
// script, Dominion included. Each is tested where it is used.
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
        if (!player || player->getClass() != CLASS_REAPER || !player->IsInWorld() || !player->IsAlive())
            return;
        // 804000 carries the armour amount and the duration; it refreshes on its own.
        if (player->HasAura(SPELL_DOMINION))
            player->CastSpell(player, SPELL_DOMINION_ARMOR, true);
        // 504047 and 560420 carry their own amount, duration and stack cap.
        if (player->HasAura(SPELL_PURGATORY) && sSpellMgr->GetSpellInfo(SPELL_PURGATORY_BUFF))
            player->CastSpell(player, SPELL_PURGATORY_BUFF, true);
        if (player->HasAura(SPELL_DAMNED) && sSpellMgr->GetSpellInfo(SPELL_DAMNED_SPEED))
            player->CastSpell(player, SPELL_DAMNED_SPEED, true);
    }

    void Consumed(AuraEffect const*, AuraEffectHandleModes)
    {
        AuraApplication const* application = GetTargetApplication();
        if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            return;
        Unit* owner = GetTarget();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        // Logout removes every aura with AURA_REMOVE_BY_DEFAULT too.
        if (!player || player->getClass() != CLASS_REAPER || !player->IsInWorld() || !player->IsAlive() ||
            !player->GetSession() || player->GetSession()->PlayerLogout() || !player->HasAura(SPELL_TORMENTOR))
            return;
        int32 delta = AscensionSpellSafe::EffectValue(SPELL_TORMENTOR_AMOUNT, EFFECT_0, player, -3000);
        uint32 remaining = player->GetSpellCooldownDelay(SPELL_TORMENTED_SOULS);
        if (!remaining || delta >= 0)
            return;
        // Same rule as the core's ModifyAscensionCooldown (SpellEffects.cpp): a reduction that
        // covers what is left is a reset.
        if (uint64(-int64(delta)) >= remaining)
            player->RemoveSpellCooldown(SPELL_TORMENTED_SOULS, true);
        else
            player->ModifySpellCooldown(SPELL_TORMENTED_SOULS, delta);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_reaper_soul_infusion::Infused,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        // 803031 carries a single aura effect (e0, aura 42): EFFECT_ALL runs this once.
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_reaper_soul_infusion::Consumed,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Soul Harvester 804311, every Reaper's level-1 passive: "Landing the killing blow on a target
// that yields experience or honor now grants Soul Harvester for $804312d." ProcFlags 0 and no
// spell_proc row: the kill is observed here, on the barbarian_kills model
// (AscensionBarbarianCompletion.cpp). HasAura is right: 804311 carries auras 42 and 4, no
// effect 190. No hook for the pet's kills: the tooltip says "landing the killing blow". No
// spell_proc row may be added on 804311 as well (double grant).
class reaper_kill_events : public PlayerScript
{
public:
    reaper_kill_events() : PlayerScript("reaper_kill_events",
        {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_CREATURE_KILL}) { }

    static void Killed(Player* player, Unit* killed)
    {
        // OnPlayerPVPKill also arrives with killer == victim (falling, drowning).
        if (!player || !killed || killed == player || player->getClass() != CLASS_REAPER ||
            !player->IsAlive() || !player->IsInWorld())
            return;
        if (player->HasAura(SPELL_SOUL_HARVESTER) && player->isHonorOrXPTarget(killed) &&
            sSpellMgr->GetSpellInfo(SPELL_SOUL_HARVESTER_REGEN))
            player->CastSpell(player, SPELL_SOUL_HARVESTER_REGEN, true);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override { Killed(killer, killed); }
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { Killed(killer, killed); }
};

// Weakened Souls: "Your Soulrend now also applies Weakened Soul". The proc window was a
// spell_proc row (family 36, Soulrend's family bit, melee spell damage, hit phase); since K7
// (19/10/2026) the row opens on all damage done (Harvester's mask) and IsSoulrendStrike()
// below keeps the old window for the debuff. The row disables effect 1, a private Ascension
// aura 354 whose core handler is nullptr.
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
        // Marqueur de binaire (une ligne au demarrage, ObjectMgr::ValidateSpellScripts) : la
        // preuve, par `strings worldserver | grep 'K7+K9'`, que ce binaire porte K7 et K9. La
        // ligne spell_proc de K7 ne se pose QU'APRES (exploitation/reglages-classes-20261019).
        LOG_INFO("server.loading", ">> Ascension Reaper K7+K9 : Weakened Souls vol de vie {} %, Painbringer sans degats sur soi",
            WEAKENED_SOULS_LIFESTEAL_PCT);
        return ValidateSpellInfo({SPELL_WEAKENED_SOUL});
    }

    // No GetDamage() test on purpose: a fully absorbed hit must still apply the debuff.
    // The consequence is worth writing down, because it will be mistaken for a bug in game.
    // A training dummy zeroes the damage (npc_training_dummy::DamageTaken, the trap already
    // recorded at AscensionCompat.cpp:2533), the core then computes SpellTypeMask =
    // PROC_SPELL_TYPE_NO_DMG_HEAL (Unit.cpp:7130-7133) and the SpellTypeMask = 1 of the
    // spell_proc row for 92146 rejects it. Weakened Souls cannot be observed on a dummy.
    // K7 : la ligne spell_proc de 92146 s'ouvre a tous les degats infliges (meme masque que
    // Harvester 92145, exploitation/reglages-classes-20261019/K7-*.sql). Le debuff, lui, reste
    // reserve a ce que l'ancienne ligne laissait passer : un sort de degats de classe MELEE
    // (PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS, l'ancien ProcFlags 16) de la famille 36 portant le
    // bit de Soulrend (l'ancien SpellFamilyMask1 8192). Avec l'ancienne ligne, ce test est
    // toujours vrai et le vol de vie ne joue que sur Soulrend : ce binaire tolere les deux lignes.
    static bool IsSoulrendStrike(ProcEventInfo& event)
    {
        SpellInfo const* spell = event.GetSpellInfo();
        return (event.GetTypeMask() & PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS) && spell &&
            spell->SpellFamilyName == SPELLFAMILY_REAPER &&
            (spell->SpellFamilyFlags[1] & SOULREND_FAMILY_FLAGS_1);
    }

    // victim->IsAlive() n'est plus exige ici mais dans la branche du debuff : le coup qui tue
    // rend sa part de vie, comme Harvester (aura_ascension_harvester::CheckProc ne l'exige pas).
    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        return owner && owner->IsPlayer() && owner->getClass() == CLASS_REAPER && owner->IsAlive() &&
            owner->IsInWorld() && event.GetActor() == owner && victim && victim != owner &&
            !owner->IsFriendlyTo(victim) && event.GetDamageInfo();
    }

    void Weaken(AuraEffect const*, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget();
        Unit* victim = event.GetActionTarget();
        if (!owner || !victim)
            return;

        // K7 : vol de vie, sur tout coup qui inflige des degats (un coup entierement absorbe
        // ne rend rien). Garde-fou d'execution plutot que Validate : un 504565 absent du DBC
        // ne doit pas eteindre le debuff avec le script entier.
        if (DamageInfo* damageInfo = event.GetDamageInfo())
            if (uint32 dealt = damageInfo->GetDamage())
                if (sSpellMgr->GetSpellInfo(SPELL_BLOOD_HARVEST))
                {
                    uint64 amount = uint64(dealt) * WEAKENED_SOULS_LIFESTEAL_PCT / 100;
                    if (amount)
                        owner->CastCustomSpell(SPELL_BLOOD_HARVEST, SPELLVALUE_BASE_POINT0,
                            int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), owner,
                            TRIGGERED_FULL_MASK);
                }

        if (!IsSoulrendStrike(event) || !victim->IsAlive() || !owner->IsAlive())
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
    new reaper_kill_events();
}
