/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

/*
 * Worldforged pickups - the CoA world object that hands out a Worldforged base item,
 * which every character may loot once and then never again.
 *
 * The data (data/sql/db-world/2026_09_16_00_worldforged_pickups.sql) restores the pickups
 * themselves: 1,555 objects - bags, buckets, bones, packets, caches - each one named after
 * the base item it holds, each holding exactly that item. 1,510 of them are also spawned,
 * at the position the community observed that object at; the other 45 are templates with
 * no spawn (the sql's own scope block: 39 with no observed position, 6 whose observation
 * has no usable map). So a count of gameobject_template rows carrying the ScriptName
 * (1,555) and a count of their spawns (1,510) are both right, and differ for that reason -
 * both measured on the live acore_world, 2026-09-21.
 * Three deliberate deviations from the captures support the rule:
 *
 *   * chest.consumable = 0, so a pickup stays spawned after it is emptied instead of
 *     despawning on a respawn timer (which would make it a realm-wide roll per respawn).
 *     The core already re-rolls a non-consumable chest's loot for the next opener:
 *     Player::SendLoot clears and fills while the object is GO_READY, and
 *     GameObject::Update returns it to GO_READY after a loot is released.
 *   * chest.chestRestockTime = 0 (Data2, GameObjectData.h:86), so the object never enters
 *     the restock path and GameObject::Update's GO_JUST_DEACTIVATED branch puts it straight
 *     back to GO_READY, which is the re-roll the next character needs.
 *   * ScriptName 'worldforged_pickup', the marker this module keys on. The captures have
 *     an empty ScriptName.
 *
 * This module supplies what data cannot: the per-character, permanent memory.
 *
 *   * 'looted' is remembered per (character, spawn) in the characters database table
 *     character_worldforged_loot and in memory for the session.
 *   * The ledger is read in Player::LoadFromDB, not on login. This matters: the core
 *     sends a player the gameobjects around them before CharacterHandler calls
 *     OnPlayerLogin, so a ledger loaded that late arrives after the client has already
 *     been told the pickup is sparkling and lootable - and nothing corrects it, because
 *     the object's own state never changes. Loading during LoadFromDB is early enough
 *     that the first values update a player receives already carries the right flags.
 *   * A pickup the character already looted stays in the world - as it does on the realm -
 *     but is inert for that character alone: no sparkle, not selectable, not interactable
 *     (GameObjectAI::BuildClientFlags). Every other character still sees it sparkling.
 *   * A pickup the character may still loot sparkles
 *     (GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE), which is how they are found.
 *
 * Server-side the same rule is enforced in four places, because a client can always ask
 * again and the four answer different questions:
 *
 *   1. GameObjectAI::BuildClientFlags - what this viewer is shown.
 *   2. GameObjectAI::GossipHello - whether the use opens anything at all. GameObject::Use
 *      is not even reached for a viewer carrying GO_FLAG_NOT_SELECTABLE.
 *   3. OnAllowedForPlayerLootCheck (worldforged_pickup_loot_veto below) - whether this
 *      character may be handed the item, asked per item, at the moment of the award. This
 *      is the one that closes the door on a loot session that outlived its claim: the
 *      chest holds one shared Loot, so the next character's open re-rolls it under the
 *      first character's still-open window, and StoreLootItem would otherwise hand over a
 *      second copy to whoever clicks the slot first.
 *   4. GameObjectAI::OnStateChanged - repairs a pickup left GO_ACTIVATED with spent loot
 *      (someone closed the loot window by logging out), which would otherwise show the
 *      next character an empty window instead of their own item.
 *
 * The award itself and the record of it are one write: the item row, its place in the
 * inventory and the ledger row go into a single character-database transaction, so a crash
 * can leave neither rather than a claimed pickup whose item was never saved.
 *
 * Identity: a pickup is recorded by its *spawn id* (the `gameobject`.`guid` row), never by
 * the runtime object GUID. This core hands out map-local generated GUIDs
 * (Map::GenerateLowGuid), so a runtime GUID is neither the database row nor stable across
 * grid reloads - keying on it would lose or invent history. The restoration writes its
 * spawns in a fixed guid block, so the ids are stable across re-imports too.
 */

