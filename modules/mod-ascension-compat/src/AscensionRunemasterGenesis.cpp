/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

// Genesis (500501), Riftblade tree: "Apply a runic brand to an enemy for $d, accumulating 50% of
// your damage dealt, and unleashing after $d as Elemental Damage. When this deals damage, it
// triggers your Weapon Engraving."
//
// What the DBC actually authors, read on 2026-09-20 from /opt/coa/server/data/dbc/Spell.dbc:
//   500501  family 0 (NOT 38 — see the warning below), DurationIndex 31 = 8000 ms, ProcFlags 0,
//           no `spell_proc` row.
//           Effect 0  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 500502
//           Effect 1  APPLY_AURA / aura 69,      MiscValue 127, MiscValueB 50, amount 1
//           Effect 2  APPLY_AURA / aura 4 DUMMY
//   500502  family 0, "Genesis / Damage", SchoolMask 28 (Fire|Nature|Frost = "Elemental"),
//           one SPELL_EFFECT_SCHOOL_DAMAGE of base value 1, i.e. a carrier for a computed amount.
//
// BOTH RECORDS CARRY SpellFamilyName 0, not 38. It matters twice, and silently: a `Validate` that
// demanded family 38 would refuse to bind this script at all, and an OnLoadSpellCustomAttr pass
// that opened with `if (info->SpellFamilyName != 38) return;` — the guard the rest of the
// Runemaster metadata uses — would never reach the absorb disarm below. Neither failure logs
// anything. This file therefore keys on ids only.
//
// THE TRAP IN EFFECT 1. Aura 69 is NOT an unknown Ascension aura: it is the stock
// SPELL_AURA_SCHOOL_ABSORB (SpellAuraDefines.h:132), and its table entry is a real handler
// (SpellAuraEffects.cpp:134, "implemented in Unit::CalcAbsorbResist"). Left as authored, the mark
// would put a ONE point, all-school (MiscValue 127) shield on the marked enemy, and Unit.cpp:2513
// removes the whole aura the moment an absorb amount reaches zero:
//     if (absorbAurEff->GetAmount() <= 0) absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
// So the very first point of damage dealt to the target would delete Genesis, with a removal mode
// that is not AURA_REMOVE_BY_EXPIRE — the brand would never pay out, and would never last 8 s.
// The metadata pass below turns that effect into a DUMMY so it can no longer absorb anything; its
// MiscValueB, which is the 50% the tooltip quotes, stays readable.
//
// MiscValueB is the only "50" the record carries, and the tooltip's only percentage. That pairing
// is the reading used here; nothing else in the data states the accumulation rate.

#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
enum RunemasterGenesisSpells : uint32
{
    SPELL_GENESIS = 500501,
    SPELL_GENESIS_DAMAGE = 500502,
    // The six Weapon Engraving chains, as 2026_09_20_04_ascension_gravures_arme.sql maps them end
    // to end: the passive the weapon enchant applies, then the payload that passive fires.
    // Duplicated here on purpose: AscensionRunemasterEngravings.cpp owns them, and is being worked
    // on in another task, so this file does not include or touch it.
    SPELL_FIRE_ENGRAVING = 653211,
    SPELL_FIREBRAND_APPLY = 653210,
    SPELL_ICE_ENGRAVING = 653266,
    SPELL_ICE_PAYLOAD = 653217,
    SPELL_EARTH_ENGRAVING = 653219,
    SPELL_EARTH_PAYLOAD = 653272,
    SPELL_WATER_ENGRAVING = 653214,
    SPELL_WATER_PAYLOAD = 653261,
    SPELL_ARCANE_ENGRAVING = 653267,
    SPELL_ARCANE_MARK = 653263,
    SPELL_AIR_ENGRAVING = 653223,
    SPELL_AIR_ENGRAVING_PROC = 653225,
    SPELL_AIR_REPLICATION = 653226
};

// Script value keys on the mark: the running total, and the guard that keeps the unleash from
// feeding the very total it is spending.
constexpr uint32 KEY_ACCUMULATED = SPELL_GENESIS;
constexpr uint32 KEY_UNLEASHING = SPELL_GENESIS_DAMAGE;

