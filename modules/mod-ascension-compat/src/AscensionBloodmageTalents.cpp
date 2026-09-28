/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionPooledVitality.h"
#include "CombatAI.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <vector>

namespace
{
enum BloodmageTalentSpells : uint32
{
    SPELL_LIQUIFY = 806310,
    SPELL_VAMPIRIC_POOLS = 504088,
    SPELL_VAMPIRIC_POOLS_LEECH = 806311,
    SPELL_DARKCASTING = 712383,
    SPELL_BLOOD_TEAR_SPAWN = 712417,
    SPELL_ACCURSED_FORM = 562572,
    SPELL_SANGUINE_SCRIPTURE = 804851,
    SPELL_SANGUINE_SCRIPTURE_BUFF = 504264,
    SPELL_CURSED_FORM_REQUIREMENT = 525031,
    SPELL_CURSED_FORM_REQUIREMENT_2 = 524861,
    SPELL_BLOODMOON_POWER = 801961,
    // Nettoyage du 2026-09-28.
    SPELL_SANGUINE_ESSENCE = 680692,
    SPELL_TRANSGRESSION = 801076,
    SPELL_TRANSGRESSION_SPIRIT = 801102,   // "Transgression / Cursed Form SLS 2": aura 174 on Spirit
    SPELL_ETERNAL_CURSE = 800157,
    SPELL_ETERNAL_CURSE_STANCE = 807736,   // "Tank Stance Aura": -10% damage, +95% threat, +50% health-cost threat
    SPELL_ETERNAL_CURSE_HIDDEN = 804320,   // +400% item armor, Feral Strength's attack power
    SPELL_HUNTER_AND_HUNTED = 807487,
    SPELL_DARK_EMBRACE_CHARGE = 804854,    // CHARGE + ROOT, both TARGET_UNIT_TARGET_ENEMY
    SPELL_BLOODSURGE = 553267,
    SPELL_BLOODCHASER = 523721,
    SPELL_GORE_TOME = 807788,
    // Troisieme passe du 2026-09-28.
    SPELL_DARK_FRENZY = 704644,            // Ferocity talent: e0 raid haste, e1 aura 4
    SPELL_DARK_FRENZY_GCD = 804845,        // e2 aura 108 SPELLMOD_GLOBAL_COOLDOWN -25 % (e0/e1 are DUMMY effects)
    SPELL_ACCURSED_FORM_BONUS = 804699,    // e1 aura 268: attack power = 50 % of Agility; e0 posed 804845
    SPELL_BLOODGALE = 681310,              // The Hunter and the Hunted's eruption
    SPELL_BLOOD_CONSTRUCTOR = 561196,      // e0 amount: +1000 ms per Thirst stack
    SPELL_THIRST = 706613,
    SPELL_BLOOD_TRAIL = 804857,            // the pool of Blood Trails 804858
    SPELL_AMALGAM_BLOOD_STRIKE = 589023,
    SPELL_AMALGAM_HEAL = 592015,           // HEAL on TARGET_UNIT_MASTER
    SPELL_AMALGAM_HEAL_SHARE = 504711,     // e0 aura 354: the 50 % of the tooltip
    // Quatrieme passe du 2026-09-28.
    SPELL_ANIMATED_BLOOD = 573299,         // spell_ranks head: 573299, 573356, 573357
    SPELL_BLOOD_ORB_TRIGGER = 712419       // "Blood Orb / Trigger": its radius (index 7, 2 yd) is the pickup reach
};

// Every creature Animated Blood can leave behind: worms, parasites and the rank 3 amalgam.
constexpr uint32 AnimatedBloodSummons[] = {325301, 335301, 315301};

// Every shape the Bloodmage's Cursed Form can take: Blood Curse and the spells that replace it.
constexpr uint32 CursedForms[] = {562572, 562720, 680692, 800157, 801076};

bool IsCursedForm(uint32 id)
{
    return std::find(std::begin(CursedForms), std::end(CursedForms), id) != std::end(CursedForms);
}

// Cursed Form abilities gate their cast on a CasterAuraSpell marker that nothing in Spell.dbc ever
// grants, so they could never be cast. Two distinct marker spells are both named "Cursed Form" in
// Spell.dbc and are split across the kit's abilities (e.g. Ravenous Strike/Lunge/Claw Sweep/Bloodfang
// Bite use 525031, while Rotclaw/Ironhide/Reave/Bloodsurge/Apotheosis and others use 524861), so both
// need to be mirrored onto the real form state, the same way Palm Sigil's marker follows
// Runeshroud/Waveforged. AscensionBloodmage::CursedForm (802877) is a third, separate marker: it is
// the ExcludeCasterAuraSpell Sanguine Mend and the pooled-vitality empowerment check both rely on to
// block casting while shapeshifted, but nothing else ever grants it either, so it needs the same sync.
//
// TWO FORMS ARE THE EXCEPTION (2026-09-28). Sanguine Essence 680692 ("a unique Cursed Form that does
// not disable Mortal Form abilities") and Transgression 801076 ("... that does not disable the use of
// Mortal Form abilities") must not carry 524861 nor 802877: 524861 is the ExcludeCasterAuraSpell of 47
// family-26 records (Bloodmoon Blast, Vampyr's Kiss, Sanguine Rupture, Taldaram's Torment...) and
// 802877 that of 65 (Sanguine Mend, Veinburst, Crimson Tide, Dark Liturgy, Liquify...), counted in
// Spell.dbc field 26 that day, and CanEmpower (AscensionBloodmageVitality.cpp) refuses both. They keep
// 525031, which excludes nothing and gates the basic Worgen techniques as before. The price, stated:
// in these two forms the 524861-gated Cursed Form abilities (53 records, field 24) are not castable,
// since the same marker cannot both allow them and allow Bloodmoon Blast.
bool LocksMortalForm(uint32 form)
{
    return form != SPELL_SANGUINE_ESSENCE && form != SPELL_TRANSGRESSION;
}

void SyncCursedFormRequirement(Player* player)
{
    bool anyForm = false;
    bool mortalLock = false;
    for (uint32 form : CursedForms)
        if (player->HasAura(form, player->GetGUID()))
        {
            anyForm = true;
            if (LocksMortalForm(form))
                mortalLock = true;
        }

    auto sync = [player](uint32 marker, bool wanted)
    {
        if (!wanted)
            player->RemoveAurasDueToSpell(marker, player->GetGUID());
        else if (player->IsInWorld() && player->IsAlive() && !player->HasAura(marker, player->GetGUID()))
            player->CastSpell(player, marker, true);
    };
    sync(SPELL_CURSED_FORM_REQUIREMENT, anyForm);
    sync(SPELL_CURSED_FORM_REQUIREMENT_2, mortalLock);
    sync(AscensionBloodmage::CursedForm, mortalLock);
    // Dark Frenzy (704644): "while a Cursed Form is active, your global cooldowns are reduced by
    // $804845s3%". The -25 % is 804845's effect 2, and the only record that posed 804845 is 804699, which
    // nothing applies (and whose periodic pose is disarmed below, so that Accursed Form alone does not give
    // it). The GUID matters: effect 0 of 704644 is a raid aura, which an ally holds with the Bloodmage as
    // caster. SUPPOSE, to be seen in game: the client applies the SPELLMOD_GLOBAL_COOLDOWN it receives
    // (SMSG_SET_PCT_SPELL_MODIFIER) to its own GCD display.
    sync(SPELL_DARK_FRENZY_GCD, anyForm && player->HasAura(SPELL_DARK_FRENZY, player->GetGUID()));
}

// Eternal Curse 800157: "reducing your damage dealt by $807736s1% and increasing your threat generated
// by $807736s2% ... your armor contribution from items is increased by $804320s1%". Neither 807736 nor
// 804320 was granted by anything (800157 carries 137/36/56 only, SpellShapeshiftForm 57 has no preset
// spell, no spell_linked_spell row). They follow the form here. 807736 is passive and therefore never
// saved, while the infinite 800157 is restored from `character_aura` before the player is in world:
// the login hook below puts them back.
void SyncEternalCurse(Player* player)
{
    bool const inForm = player->HasAura(SPELL_ETERNAL_CURSE, player->GetGUID());
    for (uint32 id : {uint32(SPELL_ETERNAL_CURSE_STANCE), uint32(SPELL_ETERNAL_CURSE_HIDDEN)})
    {
        if (!inForm)
            player->RemoveAurasDueToSpell(id, player->GetGUID());
        else if (player->IsInWorld() && player->IsAlive() && !player->HasAura(id, player->GetGUID()))
            player->CastSpell(player, id, true);
    }
}

// A fresh activation, as opposed to a copy restored at login or a refresh.
bool IsFreshActivation(Player const* player, Aura const* aura)
{
    return player->IsInWorld() && aura->GetCasterGUID() == player->GetGUID() &&
        aura->GetDuration() == aura->GetMaxDuration();
}

class spell_ascension_animated_blood : public SpellScript
{
    PrepareSpellScript(spell_ascension_animated_blood);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_DARKCASTING, SPELL_BLOOD_TEAR_SPAWN});
    }

    void HandleExtraWorms(SpellEffIndex index)
    {
        if (GetSpellInfo()->Effects[index].TriggerSpell != SPELL_BLOOD_TEAR_SPAWN)
            return;
        PreventHitDefaultEffect(index);
        Unit* caster = GetCaster();
        Aura* darkcasting = caster->GetAura(SPELL_DARKCASTING, caster->GetGUID());
        if (!darkcasting)
            return;
        uint8 const count = darkcasting->GetStackAmount();
        darkcasting->Remove();
        // The helper's zero summon count otherwise falls back to one worm on every ordinary cast.
        // Darkcasting supplies the extra worms; the parent supplies its rank/empowerment count.
        if (count)
            caster->CastCustomSpell(SPELL_BLOOD_TEAR_SPAWN, SPELLVALUE_BASE_POINT0, count, caster, true);
    }

    void ReplacePreviousBrood()
    {
        // Recasting replaces the previous brood instead of stacking a second one beside it.
        if (Unit* caster = GetCaster())
        {
            for (uint32 entry : AnimatedBloodSummons)
                caster->RemoveAllMinionsByEntry(entry);
            // Blood Constructor (561196): "Increases the duration of Animated Blood by $/1000;s1 sec per stack
            // of Thirst." Its modifier targets the hidden mirror 300796, which nothing applies (and whose
            // -8 % cast time would double Thirst's own). The stack count is taken before the cast resolves.
            _constructorStacks = 0;
            if (caster->HasAura(SPELL_BLOOD_CONSTRUCTOR))
                if (Aura const* thirst = caster->GetAura(SPELL_THIRST))
                    _constructorStacks = thirst->GetStackAmount();
        }
    }

    // The brood is summoned in the immediate phase of the cast (Spell::_cast: handle_immediate, then
    // AfterCast), so it exists here. The 1000 ms is 561196's effect 0 amount, read.
    void ExtendBrood()
    {
        Unit* caster = GetCaster();
        if (!caster || !_constructorStacks)
            return;
        AuraEffect const* constructor = caster->GetAuraEffect(SPELL_BLOOD_CONSTRUCTOR, EFFECT_0);
        int32 const perStack = constructor ? constructor->GetAmount() : 0;
        if (perStack <= 0)
            return;
        uint64 const bonus = uint64(perStack) * _constructorStacks;
        for (uint32 entry : AnimatedBloodSummons)
        {
            std::list<Creature*> brood;
            caster->GetAllMinionsByEntry(brood, entry);
            for (Creature* creature : brood)
                if (TempSummon* summon = creature ? creature->ToTempSummon() : nullptr)
                    if (summon->GetTimer() > 0)
                        summon->SetTimer(uint32(std::min<uint64>(uint64(summon->GetTimer()) + bonus,
                            std::numeric_limits<uint32>::max() / 2)));
        }
    }

    uint32 _constructorStacks = 0;

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_animated_blood::ReplacePreviousBrood);
        AfterCast += SpellCastFn(spell_ascension_animated_blood::ExtendBrood);
        // This destination-only helper is triggered in LAUNCH, before target-specific effects.
        OnEffectLaunch += SpellEffectFn(spell_ascension_animated_blood::HandleExtraWorms,
            EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL);
    }
};

