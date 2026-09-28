/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

// BLOODMAGE (CLASS_SON_OF_ARUGAL = 20, SpellFamilyName 26 = class id + 6, read at field 208 of
// Spell.dbc on 2026-09-20 — field 149 reads 0 for every record in the file and is not the family).
//
// This file wires the nine talents of the list whose Spell.dbc record states a promise the core
// cannot keep on its own. Every number below was read, never guessed; what could not be read is
// named in the comments and left out rather than invented.
//
//   680680 Atherann's Anguish  accumulate 30% of the damage you deal, explode on natural expiry
//   681403 Infuse              accumulate the group's damage, unleash on natural expiry
//   570023 Thirst for Blood    Thirst 1-5 -> Sated (570024), 6-10 -> Ravenous (570025)
//   704662 Shadows In The Night  the authored -5% damage taken, but only above 75% health
//   560259 Vampyr Lord         +35% damage, restricted to the Animated Blood summons
//   706683 Festering Maw       its proc restricted to Bite Wound's heal (556234)
//   704626 Thick Pelt          the attack-power part of 556233's flat reduction
//   704115 Clotting            gate Aortic Assault's closing cone on the talent, +25% on bleeders
//   680732 Dark Essence        the periodic heal on Blood Rituals allies, and only on them
//
// The class clean-up of 2026-09-28 added, further down: Thirst generation (92112) and Insatiable,
// Blood Moon, Apotheosis, Endure the Curse, Essence Harvester, Eternal Presence, Universal Donor,
// Acquired Taste, Aortic Aegis, the howl talents and Call of the Darkwing reactions, Scarlet
// Delirium, Spread Taldarams, Coagulation, Night Stalker, Dark Sigil, Blood Scent, Purify Blood,
// Blood Bond, Sacrificial Rite, Bloodsurge, and load-time contracts (one-charge "next cast" buffs,
// Blood Runs Cold, Terrorizer). Companion SQL: 2026_09_28_26_ascension_bloodmage_nettoyage.sql.
//
// CABLE. AddSC_AscensionBloodmageEvents() est declare a src/MP_loader.cpp:173 et appele depuis
// Addmod_ascension_compatScripts() a src/MP_loader.cpp:342 (relu le 2026-09-21). Les treize objets
// de script de ce fichier existent donc a l'execution. NE PAS ajouter une seconde declaration ni
// un second appel : chaque script serait enregistre deux fois, donc deux passages du crochet
// UNITHOOK_ON_DAMAGE, et les accumulateurs Anguish et Infuse compteraient chaque degat en double.
// Si un talent de ce fichier ne fait rien, le marqueur a lire n'est pas ici mais au demarrage :
// "Script named '...' is not assigned in the database." (ScriptMgr.h, P-051) signale les lignes du
// SQL compagnon non appliquees, ce qui est une panne de donnees, pas de cablage.
//
// ONE FIELD INDEX WORTH WRITING DOWN. Spell.dbc's EffectRadiusIndex is field 92+e, NOT 104+e
// (104+e is EffectChainTarget). Witnesses read on 2026-09-20: Arcane Explosion 1449 has
// field 92 = 13 and SpellRadius entry 13 is 10 yards; Chain Lightning 421 has field 92 = 0 and
// field 104 = 3, its three jumps. Likewise EffectBonusMultiplier is 229+e and EffectDamageMultiplier
// 216+e: Chain Lightning reads 0.571 at 229 (its stock coefficient) and 0.7 at 216 (the per-jump
// falloff). A first pass of this task read the radius at 104+e, concluded that Clotting and Dark
// Essence had no area at all, and was about to discard both; they are wired below because they do.

#include "AscensionPooledVitality.h"
#include "Creature.h"
#include "GameTime.h"
#include "Log.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <limits>
#include <list>
#include <vector>

namespace
{
enum BloodmageEventSpells : uint32
{
    // "Unleash a hemoplague upon the target area, accumulating 30% of the damage you deal to
    // affected enemies for $d, and then exploding for that amount of Shadow damage."
    SPELL_ANGUISH = 680680,
    SPELL_ANGUISH_DAMAGE = 680681,
    // "Accumulates damage dealt to the target by you, your party or raid members, and their
    // minions for $d, then unleashes the stored amount as Shadow damage."
    SPELL_INFUSE = 681403,
    SPELL_INFUSE_DAMAGE = 681404,
    // Thirst: the Bloodmage resource, 10 stacks maximum (Spell.dbc StackAmount, and the
    // DisplayMaximum of the {20, 706613} row of AscensionCompatData::ResourceDisplays).
    SPELL_THIRST = 706613,
    SPELL_THIRST_FOR_BLOOD = 570023,
    SPELL_SHADOWS_IN_THE_NIGHT = 704662,
    SPELL_VAMPYR_LORD = 560259,
    SPELL_FESTERING_MAW = 706683,
    SPELL_BITE_WOUND_HEAL = 556234,
    SPELL_THICK_PELT = 704626,
    SPELL_THICK_PELT_GUARD = 556233,
    SPELL_CLOTTING = 704115,
    SPELL_CLOTTING_DAMAGE = 806214,
    SPELL_DARK_ESSENCE = 680732,
    SPELL_DARK_ESSENCE_HEAL = 681036,
    // "Blood Rituals / Target Applied Aura", the mark Dark Liturgy leaves on allies.
    SPELL_BLOOD_RITUALS_MARK = 706623,
    // The second of the two "Cursed Form" markers every Cursed Form ability gates its cast on
    // through CasterAuraSpell; the first is AscensionBloodmage::CursedFormCheck (524861).
    // AscensionBloodmageTalents.cpp owns and documents that pair. Read back from Spell.dbc
    // field 24 on 2026-09-20: Lunge 500126 carries 525031, Rotclaw 804197 and Aortic Assault
    // 806212 carry 524861.
    SPELL_CURSED_FORM_MARKER = 525031,

    // Nettoyage du 2026-09-28. Chaque id ci-dessous a ete relu dans Spell.dbc ce jour-la ; les
    // montants sont relus sur les effets, jamais recopies ici.
    SPELL_THIRST_PASSIVE = 92112,           // "Spells that cost health now grant Thirst", e0 aura 42 -> 706613
    SPELL_THIRST_ENERGIZE = 681202,         // "Thirst SLS2 | Energizer + Crit" : l'energie de Rage de l'infobulle
    SPELL_THIRST_COMPANION = 680753,        // "Thirst SLS | Vampyr's Kiss, Sanguine Shield" : +10 %/pile a Vampiric Fang
    SPELL_INSATIABLE = 706621,              // aura 23 toutes les 3 s vers 706663
    SPELL_INSATIABLE_TICK = 706663,
    SPELL_BLOOD_MOON = 707623,              // e0 aura 354, 3 %
    SPELL_BLOOD_MOON_HEAL = 572786,
    SPELL_APOTHEOSIS = 804203,              // e0 aura 354, 100 %, "once per second"
    SPELL_APOTHEOSIS_HEAL = 807934,
    SPELL_ENDURE_THE_CURSE = 681190,
    SPELL_ENDURE_THE_CURSE_HEAL = 681189,   // HEAL_PCT 30
    SPELL_ESSENCE_HARVESTER = 806421,
    SPELL_WRETCHED = 504284,
    SPELL_ETERNAL_PRESENCE = 560001,        // e2 aura 354, 15 %
    SPELL_ETERNAL_PRESENCE_AP = 560010,
    SPELL_UNIVERSAL_DONOR = 806428,
    SPELL_ACQUIRED_TASTE = 807566,
    SPELL_ACQUIRED_TASTE_STACKS = 524905,   // effet 175 : +2 piles de Pooled Vitality
    SPELL_AORTIC_AEGIS = 806274,
    SPELL_AORTIC_AEGIS_AREA = 806275,       // porte le rayon (index 11)
    SPELL_BLOOD_VEIL = 504263,              // rang 1, chaine spell_ranks 504263 -> 572279
    SPELL_ENRAGING_HOWLS = 800766,
    SPELL_PACKLEADER_HOWLS = 504290,
    SPELL_GRIM_OMEN = 800154,
    SPELL_CALL_OF_THE_DARKWING = 801958,
    SPELL_BLOOD_CRAVING = 800780,           // partage le bit 0x800 du mot 2 avec Night Hunter's Howl
    SPELL_SHADOW_STRENGTH = 801960,
    SPELL_SHADOW_STRENGTH_DAMAGE = 536277,
    SPELL_SCREECH_OF_THE_DARKWING = 300260,
    SPELL_TALDARAMS_TORMENT = 800772,
    SPELL_BLOOD_SHIELD = 504296,
    SPELL_NIGHT_STALKER = 807774,
    SPELL_NIGHT_STALKER_BUFF = 808013,
    SPELL_DARK_SIGIL_HEAL = 707449,
    SPELL_BLOOD_SCENT_LEECH = 807300,
    SPELL_PURIFY_BLOOD_DAMAGE = 573273,
    SPELL_BLOOD_BOND_RAGE = 505325,
    SPELL_SACRIFICIAL_RITE_END = 800786,
    SPELL_BLOODCHASER = 523721,
    // Contrats de chargement
    SPELL_SOVEREIGNTY_BUFF = 504272,
    SPELL_CRIMSON_SCION_BUFF = 806425,
    SPELL_SATURATING_SUTURES_BUFF = 504137,
    SPELL_ADRENALINE_BUFF = 680678,
    SPELL_BLOOD_RUNS_COLD = 560257,
    SPELL_TERRORIZER = 806210,

    // Seconde passe du 2026-09-28 (defauts confirmes du lot, SQL 2026_09_28_26_ascension_bloodmage_nettoyage.sql, lignes des sections 1, 4, 7, 10 et 11).
    SPELL_BLOOD_SCENT = 804223,
    SPELL_SACRIFICIAL_CURSED_BLOOD = 573250, // debuff of Sacrificial Rite 800785 on the attacker
    SPELL_SACRIFICIAL_CURSED_BLOOD_DAMAGE = 806163,
    SPELL_EYES_IN_THE_NIGHT_AREA = 806723,
    SPELL_BAT_BITE = 802358,
    SPELL_BAT_BITE_HEAL = 802399,
    SPELL_SHADOW_BAT = 806066,              // e1 aura 354 -> 802399, the "100 %" of the heal
    SPELL_UNIVERSAL_DONOR_BUFF = 704238,
    SPELL_ATHERANNS_ACUITY = 504105,        // The Council Assembled: "+3 % per tick ... your next"
    SPELL_SANGUINE_SCION = 807292,
    SPELL_CRIMSON_EXPEDITION = 523727,
    SPELL_AMALGAM_BLOOD_STRIKE = 589023,
    SPELL_AMALGAM_HEAL_PASSIVE = 504711,    // e0 aura 354, 50 %: carries the amalgam's heal share

