/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
// Chronomancer (class 22, SpellFamilyName 28) talents whose client rows promise a
// behaviour that no native aura handler provides.
//
// Every number used here was read either in Spell.dbc (field indices checked against
// the layout in src/server/shared/DataStores/DBCStructure.h) or in the spell's own
// client description. Nothing is guessed; what the data does not say is left undone
// and reported instead.
#include "Group.h"
#include "GroupReference.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace
{
enum ChronomancerEventSpells : uint32
{
    VastInfinite        = 706083, // "The Vast Infinite"          effect 0 = aura 69 SCHOOL_ABSORB, 25, misc 127, miscB 100
    VastInfiniteShare   = 707600, // "The Vast Infinite" / Damage effect 0 = SCHOOL_DAMAGE, target 25 ANY
    VastInfiniteHeal    = 707601, // "The Vast Infinite" / Heal   effect 0 = HEAL, target 21 ALLY
    Timeguard           = 804441, // "Timeguard"                  effect 0 = aura 69, effects 1/2 = DUMMY 35 and 50, ProcCharges 3
    RapidAcceleration   = 570149, // "Rapid Acceleration"         effect 0 = ADD_PCT_MODIFIER 15 op 40, effect 1 = DUMMY 15
    AcceleratedRecovery = 800857, // "Accelerated Recovery" rank 1, effect 0 = aura 8 PERIODIC_HEAL, 1 s, 15 s
    UnmakerOfRealities  = 706107, // "Unmaker of Realities"       passive DUMMY on the Chronomancer
    Hasten              = 801304, // "Hasten"                     speed / melee haste / spell haste on an ally
    HastenScaling       = 803382, // "Hasten" / SLS               ProcChance 25, effect 0 amount 30, trigger 803706
    HastyStrike         = 803706  // "Hasty Strike" / Damage      effect 0 = SCHOOL_DAMAGE, target 6 ENEMY
};

// Client SpellModOp carried by Rapid Acceleration's first effect. MAX_SPELLMOD is 32,
// so AuraEffect::CalculateSpellMod drops it with a debug line and the effect is inert.
constexpr int32 AscensionHealingScalingModOp = 40;

// Timeguard's client tooltip: "the next 3 instances of direct damage that deal more
// than 20% of their total health or reduce the target below 35% health". The 35 and
// the 50 are effects 1 and 2 of the spell; the 20 exists only in that sentence.
constexpr uint32 TimeguardHeavyHitPct = 20;

Player* EventChronomancer(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_CHRONOMANCER ? player : nullptr;
}

int32 EffectAmount(uint32 id, uint8 index, Unit const* caster)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    return info ? info->Effects[index].CalcValue(caster) : 0;
}

uint64 ScaleDown(uint64 value, uint32 percent)
{
    return percent >= 100 ? value : value * uint64(percent) / 100;
}

int32 ClampToInt32(uint64 value)
{
    return int32(std::min<uint64>(value, uint64(std::numeric_limits<int32>::max())));
}

