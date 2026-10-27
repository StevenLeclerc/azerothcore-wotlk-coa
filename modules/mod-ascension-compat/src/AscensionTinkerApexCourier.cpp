/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
//
// Tinker Apex (class 28, specialization 102), PTR only: Kesh'vaar, the Apex Quartermaster. He comes to a human
// Apex at levels 1, 10, 20, 30, 40, 50 and 60, visible to that player alone, and hands over the tier's armor
// through an instant quest (QUEST_FLAGS_AUTOCOMPLETE). The Apex Supply Beacon calls him back at will, and he
// redelivers any piece of a rewarded tier that is lost. Ids are frozen by sortie-apex/livreur/plan.json; the
// human reading is docs/CONCEPTION-apex-livreur-equipement.md (sections 4 and 5).
//
// Script names (written by exploitation/apex/apex-livreur-ptr.sql):
//   creature 9120100 npc_apex_quartermaster    gossip, quest chaining, self-dismissal
//   item     9120050 item_apex_supply_beacon   calls the quartermaster (dummy spell 93461 never cast)
//
// Durable state is the quest reward status (character_queststatus_rewarded): nothing new is stored in the base.
// The volatile state (timers, courier GUID) lives in the player's CustomData, touched only from the player's
// map thread (player update, courier AI on the same map, in-place packet handlers) or from the world thread
// while maps are idle (gossip select, PROCESS_THREADUNSAFE): no lock, no mutable global (lesson of P-050),
// except a log-once flag.

#include "AscensionSpecialization.h"

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "CreatureTextMgr.h"
#include "DataMap.h"
#include "Item.h"
#include "ItemScript.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <list>
#include <utility>
#include <vector>

