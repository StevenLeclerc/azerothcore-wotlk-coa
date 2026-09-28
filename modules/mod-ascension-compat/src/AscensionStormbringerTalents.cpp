/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionCustomResourceData.h"
#include "AscensionSpellSafe.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <vector>

namespace
{
enum StormbringerTalentSpells : uint32
{
    SPELL_CLOUDBURST = 801838,
    SPELL_CLOUDBURST_KNOCKBACK = 802385,
    SPELL_SHOCK = 804020,
    SPELL_SHOCK_DOT = 560336,
    SPELL_PERPETUAL_SHOCK = 570054,
    SPELL_CALL_LIGHTNING = 500040,
    SPELL_THUNDER_WARD = 800098,
    SPELL_STATIC = 803102,
    SPELL_GENERATE_STATIC_20 = 804086,
    SPELL_BAROMETRIC_PRESSURE = 803563,
    SPELL_BAROMETRIC_SLOW = 803566,
    SPELL_CRUSHING_PRESSURE = 803564,
    SPELL_CRUSHING_PRESSURE_AREA = 803567,
    SPELL_ELECTRICAL_CHARGE = 800299,
    SPELL_CHARGED_CONDUIT = 803790,

    // Rank-one anchors taken from `spell_ranks` (read 2026-09-28).
    SPELL_ELECTROCUTE = 801844,
    SPELL_ARM_OF_THORIM = 801847,
    SPELL_FORKED_LIGHTNING = 801851,
    SPELL_STORMFLOW = 567555,
    SPELL_DELUGE = 806400,
    SPELL_VOLT = 500928,

    // Talents repaired in this file, all SPELL_ATTR0_PASSIVE in Spell.dbc.
    SPELL_CONDUCTIVE_PASSIVE = 92098,
    SPELL_STORM_SYNERGY = 578300,
    SPELL_SHOCKINGLY_POWERFUL = 572310,
    SPELL_LIGHTNING_ROD = 300609,
    SPELL_WRATH_OF_ALAKIR = 300834,
    SPELL_HURRICANES = 300839,
    SPELL_THORIMS_GIFT = 570173,
    SPELL_LORD_OF_LIGHTNING = 707618,
    SPELL_STORM_CLOUDS = 705686,
    SPELL_AMPED_FLOW = 806411,
    SPELL_CRITICAL_CIRCUIT = 807314,
    SPELL_NEVER_STRIKES_TWICE = 804828,
    SPELL_TEMPEST_CALLING = 707615,
    SPELL_FLUX_ARC = 705643,
    SPELL_PULSE_CONVERSION = 707619,
    SPELL_SUPERCONDUCTOR = 705646,

    // Helpers those talents point at, each record read in Spell.dbc.
    SPELL_CONDUCTIVE = 567559,           // buff of the Stormbringer (tA1), StackAmount 10, e0 dummy 50 = % per stack
    SPELL_CONDUCTION = 567560,           // Frost damage, "increased by $567559s1% for each stack"
    SPELL_WRATH_OF_ALAKIR_EXTEND = 300835, // two effects 177 on Unshackle 706625, +3000 ms
    SPELL_UNSHACKLE = 706625,
    SPELL_HURRICANES_BUFF = 570129,
    SPELL_THORIMS_GIFT_AREA = 570174,    // persistent area, periodic damage every 1 s for 3 s
    SPELL_STORM_ASCENDANCE = 681110,
    SPELL_STORM_ASCENDANCE_FORM = 681187,
    SPELL_LORD_OF_LIGHTNING_BUFF = 801841,
    SPELL_BODY_OF_LIGHTNING = 500041,
    SPELL_AMPED_FLOW_BUFF = 567556,
    SPELL_ADD_10_STATIC = 804084,
    SPELL_AEROBLAST_ECHO = 802750,       // one 1 s periodic tick
    SPELL_FLUX_ARC_MARK = 804832,
    SPELL_FLUX_ARC_DAMAGE = 705644,
    SPELL_PULSE_HEAL = 504830,
    SPELL_STORMBREAKER = 705669,
    SPELL_STATIC_HIDDEN = 707038,        // Arm of Thorim growth per Static stack
    SPELL_DARK_SKIES_BUFF = 680855,
    SPELL_THUNDER_WAVE_BUFF = 712353,
    SPELL_THUNDER_WAVE = 707220,
    SPELL_EYE_OF_THE_STORM_BURST = 806455,
    SPELL_KISS_OF_THE_CLOUDS_SHIELD = 500292,
    SPELL_STORMCLOAK = 804833,
    SPELL_BLESSING_OF_LEI_SHEN = 561228,
    SPELL_BLESSING_OF_LEI_SHEN_HEAL = 561308,
    SPELL_WATERFALL = 806412,
    SPELL_WATERFALL_REDUCTION = 807661,  // three effects 165 at -500 ms
    SPELL_SURGE_OF_MIGHT_AURA = 520084,
    SPELL_RIDE_THE_LIGHTNING = 800099,