// Everyone who currently carries The Vast Infinite from the same Chronomancer. The
// spell lands with target 56 TARGET_UNIT_CASTER_AREA_RAID, so the bearers are exactly
// the group members that were in range; the aura itself is the authoritative filter.
std::vector<ObjectGuid> VastInfiniteBearers(Unit* caster, Unit* anchor)
{
    std::vector<ObjectGuid> bearers;
    Player* owner = caster ? caster->ToPlayer() : nullptr;
    if (!owner || !anchor)
        return bearers;
    ObjectGuid casterGuid = owner->GetGUID();
    auto consider = [&bearers, &casterGuid, anchor](Player* member)
    {
        if (member && member->IsAlive() && member->IsInWorld() && member->GetMap() == anchor->GetMap() &&
            member->HasAura(VastInfinite, casterGuid))
            bearers.push_back(member->GetGUID());
    };
    if (Group* group = owner->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            consider(itr->GetSource());
    else
        consider(owner);
    return bearers;
}

// "All party and raid members become one, equally sharing 25% of all damage dealt to
// them, up to a maximum of 100% of their total health. At the end of the duration, all
// affected allies are healed equal to the total damage shared split evenly amongst all
// allies." The DBC gives 25 in effect 0, the 100 in its MiscValueB, and names the two
// carriers 707600 (share) and 707601 (heal); none of that runs without this script.
class aura_ascension_vast_infinite : public AuraScript
{
    PrepareAuraScript(aura_ascension_vast_infinite);

    std::vector<ObjectGuid> _bearers;

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({VastInfiniteShare, VastInfiniteHeal});
    }

    // The absorb pool is the cap, not the shield: MiscValueB percent of the bearer's
    // maximum health. The core spends it hit after hit and drops the aura at zero,
    // which is exactly "up to a maximum of 100% of their total health".
    void Pool(AuraEffect const* effect, int32& amount, bool& recalculate)
    {
        recalculate = false;
        Unit* target = GetUnitOwner();
        int32 percent = GetSpellInfo()->Effects[effect->GetEffIndex()].MiscValueB;
        if (!target || percent <= 0)
        {
            amount = 0;
            return;
        }
        amount = ClampToInt32(target->CountPctFromMaxHealth(percent));
    }

    void Absorb(AuraEffect* effect, DamageInfo& damage, uint32& amount)
    {
        uint32 pool = amount;
        amount = 0;
        _bearers.clear();
        Unit* target = GetTarget();
        Unit* caster = GetCaster();
        SpellInfo const* source = damage.GetSpellInfo();
        // A shared portion is already someone else's mitigated damage: never share it
        // again. This guard is load bearing, not decorative: Unit::DealDamage itself
        // calls Unit::CalcAbsorbResist for every SPELL_AURA_SHARE_DAMAGE_PCT aura on the
        // unit it hits, keeping the SpellInfo it was given. Without the test, an ally
        // who runs a damage-split aura would bounce the transfer straight back in here.
        if (!pool || !damage.GetDamage() || !target || !target->IsAlive() || !caster ||
            (source && source->Id == VastInfiniteShare))
            return;
        _bearers = VastInfiniteBearers(caster, target);
        if (_bearers.size() < 2)
            return; // Nobody to become one with.
        int32 sharePct = GetSpellInfo()->Effects[effect->GetEffIndex()].CalcValue(caster);
        if (sharePct <= 0)
            return;
        uint64 share = std::min<uint64>(ScaleDown(damage.GetDamage(), uint32(std::min(sharePct, 100))), pool);
        uint64 others = uint64(_bearers.size()) - 1;
        // Absorb only what can be handed out whole, so no damage silently disappears.
        amount = uint32(std::min<uint64>((share / others) * others, pool));
    }

    // AFTER_ABSORB carries the amount the core really took off the hit, after its own
    // clamp to the remaining damage and after the attacker's absorb-piercing auras.
    void Spread(AuraEffect* /*effect*/, DamageInfo& /*damage*/, uint32& amount)
    {
        Unit* target = GetTarget();
        Unit* caster = GetCaster();
        SpellInfo const* carrier = sSpellMgr->GetSpellInfo(VastInfiniteShare);
        if (!amount || _bearers.size() < 2 || !target || !caster || !carrier)
            return;
        uint64 others = uint64(_bearers.size()) - 1;
        uint64 portion = uint64(amount) / others;
        if (!portion)
            return;
        ObjectGuid victimGuid = target->GetGUID();
        ObjectGuid casterGuid = caster->GetGUID();
        SpellSchoolMask school = carrier->GetSchoolMask();
        uint64 handedOut = 0;
        for (ObjectGuid guid : _bearers)
        {
            if (guid == victimGuid)
                continue;
            Unit* ally = ObjectAccessor::GetUnit(*target, guid);
            if (!ally || !ally->IsAlive() || !ally->IsInWorld())
                continue;
            // Already mitigated on the original victim: deal it raw, self-inflicted, so
            // it cannot be absorbed a second time nor re-enter this handler.
            uint32 dealt = Unit::DealDamage(ally, ally, uint32(portion), nullptr, DOT, school, carrier, false);
            handedOut += dealt;
            ally->SendSpellNonMeleeDamageLog(ally, carrier, dealt, school, 0, 0, false, 0);
        }
        uint64 credit = handedOut / uint64(_bearers.size());
        if (!credit)
            return;
        // Every bearer banks the same share of every transfer, so each aura ends the
        // duration holding "the total damage shared split evenly amongst all allies".
        for (ObjectGuid guid : _bearers)
            if (Unit* ally = ObjectAccessor::GetUnit(*target, guid))
                if (Aura* aura = ally->GetAura(VastInfinite, casterGuid))
                    aura->SetScriptValue(VastInfiniteHeal,
                        std::min<uint64>(aura->GetScriptValue(VastInfiniteHeal) + credit,
                            uint64(std::numeric_limits<int32>::max())));
    }

    void Payout(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        AuraApplication const* application = GetTargetApplication();
        if (!application)
            return;
        AuraRemoveMode mode = application->GetRemoveMode();
        // EXPIRE is the announced end of the duration; ENEMY_SPELL is the core's mode
        // when the absorb pool runs out, which ends the effect just as definitively.
        if (mode != AURA_REMOVE_BY_EXPIRE && mode != AURA_REMOVE_BY_ENEMY_SPELL)
            return;
        Unit* target = GetTarget();
        Unit* caster = GetCaster();
        uint64 credit = GetAura() ? GetAura()->GetScriptValue(VastInfiniteHeal) : 0;
        if (!credit || !target || !target->IsAlive() || !target->IsInWorld())
            return;
        // The banked share belongs to the bearer, not to the Chronomancer. If the caster
        // logged out or changed map the payout still happens, self cast: 707601 takes
        // target 21 TARGET_UNIT_TARGET_ALLY, and a unit is its own valid assist target.
        Unit* healer = caster && caster->IsInWorld() ? caster : target;
        healer->CastCustomSpell(VastInfiniteHeal, SPELLVALUE_BASE_POINT0, ClampToInt32(credit), target, true);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_vast_infinite::Pool,
            EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_vast_infinite::Absorb, EFFECT_0);
        AfterEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_vast_infinite::Spread, EFFECT_0);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_vast_infinite::Payout,
            EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// "Place a magical guard on an ally, causing the next 3 instances of direct damage that