class bloodmage_talent_events : public UnitScript
{
public:
    bloodmage_talent_events() : UnitScript("bloodmage_talent_events", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SON_OF_ARUGAL || !aura)
            return;
        uint32 const id = aura->GetId();
        if (IsCursedForm(id))
        {
            SyncCursedFormRequirement(player);
            if (id == SPELL_ETERNAL_CURSE)
                SyncEternalCurse(player);
            if (player->IsAlive() && IsFreshActivation(player, aura))
                OnCursedFormActivated(player, id);
        }
        // A Dark Frenzy learned (or re-applied) while a form is up.
        if (id == SPELL_DARK_FRENZY && aura->GetCasterGUID() == player->GetGUID())
            SyncCursedFormRequirement(player);
        // Liquify (806310): "preventing you from attacking or casting spells", aura "Pacified and silenced".
        // Its aura 263 only sets PLAYER_ALLOW_ONLY_ABILITY (Bladestorm's flag), which blocks spells and
        // leaves the auto attack running (Unit::AttackerStateUpdate stops on UNIT_FLAG_PACIFIED only). Same
        // idiom as HandleAuraModPacify; the flag is taken back in OnAuraRemove, death included.
        if (id == SPELL_LIQUIFY && aura->GetCasterGUID() == player->GetGUID())
        {
            player->AttackStop();
            player->SetUnitFlag(UNIT_FLAG_PACIFIED);
        }
        // Bloodmoon Power (801961): "Your Liquify now cleanses all negative effects when used", and
        // Liquify is "Usable while stunned, feared or confused". The cleanse used to run when Liquify
        // ENDED, 6 s later, and only on dispellable effects, which spares most stuns.
        if (id == SPELL_LIQUIFY && aura->GetCasterGUID() == player->GetGUID() && player->IsAlive() &&
            player->IsInWorld() && player->HasAura(SPELL_BLOODMOON_POWER))
        {
            // Loss of control first, whatever its dispel type; Liquify's own slow (effect 2) is spared.
            player->RemoveAurasWithMechanic(IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK,
                AURA_REMOVE_BY_ENEMY_SPELL, SPELL_LIQUIFY);
            std::vector<uint32> remove;
            for (auto const& pair : player->GetAppliedAuras())
            {
                Aura const* other = pair.second->GetBase();
                if (other->GetId() != SPELL_LIQUIFY && !pair.second->IsPositive() && !other->IsPassive() &&
                    other->GetSpellInfo()->Dispel != DISPEL_NONE)
                    remove.push_back(other->GetId());
            }
            for (uint32 other : remove)
                player->RemoveAurasDueToSpell(other, ObjectGuid::Empty, 0, AURA_REMOVE_BY_ENEMY_SPELL);
        }
    }

    static void OnCursedFormActivated(Player* player, uint32 form)
    {
        // Gore Tome (807788): "For the first 5 sec after activating a Cursed Form ...". The moment is
        // written on the talent's own aura; AscensionBloodmageSecondary.cpp reads it.
        if (Aura* tome = player->GetAura(SPELL_GORE_TOME, player->GetGUID()))
            tome->SetScriptValue(SPELL_GORE_TOME, std::max<uint64>(uint64(GameTime::GetGameTimeMS().count()), 1));

        // The Hunter and the Hunted (807487, Ferocity level 50): "When you activate a Cursed Form you
        // now charge to your target if they are within $804854R yds. If they're an enemy you root
        // them for $804854d." 804854 is CHARGE + ROOT, both on TARGET_UNIT_TARGET_ENEMY: the enemy
        // half is cast as is. 804854 cannot target a friend, so the friendly half ("Leap behind the
        // targeted friend or enemy", its description) is a plain charge behind the ally: no root, no
        // spell. The two branches exclude each other (an ally is never IsValidAttackTarget). The
        // "Upon expiration ... erupt" half is OpenBloodgaleWindow below.
        if (player->HasAura(SPELL_HUNTER_AND_HUNTED))
            if (SpellInfo const* charge = sSpellMgr->GetSpellInfo(SPELL_DARK_EMBRACE_CHARGE))
            {
                Unit* target = player->GetSelectedUnit();
                if (!target)
                    target = player->GetVictim();
                if (target && target != player && target->IsAlive() && player->IsInMap(target) &&
                    player->IsWithinDistInMap(target, charge->GetMaxRange(false, player)) &&
                    player->IsWithinLOSInMap(target))
                {
                    if (player->IsValidAttackTarget(target))
                        player->CastSpell(target, SPELL_DARK_EMBRACE_CHARGE, TRIGGERED_FULL_MASK);
                    else if (player->IsValidAssistTarget(target) && !player->HasUnitState(UNIT_STATE_ROOT))
                    {
                        Position const behind = target->GetFirstCollisionPosition(player->GetCombatReach(), float(M_PI));
                        player->GetMotionMaster()->MoveCharge(behind.GetPositionX(), behind.GetPositionY(),
                            behind.GetPositionZ(), SPEED_CHARGE);
                    }
                }
            }

        // Bloodsurge (553267): "At the end of the duration, or whenever you trigger Accursed Form,
        // gain Bloodchaser instead." Removed by CANCEL so that its own expiry script
        // (aura_ascension_bloodmage_bloodsurge, AscensionBloodmageEvents.cpp) does not pay a second time.
        if (form == SPELL_ACCURSED_FORM && player->HasAura(SPELL_BLOODSURGE, player->GetGUID()))
        {
            player->RemoveAurasDueToSpell(SPELL_BLOODSURGE, player->GetGUID(), 0, AURA_REMOVE_BY_CANCEL);
            player->CastSpell(player, SPELL_BLOODCHASER, true);
        }

        // Transgression (801076) "... granting spell damage equal to $s1% of your Spirit" with Crimson
        // Terror 560073: the carrier is 801102 (aura 174 on Spirit, word 2 0x4, the only mask Crimson
        // Terror's +50 targets), which nothing cast. Without the talent its amount is 0. It lasts 30 s
        // against 15 s for the form, so the form's removal takes it away (OnAuraRemove).
        if (form == SPELL_TRANSGRESSION)
            player->CastSpell(player, SPELL_TRANSGRESSION_SPIRIT, true);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SON_OF_ARUGAL || !application)
            return;
        Aura* aura = application->GetBase();
        if (IsCursedForm(aura->GetId()))
        {
            SyncCursedFormRequirement(player);
            // Before the death/out-of-world return below: a form lost to death takes its parts along.
            if (aura->GetId() == SPELL_ETERNAL_CURSE)
                SyncEternalCurse(player);
            if (aura->GetId() == SPELL_TRANSGRESSION)
                player->RemoveAurasDueToSpell(SPELL_TRANSGRESSION_SPIRIT, player->GetGUID());
        }
        // A Dark Frenzy unlearned while a form is up takes its global cooldown reduction along. The talent's
        // aura has already left m_appliedAuras here (Unit::_UnapplyAura calls this hook last).
        if (aura->GetId() == SPELL_DARK_FRENZY && aura->GetCasterGUID() == player->GetGUID())
            SyncCursedFormRequirement(player);
        // Liquify's pacify (OnAuraApply), before the death/out-of-world return. Liquify has already left
        // m_appliedAuras here; a real pacify aura still up keeps the flag, as HandleAuraModPacify does.
        if (aura->GetId() == SPELL_LIQUIFY && !player->HasPacifyAura() && !player->HasPacifySilenceAura())
            player->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
        if (!player->IsAlive() || !player->IsInWorld() || mode == AURA_REMOVE_BY_DEATH)
            return;
        if (aura->GetId() == SPELL_LIQUIFY && aura->GetCasterGUID() == player->GetGUID() &&
            player->HasAura(SPELL_VAMPIRIC_POOLS))
            player->CastSpell(player, SPELL_VAMPIRIC_POOLS_LEECH, true);
        // Bloodmoon Power's cleanse moved to the application of Liquify (OnAuraApply) on 2026-09-28.
        if (aura->GetId() == SPELL_ACCURSED_FORM && aura->GetCasterGUID() == player->GetGUID() &&
            player->HasAura(SPELL_SANGUINE_SCRIPTURE))
            player->CastSpell(player, SPELL_SANGUINE_SCRIPTURE_BUFF, true);
        if (aura->GetId() == SPELL_ACCURSED_FORM && mode == AURA_REMOVE_BY_EXPIRE &&
            aura->GetCasterGUID() == player->GetGUID() && player->HasAura(SPELL_HUNTER_AND_HUNTED))
            OpenBloodgaleWindow(player);
    }

    // The Hunter and the Hunted (807487): "Upon expiration, your Accursed Form can be reactivated to cause
    // you to erupt in a burst of blood magic" - Bloodgale 681310, which nothing granted. Accursed Form's
    // slot is rewritten to Bloodgale for the time Accursed Form spends on cooldown (its RecoveryTime, 30 s,
    // starts at its removal: SPELL_ATTR0_DISABLED_WHILE_ACTIVE). DECISION: no field states the window; that
    // cooldown is the span during which "reactivating" Accursed Form cannot mean anything else. Hemostasis
    // idiom (P-144): the target is learned unannounced, never with learnSpell. Only a natural expiry opens
    // it (a cancel or a death does not "expire"). 681310's ExcludeCasterAuraSpell 802877 was removed by
    // SyncCursedFormRequirement just before this hook's caller returned here.
    static void OpenBloodgaleWindow(Player* player)
    {
        SpellInfo const* form = sSpellMgr->GetSpellInfo(SPELL_ACCURSED_FORM);
        if (!form || !sSpellMgr->GetSpellInfo(SPELL_BLOODGALE) ||
            player->GetTemporarySpellReplacement(SPELL_ACCURSED_FORM) == SPELL_BLOODGALE)
            return;
        uint32 const window = std::max(form->RecoveryTime, form->CategoryRecoveryTime);
        if (!window)
            return;
        bool const learned = player->GetSpellMap().find(SPELL_BLOODGALE) == player->GetSpellMap().end();
        if (learned)
            player->LearnTemporarySpellReplacementTarget(SPELL_BLOODGALE);
        player->SetTemporarySpellReplacement(SPELL_ACCURSED_FORM, SPELL_BLOODGALE);
        if (player->GetTemporarySpellReplacement(SPELL_ACCURSED_FORM) != SPELL_BLOODGALE)
        {
            // Refused (Accursed Form not active in this spec): leave nothing behind.
            if (learned)
                player->removeSpell(SPELL_BLOODGALE, SPEC_MASK_ALL, true);
            return;
        }
        player->m_Events.AddEventAtOffset([player]() { CloseBloodgaleWindow(player); }, Milliseconds(window));
    }

    // Idempotent: the timer and the cast of Bloodgale both close the window, whichever comes first. Not
    // gated on GetTemporarySpellReplacement, which answers "no replacement" while Accursed Form is inactive
    // (another spec) and would leave the entry behind. Clearing an absent entry sends nothing
    // (Player::SetTemporarySpellReplacement), and removeSpell(..., true) only ever removes a TEMPORARY
    // spell, so a Bloodgale owned for real is kept. RESIDUAL, accepted: a timer left by a window already
    // closed by a cast would also close a later window; Accursed Form's 30 s cooldown plus its 15 s
    // duration keep the next expiry beyond that timer.
    static void CloseBloodgaleWindow(Player* player)
    {
        player->SetTemporarySpellReplacement(SPELL_ACCURSED_FORM, 0);
        player->removeSpell(SPELL_BLOODGALE, SPEC_MASK_ALL, true);
    }
};

