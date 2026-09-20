/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <array>

namespace
{
constexpr uint32 PRIMALIST_FAMILY = uint32(CLASS_WILDWALKER) + 6;

enum PrimalistEventSpells : uint32
{
    SPELL_EARTHSHAPING             = 680441,
    SPELL_GEOMOLDING               = 560170,
    SPELL_RUPTURER                 = 706208,
    SPELL_NATURAL_EFFICIENCY       = 706167,
    SPELL_NATURAL_EFFICIENCY_HEAL  = 707806,
    SPELL_FURY_OF_THE_WILD         = 801234,
    SPELL_BESTIAL_WRATH            = 803347,
    SPELL_BESTIAL_WRATH_FOCUS      = 803348,
    SPELL_BESTIAL_WRATH_MANA       = 803350,
    SPELL_BESTIAL_WRATH_VULNERABLE = 572047,
    SPELL_BONES_MARKER             = 806552,
    SPELL_BONES_STACKER            = 806378,
    SPELL_BONES_MARK               = 806554,
    SPELL_BONES_DETONATION         = 806553
};

Player* Primalist(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_WILDWALKER ? player : nullptr;
}

// The owner of a controlled summon, when that owner is a Primalist. Anything the
// player casts himself, and every creature that is not one of his summons, leaves
// here immediately: these helpers sit on hooks that the whole world walks through.
Player* PrimalistSummonOwner(Unit* unit)
{
    if (!unit || unit->IsPlayer() || !unit->IsSummon())
        return nullptr;

    Player* player = Primalist(unit->GetOwner());
    return player && unit->GetOwnerGUID() == player->GetGUID() ? player : nullptr;
}

// Earthshaping's authored contract forbids refreshing its lifetime when a stack is
// gained ("Additional applications do not refresh duration", Spell.dbc 680441).
// ModStackAmount refreshes native timers, so the remaining time is put back. Same
// idiom as HandleAscensionClassMechanics26To32SuccessfulInterrupt.
void GainEarthshaping(Player* player)
{
    if (Aura* aura = player->GetAura(SPELL_EARTHSHAPING, player->GetGUID()))
    {
        int32 remaining = aura->GetDuration();
        if (!aura->ModStackAmount(1))
            aura->SetDuration(remaining);
        return;
    }

    player->CastSpell(player, SPELL_EARTHSHAPING, true);
}

// ---------------------------------------------------------------------------------
// 706167 "Natural Efficiency" - "Whenever you are struck by a root, stun, or
// incapacitate effect you are now instantly healed for $707806s1% of your maximum
// health."
//
// The talent's only effect is an APPLY_AURA / SPELL_AURA_DUMMY, and its EffectTriggerSpell
// is 707806 - the heal itself. Its DBC ProcFlags are 174760 = 0x2AAA8, every TAKEN damage
// flag, 0x8 PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK included. SpellMgr::LoadSpellProcs therefore
// generates a default SpellProcEntry (isTriggerAura[SPELL_AURA_DUMMY] is true,
// SpellMgr.cpp:1984), and AuraEffect::HandleProc routes SPELL_AURA_DUMMY to
// HandleProcTriggerSpellAuraProc, which casts 707806. In other words the generated proc is
// not inert: as shipped, the talent heals 4% of maximum health on EVERY hit taken, white
// swings included. No proc flag can express "a control effect landed on me", so the event
// is taken from aura application instead, and the companion SQL's DisableEffectsMask = 1
// suppresses that over-heal.
// ---------------------------------------------------------------------------------
bool IsControlEffect(SpellInfo const* info)
{
    if (!info)
        return false;

    // MECHANIC_ROOT / STUN, and the four mechanics 3.3.5 uses for "incapacitate"
    // (Polymorph, Sap, Freeze, Knockout) plus Sleep. Read in SharedDefines.h.
    constexpr uint64 CONTROL_MECHANICS =
        (UI64LIT(1) << MECHANIC_ROOT) | (UI64LIT(1) << MECHANIC_STUN) |
        (UI64LIT(1) << MECHANIC_FREEZE) | (UI64LIT(1) << MECHANIC_SLEEP) |
        (UI64LIT(1) << MECHANIC_KNOCKOUT) | (UI64LIT(1) << MECHANIC_POLYMORPH) |
        (UI64LIT(1) << MECHANIC_SAPPED);

    if (info->GetAllEffectsMechanicMask() & CONTROL_MECHANICS)
        return true;

    // Records that carry no mechanic are still caught by the aura they apply.
    for (uint8 index = EFFECT_0; index < MAX_SPELL_EFFECTS; ++index)
        if (info->Effects[index].IsAura(SPELL_AURA_MOD_ROOT) ||
            info->Effects[index].IsAura(SPELL_AURA_MOD_STUN) ||
            info->Effects[index].IsAura(SPELL_AURA_MOD_CONFUSE))
            return true;

    return false;
}

class primalist_event_auras : public UnitScript
{
public:
    primalist_event_auras() : UnitScript("primalist_event_auras", true, {UNITHOOK_ON_AURA_APPLY}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        // The heal is cast from inside Unit::_ApplyAura (Unit.cpp:5110), so this runs on
        // the map update thread of the player. Unit updates are NOT single-threaded here:
        // worldserver.conf sets MapUpdate.Threads = 10. A plain member would be shared by
        // those threads - one map could clear the flag while another is still inside the
        // heal, and a legitimate heal on one map could be swallowed by a heal in flight on
        // another. thread_local makes this exactly what it has to be: a call-stack guard.
        static thread_local bool healing = false;

        Player* player = Primalist(unit);
        if (!player || !aura || !player->IsAlive() || !player->IsInWorld())
            return;

        // The heal applies no aura itself, but the healing event can feed other procs,
        // and one of those could land a new control effect back here. One heal per entry,
        // never a chain.
        if (healing)
            return;

        if (aura->GetCasterGUID() == player->GetGUID())
            return;

        // Cheap tests first: this hook is walked by every aura application in the world.
        if (!player->HasAura(SPELL_NATURAL_EFFICIENCY) || !IsControlEffect(aura->GetSpellInfo()))
            return;

        // "Struck by": a root or a stun laid on by a friendly caster - a vehicle, a
        // scripted hold, an ally's own mechanic - is not a hit. The accessor lookup
        // is only paid once the two tests above have already passed. A caster that
        // cannot be resolved (trap, departed owner) is treated as hostile.
        Unit* caster = aura->GetCaster();
        if (caster && (caster == player || player->IsFriendlyTo(caster)))
            return;

        // 707806 is a single SPELL_EFFECT_HEAL_PCT on the caster. The flag is cleared by
        // the scope guard, so an exception escaping CastSpell cannot leave this thread
        // permanently unable to heal.
        struct Reentry
        {
            bool& flag;
            explicit Reentry(bool& f) : flag(f) { flag = true; }
            ~Reentry() { flag = false; }
        } reentry(healing);

        player->CastSpell(player, SPELL_NATURAL_EFFICIENCY_HEAL, true);
    }
};

// ---------------------------------------------------------------------------------
// 706208 "Rupturer" - critical-strike branch.
//
// The 2026-09-20 data pass gave 706208 a spell_proc row that grants the base
// Geomolding stack on Seismic Spike / Seismic Crash damage. Two sentences were left
// out there, and only one of them is still missing:
//   * "Critically striking ... generates an additional stack of Geomolding plus a
//     stack of Earthshaping" - done here.
//   * "Casting Lithic Lance now reduces the cooldown of Terrasurge" - already native:
//     Lithic Lance 681251 effect 2 is a SPELL_EFFECT_TRIGGER_SPELL towards 681354,
//     whose effect 0 is SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN with MiscValue 681119
//     (Terrasurge rank 1) and -4000 ms. Nothing is added here; see the report for the
//     fact that the authored record does not gate it behind the talent.
//
// The hook does NOT call PreventDefaultAction: the native handler keeps granting the
// base stack, and only the critical bonus is added on top.
// ---------------------------------------------------------------------------------
class aura_ascension_primalist_rupturer : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_rupturer);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Id == SPELL_RUPTURER &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            spellInfo->Effects[EFFECT_0].IsAura(SPELL_AURA_PROC_TRIGGER_SPELL) &&
            spellInfo->Effects[EFFECT_0].TriggerSpell == SPELL_GEOMOLDING &&
            ValidateSpellInfo({SPELL_GEOMOLDING, SPELL_EARTHSHAPING});
    }

    // GetUnitOwner, not GetTarget: AuraScript::GetTarget switches on the hook state
    // and SPELL_SCRIPT_STATE_LOADING (2) reaches none of its cases, so in Load it
    // logs "called in a hook in which the call won't have effect" and returns null -
    // which would refuse the script on every aura, silently.
    bool Load() override { return Primalist(GetUnitOwner()) != nullptr; }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return;

        Player* player = Primalist(GetTarget());
        if (!player || !player->IsAlive() || !player->IsInWorld())
            return;

        // Geomolding caps itself at its DBC StackAmount; a second application is
        // exactly what the tooltip's "additional stack" asks for.
        player->CastSpell(player, SPELL_GEOMOLDING, true);
        GainEarthshaping(player);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_rupturer::Proc,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// ---------------------------------------------------------------------------------