// deal more than 20% of their total health or reduce the target below 35% health to deal
// 50% less damage." Effect 0 is an absorb shield of 2 points: without a script the guard
// eats two points of the first hit and vanishes.
class aura_ascension_timeguard : public AuraScript
{
    PrepareAuraScript(aura_ascension_timeguard);

    // -1 tells Unit::CalcAbsorbResist the shield has no pool of its own; the amount is
    // decided per hit below, and the three instances are the aura's DBC ProcCharges.
    void Unlimited(AuraEffect const* /*effect*/, int32& amount, bool& recalculate)
    {
        amount = -1;
        recalculate = false;
    }

    void Absorb(AuraEffect* /*effect*/, DamageInfo& damage, uint32& amount)
    {
        amount = 0;
        Unit* target = GetTarget();
        uint32 hit = damage.GetDamage();
        if (!target || !hit)
            return;
        if (damage.GetDamageType() != DIRECT_DAMAGE && damage.GetDamageType() != SPELL_DIRECT_DAMAGE)
            return; // "instances of direct damage"
        uint64 maximum = target->GetMaxHealth();
        if (!maximum)
            return;
        bool heavy = uint64(hit) * 100 > maximum * uint64(TimeguardHeavyHitPct);
        int32 floorPct = std::clamp(GetSpellInfo()->Effects[EFFECT_1].CalcValue(GetCaster()), 0, 100);
        uint64 remaining = target->GetHealth() > hit ? uint64(target->GetHealth()) - hit : 0;
        // Read as "the hit leaves them under the floor", which also covers a bearer who
        // was already under it. The tooltip's own wording does not separate the two, and
        // this is the reading a guard is for.
        bool lethal = remaining * 100 < maximum * uint64(uint32(floorPct));
        if (!heavy && !lethal)
            return;
        int32 cutPct = std::clamp(GetSpellInfo()->Effects[EFFECT_2].CalcValue(GetCaster()), 0, 100);
        amount = uint32(ScaleDown(hit, uint32(cutPct)));
    }

    void Spend(AuraEffect* /*effect*/, DamageInfo& /*damage*/, uint32& amount)
    {
        // The amount here is what the core actually removed from the hit. The aura keeps
        // an infinite pool, so the core never touches its charges by itself.
        if (amount && GetAura())
            GetAura()->DropCharge();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_timeguard::Unlimited,
            EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_timeguard::Absorb, EFFECT_0);
        AfterEffectAbsorb += AuraEffectAbsorbFn(aura_ascension_timeguard::Spend, EFFECT_0);
    }
};

// Rapid Acceleration, second half: "Your Accelerated Recovery instantly heals the target
// for 15% of the total periodic effect." Effect 1 of the talent is a bare DUMMY.
class aura_ascension_accelerated_recovery : public AuraScript
{
    PrepareAuraScript(aura_ascension_accelerated_recovery);

