/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionStarcaller.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace AscensionStarcaller
{
namespace
{
std::unordered_map<ObjectGuid, std::unique_ptr<StarcallerState>> states;
std::mutex stateMutex;
} // namespace
Player* Owner(Unit const* unit)
{
    Player* player = unit ? const_cast<Unit*>(unit)->ToPlayer() : nullptr;
    return player && player->getClass() == 26 ? player : nullptr;
}
StarcallerState& State(Player* player)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    // The map is locked for the lookup only: the caller then reads and writes the state with no
    // lock held. Kept by pointer, the state itself never moves, so an insert for another player
    // rehashing the map cannot leave that caller writing into freed memory.
    return *states.try_emplace(player->GetGUID(), std::make_unique<StarcallerState>()).first->second;
}
bool Named(SpellInfo const* info, uint32 root)
{
    return info && sSpellMgr->GetFirstSpellInChain(info->Id) == sSpellMgr->GetFirstSpellInChain(root);
}
bool Any(SpellInfo const* info, std::initializer_list<uint32> roots)
{
    for (uint32 root : roots)
        if (Named(info, root))
            return true;
    return false;
}
bool Lunar(SpellInfo const* info, Player* player)
{
    return Any(info, {575030, 575039, 800370, 574328}) || (Named(info, 680220) && player->HasAura(92134)) ||
           (Named(info, 801978) && player->HasAura(500205));
}
bool Derived(SpellInfo const* info)
{
    return info && Any(info, {801129, 807672, 524703, 804736, 707759, 805357, 954791});
}
bool Burning(Unit const* unit)
{
    if (!unit)
        return false;
    for (auto const& pair : unit->GetAppliedAuras())
        if (SpellInfo const* info = pair.second->GetBase()->GetSpellInfo();
            !pair.second->IsPositive() && (info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE) &&
            (info->HasAura(SPELL_AURA_PERIODIC_DAMAGE) || info->HasAura(SPELL_AURA_PERIODIC_LEECH)))
            return true;
    return false;
}
uint32 Count(Unit const* unit, uint32 id)
{
    Aura const* aura = unit ? unit->GetAura(id) : nullptr;
    return aura ? aura->GetStackAmount() : 0;
}
uint32 MaxPhase(Player* player)
{
    // Bright Moon raises this cap via a SPELLMOD_MAX_AURA_STACKS modifier on 802985;
    // consult the mod-adjusted value instead of hardcoding the DBC's base of 4.
    SpellInfo const* info = sSpellMgr->GetSpellInfo(802985);
    return info ? info->CalcMaxAuraStacks(player) : 4;
}
int32 Amount(uint32 id, uint8 slot, Unit* caster)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    return info ? info->Effects[slot].CalcValue(caster) : 0;
}
void Cast(Unit* caster, Unit* target, uint32 id)
{
    if (caster && target && target->IsAlive() && caster->IsInWorld())
        caster->CastSpell(target, id, true);
}
void Copy(Unit* caster, Unit* target, uint32 id, uint32 amount)
{
    if (caster && target && target->IsAlive() && amount)
        caster->CastCustomSpell(id, SPELLVALUE_BASE_POINT0, int32(std::min<uint32>(amount, INT32_MAX)), target, true);
}
void Mana(Player* player, uint32 amount, uint32 spell)
{
    if (player && amount)
        player->EnergizeBySpell(player, spell, int32(std::min<uint32>(amount, INT32_MAX)), POWER_MANA);
}
bool DelayDamage(Player* player, uint32 amount)
{
    if (!player || !amount)
        return false;
    Aura* aura = player->GetAura(954791);
    if (!aura)
    {
        Cast(player, player, 954791);
        aura = player->GetAura(954791);
    }
    if (!aura)
        return false;
    State(player).stagger += amount;
    aura->SetDuration(aura->GetMaxDuration()); // Preserve the already scheduled next tick.
    return true;
}
void PayDelayedDamage(Player* player, uint32 ticks)
{
    auto& state = State(player);
    uint64 payment = (state.stagger + std::max(1u, ticks) - 1) / std::max(1u, ticks);
    state.stagger -= payment; // Reserve before damage or removal callbacks can re-enter.
    SpellInfo const* info = sSpellMgr->GetSpellInfo(954791);
    while (payment && player->IsAlive())
    {
        uint32 amount = uint32(std::min<uint64>(INT32_MAX, payment));
        payment -= amount;
        uint32 dealt = Unit::DealDamage(player, player, amount, nullptr, DOT, SPELL_SCHOOL_MASK_ARCANE, info, false);
        player->SendSpellNonMeleeDamageLog(player, info, dealt, SPELL_SCHOOL_MASK_ARCANE, 0, 0, false, 0);
    }
    if (!player->IsAlive())
        state.stagger = 0;
}
void FinishCharge(Player* player)
{
    auto& state = State(player);
    state.chargeDistance = std::min(50.0f, player->GetDistance2d(state.chargeX, state.chargeY));
    state.chargeReady = player->HasAura(680215);
    state.chargeAura = 0;
}
std::list<Unit*> Nearby(Unit* center, float range)
{
    std::list<Unit*> units;
    if (!center || !center->IsInWorld())
        return units;
    Acore::AnyUnitInObjectRangeCheck check(center, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> search(center, units, check);
    Cell::VisitObjects(center, search, range);
    units.remove_if([center](Unit* unit) { return !unit->IsAlive() || !center->InSamePhase(unit); });
    units.sort([center](Unit* a, Unit* b) {
        float first = center->GetDistance(a), second = center->GetDistance(b);
        return first == second ? a->GetGUID() < b->GetGUID() : first < second;
    });
    return units;
}
void Reduce(Player* player, uint32 root, int32 milliseconds)
{
    for (auto const& pair : player->GetSpellMap())
        if (player->HasSpell(pair.first) && Named(sSpellMgr->GetSpellInfo(pair.first), root))
        {
            if (milliseconds == INT32_MAX)
                player->RemoveSpellCooldown(pair.first, true);
            else
                player->ModifySpellCooldown(pair.first, -milliseconds);
        }
}
void ReduceHealing(Player* player, uint32 percent)
{
    for (auto const& pair : player->GetSpellMap())
        if (player->HasSpell(pair.first))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(pair.first);
                info && info->SpellFamilyName == 32 &&
                (info->HasEffect(SPELL_EFFECT_HEAL) || info->HasAura(SPELL_AURA_PERIODIC_HEAL)))
                player->ModifySpellCooldown(pair.first,
                                            -int32(CalculatePct(player->GetSpellCooldownDelay(pair.first), percent)));
}
uint32 Highest(Player* player, uint32 root)
{
    uint32 id = root;
    for (auto const& pair : player->GetSpellMap())
        if (player->HasSpell(pair.first) && Named(sSpellMgr->GetSpellInfo(pair.first), root) &&
            sSpellMgr->GetSpellInfo(pair.first)->SpellLevel >= sSpellMgr->GetSpellInfo(id)->SpellLevel)
            id = pair.first;
    return id;
}
void Replace(Player* player, uint32 root, uint32 replacement)
{
    // Player::SetTemporarySpellReplacement returns silently when the player does not know
    // the replacement. Moonblade (801125), Starfire Barrage (802682) and Drawstring of
    // Elune (801975) are taught by no table, no trainer and no LEARN_SPELL effect, so
    // every call here used to be a silent no-op (P-044).
    // Teach the target before walking the spell map, drop the superseded one after.
    uint32 previous = 0;
    for (auto const& pair : player->GetSpellMap())
        if (player->HasSpell(pair.first) && Named(sSpellMgr->GetSpellInfo(pair.first), root))
            if (uint32 current = player->GetTemporarySpellReplacement(pair.first); current != pair.first)
                previous = current;
    if (replacement && player->GetSpellMap().find(replacement) == player->GetSpellMap().end())
        player->learnSpell(replacement, true);
    for (auto const& pair : player->GetSpellMap())
        if (player->HasSpell(pair.first) && Named(sSpellMgr->GetSpellInfo(pair.first), root))
            player->SetTemporarySpellReplacement(pair.first, replacement);
    // onlyTemporary: an independently owned permanent copy is left alone.
    if (previous && previous != replacement)
        player->removeSpell(previous, SPEC_MASK_ALL, true);
}
bool Chance(Player* player, uint32 id, uint32 cooldown, float bonus)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    if (!player->HasAura(id) || !info || State(player).timers.HasTimeUntilEvent(id) ||
        !roll_chance_f(std::clamp(float(info->ProcChance) + bonus, 0.0f, 100.0f)))
        return false;
    if (cooldown)
        State(player).timers.ScheduleEvent(id, Milliseconds(cooldown));
    return true;
}
void GainPhase(Player* player, uint32 count)
{
    if (!player || !player->IsAlive() || !player->HasAura(524781))
        return;
    uint32 max = MaxPhase(player);
    uint32 before = Count(player, 802985);
    if (Aura* aura = player->AddAura(802985, player))
        aura->SetStackAmount(std::min(max, before + count));
    if (Count(player, 802985) == max && !player->HasAura(704519))
        Cast(player, player, 704519);
}
void Stars(Player* player, Unit* target, uint32 count)
{
    if (!player || !target || !player->IsValidAttackTarget(target))
        return;
    for (uint32 i = 0; i < std::min(32u, count); ++i)
        Cast(player, target, 804378); // Native stack cap retains the caster's stack-capacity modifiers.
}
void StartConsume(Player* player, Unit* target)
{
    if (player && target && target->HasAura(804378, player->GetGUID()))
        Cast(player, target, 804716);
}
bool Consume(Player* player, Unit* target)
{
    if (!player || !target || !player->IsValidAttackTarget(target))
        return false;
    Aura* stars = target->GetAura(804378, player->GetGUID());
    if (!stars || !stars->GetStackAmount())
        return false;
    // Reserve before the triggered hit: a miss still consumes this exact star, never another caster's.
    stars->ModStackAmount(-1);
    Cast(player, target, 804995);
    float effectiveness = (player->HasAura(807659) ? 1.5f : 1) * (player->HasAura(805524) ? 1.5f : 1);
    // Celestial Shot (574348) authors its "increases the effectiveness of consuming Scattered Stars
    // by 40%" as two native spell modifiers: SPELLMOD_EFFECT1 on 804995, the hit, which stays native
    // and needs nothing here; and SPELLMOD_EFFECT2 on effect 1 of 804994 "Scattered Star CD
    // Reduction", the mana share this line pays. 804994 is never cast -- the module only reads its
    // effects -- so the literal .08f that used to stand here bypassed that modifier and half the
    // talent was dead. Reading the share through the modifier chain applies it.
    //
    // FOUR Spell.dbc records, not three, carry an aura 107/108 with SPELLMOD_ALL_EFFECTS (op 8) or
    // SPELLMOD_EFFECT2 (op 12) on family 32 whose EffectSpellClassMask (fields 122+3e, P-064)
    // reaches this effect (804994 SpellFamilyFlags, fields 209-211, are 0/65536/1048576):
    // 574348 (+40, EFFECT2, mask 0/0/1048576, left native), 574360 "Aspect of the Moonwell"
    // (+100, EFFECT2, mask 0/0/1048576) and 807659 "Dancing in the Moonlight" (+50, ALL_EFFECTS,
    // mask 0/67584/0), both dummied in ApplyContracts, and 805850 "Celestial Glaives" /
    // "SpecializationSLS" (+50, EFFECT2, mask 0/0/1048576).
    //
    // WHICH BUCKET A MODIFIER LANDS IN DECIDES WHETHER IT ADDS OR MULTIPLIES.
    // Unit::ApplyEffectModifiers makes two separate Player::ApplySpellMod calls, SPELLMOD_ALL_EFFECTS
    // then SPELLMOD_EFFECT2, so those two buckets MULTIPLY one another. Inside a single bucket
    // Player::ApplySpellMod accumulates percentages ADDITIVELY (`totalmul += CalculatePct(1.0f,
    // mod->value)`); only SPELLMOD_DAMAGE and SPELLMOD_DOT take the multiplicative branch. Hence:
    //  - 807659 is ALL_EFFECTS, a bucket of its own, so its reimplemented x1.5 in `effectiveness`
    //    multiplies exactly as the engine would have -- correct where it stands;
    //  - 574360 is EFFECT2, the SAME bucket as 574348, so its +100 must be ADDED to that +40, never
    //    multiplied by it. It is added below as one more `base`. The x2 that used to close this
    //    expression gave 8 * 1.40 * 2 = 22.4 where the DBC authors 8 * (1 + .40 + 1.00) = 19.2.
    //    Abilities.cpp keeps a separate x2 on 804995: that one reimplements 574360 effect 0, a
    //    SPELLMOD_DAMAGE, which the engine really does multiply -- correct, and not a double count,
    //    because effect 0 (mask 8388608/0/0) reaches 804995 and effect 1 reaches 804994, not both.
    // 805850 is left native on purpose: nothing can grant it -- no SkillLineAbility.dbc row, no
    // other Spell.dbc record naming it in any of the 234 fields, and no spell_script_names,
    // spell_proc, spell_linked_spell, npc_trainer or item_template row in acore_world -- and its own
    // tooltip (upstream issue 3580, closed, filed against the Chronomancer) authors intellect from
    // strength, nothing about Scattered Stars, so there is no share to guess. Were it ever granted
    // it would join the same additive bucket: 8 * (1 + .40 + 1.00 + .50) = 23.2.
    //
    // The share is read as a float on purpose: SpellEffectInfo::CalcValue truncates its own result
    // with `return int32(value)`, which would turn the 11.2 the +40% authors into 11.
    // Unit::ApplyEffectModifiers is the very call CalcValue makes for those modifiers, so going
    // through it keeps the fraction. Two measured consequences of taking that route rather than the
    // literal:
    //  - it skips sScriptMgr->ModifySpellEffectBaseValue, an identity for this pair today: 804994
    //    has no StarcallerCoefficients row (only 804995 has one), no ScalingBaseSpells row, and no
    //    other branch of the module's hooks names it;
    //  - Player::ApplySpellMod redirects to m_spellModTakingSpell when the player is mid-cast, and
    //    Player::ApplyModToSpell then registers the carrying aura in Spell::m_appliedMods, which is
    //    how the proc system drops charges. Inert today: the four modifiers above all have
    //    ProcCharges = 0 (Spell.dbc field 36), so there is no charge to consume. The .08f literal
    //    had neither behaviour; this is written down so the next reader does not rediscover it.
    //
    // EXPOSURE, stated plainly because what follows is a measurement and not a barrier: this call
    // runs the WHOLE realm's modifier chain, not just the module's. SpellInfo::IsAffected opens on
    // `if (!familyName) return true;`, so a modifier carried by a family 0 spell reaches this effect
    // whatever its mask. Spell.dbc holds 22 such records with aura 107/108 and op 8 or 12, the worst
    // being 968469-968476 "Primary Stat Food Buff", ADD_FLAT_MODIFIER SPELLMOD_EFFECT2 of
    // +15/+20/+25 with an EMPTY mask: ApplySpellMod ends on `basevalue = (basevalue * totalmul) +
    // totalflat`, so a single +20 would take the share from 11.2 to 31.2. None of the 22 is
    // reachable by a Starcaller today -- measured: no SkillLineAbility.dbc row, no Spell.dbc record
    // naming one in any of the 234 fields except 281180 -> 281182 and 707100's EffectMiscValue, and
    // zero rows in item_template.spellid_*, spell_linked_spell, spell_script_names, spell_proc and
    // npc_trainer. No ceiling written anywhere in the DBC would separate a legitimate share from
    // such a runaway, so none is invented here: the clamp below is the one real invariant, an
    // energize can never hand back more than the player's own mana pool. Revisit if any of the 22
    // ever becomes grantable.
    SpellInfo const* energize = sSpellMgr->GetSpellInfo(804994);
    float base = energize ? float(energize->Effects[EFFECT_1].CalcValue(nullptr)) : 8.0f;
    float share = energize ? player->ApplyEffectModifiers(energize, EFFECT_1, base) : base;
    if (player->HasAura(574360))
        share += base; // Same additive SPELLMOD_EFFECT2 bucket as 574348, never a factor on top.
    double mana = double(player->GetMaxPower(POWER_MANA)) * double(std::clamp(share, 0.0f, 100.0f)) /
                  100.0 * effectiveness;
    Mana(player, uint32(std::clamp(mana, 0.0, double(INT32_MAX))));
    for (uint32 helper : {804994, 504024, 706573})
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(helper))
            for (auto const& effect : info->Effects)
                if (effect.Effect == 165)
                    Reduce(player, effect.MiscValue, int32(-effect.CalcValue(player) * effectiveness));
    auto& state = State(player);
    if (player->HasAura(300252) && ++state.secondMoon == 10)
        state.secondMoon = 0, Cast(player, target, 520422);
    if (player->HasAura(704765) && ++state.cascade == 8)
        state.cascade = 0, Cast(player, player, 707425);
    if (Chance(player, 802680))
        Cast(player, player, 802681);
    if (player->HasAura(802203))
    {
        uint32 remaining = std::max(1u, sSpellMgr->GetSpellInfo(801401)->MaxAffectedTargets);
        for (Unit* ally : Nearby(target, 15))
            if (player->IsValidAssistTarget(ally) && (ally == player || player->IsInRaidWith(ally)))
            {
                Cast(player, ally, 801401);
                if (!--remaining)
                    break;
            }
    }
    return true;
}
void MarkedHeal(Player* player)
{
    for (Unit* ally : Nearby(player, 100))
        if (player->IsValidAssistTarget(ally) && ally->HasAura(574154, player->GetGUID()))
            Cast(player, ally, 704232);
}
void Aspect(Player* player, Unit* target, uint32 damage, bool forced)
{
    float bonus = player->HasAura(806739) ? float(Amount(806739)) : 0.0f;
    for (uint32 id : {801128, 805356, 801123, 800510, 803887, 803888})
    {
        if (!player->HasAura(id) || (!forced && !Chance(player, id, 0, id == 801128 ? bonus : 0)))
            continue;
        Stars(player, target);
        if (id == 801128)
        {
            uint32 value = damage + (player->HasAura(680777) ? player->GetMaxPower(POWER_MANA) / 100 : 0);
            for (uint32 i = 0; i < 3; ++i)
                Copy(player, target, 801129, value);
        }
        else if (id == 805356)
            Copy(player, target, 805357, damage / 4);
        else if (id == 801123)
        {
            if (!player->HasAura(300772))
                Cast(player, target, 801124);
        }
        else
        {
            Copy(player, target, 800507, std::max(0, Amount(id, 1, player)));
        }
        break;
    }
}
void Refresh(Player* player)
{
    auto& state = State(player);
    if (state.refreshing || !player->IsAlive())
        return;
    state.refreshing = true;
    if (player->HasSpell(800386) && !player->HasAura(524781))
        Cast(player, player, 524781);
    player->RemoveAurasDueToSpell(706301);
    auto scale = [player](uint32 id, bool active, std::initializer_list<int32> amounts) {
        if (!active)
        {
            player->RemoveAurasDueToSpell(id);
            return;
        }
        Aura* aura = player->GetAura(id);
        if (!aura)
            aura = player->AddAura(id, player);
        uint8 slot = 0;
        for (int32 amount : amounts)
        {
            if (aura && aura->GetEffect(SpellEffIndex(slot)) &&
                aura->GetEffect(SpellEffIndex(slot))->GetAmount() != amount)
                aura->GetEffect(SpellEffIndex(slot))->ChangeAmount(amount);
            ++slot;
        }
    };
    uint32 mana = player->GetPower(POWER_MANA);
    scale(100250, player->HasAura(92132), {int32(player->GetMaxPower(POWER_MANA) / 10)});
    // Local reconstruction: ratings 0.5%, block value 2% of current mana.
    scale(801148, player->HasAura(574349), {int32(mana / 200), int32(mana / 200), int32(mana / 50)});
    scale(680790, player->HasAura(680789) && player->GetHealthPct() < 35, {-10});
    scale(706436, player->HasAura(704787) && player->GetHealthPct() < 50, {10, 10});
    scale(561096, player->HasAura(561022) && player->HasAura(805356), {3, 3});
    for (WeaponAttackType type : {BASE_ATTACK, OFF_ATTACK, RANGED_ATTACK})
        player->UpdateDamagePhysical(type);
    if (Count(player, 802985) < MaxPhase(player))
        player->RemoveAurasDueToSpell(704519);
    if (!player->HasAura(300252))
        state.secondMoon = 0;
    if (!player->HasAura(704765))
        state.cascade = 0;
    if (!player->HasAura(680742))
        state.spentPercent = 0;
    if (!player->HasAura(680215))
        state.chargeReady = false;
    state.refreshing = false;
}
} // namespace AscensionStarcaller
namespace
{
class starcaller_player : public PlayerScript
{
  public:
    starcaller_player()
        : PlayerScript("starcaller_player", {PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_BEFORE_LOGOUT, PLAYERHOOK_ON_LOGOUT})
    {
    }
    void OnPlayerBeforeLogout(Player* player) override
    {
        using namespace AscensionStarcaller;
        if (Owner(player))
        {
            PayDelayedDamage(player);
            player->RemoveAurasDueToSpell(954791);
        }
    }
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        using namespace AscensionStarcaller;
        if (!Owner(player))
            return;
        auto& state = State(player);
        state.clock += diff;
        state.timers.Update(diff);
        state.scheduler.Update(diff);
        while (state.timers.ExecuteEvent())
        {
        }
        if (!state.timers.HasTimeUntilEvent(1))
        {
            Refresh(player);
            state.timers.ScheduleEvent(1, 250ms);
            for (auto it = state.counters.begin(); it != state.counters.end();)
                if (it->second <= state.clock)
                    it = state.counters.erase(it);
                else
                    ++it;
        }
    }
    void OnPlayerLogout(Player* player) override
    {
        using namespace AscensionStarcaller;
        if (Owner(player))
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            states.erase(player->GetGUID());
        }
    }
};
} // namespace
void AddSC_AscensionStarcaller()
{
    new starcaller_player();
}