// 801234 "Fury of the Wild" - "When you cast a Boon, you cast the same Boon on your
// pet at 50% effectiveness."
//
// The talent's single effect is an inert APPLY_AURA / SPELL_AURA_DUMMY. Nothing is
// computed here: the copies already exist in Spell.dbc as the five "(Pet)" records,
// built from SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS (190) effects. No number is
// invented here - but the authored copies are NOT a uniform "50% effectiveness", and
// this is what Spell.dbc actually holds (read today, effect by effect):
//   500935 Turtle  aura 15 = 4, aura 87 = -10
//     -> 500938    aura 15 = 4, aura 87 = -10       IDENTICAL, not reduced at all
//   500939 Bear    auras 166 / 213 / 138, all 10
//     -> 504611    auras 166 = 5 and 138 = 5; aura 213 (melee haste) is GONE,
//                  replaced by the inert dummy
//   500943 Hawk    aura 72 = -10, aura 216 = 12, aura 227 = 1 (triggers 997800)
//     -> 504612    aura 72 = -5, aura 227 = 1; aura 216 (spell haste) is GONE. The
//                  "1" is the periodic trigger's own value, unchanged, not half of 12
//   504856 Lion    THREE auras 117 = 40 (mechanics 1, 5, 10)
//     -> 505218    TWO auras 117 = 20 (mechanics 1, 5); the third is GONE
//   800137 Wolf    aura 31 = 25, aura 49 = 8
//     -> 802726    aura 31 = 13, aura 49 = 5; no dummy effect at all, so the player
//                  carries no application of this one, only the summons do
// So three of the five copies drop an effect on the way, and Turtle's copy is not
// weakened. That is the authored data; correcting it is a data question, not this
// script's, and nothing here compensates for it.
//
// Boon of the Eagle (504773), Boon of the Tiger, Boon of the Elements and the
// Empowered Boons have NO "(Pet)" record in Spell.dbc and are therefore left out.
// ---------------------------------------------------------------------------------
struct BoonPetCopy
{
    uint32 boon;
    uint32 petBoon;
};