    void Jumpstart(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        Player* player = EventChronomancer(GetCaster());
        if (!player || !target || !target->IsAlive() || !player->HasAura(RapidAcceleration))
            return;
        int32 percent = EffectAmount(RapidAcceleration, EFFECT_1, player);
        int32 ticks = effect->GetTotalTicks();
        int32 perTick = effect->GetAmount();
        if (percent <= 0 || ticks <= 0 || perTick <= 0)
            return;
        // The stored amount of a unit periodic heal already carries the caster's healing
        // bonus (AuraEffect::CalculateAmount), so ticks * amount is the announced total.
        uint64 instant = ScaleDown(uint64(uint32(perTick)) * uint64(uint32(ticks)),
            uint32(std::min(percent, 100)));
        if (!instant)
            return;
        HealInfo healing(player, target, uint32(ClampToInt32(instant)), GetSpellInfo(),
            GetSpellInfo()->GetSchoolMask());
        player->HealBySpell(healing);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_accelerated_recovery::Jumpstart,
            EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Unmaker of Realities: "While Hasten is active the ally now has a $803382h% chance when
// they deal damage to strike the target an additional time equal to $803382s1% of the
// damage dealt." 803382 carries ProcChance 25 and effect 0 amount 30; its aura type 354
// has a nullptr entry in AuraEffectHandler[], so nothing at all happens natively.
class aura_ascension_hasten_strikes : public AuraScript
{
    PrepareAuraScript(aura_ascension_hasten_strikes);

    bool Validate(SpellInfo const* /*info*/) override
    {
        return ValidateSpellInfo({HastyStrike, HastenScaling, UnmakerOfRealities});
    }

    bool Check(ProcEventInfo& event)
    {
        Unit* target = GetTarget();
        if (!target || event.GetActor() != target)
            return false;
        Unit* victim = event.GetActionTarget();
        if (!victim || victim == target || !victim->IsAlive() || !victim->IsInWorld())
            return false;
        DamageInfo* damage = event.GetDamageInfo();
        if (!damage || !damage->GetDamage())
            return false;
        SpellInfo const* source = event.GetSpellInfo();
        if (source && source->Id == HastyStrike)
            return false; // the extra strike must never feed itself
        // Both Hasten rows may be present on the same ally; only one may carry the proc.
        if (GetId() == HastenScaling && target->HasAura(Hasten, GetCasterGUID()))
            return false;
        Player* player = EventChronomancer(GetCaster());
        return player && player->HasAura(UnmakerOfRealities);
    }

    void Strike(ProcEventInfo& event)
    {
        // Aura 354, 31, 192 and 216 have no case of their own in AuraEffect::HandleProc,
        // but stop the per-effect pass anyway rather than rely on that.
        PreventDefaultAction();
        Unit* target = GetTarget();
        Unit* victim = event.GetActionTarget();
        DamageInfo* damage = event.GetDamageInfo();
        if (!target || !victim || !damage)
            return;
        int32 percent = EffectAmount(HastenScaling, EFFECT_0, GetCaster());
        if (percent <= 0)
            return;
        uint64 blow = ScaleDown(damage->GetDamage(), uint32(std::min(percent, 100)));
        if (!blow)
            return;
        target->CastCustomSpell(HastyStrike, SPELLVALUE_BASE_POINT0, ClampToInt32(blow), victim, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_hasten_strikes::Check);
        OnProc += AuraProcFn(aura_ascension_hasten_strikes::Strike);
    }
};

void ApplyChronomancerEventContracts(SpellInfo* info)
{
    if (!info || info->SpellFamilyName != 28)
        return;
    if (info->Id == RapidAcceleration)
    {
        // The client row asks for SpellModOp 40. MAX_SPELLMOD is 32, so
        // AuraEffect::CalculateSpellMod refuses the modifier and the effect is inert.
        // "Increases the bonus healing scaling of Accelerated Recovery" is the spell
        // power coefficient, which is SPELLMOD_BONUS_MULTIPLIER in both
        // Unit::SpellHealingBonusDone and Unit::SpellDamageBonusDone.
        SpellEffectInfo& scaling = info->Effects[EFFECT_0];
        if (scaling.ApplyAuraName == SPELL_AURA_ADD_PCT_MODIFIER &&
            scaling.MiscValue == AscensionHealingScalingModOp)
            scaling.MiscValue = SPELLMOD_BONUS_MULTIPLIER;
    }
    if (info->Id == VastInfiniteHeal || info->Id == HastyStrike)
    {
        // Both carry an amount resolved from damage that already went through the
        // caster's outgoing modifiers. Running them again would count them twice.
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
    }
}

class chronomancer_event_contracts : public GlobalScript
{
public:
    chronomancer_event_contracts() : GlobalScript("chronomancer_event_contracts",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        ApplyChronomancerEventContracts(info);
    }
};
} // namespace

void AddSC_AscensionChronomancerEvents()
{
    new chronomancer_event_contracts();
    RegisterSpellScript(aura_ascension_vast_infinite);
    RegisterSpellScript(aura_ascension_timeguard);
    RegisterSpellScript(aura_ascension_accelerated_recovery);
    RegisterSpellScript(aura_ascension_hasten_strikes);
}
