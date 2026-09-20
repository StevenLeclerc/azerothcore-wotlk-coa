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
        // Rebuilt from scratch for this hit; never emptied before the guard above. A
        // re-entry through the shared damage reaches this handler while Spread() is still
        // walking the bearers, and Spread() only owns a local copy because of that.
        _bearers = VastInfiniteBearers(caster, target);
        if (_bearers.size() < 2)
            return; // Nobody to become one with.
        int32 sharePct = GetSpellInfo()->Effects[effect->GetEffIndex()].CalcValue(caster);
        if (sharePct <= 0)
            return;
        uint64 share = std::min<uint64>(ScaleDown(damage.GetDamage(), uint32(std::min(sharePct, 100))), pool);
        uint64 others = uint64(_bearers.size()) - 1;
        // Ask for a multiple of the share count. The core still trims what it grants: it
        // clamps to the remaining damage and then applies the attacker's absorb piercing
        // (Unit::CalcAbsorbResist, AddPct(currentAbsorb, -auraAbsorbMod)), both AFTER this
        // handler. What reaches Spread is therefore not guaranteed to be a multiple, and
        // the leftover of the division there is absorbed without being handed out: at most
        // others - 1 points per hit.
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
        // Take ownership of the list before dealing anything. Unit::DealDamage below can
        // come back through Unit::CalcAbsorbResist and into Absorb() when an ally carries a
        // damage split aura cast by this same victim; Absorb() would then reassign the
        // member vector under the loop that walks it. The local copy is stable, and the
        // member is left empty for that re-entry to fill.
        std::vector<ObjectGuid> bearers;
        bearers.swap(_bearers);
        uint64 others = uint64(bearers.size()) - 1;
        uint64 portion = uint64(amount) / others;
        if (!portion)
            return;
        ObjectGuid victimGuid = target->GetGUID();
        ObjectGuid casterGuid = caster->GetGUID();
        SpellSchoolMask school = carrier->GetSchoolMask();
        uint64 handedOut = 0;
        for (ObjectGuid guid : bearers)
        {
            if (guid == victimGuid)
                continue;
            Unit* ally = ObjectAccessor::GetUnit(*target, guid);
            if (!ally || !ally->IsAlive() || !ally->IsInWorld())
                continue;
            // Already mitigated on the original victim: deal it raw, self-inflicted, so
            // it cannot be absorbed a second time nor re-enter this handler.
            // No combat log packet on purpose: 706083 lands with target 56
            // TARGET_UNIT_CASTER_AREA_RAID, so this loop runs up to 39 times per absorbed
            // hit, and Unit::SendSpellNonMeleeDamageLog ends on SendMessageToSet(&data,
            // true), i.e. one broadcast to every player in visibility range each time. The
            // transfer stays visible through the health bar it takes off.
            uint32 dealt = Unit::DealDamage(ally, ally, uint32(portion), nullptr, DOT, school, carrier, false);
            handedOut += dealt;
        }
        uint64 credit = handedOut / uint64(bearers.size());
        if (!credit)
            return;
        // Every bearer banks the same share of every transfer, so each aura ends the
        // duration holding "the total damage shared split evenly amongst all allies".
        for (ObjectGuid guid : bearers)
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
        // logged out (GetCaster() is then nullptr) or simply stands on another map, the
        // payout still happens, self cast: 707601 takes target 21 TARGET_UNIT_TARGET_ALLY,
        // and a unit is its own valid assist target. IsInWorld() alone would not do:
        // a Chronomancer who changed map is still in world, and the cast would be thrown
        // from there and silently lost. Same test as VastInfiniteBearers above.
        Unit* healer = caster && caster->IsInWorld() && caster->GetMap() == target->GetMap() ? caster : target;
        // 707601 carries DieSides 1 (measured in Spell.dbc, field 74), and
        // SpellEffectInfo::CalcValue adds that roll on top of the base points supplied
        // here: the heal lands one point above the banked credit.
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

    // Effects 1 and 2 are constants: EffectRealPointsPerLevel is 0.0 and EffectDieSides is
    // 1 for both (measured in Spell.dbc), so CalcValue always returns 35 and 50. Read them
    // once instead of at every hit taken: the aura lasts 120 s (DurationIndex 4) and this
    // handler runs for every single damage event on the bearer, and CalcValue calls
    // sScriptMgr->ModifySpellEffectBaseValue each time it is asked.
    int32 _floorPct = 0;
    int32 _cutPct = 0;
    bool _thresholdsRead = false;

    void ReadThresholds()
    {
        if (_thresholdsRead)
            return;
        Unit* caster = GetCaster();
        _floorPct = std::clamp(GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster), 0, 100);
        _cutPct = std::clamp(GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster), 0, 100);
        _thresholdsRead = true;
    }

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
        ReadThresholds();
        bool heavy = uint64(hit) * 100 > maximum * uint64(TimeguardHeavyHitPct);
        uint64 remaining = target->GetHealth() > hit ? uint64(target->GetHealth()) - hit : 0;
        // Read as "the hit leaves them under the floor", which also covers a bearer who
        // was already under it. The tooltip's own wording does not separate the two, and
        // this is the reading a guard is for.
        bool lethal = remaining * 100 < maximum * uint64(uint32(_floorPct));
        if (!heavy && !lethal)
            return;
        amount = uint32(ScaleDown(hit, uint32(_cutPct)));
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
//
// KNOWN AND ACCEPTED: Keep Accelerating / Resilience propagate Accelerated Recovery by
// recasting it (AscensionChronomancerTime.cpp, SpreadRecovery), and only shorten the copy
// AFTER the cast returns, i.e. after this hook has already run. Such a copy therefore pays
// the full instant heal even though it will tick fewer times. Telling a propagated copy
// from a fresh cast needs a marker set by the propagating side, which is not in this file;
// nothing here invents one. The cap below only protects against an application that is
// already short when the aura is applied.
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
        // AuraEffect::GetTotalTicks() divides the aura's MAXIMUM duration by the amplitude,
        // so it announces the nominal 15 ticks even for an application that will not last
        // that long. Pay for what this application can actually deliver.
        Aura* aura = GetAura();
        int32 amplitude = effect->GetAmplitude();
        if (aura && amplitude > 0 && aura->GetDuration() > 0 && aura->GetDuration() < aura->GetMaxDuration())
            ticks = std::min(ticks, aura->GetDuration() / amplitude);
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
        // Two known and accepted gaps with the tooltip, both measured, neither corrected
        // here because no number says what the intent is: 803706 carries DieSides 1
        // (Spell.dbc field 74) and SpellEffectInfo::CalcValue adds that roll on top of the
        // base points given here, so the strike lands one point high; and the 30% is taken
        // from damage->GetDamage(), which is already mitigated on the victim, then goes
        // through resistance and absorption again on its way out.
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
        // caster's outgoing modifiers. Running them again would count them twice, and the
        // attribute below is what prevents it.
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        // Belt and braces only: measured in Spell.dbc (EffectBonusMultiplier, fields
        // 229-231), both spells already carry 0.0 on all three effects. This line changes
        // nothing today; it keeps the contract true if the client rows ever change.
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