    // Records rewritten by contract below.
    SPELL_SHOCKING_AEGIS = 680316,
    SPELL_FLOW_OF_WRATH = 801855,
    SPELL_INVOKING_STORMS_1 = 705667,
    SPELL_INVOKING_STORMS_2 = 707793,
    SPELL_UNBOUND_ELEMENTALIST_1 = 705702,
    SPELL_AETHERMANCY_1 = 705690,
    SPELL_AETHERMANCY_2 = 705691,
    SPELL_IONIC_AURA = 707652
};

constexpr uint32 STORMBRINGER_SPELL_FAMILY = 22;

// Records that are NOT chained in `spell_ranks` (measured 2026-09-28): each id is named.
bool IsTorrentialWrath(uint32 id) { return id == 804017 || (id >= 503352 && id <= 503360); }
bool IsBrine(uint32 id) { return id >= 807105 && id <= 807111; }
bool IsAeroblast(uint32 id) { return id == 801839 || (id >= 501450 && id <= 501458); }

// Spell ids of the Static spenders, read from the resource service's own table, so that a
// rule added there (Aeroblast, Torrential Wrath...) is followed here without a second list.
// Rules marked None (Stormflow 567555, Exhale 705672, Discharge 805288) only check the
// threshold and spend nothing: they are not spenders.
bool IsStaticSpender(uint32 spellId)
{
    for (AscensionCompatData::ResourceCostRule const& rule : AscensionCompatData::ResourceCostRules)
        if (rule.ClassId == CLASS_STORMBRINGER && rule.ResourceSpellId == SPELL_STATIC &&
            rule.Consumption != AscensionCompatData::ResourceConsumption::None &&
            spellId >= rule.FirstSpellId && spellId <= rule.LastSpellId)
            return true;
    return false;
}

// The talent's own ProcChance, raised by SPELLMOD_CHANCE_OF_SUCCESS like the core does for
// a native proc (SpellAuras.cpp:2311-2312). A bare roll on ProcChance would ignore it.
bool RollTalentChance(Player* player, uint32 talentId)
{
    SpellInfo const* talent = AscensionSpellSafe::Get(talentId);
    if (!talent)
        return false;
    float chance = float(talent->ProcChance);
    player->ApplySpellMod(talentId, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    return roll_chance_f(chance);
}

// A custom base point is taken as the final value: Spell::SetSpellValue stores
// CalcBaseValue(value) (value - 1 when DieSides != 0) and SpellEffectInfo::CalcValue adds the
// die back (DieSides 1: +1). Nothing to offset; only a negative amount is refused.
int32 ExactBasePoints(uint32 /*spellId*/, uint8 /*effIndex*/, int32 wanted)
{
    return std::max(wanted, 0);
}

// Value of one effect of a learnt talent, as the player's aura carries it (talent mods included).
int32 TalentAmount(Player* player, uint32 talentId, uint8 effIndex)
{
    AuraEffect const* effect = player->GetAuraEffect(talentId, effIndex);
    return effect ? effect->GetAmount() : 0;
}

// Per-cast markers kept on the Spell (Spell::SetScriptValue), the carrier the class already uses.
bool FirstForCast(Spell* spell, uint32 key)
{
    if (spell->GetScriptValue(key))
        return false;
    spell->SetScriptValue(key, 1);
    return true;
}

class stormbringer_talent_casts : public AllSpellScript
{
public:
    stormbringer_talent_casts() : AllSpellScript("stormbringer_talent_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT, ALLSPELLHOOK_ON_CALCULATED_TARGET}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        if (!info || info->SpellFamilyName != STORMBRINGER_SPELL_FAMILY)
            return;
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || !spell || spell->IsTriggered())
            return;

        if (info->Id == SPELL_CLOUDBURST)
            // The active spell has a zero-radius dummy. Its separate native
            // helper supplies the ten-yard area and authored knockback speeds.
            player->CastSpell(player, SPELL_CLOUDBURST_KNOCKBACK, true);

        // 300839 Hurricanes: "Casting Unshackle now unshackles you" (570129 on the player).
        if (info->Id == SPELL_UNSHACKLE && player->HasAura(SPELL_HURRICANES))
            player->CastSpell(player, SPELL_HURRICANES_BUFF, true);

        // 707618 Lord of Lightning: Storm Ascendance "additionally grants you Lord of Lightning".
        if (info->Id == SPELL_STORM_ASCENDANCE && player->HasAura(SPELL_LORD_OF_LIGHTNING))
            player->CastSpell(player, SPELL_LORD_OF_LIGHTNING_BUFF, true);

        // 705686 Storm Clouds: Body of Lightning cast on an ally "now automatically casts on you".
        // The copy is triggered: no Static cost (the resource service skips triggered casts)
        // and it cannot come back here. 520843, the authored trigger, is not used: its effect 1
        // is a dispel that the talent does not promise.
        if (info->Id == SPELL_BODY_OF_LIGHTNING && player->HasAura(SPELL_STORM_CLOUDS))
        {
            Unit* target = spell->m_targets.GetUnitTarget();
            if (target && target != player)
                player->CastSpell(player, SPELL_BODY_OF_LIGHTNING, true);
        }

        // 806411 Amped Flow: "While channeling Stormflow, your Shock now strikes 1 additional
        // target". 567556 (SPELLMOD_JUMP_TARGETS) is held for the channel only; the mirror
        // below removes it as soon as the channel is gone.
        if (sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_STORMFLOW && player->HasAura(SPELL_AMPED_FLOW))
            if (Aura* amped = player->AddAura(SPELL_AMPED_FLOW_BUFF, player))
                if (int32 channel = spell->GetSpellInfo()->GetMaxDuration(); channel > 0)
                    amped->SetDuration(std::max(amped->GetDuration(), channel));

        // 300834 Wrath of Al'Akir: "Depleting Static extends the duration of your Unshackle".
        // The spend happens in this very hook (AscensionCompat.cpp OnSpellCast), which skips
        // it under Charged Conduit; the same exemption is kept here.
        if (player->HasAura(SPELL_WRATH_OF_ALAKIR) && !player->HasAura(SPELL_CHARGED_CONDUIT) &&
            IsStaticSpender(info->Id))
            player->CastSpell(player, SPELL_WRATH_OF_ALAKIR_EXTEND, true);
    }

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& result) override
    {
        // Conduction: "increased by $567559s1% for each stack of Conductive". Conductive is
        // the Stormbringer's own buff (every effect of 567559 is TARGET_UNIT_CASTER, aura text
        // "Some of your abilities deal additional Frost Damage"). Nothing is consumed.
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || info->Id != SPELL_CONDUCTION || !target || !result.damage)
            return;
        Unit* caster = spell->GetCaster();
        Aura const* conductive = caster ? caster->GetAura(SPELL_CONDUCTIVE, caster->GetGUID()) : nullptr;
        SpellInfo const* conductiveInfo = AscensionSpellSafe::Get(SPELL_CONDUCTIVE);
        if (!conductive || !conductiveInfo)
            return;
        int32 perStack = conductiveInfo->Effects[EFFECT_0].CalcValue(caster);
        if (perStack <= 0)
            return;
        float multiplier = 1.0f + float(perStack) * conductive->GetStackAmount() / 100.0f;
        result.damage = uint32(result.damage * multiplier);
        result.damageBeforeTakenMods = uint32(result.damageBeforeTakenMods * multiplier);
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool critical) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || player->getClass() != CLASS_STORMBRINGER || info->SpellFamilyName != STORMBRINGER_SPELL_FAMILY ||
            !target || target == player || player->IsFriendlyTo(target) || miss != SPELL_MISS_NONE)
            return;

        uint32 const first = sSpellMgr->GetFirstSpellInChain(info->Id);
        bool const triggered = spell->IsTriggered();

        if (info->Id == SPELL_PERPETUAL_SHOCK || (!triggered && first == SPELL_SHOCK))
        {
            // Two native half-second ticks each copy ten percent of the resolved hit.
            // The damage-over-time component does not require Call Lightning.
            if (damage && !spell->GetScriptValue(SPELL_SHOCK_DOT))
            {
                spell->SetScriptValue(SPELL_SHOCK_DOT, 1);
                player->CastCustomSpell(SPELL_SHOCK_DOT, SPELLVALUE_BASE_POINT0, int32(damage / 10), target, true);
            }
            if (player->HasSpell(SPELL_CALL_LIGHTNING) && !player->HasAura(SPELL_THUNDER_WARD) &&
                !spell->GetScriptValue(SPELL_STATIC))
            {
                spell->SetScriptValue(SPELL_STATIC, 1);
                player->CastSpell(player, SPELL_GENERATE_STATIC_20, true);
            }
        }

        // Effects placed ON the target need it alive; the self-refunds below do not (a
        // killing critical still refunds).
        bool const alive = target->IsAlive();

        // --- Conductive / Conduction (92098, 578300, 572310, Deluge) -------------------------
        // Torrential Wrath first reads the Conductive already there, then a critical may add one.
        if (alive && !triggered && IsTorrentialWrath(info->Id))
        {
            if (player->HasAura(SPELL_CONDUCTIVE_PASSIVE) && player->HasAura(SPELL_CONDUCTIVE, player->GetGUID()))
                player->CastSpell(target, SPELL_CONDUCTION, true);
            if (critical && player->HasAura(SPELL_SHOCKINGLY_POWERFUL) && FirstForCast(spell, SPELL_SHOCKINGLY_POWERFUL))
                player->AddAura(SPELL_CONDUCTIVE, player);
        }
        // 92098: "Shock/Brine now grants Conductive". 567559 is a buff of the Stormbringer
        // (all three effects TARGET_UNIT_CASTER; 578300 triggers it in TARGET_UNIT_CASTER too).
        // AddAura, not a cast: effect 1 (SPELL_EFFECT 64 -> 567573, "Consume this with Call
        // Lightning or Torrential Wrath", +30% per stack) has no consumption anywhere and is
        // left out. One stack per cast, not one per struck enemy.
        if (alive && !triggered && (first == SPELL_SHOCK || IsBrine(info->Id)) && player->HasAura(SPELL_CONDUCTIVE_PASSIVE) &&
            FirstForCast(spell, SPELL_CONDUCTIVE))
            player->AddAura(SPELL_CONDUCTIVE, player);
        // 578300 Storm Synergy: Electrocute "causes it to apply Conductive".
        if (alive && !triggered && first == SPELL_ELECTROCUTE && player->HasAura(SPELL_STORM_SYNERGY) &&
            FirstForCast(spell, SPELL_CONDUCTIVE))
            player->AddAura(SPELL_CONDUCTIVE, player);
        // 806400 Deluge: "triggering Conduction", once per struck enemy.
        if (alive && !triggered && first == SPELL_DELUGE)
            player->CastSpell(target, SPELL_CONDUCTION, true);

        // --- Forked Lightning ------------------------------------------------------------------
        if (alive && !triggered && first == SPELL_FORKED_LIGHTNING)
        {
            // 300609 Lightning Rod: one roll per cast; on success the Volt of the main target
            // is copied to every enemy the cast strikes.
            if (damage && player->HasAura(SPELL_LIGHTNING_ROD))
            {
                if (!spell->GetScriptValue(SPELL_LIGHTNING_ROD))
                    spell->SetScriptValue(SPELL_LIGHTNING_ROD, RollTalentChance(player, SPELL_LIGHTNING_ROD) ? 2 : 1);
                Unit* primary = spell->m_targets.GetUnitTarget();
                if (spell->GetScriptValue(SPELL_LIGHTNING_ROD) == 2 && primary && primary != target)
                    if (Aura const* volt = primary->GetAuraOfRankedSpell(SPELL_VOLT, player->GetGUID()))
                        player->AddAura(volt->GetId(), target);
            }
        }

        // 705643 Flux Arc, first sentence: Forked Lightning and Arm of Thorim mark enemies.
        if (alive && !triggered && (first == SPELL_FORKED_LIGHTNING || first == SPELL_ARM_OF_THORIM) &&
            player->HasAura(SPELL_FLUX_ARC))
            player->AddAura(SPELL_FLUX_ARC_MARK, target);

        if (!triggered && damage)
        {
            // 705643 Flux Arc, second sentence: Call Lightning against a marked enemy deals
            // $s1% of its damage to it and nearby enemies (705644, area around the target).
            if (alive && first == SPELL_CALL_LIGHTNING && target->HasAura(SPELL_FLUX_ARC_MARK, player->GetGUID()))
                if (int32 pct = TalentAmount(player, SPELL_FLUX_ARC, EFFECT_0); pct > 0)
                    player->CastCustomSpell(SPELL_FLUX_ARC_DAMAGE, SPELLVALUE_BASE_POINT0,
                        ExactBasePoints(SPELL_FLUX_ARC_DAMAGE, EFFECT_0, int32(CalculatePct(damage, pct))), target, true);

            // 570173 Thorim's Gift: "$s1% of its damage dealt ... every 1 sec for 3 sec".
            // The talent's tooltip (15% per tick) is the one the player reads; 570174's own
            // text says 20% in total. The talent is followed.
            // The patch is laid beneath the target, so it is laid even on a killing blow.
            if (first == SPELL_ARM_OF_THORIM)
                if (int32 pct = TalentAmount(player, SPELL_THORIMS_GIFT, EFFECT_0); pct > 0)
                    player->CastCustomSpell(SPELL_THORIMS_GIFT_AREA, SPELLVALUE_BASE_POINT0,
                        ExactBasePoints(SPELL_THORIMS_GIFT_AREA, EFFECT_0, int32(CalculatePct(damage, pct))), target, true);

            // 707615 Tempest Calling, effect 1 (aura 354, no core handler): Aeroblast hits
            // again for 40% of the damage dealt, one second later (802750, one tick at 999 ms).
            if (alive && IsAeroblast(info->Id) && FirstForCast(spell, SPELL_AEROBLAST_ECHO))
                if (int32 pct = TalentAmount(player, SPELL_TEMPEST_CALLING, EFFECT_1); pct > 0)
                    player->CastCustomSpell(SPELL_AEROBLAST_ECHO, SPELLVALUE_BASE_POINT0,
                        ExactBasePoints(SPELL_AEROBLAST_ECHO, EFFECT_0, int32(CalculatePct(damage, pct))), target, true);

            // 804828 Never Strikes Twice?: Electrocute has a $h% chance to strike again.
            if (alive && first == SPELL_ELECTROCUTE && player->HasAura(SPELL_NEVER_STRIKES_TWICE) &&
                FirstForCast(spell, SPELL_NEVER_STRIKES_TWICE) && RollTalentChance(player, SPELL_NEVER_STRIKES_TWICE))
                player->CastSpell(target, info->Id, true);

            // 807314 Critical Circuit: direct criticals of a Static spender refund 10 Static.
            // Deferred to the player's next update: a spell with no travel time (Arm of
            // Thorim, speed 0) hits inside handle_immediate, BEFORE the service spends the
            // Static in OnSpellCast (Spell.cpp:4079 then :4129). Given at once, the refund was
            // wiped by an "All" rule, or clipped at the cap before a "Fixed" spend.
            if (critical && player->HasAura(SPELL_CRITICAL_CIRCUIT) && IsStaticSpender(info->Id) &&
                FirstForCast(spell, SPELL_CRITICAL_CIRCUIT))
            {
                // The player's own event queue runs on its map update and is killed with the
                // unit (Unit::CleanupBeforeRemoveFromMap, Unit.cpp:13236): the pointer stays valid.
                player->m_Events.AddEventAtOffset([player]()
                {
                    if (player->IsInWorld() && player->IsAlive())
                        player->CastSpell(player, SPELL_ADD_10_STATIC, true);
                }, 1ms);
            }
        }
    }
};

