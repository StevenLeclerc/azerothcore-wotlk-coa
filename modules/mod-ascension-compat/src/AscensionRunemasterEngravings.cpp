/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

// Weapon Engravings: the riders the core cannot run on its own.
//
// The proc entries themselves are data (2026_09_20_04_ascension_gravures_arme.sql); the
// payloads below are what no table can express. Fire lives in AscensionRunemasterSecondary.cpp
// with Firebrand and stays there.
//
//   Air     653225 carries Ascension's private aura 354, which has no core handler at all
//           (AuraEffectHandler[354] is nullptr): without a script the proc entry fires into
//           the void. It replicates a share of the damage dealt, ignoring armour.
//   Ice     653266 triggers its Frost damage natively; "Each trigger can apply one stack of
//           Icebound Momentum" and the stacks' own damage bonus do not.
//   Water   653261 drains a flat 2 points because its base value is the tooltip's PERCENT
//           ("up to $s1% of your maximum mana"), not an amount.
//   Arcane  653263 marks the target; storing the healing it receives and paying it back at
//           expiry needs a heal hook.

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
enum RunemasterEngravingSpells : uint32
{
    SPELL_AIR_ENGRAVING_PROC = 653225,
    SPELL_AIR_REPLICATION = 653226,
    SPELL_ICE_ENGRAVING = 653266,
    SPELL_ICEBOUND_MOMENTUM = 712490,
    SPELL_ICEBOUND_MOMENTUM_HIT = 712491,
    SPELL_ICEBOUND_MOMENTUM_ICD = 712495,
    SPELL_WATER_DRAIN = 653261,
    SPELL_ARCANE_MARK = 653263,
    SPELL_ARCANE_STORED_HEALING = 712493,
    SPELL_FIREBRAND_APPLY = 653210,
    SPELL_EARTH_PAYLOAD = 653272,
    SPELL_WATER_PAYLOAD = 653261,
    SPELL_ICE_PAYLOAD = 653217,
    SPELL_AIR_ENGRAVING_PASSIVE = 653223,
    SPELL_CONVERGENCE = 801086,
    SPELL_CONVERGENCE_HIT = 560241,
    SPELL_EXPANSIVE_ENGRAVER = 807496
};

// The six payloads a Weapon Engraving fires. "Weapon Engravings you trigger" (Convergence)
// has no other common point: four of the six are cast natively by the core's aura 42 handler
// and never pass through a script of ours.
bool IsEngravingPayload(uint32 id)
{
    return id == SPELL_FIREBRAND_APPLY || id == SPELL_ICE_PAYLOAD || id == SPELL_WATER_PAYLOAD ||
        id == SPELL_EARTH_PAYLOAD || id == SPELL_AIR_REPLICATION || id == SPELL_ARCANE_MARK;
}

// Shared by the Air and Ice engraving handlers: the tooltips say "direct damage", which is
// what the proc flags of their `spell_proc` rows already select (DONE_PERIODIC is absent);
// the damage-type test is the same belt-and-braces the Fire handler keeps.
bool IsOwnDirectDamage(Unit* player, ProcEventInfo& event)
{
    DamageInfo const* damage = event.GetDamageInfo();
    Unit* victim = event.GetActionTarget();
    return player && player->IsPlayer() && player->getClass() == CLASS_SPIRIT_MAGE && player->IsAlive() &&
        event.GetActor() == player && victim && victim != player && victim->IsAlive() &&
        !player->IsFriendlyTo(victim) && damage && damage->GetDamage() && damage->GetDamageType() != DOT;
}

int32 ShareOfDamage(uint32 damage, int32 percent)
{
    uint64 amount = uint64(damage) * uint64(std::clamp(percent, 0, 100)) / 100;
    return int32(std::min<uint64>(amount, std::numeric_limits<int32>::max()));
}

// Air Engraving: "Your direct damage has a 15% chance to replicate 30% of the damage dealt.
// Replicated damage ignores armor." The share is effect 0's own amount.
class aura_ascension_runemaster_air_engraving : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_air_engraving);

    bool Check(ProcEventInfo& event)
    {
        return GetCaster() == GetTarget() && IsOwnDirectDamage(GetTarget(), event);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        int32 const amount = ShareOfDamage(event.GetDamageInfo()->GetDamage(), effect->GetAmount());
        if (!amount)
            return;
        GetTarget()->CastCustomSpell(SPELL_AIR_REPLICATION, SPELLVALUE_BASE_POINT0, amount,
            event.GetActionTarget(), TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_air_engraving::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_runemaster_air_engraving::Proc, EFFECT_0,
                                         AuraType(354));
    }
};