    // Troisieme passe du 2026-09-28 (defauts confirmes par la contradiction ; SQL 2026_09_28_26 : section 7 pour Shadow Vigil, aucune ligne pour les autres).
    SPELL_NIGHTMARE = 300226,               // e0 aura 23 toutes les 0,5 s vers 301273
    SPELL_NIGHTMARE_STATE = 301273,         // e0 aura 354 : les 10 % ; e2 aura 42 vers 704257
    SPELL_NIGHTMARE_DAMAGE = 303013,        // SCHOOL_DAMAGE, TARGET_UNIT_TARGET_ENEMY
    SPELL_NIGHTMARE_HEAL = 303026,          // HEAL, TARGET_UNIT_TARGET_ALLY
    SPELL_NIGHTMARE_PRICE = 704257,         // e1 (effet 174) : les 2 % de vie maximale de l'infobulle
    SPELL_MALEDICTION_MARK = 704120,        // e0 aura 271 +20 % ; e1 le saignement de 12 s
    SPELL_MALEDICTION = 800776,             // "your next 5 direct damage spells"
    SPELL_ANEURYSM = 806099,
    SPELL_ANEURYSM_HEAL = 520860,           // HEAL_PCT 5
    SPELL_SHADOW_VIGIL = 806182
};

// "Your next 5 direct damage spells" (800776). No field carries the 5: 704120 has ProcCharges 0 and a
// charge count would remove the whole aura, bleed included, when it ran out.
constexpr uint32 MaledictionHits = 5;

// Eyes In The Night (806723): "you generate a large amount of threat against the enemy that triggered it".
// DECISION, carried by no field: the Bloodmage gains threat equal to this multiple of the direct damage
// the ally has just taken. Named here so the number is visible and can be changed in one place.
constexpr float EyesInTheNightThreatMultiplier = 5.0f;

// Aortic Assault has seven ranks — 806212, then 807780-807785, which AscensionSpellProgressionData.h
// lists as its rank-ups at levels 24, 32, 40, 48, 56 and 60. Effect 2 of all seven was read and is
// the same SPELL_AURA_PERIODIC_TRIGGER_SPELL of amplitude 3000 on 806214. They are named in the
// companion SQL, not here: the script keys on that effect's shape, not on a list of ids.

constexpr uint32 BloodmageFamily = 26;

// Script-value keys on an accumulator mark. The key space belongs to the aura instance and is
// arbitrary; the payload id carries the running total, and this one the "already paying out"
// flag. Neither can collide: nothing else scripts these two auras.
constexpr uint32 KEY_UNLEASHING = 1;

// Infuse's unleash share. THIS 100 IS NOT READ FROM THE DATA, and it is the only number in this
// file that no record states: 681403's effect 1 carries MiscValueB 0, where Atherann's Anguish
// carries 30 and Genesis 500501 carries 50, and its description names no percentage either
// ("...then unleashes the stored amount as Shadow damage"). That sentence is taken literally.
constexpr int32 InfuseUnleashPercent = 100;

// Every creature Animated Blood can leave behind: worms, parasites and the rank 3 amalgam.
// Duplicated from AscensionBloodmageTalents.cpp, which owns the list and which this task is not
// allowed to modify; the two copies must stay in step.
constexpr uint32 AnimatedBloodSummons[] = {325301, 335301, 315301};

bool IsAnimatedBlood(uint32 entry)
{
    return std::find(std::begin(AnimatedBloodSummons), std::end(AnimatedBloodSummons), entry) !=
        std::end(AnimatedBloodSummons);
}

// How many accumulator marks are alive in the whole realm. UNITHOOK_ON_DAMAGE runs for every
// damage event of every one of the ~700 units in world, and the Infuse branch cannot filter on
// the attacker's class (a party member of any class, or their minion, feeds the mark). This
// counter turns the common case into one relaxed atomic load. Maps are updated from several
// threads, so a plain int here would be a data race.
std::atomic<uint32> ActiveInfuse{0};
std::atomic<uint32> ActiveAnguish{0};

// Suppresses accumulation while an unleash is resolving.
//
// WHAT IT ACTUALLY COVERS, read back from the data rather than assumed: Infuse's payload 681404 is
// TARGET_UNIT_DEST_AREA_ENEMY at EffectRadiusIndex 13 (field 92, = 10 yards), so one unleash
// splashes onto the neighbouring enemies, and every Infuse mark standing in that splash — including
// the legitimate marks of other groups — would accumulate the payout. This gate drops that
// accumulation for the whole realm for the duration of the resolution. It is deliberately
// conservative: it silences marks that have nothing to do with the unleash, and that is accepted.
//
// WHAT IT IS NOT FOR, contrary to what this comment claimed until 2026-09-20: Atherann's Anguish
// does not need it, and the two facts that were invoked for it are both false. Its three effects
// are all SPELL_EFFECT_APPLY_AURA, so each marked enemy owns its own UnitAura (UnitAura::
// FillTargetMap, SpellAuras.cpp: the "non-area aura" branch pushes the aura's own unit owner) and
// expires in ITS OWN Unit::_UpdateSpells pass — the expiry loop at Unit.cpp:4240 walks the
// HOLDER's m_ownedAuras, never the caster's — so the marks of a pack never expire in one pass.
// And its payload 680681 is single target (ImplicitTargetA 6, no radius), so it cannot reach the
// other marks of the pack at all. The one mark it can reach is the one being paid, which
// KEY_UNLEASHING already covers on its own.
//
// A map is updated by a single thread at a time, and the triggered cast resolves inside the same
// call, so a thread_local depth is the right scope.
thread_local uint32 UnleashDepth = 0;

struct UnleashGuard
{
    UnleashGuard() { ++UnleashDepth; }
    ~UnleashGuard() { --UnleashDepth; }
    UnleashGuard(UnleashGuard const&) = delete;
    UnleashGuard& operator=(UnleashGuard const&) = delete;
};

// fetch_sub on zero wraps to UINT32_MAX and would leave the realm-wide gate permanently open,
// so the decrement never goes below zero.
void ReleaseMark(std::atomic<uint32>& counter)
{
    uint32 seen = counter.load(std::memory_order_relaxed);
    while (seen && !counter.compare_exchange_weak(seen, seen - 1, std::memory_order_relaxed))
        ;
}

bool IsBloodmage(Unit const* unit)
{
    return unit && unit->IsPlayer() && unit->getClass() == CLASS_SON_OF_ARUGAL;
}

// Custom base points travel through native float arithmetic before returning to int32, the same
// clamp AscensionBloodmageSecondary.cpp and AscensionBloodmageVitality.cpp already use.
int32 ClampBasePoints(uint64 amount)
{
    uint32 const maximum = uint32(std::nextafter(float(std::numeric_limits<int32>::max()), 0.0f));
    return int32(std::min<uint64>(amount, maximum));
}

// Recharge interne portee par l'instance d'aura du talent : rend vrai au plus une fois par
// `windowMs` et arme la fenetre. La valeur disparait avec l'aura (changement de spe, mort pour un
// sort non passif), ce qui est accepte : aucune infobulle ne promet une recharge qui survit a l'aura.
bool TryInternalCooldown(Aura* aura, uint32 key, uint64 windowMs)
{
    if (!aura)
        return false;
    uint64 const now = std::max<uint64>(uint64(GameTime::GetGameTimeMS().count()), 1);
    uint64 const last = aura->GetScriptValue(key);
    if (last && now >= last && now - last < windowMs)
        return false;
    aura->SetScriptValue(key, now);
    return true;
}

// Les hurlements (Howl spells) de l'infobulle d'Enraging Howls, Packleader, Grim Omen et Shadow
// Strength. Recenses dans Spell.dbc le 2026-09-28 par masque de famille 26 : mot 1 0x4 = Monstrous
// Howl 804811 seul ; mot 2 0x20000 = Blood Howl 800782 (et 804811) ; mot 2 0x80 = Wicked Howl 804207
// et Shadow Howl 806177 ; mot 2 0x800 = Night Hunter's Howl 500124 et ses rangs 501680-501686, mais
// aussi Blood Craving 800780, qui n'est pas un hurlement et est ecarte par son id. Revitalizing Howl
// 804196 et Monstrous Howl | Heal 804091 ne portent aucun bit et sont nommes.
bool IsHowl(SpellInfo const* info)
{
    if (!info || info->SpellFamilyName != BloodmageFamily || info->Id == SPELL_BLOOD_CRAVING)
        return false;
    flag96 const& f = info->SpellFamilyFlags;
    return (f[1] & 0x4) || (f[2] & (0x20000 | 0x80 | 0x800)) || info->Id == 804196 || info->Id == 804091;
}

// The shared body of the two accumulators: pay out what the mark stored, once, on natural expiry.
// `percent` is the share the record authors; 100 means "the stored amount", which is what Infuse's
// description says and all its data allows (see the Infuse note in the metadata pass below).
//
// A REFRESH KEEPS THE RUNNING TOTAL, and that is a decision, not an oversight. Recasting the mark
// on an already-marked enemy goes through Unit::_TryStackingOrRefreshingExistingAura, which ends
// in Aura::ModStackAmount (Unit.cpp:4969) and never re-runs an AfterEffectApply registered with
// AURA_EFFECT_HANDLE_REAL — the `_counted` flags of the two scripts below rely on exactly that.
// So neither the script instance nor its stored value is rebuilt: the mark pays once, for
// everything accumulated since it was FIRST applied, and a recast only extends the window. The
// descriptions ("accumulating ... for $d, and then exploding for that amount") do not say which
// of the two it should be, and this is the behaviour Genesis (AscensionRunemasterGenesis.cpp)
// already has; it is inherited from that pattern rather than chosen here, and it is written down
// so the next reader does not take it for a bug.
void UnleashMark(AuraScript* script, uint32 payloadId, int32 percent)
{
    Aura* mark = script->GetAura();
    Unit* caster = script->GetCaster();
    Unit* target = script->GetTarget();
    AuraApplication const* application = script->GetTargetApplication();
    // Natural expiry only: a dispel, a death, an early removal or a refresh pays nothing. Same
    // test as Genesis (AscensionRunemasterGenesis.cpp) and Firebrand.
    if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
        return;
    if (!caster || !target || !target->IsAlive() || !caster->IsInWorld() ||
        !caster->IsValidAttackTarget(target))
        return;
    uint64 const stored = mark->GetScriptValue(payloadId);
    if (!stored)
        return;
    // Re-entrancy guard on the aura itself, set and never cleared: this handler only runs on the
    // removal that destroys the aura, and the payload deals damage to this very target while the
    // mark may still sit in the target's applied-aura map.
    if (mark->GetScriptValue(KEY_UNLEASHING))
        return;
    mark->SetScriptValue(KEY_UNLEASHING, 1);
    uint64 const payout = stored * uint64(std::clamp(percent, 0, 100)) / 100;
    if (!payout)
        return;
    UnleashGuard guard;
    caster->CastCustomSpell(payloadId, SPELLVALUE_BASE_POINT0, ClampBasePoints(payout), target,
        TRIGGERED_FULL_MASK);
}

// ---------------------------------------------------------------------------------------------
// Accumulation, and the two flat damage reductions that also need Unit::DealDamage.
//
// UNITHOOK_ON_DAMAGE rather than an AllSpellScript: the tooltips say "the damage you deal" and
// "damage dealt to the target" without qualifying it, and Unit::DealDamage is the single point
// every kind of damage passes through — melee swings, spell hits, periodic ticks — with the
// amount already mitigated. ALLSPELLHOOK_ON_HIT_RESULT would miss auto attacks entirely.
//
// The filter is ordered cheapest first on purpose: two relaxed atomic loads and a pointer test
// before anything touches an aura map.
class bloodmage_damage_events : public UnitScript
{
public:
    bloodmage_damage_events() : UnitScript("bloodmage_damage_events", true, {UNITHOOK_ON_DAMAGE}) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim)
            return;

        // Shadows In The Night (704662): "While above 75% health, your damage taken is reduced by
        // $s2%." Effect 1 authors a permanent SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN of -5 on every
        // school (MiscValue 127), i.e. the reduction applies at ALL times, which is not what the
        // talent promises. The metadata pass below turns that effect into a DUMMY so the core
        // stops applying it, and the condition is enforced here instead. The 5 is read back from
        // the very effect that was disarmed, never hard-coded.
        //
        // TWO DEPARTURES FROM THE AURA THAT WAS DISARMED, both forced by the hook this uses, both
        // stated here because neither is visible from the tooltip:
        //   - ORDER. Aura 87 is consumed inside Unit::SpellDamageBonusTaken (Unit.cpp:9351) and
        //     Unit::MeleeDamageBonusTaken (Unit.cpp:10856), i.e. BEFORE absorption. OnDamage is
        //     called from Unit::DealDamage (Unit.cpp:1004), i.e. AFTER it. So a shield now soaks
        //     the unreduced amount and the 5% is taken from whatever got through. Accepted: this
        //     is the only hook that also sees auto attacks.
        //   - SCOPE. Those two native callers only ever run on damage that has an attacker.
        //     DealDamage also carries falls, drowning and lava, which Player::EnvironmentalDamage
        //     (Player.cpp:883) pushes through it with attacker == victim; the disarmed aura never
        //     touched those. The `attacker` and `attacker != victim` tests below keep them out —
        //     the talent says "your damage taken", not "your cliff fall". They cannot be obtained
        //     by moving this block below the shared guard on the next lines, which also returns on
        //     UnleashDepth: the reduction must keep applying to an unleash payload.
        //     RESIDUAL, and accepted knowingly: `attacker != victim` is the widest filter this
        //     hook offers — UnitScript::OnDamage is handed (attacker, victim, damage) and no
        //     damagetype (UnitScript.h:70), so SELF_DAMAGE cannot be told apart from a genuinely
        //     self-inflicted spell. A Bloodmage spell that damages its own caster therefore loses
        //     the 5% it would have had from the native aura. No such spell is wired in this file;
        //     the alternative, keeping falls in scope, is the worse of the two errors.
        if (damage && attacker && attacker != victim && IsBloodmage(victim))
            if (AuraEffect const* shadows = victim->GetAuraEffect(SPELL_SHADOWS_IN_THE_NIGHT, EFFECT_1))
                if (victim->HealthAbovePct(75))
                {
                    int32 const pct = std::clamp(std::abs(shadows->GetAmount()), 0, 100);
                    damage -= CalculatePct(damage, pct);
                }