// Closes the Bloodgale window once it has been used. Deferred to the next update: the spell being cast
// must not leave the spell map while its own cast is still resolving.
class bloodmage_bloodgale_cast : public AllSpellScript
{
public:
    bloodmage_bloodgale_cast() : AllSpellScript("bloodmage_bloodgale_cast", {ALLSPELLHOOK_ON_CAST}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
    {
        if (!info || info->Id != SPELL_BLOODGALE || spell->IsTriggered())
            return;
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SON_OF_ARUGAL)
            return;
        player->m_Events.AddEventAtOffset([player]() { bloodmage_talent_events::CloseBloodgaleWindow(player); },
            Milliseconds(0));
    }
};

class bloodmage_talent_contracts : public GlobalScript
{
public:
    bloodmage_talent_contracts() : GlobalScript("bloodmage_talent_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != 26)
            return;

        // Dark Frenzy's carrier (804845) follows the form and the talent (SyncCursedFormRequirement); the
        // infinite aura is re-posed on login, so a saved copy would only outlive the talent or the form.
        if (info->Id == SPELL_DARK_FRENZY_GCD)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }

        // Accursed Form's attack power (804699 effect 1, "$804699s2% of your Agility"), which the companion
        // SQL now links to 562572 (spell_linked_spell type 2). Its effect 0 re-poses 804845 every 3 s: left
        // armed, every Accursed Form would carry Dark Frenzy's -25 % global cooldown without the talent.
        // The link re-applies it with the form (also when the form is restored at login), so it is not saved.
        if (info->Id == SPELL_ACCURSED_FORM_BONUS)
        {
            SpellEffectInfo& pose = info->Effects[EFFECT_0];
            if (pose.Effect == SPELL_EFFECT_APPLY_AURA && pose.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL &&
                pose.TriggerSpell == SPELL_DARK_FRENZY_GCD &&
                info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_MOD_ATTACK_POWER_OF_STAT_PERCENT)
                pose.ApplyAuraName = SPELL_AURA_DUMMY;
            else
                LOG_ERROR("module.ascension_compat", "Bloodmage: unexpected Accursed Form bonus record {}", info->Id);
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }

