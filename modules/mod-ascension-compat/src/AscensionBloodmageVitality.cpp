/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionPooledVitality.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "WorldPacket.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using namespace AscensionBloodmage;
constexpr uint32 VitalityCost = 10;
constexpr uint32 DarkLiturgy = 800781;
// Quatrieme passe du 2026-09-28: Stamina terms of two tooltips, carried by no DBC field.
constexpr uint32 BloodRitualsHeal = 704119;     // 706624: "${$704119m1+$704119ppl1+$BH*0.07+$STA*0.085}"
constexpr double BloodRitualsStaminaShare = 0.085;
constexpr uint32 EnragingWoundLeech = 560589;   // 300978: "${$560589m1*$<scalingbp>+$STA*0.3}"
constexpr double EnragingWoundsStaminaShare = 0.3;

bool IsBloodmage(Player const* player)
{
    return player && player->getClass() == CLASS_SON_OF_ARUGAL && player->IsAlive() && player->IsInWorld();
}

bool CanEmpower(Player* player)
{
    if (!IsBloodmage(player) || !player->HasAura(PooledVitalityTalent) || player->HasAura(CursedFormCheck) ||
        player->HasAura(CursedForm))
        return false;
    Aura const* pool = player->GetAura(PooledVitality, player->GetGUID());
    return pool && uint32(pool->GetStackAmount()) >= VitalityCost;
}

class bloodmage_vitality_casts : public AllSpellScript
{
public:
    bloodmage_vitality_casts() : AllSpellScript("bloodmage_vitality_casts",
        {ALLSPELLHOOK_CAN_PREPARE, ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_BEFORE_EFFECTS,
            ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    bool CanPrepare(Spell* spell, SpellCastTargets const*, AuraEffect const*) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        SpellInfo const* info = spell->GetSpellInfo();
        if (!spell->IsTriggered() && info->SpellFamilyName == 26 && info->PowerType == POWER_RAGE &&
            (info->ManaCost || info->ManaCostPercentage) && CanEmpower(player))
            spell->SetScriptValue(PooledVitalityTalent, GetEmpowerment(info->Id));
        return true;
    }

    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        // A cast prepared with an instant/free benefit cannot finish with stacks
        // that expired or were spent by a different cast during its cast time.
        if (spell->GetScriptValue(PooledVitalityTalent) && !CanEmpower(spell->GetCaster()->ToPlayer()))
            result = SPELL_FAILED_CASTER_AURASTATE;
    }

    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const*) override
    {
        Player* player = caster->ToPlayer();
        if (!spell->GetScriptValue(PooledVitalityTalent) || spell->GetScriptValue(PooledVitality) ||
            !CanEmpower(player))
            return;
        spell->SetScriptValue(PooledVitality, 1);
        player->GetAura(PooledVitality, player->GetGUID())->ModStackAmount(-int32(VitalityCost));
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = caster->ToPlayer();
        // Empowered Transfusion: its -60 s is applied by Player::ApplySpellMod to this cast only, so the
        // client, which computes a player's cooldown from the DBC and the spell modifiers it was sent,
        // shows the full 180 s. The cooldown is already set here (Spell::SendSpellCooldown runs before the
        // OnSpellCast hook), so the delay read back is the real one. Before the guard below, which returns
        // for every cast whose stacks were not spent.
        if (IsBloodmage(player) && !spell->IsTriggered() &&
            spell->GetScriptValue(PooledVitalityTalent) == Transfusion && GetEmpowerment(info->Id) == Transfusion)
            if (uint32 const delay = player->GetSpellCooldownDelay(info->Id))
            {
                WorldPacket data;
                player->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, info->Id, delay);
                player->SendDirectMessage(&data);
            }
        if (!IsBloodmage(player) || spell->IsTriggered() || info->SpellFamilyName != 26 ||
            !spell->GetScriptValue(PooledVitality) || spell->GetScriptValue(VitalityForLater))
            return;
        spell->SetScriptValue(VitalityForLater, 1);
        if (spell->GetScriptValue(PooledVitalityTalent))
        {
            if (player->HasAura(VitalityForLater))
                player->CastSpell(player, VitalityHeal, true);
        }
        else if (info->PowerType == POWER_HEALTH && player->HasAura(PooledVitalityTalent))
            player->CastSpell(player, PooledVitality, true);
    }

    void OnSpellHitResult(Spell* spell, Unit*, uint8 miss, uint32, uint32 healing, bool) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        if (!IsBloodmage(player) || spell->IsTriggered() || miss != SPELL_MISS_NONE || !healing ||
            spell->GetScriptValue(PooledVitalityTalent) != Mend || spell->GetScriptValue(MendSelfHeal))
            return;
        spell->SetScriptValue(MendSelfHeal, 1);
        // Custom basepoints pass through native float arithmetic before returning to int32.
        uint32 maximum = uint32(std::nextafter(float(std::numeric_limits<int32>::max()), 0.0f));
        int32 amount = int32(std::min(healing / 2, maximum));
        if (amount)
            player->CastCustomSpell(player, MendSelfHeal, &amount, nullptr, nullptr, true);
    }
};