namespace
{
// ---- Constants (plan.json -> cpp.constantes, cpp.temps_ms) -------------------------------------------------
constexpr uint32 SPEC_APEX = 102;
constexpr uint32 NPC_APEX_QUARTERMASTER = 9120100;
constexpr uint32 ITEM_APEX_SUPPLY_BEACON = 9120050;
constexpr uint32 SPELL_ETHEREAL_TELEPORT = 34427;

struct CourierTier
{
    uint8 Level;
    uint32 Quest;
};

// Tier index i whispers creature_text group i on arrival (groups 0-6).
constexpr std::array<CourierTier, 7> TIERS =
{{
    { 1, 26101 }, { 10, 26110 }, { 20, 26120 }, { 30, 26130 }, { 40, 26140 }, { 50, 26150 }, { 60, 26160 },
}};
constexpr uint32 QUEST_LAST_CRATE = 26160;

// must match AscensionCompat.cpp TINKER_APEX_STARTER_ITEMS
constexpr std::array<uint32, 3> STARTER_WEAPONS = { 9120004, 9120005, 9120006 };
constexpr char STARTER_SETTING[] = "core.apex_starter"; // read only, never written here
constexpr uint8 STARTER_WEAPONS_LEVEL = 10;

constexpr uint32 NPC_TEXT_PENDING = 9120100;
constexpr uint32 NPC_TEXT_CLEAR = 9120101;
constexpr uint32 NPC_TEXT_NOT_APEX = 9120102;

constexpr uint8 TALK_FAREWELL = 7;
constexpr uint8 TALK_FAREWELL_FINAL = 8;
constexpr uint8 TALK_REDELIVERED = 9;
constexpr uint8 TALK_BAGS_FULL = 10;
constexpr uint8 TALK_BEACON_CLEAR = 11;

constexpr uint32 GOSSIP_ACTION_REDELIVER = 1;
constexpr uint32 GOSSIP_ACTION_DISMISS = 3;
constexpr char GOSSIP_TEXT_REDELIVER[] = "I lost some of my Apex gear.";
constexpr char GOSSIP_TEXT_DISMISS[] = "Not now, Kesh'vaar.";

constexpr uint32 CHECK_AFTER_LOGIN_MS = 8000;
constexpr uint32 CHECK_AFTER_LEVEL_MS = 3000;
constexpr uint32 CHECK_PERIOD_MS = 10000;
constexpr uint32 COURIER_LIFETIME_MS = 180000;
constexpr uint32 BEACON_COOLDOWN_MS = 60000;
// Redelivery is rate-limited like the beacon: the starter weapons sell for 100 copper and are not unique (D-4), so an
// unlimited "I lost some of my Apex gear" next to a vendor would mint money (review of 27/10).
constexpr uint32 REDELIVER_COOLDOWN_MS = 60000;
constexpr uint32 AUTO_WAIT_INITIAL_MS = 15 * MINUTE * IN_MILLISECONDS;
constexpr uint32 AUTO_WAIT_MAX_MS = 60 * MINUTE * IN_MILLISECONDS;
constexpr float COURIER_MAX_DISTANCE = 80.0f;
constexpr float DUPLICATE_SWEEP_RANGE = 100.0f;
constexpr float SUMMON_DISTANCE = 3.0f;
constexpr float FOLLOW_DISTANCE = 2.5f;
constexpr uint32 COURIER_AI_CHECK_MS = 1000;

constexpr char STATE_KEY[] = "ApexCourier";

// Log once when the data is not there (SQL not applied): no spam every 10 s for each Apex.
std::atomic<bool> MissingQuestLogged{ false };
std::atomic<bool> SummonFailureLogged{ false };

// ---- Per-character state --------------------------------------------------------------------------------------
struct ApexCourierState final : public DataMap::Base
{
    uint32 checkInMs = CHECK_PERIOD_MS;
    uint32 autoBlockMs = 0;
    uint32 autoStepMs = AUTO_WAIT_INITIAL_MS;
    uint32 beaconCooldownMs = 0;
    uint32 redeliverCooldownMs = 0;
    ObjectGuid courier;
};

/// Only for a human Tinker (callers filter first): bots never get a CustomData entry.
ApexCourierState& State(Player* player)
{
    return *player->CustomData.GetDefault<ApexCourierState>(STATE_KEY);
}

bool IsHumanTinker(Player const* player)
{
    return player && player->getClass() == CLASS_TINKER && player->GetSession() && !player->GetSession()->IsBot();
}

// ---- Predicates -----------------------------------------------------------------------------------------------
/// Apex for the courier: no level threshold (tier 1 depends on it), unlike IsTinkerApex in AscensionCompat.cpp.
bool IsCourierApex(Player const* player)
{
    return IsHumanTinker(player) && GetAscensionActiveSpecialization(player) == SPEC_APEX;
}

/// Index of the first tier due (level reached, quest loaded, not rewarded), -1 when none.
int32 PendingTier(Player const* player)
{
    uint8 const level = player->GetLevel();
    for (std::size_t i = 0; i < TIERS.size(); ++i)
    {
        if (TIERS[i].Level > level)
            break;
        if (!sObjectMgr->GetQuestTemplate(TIERS[i].Quest))
        {
            if (!MissingQuestLogged.exchange(true))
                LOG_ERROR("module.ascension_compat", "Apex courier: quest template {} is missing, its tier is skipped "
                          "(exploitation/apex/apex-livreur-ptr.sql not applied?)", TIERS[i].Quest);
            continue;
        }
        if (!player->GetQuestRewardStatus(TIERS[i].Quest))
            return int32(i);
    }
    return -1;
}

/// Reward items of a tier's quest (RewardItemId, non-zero), empty when the quest is not loaded.
std::vector<uint32> TierItems(std::size_t tier)
{
    std::vector<uint32> items;
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(TIERS[tier].Quest))
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            if (quest->RewardItemId[i])
                items.push_back(quest->RewardItemId[i]);
    return items;
}

/// Equipped, in the bags, in the bank (HasItemCount with inBankAlso walks equipment slots first), or waiting in
/// the mailbox (e.g. starter weapons mailed because the bags were full).
bool HoldsItem(Player const* player, uint32 itemId)
{
    if (player->HasItemCount(itemId, 1, true))
        return true;
    for (Mail const* mail : player->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED)
            continue;
        for (MailItemInfo const& info : mail->items)
            if (info.item_template == itemId)
                return true;
    }
    return false;
}

