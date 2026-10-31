/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
//
// Tinker Apex (class 28, specialization 102): Kesh'vaar, the Apex Quartermaster. Visible to that player alone.
//
// Hunts by tier (docs/CONCEPTION-apex-chasses-paliers.md, decisions of 31/10/2026, PTR first): he never comes below
// level 10, nor to a Tinker outside the Apex specialization. At level 10 an instant delivery (26110) hands over the
// beacon and the Initiate pauldrons; then one hunt per tier, offered IN ORDER (the PrevQuestID chain says the same):
// 26111 (10), 26121 (20), 26131 (30), 26141 (40), 26151 (50), 26161 (60). A hunt is an event quest (SpecialFlags 2):
// accepting it writes an 'ask' demand "apex chasse <tier> <quest>" in coa_dir_demande, and the Director service, the
// only judge of the hunt, completes it through Player::AreaExploredOrEventHappens (verb `valider`, mod-playerbots).
// While a hunt is in the log and incomplete, Kesh'vaar does not come on his own; once it is complete, he comes back
// to reward it. "Find me another prey." sends the demand again, at most once every 5 min per character.
// The beacon calls him back at will, and he redelivers any piece of a rewarded step that is lost.
// The chain of 27/10 (26101, 26120-26160) is no longer offered; a character who rewarded its last quest (26160,
// Thorin on the PTR) keeps his ledger closed: no hunt, redelivery of what he earned only.
//
// Script names (exploitation/apex/apex-livreur-ptr.sql, exploitation/apex/apex-chasses-paliers-ptr.sql):
//   creature 9120100 npc_apex_quartermaster    gossip, quest chaining, hunt demands, self-dismissal
//   item     9120050 item_apex_supply_beacon   calls the quartermaster (dummy spell 93461 never cast)
//
// Durable state is the quest status (character_queststatus, character_queststatus_rewarded): nothing new is stored in
// the base, except the demands sent to the Director. The volatile state (timers, courier GUID) lives in the player's
// CustomData, touched only from the player's map thread (player update, courier AI on the same map, in-place packet
// handlers) or from the world thread while maps are idle (gossip select, PROCESS_THREADUNSAFE): no lock. The only
// shared state is the per-character time of the last hunt demand, under its own mutex (lesson of P-050).

#include "AscensionSpecialization.h"

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "CreatureTextMgr.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
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
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// ---- Constants (plan.json -> cpp.constantes, cpp.temps_ms; docs/CONCEPTION-apex-chasses-paliers.md § 2) ----------
constexpr uint32 SPEC_APEX = 102;
constexpr uint8 COURIER_MIN_LEVEL = 10;   // decision Q2 of 31/10: nothing before level 10
constexpr uint32 NPC_APEX_QUARTERMASTER = 9120100;
constexpr uint32 ITEM_APEX_SUPPLY_BEACON = 9120050;
constexpr uint32 SPELL_ETHEREAL_TELEPORT = 34427;

constexpr uint8 NO_TEXT = 0xFF;

struct CourierStep
{
    uint8 Level;       // tier: the step is due from this level
    uint32 Quest;
    bool Hunt;         // event quest completed by the Director (SpecialFlags 2), else an instant delivery
    uint8 OfferText;   // creature_text group whispered when the step is offered
    uint8 ReturnText;  // creature_text group whispered when a hunt is rewarded
};

// In order: a step is offered only once every step before it is rewarded (and the quests chain by PrevQuestID).
constexpr std::array<CourierStep, 7> STEPS =
{{
    { 10, 26110, false, 12, NO_TEXT },   // beacon + Apex Initiate Pauldrons
    { 10, 26111, true, 13, 14 },         // Apex Blooded Pauldrons
    { 20, 26121, true, 15, 16 },         // Apex Blooded Hauberk
    { 30, 26131, true, 17, 18 },         // Apex Blooded Mask
    { 40, 26141, true, 19, 20 },         // Apex Blooded Legguards
    { 50, 26151, true, 21, 22 },         // Apex Blooded Gauntlets + Boots
    { 60, 26161, true, 23, 24 },         // Crate of the Elders (the eight Elder-Marked pieces)
}};