constexpr std::array<BoonPetCopy, 5> FURY_OF_THE_WILD_BOONS =
{{
    {500935, 500938},   // Boon of the Turtle
    {500939, 504611},   // Boon of the Bear
    {500943, 504612},   // Boon of the Hawk
    {504856, 505218},   // Boon of the Lion
    {800137, 802726}    // Boon of the Wolf
}};

uint32 PetBoonOf(uint32 spellId)
{
    for (BoonPetCopy const& pair : FURY_OF_THE_WILD_BOONS)
        if (pair.boon == spellId)
            return pair.petBoon;
    return 0;
}

class spell_ascension_primalist_fury_of_the_wild : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_fury_of_the_wild);

    bool Validate(SpellInfo const* spellInfo) override
    {
        if (!spellInfo || spellInfo->SpellFamilyName != PRIMALIST_FAMILY)
            return false;

        uint32 petBoon = PetBoonOf(spellInfo->Id);
        SpellInfo const* copy = petBoon ? sSpellMgr->GetSpellInfo(petBoon) : nullptr;

        // Refuse the pairing unless the companion record really is the summon-side
        // copy, otherwise the talent would hand the player a second personal buff.
        return copy && copy->SpellFamilyName == PRIMALIST_FAMILY && copy->HasAreaAuraEffect();
    }

    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    void Handle()
    {
        Player* player = Primalist(GetCaster());
        uint32 petBoon = PetBoonOf(GetSpellInfo()->Id);
        if (!player || !petBoon || !player->IsAlive() || !player->IsInWorld() ||
            !player->HasAura(SPELL_FURY_OF_THE_WILD) || !player->GetGuardianPet())
            return;

        // The five player Boons are mutually exclusive through spell_group 1132
        // (stack_rule 2, SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER), but the
        // five "(Pet)" copies are in NO group and every one of them is permanent
        // (DurationIndex 21 -> SpellDuration.dbc 21 = -1). Left alone, cycling the
        // Boons would end with the summons carrying all five copies at once, for good.
        // They cannot simply be added to group 1132 either: an exclusive-from-same-
        // caster group is checked between the incoming and the existing aura without
        // regard to which is the copy (Aura::CanStackWith, SpellAuras.cpp:2001), so the
        // copy would immediately remove the player Boon that just cast it. The previous
        // copy is therefore taken down here, one line before the new one goes up.
        //
        // RemoveOwnedAura, not RemoveAurasDueToSpell: the latter only walks
        // m_appliedAuras (Unit.cpp:5471), and 802726 has no SPELL_EFFECT_APPLY_AURA
        // effect at all, so the player owns that aura without carrying an application
        // of it. Removing the owned aura takes the summons' applications with it.
        for (BoonPetCopy const& pair : FURY_OF_THE_WILD_BOONS)
            if (pair.petBoon != petBoon)
                player->RemoveOwnedAura(pair.petBoon, player->GetGUID());

        // The copy is cast on the player: its area-aura effects are the ones that
        // reach the summons, the player himself only keeps its inert dummy (and for
        // 802726, not even that).
        player->CastSpell(player, petBoon, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_primalist_fury_of_the_wild::Handle);
    }
};