#include "Bag.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GlobalScript.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
// ScriptName on gameobject_template and gameobject rows of every pickup.
constexpr char const* WorldforgedPickupScript = "worldforged_pickup";
constexpr char const* WorldforgedLootTable = "character_worldforged_loot";
constexpr char const* LogCategory = "module.worldforged";

// Where the ledger table comes from. This repack runs with Updates.EnableDatabases = 0
// (worldserver.conf), so the file is applied by hand and can simply not have been.
constexpr char const* WorldforgedLootSql =
    "modules/mod-worldforged-pickups/data/sql/db-characters/2026_09_16_00_worldforged_loot.sql";

[[nodiscard]] bool IsWorldforgedPickup(GameObject const* go)
{
    if (!go || !go->GetGOInfo() || !go->GetSpawnId())
        return false;

    uint32 const scriptId = go->GetScriptId();
    return scriptId != 0 && sObjectMgr->GetScriptName(scriptId) == WorldforgedPickupScript;
}

// Per-character record of the pickups already looted, backed by the characters database.
class WorldforgedLootStore
{
public:
    static WorldforgedLootStore& Instance()
    {
        static WorldforgedLootStore store;
        return store;
    }

    [[nodiscard]] bool HasLooted(uint32 characterGuid, uint32 spawnId) const
    {
        // Fail closed. With no ledger table there is no memory of what a character has
        // already taken: every Query returns nullptr, which is indistinguishable from
        // "this character has looted nothing", so every pickup in the world would
        // sparkle for everyone, for ever, and the realm would fill with duplicates of
        // every Worldforged base item with nothing in any log to say why. Refusing them
        // all is the recoverable half of that choice - the table is applied, the realm
        // is restarted, and nothing was lost meanwhile. VerifyLedger says so at startup.
        if (!_ledgerReady.load(std::memory_order_relaxed))
            return true;

        std::lock_guard<std::mutex> lock(_mutex);
        auto itr = _looted.find(characterGuid);
        return itr != _looted.end() && itr->second.count(spawnId) != 0;
    }

    [[nodiscard]] bool LedgerReady() const
    {
        return _ledgerReady.load(std::memory_order_relaxed);
    }

    // Asked once, at startup, on the world thread. Everything this module can silently
    // get wrong starts the same way - the ledger table is not there - and no caller
    // downstream can tell an empty result from a failed one, so the distinction is made
    // here, once, and said out loud.
    void VerifyLedger()
    {
        // The three columns this module reads and writes, not just the table name: a
        // renamed column fails exactly the same way a missing table does - a nullptr
        // QueryResult that reads as "this character has looted nothing".
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `information_schema`.`columns` "
            "WHERE `table_schema` = DATABASE() AND `table_name` = '{}' "
            "AND `column_name` IN ('guid', 'spawn_id', 'entry')", WorldforgedLootTable);

        if (!result)
        {
            // The check itself failed, which says nothing about the table. Left open
            // rather than locking every pickup on the strength of a broken query.
            LOG_ERROR(LogCategory, "Could not ask the characters database whether `{}` exists; "
                      "the per-character pickup ledger is left enabled and may be reading nothing.",
                      WorldforgedLootTable);
            return;
        }

        if ((*result)[0].Get<uint64>() != 3)
        {
            _ledgerReady.store(false, std::memory_order_relaxed);
            LOG_ERROR(LogCategory, "`{}` is missing from the characters database, or no longer carries "
                      "`guid`, `spawn_id` and `entry`: every Worldforged pickup is refused to every "
                      "character until it does. Apply {} by hand.",
                      WorldforgedLootTable, WorldforgedLootSql);
            return;
        }