// Ice Engraving: the Frost damage is the native trigger of effect 0, so this handler must NOT
// prevent the default action — it only adds what the tooltip promises on top: "Each trigger can
// apply one stack of Icebound Momentum", rate-limited by the helper the data already provides.
class aura_ascension_runemaster_ice_engraving : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_ice_engraving);

    bool Check(ProcEventInfo& event)
    {
        return GetCaster() == GetTarget() && IsOwnDirectDamage(GetTarget(), event);
    }

    void Momentum(AuraEffect const* /*effect*/, ProcEventInfo& /*event*/)
    {
        Unit* player = GetTarget();
        if (player->HasAura(SPELL_ICEBOUND_MOMENTUM_ICD, player->GetGUID()))
            return;
        player->CastSpell(player, SPELL_ICEBOUND_MOMENTUM, true);
        player->CastSpell(player, SPELL_ICEBOUND_MOMENTUM_ICD, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_ice_engraving::Check);
        AfterEffectProc += AuraEffectProcFn(aura_ascension_runemaster_ice_engraving::Momentum, EFFECT_0,
                                            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Water Engraving payload: "drain up to $s1% of your maximum mana from the target, restoring the
// amount drained". Effect 0 is a POWER_DRAIN whose base value is the PERCENT, so as authored it
// drains two points of mana. The gain multiplier is already 1.0 in the DBC, so the restoration
// side needs nothing: only the amount is wrong.
class spell_ascension_runemaster_water_engraving : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_water_engraving);

    bool Load() override
    {
        return GetCaster() && GetCaster()->IsPlayer() && GetCaster()->getClass() == CLASS_SPIRIT_MAGE;
    }

    void Scale()
    {
        Unit* caster = GetCaster();
        int32 const percent = std::clamp(GetSpellInfo()->Effects[EFFECT_0].CalcValue(caster), 0, 100);
        uint64 const pool = uint64(caster->GetMaxPower(POWER_MANA)) * uint64(percent) / 100;
        GetSpell()->SetSpellValue(SPELLVALUE_BASE_POINT0,
            int32(std::min<uint64>(pool, std::numeric_limits<int32>::max())));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_runemaster_water_engraving::Scale);
    }
};

// Arcane Engraving mark: "The mark stores effective healing received. When it expires naturally,
// it deals Arcane damage equal to $s2% of the stored healing." The running total lives on the
// aura itself, so it dies with the mark and cannot leak between two applications.
class aura_ascension_runemaster_arcane_mark : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_arcane_mark);

    void Payout(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || !caster || !target->IsAlive())
            return;
        uint64 const stored = GetAura()->GetScriptValue(SPELL_ARCANE_MARK);
        if (!stored)
            return;
        uint64 const payout = stored * uint64(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        if (!payout)
            return;
        caster->CastCustomSpell(SPELL_ARCANE_STORED_HEALING, SPELLVALUE_BASE_POINT0,
            int32(std::min<uint64>(payout, std::numeric_limits<int32>::max())), target, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_runemaster_arcane_mark::Payout, EFFECT_1,
                                                SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class runemaster_engraving_events : public UnitScript
{
public:
    runemaster_engraving_events() : UnitScript("runemaster_engraving_events", true, {UNITHOOK_ON_HEAL}) { }

    // Feeds the Arcane mark. `gain` is the heal about to be applied, so the part that would
    // overheal is subtracted here: the tooltip says EFFECTIVE healing received.
    void OnHeal(Unit* /*healer*/, Unit* reciever, uint32& gain) override
    {
        if (!reciever || !gain || !reciever->IsAlive())
            return;
        Aura* mark = reciever->GetAura(SPELL_ARCANE_MARK);
        if (!mark)
            return;
        uint64 const missing = reciever->GetMaxHealth() > reciever->GetHealth()
            ? reciever->GetMaxHealth() - reciever->GetHealth() : 0;
        uint64 const effective = std::min<uint64>(gain, missing);
        if (effective)
            mark->SetScriptValue(SPELL_ARCANE_MARK, mark->GetScriptValue(SPELL_ARCANE_MARK) + effective);
    }
};

class runemaster_engraving_momentum : public AllSpellScript
{
public:
    runemaster_engraving_momentum() : AllSpellScript("runemaster_engraving_momentum",
        {ALLSPELLHOOK_ON_HIT_RESULT, ALLSPELLHOOK_ON_CAST}) { }

    // Convergence (801086): "For 15 s, the next 10 Weapon Engravings you trigger will now deal
    // an additional <damage> Elemental Damage." The count lives on the aura, so it dies with it.
    // The amount follows the tooltip's own formula: the helper's value + SP*0.3 + AP*0.2.
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !IsEngravingPayload(info->Id))
            return;
        Aura* convergence = player->GetAura(SPELL_CONVERGENCE, player->GetGUID());
        Unit* target = spell->m_targets.GetUnitTarget();
        if (!convergence || !target || target == player || player->IsFriendlyTo(target))
            return;
        SpellInfo const* helper = sSpellMgr->GetSpellInfo(SPELL_CONVERGENCE_HIT);
        if (!helper)
            return;
        int64 const amount = int64(helper->Effects[EFFECT_0].CalcValue(player)) +
            int64(std::max(0, player->SpellBaseDamageBonusDone(helper->GetSchoolMask())) * 0.3f) +
            int64(std::max(0.0f, player->GetTotalAttackPowerValue(BASE_ATTACK)) * 0.2f);
        if (amount > 0)
            player->CastCustomSpell(SPELL_CONVERGENCE_HIT, SPELLVALUE_BASE_POINT0,
                int32(std::min<int64>(amount, std::numeric_limits<int32>::max())), target,
                TRIGGERED_FULL_MASK);
        uint64 const spent = convergence->GetScriptValue(SPELL_CONVERGENCE) + 1;
        if (spent >= 10)
            convergence->Remove();
        else
            convergence->SetScriptValue(SPELL_CONVERGENCE, spent);
    }

    // Icebound Momentum: "The Runemaster deals $s1% additional damage per stack as Frost damage."
    // Triggered spells are skipped, which both matches "the Runemaster deals" and keeps the Frost
    // helper below from feeding itself.
    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool) override
    {
        Player* player = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !player->IsAlive() || !player->IsInWorld() ||
            spell->IsTriggered() || !damage || miss != SPELL_MISS_NONE || !target || !target->IsAlive() ||
            target == player || player->IsFriendlyTo(target))
            return;
        Aura* momentum = player->GetAura(SPELL_ICEBOUND_MOMENTUM, player->GetGUID());
        if (!momentum)
            return;
        SpellInfo const* reference = sSpellMgr->GetSpellInfo(SPELL_ICEBOUND_MOMENTUM);
        if (!reference)
            return;
        int32 const percent = reference->Effects[EFFECT_0].CalcValue(player) * momentum->GetStackAmount();
        int32 const amount = ShareOfDamage(damage, percent);
        if (!amount)
            return;
        player->CastCustomSpell(SPELL_ICEBOUND_MOMENTUM_HIT, SPELLVALUE_BASE_POINT0, amount, target,
            TRIGGERED_FULL_MASK);
    }
};