// 707038 "Static Hidden" carries Arm of Thorim's growth with Static (e0 +20 ms cast time,
// e1 +1% damage, per stack, StackAmount 100, mask of Arm of Thorim only). Nothing applied it.
// It is mirrored from the Static stack on every player update; this also restores it after a
// death or a reconnection (707038 is not passive, P-162).
class stormbringer_static_mirror : public PlayerScript
{
public:
    stormbringer_static_mirror() : PlayerScript("stormbringer_static_mirror", {PLAYERHOOK_ON_UPDATE}) { }

    void OnPlayerUpdate(Player* player, uint32) override
    {
        if (player->getClass() != CLASS_STORMBRINGER || !player->IsInWorld())
            return;

        Aura* hidden = player->GetAura(SPELL_STATIC_HIDDEN, player->GetGUID());
        Aura const* charge = player->IsAlive() ? player->GetAura(SPELL_STATIC) : nullptr;
        uint8 stacks = charge ? charge->GetStackAmount() : 0;
        if (!stacks)
        {
            if (hidden)
                player->RemoveAurasDueToSpell(SPELL_STATIC_HIDDEN, player->GetGUID());
        }
        else
        {
            if (!hidden)
                hidden = player->AddAura(SPELL_STATIC_HIDDEN, player);
            if (hidden && hidden->GetStackAmount() != stacks)
                hidden->SetStackAmount(stacks);
        }

        // Amped Flow lasts as long as the Stormflow channel, not a moment longer.
        if (player->HasAura(SPELL_AMPED_FLOW_BUFF, player->GetGUID()))
        {
            Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!channel || sSpellMgr->GetFirstSpellInChain(channel->GetSpellInfo()->Id) != SPELL_STORMFLOW)
                player->RemoveAurasDueToSpell(SPELL_AMPED_FLOW_BUFF, player->GetGUID());
        }
    }
};