        // The spawn count is the other half of the picture: the rule is enforced per
        // (character, spawn id), so a data import that carried the pickups away shows up
        // in this one line and nowhere else. Measured on the realm in service on
        // 2026-09-21: 1,510 spawns carry the script (the file header above still says
        // 1,555 - that figure is the restoration's, not the world database's).
        uint32 spawns = 0;
        if (uint32 const wantedScript = sObjectMgr->GetScriptId(WorldforgedPickupScript))
        {
            for (auto const& entry : sObjectMgr->GetAllGOData())
            {
                // Same resolution order as GameObject::GetScriptId: the spawn row first,
                // the template only when the spawn does not name a script.
                uint32 scriptId = entry.second.ScriptId;
                if (!scriptId)
                    if (GameObjectTemplate const* proto = sObjectMgr->GetGameObjectTemplate(entry.second.id))
                        scriptId = proto->ScriptId;

                if (scriptId == wantedScript)
                    ++spawns;
            }
        }

        LOG_INFO(LogCategory, "Worldforged pickups ready: `{}` present, {} spawn(s) carry the '{}' script.",
                 WorldforgedLootTable, spawns, WorldforgedPickupScript);

        if (!spawns)
            LOG_ERROR(LogCategory, "No gameobject spawn carries the '{}' script: the module is loaded but "
                      "there is nothing for it to guard. Apply the pickup data.", WorldforgedPickupScript);
    }

    // Called from Player::LoadFromDB, before the player can be sent a single gameobject.
    // The whole ledger for a character is small (one row per pickup looted so far) and is
    // read once, not per pickup.
    void Load(uint32 characterGuid)
    {
        // Nothing to read, and asking would put one failing query per login into the
        // database error log. HasLooted already refuses every pickup in this state.
        if (!LedgerReady())
            return;

        std::unordered_set<uint32> looted;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT `spawn_id` FROM `{}` WHERE `guid` = {}", WorldforgedLootTable, characterGuid))
        {
            do
            {
                looted.insert((*result)[0].Get<uint32>());
            } while (result->NextRow());
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _looted[characterGuid] = std::move(looted);
    }

    void Unload(uint32 characterGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _looted.erase(characterGuid);
    }

    // The item and the fact that this pickup is now spent for this character are written
    // together: item_instance, character_inventory and the ledger row commit as one. The
    // item is still ITEM_NEW here - nothing has written it yet, because it is only flushed
    // with the next character save - so these are exactly the writes Player::_SaveInventory
    // would make, moved to the moment of the award.
    //
    // Order matters here, though not as much as the move looks. The in-memory mark used to
    // be set before the transaction was even opened, so a transaction that never landed
    // left the pickup spent for the whole session anyway - inert in BuildClientFlags,
    // refused by OnAllowedForPlayerLootCheck - while SaveToDB had already taken the item
    // out of the player's update queue, so the item was gone at the next login too. The
    // mark is now set after the commit, and that on its own changes nothing as long as the
    // outcome of the commit is not read: CommitTransaction only queues the transaction and
    // returns void (DatabaseWorkerPool.h), and no path between the pre-check above and the
    // insert below can leave this function. What the move buys is an order that matches the
    // facts, and a place for an outcome test to go if one is ever added - not a guard. The
    // duplicate it theoretically opens - the pre-check and the insert are no longer one
    // locked test-and-set - cannot happen from one character, whose hooks all run on their
    // own map thread in order, and is closed database-side by INSERT IGNORE.
    //
    // What is still not reported is a transaction that fails: CommitTransaction is
    // asynchronous and returns void, and the only ways to learn the outcome
    // (DirectCommitTransaction, which would block a map thread on disk, or
    // AsyncCommitTransaction, which needs a callback queue this module does not have)
    // both cost more than the defect is worth. Said here rather than left to be guessed.
    void Claim(Player* player, Item* item, uint32 spawnId, uint32 entry)
    {
        if (!LedgerReady())
            return;                                         // no table to write the claim into

        uint32 const characterGuid = player->GetGUID().GetCounter();

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto itr = _looted.find(characterGuid);
            if (itr != _looted.end() && itr->second.count(spawnId) != 0)
                return;                                     // already known, nothing to write
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        ObjectGuid::LowType bagGuid = 0;
        if (Bag* container = item->GetContainer())
            bagGuid = container->GetGUID().GetCounter();

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_INVENTORY_ITEM);
        stmt->SetData(0, characterGuid);
        stmt->SetData(1, bagGuid);
        stmt->SetData(2, item->GetSlot());
        stmt->SetData(3, item->GetGUID().GetCounter());
        trans->Append(stmt);

        // SaveToDB marks the item ITEM_UNCHANGED but leaves its pointer in the player's update queue.
        // Any later change (sold, destroyed, stacked) would queue it a second time, and the next
        // character save would then delete it on the first entry and read freed memory on the second.
        item->RemoveFromUpdateQueueOf(player);
        item->SaveToDB(trans);                              // item_instance, then ITEM_UNCHANGED

        trans->Append("INSERT IGNORE INTO `{}` (`guid`, `spawn_id`, `entry`) VALUES ({}, {}, {})",
                      WorldforgedLootTable, characterGuid, spawnId, entry);

        CharacterDatabase.CommitTransaction(trans);

        std::lock_guard<std::mutex> lock(_mutex);
        _looted[characterGuid].insert(spawnId);
    }