        if (info->Id != SPELL_VAMPIRIC_POOLS_LEECH ||
            info->Effects[EFFECT_0].Effect != SPELL_EFFECT_HEALTH_LEECH)
            return;

        // Vampiric Pools leeches and fears the same nearby targets when Liquify ends.
        // Keep the existing leech amount/coefficient and native damage-break proc data.
        info->DurationEntry = sSpellDurationStore.LookupEntry(32); // Six seconds.
        info->AttributesCu |= SPELL_ATTR0_CU_NEGATIVE_EFF1;
        auto& fear = info->Effects[EFFECT_1];
        fear.Effect = SPELL_EFFECT_APPLY_AURA;
        fear.ApplyAuraName = SPELL_AURA_MOD_FEAR;
        fear.Mechanic = MECHANIC_FEAR;
        fear.TargetA = info->Effects[EFFECT_0].TargetA;
        fear.TargetB = info->Effects[EFFECT_0].TargetB;
        fear.RadiusEntry = info->Effects[EFFECT_0].RadiusEntry;
    }
};

// The forms restored from `character_aura` are applied before the player is in world, where every
// sync above refuses to cast. Once in world, the markers and Eternal Curse's parts are put back.
class bloodmage_talent_login : public PlayerScript
{
public:
    bloodmage_talent_login() : PlayerScript("bloodmage_talent_login", {PLAYERHOOK_ON_LOGIN}) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!player || player->getClass() != CLASS_SON_OF_ARUGAL || !player->IsAlive())
            return;
        SyncCursedFormRequirement(player);
        SyncEternalCurse(player);
        if (!player->HasAura(SPELL_TRANSGRESSION, player->GetGUID()))
            player->RemoveAurasDueToSpell(SPELL_TRANSGRESSION_SPIRIT, player->GetGUID());
    }
};