// While the shared stormbringer_superconductor multiplier (AscensionResourceTalents.cpp) is
// still in the binary — its contract turns Superconductor's effect 0 into a DUMMY — the native
// +1% per stack of 707038 would multiply with it for Superconductor holders ((1+n%)^2 instead
// of the designed 1+2n%). Their damage part stays with the module multiplier until the shared
// removal ships; the cast time part and every player without the talent get the native value.
class aura_ascension_stormbringer_static_hidden : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_static_hidden);

    void CalcDamage(AuraEffect const*, int32& amount, bool&)
    {
        SpellInfo const* superconductor = sSpellMgr->GetSpellInfo(SPELL_SUPERCONDUCTOR);
        Unit* caster = GetCaster();
        if (superconductor && superconductor->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_DUMMY &&
            caster && caster->HasAura(SPELL_SUPERCONDUCTOR))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_stormbringer_static_hidden::CalcDamage,
            EFFECT_1, SPELL_AURA_ADD_PCT_MODIFIER);
    }
};

class stormbringer_resource_contracts : public GlobalScript
{
public:
    stormbringer_resource_contracts() : GlobalScript("stormbringer_resource_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    // Helpers that copy an amount already resolved by the source hit: no crit, no caster or
    // taken modifiers, no spell power a second time (same contract as SPELL_SHOCK_DOT).
    static void InheritResolvedAmount(SpellInfo* info)
    {
        info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
        info->AscensionInheritsResolvedAmount = true;
        for (SpellEffectInfo& effect : info->Effects)
            effect.BonusMultiplier = 0.0f;
    }

    static void Skipped(SpellInfo const* info, char const* what)
    {
        LOG_ERROR("module.ascension_compat", "Stormbringer contract {}: {} no longer matches Spell.dbc, skipped",
            info->Id, what);
    }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->SpellFamilyName != STORMBRINGER_SPELL_FAMILY)
            return;
        switch (info->Id)
        {
            case SPELL_SHOCK_DOT:
            case SPELL_AEROBLAST_ECHO:
            case SPELL_THORIMS_GIFT_AREA:
            case SPELL_FLUX_ARC_DAMAGE:
            case SPELL_BLESSING_OF_LEI_SHEN_HEAL:
                InheritResolvedAmount(info);
                break;
            default:
                break;
        }
        if (info->Id == SPELL_FLUX_ARC_DAMAGE)
            // TargetAuraSpell 804832 is tested on EVERY target (SpellInfo::CheckTarget), so the
            // "nearby enemies" of the tooltip would be dropped. The mark is tested by the caller.
            info->TargetAuraSpell = 0;
        if (info->Id == SPELL_PERPETUAL_SHOCK)
            // The hit callback supplies the learned-spell gate and one 20-Static grant.
            info->Effects[EFFECT_1].Effect = 0;
        if (info->Id == SPELL_CHARGED_CONDUIT)
            // Keep the charges until this ten-second buff ends.
            info->Effects[EFFECT_2].Effect = 0;