// "When this deals damage, it triggers your Weapon Engraving." Guaranteed, the way Water Runes
// forces one on every Hurricane strike (AscensionRunemasterHurricane.cpp, WaterRunes). Every
// engraving the Runemaster currently carries fires: a dual wielder can hold two different ones,
// and the tooltip gives no rule for picking between them.
//
// Casting the payloads is also what makes Convergence (801086) count these triggers: its counter
// watches ALLSPELLHOOK_ON_CAST for the six payload ids, and Spell::cast raises that hook for
// triggered casts too (Spell.cpp:4129).
void TriggerWeaponEngravings(Unit* caster, Unit* target, uint32 dealt)
{
    if (caster->HasAura(SPELL_FIRE_ENGRAVING))
    {
        // Firebrand's own rule, quoted by the Fire engraving handler: "Additional applications do
        // not refresh its duration." A plain cast would refresh it, so stack and restore instead.
        if (Aura* brand = target->GetAura(SPELL_FIREBRAND_APPLY, caster->GetGUID()))
        {
            int32 const duration = brand->GetDuration();
            brand->ModStackAmount(1);
            brand->SetDuration(duration);
        }
        else
            caster->CastSpell(target, SPELL_FIREBRAND_APPLY, true);
    }
    if (caster->HasAura(SPELL_ICE_ENGRAVING))
        caster->CastSpell(target, SPELL_ICE_PAYLOAD, true);
    if (caster->HasAura(SPELL_EARTH_ENGRAVING))
        caster->CastSpell(target, SPELL_EARTH_PAYLOAD, true);
    if (caster->HasAura(SPELL_WATER_ENGRAVING))
        caster->CastSpell(target, SPELL_WATER_PAYLOAD, true);
    if (caster->HasAura(SPELL_ARCANE_ENGRAVING))
        caster->CastSpell(target, SPELL_ARCANE_MARK, true);
    if (caster->HasAura(SPELL_AIR_ENGRAVING))
    {
        // Air is the one payload that carries no amount of its own: it replicates a share of the
        // damage that triggered it. The share is effect 0 of 653225, the Passive2 that holds the
        // proc (29 + 1 = 30 in the DBC today) — read, never hard-coded.
        SpellInfo const* air = sSpellMgr->GetSpellInfo(SPELL_AIR_ENGRAVING_PROC);
        int32 const share = air ? std::clamp(air->Effects[EFFECT_0].CalcValue(caster), 0, 100) : 0;
        uint64 const replicated = uint64(dealt) * uint64(share) / 100;
        if (replicated)
            caster->CastCustomSpell(SPELL_AIR_REPLICATION, SPELLVALUE_BASE_POINT0,
                int32(std::min<uint64>(replicated, std::numeric_limits<int32>::max())), target,
                TRIGGERED_FULL_MASK);
    }
}

// The accumulation. UNITHOOK_ON_DAMAGE rather than an AllSpellScript on ALLSPELLHOOK_ON_HIT_RESULT
// for two reasons: the tooltip says "your damage dealt" without qualifying it, and Unit::DealDamage
// (Unit.cpp:1004) is the single point every kind of damage passes through — melee swings, spell
// hits, periodic ticks — with the amount already mitigated, which is the figure a player reads in
// their combat log. ON_HIT_RESULT would see spell hits only, and would miss auto attacks entirely,
// which is most of a Riftblade's output.
//
// This hook runs for every damage event in the realm, so the filter is ordered cheapest first: an
// integer test, three pointer tests, a type-id test and a byte compare, all before the only lookup.
class runemaster_genesis_events : public UnitScript
{
public:
    runemaster_genesis_events() : UnitScript("runemaster_genesis_events", true, {UNITHOOK_ON_DAMAGE}) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !attacker || !victim || attacker == victim || !attacker->IsPlayer() ||
            attacker->getClass() != CLASS_SPIRIT_MAGE)
            return;
        // Caster-scoped: two Runemasters branding the same enemy each feed their own mark, and a
        // Runemaster never feeds someone else's.
        Aura* mark = victim->GetAura(SPELL_GENESIS, attacker->GetGUID());
        if (!mark || mark->GetScriptValue(KEY_UNLEASHING))
            return;
        mark->SetScriptValue(KEY_ACCUMULATED, mark->GetScriptValue(KEY_ACCUMULATED) + damage);
    }
};