// The chain of 27/10, no longer offered: its rewards stay redeliverable to whoever earned them, and its last quest
// closes the ledger (the character already holds the Elder-Marked set).
constexpr std::array<uint32, 6> LEGACY_QUESTS = { 26101, 26120, 26130, 26140, 26150, 26160 };
constexpr uint32 LEGACY_LAST_QUEST = 26160;

// The crate of 26161 is opened (item_loot_template): what is owed afterwards is its content, not a new crate.
constexpr uint32 ITEM_ELDER_CRATE = 9120051;
constexpr std::array<uint32, 8> ELDER_CRATE_CONTENT =
    { 9120040, 9120041, 9120042, 9120043, 9120044, 9120045, 9120046, 9120047 };

// must match AscensionCompat.cpp TINKER_APEX_STARTER_ITEMS
constexpr std::array<uint32, 3> STARTER_WEAPONS = { 9120004, 9120005, 9120006 };
constexpr char STARTER_SETTING[] = "core.apex_starter"; // read only, never written here
constexpr uint8 STARTER_WEAPONS_LEVEL = 10;

constexpr uint32 NPC_TEXT_PENDING = 9120100;
constexpr uint32 NPC_TEXT_CLEAR = 9120101;
constexpr uint32 NPC_TEXT_NOT_APEX = 9120102;
constexpr uint32 NPC_TEXT_HUNT_OFFER = 9120103;
constexpr uint32 NPC_TEXT_HUNTING = 9120104;

constexpr uint8 TALK_FAREWELL = 7;
constexpr uint8 TALK_REDELIVERED = 9;
constexpr uint8 TALK_BAGS_FULL = 10;
constexpr uint8 TALK_BEACON_CLEAR = 11;
constexpr uint8 TALK_HUNT_DONE = 25;        // arrival, a hunt is complete
constexpr uint8 TALK_ANOTHER_PREY = 26;     // "Find me another prey.": demand sent
constexpr uint8 TALK_PREY_TOO_SOON = 27;    // "Find me another prey.": within 5 min of the last demand
constexpr uint8 TALK_STILL_HUNTING = 28;    // beacon call during a hunt

constexpr uint32 GOSSIP_ACTION_REDELIVER = 1;
constexpr uint32 GOSSIP_ACTION_DISMISS = 3;
constexpr uint32 GOSSIP_ACTION_ANOTHER_PREY = 4;
constexpr char GOSSIP_TEXT_REDELIVER[] = "I lost some of my Apex gear.";
constexpr char GOSSIP_TEXT_DISMISS[] = "Not now, Kesh'vaar.";
constexpr char GOSSIP_TEXT_ANOTHER_PREY[] = "Find me another prey.";

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
// "Find me another prey." at most once every 5 min per character, counted from the last demand (accept included).
constexpr Seconds ANOTHER_PREY_COOLDOWN = 300s;
constexpr float COURIER_MAX_DISTANCE = 80.0f;
constexpr float DUPLICATE_SWEEP_RANGE = 100.0f;
constexpr float SUMMON_DISTANCE = 3.0f;
constexpr float FOLLOW_DISTANCE = 2.5f;
constexpr uint32 COURIER_AI_CHECK_MS = 1000;

constexpr char STATE_KEY[] = "ApexCourier";

// Log once when the data is not there (SQL not applied): no spam every 10 s for each Apex.
std::atomic<bool> MissingQuestLogged{ false };
std::atomic<bool> SummonFailureLogged{ false };

// Time of the last hunt demand per character (guid counter). Written from the map thread (quest accept, in place)
// and from the world thread (gossip select): hence the mutex. One entry per Apex character, never erased.
std::mutex HuntDemandLock;
std::unordered_map<uint32, Seconds> LastHuntDemand;

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
/// Apex for the courier: a human Tinker, specialization 102, level 10 or more (same threshold as IsTinkerApex in
/// AscensionCompat.cpp since the decisions of 31/10).
bool IsCourierApex(Player const* player)
{
    return IsHumanTinker(player) && player->GetLevel() >= COURIER_MIN_LEVEL &&
           GetAscensionActiveSpecialization(player) == SPEC_APEX;
}