class bloodmage_vitality_scaling : public UnitScript
{
public:
    bloodmage_vitality_scaling() : UnitScript("bloodmage_vitality_scaling", true,
        {UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE}) { }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        if (!caster || !caster->IsPlayer() || caster->getClass() != CLASS_SON_OF_ARUGAL || !info ||
            info->SpellFamilyName != 26 || index >= MAX_SPELL_EFFECTS)
            return;
        SpellEffectInfo const& effect = info->Effects[index];
        // The Spirit terms of the tooltips. Spirit has no native coefficient channel (spell_bonus_data
        // carries SP and AP only), so it is added to the base value here; the SP/AP terms are the
        // spell_bonus_data rows of 2026_09_28_26_ascension_bloodmage_nettoyage.sql. None of these
        // numbers is in a DBC field: each is the one written in the named tooltip.
        Stats stat = STAT_SPIRIT;
        double share = 0.0;
        if (info->Id == VitalityHeal && index == EFFECT_0 && effect.Effect == SPELL_EFFECT_HEAL)
            share = 0.5;
        // Sanguine Mend, its nine ranks by id (GetEmpowerment), never 681032 MendSelfHeal, which
        // inherits the final amount: "${$m1+0+$SPI*0.25+$AP*.2+$SP*.58}".
        else if (GetEmpowerment(info->Id) == Mend && index == EFFECT_0 && effect.Effect == SPELL_EFFECT_HEAL)
            share = 0.25;
        // Heartbreak, its eight ranks: "${$m1+0+$sps*0.5+$spi*1}" on the leech (effect 0) and
        // "${$m3+0+$BH*.3+$spi*1}" on effect 2, the value 142 hands to 520315. 520315 itself is left
        // alone: it receives that value as custom base points, and Spirit would count twice.
        else if (GetEmpowerment(info->Id) == Heartbreak &&
            ((index == EFFECT_0 && effect.Effect == SPELL_EFFECT_HEALTH_LEECH) ||
                (index == EFFECT_2 && effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE)))
            share = 1.0;
        // Dark Liturgy, its six ranks (spell_ranks head 800781): "${$m1+0+$SP*0.35+$SPI*0.25}" on the bounce
        // heal (effect 0). The SP term is its spell_bonus_data row (2026_09_28_26); this hook is called for
        // each target of the bounce, on that target's base value, so the Spirit is added once per heal.
        else if (sSpellMgr->GetFirstSpellInChain(info->Id) == DarkLiturgy && index == EFFECT_0 &&
            effect.Effect == SPELL_EFFECT_HEAL)
            share = 0.25;
        // Blood Rituals heal (Dark Liturgy's marks, AscensionBloodmageSecondary.cpp): "+$STA*0.085". The
        // "$BH*0.07" is its spell_bonus_data row (2026_09_28_26). Called once per healed ally.
        else if (info->Id == BloodRitualsHeal && index == EFFECT_0 && effect.Effect == SPELL_EFFECT_HEAL)
        {
            stat = STAT_STAMINA;
            share = BloodRitualsStaminaShare;
        }
        // Enraging Wounds 300978, its leech 560589: "+$STA*0.3". The base points were multiplied by
        // $scalingbp in the ascension_scaling_base hook, registered before this one (MP_loader.cpp:189 against
        // :325; hooks run in registration order): the Stamina term is added after, unscaled. The leech's heal
        // is its damage x 1.0, so the term is not counted twice.
        else if (info->Id == EnragingWoundLeech && index == EFFECT_0 && effect.Effect == SPELL_EFFECT_HEALTH_LEECH)
        {
            stat = STAT_STAMINA;
            share = EnragingWoundsStaminaShare;
        }
        if (share == 0.0)
            return;
        double amount = double(value) + caster->GetStat(stat) * share;
        if (std::isfinite(amount) && amount >= 0 &&
            double(float(amount)) <= std::numeric_limits<int32>::max())
            value = float(amount);
    }
};