        // 570129 Hurricanes: "increasing your damage dealt by 15%" on an aura 79 whose school
        // mask (MiscValue) is 0, i.e. no school at all.
        if (info->Id == SPELL_HURRICANES_BUFF)
        {
            if (info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_MOD_DAMAGE_PERCENT_DONE && !info->Effects[EFFECT_0].MiscValue)
                info->Effects[EFFECT_0].MiscValue = SPELL_SCHOOL_MASK_ALL;
            else
                Skipped(info, "effect 0 aura 79 with an empty school mask");
        }

        // 681187 Storm Ascendance form: effect 0 transforms into creature 161579, which is not
        // in creature_template; the core then falls back to display 16358, a pink pig
        // (SpellAuraEffects.cpp HandleAuraTransform). Neutralised until a model is chosen; the
        // -50% magic damage taken (effect 1) is kept.
        if (info->Id == SPELL_STORM_ASCENDANCE_FORM)
        {
            if (info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_TRANSFORM && info->Effects[EFFECT_0].MiscValue == 161579)
                info->Effects[EFFECT_0].Effect = 0;
            else
                Skipped(info, "effect 0 transform into 161579");
        }

        // 567556 Amped Flow: the tooltip names Shock and Brine; the raw mask also holds Gale's
        // bit (flags1 0x20000), which would give Gale +2 chain targets during the channel.
        if (info->Id == SPELL_AMPED_FLOW_BUFF)
        {
            if (info->Effects[EFFECT_0].SpellClassMask == flag96(2048, 131200, 0))
                info->Effects[EFFECT_0].SpellClassMask = flag96(2048, 128, 0);
            else
                Skipped(info, "mask (2048,131200,0)");
        }

        // 680316 Shocking Aegis: "reduces your chance to be critically struck" is stored as
        // +5 on aura 197, which the core ADDS to the attacker's crit (Unit.cpp). The Blizzard
        // convention is negative (Molten Armor 30482 e1 = -5).
        if (info->Id == SPELL_SHOCKING_AEGIS)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_1];
            int32 value = effect.BasePoints + (effect.DieSides == 1 ? 1 : 0);
            if (effect.ApplyAuraName == SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE && value > 0 &&
                (effect.DieSides == 0 || effect.DieSides == 1))
                effect.BasePoints = -value - (effect.DieSides == 1 ? 1 : 0);
            else
                Skipped(info, "effect 1 positive aura 197");
        }

        // 801855 Flow of Wrath: the SPELLMOD_EFFECT2 mask sits in word 2, where family 22 has
        // only 560032; Drown carries 0x08000000 in word 1 (as Washed Away 807657 targets it).
        if (info->Id == SPELL_FLOW_OF_WRATH)
        {
            if (info->Effects[EFFECT_0].SpellClassMask == flag96(0, 0, 134217728))
                info->Effects[EFFECT_0].SpellClassMask = flag96(0, 134217728, 0);
            else
                Skipped(info, "mask (0,0,134217728)");
        }

        // 705667/707793 Invoking Storms: SpellModOp 41 is an Ascension private op that
        // AuraEffect::CalculateSpellMod drops; its meaning is "spell power scaling", i.e.
        // SPELLMOD_BONUS_MULTIPLIER. The tooltip also names Arm of Thorim (flags1 0x2).
        if (info->Id == SPELL_INVOKING_STORMS_1 || info->Id == SPELL_INVOKING_STORMS_2)
        {
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && effect.MiscValue == 41)
            {
                effect.MiscValue = SPELLMOD_BONUS_MULTIPLIER;
                effect.SpellClassMask |= flag96(0, 2, 0);
            }
            else
                Skipped(info, "effect 0 pct modifier op 41");
        }

        // 705702 Unbound Elementalist rank 1: the +20% sits on effect 1, aimed at the pet,
        // where AuraEffect::ApplySpellMod does nothing (not a player). Effect 0, on the player
        // who casts Unshackle, holds +0%. Effect 1's value is moved onto effect 0.
        if (info->Id == SPELL_UNBOUND_ELEMENTALIST_1)
        {
            SpellEffectInfo& own = info->Effects[EFFECT_0];
            SpellEffectInfo const& pet = info->Effects[EFFECT_1];
            if (own.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && own.MiscValue == SPELLMOD_ALL_EFFECTS &&
                pet.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && pet.MiscValue == SPELLMOD_ALL_EFFECTS &&
                own.DieSides == pet.DieSides)
                own.BasePoints = pet.BasePoints;
            else
                Skipped(info, "effects 0/1 pct modifiers on all effects");
        }

        // 705691 Aethermancy rank 2: a PERCENT critical-chance modifier (ignored or multiplying,
        // Player::ApplySpellMod) where rank 1 705690 is a flat +20. Made flat, on rank 1's mask
        // (Invigorating Surge only; the raw mask also covers Updraft).
        if (info->Id == SPELL_AETHERMANCY_2)
        {
            SpellInfo const* rank1 = sSpellMgr->GetSpellInfo(SPELL_AETHERMANCY_1);
            SpellEffectInfo& effect = info->Effects[EFFECT_0];
            if (rank1 && effect.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER && effect.MiscValue == SPELLMOD_CRITICAL_CHANCE &&
                rank1->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER)
            {
                effect.ApplyAuraName = SPELL_AURA_ADD_FLAT_MODIFIER;
                effect.SpellClassMask = rank1->Effects[EFFECT_0].SpellClassMask;
            }
            else
                Skipped(info, "effect 0 pct crit modifier");
        }

        // 707652 Ionic Aura: effects 0/1 raise max mana and rage (aura 132 misc 0 and 1);
        // effect 2 is a caster dummy. It becomes the same raid aura for Energy (misc 3).
        // Focus and Runic Power have no slot left and stay unserved (reported).
        if (info->Id == SPELL_IONIC_AURA)
        {
            SpellEffectInfo const& mana = info->Effects[EFFECT_0];
            SpellEffectInfo& spare = info->Effects[EFFECT_2];
            if (spare.Effect == SPELL_EFFECT_APPLY_AURA && spare.ApplyAuraName == SPELL_AURA_DUMMY &&
                mana.Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID && mana.ApplyAuraName == SPELL_AURA_MOD_INCREASE_ENERGY_PERCENT)
            {
                spare.Effect = SPELL_EFFECT_APPLY_AREA_AURA_RAID;
                spare.ApplyAuraName = SPELL_AURA_MOD_INCREASE_ENERGY_PERCENT;
                spare.MiscValue = POWER_ENERGY;
                spare.BasePoints = mana.BasePoints;
                spare.DieSides = mana.DieSides;
                spare.RadiusEntry = mana.RadiusEntry;
            }
            else
                Skipped(info, "effect 2 dummy beside the raid mana aura");
        }

        // 500292 Kiss of the Clouds shield: "the target suffers no spell pushback" while it
        // holds. The empty effect 1 becomes SPELL_AURA_REDUCE_PUSHBACK 100 on the same target
        // (Spell::Delayed returns at >= 100). Effect 142 of 500039 passes BASE_POINT1 = m1
        // (>= 100 on every rank), which keeps it at or above 100.
        if (info->Id == SPELL_KISS_OF_THE_CLOUDS_SHIELD)
        {
            SpellEffectInfo& spare = info->Effects[EFFECT_1];
            if (!spare.Effect && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB)
            {
                spare.Effect = SPELL_EFFECT_APPLY_AURA;
                spare.ApplyAuraName = SPELL_AURA_REDUCE_PUSHBACK;
                spare.BasePoints = 99;
                spare.DieSides = 1;
                spare.TargetA = info->Effects[EFFECT_0].TargetA;
                spare.TargetB = info->Effects[EFFECT_0].TargetB;
                info->_InitializeExplicitTargetMask();
            }
            else
                Skipped(info, "empty effect 1 beside the absorb");
        }
    }
};