// Expansive Engraver (807496), quart « Earth »: "While Air Engraving is active, Earth Engraving
// deals 10% increased damage to the first target hit, plus an additional 10% for each subsequent
// target." The two percentages are literal in the tooltip, not $s variables.
class spell_ascension_runemaster_earth_engraving : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_earth_engraving);

    uint8 _hit = 0;

    bool Load() override
    {
        Unit* caster = GetCaster();
        return caster && caster->IsPlayer() && caster->getClass() == CLASS_SPIRIT_MAGE &&
            caster->HasAura(SPELL_EXPANSIVE_ENGRAVER) && caster->HasAura(SPELL_AIR_ENGRAVING_PASSIVE);
    }

    void Escalate()
    {
        // One SpellScript instance per cast, so the counter cannot leak between two casts.
        ++_hit;
        SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), 10 * _hit));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_runemaster_earth_engraving::Escalate);
    }
};

class runemaster_engraving_metadata : public GlobalScript
{
public:
    runemaster_engraving_metadata() : GlobalScript("runemaster_engraving_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    // The engraving spells carry SpellFamilyName 0, so this pass cannot use the family guard the
    // rest of the Runemaster metadata uses: it names its four ids instead.
    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->Id == SPELL_AIR_REPLICATION || info->Id == SPELL_ICEBOUND_MOMENTUM_HIT)
        {
            // Both carry a share of damage that has already been resolved once: it must not be
            // scaled or reduced a second time. Same treatment as the Fists of Power hit and the
            // Arcane Sigil DoT (AscensionRunemasterSecondary.cpp).
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        // Icebound Momentum's payload is NOT barred from critting: Expansive Engraver grants it
        // "30 percentage points of critical strike chance", so the design expects it to crit.
        // The Air replication has no such clause and keeps the bar.
        if (info->Id == SPELL_AIR_REPLICATION)
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        if (info->Id == SPELL_AIR_REPLICATION)
            // "Replicated damage ignores armor" — and its school mask includes Normal (9),
            // so without this the armour reduction would apply.
            info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
        if (info->Id == SPELL_ARCANE_STORED_HEALING)
        {
            // A share of stored HEALING, not of resolved damage: spell power must not inflate it
            // and it must not crit, but it takes damage reduction like any other Arcane hit.
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        if (info->Id == SPELL_ICEBOUND_MOMENTUM_ICD || info->Id == SPELL_ICEBOUND_MOMENTUM)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
    }
};
}

void AddSC_AscensionRunemasterEngravings()
{
    new runemaster_engraving_events();
    new runemaster_engraving_momentum();
    new runemaster_engraving_metadata();
    RegisterSpellScript(aura_ascension_runemaster_air_engraving);
    RegisterSpellScript(aura_ascension_runemaster_ice_engraving);
    RegisterSpellScript(spell_ascension_runemaster_water_engraving);
    RegisterSpellScript(aura_ascension_runemaster_arcane_mark);
    RegisterSpellScript(spell_ascension_runemaster_earth_engraving);
}