CourierStep const* StepOfQuest(uint32 questId)
{
    for (CourierStep const& step : STEPS)
        if (step.Quest == questId)
            return &step;
    return nullptr;
}

bool IsHuntQuest(uint32 questId)
{
    CourierStep const* step = StepOfQuest(questId);
    return step && step->Hunt;
}

enum class StepState : uint8
{
    None,       // nothing due
    Deliver,    // an instant delivery to hand over
    Offer,      // a hunt to offer (not in the log)
    Hunting,    // a hunt in the log, incomplete: no automatic visit
    Return,     // a hunt complete, to reward
};

struct PendingStep
{
    int32 Index = -1;
    StepState State = StepState::None;
};

/// The first step due (level reached, quest loaded, not rewarded) and where the character stands on it. The chain
/// stops at a step whose quest is missing (SQL not applied): the next ones need it rewarded anyway (PrevQuestID).
PendingStep FindPendingStep(Player const* player)
{
    PendingStep pending;
    if (player->GetQuestRewardStatus(LEGACY_LAST_QUEST))
        return pending;
    uint8 const level = player->GetLevel();
    for (std::size_t i = 0; i < STEPS.size(); ++i)
    {
        CourierStep const& step = STEPS[i];
        if (step.Level > level)
            break;
        if (!sObjectMgr->GetQuestTemplate(step.Quest))
        {
            if (!MissingQuestLogged.exchange(true))
                LOG_ERROR("module.ascension_compat", "Apex courier: quest template {} is missing, the chain stops "
                          "there (exploitation/apex/apex-chasses-paliers-ptr.sql not applied?)", step.Quest);
            break;
        }
        if (player->GetQuestRewardStatus(step.Quest))
            continue;
        pending.Index = int32(i);
        if (!step.Hunt)
            pending.State = StepState::Deliver;
        else
            switch (player->GetQuestStatus(step.Quest))
            {
                case QUEST_STATUS_COMPLETE:
                    pending.State = StepState::Return;
                    break;
                case QUEST_STATUS_INCOMPLETE:
                case QUEST_STATUS_FAILED:   // never failed by this code; held like a hunt in progress
                    pending.State = StepState::Hunting;
                    break;
                default:
                    pending.State = StepState::Offer;
                    break;
            }
        return pending;
    }
    return pending;
}

/// A visit is owed on his own initiative: something to hand over, to offer or to reward. Never during a hunt.
bool VisitDue(PendingStep const& pending)
{
    return pending.State == StepState::Deliver || pending.State == StepState::Offer ||
           pending.State == StepState::Return;
}