// 300593 Dark Skies: a direct damage spell that does not crit gives 680855 (+2% crit,
// stacking); a critical removes it. One decision per CAST (SpellPhaseMask 1 in spell_proc):
// that phase already knows whether any target was critically struck (Spell.cpp, hitMask
// |= PROC_HIT_CRITICAL before PROC_SPELL_PHASE_CAST), so a multi-target cast counts once.
class aura_ascension_stormbringer_dark_skies : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_dark_skies);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_DARK_SKIES_BUFF}); }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* source = eventInfo.GetSpellInfo();
        return eventInfo.GetActor() == GetTarget() && source && source->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE);
    }

    void Proc(AuraEffect const*, ProcEventInfo& eventInfo)
    {
        if (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
        {
            PreventDefaultAction();
            GetTarget()->RemoveAurasDueToSpell(SPELL_DARK_SKIES_BUFF);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_dark_skies::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_stormbringer_dark_skies::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 560030 Lightning Cage ("your damage spells generate 20 Static") and 681110 Storm Ascendance
// ("Damaging spells generate 10 additional Static"): their spell_proc rows use the HIT phase,
// emitted once per struck target (Spell::DoAllEffectOnTarget). One gain per cast is kept, with
// the per-cast marker of Tailwind (AscensionStormbringerEvents.cpp); a bouncing Forked
// Lightning would otherwise pay once per bounce.
class aura_ascension_stormbringer_once_per_cast : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_once_per_cast);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (eventInfo.GetActor() != GetTarget())
            return false;
        if (Spell const* procSpell = eventInfo.GetProcSpell())
        {
            if (procSpell->GetScriptValue(GetId()))
                return false;
            const_cast<Spell*>(procSpell)->SetScriptValue(GetId(), 1);
        }
        return true;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_once_per_cast::CheckProc);
    }
};

// 806412 Waterfall: "Dealing critical damage now reduces all of your cooldowns by 0.5 sec."
// 807661 names three fixed ids only (one of them rank 2 of Torrential Wrath); the tooltip is
// followed: every Stormbringer (family 22) spell cooldown the player has running. The amount
// is read from 807661. Once per cast.
class aura_ascension_stormbringer_waterfall : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_waterfall);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_WATERFALL_REDUCTION}); }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (eventInfo.GetActor() != GetTarget() || !GetTarget()->ToPlayer())
            return false;
        if (Spell const* procSpell = eventInfo.GetProcSpell())
        {
            if (procSpell->GetScriptValue(SPELL_WATERFALL))
                return false;
            const_cast<Spell*>(procSpell)->SetScriptValue(SPELL_WATERFALL, 1);
        }
        return true;
    }

    void Proc(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        SpellInfo const* reduction = sSpellMgr->GetSpellInfo(SPELL_WATERFALL_REDUCTION);
        if (!player || !reduction || reduction->Effects[EFFECT_0].Effect != SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN)
            return;
        int32 delta = reduction->Effects[EFFECT_0].CalcValue(player);
        if (delta >= 0)
            return;

        std::vector<uint32> running;
        for (auto const& [spellId, cooldown] : player->GetSpellCooldownMap())
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
                if (spellInfo->SpellFamilyName == STORMBRINGER_SPELL_FAMILY)
                    running.push_back(spellId);

        uint32 const reductionMs = uint32(-int64(delta));
        for (uint32 spellId : running)
            if (uint32 remaining = player->GetSpellCooldownDelay(spellId))
            {
                if (reductionMs >= remaining)
                    player->RemoveSpellCooldown(spellId, true);
                else
                    player->ModifySpellCooldown(spellId, delta);
            }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_waterfall::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_stormbringer_waterfall::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 561228 Blessing of Lei Shen: "Damage dealt by Stormflow now heals you equal to the damage
// dealt." Effect 0 is aura 354 (no core handler) with value 100 and trigger 561308.
class aura_ascension_stormbringer_lei_shen : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_lei_shen);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_BLESSING_OF_LEI_SHEN_HEAL}); }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* source = eventInfo.GetSpellInfo();
        return eventInfo.GetActor() == GetTarget() && GetTarget()->IsAlive() && eventInfo.GetDamageInfo() &&
            eventInfo.GetDamageInfo()->GetDamage() && source &&
            sSpellMgr->GetFirstSpellInChain(source->Id) == SPELL_STORMFLOW;
    }

    void Proc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        int32 heal = int32(CalculatePct(eventInfo.GetDamageInfo()->GetDamage(), aurEff->GetAmount()));
        if (heal > 0)
            GetTarget()->CastCustomSpell(SPELL_BLESSING_OF_LEI_SHEN_HEAL, SPELLVALUE_BASE_POINT0,
                ExactBasePoints(SPELL_BLESSING_OF_LEI_SHEN_HEAL, EFFECT_0, heal), GetTarget(), true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_lei_shen::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_stormbringer_lei_shen::Proc, EFFECT_0, AuraType(354));
    }
};