        // Placed BEFORE the shared guard below on purpose: an unleash payout (UnleashDepth) is damage
        // the Bloodmage deals and takes like any other, and these talents say "all damage".
        if (damage && attacker && attacker != victim && IsBloodmage(attacker) && attacker->IsAlive())
        {
            // Blood Moon (707623): "While below 75% health, you now heal for $s1% of all damage you
            // deal." Effect 0 is aura 354, which has no handler in this core (SpellAuraEffects.cpp:419),
            // and the record has ProcFlags 0: only this hook sees every kind of damage. The 3 is read
            // on the effect. 572786 already carries CANT_CRIT and IGNORE_CASTER_MODIFIERS in the DBC,
            // and EffectBonusMultiplier 0; SpellHealingBonusDone (Unit.cpp:10090) still applies the
            // percentage healing-done auras to it, as it does for every leech in this kit.
            if (AuraEffect const* moon = attacker->GetAuraEffect(SPELL_BLOOD_MOON, EFFECT_0, attacker->GetGUID()))
                if (attacker->HealthBelowPct(75))
                    if (uint32 heal = CalculatePct(damage, std::max(moon->GetAmount(), 0)))
                        attacker->CastCustomSpell(SPELL_BLOOD_MOON_HEAL, SPELLVALUE_BASE_POINT0,
                            ClampBasePoints(heal), attacker, TRIGGERED_FULL_MASK);

            // Apotheosis (804203): "heal for $s1% of all damage dealt ... Can only trigger once per
            // second." Same dead aura 354; the 100 is effect 0's amount, the second is the tooltip's
            // (no field carries it). The Rage drain per second is native (ManaPerSecond 50).
            if (AuraEffect const* trance = attacker->GetAuraEffect(SPELL_APOTHEOSIS, EFFECT_0, attacker->GetGUID()))
                if (uint32 heal = CalculatePct(damage, std::max(trance->GetAmount(), 0)))
                    if (TryInternalCooldown(trance->GetBase(), SPELL_APOTHEOSIS_HEAL, 1000))
                        attacker->CastCustomSpell(SPELL_APOTHEOSIS_HEAL, SPELLVALUE_BASE_POINT0,
                            ClampBasePoints(heal), attacker, TRIGGERED_FULL_MASK);
        }

        // Talents of the Bloodmage as the one being hit. OnDamage runs before Unit::DealDamage reads
        // the victim's health (Unit.cpp:1005 then 1148), so a heal cast here lands before the hit.
        if (damage && attacker && attacker != victim && IsBloodmage(victim) && victim->IsAlive())
        {
            uint32 const health = victim->GetHealth();
            bool const survives = health > damage;

            // Endure the Curse (681190): "taking damage that would reduce your health below 10% will
            // instantly heal you for $681189s1% of your maximum health. Can only occur once." The
            // 30 lives in 681189 and stays there. A blow bigger than the remaining health still kills:
            // "would reduce your health below 10%" is read as a survivable blow, and that is a choice.
            if (Aura* endure = victim->GetAura(SPELL_ENDURE_THE_CURSE, victim->GetGUID()))
                if (!endure->GetScriptValue(SPELL_ENDURE_THE_CURSE_HEAL) && survives &&
                    health - damage < victim->CountPctFromMaxHealth(10))
                {
                    endure->SetScriptValue(SPELL_ENDURE_THE_CURSE_HEAL, 1);
                    victim->CastSpell(victim, SPELL_ENDURE_THE_CURSE_HEAL, TRIGGERED_FULL_MASK);
                }

            // Essence Harvester (806421): "Being struck by an enemy while below 35% health instantly
            // resets the cooldown of your Vampiric Fang and grants you Wretched. Can only occur once
            // per minute." 504284 does both (its effect 1 is 195 on 804726 with MiscValueB 1, all
            // ranks). Its CasterAuraState 13 is skipped by a triggered cast, hence the explicit test.
            if (Aura* harvester = victim->GetAura(SPELL_ESSENCE_HARVESTER, victim->GetGUID()))
                if (survives && health - damage < victim->CountPctFromMaxHealth(35) &&
                    victim->IsValidAttackTarget(attacker) &&
                    TryInternalCooldown(harvester, SPELL_WRETCHED, 60000))
                    victim->CastSpell(victim, SPELL_WRETCHED, TRIGGERED_FULL_MASK);

            // Eternal Presence (560001) effect 2: "you now gain attack power equal to $s3% of the
            // damage taken for $560010d. Can only occur once every 10 sec." Effect 2 is aura 354 and
            // only the owner of the talent holds it (effects 0/1 are the raid aura); the 10 s is the
            // tooltip's, no field carries it.
            if (AuraEffect const* presence = victim->GetAuraEffect(SPELL_ETERNAL_PRESENCE, EFFECT_2, victim->GetGUID()))
                if (uint32 ap = CalculatePct(damage, std::max(presence->GetAmount(), 0)))
                    if (TryInternalCooldown(presence->GetBase(), SPELL_ETERNAL_PRESENCE_AP, 10000))
                        victim->CastCustomSpell(SPELL_ETERNAL_PRESENCE_AP, SPELLVALUE_BASE_POINT0,
                            ClampBasePoints(ap), victim, TRIGGERED_FULL_MASK);
        }

        // Cursed Blood (573250), the debuff Sacrificial Rite 800785 puts on its attackers: "dealing damage
        // now causes you to take ... Shadow Damage" (806163, "Can only occur once per sec"). Its effect 1
        // (aura 42 -> 806163) is TARGET_UNIT_CASTER: it lands on the Bloodmage, with ProcFlags 0, and the
        // attacker never paid. The marked unit is recognised by EFFECT_0 (aura 118, TARGET_UNIT_TARGET_ENEMY):
        // the Bloodmage's own copy only holds effect 1, so a HasAura test would make him pay for his own
        // damage. The once-per-second window is armed on the mark BEFORE the cast; the 806163 hit comes
        // back here with the Bloodmage as attacker (original caster), who holds no EFFECT_0: no loop.
        if (damage && attacker && attacker != victim && attacker->IsAlive() && attacker->IsInWorld())
            if (AuraEffect const* mark = attacker->GetAuraEffect(SPELL_SACRIFICIAL_CURSED_BLOOD, EFFECT_0))
                if (Aura* rite = mark->GetBase(); rite &&
                    TryInternalCooldown(rite, SPELL_SACRIFICIAL_CURSED_BLOOD_DAMAGE, 1000))
                    attacker->CastSpell(attacker, SPELL_SACRIFICIAL_CURSED_BLOOD_DAMAGE, TRIGGERED_FULL_MASK,
                        nullptr, nullptr, rite->GetCasterGUID());

        if (!damage || !attacker || attacker == victim || UnleashDepth)
            return;

        // Atherann's Anguish: "the damage YOU deal to affected enemies". Caster-scoped, so two
        // Bloodmages plaguing the same pack each feed their own mark and never someone else's.
        if (ActiveAnguish.load(std::memory_order_relaxed) && IsBloodmage(attacker))
            if (Aura* mark = victim->GetAura(SPELL_ANGUISH, attacker->GetGUID()))
                if (!mark->GetScriptValue(KEY_UNLEASHING))
                    mark->SetScriptValue(SPELL_ANGUISH_DAMAGE,
                        mark->GetScriptValue(SPELL_ANGUISH_DAMAGE) + damage);

        if (!ActiveInfuse.load(std::memory_order_relaxed))
            return;
        // Infuse: "damage dealt to the target by you, your party or raid members, and their
        // minions". The attacker can be anyone, so resolve it to the player behind it once and
        // only after the marks have been found.
        auto const bounds = victim->GetAppliedAuras().equal_range(SPELL_INFUSE);
        if (bounds.first == bounds.second)
            return;
        Player* source = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!source)
            return;
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            Aura* mark = itr->second->GetBase();
            if (mark->GetScriptValue(KEY_UNLEASHING))
                continue;
            Unit* caster = mark->GetCaster();
            Player* owner = caster ? caster->ToPlayer() : nullptr;
            if (!owner || (owner != source && !owner->IsInSameRaidWith(source)))
                continue;
            mark->SetScriptValue(SPELL_INFUSE_DAMAGE, mark->GetScriptValue(SPELL_INFUSE_DAMAGE) + damage);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// 680680 Atherann's Anguish. The running total lives on the aura instance, so it dies with the
// mark, cannot leak between two applications and cannot be shared between two enemies of the pack.
class aura_ascension_bloodmage_anguish : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_anguish);
    // One increment per script instance at most, so the realm-wide gate can never drift upwards
    // if the core ever handled the effect twice.
    bool _counted = false;

    bool Validate(SpellInfo const* info) override
    {
        // Keyed on the id: the ids are what this logic depends on, and a SpellFamilyName guard
        // that ever stopped matching would disarm the script without logging a line (P-055).
        return info && info->Id == SPELL_ANGUISH &&
            info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_ANGUISH_DAMAGE &&
            ValidateSpellInfo({SPELL_ANGUISH_DAMAGE});
    }

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
            return;
        _counted = true;
        ActiveAnguish.fetch_add(1, std::memory_order_relaxed);
    }

    void Unleash(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        // Release first and unconditionally: every path out of Unleash must leave the counter
        // balanced, including the early returns for a dispel or a dead target.
        if (_counted)
        {
            _counted = false;
            ReleaseMark(ActiveAnguish);
        }
        // The rate is effect 1's MiscValueB — the 30 the description quotes, and the only 30 the
        // record carries. Accumulating the raw damage and taking the share once loses less than
        // taking 30% of every single hit.
        UnleashMark(this, SPELL_ANGUISH_DAMAGE, GetSpellInfo()->Effects[EFFECT_1].MiscValueB);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_anguish::Apply, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_anguish::Unleash, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 681403 Infuse. Same shape, one difference: the record authors no share at all (effect 1's
// MiscValueB is 0, where Atherann's Anguish carries its 30 and Genesis its 50), and no percentage
// appears in its description. "Unleashes the stored amount" is therefore taken literally: 100%.
class aura_ascension_bloodmage_infuse : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_infuse);
    bool _counted = false;

    bool Validate(SpellInfo const* info) override
    {
        return info && info->Id == SPELL_INFUSE &&
            info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_INFUSE_DAMAGE &&
            ValidateSpellInfo({SPELL_INFUSE_DAMAGE});
    }

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
            return;
        _counted = true;
        ActiveInfuse.fetch_add(1, std::memory_order_relaxed);
    }

    void Unleash(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        if (_counted)
        {
            _counted = false;
            ReleaseMark(ActiveInfuse);
        }
        UnleashMark(this, SPELL_INFUSE_DAMAGE, InfuseUnleashPercent);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_infuse::Apply, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_infuse::Unleash, EFFECT_0,
            SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---------------------------------------------------------------------------------------------
// 570023 Thirst for Blood. "Receive a bonus based on your Thirst level. Between 1-5 stacks of
// Thirst: Spell haste increased by $570024s1%. Between 6-10 stacks of Thirst: The bonus damage
// dealt by spell critical strikes is increased by $570025s1%."
//
// Both halves exist and work on their own: 570024 "Sated" is a SPELL_AURA_HASTE_SPELLS of 10, and
// 570025 "Ravenous" is a SPELL_AURA_MOD_CRIT_DAMAGE_BONUS of 100 on the magic schools (MiscValue
// 126) whose effect 1 is a SPELL_EFFECT_REMOVE_AURA on 570024. Nothing ever granted either of
// them: 570023's own two effects are inert DUMMYs, and neither id appears anywhere in the module
// or in spell_proc / spell_linked_spell / spell_script_names.
//
// The tier is derived from the live Thirst stack, never stored. Both tier spells are read out of
// the talent's own record — effect 0 triggers the high tier (570025), effect 1 the low one
// (570024) — so a DBC import that repoints them is followed and no id is written twice.
void SyncThirstTiers(Player* player, uint32 stacks)
{
    if (!IsBloodmage(player))
        return;
    SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_THIRST_FOR_BLOOD);
    uint32 const high = talent ? talent->Effects[EFFECT_0].TriggerSpell : 0;
    uint32 const low = talent ? talent->Effects[EFFECT_1].TriggerSpell : 0;
    if (!high || !low)
        return;
    // "Between 1-5" and "Between 6-10" are the only boundaries written anywhere; 10 is also
    // Thirst's StackAmount in Spell.dbc and the DisplayMaximum of its ResourceDisplays row. A
    // count above the authored maximum still reads as the high tier.
    bool const eligible = player->IsAlive() && player->IsInWorld() &&
        player->HasAura(SPELL_THIRST_FOR_BLOOD);
    uint32 const grant = !eligible || !stacks ? 0 : stacks <= 5 ? low : high;
    for (uint32 tier : {low, high})
        if (tier != grant)
            player->RemoveAurasDueToSpell(tier, player->GetGUID());
    if (grant && !player->HasAura(grant, player->GetGUID()))
        player->CastSpell(player, grant, true);
}

