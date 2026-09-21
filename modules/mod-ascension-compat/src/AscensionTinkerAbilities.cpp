/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionSpellSafe.h"
#include "AscensionTinker.h"
#include "AscensionTinkerData.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
namespace
{
using namespace AscensionTinker;
// Snipe has no spell_ranks chain, so Named() cannot walk it: GetFirstSpellInChain
// returns each rank unchanged. These are the eight ranks read in Spell.dbc under the
// name "Snipe", all with category 723, effect 121 and spell levels 13, 17, 25, 33, 41,
// 49, 57 and 65. 560014, also named "Snipe" but with rank "NPC", is the movement slow
// helper that 800345's description points at; it is deliberately excluded.
bool Snipe(SpellInfo const* info)
{
    if (!info)
        return false;
    for (uint32 id : {800345u, 502500u, 502501u, 502502u, 502503u, 502504u, 502505u, 502506u})
        if (info->Id == id)
            return true;
    return false;
}
// EventMap::EventId is a uint16, so every timer the class schedules with a spell id is
// really kept under id & 0xFFFF: State().timers holds 801816 as 15384, 705810 as 50450,
// and so on. Two ids that agree modulo 65536 would silently share one cooldown. This new
// slot is therefore declared truncated on purpose and checked, at compile time, against
// every other id the Tinker schedules today. The list is kept by hand: a timer added
// anywhere in the class must be added here as well, or the static_assert below stops
// proving anything. The slots in use are listed by
//   /usr/bin/grep -rn "timers\.\|Chance(" AscensionTinker*.cpp
constexpr uint32 TinkerTimerIds[] = {801816, 503534, 560785, 560786, 561267, 572367,
                                     680975, 705803, 705810, 705815, 806627, 806629,
                                     806758, 807499};
constexpr bool TinkerTimerSlotIsFree(uint16 slot)
{
    for (uint32 id : TinkerTimerIds)
        if (uint16(id) == slot)
            return false;
    return true;
}
constexpr uint16 MedicalOperativeCooldown = uint16(556500);
static_assert(TinkerTimerSlotIsFree(MedicalOperativeCooldown),
    "Medical Operative would share a truncated EventMap slot with another Tinker timer");
bool Select(uint32 id, SpellInfo const* info)
{
    switch (id)
    {
        case 707250: case 707261: case 653273:
            return Named(info,500549) || (info && info->Id == 500213);
        case 707272: return Named(info,801005);
        case 537247: return Named(info,500235);
        case 503553: return Any(info,{801707,802684});
        case 680998: return info && info->Id == 500213;
        case 681245: return info && info->Id == 504594;
        default: return false;
    }
}
void Snapshot(Player* player, Spell* spell)
{
    if (spell->IsTriggered())
        return;
    for (uint32 id : TinkerFinite)
        if (Select(id,spell->GetSpellInfo()))
            if (Aura* aura = player->GetAura(id))
            {
                if (!aura->GetScriptValue(Scrap))
                    aura->SetScriptValue(Scrap,++State(player).sequence);
                spell->SetScriptValue(id,aura->GetScriptValue(Scrap));
            }
}
void Finish(Player* player, Spell* spell)
{
    for (uint32 id : TinkerFinite)
        if (uint64 generation = spell->GetScriptValue(id))
        {
            // The children calculate their damage during the channel. Keep the
            // selected modifiers until its owner-side aura ends, then spend once.
            if (spell->GetSpellInfo()->Id == 500213)
                if (Aura* channel = player->GetAura(500213,player->GetGUID()))
                {
                    channel->SetScriptValue(id,generation);
                    continue;
                }
            Spend(player,id,generation);
        }
}
void Counter(Player* player, uint32 stack, uint32 buff)
{
    Cast(player,player,stack);
    if (Count(player,stack) >= 4)
    {
        player->RemoveAurasDueToSpell(stack);
        Grant(player,buff);
    }
}
class tinker_spells : public AllSpellScript
{
public:
    tinker_spells() : AllSpellScript("tinker_spells",
        {ALLSPELLHOOK_ON_SPELL_CHECK_CAST,ALLSPELLHOOK_ON_BEFORE_EFFECTS,ALLSPELLHOOK_ON_CAST,
         ALLSPELLHOOK_ON_CRIT_CHANCE,ALLSPELLHOOK_ON_HIT_RESULT}) { }
    void OnSpellCheckCast(Spell* spell, bool, SpellCastResult& result) override
    {
        Player* player = Owner(spell->GetCaster());
        auto* info = spell->GetSpellInfo();
        if (!player || player != spell->GetCaster() || info->SpellFamilyName != 34 || spell->IsTriggered() ||
            result != SPELL_CAST_OK)
            return;
        if (info->Id == Mechsuit && !Count(player,Scrap))
            result = SPELL_FAILED_NO_POWER;
        if ((info->Id == 500213 || Any(info,{801387,801389,805372})) && !player->HasAura(Mechsuit))
            result = SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
        if (info->Id == 504594 && !player->HasAura(681245))
            result = SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
    }
    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (player == caster && player && info->SpellFamilyName == 34)
        {
            Snapshot(player,spell);
            if (!spell->IsTriggered())
                for (uint32 module : TinkerModules)
                    if (info->Id == module)
                        ActivateModule(player,info->Id);
        }
    }
    void OnSpellCritChance(Spell* spell, Unit* target, float& chance) override
    {
        Player* player = Owner(spell->GetCaster());
        auto* info = spell->GetSpellInfo();
        if (!player || !target || info->SpellFamilyName != 34)
            return;
        if ((Named(info,805351) && target->GetCreatureType() == CREATURE_TYPE_MECHANICAL) || info->Id == 801745 ||
            (info->Id == 500220 && player->HasAura(707244) && target->GetAuraOfRankedSpell(500232,player->GetGUID())) ||
            (Named(info,680196) && player->HasAura(560791) && Nanobots(player,target)))
            chance = 100;
    }
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = Owner(caster);
        if (!player || player != caster || info->SpellFamilyName != 34 || spell->IsTriggered())
            return;
        uint32 id = info->Id;
        Unit* target = spell->m_targets.GetUnitTarget();
        if (id == Mechsuit && player->HasAura(Mechsuit))
        {
            if (player->HasAura(807635))
            {
                Cast(player,player,504811);
                if (AuraEffect* regeneration = player->GetAuraEffect(807635,EFFECT_0))
                    regeneration->ResetPeriodic(true);
            }
            if (player->HasAura(503569))
                Cast(player,player,504749);
        }
        Finish(player,spell);
        bool shot = Named(info,500549);
        bool sticky = Named(info,500232);
        bool rocket = Named(info,500235);
        bool bomb = Named(info,801005);
        if (player->HasAura(92141) && (shot || sticky))
            Resource(player,Scrap,shot ? 3 : 10);
        if (NotifySpellAttack(player,info,target))
            for (Creature* device : Devices(player))
                device->AI()->SetGUID(target->GetGUID(),1);
        if (shot)
        {
            if (player->HasAura(707249))
                Counter(player,707251,707250);
            if (player->HasAura(572545) && player->HasAura(653232))
                Counter(player,653282,653273);
            if (player->HasAura(707277))
                for (Creature* device : Devices(player))
                    if (Permanent(device->GetEntry()) || Turret(device->GetEntry()))
                        Cast(player,device,707278);
        }
        if ((shot || Named(info,805351)) && player->HasAura(705786))
            PetCast(player,nullptr,705787);
        if (MechAbility(info) && player->HasAura(707395))
            PetCast(player,nullptr,805519);
        if ((shot || MechAbility(info)) && player->HasAura(520022))
            for (Creature* device : Devices(player))
                if (Turret(device->GetEntry()))
                {
                    Cast(player,device,578323);
                    // Sans 578323 dans le DBC la charge ne peut pas s'appliquer non plus :
                    // un seuil de 0 ferait partir la decharge a chaque coup.
                    if (uint32 charges = AscensionSpellSafe::StackAmount(578323, 0);
                        charges && Count(device,578323) >= charges)
                    {
                        device->RemoveAurasDueToSpell(578323);
                        Cast(device,target,578335);
                    }
                }
        if (sticky)
        {
            if (player->HasAura(707260))
                Grant(player,707261,2);
            if (player->HasAura(807388) && player->HasAura(Mechsuit))
                Grant(player,680998);
            if (player->HasAura(706695))
                PetCast(player,nullptr,706698,true);
        }
        if (id == 500535 && player->HasAura(707271))
        {
            Reduce(player,801005,INT32_MAX);
            Grant(player,707272);
        }
        if (bomb && spell->GetScriptValue(707272))
        {
            Reduce(player,801005,INT32_MAX);
            if (player->HasAura(707273))
                Reduce(player,500232,std::abs(Amount(707274)));
        }
        if (bomb && player->HasAura(707237))
            PetCast(player,target,707238,true);
        if (rocket && player->HasAura(707259))
            PetCast(player,target,Highest(player,500235),true);
        if (bomb || rocket)
            for (Creature* device : Devices(player))
                if (device->GetEntry() == 467073)
                {
                    Position position = device->GetPosition();
                    for (uint8 n = 0; n < 3; ++n)
                        Summon(player,target,500535,&position);
                    break;
                }
        if (id == 802052 && player->HasAura(300636))
            Summon(player,target,802477);
        if (Any(info,{801707,802684}) && Chance(player,705803))
            player->RestoreSpellChargeCategory(13,1);
        if (Any(info,{504527,504594}) && player->HasAura(807500))
            for (uint32 root : {801387,801389,805372})
                Reduce(player,root,INT32_MAX);
        if (id == 504594)
            for (Creature* device : Devices(player))
                Cast(player,device,505161);
        if (Build(info))
        {
            Reduce(player,500236,std::abs(Amount(807225)));
            if (info->SpellFamilyFlags & flag96(0,512,0))
                if (player->HasAura(560782))
                    Reduce(player,560744,std::abs(Amount(560783)));
            // Turbo Inventor: 560794 carries the amount, the duration and the five
            // stack ceiling. The Repair Shot that spends them is in OnSpellHitResult.
            if (player->HasAura(560795))
                Cast(player,player,560794);
        }
        if ((id == 800349 || id == 806757) && player->HasAura(806758))
            Summon(player,target,806760);
        if (id == 500249)
        {
            ObjectGuid ownerGuid = player->GetGUID();
            ObjectGuid targetGuid = target ? target->GetGUID() : State(player).focus;
            State(player).scheduler.Schedule(3000ms,[ownerGuid,targetGuid](TaskContext)
            {
                if (Player* owner = ObjectAccessor::FindPlayer(ownerGuid))
                    if (Unit* enemy = ObjectAccessor::GetUnit(*owner,targetGuid))
                        PetCast(owner,enemy,500579,true);
            });
        }
        Refresh(player);
    }
    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32 healing, bool critical) override
    {
        Player* player = Owner(spell->GetCaster());
        auto* info = spell->GetSpellInfo();
        if (!player || !target || info->SpellFamilyName != 34 || miss != SPELL_MISS_NONE)
            return;
        // Repair player targets carrying the old permanent beacon marker; owned devices keep their guard.
        if (healing && target->IsPlayer() && Any(info, {801707, 529288}))
            target->RemoveAurasDueToSpell(560711);
        if (damage && Named(info,801005) && player->HasAura(805314))
            if (Aura* tracer = target->GetAura(653247,player->GetGUID()); tracer && tracer->GetStackAmount() >= 10)
                Cast(player,target,803438);
        if (Nanobots(player,target) && player->HasAura(560734) &&
            Any(info,{801809,801709,502537,801808,803552}))
            Cast(player,target,560736);
        if (healing && Named(info,680196))
        {
            if (critical && player->HasAura(805306))
                for (Unit* ally : Allies(player,target,Radius(706255),AscensionSpellSafe::MaxAffectedTargets(706255,1)))
                    Copy(player,ally,706255,CalculatePct(healing,15));
            if (player->HasAura(560787))
                for (Creature* device : Devices(player))
                    for (Unit* ally : Allies(player,device,Radius(681513),1))
                        Cast(device,ally,681513);
        }
        if (healing && Named(info,801707) && player->HasAura(524834))
            if (Creature* device = target->ToCreature(); device && Owned(player,device))
                Overcharge(player,device);
        bool brilliance = spell->GetScriptValue(653273) != 0;
        if (info->Id == 500577)
            if (Spell* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                channel && channel->GetSpellInfo()->Id == 500213 && channel->GetScriptValue(653273) &&
                !channel->GetScriptValue(653244) && damage)
            {
                channel->SetScriptValue(653244,1);
                brilliance = true;
            }
        if (damage && brilliance)
            player->CastCustomSpell(653244,SPELLVALUE_BASE_POINT0,
                Amount(653244,0,player) + int32(player->GetTotalAttackPowerValue(RANGED_ATTACK) * .1f),target,true);
        // Talents whose tooltip names the Tinker's own shot or heal. Owner() also answers
        // for a device or a pet, and turrets fire copies of these spells, so the caster
        // has to be the player himself. None of the spells cast below is a Snipe, a Scrap
        // Shot or a Repair Shot, so this block cannot re-enter itself.
        // These four talents could also have been expressed as spell_proc rows: Scrap Shot
        // (500549 and its six ranks) and Repair Shot (801707 and its ten ranks) do carry
        // selectable SpellFamilyFlags, (16,0,256) and (32768,2,0), read in Spell.dbc. Only
        // Snipe is genuinely flagless, (0,0,0) on all eight ranks. The four are kept in C++
        // and in one place because the two Barrel Choke halves need a guard - two talent ids
        // for the same payload, one stack per cast - that no proc mask can express.
        if (spell->GetCaster() != player)
            return;
        // Piercing Impact. 705756 is listed in TinkerDrivers, so ApplyContracts dummies its
        // aura 42 and clears its ProcFlags before LoadSpellProcs runs: in the DBC that proc
        // is armed with ProcFlags 0x4 at 100%, which would fire on every melee auto attack.
        // The tooltip only promises a bleed on Snipe critical strikes, which is this path.
        if (damage && critical && Snipe(info) && player->HasAura(705756))
            Cast(player,target,705757); // 705757 holds the bleed and its ticks.
        if (damage && Shot(info))
        {
            // Shot() is Any(info,{500549,500577}); Gatling Gun 500577 carries exactly the
            // SpellFamilyFlags of the seven Scrap Shot ranks, (16,0,256), so the original
            // data does not tell the two forms apart either and it is included on purpose.
            // Barrel Choke, damage half. The data gives one payload per id - effect 0 of
            // 705762 triggers 560071, effect 0 of 563251 triggers 513297 - but the tooltip
            // of each id promises both halves, and the two tooltips disagree on what the
            // Scrap Shot half does ("damage done" for 705762, "critical strike chance" for
            // 563251) while 560071 is aura 271 MOD_DAMAGE_FROM_CASTER in both cases. Neither
            // id is a rank of the other: they share a name and an icon and spell_ranks knows
            // neither. Either aura therefore grants both halves, once.
            if (player->HasAura(705762) || player->HasAura(563251))
                Cast(player,target,560071); // damage taken from the Tinker, three stacks.
            if (critical && player->HasAura(706813))
                Cast(player,target,503561); // Shattering Shells: 503561 holds the bleed.
        }
        if (healing && Named(info,801707))
        {
            if (critical && player->HasAura(672336))
                Cast(player,target,672335); // Healing Shells: periodic heal on the ally just healed.
            // Barrel Choke, healing half; see the damage half above for the two ids. 513297
            // holds the percentage, the ten seconds and the three stack ceiling, and its
            // class mask (0,2,0) already names Repair Shot. One stack per Repair Shot used,
            // not per ally healed, hence the per-cast marker; it is taken after the heal,
            // so a cast never buffs itself.
            if ((player->HasAura(563251) || player->HasAura(705762)) && !spell->GetScriptValue(563251))
            {
                spell->SetScriptValue(563251,1);
                Cast(player,player,513297);
            }
            // Medical Operative. The talent aura is the passive 556500 ("SLS"); 556502 is its
            // payload - aura 4, four stacks, fifteen seconds - applied by 556500's own effect
            // 1 (aura 42 -> 556502, dormant today: DBC ProcFlags 0 and no spell_proc row,
            // P-045) and removed by 542512's effect 1 (SPELL_EFFECT_REMOVE_AURA -> 556502).
            // Either one on the player means the talent is taken, so both are accepted.
            // Category 13, the Beacons, is read from 542512's first effect,
            // SPELL_EFFECT_ASCENSION_RESTORE_SPELL_CHARGES. 542512 itself is not cast here
            // because its second effect would strip 556502. The lockout is the tooltip's own.
            if (critical && (player->HasAura(556500) || player->HasAura(556502)))
            {
                auto& state = State(player);
                if (!state.timers.HasTimeUntilEvent(MedicalOperativeCooldown))
                {
                    state.timers.ScheduleEvent(MedicalOperativeCooldown,1000ms);
                    player->RestoreSpellChargeCategory(13,1);
                }
            }
            if (player->HasAura(560795))
                if (SpellInfo const* stacking = sSpellMgr->GetSpellInfo(560794); stacking &&
                    stacking->StackAmount && Count(player,560794) >= stacking->StackAmount)
                    // Turbo Inventor spends the whole stack on one Repair Shot. 680360
                    // ("Turbo Nerd") is the spell the data provides for exactly that: effect
                    // 0 is SPELL_EFFECT_REMOVE_AURA on 560794 with TARGET_UNIT_CASTER, effect
                    // 1 triggers 570150 with TARGET_UNIT_TARGET_ALLY. Casting it leaves the
                    // order and the targets in the data instead of transcribing them here.
                    Cast(player,target,680360);
        }
    }
};
class spell_ascension_tinker_ability : public SpellScript
{
    PrepareSpellScript(spell_ascension_tinker_ability);
    bool handled = false;
    void Effect(SpellEffIndex index)
    {
        Player* player = Owner(GetCaster());
        if (!player)
            return;
        uint32 id = GetSpellInfo()->Id;
        auto type = GetSpellInfo()->Effects[index].Effect;
        if (id == 801744)
        {
            PreventHitDefaultEffect(index);
            if (handled)
                return;
            handled = true;
            Position position = GetExplTargetDest() ? GetExplTargetDest()->GetPosition() : player->GetPosition();
            ObjectGuid owner = player->GetGUID();
            uint32 map = player->GetMapId(), phase = player->GetPhaseMask();
            State(player).scheduler.Schedule(Milliseconds(GetSpellInfo()->GetDuration()),
                [owner,position,map,phase](TaskContext)
            {
                Player* caster = ObjectAccessor::FindPlayer(owner);
                if (!caster || !caster->IsAlive() || caster->GetMapId() != map || caster->GetPhaseMask() != phase)
                    return;
                auto targets = Nearby(caster,caster->GetExactDist(&position) + Radius(801744));
                targets.remove_if([caster,position](Unit* enemy)
                {
                    return !caster->IsValidAttackTarget(enemy) || enemy->GetExactDist(&position) > Radius(801744);
                });
                // Lu depuis une tache differee : un 801744 disparu planterait le fil de
                // carte plusieurs secondes apres le lancement. 1 cible par defaut.
                uint32 limit = AscensionSpellSafe::MaxAffectedTargets(801744, 1);
                if (limit && targets.size() > limit)
                    targets.resize(limit);
                for (Unit* enemy : targets)
                    Cast(caster,enemy,801745);
                if (caster->HasAura(806762))
                    caster->CastSpell(position.GetPositionX(),position.GetPositionY(),position.GetPositionZ(),806763,true);
            });
        }
        bool summon = type == SPELL_EFFECT_SUMMON || type == SPELL_EFFECT_SUMMON_OBJECT_WILD || type == SPELL_EFFECT_TRANS_DOOR ||
            id == 500535 || id == 500600;
        if (summon)
        {
            PreventHitDefaultEffect(index);
            if (!handled)
            {
                handled = true;
                Position position = GetExplTargetDest() ? GetExplTargetDest()->GetPosition() : player->GetPosition();
                Summon(player,GetExplTargetUnit(),id,&position);
            }
        }
        if (Any(GetSpellInfo(),{805372}) && type == SPELL_EFFECT_SCRIPT_EFFECT)
        {
            PreventHitDefaultEffect(index);
            if (GetHitUnit())
                PetCast(player,GetHitUnit(),805459);
        }
        if (id == 524835 || id == 800349 || id == 801798)
        {
            PreventHitDefaultEffect(index);
            if (handled)
                return;
            handled = true;
            if (id == 801798)
                Detonate(player);
            else
                for (Creature* device : Devices(player))
                    if (id == 524835)
                        Overcharge(player,device);
                    else if (Turret(device->GetEntry()) && player->IsWithinDistInMap(device,40))
                        Cast(player,device,706692);
        }
        if ((id == 802176 || id == 802236) && GetCaster()->IsCreature())
            if (Creature* device = GetCaster()->ToCreature(); Owned(player,device) && device->GetEntry() == 50300)
                device->DespawnOrUnsummon(100ms);
    }
    void Hit()
    {
        Player* player = Owner(GetCaster());
        Unit* target = GetHitUnit();
        if (!player || !target)
            return;
        uint32 id = GetSpellInfo()->Id;
        if ((Named(GetSpellInfo(),801009) || id == 801982 || id == 802477) && player->HasAura(707262))
        {
            uint32 enemies = 0;
            for (auto const& hit : *GetSpell()->GetUniqueTargetInfo())
                if (hit.effectMask & 1)
                    ++enemies;
            if (Creature* device = GetCaster()->ToCreature(); device && Owned(player,device))
                enemies = device->AI()->GetData(2);
            if (enemies == 1)
                SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(),Amount(707262)));
        }
        if (id == 806074)
            if (Creature* device = GetCaster()->ToCreature(); device && Owned(player,device))
                SetHitDamage(int32(int64(GetHitDamage()) * device->AI()->GetData(3) / 1000));
        if (Named(GetSpellInfo(),801009) && player->HasAura(803074) && !handled)
        {
            handled = true;
            Position position = target->GetPosition();
            Summon(player,target,850020,&position);
        }
    }
    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_ascension_tinker_ability::Effect,EFFECT_ALL,SPELL_EFFECT_ANY);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_tinker_ability::Effect,EFFECT_ALL,SPELL_EFFECT_ANY);
        OnHit += SpellHitFn(spell_ascension_tinker_ability::Hit);
    }
};
}
void AddSC_AscensionTinkerAbilities()
{
    new tinker_spells();
    RegisterSpellScript(spell_ascension_tinker_ability);
}
