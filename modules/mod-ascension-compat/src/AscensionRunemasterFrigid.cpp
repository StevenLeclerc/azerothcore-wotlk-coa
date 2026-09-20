/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

// Runemaster (class 32 CLASS_SPIRIT_MAGE, spell family 38), Arcane tree:
// Frigid Elements (707654) and the Frigid Fusion mechanic it modifies.
//
// ---------------------------------------------------------------------------------------
// FIELD OFFSETS, PINNED ON THE CORE ITSELF AND NOT ON A SURVEY
//
//   * SpellFamilyName = field 208, SpellFamilyFlags = 209/210/211.
//   * EffectSpellClassMask = fields 122-130, EFFECT-MAJOR: effect e uses 122+3e .. 124+3e.
//     DBCStructure.h:1750 declares `std::array<flag96, MAX_SPELL_EFFECTS> EffectSpellClassMask;
//     // 122-130` and Util.h:441 `class flag96 { uint32 part[3]; }`, so the nine integers are
//     read effect by effect, not part by part; SpellInfo.cpp:350 then hands
//     `EffectSpellClassMask[effIndex]` to the effect. AscensionRunemasterRunes.cpp:13-14, same
//     class and same directory, already states it.
//     A part-major reading (122+e / 125+e / 128+e) INVERTS the diagnosis on this very talent,
//     and an earlier pass of this file was written on it. Witness that separates the two, since
//     a single-effect spell such as Improved Fireball 11069 cannot: 12100..12104 ("Presence of
//     Mind" Warlock/Priest/Druid/Paladin/Shaman) have one live effect each (Effect[1] = 0) and
//     five different SpellFamilyName (5, 6, 7, 10, 11) with five different 122-124, yet all
//     five carry the SAME field 125 = 0x20400001. Part-major would make that the part B of five
//     unrelated families' real masks, which is impossible; effect-major makes it the part A of a
//     dead effect 1, which is just leftover data.
//
// 707654 "Frigid Elements", family 38, passive (read in /opt/coa/server/data/dbc/Spell.dbc):
//   E0  APPLY_AURA 107 ADD_FLAT_MODIFIER  MiscValue 10 (SPELLMOD_CASTING_TIME)   +1000
//       EffectSpellClassMask = (0, 0x08000000, 0)
//   E1  APPLY_AURA 108 ADD_PCT_MODIFIER   MiscValue 24 (SPELLMOD_BONUS_MULTIPLIER) +50
//       EffectSpellClassMask = (0, 0x08000000, 0)
//   E2  APPLY_AURA 4   DUMMY                                                      +10
//
// BOTH MASKS ARE ALREADY CORRECT, AND NOTHING HERE TOUCHES THEM. Frigid Blast carries
// SpellFamilyFlags (0, 0x08000000, 0) on all nine ranks (500118, 502853..502860), and over the
// whole Spell.dbc no other family-38 spell holds that bit in part 1: the two modifiers already
// name exactly those nine ranks. There is no empty mask, no wildcard over the 1230 spells of
// family 38, and no overreach onto the five Weapon Engraving spells that carry 0x08000000 in
// part 0 (653212, 653215, 653220, 653224, 653230) — no mask of 707654 has a non-zero part 0.
// A GlobalScript rewriting these masks would only freeze them against a future DBC import.
//
// The talent is reachable in play: 707654 is entry id 11605 of
// /opt/coa/server/data/dbc/CharacterAdvancement.dbc, string "Frigid Elements". (It is absent
// from Talent.dbc, which is not the file Ascension advancement uses.)
//
// ---------------------------------------------------------------------------------------
// THE REAL DEFECT: FRIGID FUSION WAS NEVER IMPLEMENTED, AND ITS AURA DELETES ITSELF
//
// 803225 "Frigid Fusion" (Aura) is applied to the victim by every Frigid Blast (its E1 is
// SPELL_EFFECT_TRIGGER_SPELL -> 803225) and says:
//     "Accumulate $?s707654[30%][20%] of your damage dealt to the target over $d.
//      After the duration, deal it as Frost Damage to the target."
// with $d = 6 sec (DurationIndex 32 -> SpellDuration.dbc row 32 = 6000 ms). Its E0 is a DUMMY
// aura carrying TriggerSpell 572338 "Frigid Fusion" (Damage). Nothing implemented any of it:
// `grep -rn '803225\|572338' src/ modules/` over /opt/coa/core-main returned nothing, and
// acore_world.spell_script_names held no row for either. The debuff was applied, showed
// "Accumulating damage", and expired with no effect; 707654's E2 modified a number nobody read.
//
// (a) THE AURA CANNOT SURVIVE ITS OWN APPLICATION AS THE DBC SHIPS IT. 803225's E1 is an
//     aura 69 SPELL_AURA_SCHOOL_ABSORB, MiscValue 127 (every school), BasePoints 0 and
//     DieSides 1, i.e. an amount of exactly 1 (SpellInfo.cpp:463-500). Unit::CalcAbsorbResist
//     (Unit.cpp:2472, then 2513-2520) subtracts what it absorbs from that amount and calls
//     `absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL)` the moment it reaches 0 —
//     the WHOLE aura, accumulator effect included, on the first point of damage the victim
//     takes. And the aura is applied while the Frigid Blast that triggers it is still being
//     launched: SpellEffects.cpp:1100-1104 runs SPELL_EFFECT_TRIGGER_SPELL at
//     SPELL_EFFECT_HANDLE_LAUNCH_TARGET / _LAUNCH, before the caster's own damage is computed
//     and dealt, so Frigid Blast destroys the debuff it has just applied. 500118 sets neither
//     AscensionIgnoreAbsorb nor AscensionIgnoreAbsorbAndResistance, so there is no escape
//     hatch. Untouched, the accumulator below never finds an aura, the detonation never sees
//     AURA_REMOVE_BY_EXPIRE, and this whole file is dead code.
//     Disarmed in runemaster_frigid_metadata, on 803225 alone, in the same shape as
//     AscensionNecromancerContracts.cpp:28-32 for 552011: only if the effect still is the one
//     that was read, so that a DBC import which reshuffles it disarms the correction instead
//     of moving it onto something else (P-047).
// (b) accumulation and detonation themselves, implemented below.
//
// The accumulation percentage is taken from the data, not from the tooltip text:
//   * base: MiscValueB of 803225's E1. It reads 20, exactly the 20% the description prints.
//     STATED AS AN INFERENCE, not as a read contract: E1 is an absorb whose MiscValue 127 is
//     the school mask and whose own amount is 1, so MiscValueB carries no native meaning there
//     and 20 can only be the designer's payload. Disarming the absorb (a) does not move that
//     field. If the reading is ever shown wrong, the number to change is in the DBC, not here.
//   * talent: the amount of 707654's E2 (BasePoints 9 + DieSides 1 = 10), added as percentage
//     points. 20 -> 30 matches the $?s707654[30%][20%] switch exactly.
// No number in this file is invented; every one of them is read at runtime.
//
// WHAT IS BANKED, STATED RATHER THAN IMPLIED. Unit::DealDamage calls the OnDamage hook at
// Unit.cpp:1004, at the very top of the function and before any clamping against the victim's
// remaining health, so a killing blow banks its overkill too; and since the aura is applied at
// launch (see (a)), the Frigid Blast that opens or refreshes the window banks its own damage.
// Both follow "of your damage dealt to the target" literally and nothing in the data asks for
// either restriction, so neither is clamped here — but neither should be discovered by
// surprise when the talent is tuned.
//
// MEASURED, NOT ASSUMED — what a second Frigid Blast does to a bank in progress. Reapplying the
// aura goes through Unit::_TryStackingOrRefreshingExistingAura -> Aura::ModStackAmount ->
// Aura::SetStackAmount (SpellAuras.cpp:943), whose loop calls
// `m_effects[i]->ChangeAmount(m_effects[i]->CalculateAmount(caster), false, true)` on EVERY
// effect, WITHOUT consulting CanBeRecalculated(). The bank is therefore reset to 0 on every
// refresh, and AuraEffect::SetAmount's `m_canBeRecalculated = false` does not protect it. That
// is left as it stands: it is the reading "over $d" already implies — each cast restarts the
// six-second window — and it also removes any way for the pool to run away over a long fight.
//
// 572338 carries an already-resolved amount, so it gets the same metadata contract this module
// already applies to that kind of spell (see runemaster_secondary_metadata for 712298 and
// 807819). The DBC agrees: 572338 already carries SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS
// ("Deals fixed damage") on its own.
//
// LEFT ASIDE, NAMED SO IT IS NOT REDISCOVERED: Spell.dbc holds a second, parallel and equally
// unimplemented Frigid Fusion outside family 38 — 365058 "Frigid Blast" (Rank 9, family 0)
// triggers 365063 "Frigid Fusion" (Aura, family 0, same DurationIndex 32, same E1 absorb with
// MiscValue 127 / MiscValueB 20), which triggers 365066 "Frigid Fusion" (Damage). Nothing here
// covers them: the SQL hooks 803225 only, and Validate() requires family 38. Nothing indicates
// 365058 is reachable in play; if a survey ever shows it is, 365063 needs its own hook and the
// family guard below has to be relaxed.

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
enum RunemasterFrigidSpells : uint32
{
    SPELL_FRIGID_ELEMENTS = 707654,
    SPELL_FRIGID_FUSION_AURA = 803225,
    SPELL_FRIGID_FUSION_DAMAGE = 572338
};