uint32 ThirstStacks(Player const* player)
{
    Aura const* thirst = player ? player->GetAura(SPELL_THIRST) : nullptr;
    return thirst ? thirst->GetStackAmount() : 0;
}

// Riding on Thirst itself. Thirst already carries `aura_ascension_resource_talent_refresh`, which
// proves the idiom: an AfterEffectApply registered with AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK is
// re-entered on every stack change, because Aura::SetStackAmount calls AuraEffect::ChangeAmount
// with onStackOrReapply = true, which re-handles the effect (SpellAuras.cpp, Aura::SetStackAmount).
class aura_ascension_bloodmage_thirst_tiers : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_thirst_tiers);

    void Apply(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        SyncThirstTiers(player, GetStackAmount());
        // 92112: "Reaching $706613u stacks makes you Insatiable, reducing your damage dealt by
        // $706663s1% every $706621t1 sec until Thirst ends." The maximum is Thirst's own StackAmount.
        if (IsBloodmage(player) && player->IsAlive() && player->IsInWorld() &&
            player->HasAura(SPELL_THIRST_PASSIVE) && GetSpellInfo()->StackAmount &&
            GetStackAmount() >= GetSpellInfo()->StackAmount && !player->HasAura(SPELL_INSATIABLE, player->GetGUID()))
            player->CastSpell(player, SPELL_INSATIABLE, true);
        // Vampiric Fang: "Damage increased by $680753s3% for each Thirst stack." 680753 is Thirst's
        // companion record (StackAmount 10, effect 2 +10% on Vampiric Fang, effect 0 the Sanguine
        // Shield absorb): nothing applied it. It mirrors the Thirst stack count and goes with Thirst.
        if (IsBloodmage(player) && player->IsAlive() && player->IsInWorld())
        {
            Aura* companion = player->GetAura(SPELL_THIRST_COMPANION, player->GetGUID());
            if (!companion)
                companion = player->AddAura(SPELL_THIRST_COMPANION, player);
            if (companion)
            {
                uint32 const cap = std::max<uint32>(companion->GetSpellInfo()->StackAmount, 1);
                uint8 const wanted = uint8(std::clamp<uint32>(GetStackAmount(), 1, cap));
                if (companion->GetStackAmount() != wanted)
                    companion->SetStackAmount(wanted);
            }
        }
    }

    void Remove(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        SyncThirstTiers(target->ToPlayer(), 0);
        // "until Thirst ends": Insatiable, its stacking malus and the Rage companion of 92112 all
        // belong to this Thirst and go with it, whatever removed it (Vampiric Fang, expiry, death).
        for (uint32 id : {uint32(SPELL_INSATIABLE), uint32(SPELL_INSATIABLE_TICK), uint32(SPELL_THIRST_ENERGIZE),
                 uint32(SPELL_THIRST_COMPANION)})
            target->RemoveAurasDueToSpell(id, target->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_thirst_tiers::Apply, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_thirst_tiers::Remove, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The talent can be learned, unlearned or lost to a spec change while Thirst is already up, so the
// tier has to be re-derived at both ends. The removal path clears unconditionally rather than
// going through SyncThirstTiers, whose HasAura(talent) test is not reliable while the talent aura
// is itself being unapplied.
class aura_ascension_bloodmage_thirst_for_blood : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_thirst_for_blood);

    void Refresh(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        SyncThirstTiers(player, ThirstStacks(player));
    }

    void Clear(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* player = GetTarget();
        SpellInfo const* talent = GetSpellInfo();
        for (uint8 index : {uint8(EFFECT_0), uint8(EFFECT_1)})
            if (uint32 tier = talent->Effects[index].TriggerSpell)
                player->RemoveAurasDueToSpell(tier, player->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_thirst_for_blood::Refresh,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_thirst_for_blood::Clear,
            EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// ---------------------------------------------------------------------------------------------
// 560259 Vampyr Lord: "Increases the damage dealt by your Animated Blood by $s1%."
//
// Effect 0 is SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS (190), which this core does implement
// (SpellEffects.cpp maps it to Spell::EffectApplyAreaAura, and UnitAura::FillTargetMap gives it
// its own branch that collects every summon in Unit::m_Controlled). So the +35
// SPELL_AURA_MOD_DAMAGE_PERCENT_DONE on every school (MiscValue 127) is already granted by the
// data — to EVERY summon the Bloodmage owns, not only to Animated Blood. Read in the core, not
// observed in game: the talent is not silent, it is too generous, and the repair is a target
// filter, not a bonus. Adding a second +35% here would double the talent.
//
// Aura::CanBeAppliedOn consults DoCheckAreaTarget for every candidate including the aura's own
// owner, so the owner must be let through or effect 1 would stop applying to the Bloodmage.
class aura_ascension_bloodmage_vampyr_lord : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_vampyr_lord);

    bool CheckAreaTarget(Unit* target)
    {
        // GetAura()->GetOwner() rather than GetUnitOwner(): the latter asserts the aura is a unit
        // aura, and this hook must stay safe whatever the record is later made to create.
        if (!target || target == GetAura()->GetOwner())
            return true;
        Creature* summon = target->ToCreature();
        return summon && IsAnimatedBlood(summon->GetEntry());
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(aura_ascension_bloodmage_vampyr_lord::CheckAreaTarget);
    }
};

// ---------------------------------------------------------------------------------------------
// 706683 Festering Maw: "Healing with Bite Wound now reduces the cooldown of Lunge by 1 sec."
//
// The payload already works: effect 0 is a SPELL_AURA_PROC_TRIGGER_SPELL on 706718, whose single
// effect is SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN (165) with MiscValue 500126 (Lunge) and a base
// value of -1000, and that effect has a real handler (Spell::EffectAscensionModifyCooldown ->
// ModifyAscensionCooldown, SpellEffects.cpp). The proc row added on 2026-09-20 also exists:
//   spell_proc(706683): SpellFamilyName 26, SpellFamilyMask 0, ProcFlags 1024
//                       (PROC_FLAG_DONE_SPELL_NONE_DMG_CLASS_POS), AttributesMask 2.
// With SpellFamilyMask 0 that row matches EVERY positive, dmg-class-none family 26 spell the
// Bloodmage casts, so today any heal at all shortens Lunge. This check narrows it to the one
// spell the description names: 556234 "Bite Wound / Heal", the SPELL_EFFECT_HEAL that 556235
// triggers. It takes nothing away from the existing row, which is left untouched.
class aura_ascension_bloodmage_festering_maw : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_festering_maw);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BITE_WOUND_HEAL});
    }

    bool CheckProc(ProcEventInfo& event)
    {
        SpellInfo const* info = event.GetSpellInfo();
        return info && info->Id == SPELL_BITE_WOUND_HEAL;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_festering_maw::CheckProc);
    }
};

// ---------------------------------------------------------------------------------------------
// 704626 Thick Pelt: "Physical damage taken now reduces the Physical damage taken from the next
// attack within $556233d by ${($556233m1+$AP*0.2)}."
//
// Everything but the attack-power term is already in place and was verified: 704626's effect 0 is
// a SPELL_AURA_PROC_TRIGGER_SPELL on 556233, its spell_proc row exists since 2026-09-20
// (SchoolMask 1, ProcFlags 40 = PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK | PROC_FLAG_TAKEN_SPELL_MELEE_
// DMG_CLASS), and 556233 itself is a one-charge SPELL_AURA_MOD_DAMAGE_TAKEN of -15 on the
// physical school with its own non-zero DBC ProcFlags (0x2a8) so P-045 does not apply to it.
//
// What is missing is the scaling: a flat 15 points is nothing at level 80, and both descriptions
// say the reduction scales with attack power. The two disagree on the coefficient — 556233's own
// description says "$AP*0.25", 704626's says "$AP*0.2" — so the one written on the record whose
// amount this is (556233) is the one applied, and the disagreement is reported rather than
// averaged away. No other source states a coefficient.

// NOT DATA-DRIVEN, and it has to be named for that to be visible at a glance. Read on 2026-09-20
// out of 556233's own description text ("$AP*0.25"); 704626's description disagrees ("$AP*0.2").
// No DBC field carries either: EffectBonusMultiplier (field 229+e) and PointsPerComboPoint are
// 0.0 on both records, and no table of this module holds the number. A tooltip re-import will
// therefore leave this silently stale — there is nothing for the code to compare it against.
constexpr double ThickPeltAttackPowerShare = 0.25;

class bloodmage_thick_pelt_scaling : public UnitScript
{
public:
    bloodmage_thick_pelt_scaling() : UnitScript("bloodmage_thick_pelt_scaling", true,
        {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE}) { }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        if (!info || info->Id != SPELL_THICK_PELT_GUARD || index != EFFECT_0 || !IsBloodmage(caster) ||
            info->SpellFamilyName != BloodmageFamily ||
            info->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_MOD_DAMAGE_TAKEN ||
            !caster->HasAura(SPELL_THICK_PELT))
            return;
        // The authored amount is negative because it is a reduction, so the attack-power share is
        // subtracted, not added.
        double const amount = double(value) -
            double(caster->GetTotalAttackPowerValue(BASE_ATTACK)) * ThickPeltAttackPowerShare;
        if (std::isfinite(amount) && amount <= 0.0 &&
            double(float(amount)) >= double(std::numeric_limits<int32>::min()))
            value = float(amount);
    }
};