// 520084 Surge of Might (aura on allies): "deal ${$spn*0.7} additional damage as Nature
// damage when they deal direct damage". Effect 0 is PROC_TRIGGER_SPELL_WITH_VALUE toward
// 573254, whose amount is the aura's. The Stormbringer's Nature spell power is snapshot here;
// 0.7 is the tooltip's coefficient, the only number restated. 573254 has no coefficient of its
// own (BonusMultiplier 0, no spell_bonus_data), so the ally's spell power is not added.
class aura_ascension_stormbringer_surge_of_might : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_surge_of_might);

    void CalcAmount(AuraEffect const*, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;
        amount = ExactBasePoints(573254, EFFECT_0, int32(caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE) * 0.7f));
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return eventInfo.GetActor() == GetTarget() && eventInfo.GetDamageInfo() && eventInfo.GetDamageInfo()->GetDamage();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_stormbringer_surge_of_might::CalcAmount,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE);
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_surge_of_might::CheckProc);
    }
};

// 804833 Stormcloak: "all attacks against you have a $h% chance to deal half damage". Effect 0
// is a 2-point absorb that the first hit used up, removing the barrier (and its linked 805161).
// It becomes an unlimited absorb that takes half of a hit on a successful roll. Effect 1 (the
// native 10-Static proc) and effect 2 are untouched.
class aura_ascension_stormbringer_stormcloak : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_stormcloak);

    void CalcAmount(AuraEffect const*, int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }

    void Absorb(AuraEffect*, DamageInfo& damage, uint32& absorb)
    {
        absorb = 0;
        Unit* owner = GetTarget();
        if (!owner || damage.GetAttacker() == owner || !damage.GetDamage())
            return;
        float chance = float(GetSpellInfo()->ProcChance);
        if (Player* modOwner = owner->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_CHANCE_OF_SUCCESS, chance);
        if (roll_chance_f(chance))
            absorb = damage.GetDamage() / 2;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_stormbringer_stormcloak::CalcAmount,
            EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_stormbringer_stormcloak::Absorb, EFFECT_0);
    }
};

// 500292 Kiss of the Clouds shield: "${0+$m1+$INT*1}", the Intellect part was missing.
// The caster of 500292 is the Stormbringer (it triggers it through effect 142).
class aura_ascension_stormbringer_kiss_shield : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_kiss_shield);

    void CalcAmount(AuraEffect const*, int32& amount, bool&)
    {
        Unit* caster = GetCaster();
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (player && player->getClass() == CLASS_STORMBRINGER)
            amount += int32(player->GetStat(STAT_INTELLECT)); // $INT*1: the coefficient 1 is the tooltip's
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_stormbringer_kiss_shield::CalcAmount,
            EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
    }
};

// 806455 Eye of the Storm burst: the aura 227 of 806307 passes its rank value as BASE_POINT0,
// which lands on effect 0 (a dummy); the damage is effect 1 (bp 0). Moved to effect 1.
class spell_ascension_stormbringer_eye_burst : public SpellScript
{
    PrepareSpellScript(spell_ascension_stormbringer_eye_burst);

    void Launch(SpellEffIndex)
    {
        SpellInfo const* info = GetSpellInfo();
        // SetSpellValue stored CalcBaseValue(value), i.e. value - 1 when the die is not 0.
        int32 value = GetSpellValue()->EffectBasePoints[EFFECT_0] + (info->Effects[EFFECT_0].DieSides ? 1 : 0);
        if (value > 0)
            SetEffectValue(value);
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_stormbringer_eye_burst::Launch, EFFECT_1,
            SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 705692 Thunder Wave: Forked Lightning damage has a 5% chance (spell_proc) to give 712353,
// "Your Forked Lightning has transformed into Thunder Wave!". The next Forked Lightning then
// casts Thunder Wave 707220 at its target and deals no damage of its own.
class spell_ascension_stormbringer_forked_lightning : public SpellScript
{
    PrepareSpellScript(spell_ascension_stormbringer_forked_lightning);

    bool transformed = false;

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_THUNDER_WAVE_BUFF, SPELL_THUNDER_WAVE}); }

    void Transform()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (GetSpell()->IsTriggered() || !caster || !target || !caster->HasAura(SPELL_THUNDER_WAVE_BUFF))
            return;
        caster->RemoveAurasDueToSpell(SPELL_THUNDER_WAVE_BUFF);
        transformed = true;
        caster->CastSpell(target, SPELL_THUNDER_WAVE, true);
    }

    void Silence(SpellEffIndex effIndex)
    {
        if (transformed)
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_stormbringer_forked_lightning::Transform);
        OnEffectLaunchTarget += SpellEffectFn(spell_ascension_stormbringer_forked_lightning::Silence, EFFECT_0,
            SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// 707619 Pulse Conversion: "Dispelling a magic effect with Stormbreaker now heals you for 3%".
// No hit mask says "dispel succeeded": the target's dispellable magic buffs are counted
// before and after the hit.
class spell_ascension_stormbringer_stormbreaker : public SpellScript
{
    PrepareSpellScript(spell_ascension_stormbringer_stormbreaker);

    uint32 before = 0;

    static uint32 MagicBuffs(Unit* unit)
    {
        uint32 count = 0;
        for (auto const& [id, application] : unit->GetAppliedAuras())
        {
            Aura const* aura = application->GetBase();
            if (application->IsPositive() && aura->GetSpellInfo()->Dispel == DISPEL_MAGIC)
                count += std::max<uint32>({1u, uint32(aura->GetStackAmount()), uint32(aura->GetCharges())});
        }
        return count;
    }

    void Before(SpellMissInfo missInfo)
    {
        Unit* target = IsInTargetHook() ? GetHitUnit() : nullptr;
        before = (target && missInfo == SPELL_MISS_NONE) ? MagicBuffs(target) : 0;
    }

    void After()
    {
        Unit* target = IsInTargetHook() ? GetHitUnit() : nullptr;
        Unit* caster = GetCaster();
        if (!target || !caster || !before || !caster->HasAura(SPELL_PULSE_CONVERSION))
            return;
        if (MagicBuffs(target) < before)
            caster->CastSpell(caster, SPELL_PULSE_HEAL, true);
        before = 0;
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ascension_stormbringer_stormbreaker::Before);
        AfterHit += SpellHitFn(spell_ascension_stormbringer_stormbreaker::After);
    }
};