bool HasCursedForm(Unit const* unit)
{
    for (uint32 form : CursedForms)
        if (unit->HasAura(form))
            return true;
    return false;
}

// 804857 Blood Trail, the pool Blood Trails 804858 spills: "Allies in a Cursed Form who move through the
// pools of blood gain Trail of Blood." Effect 1 (persistent area aura 23 on 804859, DEST_AREA_ALLY) had no
// form filter. Effects 0 and 2 carry no aura, so the check only ever decides effect 1.
class aura_ascension_bloodmage_blood_trail : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_blood_trail);

    bool CheckAreaTarget(Unit* target)
    {
        return target && target->IsPlayer() && HasCursedForm(target);
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(aura_ascension_bloodmage_blood_trail::CheckAreaTarget);
    }
};

// Animated Blood rank 3 (573357) summons one amalgam, 315301: "The amalgam will periodically unleash a
// Blood Strike on its target, dealing ${$589023m1*$<scalingbp>+$sps*.4+$AP*.15} Shadow damage, healing you
// for $504711s1% of the damage dealt." The creature had no AI of its own and never cast 589023. It keeps
// the AggressorAI it was given until now (not civilian, not neutral: CreatureAISelector's permit), and
// adds the strike. DECISION: "periodically" is carried by no field; AmalgamStrikeIntervalMs names it.
// The amalgam casts in its own name, so its own done modifiers (Vampyr Lord 560259, restricted to the
// Animated Blood summons) apply, and threat and damage credit stay where they were. The Bloodmage's
// spell power and attack power are added by the spell script below.
constexpr uint32 AmalgamStrikeIntervalMs = 6000;