// ---------------------------------------------------------------------------------------------
// The load-time metadata pass. Everything here is guarded on the exact shape read in the DBC on
// 2026-09-20, so a later import that authors something else is left alone.
class bloodmage_event_contracts : public GlobalScript
{
public:
    bloodmage_event_contracts() : GlobalScript("bloodmage_event_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != BloodmageFamily)
            return;

        // Crimson Expedition (523727): "Increases the critical strike chance of your Bloodbolt, Blood Shards,
        // and Aortic Assault by $s1%." Its mask (word 1 0x400002) names Blood Shard, Gore Barrage, Clotting and
        // the Aortic Assault strike, not Bloodbolt (word 1 0x20000, its eight ranks). Nothing else gives
        // Bloodbolt this bonus. Set before any aura exists; the spell modifier takes the effect's mask and the
        // client receives it bit by bit (Player::AddSpellMod), so its display follows.
        if (info->Id == SPELL_CRIMSON_EXPEDITION)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
                effect.MiscValue == SPELLMOD_CRITICAL_CHANCE && effect.SpellClassMask == flag96(0, 0x400002, 0))
                effect.SpellClassMask[1] |= 0x20000;
        }

        // Bat Bite heal (802399): "healing you equal to the damage dealt" (801958). It is cast by whoever
        // bit (spell_ascension_bloodmage_bat_bite, the bats are cosmetic), on TARGET_UNIT_MASTER, which a
        // player does not have: the heal would find no one. Retargeted on the caster; its amount is the
        // resolved damage, taken as is.
        if (info->Id == SPELL_BAT_BITE_HEAL)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.Effect == SPELL_EFFECT_HEAL && effect.TargetA.GetTarget() == TARGET_UNIT_MASTER)
            {
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
                info->AscensionInheritsResolvedAmount = true;
            }
        }

        if (info->Id == SPELL_ANGUISH || info->Id == SPELL_INFUSE)
        {
            // THE TRAP OF EFFECT 1, identical in both records and identical to Genesis (500501).
            // Aura 69 is not an unknown Ascension aura: it is the stock SPELL_AURA_SCHOOL_ABSORB
            // (SpellAuraDefines.h:132) with a real implementation in Unit::CalcAbsorbResist. Left
            // as authored, the mark would put a ONE point, all-school (MiscValue 127) shield on
            // the marked enemy, and Unit.cpp removes the whole aura as soon as an absorb amount
            // reaches zero, with AURA_REMOVE_BY_ENEMY_SPELL:
            //     if (absorbAurEff->GetAmount() <= 0)
            //         absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
            // The first point of damage would delete the mark, with a removal mode that is not
            // AURA_REMOVE_BY_EXPIRE: it would never last its 10 s and never pay out. Turning the
            // effect into a DUMMY disarms the shield and keeps its MiscValueB readable — which is
            // where Atherann's Anguish's 30% lives.
            if (info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
                info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
            // The running total is in memory only. A mark restored from `character_aura` after a
            // restart would expire with nothing to pay, so it must not be saved at all.
            //
            // THIS CLEAR IS WIDER THAN IT LOOKS, and the trap is in the core, not here:
            // SPELL_ATTR0_CU_FORCE_AURA_SAVING is 0x20000800 (SpellInfo.h:206) and overlaps
            // SPELL_ATTR0_CU_IGNORE_EVADE (0x00000800, SpellInfo.h:188) and
            // SPELL_ATTR0_CU_ONLY_ONE_AREA_AURA (0x20000000, SpellInfo.h:207), so every
            // `&= ~FORCE_AURA_SAVING` in this codebase clears three flags. Harmless on these two
            // records — `spell_custom_attr` holds no row for 680680 or 681403, and neither is an
            // area aura — and it is the shared idiom of 18 other files of this module, so the
            // repair belongs in the core and not in a class file.
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }

        if (info->Id == SPELL_ANGUISH_DAMAGE || info->Id == SPELL_INFUSE_DAMAGE)
        {
            // A share of damage that has already been resolved once against this same target: it
            // must not be mitigated, buffed or critted a second time. Exactly the treatment
            // AscensionBloodmageSecondary.cpp already gives Reave's and Vampyr's Kiss's copies.
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            // The stock 3.3.5a coefficient column holds 0.25 for 680681 and 0.15 for 681404; the
            // unleashed amount is the accumulated total and nothing else.
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }

        // 681404 is deliberately NOT retargeted. It is authored as TARGET_UNIT_DEST_AREA_ENEMY
        // with EffectRadiusIndex 13 (field 92, = 10 yards): the unleash splashes around the
        // marked enemy, where Atherann's payload 680681 is single target (implicit target 6).
        // Casting it at the marked unit gives Spell::InitExplicitTargets a destination to use
        // (Spell.cpp: "try to use unit target if provided"), and the 10-yard area resolves from
        // there.

        if (info->Id == SPELL_SHADOWS_IN_THE_NIGHT &&
            info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)
        {
            // Disarm the unconditional reduction; bloodmage_damage_events re-applies it only above
            // 75% health, reading the amount back from this very effect.
            info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
        }

        // ---- Nettoyage du 2026-09-28. Each rewrite is guarded on the exact shape read that day. ----

        // Endure the Curse (681190): effect 0 is a SCHOOL_ABSORB of amount 0 (bp -1 + die 1). The core
        // removes the whole aura as soon as an absorb reaches 0 (Unit.cpp:2512-2519), so the first hit
        // of any school also took away the -30% damage taken of effect 1. Same trap as Anguish and
        // Infuse above; the "heal below 10%" half is in bloodmage_damage_events.
        if (info->Id == SPELL_ENDURE_THE_CURSE && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
            info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;

        // "Your NEXT ...": four spell-modifier buffs authored with ProcCharges 0, so every cast of the
        // window took the benefit. One charge lets the native charged spellmod consume the buff on the
        // first affected cast (Player::RemoveSpellMods, Player.cpp:10551-10575), the pattern of 707319
        // (AscensionClassMechanics.cpp:656-670). No spell_proc row may exist for these ids:
        // RemoveSpellMods skips any aura that has one.
        //   504272 Sovereignty      "the cost of your next Bloodbolt"   (stacks 5: the whole stack goes)
        //   806425 Crimson Scion    "your next Sanguine Mend ... instant cast"
        //   504137 Sat. Sutures     "the cost of your next Sanguine Mend"
        //   680678 Adrenaline       "your next Dark Liturgy or Bloodmoon Blast instant cast"
        {
            struct ChargedBuff { uint32 id; int32 misc; flag96 mask; };
            static ChargedBuff const charged[] = {
                {SPELL_SOVEREIGNTY_BUFF, SPELLMOD_COST, flag96(0, 0x20000, 0)},
                {SPELL_CRIMSON_SCION_BUFF, SPELLMOD_CASTING_TIME, flag96(0x80000, 0, 0)},
                {SPELL_SATURATING_SUTURES_BUFF, SPELLMOD_COST, flag96(0x80000, 0, 0)},
                {SPELL_ADRENALINE_BUFF, SPELLMOD_CASTING_TIME, flag96(0, 0x2800, 0)}};
            for (ChargedBuff const& buff : charged)
                if (info->Id == buff.id)
                {
                    SpellEffectInfo const& mod = info->Effects[EFFECT_0];
                    if (info->ProcCharges == 0 && info->ProcFlags == 0 && mod.Effect == SPELL_EFFECT_APPLY_AURA &&
                        mod.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && mod.MiscValue == buff.misc &&
                        mod.SpellClassMask == buff.mask)
                        info->ProcCharges = 1;
                    else
                        LOG_ERROR("module.ascension_compat", "Bloodmage: unexpected record {} for its one-charge contract", info->Id);
                }
        }

        // Blood Runs Cold (560257): "Your Blood Pact now also increases your healing received by $s1%."
        // Its flat modifier is authored on SPELLMOD_EFFECT1, i.e. Blood Pact's effect 0 (the -50%
        // ranged hit chance, which it turned into -30%). The healing is Blood Pact's effect 1 (aura
        // 118, base 0), reached by SPELLMOD_EFFECT2 (Unit.cpp:12129-12148). Word 2 0x10000 is Blood
        // Pact 801955 and Blood Pact SLS 801964 (one effect, untouched by EFFECT2). The client learns
        // the modifier's op from SMSG_SET_FLAT_SPELL_MODIFIER, so the tooltip follows.
        if (info->Id == SPELL_BLOOD_RUNS_COLD)
        {
            SpellEffectInfo& mod = info->Effects[EFFECT_0];
            if (mod.Effect == SPELL_EFFECT_APPLY_AURA && mod.ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER &&
                mod.MiscValue == SPELLMOD_EFFECT1 && mod.SpellClassMask == flag96(0, 0, 0x10000))
                mod.MiscValue = SPELLMOD_EFFECT2;
            else
                LOG_ERROR("module.ascension_compat", "Bloodmage: unexpected Blood Runs Cold record {}", info->Id);
        }

        // Terrorizer (806210): "Increases the damage of Veinburst, Bloodfang Bite, and Claw Sweep on
        // enemies below 35% health by $s1%." Aura 112 with the raw private selector 20007 / 13, which
        // the core only honours on a converted aura 303 (Unit.cpp:8852-8862 and 10792-10802: state in
        // MiscValue, 20007 in MiscValueB, the class mask as filter, the state tested on the VICTIM).
        // Precedents: AscensionTemplarContracts.cpp:108, AscensionClassMechanics.cpp:585-597.
        // Enduring 300585 carries the same selector but its tooltip says "while below 35% health"
        // (the Bloodmage): it is handled on the caster's health in AscensionBloodmageSecondary.cpp.
        if (info->Id == SPELL_TERRORIZER)
        {
            SpellEffectInfo& bonus = info->Effects[EFFECT_0];
            if (bonus.Effect == SPELL_EFFECT_APPLY_AURA && bonus.ApplyAuraName == SPELL_AURA_OVERRIDE_CLASS_SCRIPTS &&
                bonus.MiscValue == 20007 && bonus.MiscValueB == AURA_STATE_HEALTHLESS_35_PERCENT &&
                bonus.TargetA.GetTarget() == TARGET_UNIT_CASTER)
            {
                bonus.ApplyAuraName = SPELL_AURA_MOD_DAMAGE_DONE_VERSUS_AURASTATE;
                bonus.MiscValue = AURA_STATE_HEALTHLESS_35_PERCENT;
                bonus.MiscValueB = ASCENSION_CLASSMASK_AURASTATE_DAMAGE;
            }
            else
                LOG_ERROR("module.ascension_compat", "Bloodmage: unexpected Terrorizer record {}", info->Id);
        }

        // Blood Scent (804223, Ferocity talent): the passive carries TargetAuraState 13 (below 35 %), and a
        // talent passive is cast on the player himself (Player::_addTalentAurasAndSpells), so
        // SpellInfo::CheckTarget tests the PLAYER's health (SpellInfo.cpp:1996-2000): learned or restored
        // above 35 % health, the aura was never applied. The "below 35 %" condition stays where it
        // belongs: the proc Check (the victim's health), 807300's own TargetAuraState on the enemy, and the
        // aura 303 misc 13 of Accursed Form. Precedent: AscensionWitchHunterStake.cpp (680540).
        if (info->Id == SPELL_BLOOD_SCENT)
        {
            if (info->HasAttribute(SPELL_ATTR0_PASSIVE) && info->TargetAuraState == AURA_STATE_HEALTHLESS_35_PERCENT)
                info->TargetAuraState = 0;
            else if (info->TargetAuraState)
                LOG_ERROR("module.ascension_compat", "Bloodmage: unexpected Blood Scent record {}", info->Id);
        }

        // Nightmare (300226): its two payloads receive a share of an amount already resolved against this
        // target (bloodmage_cast_events). Same treatment as the unleash payloads above.
        if (info->Id == SPELL_NIGHTMARE_DAMAGE || info->Id == SPELL_NIGHTMARE_HEAL)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }

        // Transient companions of Thirst (92112): they belong to a live Thirst stack and are cleared
        // when it ends. Restored from `character_aura` without it they would last forever (both are
        // infinite-duration records). Same three-flag overlap note as above.
        if (info->Id == SPELL_INSATIABLE || info->Id == SPELL_THIRST_ENERGIZE || info->Id == SPELL_THIRST_COMPANION)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }

        if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_THIRST_FOR_BLOOD))
            if (info->Id == talent->Effects[EFFECT_0].TriggerSpell ||
                info->Id == talent->Effects[EFFECT_1].TriggerSpell)
            {
                // Sated and Ravenous are re-derived from the live Thirst stack on every stack
                // change; a copy restored from `character_aura` would be a tier with no resource
                // behind it. Same three-flag overlap as above (SpellInfo.h:206/188/207); harmless
                // for the same two reasons, checked on 570024 and 570025.
                info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
                info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
            }
    }
};