private:
    WorldforgedLootStore() = default;

    mutable std::mutex _mutex;
    std::unordered_map<uint32, std::unordered_set<uint32>> _looted;

    // Read from every map thread, written once on the world thread at startup. True until
    // VerifyLedger proves the table is not there, so a realm that never reaches OnStartup
    // behaves exactly as it did before rather than locking itself out.
    std::atomic<bool> _ledgerReady{true};
};

class WorldforgedPickupAI : public GameObjectAI
{
public:
    explicit WorldforgedPickupAI(GameObject* go) : GameObjectAI(go) { }

    // Sparkle marks a pickup this viewer may still loot; a viewer who already has must see
    // it as inert scenery - visible, like on the realm, but not openable.
    void BuildClientFlags(Player const* target, uint16& dynFlags, uint32& goFlags) override
    {
        if (!target)
            return;

        if (WorldforgedLootStore::Instance().HasLooted(target->GetGUID().GetCounter(), me->GetSpawnId()))
        {
            goFlags |= GO_FLAG_LOCKED | GO_FLAG_NOT_SELECTABLE;
            return;
        }

        if (sWorld->getBoolConfig(CONFIG_OBJECT_SPARKLES))
            dynFlags |= GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE;
    }

    // The server-side refusal: a client that still has a spent pickup in memory (or a
    // macro) gets nothing to open.
    bool GossipHello(Player* player, bool /*reportUse*/) override
    {
        if (!player)
            return true;

        if (!WorldforgedLootStore::Instance().HasLooted(player->GetGUID().GetCounter(), me->GetSpawnId()))
            return false;

        if (me->loot.isLooted())
        {
            me->loot.clear();
            player->SendLootRelease(me->GetGUID());
        }

        return true;
    }

    void OnStateChanged(uint32 state, Unit* unit) override
    {
        if (state != GO_ACTIVATED || !unit)
            return;

        Player* player = unit->ToPlayer();
        if (!player)
            return;

        uint32 const characterGuid = player->GetGUID().GetCounter();
        uint32 const spawnId = me->GetSpawnId();

        // Spent for this character: hand out nothing, and do not leave an open loot.
        if (WorldforgedLootStore::Instance().HasLooted(characterGuid, spawnId))
        {
            if (me->loot.isLooted())
            {
                me->loot.clear();
                player->SendLootRelease(me->GetGUID());
            }
            return;
        }

        // Spent for someone else and left behind - a loot window closed by a logout never
        // released the object, so it is still GO_ACTIVATED with an empty loot. Re-arm it so
        // this character gets their own roll instead of an empty window. The flag guards the
        // nested state change this triggers: one re-arm per interaction, never a loop.
        if (!_rearming && me->loot.isLooted())
        {
            _rearming = true;
            me->loot.clear();
            me->SetLootState(GO_READY);
            player->SendLoot(me->GetGUID(), LOOT_CORPSE);
            _rearming = false;
        }
    }

private:
    bool _rearming = false;              // an empty loot table can never spin this
};