// ---------------------------------------------------------------------------------
// 803347 "Bestial Wrath" - pet branch.
//
// Sentence one ("When you critically strike, your pet restores Focus") was repaired by
// the 2026-09-20 spell_proc row: effect 0 is an aura 42 towards 803348, whose
// SPELL_EFFECT_ENERGIZE targets TARGET_UNIT_PET. Sentence two is about the PET's
// critical strikes, and a pet does not proc its owner's auras: Unit::ProcSkillsAndAuras
// only walks the actor's own applications, there is no forwarding to the owner.
//
// Effect 1 of the talent is an inert APPLY_AURA / SPELL_AURA_DUMMY on the player. The
// metadata hook below turns it into SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS, the
// very pattern the five "Boon of the X (Pet)" records already ship, so the summons
// carry that effect and the player's own application keeps effect 0 alone. The pet's
// critical strikes then meet the existing spell_proc row (ProcFlags 69972, HitMask 2)
// on its own application, and this script does the work.
//
// Effect 190 pushes that application onto EVERY controlled summon of the player
// (SpellAuras.cpp:2882-2895 walks owner->m_Controlled), and PrimalistSummonOwner
// accepts exactly the same population - so without a filter a second summon would
// restore another 2% of maximum mana and lay another 572047 on the victim. The
// tooltip of 803347 says "When your pet critically strikes", singular, so Proc keeps
// the guardian pet alone. (The five "(Pet)" Boons reach every summon the same way,
// but that is the authored data's doing, not this file's.)
// ---------------------------------------------------------------------------------
class aura_ascension_primalist_bestial_wrath : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_bestial_wrath);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Id == SPELL_BESTIAL_WRATH &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            spellInfo->Effects[EFFECT_0].IsAura(SPELL_AURA_PROC_TRIGGER_SPELL) &&
            spellInfo->Effects[EFFECT_0].TriggerSpell == SPELL_BESTIAL_WRATH_FOCUS &&
            spellInfo->Effects[EFFECT_1].Effect == SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS &&
            ValidateSpellInfo({SPELL_BESTIAL_WRATH_MANA, SPELL_BESTIAL_WRATH_VULNERABLE});
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        // Effect 1 carries no EffectTriggerSpell: the shared SPELL_AURA_DUMMY /
        // SPELL_AURA_PROC_TRIGGER_SPELL case of AuraEffect::HandleProc has nothing to
        // do with it, and it must not be left to run.
        PreventDefaultAction();

        Unit* summon = GetTarget();
        Player* player = PrimalistSummonOwner(summon);
        Unit* victim = eventInfo.GetActionTarget();
        if (!player || !summon->IsAlive() || eventInfo.GetActor() != summon ||
            !victim || victim == summon || summon->IsFriendlyTo(victim))
            return;

        // "your pet", not "each of your summons": see the note above the class.
        if (summon != player->GetGuardianPet())
            return;

        // 803350 resolves TARGET_UNIT_MASTER and 803350's ENERGIZE_PCT reaches the
        // player because Unit::CanReceivePowerFromSpell falls back to the display
        // power, which is Mana for class 31. Both are cast by the summon.
        summon->CastSpell(player, SPELL_BESTIAL_WRATH_MANA, true);
        summon->CastSpell(victim, SPELL_BESTIAL_WRATH_VULNERABLE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_bestial_wrath::Proc,
            EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// ---------------------------------------------------------------------------------
// 806552 "Bring Me Their Bones" - "Mark an enemy for $d, causing damage dealt by your
// pet's abilities to the target to add a stack ... At 5 stacks, your pet consumes this
// buff to deal ${$806553m1+$AP*0.5} damage to the target, ignoring Armor."
//
// What the authored chain does and why it cannot work as written:
//   806552 effect 1 triggers 806554 "Mark" (5 stacks) on the enemy, effect 2 triggers
//   806378 "Stacker Passive on Pet", whose single effect is an aura 42 towards 806589.
//   806378 has ProcFlags 0 in Spell.dbc, so SpellMgr::LoadSpellProcs generates nothing
//   and the aura is never a proc candidate (P-045) - the companion SQL supplies the row.
//   806589 itself is broken twice over: its effect 0 is a
//   SPELL_EFFECT_ASCENSION_MODIFY_AURA_STACKS whose MiscValue is 0, and
//   ModifyAscensionAuraStacks returns at once on a zero delta; its effect 1 fires the
//   806553 detonation on EVERY hit instead of the fifth. The proc handler below
//   replaces the whole chain, which is why it prevents the default action.
//
// WHAT IS NOT WIRED, said plainly rather than guessed:
//   * "${$806553m1+$AP*0.5}". Only the m1 part exists: 806553 effect 0 is a
//     SPELL_EFFECT_SCHOOL_DAMAGE with base points 309 (value 310), and there is no
//     spell_bonus_data row for 806553 (checked in acore_world). The attack-power half
//     is therefore NOT applied and the detonation deals a flat 310. A spell_bonus_data
//     row (entry 806553, ap_bonus 0.5) would reach Unit::SpellDamageBonusDone
//     (Unit.cpp:9260-9267, called for every EffectSchoolDMG), but the caster of 806553
//     is the SUMMON, so such a row would scale on the pet's attack power, while the
//     tooltip's $AP is resolved against the player who owns 806552. That ambiguity is
//     not ours to settle from the data available, so the coefficient is left out and
//     recorded here rather than approximated.
//   * The two authored tooltips disagree about the trigger point, and the code follows
//     806552's: 806552 effect 1 triggers 806554 at ONE stack on cast, then each pet hit
//     adds one, so the detonation at 5 stacks lands on the FOURTH pet hit. That is
//     literally 806552's "At 5 stacks, your pet consumes this buff", but 806554's own
//     tooltip says "After 5 pet attacks". Reaching six is not possible: 806554's DBC
//     StackAmount is 5 and Aura::ModStackAmount caps there (SpellAuras.cpp:969-1006).
//     Making it the fifth hit needs a separate counter, i.e. new data - not done here.
// ---------------------------------------------------------------------------------
class spell_ascension_primalist_bring_me_their_bones : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_bring_me_their_bones);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo && spellInfo->Id == SPELL_BONES_MARKER &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            ValidateSpellInfo({SPELL_BONES_STACKER});
    }

    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    void Handle()
    {
        Player* player = Primalist(GetCaster());
        Guardian* pet = player ? player->GetGuardianPet() : nullptr;
        if (!pet || !pet->IsAlive() || !player->IsInWorld())
            return;

        // Effect 2 of the marker already triggers 806378, but it does so through an
        // enemy unit target (ImplicitTargetA 6), so the pet never gets it from the
        // authored chain. Cast unconditionally: 806378 has StackAmount 0, so a second
        // cast cannot double anything, it refreshes the existing application
        // (Aura::TryRefreshStackOrCreate). Skipping the cast when the pet already
        // carries the aura would be worse than useless - 806378 and 806554 share
        // DurationIndex 8 (SpellDuration.dbc 8 = 15000 ms), so a re-cast while the old
        // stacker is still running would leave the stacker expiring up to 15 s before
        // the new mark, and the pet's last hits would stop counting.
        player->CastSpell(pet, SPELL_BONES_STACKER, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_primalist_bring_me_their_bones::Handle);
    }
};