struct npc_ascension_bloodmage_amalgam : public AggressorAI
{
    explicit npc_ascension_bloodmage_amalgam(Creature* creature) : AggressorAI(creature) { }

    void UpdateAI(uint32 diff) override
    {
        AggressorAI::UpdateAI(diff);
        _strike = _strike > diff ? _strike - diff : 0;
        if (_strike)
            return;
        Unit* victim = me->GetVictim();
        Unit* owner = me->GetOwner();
        Player* bloodmage = owner ? owner->ToPlayer() : nullptr;
        if (!victim || !bloodmage || bloodmage->getClass() != CLASS_SON_OF_ARUGAL || !bloodmage->IsAlive() ||
            !me->IsAlive() || me->HasUnitState(UNIT_STATE_CASTING) || !me->IsWithinMeleeRange(victim) ||
            !me->IsValidAttackTarget(victim))
            return;
        me->CastSpell(victim, SPELL_AMALGAM_BLOOD_STRIKE, false);
        _strike = AmalgamStrikeIntervalMs;
    }

    uint32 _strike = 0;
};

// 589023 Blood Strike, the amalgam's strike. The base points are scaled by $scalingbp in CalcValue
// (AscensionScalingBase.cpp lists {589023, 1}); the owner's terms are added after it, at launch, before
// the done/taken bonuses (Spell::EffectSchoolDMG runs in LAUNCH_TARGET). 589023 has EffectBonusMultiplier
// 0 and no spell_bonus_data row, so the amalgam's own attack power adds nothing. The 0.4 and 0.15 are
// the tooltip's; no field carries them. The heal goes to the master (592015, TARGET_UNIT_MASTER, cast by
// the amalgam), for 504711's 50 % of the damage dealt, read.
constexpr float AmalgamSpellPowerShare = 0.4f;
constexpr float AmalgamAttackPowerShare = 0.15f;