// The marker itself: the database says which gameobjects are pickups, this says what they do.
class worldforged_pickup_script : public GameObjectScript
{
public:
    worldforged_pickup_script() : GameObjectScript(WorldforgedPickupScript) { }

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new WorldforgedPickupAI(go);
    }
};

// The last word on whether a pickup hands over its item, asked once per loot slot at the
// moment the award is decided. Player::StoreLootItem calls LootItem::AllowedForPlayer for
// every slot of a request, and everything a client can ask for - a click, an auto-store, a
// group roll - goes through StoreLootItem, so a character who has spent this pickup cannot
// be handed it again even while their old loot window is still open.
//
// The hook's name reads backwards, so read it with the macro: ScriptMgrMacros.h's
// CALL_ENABLED_BOOLEAN_HOOKS returns false as soon as any script returns true
// ("if (action) return false"), and AllowedForPlayer drops the item when that call returns
// false. Returning true here is therefore what withholds the item, and false is the
// do-nothing default for every other loot in the game.
class worldforged_pickup_loot_veto : public GlobalScript
{
public:
    worldforged_pickup_loot_veto() : GlobalScript("worldforged_pickup_loot_veto",
        {GLOBALHOOK_ON_ALLOWED_FOR_PLAYER_LOOT_CHECK}) { }

    bool OnAllowedForPlayerLootCheck(Player const* player, ObjectGuid source) override
    {
        if (!player || !source.IsGameObject())
            return false;

        GameObject* go = player->GetMap()->GetGameObject(source);
        return IsWorldforgedPickup(go) &&
            WorldforgedLootStore::Instance().HasLooted(player->GetGUID().GetCounter(), go->GetSpawnId());
    }
};

class worldforged_pickup_lifecycle : public PlayerScript
{
public:
    worldforged_pickup_lifecycle() : PlayerScript("worldforged_pickup_lifecycle",
        {PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_LOOT_ITEM}) { }

    // Early enough that no gameobject has been sent to this client yet.
    void OnPlayerLoadFromDB(Player* player) override
    {
        WorldforgedLootStore::Instance().Load(player->GetGUID().GetCounter());
    }

    void OnPlayerLogout(Player* player) override
    {
        WorldforgedLootStore::Instance().Unload(player->GetGUID().GetCounter());
    }

    // The moment the base item leaves a pickup, that pickup is spent for this character -
    // whether it was clicked, auto-stored or taken through the group window.
    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        if (!player || !item || !lootguid.IsGameObject())
            return;

        GameObject* go = player->GetMap()->GetGameObject(lootguid);
        if (!IsWorldforgedPickup(go))
            return;

        WorldforgedLootStore::Instance().Claim(player, item, go->GetSpawnId(), go->GetEntry());

        // Make this client re-read the flags, so the pickup goes inert for this viewer now.
        go->ForceValuesUpdateAtIndex(GAMEOBJECT_FLAGS);
        go->ForceValuesUpdateAtIndex(GAMEOBJECT_DYNAMIC);
    }
};

// The one line of startup this module needed: it is entirely data-driven - a ledger table
// applied by hand and a block of spawns carrying a script name - and until now it said nothing
// at all when either was missing, so a realm brought up on an unmigrated database handed
// every Worldforged item out again, to everyone, silently.
class worldforged_pickup_world : public WorldScript
{
public:
    worldforged_pickup_world() : WorldScript("worldforged_pickup_world", {WORLDHOOK_ON_STARTUP}) { }

    // Runs from Main.cpp after the world is initialised, so gameobject spawn data is in.
    void OnStartup() override
    {
        WorldforgedLootStore::Instance().VerifyLedger();
    }
};
}

void AddWorldforgedPickupsScripts()
{
    new worldforged_pickup_script();
    new worldforged_pickup_loot_veto();
    new worldforged_pickup_lifecycle();
    new worldforged_pickup_world();
}