// Spell family of the Runemaster: class id + 6, as AscensionRunemasterBrand.cpp spells it out.
constexpr uint32 SPELL_FAMILY_RUNEMASTER = uint32(CLASS_SPIRIT_MAGE) + 6;

// Percentage points of damage Frigid Fusion banks, read from the DBC on every event so the
// tooltip and the code can never drift apart.
int32 FrigidFusionPercent(Player const* player, SpellInfo const* fusion)
{
    int32 percent = fusion->Effects[EFFECT_1].MiscValueB;
    if (AuraEffect const* talent = player->GetAuraEffect(SPELL_FRIGID_ELEMENTS, EFFECT_2))
        percent += talent->GetAmount();
    return std::clamp(percent, 0, 100);
}

class runemaster_frigid_metadata : public GlobalScript
{
public:
    runemaster_frigid_metadata() : GlobalScript("runemaster_frigid_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != SPELL_FAMILY_RUNEMASTER)
            return;

        if (info->Id == SPELL_FRIGID_FUSION_AURA)
        {
            // See (a) in the header: a one-point all-school absorb that takes the whole aura
            // down with it on the first damage the victim receives. Neutralised only while the
            // effect still is the one that was read.
            SpellEffectInfo& absorb = info->Effects[EFFECT_1];
            if (absorb.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB &&
                absorb.MiscValue == int32(SPELL_SCHOOL_MASK_ALL))
                absorb.ApplyAuraName = SPELL_AURA_DUMMY;
            return;
        }

        if (info->Id == SPELL_FRIGID_FUSION_DAMAGE)
        {
            // Carries damage already dealt once: no second helping of the caster's modifiers,
            // no crit on a sum that could already contain crits, no coefficient.
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
    }
};

// Banks the caster's damage into his own Frigid Fusion on that victim.
class runemaster_frigid_fusion_accumulator : public UnitScript
{
public:
    runemaster_frigid_fusion_accumulator() : UnitScript("runemaster_frigid_fusion_accumulator", true,
        {UNITHOOK_ON_DAMAGE}) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        // Unit::DealDamage calls this for every damage event in the world, with some seven
        // hundred characters in it: the class test comes before anything that touches a map.
        // "your damage dealt" is read strictly: the attacker must be the Runemaster himself, so
        // a Rift Clone's or any guardian's damage is not banked.
        if (!attacker || !victim || attacker == victim || !damage || !attacker->IsPlayer() ||
            attacker->getClass() != CLASS_SPIRIT_MAGE)
            return;