class spell_ascension_bloodmage_amalgam_strike : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_amalgam_strike);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_AMALGAM_HEAL, SPELL_AMALGAM_HEAL_SHARE});
    }

    static Player* Master(Unit* caster)
    {
        Unit* owner = caster ? caster->GetOwner() : nullptr;
        Player* player = owner ? owner->ToPlayer() : nullptr;
        return player && player->getClass() == CLASS_SON_OF_ARUGAL ? player : nullptr;
    }

    void AddOwnerPower(SpellEffIndex /*index*/)
    {
        Player* master = Master(GetCaster());
        if (!master)
            return;
        double const bonus = double(master->SpellBaseDamageBonusDone(GetSpellInfo()->GetSchoolMask())) *
            AmalgamSpellPowerShare + double(master->GetTotalAttackPowerValue(BASE_ATTACK)) * AmalgamAttackPowerShare;
        double const value = double(GetEffectValue()) + std::max(bonus, 0.0);
        if (std::isfinite(value) && value >= 0.0 && value <= double(std::numeric_limits<int32>::max() / 2))
            SetEffectValue(int32(value));
    }

    void HealMaster()
    {
        Unit* amalgam = GetCaster();
        Player* master = Master(amalgam);
        int32 const dealt = GetHitDamage();
        if (!master || !master->IsAlive() || !amalgam->IsInMap(master) || dealt <= 0)
            return;
        SpellInfo const* share = sSpellMgr->GetSpellInfo(SPELL_AMALGAM_HEAL_SHARE);
        int32 const pct = share ? std::clamp(share->Effects[EFFECT_0].CalcValue(master), 0, 100) : 0;
        if (int32 const heal = CalculatePct(dealt, pct); heal > 0)
            amalgam->CastCustomSpell(SPELL_AMALGAM_HEAL, SPELLVALUE_BASE_POINT0, heal, master, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_bloodmage_amalgam_strike::AddOwnerPower, EFFECT_0,
            SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterHit += SpellHitFn(spell_ascension_bloodmage_amalgam_strike::HealMaster);
    }
};