// ---------------------------------------------------------------------------------------------
// 704115 Clotting: "At the end of Aortic Assault's duration, all enemies in a $806214a1 yd frontal
// cone suffer ${$806214m1*$<scalingbp>+$AP*0.5} Physical damage. Damage dealt by this effect is
// increased by $s2% against bleeding targets."
//
// Aortic Assault already fires the cone: effect 2 of each of its seven ranks is a
// SPELL_AURA_PERIODIC_TRIGGER_SPELL of amplitude 3000 on 806214, on a 3000 ms aura, so it ticks
// once as the ability ends. 806214 is a real 10-yard TARGET_UNIT_CONE_ENEMY_54 hit
// (EffectRadiusIndex 13) and its base points are already level-normalised by the module's
// $scalingbp table (AscensionScalingBaseData.h carries {806214, 1}).
//
// Two things were missing, and both are the talent itself:
//   - the gate. The description reads "$?s704115[ ... ][]": the cone belongs to Clotting, yet the
//     periodic effect fires for every Bloodmage, talent or not.
//   - "$s2% against bleeding targets", the 25 that effect 1 of 704115 carries.
//
// The "$AP*0.5" of the description is the spell_bonus_data row of 2026_09_28_26 (ap_bonus 0.5,
// direct_bonus 0): Unit::SpellDamageBonusDone adds it to DoneTotal, after CalcValue and after the
// $scalingbp normaliser (a base value hook), so it is not scaled. The +25 % below multiplies the hit,
// AP term included, as the tooltip says. 0.5 is 704115's text; Aortic Assault's button says 0.6.
class aura_ascension_bloodmage_aortic_assault : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_aortic_assault);

    bool Validate(SpellInfo const* info) override
    {
        return info && info->Effects[EFFECT_2].TriggerSpell == SPELL_CLOTTING_DAMAGE &&
            ValidateSpellInfo({SPELL_CLOTTING_DAMAGE});
    }

    void Clot(AuraEffect const* /*effect*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(SPELL_CLOTTING))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_bloodmage_aortic_assault::Clot,
            EFFECT_2, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

class spell_ascension_bloodmage_clotting : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_clotting);

    void Bleeding()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const dealt = GetHitDamage();
        if (!caster || !target || dealt <= 0 || !target->HasAuraState(AURA_STATE_BLEEDING))
            return;
        // The 25 is effect 1 of the talent, read back, never written here. Its aura type is 112
        // SPELL_AURA_OVERRIDE_CLASS_SCRIPTS with MiscValue 20007, a class-script id this core does
        // not know, so the effect carries the number and nothing else: it has to be spent by hand.
        AuraEffect const* talent = caster->GetAuraEffect(SPELL_CLOTTING, EFFECT_1);
        if (!talent)
            return;
        int32 const pct = std::clamp(talent->GetAmount(), 0, 1000);
        if (!pct)
            return;
        int64 const bonus = int64(dealt) * pct / 100;
        SetHitDamage(int32(std::min<int64>(int64(dealt) + bonus, std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_bloodmage_clotting::Bleeding);
    }
};

// ---------------------------------------------------------------------------------------------
// 680732 Dark Essence: "Casting Cursed Form abilities or Bloodbolt will now heal allies affected
// by Blood Rituals for ${$681036m1+$681036ppl1+$BH*0.1} every $681036t1 sec for $681036d."
//
// 681036 "Dark Essence / Heal" is complete on its own: SPELL_AURA_PERIODIC_HEAL, 1500 ms
// amplitude, 3000 ms duration, TARGET_SRC_CASTER / TARGET_UNIT_SRC_AREA_ALLY at EffectRadiusIndex
// 23 (= 40 yards), EffectBonusMultiplier 1.0. Nothing ever cast it, and nothing restricted it to
// the marked allies the description names. Since 2026-09-28 the 1.0 is replaced by the tooltip's
// "$BH*0.1": spell_bonus_data row 681036 (dot_bonus 0.1) of 2026_09_28_26_ascension_bloodmage_nettoyage.sql.
//
// "Cursed Form abilities" is not a guess: every one of them gates its own cast on a CasterAuraSpell
// marker, 525031 or 524861, which is the pair AscensionBloodmageTalents.cpp already documents and
// mirrors onto the real form state. Bloodbolt is the empowerment family AscensionPooledVitality.h
// already names. Both are read, not enumerated here.
//
// HOW WIDE THAT SET IS, counted on 2026-09-20 rather than assumed: 86 Spell.dbc records of family
// 26 (field 208) carry one of the two markers at CasterAuraSpell (field 24), and 85 of them are
// castable — exactly one has SPELL_ATTR0_PASSIVE. It is NOT a list of attacks: Ironhide 801956,
// Eternal Resolve 801962, Endure the Curse 681189/681190, Terrify 804198 and the howls (Blood Howl
// 800782, Monstrous Howl 804091 and 804811, Wicked Howl 804207) sit in it beside Lunge 500126,
// Rotclaw 804197, Reave 800490 and Aortic Assault 806212, so casting a defensive or a shout also
// triggers the heal. That is left as is: the description says "Cursed Form abilities", not "Cursed
// Form attacks", and no record states a narrower set — narrowing it to damaging spells would be a
// rule this project has nowhere read. Triggered sub-casts are excluded below by IsTriggered(), and
// passives are never cast.
//
// The tooltip's second line ("Healing from Sanguine Essence increases target's critical strike
// chance by $680693s3% for $680593d") is left alone: 680693 has a single effect, so its "s3" names
// an effect that does not exist, and no other record states that chance.
class bloodmage_dark_essence_casts : public AllSpellScript
{
public:
    bloodmage_dark_essence_casts() : AllSpellScript("bloodmage_dark_essence_casts",
        {ALLSPELLHOOK_ON_CAST}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
    {
        // Ordered cheapest first: this hook runs for every cast in the realm.
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!IsBloodmage(player) || spell->IsTriggered() || !info ||
            info->SpellFamilyName != BloodmageFamily || !player->HasAura(SPELL_DARK_ESSENCE))
            return;
        bool const cursedForm = info->CasterAuraSpell == SPELL_CURSED_FORM_MARKER ||
            info->CasterAuraSpell == AscensionBloodmage::CursedFormCheck;
        if (!cursedForm &&
            AscensionBloodmage::GetEmpowerment(info->Id) != AscensionBloodmage::Bloodbolt)
            return;
        player->CastSpell(player, SPELL_DARK_ESSENCE_HEAL, true);
    }
};

class spell_ascension_bloodmage_dark_essence : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_dark_essence);

    void Select(std::list<WorldObject*>& targets)
    {
        // "allies affected by Blood Rituals", and no one else. Whose mark it is, is not stated, so
        // the caster of 706623 is not checked.
        targets.remove_if([](WorldObject* object)
        {
            Unit* unit = object ? object->ToUnit() : nullptr;
            return !unit || !unit->HasAura(SPELL_BLOOD_RITUALS_MARK);
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(
            spell_ascension_bloodmage_dark_essence::Select, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
    }
};

// ---------------------------------------------------------------------------------------------
// Nettoyage du 2026-09-28 : les talents dont la promesse se lit au lancement ou au toucher d'un sort
// du Bloodmage, et qu'aucune ligne spell_proc ne pouvait exprimer (condition de cout, liste de sorts
// sans masque commun, parcours de groupe). Tous sont des auras 42 ou 4 veritables : HasAura suffit
// (aucun effet 190 ici). AUCUNE ligne spell_proc n'est posee pour ces talents : elle ferait compter
// le meme effet deux fois.
class bloodmage_cast_events : public AllSpellScript
{
public:
    bloodmage_cast_events() : AllSpellScript("bloodmage_cast_events",
        {ALLSPELLHOOK_ON_BEFORE_EFFECTS, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT,
            ALLSPELLHOOK_ON_SUCCESSFUL_INTERRUPT}) { }

    static void CallDarkwing(Player* player)
    {
        player->CastSpell(player, SPELL_CALL_OF_THE_DARKWING, true);
    }

    // Everything that reacts to Call of the Darkwing itself, whoever triggered it (a talent proc, a
    // howl, Monstrous Howl's charges): the cast reaches this hook triggered or not.
    static void OnDarkwing(Player* player)
    {
        // Shadow Strength (801960): "triggering Call of the Darkwing now reduces the cooldown of all
        // Howl spells by $/1000;556239S1 sec. Can only occur once per sec." Its two effects are the
        // two cooldown records (556239: 806177/500124/804207 ; 556238: 804195/800782/804811).
        if (Aura* strength = player->GetAura(SPELL_SHADOW_STRENGTH, player->GetGUID()))
            if (TryInternalCooldown(strength, SPELL_CALL_OF_THE_DARKWING, 1000))
                for (uint8 i : {uint8(EFFECT_0), uint8(EFFECT_1)})
                    if (uint32 reduce = strength->GetSpellInfo()->Effects[i].TriggerSpell)
                        player->CastSpell(player, reduce, true);
        // Screech of the Darkwing (300260): "Your effects that trigger Call of the Darkwing now trigger
        // an additional Call of the Darkwing. Can only occur once per sec." The window is armed before
        // the extra cast, which re-enters this hook and stops there.
        if (Aura* screech = player->GetAura(SPELL_SCREECH_OF_THE_DARKWING, player->GetGUID()))
            if (TryInternalCooldown(screech, SPELL_CALL_OF_THE_DARKWING, 1000))
                CallDarkwing(player);
    }

    // Spell::_cast calls this after TakePower and BEFORE handle_immediate (Spell.cpp:3973, 3991, 4079): an
    // instant ability resolves its hits (OnSpellHitResult) before OnSpellCast, so a mark set in OnSpellCast
    // would arrive too late for them.
    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!IsBloodmage(player) || !info || info->SpellFamilyName != BloodmageFamily || spell->IsTriggered() ||
            !player->IsAlive() || spell->GetScriptValue(SPELL_NIGHTMARE))
            return;

        // Nightmare (300226): "While above 50% maximum health, abilities that cost Rage now consume
        // $704257s2% of your maximum health to damage or heal your target for an additional $301273s1% of
        // the value." 301273 (kept up by 300226 every 0.5 s) holds the 10 on an aura 354 that has no
        // handler, and its 704257 trigger has ProcFlags 0 and no spell_proc row: nothing ran. No chance is
        // rolled: the visible tooltip promises it on every use (the "$h% chance" is 303013's, unreachable).
        // The price is paid once per cast, never per target; the mark is read back at each hit below.
        // The Rage cost was already taken (Spell::TakePower runs before this hook). Above 50 %, a 2 % price
        // cannot kill.
        if (info->PowerType == POWER_RAGE && spell->GetPowerCost() > 0 && player->HasAura(SPELL_NIGHTMARE_STATE) &&
            player->HealthAbovePct(50))
            if (SpellInfo const* price = sSpellMgr->GetSpellInfo(SPELL_NIGHTMARE_PRICE))
                if (int32 const pct = std::clamp(price->Effects[EFFECT_1].CalcValue(player), 0, 100))
                    if (uint32 const cost = player->CountPctFromMaxHealth(pct); cost && cost < player->GetHealth())
                    {
                        player->ModifyHealth(-int32(cost));
                        spell->SetScriptValue(SPELL_NIGHTMARE, 1);
                    }
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
    {
        // Cheapest first: this hook runs for every cast in the realm.
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!IsBloodmage(player) || !info || info->SpellFamilyName != BloodmageFamily || !player->IsAlive())
            return;
        if (info->Id == SPELL_CALL_OF_THE_DARKWING)
        {
            OnDarkwing(player);
            return;
        }
        if (spell->IsTriggered())
            return;

        // Thirst (92112, level 10 Blood specialization passive): "Spells that cost health now grant
        // Thirst." The health cost is taken in Spell::TakePower (Spell.cpp:3973) before this hook
        // (Spell.cpp:4129), so GetPowerCost() is what was actually paid. 92112's effect 0 is a real
        // aura 42 (HasAura holds); its ProcFlags are 0 and a "costs health" condition does not exist
        // in spell_proc. Its trigger is read from the record. 681202 is Thirst's own "Generates
        // $/10;681202s1 Rage": cast with each stack this passive grants, and removed with Thirst
        // (aura_ascension_bloodmage_thirst_tiers). Other Thirst sources (Wretched, Sanguine Rupture
        // with 500107) carry their own income and do not pass here.
        if (info->PowerType == POWER_HEALTH && spell->GetPowerCost() > 0)
            if (AuraEffect const* passive = player->GetAuraEffect(SPELL_THIRST_PASSIVE, EFFECT_0))
                if (uint32 thirst = passive->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
                {
                    player->CastSpell(player, thirst, true);
                    // Only with a live Thirst to carry it: its removal is what clears the companion.
                    if (player->HasAura(thirst))
                        player->CastSpell(player, SPELL_THIRST_ENERGIZE, true);
                }

        // Universal Donor (806428) and Acquired Taste (807566): "Casting Infuse or Vampyr's Kiss now
        // ...". Vampyr's Kiss is the whole family-26 mask 0x8000 of word 0 (its seven ranks, and
        // nothing else, counted 2026-09-28). Acquired Taste casts 524905 only: that record carries the
        // "2 stacks"; its effect 1 (680687) would add a third.
        if (info->Id == SPELL_INFUSE || (info->SpellFamilyFlags[0] & 0x8000))
        {
            if (Aura const* donor = player->GetAura(SPELL_UNIVERSAL_DONOR, player->GetGUID()))
                if (uint32 buff = donor->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
                    player->CastSpell(player, buff, true);
            if (player->HasAura(SPELL_ACQUIRED_TASTE))
                player->CastSpell(player, SPELL_ACQUIRED_TASTE_STACKS, true);
        }

        if (IsHowl(info))
        {
            // Enraging Howls (800766) and Packleader (504290): "Casting Howl spells now ...". Their
            // payloads are their own effect 0 triggers (504157 party aura, 504289 summons aura).
            for (uint32 talent : {uint32(SPELL_ENRAGING_HOWLS), uint32(SPELL_PACKLEADER_HOWLS)})
                if (Aura const* aura = player->GetAura(talent, player->GetGUID()))
                    if (uint32 payload = aura->GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
                        player->CastSpell(player, payload, true);
            // Grim Omen (800154): "... or using Howl spells, now triggers Call of the Darkwing".
            if (player->HasAura(SPELL_GRIM_OMEN))
                CallDarkwing(player);
            // Shadow Strength (801960): "Casting Howl spells now deal ... Shadow damage to all nearby
            // enemies ... Can only occur once per sec." The damage record is 536277; its attribute
            // formula (STA, AGI, AP) is carried by no field and is not added.
            if (Aura* strength = player->GetAura(SPELL_SHADOW_STRENGTH, player->GetGUID()))
                if (TryInternalCooldown(strength, SPELL_SHADOW_STRENGTH_DAMAGE, 1000))
                    player->CastSpell(player, SPELL_SHADOW_STRENGTH_DAMAGE, true);
        }
    }

    // Malediction (800776): "increasing the damage they take from your next 5 direct damage spells by
    // $704120s1%". 704120's aura 271 is a plain multiplier with no counter (Unit.cpp:9426-9431), so every
    // masked hit of the 12 s took the bonus. A hit is counted when it actually received the multiplier
    // (the effect's class mask, and a spell that does not skip the taken modifiers), triggered or not:
    // Sanguine Rupture's damage record 800775 is triggered and sits in the mask. The fifth hit has already
    // had its bonus (the damage is computed before this hook); then the multiplier drops to 0 and the bleed
    // of effect 1 keeps its 12 s. A recast refreshes the aura, whose amounts the core recalculates
    // (Aura::SetStackAmount): the duration going up is how the counter sees it and starts again.
    static void CountMalediction(Player* player, Unit* target, SpellInfo const* info)
    {
        if (target == player || info->AscensionInheritsResolvedAmount ||
            info->HasAttribute(SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS))
            return;
        Aura* mark = target->GetAura(SPELL_MALEDICTION_MARK, player->GetGUID());
        AuraEffect* bonus = mark ? mark->GetEffect(EFFECT_0) : nullptr;
        if (!bonus || bonus->GetAuraType() != SPELL_AURA_MOD_DAMAGE_FROM_CASTER || !bonus->IsAffectedOnSpell(info))
            return;
        // Key SPELL_MALEDICTION: hits counted. Key SPELL_MALEDICTION_MARK: the duration seen last, plus 1.
        uint64 const now = uint64(std::max(mark->GetDuration(), 0)) + 1;
        uint64 const seen = mark->GetScriptValue(SPELL_MALEDICTION_MARK);
        uint64 hits = mark->GetScriptValue(SPELL_MALEDICTION);
        if (seen && now > seen)
            hits = 0;
        mark->SetScriptValue(SPELL_MALEDICTION_MARK, now);
        if (!bonus->GetAmount())
            return;
        mark->SetScriptValue(SPELL_MALEDICTION, ++hits);
        if (hits >= MaledictionHits)
            bonus->ChangeAmount(0);
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32 healing,
        bool critical) override
    {
        Unit* casterUnit = spell->GetCaster();
        Player* player = casterUnit ? casterUnit->ToPlayer() : nullptr;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!IsBloodmage(player) || !info || info->SpellFamilyName != BloodmageFamily || !target ||
            miss != SPELL_MISS_NONE || !player->IsAlive())
            return;
        if (damage)
            CountMalediction(player, target, info);
        if (spell->IsTriggered())
            return;

        // Nightmare (300226): the cast paid its price in OnSpellBeforeEffects. Each target hit or healed
        // receives the extra share of what this very hit did. The 10 is 301273's effect 0 (the live aura
        // if it is still up, the record otherwise: it is refreshed every 0.5 s and may just have lapsed).
        // The payloads are triggered and marked as resolved amounts (contract above): nothing counts twice.
        if (spell->GetScriptValue(SPELL_NIGHTMARE) && (damage || healing))
        {
            int32 pct = 0;
            if (AuraEffect const* state = player->GetAuraEffect(SPELL_NIGHTMARE_STATE, EFFECT_0))
                pct = state->GetAmount();
            else if (SpellInfo const* state = sSpellMgr->GetSpellInfo(SPELL_NIGHTMARE_STATE))
                pct = state->Effects[EFFECT_0].CalcValue(player);
            pct = std::clamp(pct, 0, 100);
            if (damage && target != player && target->IsAlive() && player->IsValidAttackTarget(target))
            {
                if (uint32 const extra = CalculatePct(damage, pct))
                    player->CastCustomSpell(SPELL_NIGHTMARE_DAMAGE, SPELLVALUE_BASE_POINT0, ClampBasePoints(extra),
                        target, TRIGGERED_FULL_MASK);
            }
            else if (healing && target->IsAlive() && (target == player || player->IsValidAssistTarget(target)))
            {
                if (uint32 const extra = CalculatePct(healing, pct))
                    player->CastCustomSpell(SPELL_NIGHTMARE_HEAL, SPELLVALUE_BASE_POINT0, ClampBasePoints(extra),
                        target, TRIGGERED_FULL_MASK);
            }
        }

        // Grim Omen (800154): "Critical strikes with Ravenous Strike or Bloodfang Bite" (word 1 bits
        // 0x1 and 0x800000; the second also covers Crimson Maw 803388, the empowered Bite).
        if (critical && (info->SpellFamilyFlags[1] & 0x800001) && target != player &&
            player->HasAura(SPELL_GRIM_OMEN))
            CallDarkwing(player);

        // Aortic Aegis (806274): "Your Blood Veil now applies to the party members of the target."
        // Not a spell_proc row: 806275 would cast rank 1 (504263) whatever rank is known, on
        // TARGET_UNIT_SRC_AREA_PARTY, i.e. the caster's party. Here the SAME rank goes to every living
        // member of the target's own party within 806275's radius around the target. The copies are
        // triggered, so they never come back through this hook.
        // Blood Veil is TARGET_UNIT_TARGET_ALLY, the caster included: on a self-cast "the party members of
        // the target" are the Bloodmage's own, and the loop below already skips the target itself.
        if (sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_BLOOD_VEIL &&
            player->HasAura(SPELL_AORTIC_AEGIS))
            if (Player* ally = target->ToPlayer())
                if (Group* group = ally->GetGroup())
                {
                    SpellInfo const* area = sSpellMgr->GetSpellInfo(SPELL_AORTIC_AEGIS_AREA);
                    float const radius = area ? area->Effects[EFFECT_0].CalcRadius(player) : 0.0f;
                    std::vector<Player*> members;
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                        if (Player* member = ref->GetSource())
                            if (member != ally && member->IsInWorld() && member->IsAlive() &&
                                member->IsInMap(ally) && member->InSamePhase(ally) &&
                                ally->IsInSameGroupWith(member) && ally->IsWithinDistInMap(member, radius) &&
                                player->IsValidAssistTarget(member))
                                members.push_back(member);
                    for (Player* member : members)
                        player->CastSpell(member, info->Id, TRIGGERED_FULL_MASK);
                }
    }

    // Aneurysm (806099): "Successfully interrupting an enemy heals you for $520860s1% of your maximum
    // health." 520860's only trigger is 570211, which nothing grants, and no 3.3.5 ProcFlag means
    // "interrupt". The core calls this hook once per target actually interrupted (SpellEffects.cpp:4115);
    // 806099 is single target. The 5 % is 520860's HEAL_PCT, read by its own effect.
    void OnSpellSuccessfulInterrupt(Spell* spell, Unit* /*target*/) override
    {
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        if (!info || info->Id != SPELL_ANEURYSM || info->SpellFamilyName != BloodmageFamily)
            return;
        Unit* caster = spell->GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (IsBloodmage(player) && player->IsAlive() && player->IsInWorld())
            player->CastSpell(player, SPELL_ANEURYSM_HEAL, TRIGGERED_FULL_MASK);
    }
};