// 800099 Ride the Lightning: "blink forward a long distance, dealing damage to all enemies in
// your path". The data carries the cone damage (effect 2, 20 yd) but no movement. The length of
// the leap is that cone's radius, read, not invented. AfterCast: the cone was taken from the
// starting point, so it covers the path.
class spell_ascension_stormbringer_ride_the_lightning : public SpellScript
{
    PrepareSpellScript(spell_ascension_stormbringer_ride_the_lightning);

    void Leap()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsAlive() || caster->HasUnitState(UNIT_STATE_ROOT) || caster->GetVehicle() ||
            caster->IsInFlight())
            return;
        float distance = GetSpellInfo()->Effects[EFFECT_2].CalcRadius(caster);
        if (distance <= 0.0f)
            return;
        Position destination = caster->GetFirstCollisionPosition(distance, 0.0f);
        // casting = true, as Spell::EffectLeap does for a blink: the teleport must not
        // interrupt the spell that performs it.
        caster->NearTeleportTo(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
            caster->GetOrientation(), true);
        if (caster->IsPlayer()) // as Spell::EffectTeleportUnits: a unit does it inside NearTeleportTo
            caster->UpdateObjectVisibility(true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_stormbringer_ride_the_lightning::Leap);
    }
};

// Barometric Pressure 803563 starts 803566; Crushing Pressure 803564 has the same shape
// (effect 1 caster dummy) and starts 803567, which lengthens snares and roots.
class aura_ascension_barometric_pressure : public AuraScript
{
    PrepareAuraScript(aura_ascension_barometric_pressure);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_BAROMETRIC_SLOW, SPELL_CRUSHING_PRESSURE_AREA});
    }

    uint32 Companion() const
    {
        return GetId() == SPELL_CRUSHING_PRESSURE ? SPELL_CRUSHING_PRESSURE_AREA : SPELL_BAROMETRIC_SLOW;
    }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        // Only the owner's dummy effect may start the companion area aura.
        // Starting it on each hostile recipient would create extra area sources.
        if (GetCaster() && GetCaster() == GetTarget())
            GetCaster()->AddAura(Companion(), GetTarget());
    }

    void OnRemove(AuraEffect const*, AuraEffectHandleModes)
    {
        if (GetCasterGUID() == GetTarget()->GetGUID())
            GetTarget()->RemoveAurasDueToSpell(Companion(), GetCasterGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_barometric_pressure::Apply,
            EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_barometric_pressure::OnRemove,
            EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_electrical_charge : public AuraScript
{
    PrepareAuraScript(aura_ascension_electrical_charge);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_ELECTRICAL_CHARGE, SPELL_CHARGED_CONDUIT});
    }

    void Tick(AuraEffect const*)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget();
        if (owner->isMoving() || owner->HasAura(SPELL_CHARGED_CONDUIT))
            return;
        owner->CastSpell(owner, SPELL_ELECTRICAL_CHARGE, true);
        if (Aura* charges = owner->GetAura(SPELL_ELECTRICAL_CHARGE))
            // GetSpellInfo() rend nullptr si l'id sort du Spell.dbc (P-047) ; un repli a 0 ferait
            // partir le conduit des la premiere charge. 20 est la valeur lue au Spell.dbc en
            // service le 2026-09-21 (800299 "Electrical Charge", StackAmount=20).
            if (charges->GetStackAmount() >= AscensionSpellSafe::StackAmount(SPELL_ELECTRICAL_CHARGE, 20))
                owner->CastSpell(owner, SPELL_CHARGED_CONDUIT, true);
    }

    void OnRemove(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_CHARGED_CONDUIT);
        GetTarget()->RemoveAurasDueToSpell(SPELL_ELECTRICAL_CHARGE);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_electrical_charge::Tick,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_electrical_charge::OnRemove,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_charged_conduit : public AuraScript
{
    PrepareAuraScript(aura_ascension_charged_conduit);

    void OnRemove(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_ELECTRICAL_CHARGE);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_charged_conduit::OnRemove,
            EFFECT_1, SPELL_AURA_HASTE_SPELLS, AURA_EFFECT_HANDLE_REAL);
    }
};
}

void AddSC_AscensionStormbringerTalents()
{
    new stormbringer_talent_casts();
    new stormbringer_static_mirror();
    new stormbringer_resource_contracts();
    RegisterSpellScript(aura_ascension_barometric_pressure);
    RegisterSpellScript(aura_ascension_electrical_charge);
    RegisterSpellScript(aura_ascension_charged_conduit);
    RegisterSpellScript(aura_ascension_stormbringer_static_hidden);
    RegisterSpellScript(aura_ascension_stormbringer_dark_skies);
    RegisterSpellScript(aura_ascension_stormbringer_waterfall);
    RegisterSpellScript(aura_ascension_stormbringer_once_per_cast);
    RegisterSpellScript(aura_ascension_stormbringer_lei_shen);
    RegisterSpellScript(aura_ascension_stormbringer_surge_of_might);
    RegisterSpellScript(aura_ascension_stormbringer_stormcloak);
    RegisterSpellScript(aura_ascension_stormbringer_kiss_shield);
    RegisterSpellScript(spell_ascension_stormbringer_eye_burst);
    RegisterSpellScript(spell_ascension_stormbringer_forked_lightning);
    RegisterSpellScript(spell_ascension_stormbringer_stormbreaker);
    RegisterSpellScript(spell_ascension_stormbringer_ride_the_lightning);
}
