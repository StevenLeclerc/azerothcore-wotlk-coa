/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionSpellSafe.h"
#include "AscensionXoroth.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
namespace
{
using namespace AscensionXoroth;
constexpr uint32 selected[] = {680197, 680203, 681184, 520021, 802617, 802618, 524913, 680729, 712294};
bool Select(SpellInfo const* info, uint32 id)
{
    switch (id)
    {
    case 680197:
    case 802618:
        return Infernal(info);
    case 680203:
        return Sever(info);
    case 681184:
        return Named(info, 805555);
    case 520021:
        return Named(info, 806965);
    case 802617:
        return Named(info, 800168);
    case 524913:
        return Named(info, 800168) || Named(info, 802581);
    case 680729:
        return Named(info, 804353);
    case 712294:
        return Named(info, 504581);
    default:
        return false;
    }
}
bool RefundableMiss(Player* player, Spell* spell)
{
    if (auto info = spell->GetSpellInfo(); info && info->Id == 520292)
        return State(player).bellowsResult == 1;
    bool hostile = false, refundable = true;
    for (auto const& hit : *spell->GetUniqueTargetInfo())
        if (hit.targetGUID != player->GetGUID())
            if (Unit* victim = ObjectAccessor::GetUnit(*player, hit.targetGUID); victim && player->IsHostileTo(victim))
            {
                hostile = true;
                if (hit.missCondition != SPELL_MISS_MISS && hit.missCondition != SPELL_MISS_DODGE &&
                    hit.missCondition != SPELL_MISS_PARRY)
                    refundable = false;
            }
    return hostile && refundable;
}
void RecordBellowsResult(Player* player, SpellInfo const* info, Unit* target, uint8 miss)
{
    if ((info->Id == 520292 || info->Id == 520857) && player->IsHostileTo(target))
    {
        State(player).bellowsResult |= 1;
        if (miss != SPELL_MISS_MISS && miss != SPELL_MISS_DODGE && miss != SPELL_MISS_PARRY)
            State(player).bellowsResult |= 2;
    }
}
void BeginResources(Player* player, Spell* spell)
{
    auto info = spell->GetSpellInfo();
    auto& state = State(player);
    spell->SetScriptValue(500907, state.fire);
    spell->SetScriptValue(800998, state.blood);
    if (!spell->IsTriggered())
    {
        if (info->Id == 520292)
            state.bellowsResult = 0;
        state.fire = Spender(info) ? Count(player, 500906) : 0;
        spell->SetScriptValue(500906, state.fire);
        if (state.fire)
            player->RemoveAurasDueToSpell(500906);
        state.blood = (info->Id == 520294 || info->Id == 805679) ? Count(player, 800999) : 0;
        spell->SetScriptValue(800999, state.blood);
        if (state.blood)
            player->RemoveAurasDueToSpell(800999);
        for (uint32 id : selected)
            if (Select(info, id))
                if (Aura* aura = player->GetAura(id))
                {
                    spell->SetScriptValue(id, aura->GetScriptValue(500906));
                    if (id == 680203 && aura->GetStackAmount() < 3)
                        spell->SetScriptValue(id, 0);
                }
        if (spell->GetScriptValue(524913))
            for (auto const& target : *spell->GetUniqueTargetInfo())
                if (Unit* unit = ObjectAccessor::GetUnit(*player, target.targetGUID);
                    unit && unit->IsControlledByPlayer() && player->IsHostileTo(unit))
                    spell->SetScriptValue(524914, 1);
    }
    else
        spell->SetScriptValue(500906, state.fire);
}
void ConsumeSelected(Player* player, Spell* spell)
{
    for (uint32 sid : selected)
        if (uint64 sequence = spell->GetScriptValue(sid))
            if (Aura* aura = player->GetAura(sid); aura && aura->GetScriptValue(500906) == sequence)
            {
                if (sid == 681184 || sid == 524913)
                {
                    uint64 left = aura->GetScriptValue(sid);
                    if (sid == 524913 && spell->GetScriptValue(524914))
                        left = 1;
                    if (left > 1)
                        aura->SetScriptValue(sid, left - 1);
                    else
                        aura->Remove();
                }
                else
                    aura->Remove();
            }
}
class xoroth_casts : public AllSpellScript
{
  public:
    xoroth_casts()
        : AllSpellScript("xoroth_casts",
                         {ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_BEFORE_EFFECTS, ALLSPELLHOOK_ON_CAST,
                          ALLSPELLHOOK_ON_CALCULATED_TARGET, ALLSPELLHOOK_ON_HIT_RESULT,
                          ALLSPELLHOOK_ON_CALC_MAX_DURATION, ALLSPELLHOOK_ON_CRIT_CHANCE,
                          ALLSPELLHOOK_ON_SUCCESSFUL_INTERRUPT, ALLSPELLHOOK_ON_INTERRUPT_DURATION})
    {
    }
    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player || spell->IsTriggered() || result != SPELL_CAST_OK)
            return;
        auto info = spell->GetSpellInfo();
        if (Spender(info) && !Count(player, 500906))
            result = SPELL_FAILED_NO_POWER;
        if ((info->Id == 520294 || info->Id == 805679) && !Count(player, 800999))
            result = SPELL_FAILED_NO_POWER;
        if ((info->Id == 804702 || info->Id == 804704) && (!player->HasAura(804703) || player->HasAura(807248)))
            result = SPELL_FAILED_CASTER_AURASTATE;
        // Impcaller 706755 transforms Call: Hellfire Imp into Impcaller (TalentReplacements row
        // {706755, 804883, 707666}). A client cast of 804883 is already redirected to 707666; this refuses
        // the casts that bypass that redirect (playerbots, scripts), and only while the slot shows
        // Impcaller, so a Call button the client still shows is never refused.
        if (info->Id == 804883 && player->GetTemporarySpellReplacement(804883) != 804883)
            result = SPELL_FAILED_NOT_KNOWN;
        if (info->Id == 801042)
        {
            Unit* unit = spell->m_targets.GetUnitTarget();
            Creature* corpse = unit ? unit->ToCreature() : nullptr;
            if (player->IsInCombat())
                result = SPELL_FAILED_AFFECTING_COMBAT;
            else if (!corpse || corpse->getDeathState() != DeathState::Corpse ||
                     (corpse->GetCreatureType() != CREATURE_TYPE_HUMANOID &&
                      corpse->GetCreatureType() != CREATURE_TYPE_BEAST &&
                      corpse->GetCreatureType() != CREATURE_TYPE_DEMON))
                result = SPELL_FAILED_BAD_TARGETS;
        }
    }
    void OnCalcMaxDuration(Aura const* aura, int32& duration) override
    {
        Player* player = Owner(aura->GetCaster());
        if (!player)
            return;
        uint32 fire = State(player).fire, id = aura->GetId();
        if (id == 801064) // 500906 e2 (dummied) carries $s3 per Demonfire; Suffusion of Essence adds its EFFECT3 mod
            duration = int32(fire) * Amount(500906, EFFECT_2, player);
        if (id == 801017)
            duration *= 1 + fire;
        if (id == 803889)
            duration = int32(duration * (1 + .2f * fire));
        if (id == 805746 && player->HasAura(704964))
            duration = int32(duration * (1 + Amount(704964, 1) / 100.0f));
        if (id == 801052)
            duration = int32(duration * State(player).unleash);
        // Darkrider's native +$s2% only reaches family 23: Greater Mark of Rivendare is family 36.
        if (id == 803730 && duration > 0 && player->HasAura(704980))
            duration = int32(duration * (1 + Amount(704980, EFFECT_1) / 100.0f));
    }
    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        // Deflection consumes exactly one recipient's Sigil after native target resolution.
        for (TargetInfo& target : *spell->GetUniqueTargetInfo())
            if (target.missCondition == SPELL_MISS_DEFLECT)
                if (Unit* unit = ObjectAccessor::GetUnit(*caster, target.targetGUID))
                    unit->RemoveAurasDueToSpell(302555);
        Player* player = Owner(caster);
        if (!player || info->SpellFamilyName != 23)
            return;
        BeginResources(player, spell);
        // Chainwhip interrupts through its silence (aura 27), never through effect 68, so the core's
        // OnSpellSuccessfulInterrupt never runs. Note the target casting a silenceable spell now
        // (same rule as HandleAuraModSilence); OnSpellHitResult refunds once the silence landed.
        if (Named(info, 800081) && !spell->IsTriggered())
            for (TargetInfo const& hit : *spell->GetUniqueTargetInfo())
                if (!spell->GetScriptValue(800836))
                    if (Unit* unit = ObjectAccessor::GetUnit(*player, hit.targetGUID); unit && unit != player)
                        for (uint32 i = CURRENT_MELEE_SPELL; i < CURRENT_MAX_SPELL; ++i)
                            if (Spell* current = unit->GetCurrentSpell(CurrentSpellTypes(i));
                                current && current->m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                            {
                                spell->SetScriptValue(800836, unit->GetGUID().GetRawValue());
                                break;
                            }
        if (info->Id == 801037)
        {
            Unit* primary = spell->m_targets.GetUnitTarget();
            uint32 count = 1;
            for (Unit* target : Nearby(primary, 8))
                if (target != primary && player->IsValidAttackTarget(target) && primary->IsWithinLOSInMap(target))
                {
                    spell->AddUnitTargetForScript(target, 1);
                    if (++count == 10)
                        break;
                }
        }
        if (player->HasAura(704185) && (Named(info, 500020) || Named(info, 800081)))
        {
            Unit* primary = spell->m_targets.GetUnitTarget();
            uint32 count = 0;
            for (Unit* target : Nearby(primary, 8))
                if (target != primary && player->IsValidAttackTarget(target) && player->IsWithinLOSInMap(target))
                {
                    spell->AddUnitTargetForScript(target, 7);
                    if (++count == uint32(std::max(0, Amount(704185, 1))))
                        break;
                }
        }
    }
    void OnSpellCritChance(Spell* spell, Unit*, float& chance) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player)
            return;
        // Chop Shop (704953): "Each stack of Demon's Blood now increases the critical strike chance of
        // Brimstone Bludgeon and Meatsaw by $s2%." $s2 is this talent's own Effects[1] value; Demon's
        // Blood is the 800999 stack. Sever() covers Sever and its Brimstone Bludgeon transform alike.
        // Known and deliberate gap, on the Meatsaw side only: when Dread's 712294 is up, Auras.cpp
        // does Replace(player, 800340, 504581) and the button casts Annihilation 504581, which is not
        // in Meatsaw's spell_ranks chain (800340, 501508, 501509, 501510, 578119), so Named() is false
        // and this bonus silently stops applying. Nothing written says Chop Shop should follow it: the
        // official tooltip quoted by upstream issue 486 names "Brimstone Bludgeon and Meatsaw", and no
        // tracker entry mentions 504581 with Chop Shop. Adding `|| Named(info, 504581)` would be an
        // extrapolation, so it is left out and recorded here instead.
        if (player->HasAura(704953))
            if (SpellInfo const* info = spell->GetSpellInfo(); Sever(info) || Named(info, 800340))
                chance += float(Count(player, 800999)) * float(Amount(704953, 1));
        if (spell->GetScriptValue(524913) || spell->GetScriptValue(802618))
            chance = 100;
    }
    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& result) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player || result.damage <= 0 || !target)
            return;
        auto info = spell->GetSpellInfo();
        uint32 fire = uint32(spell->GetScriptValue(500906));
        float factor = 1;
        if (Named(info, 800168))
            factor *= 1 + .20f * fire;
        if (info->Id == 524897)
            factor *= 1 + float(fire);
        if (Named(info, 801059))
            factor *= 1 + .5f * fire;
        if (Named(info, 802581))
            factor *= 1 + .18f * fire;
        if (Named(info, 806965))
            factor *= 1 + .2f * fire;
        if (info->Id == 804702 || info->Id == 804704)
            factor *= 1 + Count(player, 804787) * Amount(804787, 1) / 100.0f;
        if (spell->GetScriptValue(524913) || spell->GetScriptValue(802618))
            result.crit = true;
        if (spell->GetScriptValue(524913))
            factor *= target->IsControlledByPlayer() ? 1.25f : 2.0f;
        // Annihilation: "+$712402s1% damage if you are above 75% health or below 35% health". The DBC
        // grants it through 712294's periodic triggers, which the contract removes.
        if (Named(info, 504581))
        {
            float health = player->GetHealthPct();
            if (health > 75.0f)
                factor *= 1 + Amount(712402, EFFECT_0, player) / 100.0f;
            else if (health < 35.0f)
                factor *= 1 + Amount(712320, EFFECT_0, player) / 100.0f;
        }
        result.damage = int32(std::min<double>(INT32_MAX / 2, double(result.damage) * factor));
        result.damageBeforeTakenMods =
            int32(std::min<double>(INT32_MAX / 2, double(result.damageBeforeTakenMods) * factor));
    }
    void OnSpellInterruptDuration(Spell* spell, Unit*, int32& duration) override
    {
        if (Player* player = Owner(spell->GetCaster()); player && spell->GetSpellInfo()->Id == 802857)
            duration = int32(duration * State(player).unleash);
    }
    void OnSpellSuccessfulInterrupt(Spell* spell, Unit*) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player || !Named(spell->GetSpellInfo(), 800081) || spell->GetScriptValue(800835))
            return;
        spell->SetScriptValue(800835, 1);
        Reduce(player, 800081, int32(spell->GetSpellInfo()->RecoveryTime * Amount(800835) / 100));
    }
    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool critical) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player || !target)
            return;
        auto info = spell->GetSpellInfo();
        RecordBellowsResult(player, info, target, miss);
        if (miss != SPELL_MISS_NONE)
            return;
        // Chainwhip: "If you successfully interrupt an enemy, this will refund $800835s1% of the cooldown."
        if (Named(info, 800081) && !spell->IsTriggered() && !spell->GetScriptValue(800835) &&
            spell->GetScriptValue(800836) == target->GetGUID().GetRawValue() &&
            target->HasAuraEffect(info->Id, EFFECT_2, player->GetGUID()))
        {
            spell->SetScriptValue(800835, 1);
            int32 cooldown = int32(info->RecoveryTime);
            player->ApplySpellMod(info->Id, SPELLMOD_COOLDOWN, cooldown);
            Reduce(player, 800081, cooldown * std::max(0, Amount(800835)) / 100);
        }
        uint32 id = info->Id, fire = spell->GetScriptValue(500906);
        if (id == 520292 || id == 520857)
            Reduce(player, 706756, std::abs(Amount(704541)));
        if (spell->IsTriggered())
            return;
        if (Named(info, 806965))
            if (Aura* aura = target->GetAura(id, player->GetGUID()))
            {
                aura->SetScriptValue(802619, 0);
                if (AuraEffect* e = aura->GetEffect(EFFECT_0))
                    e->ChangeAmount(int32(e->GetAmount() * (1 + .2f * fire)));
            }
        if (id == 803334)
            if (AuraEffect* e = target->GetAuraEffect(id, EFFECT_0, player->GetGUID()))
                e->ChangeAmount(-4 * int32(fire));
        if (!damage)
            return;
        if (Named(info, 805555) && player->HasAura(680199))
        {
            Copy(player, player, 681206, damage / 6);
            Cast(player, player, 680203);
        }
        if (Sever(info) && spell->GetScriptValue(680203))
        {
            uint32 n = 0;
            for (Unit* enemy : Nearby(target, 10))
                if (player->IsValidAttackTarget(enemy))
                {
                    Copy(player, enemy, 680204, damage * 2);
                    if (++n == 5)
                        break;
                }
        }
        if (Sever(info) && player->HasAura(300375))
            Cast(player, target, 681268);
        if (player->HasAura(300388) && (Named(info, 800340) || Named(info, 804353)))
            Cast(player, target, 800341);
        if (Named(info, 800340) && player->HasAura(704952))
            Cast(player, player, 553279);
        if (player->HasAura(705015) && (Named(info, 805555) || Named(info, 800340)))
            Cast(player, target, 680979);
        if (Spender(info) && player->HasAura(705018))
            Cast(player, target, 705019);
        if (Named(info, 802581))
            Cast(player, target, 803240);
        if (Named(info, 800081) && player->HasAura(704987))
            Cast(player, target, 806219);
        if (Infernal(info) && player->HasAura(704997))
        {
            bool marked = false;
            for (auto const& pair : target->GetAppliedAuras())
                if (pair.second->GetBase()->GetCasterGUID() == player->GetGUID())
                    if (Named(pair.second->GetBase()->GetSpellInfo(), 801059) ||
                        (player->HasAura(704993) && Named(pair.second->GetBase()->GetSpellInfo(), 806965)))
                        marked = true;
            if (marked)
                Cast(player, target, 801037);
        }
        if (Named(info, 804353) && critical && player->HasAura(706563))
        {
            Cast(player, player, 804886);
            Gain(player, 1);
        }
        // Carver (707390) is granted by the FirstCriticalDamagingHit rows of AscensionCustomResourceData.h
        // alone: a Gain here as well gave 3 Demonfire per Gore crit instead of 2.
        if (Named(info, 805671))
        {
            if (player->HasActiveSpell(SPELL_DEMONIC_VISAGE))
                Cast(player, target, SPELL_DEMONIC_VISAGE_SLOW);
            if (player->HasAura(704958))
                Spread(player, target);
            if (Chance(player, 805703, 0, Count(player, 500906) * 5))
                Gain(player, 1);
        }
        if (Named(info, 800168) && fire == 6 && player->HasAura(704973))
        {
            Copy(player, target, 704974, damage * Amount(704973) / 100);
            if (player->HasAura(807144))
            {
                Reduce(player, 500904, std::abs(Amount(807165)));
                Reduce(player, 520294, std::abs(Amount(807165)));
            }
        }
        if (Named(info, 800168) && critical && player->HasAura(500551))
            Cast(player, player->GetPet(), 500552);
        if (Infernal(info) && player->HasAura(802615) && !spell->GetScriptValue(802615))
        {
            spell->SetScriptValue(802615, 1);
            Cast(player, player, 802617);
        }
        if (Named(info, 804353) && spell->GetScriptValue(680729))
        {
            bool old = State(player).curseShield;
            State(player).curseShield = true;
            Cast(player, player, 805680);
            State(player).curseShield = old;
        }
        if (!spell->GetScriptValue(706331) && Spender(info) && player->HasAura(706331))
        {
            spell->SetScriptValue(706331, 1);
            player->RestoreSpellCharge(Highest(player, 805555));
        }
        if (!spell->GetScriptValue(704957) && Spender(info) && player->HasAura(704957))
        {
            spell->SetScriptValue(704957, 1);
            Cast(player, player, 681184);
        }
        if (Named(info, 800168) && player->HasAura(802610))
            if (Pet* pet = player->GetPet(); pet && pet->GetEntry() == 510100 && pet->HasAura(802602))
                pet->CastCustomSpell(704974, SPELLVALUE_BASE_POINT0, int32(damage * .5f), target, true);
        if (spell->GetScriptValue(524913) && target->IsControlledByPlayer())
            spell->SetScriptValue(524914, 1);
    }
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = Owner(caster);
        if (!player || info->SpellFamilyName != 23)
            return;
        if (!spell->IsTriggered())
        {
            uint32 id = info->Id, fire = spell->GetScriptValue(500906);
            Unit* target = spell->m_targets.GetUnitTarget();
            bool refunded = fire && RefundableMiss(player, spell);
            if (refunded)
                Gain(player, fire);
            if (fire && !refunded)
            {
                if (fire >= 4 && player->HasAura(805706) && target)
                    if (Aura* apocalypse = target->GetAura(560817, player->GetGUID()))
                        apocalypse->SetDuration(apocalypse->GetMaxDuration()); // Claws of Hell.
                if (player->HasAura(520008) && fire >= 4)
                    Gain(player, 1);
                if (player->HasAura(704973) && fire == 6 && (Named(info, 800168) || id == 803334))
                    Gain(player, 2);
                if (player->HasAura(560633) && fire == 6)
                    Unleash(player, player, .5f);
                if (player->HasAura(802345))
                {
                    HealInfo heal(player, player,
                                  uint32(player->GetMaxHealth() * fire * (player->HasAura(704182) ? .015f : .01f)),
                                  info, info->GetSchoolMask());
                    player->HealBySpell(heal);
                }
                if (player->HasAura(705001))
                    Cast(player, player, 705002);
            }
            if (id == 520292)
                Reduce(player, id, 3000 * fire);
            if (id == 802342)
                Cast(player, player, 801017);
            if (id == 805679)
                Cast(player, player, 805680);
            if (id == 801042)
                for (Unit* summon : Nearby(player, 100))
                    if (summon->GetOwnerGUID() == player->GetGUID() &&
                        (summon->GetEntry() == 50301 || summon->GetEntry() == 50375))
                        player->AddAura(id, summon);
            if (id == 807247)
                Cast(player, player, 807248);
            if (id == 801061)
                for (Unit* ally : Nearby(player, 20))
                    if (ally == player || player->IsInRaidWith(ally))
                        Cast(player, ally, 302555);
            if (id == 520295)
                Cast(player, player, 520752);
            if (id == 804169)
                Cast(player, player, 680729);
            if (id == 801063 && player->HasAura(300395))
                Cast(player, player, 300394);
            if (id == SPELL_UNLEASH_PESTILENCE)
            {
                if (player->HasAura(SPELL_WARPATH))
                    Cast(player, player, SPELL_WARPATH_PROTECTION);
                // Hellbreaker (680216): "Casting Unleash Pestilence now removes all active movement
                // impairing effects from you." Its own DBC effect is a PROC_TRIGGER_SPELL towards
                // 680217 with ProcFlags 0 and no spell_proc row, and - being SpellFamilyName 21, not
                // 23 - ApplyContracts never sees it either, so nothing ever cast 680217. 680217 is
                // DISPEL_MECHANIC of MECHANIC_SNARE plus MECHANIC_IMMUNITY to snare and root.
                if (player->HasAura(680216))
                    Cast(player, player, 680217);
                Unleash(player, player);
                if (player->HasAura(704961))
                    if (Pet* pet = player->GetPet(); pet && pet->GetEntry() == 510100)
                        Unleash(player, pet, 1, true);
                if (Chance(player, 520296, 30000))
                    Reduce(player, SPELL_UNLEASH_PESTILENCE, INT32_MAX);
            }
            if (Pestilence(id) && player->HasAura(300398) && !State(player).timers.HasTimeUntilEvent(300398))
                if (Pet* pet = player->GetPet(); pet && pet->GetEntry() == 510100)
                {
                    for (uint32 sid : {802603, 802604, 802605, 806962})
                        pet->RemoveAurasDueToSpell(sid);
                    Cast(player, pet, id == 801053 ? 802605 : id == 802344 ? 802603 : id == 804786 ? 806962 : 802604);
                    State(player).timers.ScheduleEvent(300398, 10s);
                }
            if (Infernal(info) && player->HasAura(707666))
            {
                // Impcaller's imp is 807699, whose e0 Imp Gang Boss 706754 raises by 2 (mask 32).
                uint32 imps = uint32(std::clamp(Amount(807699, EFFECT_0, player), 1, 12));
                for (uint32 n = 0; n < imps; ++n)
                    Summon(player, 50301, player->GetNearPosition(2, 0),
                           uint32(AscensionSpellSafe::Duration(807699, 30000)));
            }
            if (Named(info, 801059) && fire == 6 && player->HasAura(704452))
                Cast(player, player, 801006);
            if (Named(info, 800340) && Chance(player, 704975))
                Cast(player, player, 707376);
            if (Sever(info))
            {
                // Brimstone Bludgeon: "Unleashes your active Pestilence to trigger a powerful effect."
                if (Named(info, 520005))
                    Unleash(player, target && target->IsInWorld() ? target : player);
                if (player->HasAura(680199))
                    Reduce(player, id, Named(info, 520005) ? -12000 : -2000);
                if (player->HasAura(705005))
                    Cast(player, player, 680197);
                if (player->HasAura(705008))
                    Cast(player, player, 680992);
                if (player->HasAura(680199) && target)
                {
                    ObjectGuid owner = player->GetGUID(), victim = target->GetGUID();
                    uint32 sid = id;
                    State(player).scheduler.Schedule(1s, [owner, victim, sid](TaskContext) {
                        if (Player* p = ObjectAccessor::FindConnectedPlayer(owner))
                            if (Unit* unit = ObjectAccessor::GetUnit(*p, victim))
                                if (p->IsAlive() && p->IsValidAttackTarget(unit))
                                    Cast(p, unit, sid);
                    });
                }
            }
            if (Named(info, 805555) && player->HasAura(705008))
                if (Aura* aura = player->GetAura(680992))
                    aura->SetDuration(std::min(aura->GetMaxDuration(), aura->GetDuration() + std::abs(Amount(681313))));
            if (Named(info, 800168) && fire == 6 && !refunded)
            {
                if (player->HasAura(802615))
                    Cast(player, player, 802618);
                if (player->HasAura(805693))
                    Cast(player, player, 520021);
                if (player->HasAura(802610))
                    Reduce(player, 802602, std::abs(Amount(802611)));
            }
            if (id == 804703 && player->HasAura(706295))
            {
                Cast(player, player, 706384);
                // Mounted Champion "now has a $/1000;s2 sec cooldown": the core drops a flat SPELLMOD_COOLDOWN
                // on a spell without base cooldown, and the client knows none, hence the packet.
                if (uint32 cooldown = uint32(std::max(0, Amount(706295, EFFECT_1))))
                {
                    player->AddSpellCooldown(id, 0, cooldown);
                    WorldPacket data;
                    player->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, id, cooldown);
                    player->SendDirectMessage(&data);
                }
            }
            // Hellfire Bellows: the 15 sec cooldown exists only server side (contract); the client's DBC has none.
            if (id == 520292)
                if (uint32 left = player->GetSpellCooldownDelay(id))
                {
                    WorldPacket data;
                    player->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, id, left);
                    player->SendDirectMessage(&data);
                }
            if ((Named(info, 500020) || Named(info, 800081)) && player->HasAura(807336))
            {
                Cast(player, target, 807522);
                Cast(player, player, 808007);
            }
            if (Named(info, 805671) && player->HasAura(707515) && player->HasAura(805696))
                Reduce(player, 805671, INT32_MAX);
            ConsumeSelected(player, spell);
        }
        State(player).fire = spell->GetScriptValue(500907);
        State(player).blood = spell->GetScriptValue(800998);
    }
};
class spell_ascension_xoroth_ability : public SpellScript
{
    PrepareSpellScript(spell_ascension_xoroth_ability);
    bool summoned = false;
    void Effect(SpellEffIndex index)
    {
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetSpellInfo()->Id;
        if (Named(GetSpellInfo(), 500904) && index == EFFECT_2)
        {
            if (GetSpellInfo()->Effects[index].Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE)
                return; // native forwarding is preserved and tested, including die-side encoding
        }
        if (index != EFFECT_0)
            return;
        if (id == 801042)
        {
            PreventHitDefaultEffect(index);
            // Effect est branche sur OnEffectHit AUSSI, qui n'est pas un
            // crochet a cible (SpellScript::IsInTargetHook) : GetHitUnit() y
            // journalise une erreur et rend nullptr, que le `if` ecarte deja.
            if (Unit* unit = IsInTargetHook() ? GetHitUnit() : nullptr)
                if (Creature* corpse = unit->ToCreature(); corpse && corpse->getDeathState() == DeathState::Corpse)
                {
                    corpse->RemoveCorpse();
                    player->EnergizeBySpell(player, id, 200, POWER_RAGE);
                }
            return;
        }
        if (id != 524897 && id != 805966 && id != 807699 && id != 704247 && id != 706756 && id != 804774)
            return;
        PreventHitDefaultEffect(index);
        if (summoned)
            return;
        summoned = true;
        Position position = GetExplTargetDest() ? GetExplTargetDest()->GetPosition() : player->GetNearPosition(2, 0);
        uint32 entry = id == 704247 ? 50375 : id == 706756 ? 51323 : id == 804774 ? 50268 : 50301;
        uint32 count = id == 524897 ? 1 + uint32(GetSpell()->GetScriptValue(500906)) : std::max(1, GetEffectValue());
        uint32 duration = std::max(1000, GetSpellInfo()->GetDuration());
        for (uint32 n = 0; n < std::min(12u, count); ++n)
            Summon(player, entry, position, duration);
    }
    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_ascension_xoroth_ability::Effect, EFFECT_ALL, SPELL_EFFECT_ANY);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_xoroth_ability::Effect, EFFECT_ALL, SPELL_EFFECT_ANY);
    }
};
} // namespace
void AddSC_AscensionXorothAbilities()
{
    new xoroth_casts();
    RegisterSpellScript(spell_ascension_xoroth_ability);
}