// 806723 Eyes In The Night, the raid aura 801959 keeps on the Bloodmage's allies: "If they have less
// health than you when this occurs, you generate a large amount of threat against the enemy that
// triggered it." The spell_proc row (2026_09_28_26) already lets the native trigger summon Call of the
// Darkwing: this runs AFTER it and never prevents it. Health is compared in absolute points, as the
// text says ("less health than you"). The amount is EyesInTheNightThreatMultiplier (a decision).
class aura_ascension_bloodmage_eyes_in_the_night : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_eyes_in_the_night);

    void Threat(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        Unit* bloodmage = GetCaster();
        Unit* ally = GetTarget();
        Unit* enemy = eventInfo.GetActor();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!IsBloodmage(bloodmage) || !bloodmage->IsAlive() || !bloodmage->IsInWorld() || !ally || ally == bloodmage ||
            !enemy || enemy == ally || enemy == bloodmage || !enemy->IsAlive() || !enemy->IsCreature() ||
            !enemy->IsInMap(bloodmage) || !enemy->GetThreatMgr().CanHaveThreatList() || !damage || !damage->GetDamage() ||
            ally->GetHealth() >= bloodmage->GetHealth() || !bloodmage->IsValidAttackTarget(enemy))
            return;
        float const amount = float(damage->GetDamage()) * EyesInTheNightThreatMultiplier;
        if (std::isfinite(amount) && amount > 0.0f)
            enemy->GetThreatMgr().AddThreat(bloodmage, amount, GetSpellInfo(), true, true);
    }

    void Register() override
    {
        AfterEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_eyes_in_the_night::Threat, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 801958 Call of the Darkwing (and 802578, its double): "Calls $s1 Shadow Bats to appear from the shadows and
// strike the target for ${$802358m1+$802358ppl1+$SP*.4+$AP*.2} Physical Damage, healing you equal to the
// damage dealt." The summon (effect 0, entry 50069, SummonProperties 61) is kept as the visual: 50069 is the
// cosmetic creature row of 2026_09_28_26. The strike and the heal are no creature's: the bats' own record
// 806066 carries the heal on an aura 354 that has no handler. Whoever casts Call of the Darkwing (the
// Bloodmage, or the ally whose Eyes In The Night aura procs: 801959 "giving them a chance") bites, once per
// bat: n is the effect's own value, the one the summon uses. The target is the explicit one if hostile,
// else the caster's victim, else an attacker of the caster; within the summon's radius (effect 0, 30 yd).
class spell_ascension_bloodmage_call_of_the_darkwing : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_call_of_the_darkwing);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BAT_BITE});
    }

    Unit* Enemy(Unit* caster)
    {
        float const reach = GetSpellInfo()->Effects[EFFECT_0].CalcRadius(caster);
        for (Unit* candidate : {GetExplTargetUnit(), caster->GetVictim(), caster->getAttackerForHelper()})
            if (candidate && candidate != caster && candidate->IsAlive() && caster->IsInMap(candidate) &&
                caster->IsValidAttackTarget(candidate) && (reach <= 0.0f || caster->IsWithinDistInMap(candidate, reach)))
                return candidate;
        return nullptr;
    }

    void Bite(SpellEffIndex /*index*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsAlive())
            return;
        int32 const bats = std::clamp(GetEffectValue(), 0, 10);
        Unit* enemy = bats ? Enemy(caster) : nullptr;
        if (!enemy)
            return;
        // The first bite can kill: stop as soon as the target is gone.
        ObjectGuid const guid = enemy->GetGUID();
        for (int32 i = 0; i < bats; ++i)
        {
            Unit* victim = ObjectAccessor::GetUnit(*caster, guid);
            if (!victim || !victim->IsAlive() || !caster->IsValidAttackTarget(victim))
                return;
            caster->CastSpell(victim, SPELL_BAT_BITE, TRIGGERED_FULL_MASK);
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_ascension_bloodmage_call_of_the_darkwing::Bite, EFFECT_0, SPELL_EFFECT_SUMMON);
    }
};

// 802358 Bat Bite: the heal "equal to the damage dealt", i.e. 806066 effect 1's 100 %, read. Effect 1 of the
// bite (TRIGGER_SPELL 802577, "Threat Redirect" to the bat's master) has no object once the caster bites
// himself: prevented. No spell_proc row on 802358 and no 806066 on any bat: the heal is counted here only.
class spell_ascension_bloodmage_bat_bite : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_bat_bite);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({SPELL_BAT_BITE_HEAL, SPELL_SHADOW_BAT});
    }

    void NoRedirect(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
    }

    void Heal()
    {
        Unit* caster = GetCaster();
        int32 const dealt = GetHitDamage();
        if (!caster || !caster->IsAlive() || dealt <= 0)
            return;
        SpellInfo const* bat = sSpellMgr->GetSpellInfo(SPELL_SHADOW_BAT);
        int32 const pct = bat ? std::clamp(bat->Effects[EFFECT_1].CalcValue(caster), 0, 100) : 0;
        if (int32 const heal = CalculatePct(dealt, pct); heal > 0)
            caster->CastCustomSpell(SPELL_BAT_BITE_HEAL, SPELLVALUE_BASE_POINT0, heal, caster, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_bloodmage_bat_bite::NoRedirect, EFFECT_1,
            SPELL_EFFECT_TRIGGER_SPELL);
        AfterHit += SpellHitFn(spell_ascension_bloodmage_bat_bite::Heal);
    }
};

// 806182 Shadow Vigil: "Envelop an ally in darkness, temporarily reducing all threat". Effect 0 is aura 103
// on TARGET_UNIT_TARGET_ALLY, whose native handler recomputes the CASTER's temporary threat modifiers
// (SpellAuraEffects.cpp:3662-3675, caster->GetThreatMgr().UpdateMyTempModifiers()): on another ally the
// aura changed nothing. UpdateMyTempModifiers sums its owner's aura-103 effects (ThreatManager.cpp:798-815),
// so recomputing the TARGET is idempotent and also right on a self-cast. Both handlers run after the effect
// is (un)registered on the target (SpellAuraEffects.cpp:858-886).
class aura_ascension_bloodmage_shadow_vigil : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_shadow_vigil);

    bool Validate(SpellInfo const* info) override
    {
        return info && info->Effects[EFFECT_0].IsAura(SPELL_AURA_MOD_TOTAL_THREAT);
    }

    void Refresh(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->GetThreatMgr().UpdateMyTempModifiers();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_bloodmage_shadow_vigil::Refresh, EFFECT_0,
            SPELL_AURA_MOD_TOTAL_THREAT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_shadow_vigil::Refresh, EFFECT_0,
            SPELL_AURA_MOD_TOTAL_THREAT, AURA_EFFECT_HANDLE_REAL);
    }
};

// 801074 Scarlet Delirium, all six ranks (spell_script_names -801074). Its ProcFlags 0xa22a8 contain
// TAKEN_PERIODIC, and with no spell_proc row the core builds an entry from them (MOD_FEAR is a
// trigger aura). The aura's own drain tick (effect 1, aura 53, every 3 s) then counted as damage that
// breaks the fear: CheckEffectProc only spares the aura's own damage while GetDuration() equals the
// maximum (SpellAuraEffects.cpp:1298-1316), and HandleBreakableCCAuraProc removes the WHOLE aura, drain
// included. "Damage caused may interrupt the effect" stays true for every other source.
class aura_ascension_bloodmage_scarlet_delirium : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_scarlet_delirium);

    bool CheckFear(AuraEffect const* /*effect*/, ProcEventInfo& eventInfo)
    {
        return eventInfo.GetSpellInfo() != GetSpellInfo();
    }

    void Register() override
    {
        DoCheckEffectProc += AuraCheckEffectProcFn(aura_ascension_bloodmage_scarlet_delirium::CheckFear,
            EFFECT_0, SPELL_AURA_MOD_FEAR);
    }
};