/// Items of rewarded tiers (plus the beacon and the starter weapons) the character no longer holds anywhere.
std::vector<uint32> MissingItems(Player* player)
{
    std::vector<uint32> wanted;
    bool anyRewarded = false;
    for (std::size_t i = 0; i < TIERS.size(); ++i)
    {
        if (!player->GetQuestRewardStatus(TIERS[i].Quest))
            continue;
        anyRewarded = true;
        for (uint32 itemId : TierItems(i))
            wanted.push_back(itemId);
    }
    if (anyRewarded)
        wanted.push_back(ITEM_APEX_SUPPLY_BEACON);
    if (player->GetLevel() >= STARTER_WEAPONS_LEVEL && player->GetPlayerSetting(STARTER_SETTING, 0).value >= 1)
        for (uint32 itemId : STARTER_WEAPONS)
            wanted.push_back(itemId);

    std::vector<uint32> missing;
    for (uint32 itemId : wanted)
        if (sObjectMgr->GetItemTemplate(itemId) && !HoldsItem(player, itemId) &&
            std::find(missing.begin(), missing.end(), itemId) == missing.end())
            missing.push_back(itemId);
    return missing;
}

/// The place allows a visit: open world, standing still enough, nothing in progress.
bool CanReceiveNow(Player const* player)
{
    if (!player->IsInWorld() || !player->IsAlive() || player->IsInCombat())
        return false;
    if (player->IsInFlight() || player->GetVehicle() || player->GetTransport() || player->IsBeingTeleported() ||
        player->duel)
        return false;
    if (player->IsFlying() || player->IsInWater())
        return false;
    return !player->GetMap()->Instanceable();
}

/// The courier this state designates, when it still serves: same map, in world, alive, within 80 yd.
Creature* ValidCourier(Player* player, ApexCourierState const& state)
{
    if (!state.courier)
        return nullptr;
    Creature* courier = ObjectAccessor::GetCreature(*player, state.courier);
    if (!courier || !courier->IsInWorld() || !courier->IsAlive() ||
        !player->IsWithinDistInMap(courier, COURIER_MAX_DISTANCE))
        return nullptr;
    return courier;
}

// ---- Summon ---------------------------------------------------------------------------------------------------
/// Sends every earlier courier of this player away, summons a new one 3 yd in front of the player and records
/// its GUID. Returns false when the core refused the summon (no template, map refusal).
bool Summon(Player* player, ApexCourierState& state, bool byBeacon)
{
    // 1. Duplicates: any quartermaster of this player nearby (an old one after a relog, for instance).
    std::list<Creature*> earlier;
    player->GetCreatureListWithEntryInGrid(earlier, NPC_APEX_QUARTERMASTER, DUPLICATE_SWEEP_RANGE);
    for (Creature* creature : earlier)
        if (TempSummon* summon = creature->ToTempSummon(); summon && summon->GetSummonerGUID() == player->GetGUID())
            summon->DespawnOrUnsummon();
    state.courier.Clear();

    // 2. Position: 3 yd in front of the player, facing the player.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    player->GetClosePoint(x, y, z, 0.0f, SUMMON_DISTANCE, 0.0f);
    Position pos;
    pos.Relocate(x, y, z);
    pos.SetOrientation(pos.GetAngle(player));

    // 3. Visible to the summoner alone (Object.h:639, last argument).
    TempSummon* courier = player->SummonCreature(NPC_APEX_QUARTERMASTER, pos, TEMPSUMMON_TIMED_DESPAWN,
                                                 COURIER_LIFETIME_MS, 0, nullptr, true);
    if (!courier)
    {
        if (!SummonFailureLogged.exchange(true))
            LOG_ERROR("module.ascension_compat", "Apex courier: summon of creature {} failed for {} "
                      "(creature_template missing?), retried at the next check", NPC_APEX_QUARTERMASTER,
                      player->GetName());
        return false;
    }

    // 4. Staging.
    state.courier = courier->GetGUID();
    courier->CastSpell(courier, SPELL_ETHEREAL_TELEPORT, true);
    courier->SetFacingToObject(player);
    courier->GetMotionMaster()->MoveFollow(player, FOLLOW_DISTANCE, PET_FOLLOW_ANGLE);

    // 5. Whisper (creature_text Type 15: only the target receives it).
    int32 const tier = PendingTier(player);
    if (tier >= 0)
        sCreatureTextMgr->SendChat(courier, uint8(tier), player);
    else if (byBeacon)
        sCreatureTextMgr->SendChat(courier, TALK_BEACON_CLEAR, player);

    // 6. Journal: "Apex courier" is the installation proof read in the binary.
    LOG_INFO("module.ascension_compat", "Apex courier summoned for {} (level {}, tier {}, beacon {})",
             player->GetName(), uint32(player->GetLevel()), tier, byBeacon ? 1 : 0);
    return true;
}