/// Reward items of a quest (RewardItemId, non-zero), empty when the quest is not loaded.
std::vector<uint32> QuestRewardItems(uint32 questId)
{
    std::vector<uint32> items;
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
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

/// Items of rewarded steps and legacy quests (plus the beacon and the starter weapons) the character no longer holds
/// anywhere. The Crate of the Elders stands for its eight pieces: a crate opened and emptied is not owed again, a
/// piece lost is (it comes alone, never in a new crate). A crate still held, unopened, owes nothing more.
std::vector<uint32> MissingItems(Player* player)
{
    std::vector<uint32> wanted;
    bool anyRewarded = false;
    auto want = [player, &wanted](uint32 questId)
    {
        for (uint32 itemId : QuestRewardItems(questId))
        {
            if (itemId != ITEM_ELDER_CRATE)
                wanted.push_back(itemId);
            else if (!HoldsItem(player, ITEM_ELDER_CRATE))
                wanted.insert(wanted.end(), ELDER_CRATE_CONTENT.begin(), ELDER_CRATE_CONTENT.end());
        }
    };
    for (CourierStep const& step : STEPS)
        if (player->GetQuestRewardStatus(step.Quest))
        {
            anyRewarded = true;
            want(step.Quest);
        }
    for (uint32 questId : LEGACY_QUESTS)
        if (player->GetQuestRewardStatus(questId))
        {
            anyRewarded = true;
            want(questId);
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

// ---- Hunt demands (the bridge to the Director service) ----------------------------------------------------------
bool AnotherPreyAllowed(Player const* player)
{
    std::lock_guard<std::mutex> guard(HuntDemandLock);
    auto itr = LastHuntDemand.find(player->GetGUID().GetCounter());
    return itr == LastHuntDemand.end() || GameTime::GetGameTime() >= itr->second + ANOTHER_PREY_COOLDOWN;
}

/// One 'ask' row in coa_dir_demande, same columns as Enregistrer (mod-playerbots CoaDirecteur.cpp): the listening
/// service reads "apex chasse <tier> <quest>" and opens the hunt. Asynchronous (DatabaseWorkerPool::Execute): the
/// thread that runs it never waits for MySQL, and no object of another thread is touched.
void SendHuntDemand(Player* player, CourierStep const& step)
{
    WorldSession* session = player->GetSession();
    if (!session)
        return;
    CharacterDatabase.Execute(
        "INSERT INTO coa_dir_demande (guid, compte, type, texte, carte, zone, aire, x, y, z, niveau, statut, cree_le) "
        "VALUES ({}, {}, 'ask', 'apex chasse {} {}', {}, {}, {}, {}, {}, {}, {}, 'nouveau', NOW())",
        player->GetGUID().GetCounter(), session->GetAccountId(), uint32(step.Level), step.Quest,
        player->GetMapId(), player->GetZoneId(), player->GetAreaId(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), uint32(player->GetLevel()));
    {
        std::lock_guard<std::mutex> guard(HuntDemandLock);
        LastHuntDemand[player->GetGUID().GetCounter()] = GameTime::GetGameTime();
    }
    LOG_INFO("module.ascension_compat", "Apex hunt demand for {} (level {}): apex chasse {} {}", player->GetName(),
             uint32(player->GetLevel()), uint32(step.Level), step.Quest);
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
    PendingStep const pending = FindPendingStep(player);
    switch (pending.State)
    {
        case StepState::Deliver:
        case StepState::Offer:
            sCreatureTextMgr->SendChat(courier, STEPS[pending.Index].OfferText, player);
            break;
        case StepState::Return:
            sCreatureTextMgr->SendChat(courier, TALK_HUNT_DONE, player);
            break;
        case StepState::Hunting:
            if (byBeacon)
                sCreatureTextMgr->SendChat(courier, TALK_STILL_HUNTING, player);
            break;
        case StepState::None:
            if (byBeacon)
                sCreatureTextMgr->SendChat(courier, TALK_BEACON_CLEAR, player);
            break;
    }

    // 6. Journal: "Apex courier" is the installation proof read in the binary.
    LOG_INFO("module.ascension_compat", "Apex courier summoned for {} (level {}, step {}, state {}, beacon {})",
             player->GetName(), uint32(player->GetLevel()), pending.Index, uint32(pending.State), byBeacon ? 1 : 0);
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

    PendingStep const pending = FindPendingStep(player);
    uint32 textId = NPC_TEXT_CLEAR;
    switch (pending.State)
    {
        case StepState::Deliver:
        case StepState::Return:
            player->PrepareQuestMenu(creature->GetGUID());
            if (player->GetFreeInventorySpace() < QuestRewardItems(STEPS[pending.Index].Quest).size())
                sCreatureTextMgr->SendChat(creature, TALK_BAGS_FULL, player);
            textId = NPC_TEXT_PENDING;
            break;
        case StepState::Offer:
            player->PrepareQuestMenu(creature->GetGUID());
            textId = NPC_TEXT_HUNT_OFFER;
            break;
        case StepState::Hunting:
            // The hunt shows in the quest list (incomplete); a new prey can be asked for.
            player->PrepareQuestMenu(creature->GetGUID());
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_ANOTHER_PREY, GOSSIP_SENDER_MAIN,
                             GOSSIP_ACTION_ANOTHER_PREY);
            textId = NPC_TEXT_HUNTING;
            break;
        case StepState::None:
            break;
    }
    if (!MissingItems(player).empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_REDELIVER, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_REDELIVER);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, GOSSIP_TEXT_DISMISS, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_DISMISS);
    SendGossipMenuFor(player, textId, creature->GetGUID());
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
            CourierStep const* rewarded = StepOfQuest(quest->GetQuestId());
            if (rewarded && rewarded->ReturnText != NO_TEXT)
                sCreatureTextMgr->SendChat(me, rewarded->ReturnText, player);

            PendingStep const pending = FindPendingStep(player);
            if (IsCourierApex(player) && VisitDue(pending))
            {
                if (pending.State != StepState::Return)
                    sCreatureTextMgr->SendChat(me, STEPS[pending.Index].OfferText, player);
                OpenCourierMenu(player, me);
                return;
            }
            // The return line of a hunt is his farewell; the last one says the ledger is closed.
            if (!rewarded || rewarded->ReturnText == NO_TEXT)
                sCreatureTextMgr->SendChat(me, TALK_FAREWELL, player);
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

    // Player::AddQuestAndCheckCompletion, after AddQuest: the hunt is in the log. Map thread (the accept packet is
    // handled in place): the demand goes out asynchronously, nothing of another thread is touched.
    bool OnQuestAccept(Player* player, Creature* creature, Quest const* quest) override
    {
        if (!player || !creature || !quest)
            return false;
        CourierStep const* step = StepOfQuest(quest->GetQuestId());
        TempSummon* summon = creature->ToTempSummon();
        if (!step || !step->Hunt || !summon || summon->GetSummonerGUID() != player->GetGUID() ||
            !IsCourierApex(player))
            return false;
        // An abandoned hunt taken again within 5 min of the last demand sends nothing more (the prey of that demand
        // may still run): "Find me another prey." serves once the 5 min are out.
        if (AnotherPreyAllowed(player))
            SendHuntDemand(player, *step);
        else
            LOG_INFO("module.ascension_compat", "Apex hunt {} taken again by {} within 5 min: no new demand",
                     step->Quest, player->GetName());
        // Nothing more to give until this prey falls (the chain waits for this hunt): he leaves.
        sCreatureTextMgr->SendChat(creature, TALK_FAREWELL, player);
        creature->DespawnOrUnsummon(5s);
        return false;
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
            case GOSSIP_ACTION_ANOTHER_PREY:
            {
                CloseGossipMenuFor(player);
                PendingStep const pending = FindPendingStep(player);
                if (pending.State != StepState::Hunting)
                    break;
                if (!AnotherPreyAllowed(player))
                {
                    sCreatureTextMgr->SendChat(creature, TALK_PREY_TOO_SOON, player);
                    break;
                }
                SendHuntDemand(player, STEPS[pending.Index]);
                sCreatureTextMgr->SendChat(creature, TALK_ANOTHER_PREY, player);
                creature->DespawnOrUnsummon(5s);
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
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_UPDATE,
          PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST }) { }

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

    // A hunt just completed (Player::CompleteQuest, through AreaExploredOrEventHappens): he comes back for it without
    // waiting out the 15-60 min of the automatic visits. Thread of the caller: the Director's verb runs on the world
    // thread while maps are idle, like the gossip handlers.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!quest || !IsHumanTinker(player) || !IsHuntQuest(quest->GetQuestId()))
            return;
        Rearm(State(player), CHECK_AFTER_LEVEL_MS);
        LOG_INFO("module.ascension_compat", "Apex hunt {} complete for {}", quest->GetQuestId(), player->GetName());
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
        if (!VisitDue(FindPendingStep(player)) || state.autoBlockMs)
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