// 704647 Spread Taldarams (Swarm of Flies 704646): "Your Bloodbolt now refreshes the duration of
// Taldaram's Torment". Effect 1 is a 173 on MiscValue 800772, which the core resolves with
// target->GetAura(MiscValue) (SpellEffects.cpp:410-442): rank 1 only, anyone's. The chain 800772 ->
// 802580 exists in spell_ranks, so GetAuraOfRankedSpell finds any rank, restricted to the caster's
// own ("your"). Duration semantics copied from the core's MiscValueB 0 branch.
class spell_ascension_bloodmage_spread_taldarams : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_spread_taldarams);

    void Refresh(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;
        Aura* torment = target->GetAuraOfRankedSpell(SPELL_TALDARAMS_TORMENT, caster->GetGUID());
        if (!torment)
            return;
        int32 const duration = GetEffectValue();
        if (duration <= 1)
        {
            torment->RefreshDuration();
            return;
        }
        if (duration > torment->GetMaxDuration())
            torment->SetMaxDuration(duration);
        torment->SetDuration(duration);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_bloodmage_spread_taldarams::Refresh, EFFECT_1,
            SPELL_EFFECT_ASCENSION_REFRESH_AURA);
    }
};

// 706258 Coagulation: "Your Blood Shield now also dispels 1 bleed effect from you every $t1 sec."
// The record is a permanent passive aura 23 that fired 504102 every 5 s with or without Blood Shield.
class aura_ascension_bloodmage_coagulation : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_coagulation);

    void Tick(AuraEffect const* /*effect*/)
    {
        if (!GetTarget()->HasAura(SPELL_BLOOD_SHIELD))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_bloodmage_coagulation::Tick, EFFECT_0,
            SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 807774 Night Stalker: "All damage and healing done is increased by $808013s1% at night, while
// indoors, or in a dungeon." The record re-casts 808013 every 10 s with no condition at all.
// "At night" is not stated in any field: it is read as the realm's local hour between 20:00 and
// 06:00 (the hour the client's sky follows, sent at login). SUPPOSE, not measured against the sky.
class aura_ascension_bloodmage_night_stalker : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_night_stalker);

    static bool IsNight()
    {
        time_t const now = time_t(GameTime::GetGameTime().count());
        tm local{};
        localtime_r(&now, &local);
        return local.tm_hour >= 20 || local.tm_hour < 6;
    }

    void Tick(AuraEffect const* /*effect*/)
    {
        Unit* target = GetTarget();
        Map* map = target->GetMap();
        if ((map && map->IsDungeon()) || !target->IsOutdoors() || IsNight())
            return;
        PreventDefaultAction();
        target->RemoveAurasDueToSpell(SPELL_NIGHT_STALKER_BUFF, target->GetGUID());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_bloodmage_night_stalker::Tick, EFFECT_0,
            SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 560535 Dark Sigil effect 1 (aura 354): "you now heal for $s2% of all critical damage dealt". The
// spell_proc row (2026_09_28_26) gives it an entry with HitMask CRITICAL and DisableEffectsMask 1;
// effect 0 is the raid crit aura, which the raid members hold alone and which must never proc.
// Pattern: AscensionPrimalistSecondary.cpp (Volcanic Blast).
class aura_ascension_bloodmage_dark_sigil : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_dark_sigil);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_DARK_SIGIL_HEAL}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return GetCaster() == GetTarget() && eventInfo.GetActor() == GetTarget() && damage &&
            damage->GetDamage() && (eventInfo.GetHitMask() & PROC_HIT_CRITICAL);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        uint64 const heal = uint64(eventInfo.GetDamageInfo()->GetDamage()) * uint64(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        if (heal)
            GetTarget()->CastCustomSpell(SPELL_DARK_SIGIL_HEAL, SPELLVALUE_BASE_POINT0, ClampBasePoints(heal),
                GetTarget(), TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_dark_sigil::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_dark_sigil::Proc, EFFECT_1, AuraType(354));
    }
};

// 804223 Blood Scent effect 1 (aura 354): "Dealing direct damage to enemies below 35% maximum health
// now leeches health equal to $s2% of the damage dealt." 807300 is a HEALTH_LEECH of multiplier 1.0
// whose TargetAuraState is 13 (below 35%), so the leech is handed the share as its base points.
class aura_ascension_bloodmage_blood_scent : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_blood_scent);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_BLOOD_SCENT_LEECH}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        return eventInfo.GetActor() == GetTarget() && damage && damage->GetDamage() && victim &&
            victim != GetTarget() && victim->IsAlive() && victim->HealthBelowPct(35);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        uint64 const leech = uint64(eventInfo.GetDamageInfo()->GetDamage()) * uint64(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        if (leech)
            GetTarget()->CastCustomSpell(SPELL_BLOOD_SCENT_LEECH, SPELLVALUE_BASE_POINT0, ClampBasePoints(leech),
                eventInfo.GetActionTarget(), TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_blood_scent::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_blood_scent::Proc, EFFECT_1, AuraType(354));
    }
};

// 520839 Purify Blood, the raid aura of 680679: "causing allied players to deal ${$sps*.6} additional
// damage as Shadow damage when they deal direct damage." Effect 0 is aura 231 on 573273 with base
// points 0, which would relaunch 573273 for 0 damage. The 0.6 is the tooltip's and no field carries
// it (EffectMultipleValue 1.0, EffectBonusMultiplier 0): named here for that reason. The ally lands
// the hit (so range is its own), the amount is the Bloodmage's shadow spell power.
constexpr float PurifyBloodSpellPowerShare = 0.6f;

class aura_ascension_bloodmage_purify_blood : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_purify_blood);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_PURIFY_BLOOD_DAMAGE}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        return eventInfo.GetActor() == GetTarget() && GetTarget()->IsPlayer() && damage && damage->GetDamage() &&
            victim && victim != GetTarget() && victim->IsAlive() && GetTarget()->IsValidAttackTarget(victim);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* bloodmage = GetCaster();
        if (!bloodmage || !bloodmage->IsInWorld())
            return;
        float const amount = float(bloodmage->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW)) * PurifyBloodSpellPowerShare;
        if (!std::isfinite(amount) || amount < 1.0f)
            return;
        GetTarget()->CastCustomSpell(SPELL_PURIFY_BLOOD_DAMAGE, SPELLVALUE_BASE_POINT0, ClampBasePoints(uint64(amount)),
            eventInfo.GetActionTarget(), TRIGGERED_FULL_MASK, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_purify_blood::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_purify_blood::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE);
    }
};

// 504627 Blood Bond, held by the bonded ally (TargetA 57): "causing direct damage taken by the ally to
// generate $/10;505325s1 Rage and ...% base health" FOR THE BLOODMAGE. The native proc would make the
// aura's holder (the ally) cast 505325 on itself (SpellAuraEffects.cpp:7082). The threat redirection
// ($s3%) is carried by no effect of the record and is not invented.
class aura_ascension_bloodmage_blood_bond : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_blood_bond);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_BLOOD_BOND_RAGE}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return damage && damage->GetDamage() && eventInfo.GetActionTarget() == GetTarget();
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* bloodmage = GetCaster();
        if (bloodmage && bloodmage != GetTarget() && bloodmage->IsAlive() && bloodmage->IsInWorld() &&
            bloodmage->IsInMap(GetTarget()))
            bloodmage->CastSpell(bloodmage, SPELL_BLOOD_BOND_RAGE, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_bloodmage_blood_bond::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_bloodmage_blood_bond::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 800785 Sacrificial Rite: "At the end of the duration, your healing done is increased by
// $800786s1% and you are healed ...". Only a natural expiry pays, as for the accumulators above.
// The "attackers are afflicted with Cursed Blood" half is the native proc of effect 1 (spell_proc row).
class aura_ascension_bloodmage_sacrificial_rite : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_sacrificial_rite);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_SACRIFICIAL_RITE_END}); }

    void End(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (GetTargetApplication() && GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE &&
            target->IsAlive() && target->IsInWorld())
            target->CastSpell(target, SPELL_SACRIFICIAL_RITE_END, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_sacrificial_rite::End, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 553267 Bloodsurge: "At the end of the duration, or whenever you trigger Accursed Form, gain
// Bloodchaser instead." The Accursed Form half lives in AscensionBloodmageTalents.cpp (the aura hook
// that already watches the form); it removes Bloodsurge with AURA_REMOVE_BY_CANCEL, so only a
// natural expiry pays here and the two paths never both grant Bloodchaser.
class aura_ascension_bloodmage_bloodsurge : public AuraScript
{
    PrepareAuraScript(aura_ascension_bloodmage_bloodsurge);

    bool Validate(SpellInfo const* /*info*/) override { return ValidateSpellInfo({SPELL_BLOODCHASER}); }

    void End(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (GetTargetApplication() && GetTargetApplication()->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE &&
            target->IsAlive() && target->IsInWorld())
            target->CastSpell(target, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell ?
                GetSpellInfo()->Effects[EFFECT_1].TriggerSpell : uint32(SPELL_BLOODCHASER), true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_bloodmage_bloodsurge::End, EFFECT_0,
            SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

}

void AddSC_AscensionBloodmageEvents()
{
    new bloodmage_damage_events();
    new bloodmage_thick_pelt_scaling();
    new bloodmage_event_contracts();
    RegisterSpellScript(aura_ascension_bloodmage_anguish);
    RegisterSpellScript(aura_ascension_bloodmage_infuse);
    RegisterSpellScript(aura_ascension_bloodmage_thirst_tiers);
    RegisterSpellScript(aura_ascension_bloodmage_thirst_for_blood);
    RegisterSpellScript(aura_ascension_bloodmage_vampyr_lord);
    RegisterSpellScript(aura_ascension_bloodmage_festering_maw);
    RegisterSpellScript(aura_ascension_bloodmage_aortic_assault);
    RegisterSpellScript(spell_ascension_bloodmage_clotting);
    new bloodmage_dark_essence_casts();
    RegisterSpellScript(spell_ascension_bloodmage_dark_essence);
    // Nettoyage du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql).
    new bloodmage_cast_events();
    RegisterSpellScript(aura_ascension_bloodmage_scarlet_delirium);
    RegisterSpellScript(spell_ascension_bloodmage_spread_taldarams);
    RegisterSpellScript(aura_ascension_bloodmage_coagulation);
    RegisterSpellScript(aura_ascension_bloodmage_night_stalker);
    RegisterSpellScript(aura_ascension_bloodmage_dark_sigil);
    RegisterSpellScript(aura_ascension_bloodmage_blood_scent);
    RegisterSpellScript(aura_ascension_bloodmage_purify_blood);
    RegisterSpellScript(aura_ascension_bloodmage_blood_bond);
    RegisterSpellScript(aura_ascension_bloodmage_sacrificial_rite);
    RegisterSpellScript(aura_ascension_bloodmage_bloodsurge);
    // Troisieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 5-7).
    RegisterSpellScript(aura_ascension_bloodmage_eyes_in_the_night);
    RegisterSpellScript(aura_ascension_bloodmage_shadow_vigil);
    // Quatrieme passe du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql, sections 8-11).
    RegisterSpellScript(spell_ascension_bloodmage_call_of_the_darkwing);
    RegisterSpellScript(spell_ascension_bloodmage_bat_bite);
}