// ---- Gossip ---------------------------------------------------------------------------------------------------
/// The quartermaster's menu for its summoner (OnGossipHello, and again after each reward).
void OpenCourierMenu(Player* player, Creature* creature)
{
    TempSummon* summon = creature->ToTempSummon();
    if (!summon || summon->GetSummonerGUID() != player->GetGUID())
    {
        CloseGossipMenuFor(player);
        return;
    }
    if (!IsCourierApex(player))
    {
        ClearGossipMenuFor(player);
        SendGossipMenuFor(player, NPC_TEXT_NOT_APEX, creature->GetGUID());
        return;
    }

    summon->SetTimer(COURIER_LIFETIME_MS);
    ClearGossipMenuFor(player);

    int32 const tier = PendingTier(player);
    if (tier >= 0)
    {
        player->PrepareQuestMenu(creature->GetGUID());
        if (player->GetFreeInventorySpace() < TierItems(std::size_t(tier)).size())
            sCreatureTextMgr->SendChat(creature, TALK_BAGS_FULL, player);
    }
    if (!MissingItems(player).empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_REDELIVER, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_REDELIVER);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_DISMISS, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_DISMISS);
    SendGossipMenuFor(player, tier >= 0 ? NPC_TEXT_PENDING : NPC_TEXT_CLEAR, creature->GetGUID());
}

// ---- Creature -------------------------------------------------------------------------------------------------
class npc_apex_quartermaster : public CreatureScript
{
public:
    npc_apex_quartermaster() : CreatureScript("npc_apex_quartermaster") { }

    struct npc_apex_quartermasterAI : public CreatureAI
    {
        explicit npc_apex_quartermasterAI(Creature* creature) : CreatureAI(creature) { }

        void MoveInLineOfSight(Unit* /*who*/) override { }
        void AttackStart(Unit* /*target*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        // Runs on the courier's map thread; the summoner is read only when on this same map.
        void UpdateAI(uint32 diff) override
        {
            if (_checkTimer > diff)
            {
                _checkTimer -= diff;
                return;
            }
            _checkTimer = COURIER_AI_CHECK_MS;

            TempSummon* summon = me->ToTempSummon();
            Player* player = summon ? ObjectAccessor::GetPlayer(*me, summon->GetSummonerGUID()) : nullptr;
            ApexCourierState const* state = (player && IsHumanTinker(player))
                ? player->CustomData.Get<ApexCourierState>(STATE_KEY) : nullptr;
            if (!player || !player->IsInWorld() || !player->IsAlive() ||
                !me->IsWithinDistInMap(player, COURIER_MAX_DISTANCE) || !state || state->courier != me->GetGUID())
            {
                me->DespawnOrUnsummon();
                return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(player, FOLLOW_DISTANCE, PET_FOLLOW_ANGLE);
        }

        // QuestHandler.cpp: called after the reward, unless a CreatureScript::OnQuestReward returns true (none here).
        void sQuestReward(Player* player, Quest const* quest, uint32 /*opt*/) override
        {
            if (!player || !quest)
                return;
            if (IsCourierApex(player) && PendingTier(player) >= 0)
            {
                OpenCourierMenu(player, me);
                return;
            }
            sCreatureTextMgr->SendChat(me, quest->GetQuestId() == QUEST_LAST_CRATE ? TALK_FAREWELL_FINAL : TALK_FAREWELL,
                                       player);
            me->DespawnOrUnsummon(5s);
        }

    private:
        uint32 _checkTimer = COURIER_AI_CHECK_MS;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_apex_quartermasterAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        OpenCourierMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);
        TempSummon* summon = creature->ToTempSummon();
        if (!summon || summon->GetSummonerGUID() != player->GetGUID() || !IsCourierApex(player))
        {
            CloseGossipMenuFor(player);
            return true;
        }