// Darkcasting (706247): "Critical strikes with abilities now spawn a Blood Orb near you for $707434d", and
// 707434: "When you run over this, restore $712383s2% of your missing health and $/10;712383s3 Rage and gain a
// stack of Darkcasting", aura "... reduce the cooldown of Animated Blood by 1 sec". The talent's proc is the
// spell_proc row of 2026_09_28_26; 707434 summons 315303 (creature row of the same file) through
// SummonProperties 61, i.e. as a guardian that SummonGuardian sets to follow its owner.
// The DBC carries a pickup of its own (712418 on the orb, ticking 712419 every 200 ms), but 712419 casts
// 712383 on ANY ally in its area (TARGET_UNIT_SRC_AREA_ALLY) and with the orb as caster, whereas Animated
// Blood only spends the Bloodmage's OWN stacks (spell_ascension_animated_blood, GetAura(712383, caster)) and
// 712418's text reserves the orb to "the Accursed Blood Mage". 712418 is therefore never posed: this AI does
// the same tick with only the summoner as taker, who casts 712383 on himself (heal, Rage and the stack are
// that record's three effects, native). The reach is 712419's radius and the tick 712418's amplitude, read;
// the 1 s is the aura text's and in no field (712383 carries no cooldown effect).
constexpr uint32 BloodOrbTickMs = 200;
constexpr uint32 DarkcastingCooldownCutMs = 1000;

void CutAnimatedBloodCooldown(Player* player)
{
    for (uint32 id = SPELL_ANIMATED_BLOOD; id; id = sSpellMgr->GetNextSpellInChain(id))
        if (uint32 const left = player->GetSpellCooldownDelay(id))
        {
            if (left <= DarkcastingCooldownCutMs)
                player->RemoveSpellCooldown(id, true);
            else
                player->ModifySpellCooldown(id, -int32(DarkcastingCooldownCutMs));
        }
}

struct npc_ascension_bloodmage_blood_orb : public NullCreatureAI
{
    explicit npc_ascension_bloodmage_blood_orb(Creature* creature) : NullCreatureAI(creature) { }

    void UpdateAI(uint32 diff) override
    {
        if (_taken)
            return;
        // An orb lies where it spawned: undo the guardian follow SummonGuardian installs after the AI.
        if (!_still)
        {
            me->GetMotionMaster()->Clear(false);
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
            _still = true;
        }
        _tick = _tick > diff ? _tick - diff : 0;
        if (_tick)
            return;
        _tick = BloodOrbTickMs;
        TempSummon* summon = me->ToTempSummon();
        Player* owner = summon ? ObjectAccessor::GetPlayer(*me, summon->GetSummonerGUID()) : nullptr;
        if (!owner || owner->getClass() != CLASS_SON_OF_ARUGAL || !owner->IsInWorld() || !owner->IsAlive() ||
            !owner->IsInMap(me) || !owner->InSamePhase(me))
            return;
        SpellInfo const* trigger = sSpellMgr->GetSpellInfo(SPELL_BLOOD_ORB_TRIGGER);
        float const reach = trigger ? trigger->Effects[EFFECT_0].CalcRadius(owner) : 0.0f;
        // Centre to centre: IsWithinDist adds both combat reaches and made the 2 yd a ~4 yd pickup, so an
        // orb spawned within 6 yd was often taken without the Bloodmage moving onto it.
        if (reach <= 0.0f || me->GetExactDist(owner) > reach)
            return;
        _taken = true;
        owner->CastSpell(owner, SPELL_DARKCASTING, true);
        CutAnimatedBloodCooldown(owner);
        me->DespawnOrUnsummon();
    }

    uint32 _tick = BloodOrbTickMs; // first test one tick after the spawn, not on it
    bool _still = false;
    bool _taken = false;
};
}

void AddSC_AscensionBloodmageTalents()
{
    new bloodmage_talent_login();
    new bloodmage_talent_events();
    new bloodmage_talent_contracts();
    RegisterSpellScript(spell_ascension_animated_blood);
    // Troisieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 5-7).
    new bloodmage_bloodgale_cast();
    RegisterSpellScript(aura_ascension_bloodmage_blood_trail);
    RegisterSpellScript(spell_ascension_bloodmage_amalgam_strike);
    RegisterCreatureAI(npc_ascension_bloodmage_amalgam);
    // Quatrieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 8-11).
    RegisterCreatureAI(npc_ascension_bloodmage_blood_orb);
}