// The mark. The running total lives on the aura instance, so it dies with the mark, cannot leak
// between two applications and cannot be shared between two targets.
class aura_ascension_runemaster_genesis : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_genesis);

    bool Validate(SpellInfo const* info) override
    {
        // No SpellFamilyName test: this record carries family 0, and demanding 38 here would
        // silently refuse the binding.
        return info && info->Id == SPELL_GENESIS &&
            info->Effects[EFFECT_0].IsAura(SPELL_AURA_DUMMY) &&
            info->Effects[EFFECT_0].TriggerSpell == SPELL_GENESIS_DAMAGE &&
            ValidateSpellInfo({SPELL_GENESIS_DAMAGE});
    }

    // Natural expiry only: a dispel, a death or an early removal pays nothing. Same test as
    // Firebrand (AscensionRunemasterSecondary.cpp) and the Arcane mark.
    void Unleash(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || !caster || !target->IsAlive())
            return;
        uint64 const stored = GetAura()->GetScriptValue(KEY_ACCUMULATED);
        if (!stored)
            return;
        // The rate is effect 1's MiscValueB — the 50 the tooltip quotes. Accumulating the raw
        // damage and taking the share once loses less than taking 50% of every single hit.
        int32 const percent = std::clamp(GetSpellInfo()->Effects[EFFECT_1].MiscValueB, 0, 100);
        uint64 const payout = stored * uint64(percent) / 100;
        if (!payout)
            return;
        // Re-entrancy guard, set and never cleared: this handler only runs on the removal that
        // destroys the aura, and both the unleash and the engraving it triggers deal damage to
        // this very target while the mark may still be in the target's applied-aura map.
        GetAura()->SetScriptValue(KEY_UNLEASHING, 1);
        caster->CastCustomSpell(SPELL_GENESIS_DAMAGE, SPELLVALUE_BASE_POINT0,
            int32(std::min<uint64>(payout, std::numeric_limits<int32>::max())), target, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_runemaster_genesis::Unleash, EFFECT_0,
                                                SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The unleash itself. The engraving is triggered from here, not from the aura, because "when this
// deals damage" is exactly this hit: OnHit is the only place that knows what the brand really dealt,
// which is what the Air replication needs a share of.
class spell_ascension_runemaster_genesis_damage : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_genesis_damage);

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->getClass() == CLASS_SPIRIT_MAGE;
    }

    void Engrave()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const dealt = GetHitDamage();
        if (dealt <= 0 || !target || target == caster || !target->IsAlive() || caster->IsFriendlyTo(target))
            return;
        TriggerWeaponEngravings(caster, target, uint32(dealt));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_runemaster_genesis_damage::Engrave);
    }
};

class runemaster_genesis_metadata : public GlobalScript
{
public:
    runemaster_genesis_metadata() : GlobalScript("runemaster_genesis_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    // Keyed on ids, NOT on SpellFamilyName: both Genesis records carry family 0, so the usual
    // `if (info->SpellFamilyName != 38) return;` guard of the Runemaster metadata passes would
    // skip them entirely and leave the absorb effect armed.
    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->Id == SPELL_GENESIS)
        {
            // Disarm the one-point all-school shield described at the top of this file. Same
            // treatment the module already gives to effects the core would read differently from
            // Ascension (AscensionPrimalistSecondary.cpp:209, AscensionGuardianCompletion.cpp:259).
            // Guarded, so a future DBC import that authors something else here is left alone.
            if (info->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
                info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
            // The running total is in memory only. A mark restored from `character_aura` after a
            // restart would expire with nothing to pay, so it must not be saved at all.
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
        if (info->Id == SPELL_GENESIS_DAMAGE)
        {
            // A share of damage that has already been resolved once against this same target: it
            // must not be reduced by that target's resilience a second time. The rest of the
            // treatment the module gives such payloads is already authored in the DBC and was read
            // there: AttributesEx2 carries SPELL_ATTR2_CANT_CRIT, AttributesEx3 carries
            // SPELL_ATTR3_IGNORE_CASTER_MODIFIERS and SPELL_ATTR3_ALWAYS_HIT, AttributesEx4 carries
            // SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS.
            info->AscensionInheritsResolvedAmount = true;
            // The stock 3.3.5a coefficient column holds 0.3 for this record; the unleashed amount is
            // the accumulated total and nothing else. `ascension_stock_coefficients` already clears
            // it from its generated list, which 500502 is in; repeated here so the contract is local.
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
    }
};
}

void AddSC_AscensionRunemasterGenesis()
{
    new runemaster_genesis_events();
    new runemaster_genesis_metadata();
    RegisterSpellScript(aura_ascension_runemaster_genesis);
    RegisterSpellScript(spell_ascension_runemaster_genesis_damage);
}