class aura_ascension_primalist_bones_stacker : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_bones_stacker);

    bool Validate(SpellInfo const* spellInfo) override
    {
        SpellInfo const* mark = sSpellMgr->GetSpellInfo(SPELL_BONES_MARK);
        return spellInfo && spellInfo->Id == SPELL_BONES_STACKER &&
            spellInfo->SpellFamilyName == PRIMALIST_FAMILY &&
            spellInfo->Effects[EFFECT_0].IsAura(SPELL_AURA_PROC_TRIGGER_SPELL) &&
            mark && mark->StackAmount > 1 && ValidateSpellInfo({SPELL_BONES_DETONATION});
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* summon = GetTarget();
        Player* player = PrimalistSummonOwner(summon);
        Unit* victim = eventInfo.GetActionTarget();
        if (!player || !summon->IsAlive() || eventInfo.GetActor() != summon ||
            !victim || victim == summon || summon->IsFriendlyTo(victim))
            return;

        // Only the enemy THIS player marked is counted. The counting is per caster; the
        // detonation is not, and the difference is worth stating: 806553 effect 1 is a
        // native SPELL_EFFECT_REMOVE_AURA (164) towards 806554, and
        // Spell::EffectRemoveAura calls RemoveAurasDueToSpell with no caster GUID
        // (SpellEffects.cpp:6554-6563). So a detonation wipes every Primalist's mark on
        // that victim, not just this one's. Suppressing it would take a SpellScript on
        // 806553 plus its spell_script_names row; it is left as authored.
        Aura* mark = victim->GetAura(SPELL_BONES_MARK, player->GetGUID());
        if (!mark)
            return;

        uint32 cap = mark->GetSpellInfo()->StackAmount;
        if (mark->ModStackAmount(1))
            return;                                 // the mark is gone, nothing to consume

        if (mark->GetStackAmount() < cap)
            return;

        // Removed before the cast, so the mark cannot be re-counted by anything the
        // detonation itself sets off.
        victim->RemoveAurasDueToSpell(SPELL_BONES_MARK, player->GetGUID());
        summon->CastSpell(victim, SPELL_BONES_DETONATION, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_bones_stacker::Proc,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

class primalist_event_metadata : public GlobalScript
{
public:
    primalist_event_metadata() : GlobalScript("primalist_event_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->SpellFamilyName != PRIMALIST_FAMILY)
            return;

        if (info->Id == SPELL_BESTIAL_WRATH)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            // Guarded by the exact shape read in Spell.dbc today, so a future data
            // pass that gives this effect real work is not silently overwritten.
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA &&
                effect.ApplyAuraName == SPELL_AURA_DUMMY && !effect.TriggerSpell)
                effect.Effect = SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS;
        }

        if (info->Id == SPELL_BONES_DETONATION)
            // "deal ... damage to the target, ignoring Armor". No DBC attribute
            // carries that; Unit.cpp:2215 reads this custom one.
            info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
    }
};
}

void AddSC_AscensionPrimalistEvents()
{
    new primalist_event_auras();
    new primalist_event_metadata();
    RegisterSpellScript(aura_ascension_primalist_rupturer);
    RegisterSpellScript(spell_ascension_primalist_fury_of_the_wild);
    RegisterSpellScript(aura_ascension_primalist_bestial_wrath);
    RegisterSpellScript(spell_ascension_primalist_bring_me_their_bones);
    RegisterSpellScript(aura_ascension_primalist_bones_stacker);
}