// 705734 Transfusion: "Swap health percentages with an ally. Increases the lower health percentage of
// the two to $s1%, if below that amount." Its single effect is SPELL_EFFECT_SCRIPT_EFFECT (77), which
// Spell::EffectScriptEffect only handles for the GENERIC and ROGUE families (SpellEffects.cpp:4170):
// the cast took its cost and cooldown and did nothing. The 40 is the effect's value.
class spell_ascension_bloodmage_transfusion : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_transfusion);

    static void SetHealthPct(Unit* unit, float pct)
    {
        double const health = double(unit->GetMaxHealth()) * double(pct) / 100.0;
        unit->SetHealth(uint32(std::clamp<double>(health, 1.0, double(unit->GetMaxHealth()))));
    }

    void Swap(SpellEffIndex /*index*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || target == caster || !caster->IsAlive() || !target->IsAlive())
            return;
        float const floor = float(std::clamp(GetEffectValue(), 0, 100));
        float casterPct = target->GetHealthPct();
        float targetPct = caster->GetHealthPct();
        if (casterPct <= targetPct)
            casterPct = std::max(casterPct, floor);
        else
            targetPct = std::max(targetPct, floor);
        SetHealthPct(caster, casterPct);
        SetHealthPct(target, targetPct);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_bloodmage_transfusion::Swap, EFFECT_0,
            SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

class spell_ascension_bloodmage_empowered : public SpellScript
{
    PrepareSpellScript(spell_ascension_bloodmage_empowered);
    bool _heartbreak = false;

    bool Load() override
    {
        return GetCaster()->IsPlayer() && GetCaster()->getClass() == CLASS_SON_OF_ARUGAL &&
            GetSpellInfo()->SpellFamilyName == 26;
    }

    bool Empowered(Empowerment kind)
    {
        return !GetSpell()->IsTriggered() && GetSpell()->GetScriptValue(PooledVitalityTalent) == kind;
    }

    void HeartbreakPower(SpellEffIndex index)
    {
        if (GetSpellInfo()->Effects[index].TriggerSpell != HeartbreakBuff)
            return;
        // Native effect 142 otherwise grants the empowered buff unconditionally,
        // and its old enemy selector gives the party buff the wrong anchor.
        PreventHitDefaultEffect(index);
        if (!Empowered(Heartbreak) || _heartbreak)
            return;
        _heartbreak = true;
        int32 amount = GetEffectValue();
        Unit* player = GetCaster();
        player->CastCustomSpell(player, HeartbreakBuff, &amount, &amount, nullptr, true);
        player->CastSpell(player, VisceralPower, true);
    }

    void ModifyHit()
    {
        if (Empowered(Bloodbolt) && GetHitUnit() == GetExplTargetUnit() && GetHitDamage() > 0)
            SetHitDamage(int32(std::min<int64>(int64(GetHitDamage()) * 2, std::numeric_limits<int32>::max())));
        if (Empowered(Fleshcraft) && GetHitUnit() && GetHitUnit()->IsAlive() && GetHitHeal() > 0)
        {
            Unit* caster = GetCaster();
            Unit* target = GetHitUnit();
            uint32 bonus = caster->SpellHealingBonusDone(target, GetSpellInfo(),
                caster->CountPctFromMaxHealth(25), HEAL, EFFECT_0);
            bonus = target->SpellHealingBonusTaken(caster, GetSpellInfo(), bonus, HEAL);
            SetHitHeal(int32(std::min<int64>(int64(GetHitHeal()) + bonus, std::numeric_limits<int32>::max())));
        }
    }

    void Register() override
    {
        if (GetEmpowerment(m_scriptSpellId) == Heartbreak)
            OnEffectLaunchTarget += SpellEffectFn(spell_ascension_bloodmage_empowered::HeartbreakPower,
                EFFECT_1, SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE);
        OnHit += SpellHitFn(spell_ascension_bloodmage_empowered::ModifyHit);
    }
};
}

void AddSC_AscensionBloodmageVitality()
{
    new bloodmage_vitality_casts();
    new bloodmage_vitality_scaling();
    RegisterSpellScript(spell_ascension_bloodmage_empowered);
    // Nettoyage du 2026-09-28 (SQL compagnon 2026_09_28_26_ascension_bloodmage_nettoyage.sql).
    RegisterSpellScript(spell_ascension_bloodmage_transfusion);
}