        switch (action)
        {
            case GOSSIP_ACTION_REDELIVER:
            {
                ApexCourierState& state = State(player);
                if (state.redeliverCooldownMs)
                {
                    ChatHandler(player->GetSession()).SendSysMessage("The quartermaster has nothing more to spare yet.");
                    CloseGossipMenuFor(player);
                    break;
                }
                uint32 given = 0;
                uint32 left = 0;
                for (uint32 itemId : MissingItems(player))
                {
                    ItemPosCountVec destinations;
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, itemId, 1) == EQUIP_ERR_OK)
                        if (Item* item = player->StoreNewItem(destinations, itemId, true))
                        {
                            player->SendNewItem(item, 1, true, false);
                            ++given;
                            continue;
                        }
                    ++left;
                }
                if (given)
                {
                    state.redeliverCooldownMs = REDELIVER_COOLDOWN_MS;
                    sCreatureTextMgr->SendChat(creature, TALK_REDELIVERED, player);
                }
                if (left)
                    sCreatureTextMgr->SendChat(creature, TALK_BAGS_FULL, player);
                LOG_INFO("module.ascension_compat", "Apex courier redelivered {} item(s) to {}", given, player->GetName());
                CloseGossipMenuFor(player);
                break;
            }
            case GOSSIP_ACTION_DISMISS:
                CloseGossipMenuFor(player);
                sCreatureTextMgr->SendChat(creature, TALK_FAREWELL, player);
                creature->DespawnOrUnsummon(3s);
                break;
            default:
                CloseGossipMenuFor(player);
                break;
        }
        return true;
    }
};

// ---- Beacon ---------------------------------------------------------------------------------------------------
class item_apex_supply_beacon : public ItemScript
{
public:
    item_apex_supply_beacon() : ItemScript("item_apex_supply_beacon") { }

    // Pattern of AscensionBankVoucher.cpp: returns true, so the dummy spell 93461 is never cast.
    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !item || item->GetEntry() != ITEM_APEX_SUPPLY_BEACON)
            return false;

        // Finish the client's item request first.
        player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);

        ChatHandler chat(player->GetSession());
        if (!IsCourierApex(player))
        {
            chat.SendSysMessage("Only an Apex can call the quartermaster.");
            return true;
        }
        ApexCourierState& state = State(player);
        if (state.beaconCooldownMs)
        {
            chat.SendSysMessage("The beacon is still recharging.");
            return true;
        }
        if (!CanReceiveNow(player))
        {
            chat.SendSysMessage("The quartermaster cannot reach you here.");
            return true;
        }
        if (ValidCourier(player, state))
            return true;

        if (Summon(player, state, true))
        {
            state.beaconCooldownMs = BEACON_COOLDOWN_MS;
            state.autoBlockMs = std::max(state.autoBlockMs, AUTO_WAIT_INITIAL_MS);
        }
        return true;
    }
};

// ---- Player ---------------------------------------------------------------------------------------------------
class AscensionApexCourierPlayerScript : public PlayerScript
{
public:
    AscensionApexCourierPlayerScript() : PlayerScript("AscensionApexCourierPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsHumanTinker(player))
            return;
        Rearm(State(player), CHECK_AFTER_LOGIN_MS);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!IsHumanTinker(player))
            return;
        Rearm(State(player), CHECK_AFTER_LEVEL_MS);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!IsHumanTinker(player))
            return;
        ApexCourierState& state = State(player);
        state.autoBlockMs = state.autoBlockMs > diff ? state.autoBlockMs - diff : 0;
        state.beaconCooldownMs = state.beaconCooldownMs > diff ? state.beaconCooldownMs - diff : 0;
        state.redeliverCooldownMs = state.redeliverCooldownMs > diff ? state.redeliverCooldownMs - diff : 0;
        if (state.checkInMs > diff)
        {
            state.checkInMs -= diff;
            return;
        }
        state.checkInMs = CHECK_PERIOD_MS;

        if (!player->IsInWorld() || !IsCourierApex(player))
            return;
        if (ValidCourier(player, state))
            return;
        state.courier.Clear();
        if (PendingTier(player) < 0 || state.autoBlockMs)
            return;
        if (!CanReceiveNow(player))
            return;
        if (!Summon(player, state, false))
            return;
        state.autoBlockMs = state.autoStepMs;
        state.autoStepMs = std::min(state.autoStepMs * 2, AUTO_WAIT_MAX_MS);
    }

private:
    static void Rearm(ApexCourierState& state, uint32 checkInMs)
    {
        state.checkInMs = checkInMs;
        state.autoBlockMs = 0;
        state.autoStepMs = AUTO_WAIT_INITIAL_MS;
    }
};
} // namespace

void AddAscensionTinkerApexCourierScripts()
{
    new AscensionApexCourierPlayerScript();
    new npc_apex_quartermaster();
    new item_apex_supply_beacon();
}