        Player* player = attacker->ToPlayer();
        // No re-entrancy guard is needed for the detonation below: Unit::_UnapplyAura erases
        // the application from m_appliedAuras (Unit.cpp:5134) BEFORE running the removal
        // handlers (Unit.cpp:5171), and Unit::GetAuraEffect (Unit.cpp:6091) reads nothing else,
        // so the detonation's own damage cannot find the aura that is detonating.
        AuraEffect* accumulator = victim->GetAuraEffect(SPELL_FRIGID_FUSION_AURA, EFFECT_0,
                                                        player->GetGUID());
        if (!accumulator)
            return;

        int32 const percent = FrigidFusionPercent(player, accumulator->GetSpellInfo());
        if (percent <= 0)
            return;

        // int64 throughout: six seconds of banked damage has no written cap, and the amount the
        // aura carries is an int32.
        int64 const banked = int64(accumulator->GetAmount()) + int64(damage) * int64(percent) / 100;
        accumulator->SetAmount(int32(std::min<int64>(banked, std::numeric_limits<int32>::max())));
    }
};

// "After the duration, deal it as Frost Damage to the target."
class aura_ascension_runemaster_frigid_fusion : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_frigid_fusion);

    bool Validate(SpellInfo const* info) override
    {
        return info && info->SpellFamilyName == SPELL_FAMILY_RUNEMASTER &&
            info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_DUMMY &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_FRIGID_FUSION_DAMAGE &&
            ValidateSpellInfo({SPELL_FRIGID_FUSION_DAMAGE});
    }

    void Detonate(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        // "After the duration": a dispel, a death or an overwrite by the next Frigid Blast is
        // not the end of the duration and pays nothing. Unit::_UnapplyAura sets the remove mode
        // (Unit.cpp:5122) before calling this, so the test is answerable here.
        AuraApplication const* application = GetTargetApplication();
        if (!application || application->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        int32 const banked = effect->GetAmount();
        if (banked <= 0)
            return;

        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        // 803225 carries SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD, so it does reach its expiry on a
        // corpse, and the caster may have died, zoned or logged out in those six seconds.
        // Aura::GetCaster resolves through ObjectAccessor::GetUnit(*GetOwner(), guid), which
        // already searches the victim's map only; IsInMap is kept anyway so that no future
        // change to that lookup can hand this a cross-map caster.
        if (!caster || !caster->IsInWorld() || !caster->IsAlive() || !caster->IsInMap(target) ||
            !target->IsAlive())
            return;

        caster->CastCustomSpell(SPELL_FRIGID_FUSION_DAMAGE, SPELLVALUE_BASE_POINT0, banked, target,
                                TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_runemaster_frigid_fusion::Detonate,
                                                EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};
}

void AddSC_AscensionRunemasterFrigid()
{
    new runemaster_frigid_metadata();
    new runemaster_frigid_fusion_accumulator();
    RegisterSpellScript(aura_ascension_runemaster_frigid_fusion);
}
