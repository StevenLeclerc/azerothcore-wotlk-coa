// mod-coa-challenges (review split): CoA.Challenges.Lifecycle.cpp
// Mechanical split of review-CoAChallenges.cpp; no logic changes.
#include "CoA.Challenges.Review.h"

namespace CoAChallenges
{

    // ---- Deferred cross-map work (P-034) -----------------------------------
    // MapUpdate.Threads is > 1: a script hook runs on the thread of ITS OWN
    // map. Touching a Player that belongs to another map from there races that
    // map's update (aura list, packets, group). P-034 states the rule: same map
    // (a->FindMap() == b->FindMap()), or database only.
    //
    // What still has to reach a holder who is online on ANOTHER map is queued
    // here and replayed from that player's own OnPlayerUpdate, on the same
    // pattern as SpellbindQueueKill / SpellbindProcessPending. The persisted
    // half of a transition is written by the producer (it touches no Player
    // object), so a disconnect before the queue is drained still leaves the
    // database correct; only the visible half is deferred.

    // Defined in CoA.Challenges.Fatigue.cpp.
    void ClearFatigueForChallenge(Player* player, uint32 challengeID);

    // Defined further down this file, used by ResetCoaCharacterState above it.
    void ClearConditionFlagCache(uint32 guid);

    // Marker prefix put on ConditionState::detail when a condition is broken
    // because its DEFINITION could not be read, not because the character did
    // anything. The `broken` flag has two readers that want opposite things:
    // ValidateChallenge reads it as "refuse to start", which is the safe
    // reading of a row we cannot parse; IsPristine reads it as "the character
    // is no longer pristine", which is the dangerous one -- it turns a free
    // cancel into a definitive coa_challenge_failure row, and with
    // BlockAllAfterFailure (default true) that condemns every future
    // activation of the character. A mistyped
    // coa_challenge_definition.conditions must block the start WITHOUT
    // condemning anyone, so IsPristine skips the states carrying this prefix.
    static constexpr char const CONDITION_DEF_ERROR_PREFIX[] = "malformed definition: ";

    static bool IsDefinitionError(ConditionState const& s)
    {
        return s.detail.rfind(CONDITION_DEF_ERROR_PREFIX, 0) == 0;
    }

    // Activation conditions that describe a PRISTINE character. They gate the
    // ENTRY into a challenge, not each of its tiers: a multi-level challenge
    // with no tracked objective only advances through CompleteAtLevelCap, so a
    // character asking for tier N>1 has necessarily broken all of them. Tier
    // N>1 is gated by HasCompletionLevel(N-1) instead. Anything else
    // (GROUP_SIZE, HAVE_FREE_INVENTORY_SLOTS, and an unknown type, which fails
    // closed) keeps applying to every tier. Orthogonal to that: a state broken
    // only because the definition is unreadable is filtered by
    // IsDefinitionError, and only in IsPristine -- see its comment above.
    static bool IsEntryOnlyCondition(std::string const& label)
    {
        return label == "LEVEL_UP"
            || label == "CANNOT_HAVE_GAINED_EXPERIENCE"
            || label == "LOOT_INTERACTION";
    }

    enum class RemoteWorkKind : uint8
    {
        Fail,
        Deactivate,
        LeaveGroup,
        MarkObjective,
        ProfessionXP
    };

    struct RemoteWork
    {
        RemoteWorkKind kind = RemoteWorkKind::Fail;
        uint32 challengeID = 0;
        uint32 level = 0;
        uint32 value = 0;               // deaths (Fail) / eventValue / rarityMult
        ObjectGuid killerSource;
        std::string objectiveType;
        // Deactivate only: the sync-decline rollback silences the 0x59B
        // broadcast, and the deferred half has to silence it too.
        bool suppressSync = false;
    };

    std::mutex RemoteWorkMutex;
    std::unordered_map<uint32, std::vector<RemoteWork>> RemoteWorkQueue;
    // Fast path. OnPlayerUpdate runs for every player on every tick on all map
    // threads; cross-map work is rare, so the drain must not take a global lock
    // when the queue is empty.
    std::atomic<uint32> RemoteWorkCount{0};

    // Same map = same update thread (P-034). A player whose map is unknown
    // (loading screen, teleport in flight) counts as remote: the safe side.
    bool OnSameMapThread(Player* a, Player* b)
    {
        if (!a || !b)
            return false;
        Map const* m = a->FindMap();
        return m != nullptr && m == b->FindMap();
    }

    // Hard cap: a kill-credit hook can fire many times per second, and a player
    // who never gets updated again (stuck loading screen) must not grow this
    // without bound.
    static constexpr size_t REMOTE_WORK_MAX = 64;

    static void QueueRemoteWork(ObjectGuid const& target, RemoteWork&& work)
    {
        if (!target)
            return;
        std::lock_guard<std::mutex> lock(RemoteWorkMutex);
        std::vector<RemoteWork>& list = RemoteWorkQueue[target.GetCounter()];
        if (list.size() >= REMOTE_WORK_MAX)
        {
            LOG_WARN("module.coa_challenges",
                "Deferred cross-map queue full for guid {}: dropping work item", target.GetCounter());
            return;
        }
        list.push_back(std::move(work));
        RemoteWorkCount.fetch_add(1, std::memory_order_release);
    }

    void QueueRemoteFail(ObjectGuid const& target, uint32 challengeID, uint32 level, uint32 deaths,
        ObjectGuid const& killerSource)
    {
        RemoteWork w;
        w.kind = RemoteWorkKind::Fail;
        w.challengeID = challengeID;
        w.level = level;
        w.value = deaths;
        w.killerSource = killerSource;
        QueueRemoteWork(target, std::move(w));
    }

    void QueueRemoteDeactivate(ObjectGuid const& target, uint32 challengeID, bool suppressSync)
    {
        RemoteWork w;
        w.kind = RemoteWorkKind::Deactivate;
        w.challengeID = challengeID;
        w.suppressSync = suppressSync;
        QueueRemoteWork(target, std::move(w));
    }

    void QueueRemoteLeaveGroup(ObjectGuid const& target)
    {
        RemoteWork w;
        w.kind = RemoteWorkKind::LeaveGroup;
        QueueRemoteWork(target, std::move(w));
    }

    void QueueRemoteMarkObjective(ObjectGuid const& target, std::string const& type, uint32 eventValue)
    {
        RemoteWork w;
        w.kind = RemoteWorkKind::MarkObjective;
        w.value = eventValue;
        w.objectiveType = type;
        QueueRemoteWork(target, std::move(w));
    }

    void QueueRemoteProfessionXP(ObjectGuid const& target, uint32 rarityMult)
    {
        RemoteWork w;
        w.kind = RemoteWorkKind::ProfessionXP;
        w.value = rarityMult;
        QueueRemoteWork(target, std::move(w));
    }

    // Drop whatever is still queued for a character leaving the world, so the
    // map does not keep entries for guids that will never be updated again.
    void ClearRemoteChallengeWork(uint32 guid)
    {
        if (!RemoteWorkCount.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(RemoteWorkMutex);
        auto it = RemoteWorkQueue.find(guid);
        if (it == RemoteWorkQueue.end())
            return;
        RemoteWorkCount.fetch_sub((uint32)it->second.size(), std::memory_order_release);
        RemoteWorkQueue.erase(it);
    }

    // Called from OnPlayerUpdate: we are on this player's own map thread, so
    // every one of these is a local write.
    void ProcessRemoteChallengeWork(Player* player)
    {
        if (!player)
            return;
        if (!RemoteWorkCount.load(std::memory_order_acquire))
            return;
        std::vector<RemoteWork> work;
        {
            std::lock_guard<std::mutex> lock(RemoteWorkMutex);
            auto it = RemoteWorkQueue.find(player->GetGUID().GetCounter());
            if (it == RemoteWorkQueue.end())
                return;
            work.swap(it->second);
            RemoteWorkQueue.erase(it);
        }
        RemoteWorkCount.fetch_sub((uint32)work.size(), std::memory_order_release);
        for (RemoteWork const& w : work)
        {
            switch (w.kind)
            {
                case RemoteWorkKind::Fail:
                    FailChallenge(player, w.challengeID, w.level, w.value, w.killerSource);
                    break;
                case RemoteWorkKind::Deactivate:
                    // Without this the rollback of a declined group sync would
                    // send the party a brand new 0x59B remove invitation, on
                    // the very challenge it is undoing.
                    if (w.suppressSync)
                    {
                        // g_suppressSyncBroadcast is ONE process-wide atomic
                        // (CoA.Challenges.Core.cpp:237), and this drain runs on
                        // every map thread. Writing `false` back in would
                        // un-suppress a rollback another thread is in the
                        // middle of (History.cpp HandleSyncResponse, Trials.cpp
                        // 791-829) and re-broadcast SMSG 0x59B on the very
                        // challenge being undone. Save and restore instead: the
                        // flag can then only ever be set, never cleared, by a
                        // thread that did not set it.
                        bool const prev = g_suppressSyncBroadcast.exchange(true);
                        DeactivateChallenge(player, w.challengeID);
                        g_suppressSyncBroadcast.store(prev);
                    }
                    else
                        DeactivateChallenge(player, w.challengeID);
                    break;
                case RemoteWorkKind::LeaveGroup:
                    if (player->GetGroup())
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage(
                            "You have been removed from the group: it now requires the same challenge set.");
                        player->RemoveFromGroup();
                    }
                    break;
                case RemoteWorkKind::MarkObjective:
                    MarkObjectives(player, w.objectiveType.c_str(), w.value);
                    break;
                case RemoteWorkKind::ProfessionXP:
                    GrantProfessionXP(player, w.value);
                    break;
            }
        }
    }

    // The persisted half of FailChallenge / DeactivateChallenge. Touches no
    // Player object, so it is safe from any thread. Both are idempotent: the
    // deferred FailChallenge replays them without doubling anything (INSERT
    // IGNORE on the (guid, challengeId) primary key, DELETE of a row already
    // gone).
    void FailChallengeDbOnly(uint32 guid, uint32 challengeID, uint32 level, uint32 deaths)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(
            "INSERT IGNORE INTO coa_challenge_failure (guid, challengeId, level, deaths, failTime) "
            "VALUES ({}, {}, {}, {}, UNIX_TIMESTAMP())", guid, challengeID, level, deaths);
        trans->Append(
            "DELETE FROM coa_character_challenge WHERE guid = {} AND challengeId = {}", guid, challengeID);
        CharacterDatabase.DirectCommitTransaction(trans);
        ClearCharChallengeCache(guid);
    }

    void DeactivateChallengeDbOnly(uint32 guid, uint32 challengeID)
    {
        CharacterDatabase.DirectExecute(
            "DELETE FROM coa_character_challenge WHERE guid = {} AND challengeId = {}", guid, challengeID);
        ClearCharChallengeCache(guid);
    }


    bool HasFailure(uint32 guid, uint32 challengeID)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT 1 FROM coa_challenge_failure WHERE guid = {} AND challengeId = {} LIMIT 1",
                guid, challengeID))
            return true;
        return false;
    }

    // Any failure at all for the character (used by BlockAllAfterFailure).
    bool HasAnyFailure(uint32 guid)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT 1 FROM coa_challenge_failure WHERE guid = {} LIMIT 1", guid))
            return true;
        return false;
    }

    bool HasCompletion(uint32 guid, uint32 challengeID)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT 1 FROM coa_challenge_completion WHERE guid = {} AND challengeId = {} LIMIT 1",
                guid, challengeID))
            return true;
        return false;
    }

    bool HasCompletionLevel(uint32 guid, uint32 challengeID, uint32 level)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT 1 FROM coa_challenge_completion WHERE guid = {} AND challengeId = {} AND level = {} LIMIT 1",
                guid, challengeID, level))
            return true;
        return false;
    }

    // Death update (SMSG 0x5A6): same 26-byte active-entry layout as 0x596,
    // with the deaths-used counter incremented. Handler 0x1369D0 overwrites
    // the active record, then derives (remaining, total) from the record +
    // the client's local WITHOUT_DEATH requirement and fires
    // CHALLENGE_DEATH_UPDATE(challengeID, remaining, total).
    // Counter = u32 at wire[10] (entry+0x16); level kept at wire[8..9].
    void SendDeathUpdate(Player* player, uint32 challengeID, uint32 level, uint32 deaths)
    {
        WorldSession* session = player->GetSession();
        if (!session)
            return;

        uint32 levelWord = (level & 0xFFFF) | ((deaths & 0xFFFF) << 16);
        WorldPacket data(SMSG_COA_CHALLENGE_DEATH_UPDATE, 32);
        data << uint32(0);
        data << uint32(challengeID);
        data << levelWord;
        data << uint32(0);
        data << uint32(0);
        data << uint32(0);
        data << uint16(0);

        session->SendPacket(&data);
        LOG_INFO("module.coa_challenges", "Sent SMSG 0x5A6 DEATH_UPDATE to {}: challengeID={} level={} deaths={}",
            player->GetName(), challengeID, level, deaths);
    }

    bool IsSharedFate(uint32 challengeID)
    {
        return DefField<bool>(challengeID, &ChallengeDef::sharedFate,
            "CoAChallenges.SharedFate." + std::to_string(challengeID), false);
    }

    // Mutual-exclusion group (client definitions). 0 = none. Two active
    // challenges with the same non-zero group cannot coexist.
    uint32 ExclusiveGroup(uint32 challengeID)
    {
        return DefField<uint32>(challengeID, &ChallengeDef::exclusiveGroup,
            "CoAChallenges.ExclusiveGroup." + std::to_string(challengeID), 0);
    }

    // A trial (client IsTrial=true) is exclusive with everything: you cannot
    // hold two trials, nor a trial together with a challenge. Challenges
    // (IsTrial=false) may stack (subject to ExclusiveGroup).
    bool IsTrialChallenge(uint32 challengeID)
    {
        return DefField<bool>(challengeID, &ChallengeDef::isTrial,
            "CoAChallenges.IsTrial." + std::to_string(challengeID), false);
    }

    bool IsPrestigeChallenge(uint32 challengeID)
    {
        return DefField<bool>(challengeID, &ChallengeDef::isPrestige,
            "CoAChallenges.IsPrestige." + std::to_string(challengeID), false);
    }

    // The client treats a player as "prestiged" iff they have COA_PRESTIGE_AURA
    // (C_Player:IsPrestiged() = HasAura(9930831), C_Player.lua:59). Prestige
    // challenges are gated on it.
    bool IsPrestiged(Player* player)
    {
        return player && player->HasAura(COA_PRESTIGE_AURA);
    }

    uint32 RequiredGameMode(uint32 challengeID)
    {
        return DefField<uint32>(challengeID, &ChallengeDef::requiredGameMode,
            "CoAChallenges.RequiredGameMode." + std::to_string(challengeID), 0);
    }

    // Game modes are NOT player-toggleable (SendConfigBatch locks the UI): the
    // bitmask is derived from the active challenges' RequiredGameMode. So a
    // trial that "is" a game mode (e.g. 61 -> Nightmare 0x100) turns the mode
    // on while active and off when it stops/fails/completes.
    void RecomputeRequiredGameModes(Player* player)
    {
        if (!player)
            return;
        uint32 guid = player->GetGUID().GetCounter();

        uint32 mask = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do
            {
                mask |= RequiredGameMode(r->Fetch()[0].Get<uint32>());
            } while (r->NextRow());
        }

        // Preserve genuinely player-toggled modes (testing flag): without this a
        // login/activate recompute would silently clear them.
        if (sConfigMgr->GetOption<bool>("CoAChallenges.GameModes.PlayerToggle", false))
            mask |= PlayerToggleMaskFor(guid);

        uint32 oldMask = CachedGameModeMask(guid);
        if (oldMask == mask)
            return;

        // Persist/cache the NEW mask first: RemoveChallengeSpell (called from
        // ApplyGameModeSpells) consults CachedGameModeMask to know which mode
        // auras are still live, so it must already reflect the removal.
        SaveGameModeMask(guid, mask);
        ApplyGameModeSpells(player, oldMask, mask);
        SendGameModeState(player, mask);
        LOG_INFO("module.coa_challenges", "Required game modes recomputed for {}: mask=0x{:X}",
            player->GetName(), mask);
    }

    // Full per-character CoA wipe for the GM `.coa reset`: strips challenge
    // auras/meters, clears EVERY coa_* row (challenge/objective/completion/
    // failure/condition/gamemode/mode-lives/survival/fatigue + the character's
    // custom trials and anything referencing them), drops the in-memory caches
    // and re-pushes the empty state to the client (active/failure/completed
    // lists + gamemode mask). Level, money and inventory are NOT touched here.
    void ResetCoaCharacterState(Player* player)
    {
        if (!player)
            return;
        uint32 guid = player->GetGUID().GetCounter();

        // Strip challenge auras (+ hunger/fatigue meters) before dropping rows.
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do { RemoveChallengeSpell(player, r->Fetch()[0].Get<uint32>()); } while (r->NextRow());
        }
        RemoveMeterAuras(player);

        // Bring the character back if dead, and drop resurrection sickness
        // (15007) left by a spirit-healer revive.
        if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f, false);
            player->SpawnCorpseBones();
        }
        player->RemoveAurasDueToSpell(15007);

        // Gamemode: remove the base auras + mode-lives counter, then the mask.
        // Cache the empty mask first so RemoveChallengeSpell (inside
        // ApplyGameModeSpells) does not treat the mode base auras as still live.
        uint32 oldMask = LoadGameModeMask(guid);
        SaveGameModeMask(guid, 0);
        ApplyGameModeSpells(player, oldMask, 0);
        ClearGameModeMaskCache(guid);

        // In-memory caches.
        Test_ClearHungerCache(guid);
        UntrackFatigue(player);
        UntrackSpellbind(player);
        UntrackInvertedBreath(player);
        UntrackLootedItems(guid);

        // Snapshot the completed (challenge, level) pairs so the client's
        // leaderboard cache for them can be refreshed after the delete.
        std::vector<std::pair<uint32, uint32>> completions;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId, level FROM coa_challenge_completion WHERE guid = {}", guid))
        {
            do
            {
                Field* f = r->Fetch();
                completions.emplace_back(f[0].Get<uint32>(), f[1].Get<uint32>());
            } while (r->NextRow());
        }
        // Same for custom-trial completions (no "completion removed" opcode, so
        // the trial leaderboard cache must be re-pushed as well).
        std::vector<std::string> trialCompletions;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT DISTINCT trialId FROM coa_custom_trial_completion WHERE guid = {}", guid))
        {
            do { trialCompletions.push_back(r->Fetch()[0].Get<std::string>()); } while (r->NextRow());
        }

        // Custom trials owned by this character: collect the ids first so the
        // realm-wide rows referencing them (entries/votes/completions/active)
        // can be dropped too, then remove the trials themselves.
        std::vector<std::string> ownedTrials;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT trialId FROM coa_custom_trial WHERE guid = {}", guid))
        {
            do { ownedTrials.push_back(r->Fetch()[0].Get<std::string>()); } while (r->NextRow());
        }

        // DB rows.
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_challenge WHERE guid = {}", guid);
        ClearCharChallengeCache(guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_objective WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_challenge_completion WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_challenge_failure WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_condition WHERE guid = {}", guid);
        // The rows are gone, so the "already written" set must go with them:
        // otherwise the next loot would skip the INSERT and LOOT_INTERACTION
        // would read a clean character forever.
        ClearConditionFlagCache(guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_gamemode WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_gamemode_lives WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_survival WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_fatigue WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_character_looted_item WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_entry WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_vote WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_completion WHERE guid = {}", guid);
        for (std::string trialId : ownedTrials)
        {
            CharacterDatabase.EscapeString(trialId);
            CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_entry WHERE trialId = '{}'", trialId);
            CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_vote WHERE trialId = '{}'", trialId);
            CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_completion WHERE trialId = '{}'", trialId);
            CharacterDatabase.DirectExecute("DELETE FROM coa_custom_trial_active WHERE trialId = '{}'", trialId);
        }

        // Re-push the cleared state to the client.
        SendActiveList(player);
        SendCriteriaState(player);
        SendFailureList(player);
        SendCompletedList(player);
        SendGameModeState(player, 0);
        SetActiveCustomTrial(player, "");   // clear the client's active custom trial
        SendTrialList(player);              // and drop the character's custom trials

        // Refresh the client's leaderboard cache for the challenges this player
        // had a record on (there is no "completion removed" opcode).
        for (auto const& [cid, lvl] : completions)
            SendCompletionList(player, cid, lvl);
        for (std::string const& t : trialCompletions)
            SendTrialCompletions(player, t);

        LOG_INFO("module.coa_challenges", "Reset all CoA state for {}", player->GetName());
    }

    uint32 RequiredGameEvent(uint32 challengeID)
    {
        return DefField<uint32>(challengeID, &ChallengeDef::requiredGameEvent,
            "CoAChallenges.RequiredGameEvent." + std::to_string(challengeID), 0);
    }

    bool NoRewards(uint32 challengeID)
    {
        return DefField<bool>(challengeID, &ChallengeDef::noRewards,
            "CoAChallenges.NoRewards." + std::to_string(challengeID), false);
    }

    bool ChallengeExists(uint32 challengeID)
    {
        return !DefField<std::string>(challengeID, &ChallengeDef::name,
            "CoAChallenges.Name." + std::to_string(challengeID), "").empty();
    }

    // Conflict check for activation. Returns the Enum.ChallengeResponse code
    // (0 none; 5 EXCLUSIVE_GROUP_ACTIVE; 13 PARTICIPATING_IN_TRIAL;
    // 14 PARTICIPATING_IN_CHALLENGE) and sets conflictId to the blocking
    // active challenge.
    uint32 ConflictingExclusiveChallenge(uint32 guid, uint32 challengeID, uint32& conflictId)
    {
        conflictId = 0;
        uint32 group = ExclusiveGroup(challengeID);
        bool newIsTrial = IsTrialChallenge(challengeID);
        for (uint32 cid : ActiveChallenges(guid))
        {
            if (cid == challengeID)
                continue;
            bool activeIsTrial = IsTrialChallenge(cid);
            if (newIsTrial && !activeIsTrial)
            {
                conflictId = cid;
                return 14; // activating a trial while in a challenge
            }
            if (!newIsTrial && activeIsTrial)
            {
                conflictId = cid;
                return 13; // activating a challenge while in a trial
            }
            // Trial vs trial: exclusive regardless of ExclusiveGroup, so two
            // definitions with group 0 cannot run at the same time.
            if (newIsTrial && activeIsTrial)
            {
                conflictId = cid;
                return 13;
            }
            if (group && ExclusiveGroup(cid) == group)
            {
                conflictId = cid;
                return 5; // same exclusive group (trials)
            }
        }
        return 0;
    }

    // "Dead forever": failing a permadeath challenge blocks resurrection.
    // EXPLICIT only (CoAChallenges.NoResurrect.<id>) — hardcore/1-life is NOT
    // permadeath by default: dying just fails the challenge.
    bool IsPermaDeath(uint32 challengeID)
    {
        return DefField<bool>(challengeID, &ChallengeDef::noResurrect,
            "CoAChallenges.NoResurrect." + std::to_string(challengeID), false);
    }

    bool HasPermaDeathFailure(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        QueryResult r = CharacterDatabase.Query(
            "SELECT challengeId FROM coa_challenge_failure WHERE guid = {}", guid);
        if (!r)
            return false;
        do
        {
            if (IsPermaDeath(r->Fetch()[0].Get<uint32>()))
                return true;
        } while (r->NextRow());
        return false;
    }

    // ---- Challenge activation (shared by StartChallenge and ActivateTrial) --
    // Validates one challenge against the client's activation rules. Returns the
    // Enum.ChallengeResponse code (0 = OK, 1..14 = reason). Sends a chat hint
    // for every rejection so the player always sees WHY activation failed.
    uint32 ValidateChallenge(Player* player, uint32 challengeID, uint32 level)
    {
        uint32 guid = player->GetGUID().GetCounter();

        if (!ChallengeExists(challengeID))                       // 1 NOT_FOUND
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Challenge {} was not found.", challengeID);
            return 1;
        }
        {
            // The client sends the tier it wants; nothing else looks at it, so
            // an out-of-range value used to be written straight into
            // coa_character_challenge.level. Tier 4000000000 on a one-tier
            // challenge then made GetChallengeRewards find nothing (no reward
            // at completion) and SendDeathUpdate, which packs the level in 16
            // bits, display garbage. HandleSaveTrial already bounds its own
            // level the same way; this is the gate the three activation paths
            // (CMSG 0x592, trial activation, group sync answer) share.
            // levelCount is 1, 25 or 100 in every shipped definition; the
            // clamp only guards a future row left at 0, which must not make the
            // challenge unstartable.
            uint32 maxLevel = ChallengeLevelCount(challengeID);
            if (maxLevel == 0)
                maxLevel = 1;
            if (level < 1 || level > maxLevel)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "Level {} does not exist for {} (1..{}).", level, ChallengeName(challengeID), maxLevel);
                LOG_WARN("module.coa_challenges",
                    "Activation of challenge {} refused for {}: level {} out of range (1..{})",
                    challengeID, player->GetName(), level, maxLevel);
                return 1;
            }
        }
        if (sConfigMgr->GetOption<bool>("CoAChallenges.EnforceNoRewards", false)
            && NoRewards(challengeID))                           // 2 NO_REWARDS
        {
            ChatHandler(player->GetSession()).PSendSysMessage("{} has no rewards.", ChallengeName(challengeID));
            return 2;
        }
        // 3 WRONG_GAME_MODE is intentionally not enforced here: game modes are
        // trial-driven. A trial whose RequiredGameMode is set provides that mode
        // itself (RecomputeRequiredGameModes turns the bit on while it is
        // active), so requiring it beforehand would block its own activation.
        if (ActiveChallenges(guid).count(challengeID))           // 4 ALREADY_ACTIVE
        {
            ChatHandler(player->GetSession()).PSendSysMessage("{} is already active.", ChallengeName(challengeID));
            return 4;
        }
        {
            uint32 conflictId = 0;
            uint32 conflictCode = ConflictingExclusiveChallenge(guid, challengeID, conflictId);
            if (conflictCode)                                    // 5/13/14 EXCLUSIVE/TRIAL/CHALLENGE
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "You already have an active challenge that conflicts ({}).", conflictId);
                return conflictCode;
            }
        }
        if (uint32 eventId = RequiredGameEvent(challengeID))     // 6 GAME_EVENT_NOT_ACTIVE
        {
            if (!sGameEventMgr->IsActiveEvent(eventId))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "{} requires a game event that is not active.", ChallengeName(challengeID));
                return 6;
            }
        }
        {
            // Some activation conditions describe a PRISTINE character
            // (LEVEL_UP / CANNOT_HAVE_GAINED_EXPERIENCE: still level 1;
            // LOOT_INTERACTION: never opened a loot window, a flag that is only
            // cleared by `.coa reset`). A multi-level challenge with no tracked
            // objective is completed by CompleteAtLevelCap, i.e. at the level
            // cap, so by the time tier 2 is asked for, the character
            // necessarily breaks them: every tier above 1 was unreachable and
            // their rewards out of reach for the whole realm. Those gates apply
            // to the ENTRY into the challenge only; tier N>1 is gated by
            // HasCompletionLevel(N-1) below instead.
            // Everything else (GROUP_SIZE, HAVE_FREE_INVENTORY_SLOTS, and an
            // unknown type, which fails closed) keeps applying to every tier.
            bool const higherTier = (level > 1 && ChallengeLevelCount(challengeID) > 1);
            for (ConditionState const& s : EvaluateConditions(player, challengeID))
            {
                if (!s.broken)
                    continue;
                if (higherTier && IsEntryOnlyCondition(s.label))
                    continue;
                                                                 // 8 CONDITIONS_NOT_MET
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "Cannot start {}: {}", ChallengeName(challengeID),
                    s.label + " (" + s.detail + ")");
                return 8;
            }
        }
        // NO_GROUP (solo-only, e.g. Ironman): cannot start while grouped.
        if (player->GetGroup())
        {
            std::string const rules = ChallengeRules(challengeID);
            if (CoAParse::ListContains(rules, "CHALLENGE_RULES_TYPE_NO_GROUP"))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "You must leave your group before starting {}.", ChallengeName(challengeID));
                return 12; // RULE_BROKEN
            }
        }
        // HIGH_RISK_ONLY (High Roller / Ironman - High-Risk): the character must
        // be in the High Risk ruleset (aura applied by mod-ascension-compat).
        if (CoAParse::ListContains(ChallengeRules(challengeID), "CHALLENGE_RULES_TYPE_HIGH_RISK_ONLY")
            && HighRiskActivationBlocked(player))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "{} requires you to be in High Risk.", ChallengeName(challengeID));
            return 12; // RULE_BROKEN
        }
        if (ChallengeLevelCount(challengeID) > 1 && level > 1
            && !HasCompletionLevel(guid, challengeID, level - 1))
        {                                                        // 9 PREVIOUS_LEVEL_NOT_COMPLETED
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Complete the previous level of {} first.", ChallengeName(challengeID));
            return 9;
        }
        if (IsPrestigeChallenge(challengeID) && !IsPrestiged(player))  // 11 NOT_PRESTIGE
        {
            ChatHandler(player->GetSession()).PSendSysMessage("{} requires prestige.", ChallengeName(challengeID));
            return 11;
        }
        if (sConfigMgr->GetOption<bool>("CoAChallenges.BlockAllAfterFailure", true)
            && HasAnyFailure(guid))                              // 12 RULE_BROKEN
        {
            LOG_INFO("module.coa_challenges", "Blocked activation of {} for {}: character already failed a challenge",
                challengeID, player->GetName());
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You have already failed a challenge and cannot start another.");
            return 12;
        }
        if (sConfigMgr->GetOption<bool>("CoAChallenges.BlockFailedReactivate", true)
            && HasFailure(guid, challengeID))                    // 12 RULE_BROKEN
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You have already failed {} and cannot retake it.", ChallengeName(challengeID));
            return 12;
        }
        // Level-scoped: a multi-level challenge must stay activatable at level N
        // once level N-1 is completed (any-level HasCompletion would lock out
        // every level past the first).
        if (sConfigMgr->GetOption<bool>("CoAChallenges.BlockCompletedReactivate", true)
            && HasCompletionLevel(guid, challengeID, level))
        {
            LOG_INFO("module.coa_challenges", "Blocked reactivation of completed challenge {} for {}",
                challengeID, player->GetName());
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You have already completed this trial ({}).", ChallengeName(challengeID));
            return 12;
        }
        return 0;
    }

    // On activation, drop group members who don't share the activator's
    // challenge set (CoAChallenges.RequireSameChallengeToGroup). Solo/NO_GROUP
    // challenges are blocked before reaching here, so this covers same-challenge
    // group trials (Boss Blitz, Duo/Trio...).
    void EnforceGroupOnActivation(Player* player)
    {
        if (!sConfigMgr->GetOption<bool>("CoAChallenges.RequireSameChallengeToGroup", true))
            return;
        // When group sync is enabled, members are invited via SMSG 0x59B and
        // opt into the set (CMSG 0x59C) instead of being kicked immediately.
        if (sConfigMgr->GetOption<bool>("CoAChallenges.SyncChallengesWithGroup", true))
            return;
        Group* group = player->GetGroup();
        if (!group)
            return;

        std::set<uint32> const mine = ActiveChallenges(player->GetGUID().GetCounter());
        std::vector<ObjectGuid> kick;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == player->GetGUID())
                continue;
            if (ActiveChallenges(slot.guid.GetCounter()) != mine)
                kick.push_back(slot.guid);
        }
        for (ObjectGuid const& g : kick)
        {
            Player* member = ObjectAccessor::FindPlayer(g);
            if (!member)
                continue;
            // P-034: another map = another update thread. Writing to his session
            // and pulling him out of the group there races his own update.
            if (!OnSameMapThread(member, player))
            {
                QueueRemoteLeaveGroup(g);
                continue;
            }
            ChatHandler(member->GetSession()).PSendSysMessage(
                "You have been removed from the group: it now requires the same challenge set.");
            member->RemoveFromGroup();
        }
    }

    // Persist + apply aura/hunger for one challenge (no validation).
    void DoActivateChallenge(Player* player, uint32 challengeID, uint32 level)
    {
        uint32 guid = player->GetGUID().GetCounter();
        // Direct (synchronous): the active-list/criteria re-push below reads the
        // row right after, and activation is rare enough that blocking is fine.
        CharacterDatabase.DirectExecute(
            "REPLACE INTO coa_character_challenge (guid, challengeId, level, deaths, hunger, thirst, startTime) "
            "VALUES ({}, {}, {}, 0, 0, 0, UNIX_TIMESTAMP())",
            guid, challengeID, level);
        ClearCharChallengeCache(guid);
        ApplyChallengeSpell(player, challengeID, level);
        TrackHunger(player, challengeID);
        TrackFatigue(player, challengeID);
        TrackSpellbind(player, challengeID);
        TrackInvertedBreath(player, challengeID);
        RefreshRegenTracking(player);
        RefreshHighRiskTracking(player);
        RefreshLootedTracking(player);
        RecomputeRequiredGameModes(player);
        EnforceGroupOnActivation(player);

        // Static party-size gates (DUO/TRIO): the size was enforced by the
        // GROUP_SIZE condition in ValidateChallenge, so record them as met now
        // (they show up as done criteria and count for completion).
        for (Objective const& o : GetObjectives(challengeID))
        {
            if (o.type == "CHALLENGE_REQUIREMENT_TYPE_DUO"
                || o.type == "CHALLENGE_REQUIREMENT_TYPE_TRIO")
                SetObjectiveDone(guid, challengeID, o.key);
        }

        // Push the active set + the criteria cache for every activation path
        // (CMSG 0x592, trial activate, `.coa e2e`): the per-objective banner
        // (0x59A) needs the 0x599 list to have seeded the criterion first.
        SendActiveList(player);
        SendCriteriaState(player);

        // Invite the rest of the group to sync this challenge set (SMSG 0x59B).
        BroadcastChallengeSync(player, challengeID, level, false);
    }

    // Validate + activate a single challenge. Returns 0 on success.
    uint32 ActivateChallenge(Player* player, uint32 challengeID, uint32 level)
    {
        uint32 code = ValidateChallenge(player, challengeID, level);
        if (code)
            return code;
        DoActivateChallenge(player, challengeID, level);
        return 0;
    }

    // Remove one challenge from the active set (no response packets).
    void DeactivateChallenge(Player* player, uint32 challengeID)
    {
        uint32 guid = player->GetGUID().GetCounter();
        CharacterDatabase.DirectExecute(
            "DELETE FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
            guid, challengeID);
        ClearCharChallengeCache(guid);
        RemoveChallengeSpell(player, challengeID);
        RemoveMeterAuras(player);
        RemoveHungerChallenge(player, challengeID);
        // Drop THIS challenge's fatigue counter, then rebuild from whatever
        // other active challenge still carries FATIGUED_UNLESS_RESTED. The old
        // global ClearFatigue() erased every coa_character_fatigue row of the
        // character and nothing repopulated the tracker, so ending one
        // challenge silently disarmed another one's gauge until the next login.
        if (IsFatigueChallenge(challengeID))
        {
            ClearFatigueForChallenge(player, challengeID);
            RefreshFatigueTracking(player);
        }
        UntrackSpellbind(player);
        // Rebuild for any OTHER active challenge that still carries a spellbind
        // rule (non-exclusive challenges can coexist: removing one must not
        // silently kill another's roulette).
        RefreshSpellbindTracking(player);
        // Same reason for INVERTED_BREATH: recompute from the remaining active
        // set instead of a blind untrack.
        RefreshInvertedBreathTracking(player);
        RefreshRegenTracking(player);
        RefreshHighRiskTracking(player);
        RefreshLootedTracking(player);
        RecomputeRequiredGameModes(player);

        // Keep the client's active + criteria caches consistent (all
        // deactivation paths: CMSG 0x594, trial deactivate).
        SendActiveList(player);
        SendCriteriaState(player);

        // Tell the rest of the group to drop it too (SMSG 0x59B remove).
        BroadcastChallengeSync(player, challengeID, 0, true);
    }

    // ---- Rules (CHALLENGE_RULES_TYPE_*) ----------------------------------
    // Per-challenge rules live in the world DB (coa_challenge_definition.rules)
    // as a semicolon-separated list, read through ChallengeRules(). A rule
    // applies if ANY active challenge of the player lists it. Enforcement is
    // server-side (client UI is cosmetic).

    bool RuleListContains(std::string const& list, std::string const& rule)
    {
        return CoAParse::ListContains(list, rule);
    }

    // In-memory mirror of coa_character_challenge, for the same reason the game mode mask has one:
    // PlayerHasRule is called from the combat hooks and must not reach the database.
    std::mutex CharChallengeMutex;
    std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> CharChallengeCache;

    std::vector<std::pair<uint32, uint32>> LoadCharChallenges(uint32 guid)
    {
        std::vector<std::pair<uint32, uint32>> active;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId, level FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do
            {
                Field* f = r->Fetch();
                active.emplace_back(f[0].Get<uint32>(), f[1].Get<uint32>());
            } while (r->NextRow());
        }
        return active;
    }

    // The character's active challenges, kept in memory the way the game mode mask already is.
    // PlayerHasRule runs from the combat hooks - every hit, every heal, every melee roll - and
    // asking the database there put a synchronous query on the map threads: measured at 18 000
    // SELECT a second on a realm with a thousand characters fighting, against an empty table.
    //
    // A copy is returned on purpose. A reference would point inside a map that another map
    // thread can rehash under the caller, which is the failure #4021 had to fix elsewhere.
    std::vector<std::pair<uint32, uint32>> CachedCharChallenges(uint32 guid)
    {
        {
            std::lock_guard<std::mutex> lock(CharChallengeMutex);
            auto it = CharChallengeCache.find(guid);
            if (it != CharChallengeCache.end())
                return it->second;
        }
        std::vector<std::pair<uint32, uint32>> active = LoadCharChallenges(guid);
        std::lock_guard<std::mutex> lock(CharChallengeMutex);
        CharChallengeCache[guid] = active;
        return active;
    }

    // Called wherever the module adds or removes a row of coa_character_challenge, and at logout.
    void ClearCharChallengeCache(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(CharChallengeMutex);
        CharChallengeCache.erase(guid);
    }

    bool PlayerHasRule(Player* player, char const* rule)
    {
        // The combat hooks read the rules through here, and they are registered whatever the
        // setting says: without this an operator who turns the module off still pays for it.
        if (!player || !sConfigMgr->GetOption<bool>("CoAChallenges.Enable", true))
            return false;
        uint32 guid = player->GetGUID().GetCounter();

        for (auto const& [cid, unusedLevel] : CachedCharChallenges(guid))
        {
            std::string rules = ChallengeRules(cid);
            if (RuleListContains(rules, rule))
                return true;
        }

        // Game mode scope: a mode that is on without a trial applies its base
        // challenge's rules too (bit -> base, e.g. 0x100 -> 61 Nightmare).
        uint32 mask = CachedGameModeMask(guid);
        for (auto const& [bit, base] : GameModeBaseSnapshot())
        {
            if (!(mask & bit))
                continue;
            std::string rules = ChallengeRules(base);
            if (RuleListContains(rules, rule))
                return true;
        }
        return false;
    }

    // Active challenge carrying `rule` (0 if none), with its level in `level`.
    // Used by FAILABLE_* rules that must fail the specific trial.
    uint32 ActiveChallengeWithRule(Player* player, char const* rule, uint32& level)
    {
        level = 0;
        if (!player)
            return 0;
        uint32 guid = player->GetGUID().GetCounter();
        for (auto const& [cid, challengeLevel] : CachedCharChallenges(guid))
        {
            std::string rules = ChallengeRules(cid);
            if (RuleListContains(rules, rule))
            {
                level = challengeLevel;
                return cid;
            }
        }
        return 0;
    }

    std::set<uint32> ActiveChallenges(uint32 guid)
    {
        std::set<uint32> out;
        for (auto const& [cid, unusedLevel] : CachedCharChallenges(guid))
            out.insert(cid);
        return out;
    }

    // ---- Activation conditions -------------------------------------------
    // Per-challenge conditions are generated into CoAChallenges.Conditions.<id>
    // as "TYPE:V1/V2/V3;...". A condition is false by default and becomes
    // "broken" (blocking activation) when the player violates it. Loot/level
    // flags persist per character; group/inventory are checked live.

    // Condition flags are write-once per character ("LOOTED" is set the first
    // time a loot window is opened and only `.coa reset` clears it), but
    // SetConditionFlag is called from OnPlayerBeforeSendLoot: every loot of
    // every character. On a realm carrying ~700 bots that loot continuously
    // this queued thousands of INSERT IGNORE a minute into CharacterDatabase,
    // all no-ops after the first one, and that noise sits in front of the
    // writes that matter (character saves, reward transactions).
    //
    // This set remembers what has already been written for a connected
    // character so the repeats never reach the queue. It is a WRITE-side cache
    // only: HasConditionFlag still reads the table, so no gameplay gate ever
    // depends on it being fresh. Cleared at logout (CoA.Challenges.Scripts.cpp,
    // OnPlayerLogout) and by ResetCoaCharacterState, the two places the rows
    // themselves go away.
    std::mutex ConditionFlagMutex;
    std::unordered_map<uint32, std::unordered_set<std::string>> WrittenConditionFlags;

    void ClearConditionFlagCache(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(ConditionFlagMutex);
        WrittenConditionFlags.erase(guid);
    }

    void SetConditionFlag(uint32 guid, char const* flag)
    {
        std::string eflag = flag ? flag : "";
        {
            // insert() tells us in one step whether this character already had
            // the flag written since login; if so there is nothing to do.
            std::lock_guard<std::mutex> lock(ConditionFlagMutex);
            if (!WrittenConditionFlags[guid].insert(eflag).second)
                return;
        }
        CharacterDatabase.EscapeString(eflag);
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO coa_character_condition (guid, flag) VALUES ({}, '{}')",
            guid, eflag);
    }

    bool HasConditionFlag(uint32 guid, char const* flag)
    {
        std::string eflag = flag ? flag : "";
        CharacterDatabase.EscapeString(eflag);
        return (bool)CharacterDatabase.Query(
            "SELECT 1 FROM coa_character_condition WHERE guid = {} AND flag = '{}' LIMIT 1",
            guid, eflag);
    }

    uint32 FreeInventorySlots(Player* player)
    {
        if (!player)
            return 0;
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++free;
        // Equipped bags count too: "has a free inventory slot" is not backpack-only.
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                free += bag->GetFreeSlots();
        return free;
    }

    // coa_challenge_definition.conditions is hand-editable, so a stray space
    // around a token or a value must not change how the entry reads.
    static std::string TrimSpaces(std::string const& text)
    {
        size_t const b = text.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return std::string();
        size_t const e = text.find_last_not_of(" \t\r\n");
        return text.substr(b, e - b + 1);
    }

    std::vector<ConditionState> EvaluateConditions(Player* player, uint32 challengeID)
    {
        std::vector<ConditionState> out;
        std::string conds = ChallengeConditions(challengeID);
        if (conds.empty())
            return out;

        uint32 guid = player->GetGUID().GetCounter();
        // CoAParse::Split drops empty tokens, so a hand-edited definition that
        // ends with ';' no longer produces a nameless entry. That empty token
        // used to reach the "unknown type" branch below, which fails closed:
        // one stray semicolon made the challenge permanently unstartable for
        // the whole realm, with only a LOG_WARN naming type '' to say so.
        for (std::string const& entry : CoAParse::Split(conds, ';'))
        {
            size_t colon = entry.find(':');
            std::string type = TrimSpaces((colon == std::string::npos) ? entry : entry.substr(0, colon));
            if (type.empty())
                continue;   // whitespace-only token, same story as the empty one
            uint32 v1 = 0;
            bool valueReadable = true;
            if (colon != std::string::npos)
            {
                std::string v = entry.substr(colon + 1);
                size_t slash = v.find('/');
                if (slash != std::string::npos)
                    v = v.substr(0, slash);
                v = TrimSpaces(v);
                // An unreadable value is NOT zero. std::stoul used to throw on
                // "5x" or " 5" and the catch put 0 there, which is a different
                // rule: GROUP_SIZE 0 means "must be solo", so a typo silently
                // turned "party of 5 required" into "party forbidden", with no
                // log line at all. CoAParse::ToU32 is strict (no sign, no
                // partial parse, no overflow) and returns 0 on refusal, so a
                // zero that did not come from an all-zero string is a refusal.
                v1 = CoAParse::ToU32(v);
                if (v.empty() || (v1 == 0 && v.find_first_not_of('0') != std::string::npos))
                    valueReadable = false;
            }

            ConditionState s;
            if (!valueReadable)
            {
                // Fail closed, like the unknown-type branch: refusing to start
                // is the safe reading of a definition we could not parse, and
                // inventing a value is what this defect was about. The error
                // line names the challenge and the token so the row can be
                // fixed, and the player is told which condition blocked them.
                s.label = type;
                s.broken = true;
                s.detail = CONDITION_DEF_ERROR_PREFIX + std::string("unreadable value in '") + entry + "'";
                LOG_ERROR("module.coa_challenges",
                    "Challenge {}: condition '{}' has an unreadable value -> blocked",
                    challengeID, entry);
                out.push_back(s);
                continue;
            }
            if (type == "CHALLENGE_CONDITIONS_TYPE_GROUP_SIZE")
            {
                uint32 g = player->GetGroup() ? player->GetGroup()->GetMembersCount() : 1;
                s.label = "GROUP_SIZE";
                if (v1 == 0)
                {
                    // "Your group must have 0 members" -> must be solo.
                    s.broken = (player->GetGroup() != nullptr);
                    s.detail = s.broken
                        ? "requires solo, current group of " + std::to_string(g)
                        : "solo";
                }
                else
                {
                    s.broken = (g != v1);
                    s.detail = "requires " + std::to_string(v1) + ", current " + std::to_string(g);
                }
            }
            else if (type == "CHALLENGE_CONDITIONS_TYPE_HAVE_FREE_INVENTORY_SLOTS")
            {
                uint32 f = FreeInventorySlots(player);
                s.label = "HAVE_FREE_INVENTORY_SLOTS";
                s.broken = (f < v1);
                s.detail = "requires " + std::to_string(v1) + ", current " + std::to_string(f);
            }
            else if (type == "CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION")
            {
                bool looted = HasConditionFlag(guid, "LOOTED");
                s.label = "LOOT_INTERACTION";
                s.broken = looted;
                s.detail = looted ? "LOOTED" : "clean";
            }
            else if (type == "CHALLENGE_CONDITIONS_TYPE_LEVEL_UP")
            {
                // Client tooltip: "Cannot have gained experience" -> the player
                // must not have gained any level (still level 1).
                bool leveled = player->GetLevel() > 1;
                s.label = "LEVEL_UP";
                s.broken = leveled;
                s.detail = "requires level 1, current " + std::to_string(player->GetLevel());
            }
            else if (type == "CHALLENGE_CONDITIONS_TYPE_CANNOT_HAVE_GAINED_EXPERIENCE")
            {
                // Legacy alias (not a real client enum); same meaning as LEVEL_UP.
                bool leveled = player->GetLevel() > 1;
                s.label = "CANNOT_HAVE_GAINED_EXPERIENCE";
                s.broken = leveled;
                s.detail = "requires level 1, current " + std::to_string(player->GetLevel());
            }
            else
            {
                // Unknown condition type: fail closed. An unhandled gate must not
                // silently allow activation.
                s.label = type;
                s.broken = true;
                s.detail = CONDITION_DEF_ERROR_PREFIX + std::string("unhandled condition type");
                LOG_WARN("module.coa_challenges",
                    "Unhandled activation condition type '{}' (challenge {}) -> blocked",
                    type, challengeID);
            }

            out.push_back(s);
        }
        return out;
    }

    // Empty return = OK; otherwise a human-readable reason.
    std::string CheckActivationConditions(Player* player, uint32 challengeID)
    {
        for (ConditionState const& s : EvaluateConditions(player, challengeID))
            if (s.broken)
                return s.label + " (" + s.detail + ")";
        return "";
    }

    // "Pristine": nothing has happened since the challenge was activated — no
    // objective progress, no deaths, and every activation condition still
    // holds. Deactivating while pristine is a free cancel (lets a fresh
    // character undo a wrong pick); otherwise a challenge with lives is failed.
    bool IsPristine(Player* player, uint32 challengeID)
    {
        uint32 guid = player->GetGUID().GetCounter();
        // Party-size gates (DUO/TRIO) are recorded as met at activation; they
        // are not progress, so they must not clear "pristine".
        for (Objective const& o : GetObjectives(challengeID))
        {
            if (o.type == "CHALLENGE_REQUIREMENT_TYPE_DUO"
                || o.type == "CHALLENGE_REQUIREMENT_TYPE_TRIO")
                continue;
            if (IsObjectiveDone(guid, challengeID, o.key))
                return false;
        }
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT deaths FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                guid, challengeID))
        {
            if (r->Fetch()[0].Get<uint32>() > 0)
                return false;
        }
        // Same entry-only exemption as ActivateChallenge: without it, the
        // pristine-character gates a tier-2 holder necessarily breaks would
        // turn a free cancel into a failure. CheckActivationConditions is left
        // alone on purpose -- it answers "can this be STARTED", which is the
        // entry question, and it is what the .coa conditions report shows.
        uint32 const activeLevel = ActiveChallengeLevel(guid, challengeID);
        bool const higherTier = (activeLevel > 1 && ChallengeLevelCount(challengeID) > 1);
        for (ConditionState const& s : EvaluateConditions(player, challengeID))
        {
            if (!s.broken)
                continue;
            // A definition we could not read says nothing about the character.
            // Counting it as "not pristine" here is what would send a cancel
            // on a lives >= 1 challenge straight into FailChallenge. The start
            // stays blocked (ValidateChallenge keeps its refusal) and the
            // error is already logged by EvaluateConditions.
            if (IsDefinitionError(s))
                continue;
            if (higherTier && IsEntryOnlyCondition(s.label))
                continue;
            return false;
        }
        return true;
    }

    // ---- Objectives (Requirements[] level-restricted) --------------------
    // Generated into CoAChallenges.Objectives.<id> as "TYPE:V1/V2/V3;...".
    // V1 = target (creature/item/quest id, or money amount); V2 = the level the
    // objective must be completed BEFORE. COMPLETE_WITHOUT_DEATH is the Lives
    // mechanic; DUO/TRIO are tracked as party-size gates and marked done right
    // after a successful activation (see DoActivateChallenge).
    // EARN_MONEY_BEFORE_LEVEL is validated AT the level-up against the gold the
    // player is carrying at that instant (hoard mechanic): reaching the level
    // without the amount kills the player.

    std::mutex ObjectiveCacheMutex;
    std::unordered_map<uint32, std::vector<Objective>> ObjectiveCache;

    // Returned by value: LoadChallengeDefinitions can clear ObjectiveCache on
    // .coa challenges reload, which would dangle a returned reference.
    std::vector<Objective> GetObjectives(uint32 challengeID)
    {
        std::lock_guard<std::mutex> lock(ObjectiveCacheMutex);
        auto it = ObjectiveCache.find(challengeID);
        if (it != ObjectiveCache.end())
            return it->second;

        std::string objs = DefField<std::string>(challengeID, &ChallengeDef::objectives, "", "");
        return ObjectiveCache.emplace(challengeID, CoAParse::ParseEntries(objs)).first->second;
    }

    void SetObjectiveDone(uint32 guid, uint32 challengeID, std::string const& key)
    {
        std::string ekey = key;
        CharacterDatabase.EscapeString(ekey);
        // Direct (synchronous): the completion check reads right after, and
        // objective completion is rare enough that blocking is fine.
        CharacterDatabase.DirectExecute(
            "INSERT IGNORE INTO coa_character_objective (guid, challengeId, objective) VALUES ({}, {}, '{}')",
            guid, challengeID, ekey);
    }

    bool IsObjectiveDone(uint32 guid, uint32 challengeID, std::string const& key)
    {
        std::string ekey = key;
        CharacterDatabase.EscapeString(ekey);
        return (bool)CharacterDatabase.Query(
            "SELECT 1 FROM coa_character_objective WHERE guid = {} AND challengeId = {} AND objective = '{}' LIMIT 1",
            guid, challengeID, ekey);
    }

    bool IsTrackedObjective(std::string const& type)
    {
        return CoAParse::IsTrackedObjective(type);
    }

    // All tracked objectives done? (challenges with no tracked objectives
    // never auto-complete here).
    bool ObjectivesAllDone(uint32 guid, uint32 challengeID)
    {
        bool any = false;
        for (Objective const& o : GetObjectives(challengeID))
        {
            if (!IsTrackedObjective(o.type))
                continue;
            any = true;
            if (!IsObjectiveDone(guid, challengeID, o.key))
                return false;
        }
        return any;
    }

    // All tracked objectives done, treating "no tracked objectives" as met
    // (challenges whose only requirement is "N Lives" reach this at the cap).
    bool RequirementsMet(uint32 guid, uint32 challengeID)
    {
        for (Objective const& o : GetObjectives(challengeID))
            if (IsTrackedObjective(o.type) && !IsObjectiveDone(guid, challengeID, o.key))
                return false;
        return true;
    }

    // Reaching the level cap completes every active challenge whose
    // requirements are met. Challenges with no tracked objective (e.g. the
    // "1 Life" Hardcore trials) only complete here.
    void CompleteAtLevelCap(Player* player)
    {
        if (!sConfigMgr->GetOption<bool>("CoAChallenges.CompleteAtLevelCap", true))
            return;
        uint32 cap = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        if (!cap || player->GetLevel() < cap)
            return;
        uint32 guid = player->GetGUID().GetCounter();
        for (uint32 cid : ActiveChallenges(guid))
        {
            if (RequirementsMet(guid, cid))
                CompleteChallenge(player, cid);
        }
    }

    // Quest color (client UIParent.lua GetQuestDifficultyColor):
    //   diff = questLevel - playerLevel
    //   >= 5 red, >= 3 orange, >= -2 yellow, >= -greenRange green, else gray.
    // Returns 0 gray, 1 green, 2 yellow, 3 orange, 4 red.
    int QuestColor(uint32 questLevel, uint32 playerLevel)
    {
        int diff = int(questLevel) - int(playerLevel);
        if (diff >= 5) return 4;
        if (diff >= 3) return 3;
        if (diff >= -2) return 2;
        if (-diff <= 5) return 1;
        return 0;
    }

    // NO_LEVEL_PAST_REQUIREMENTS: the lowest level ABOVE the player's current
    // one that still has an unmet tracked objective in an active challenge
    // carrying the rule, or 0 if none. XP must be capped one point short of it,
    // because a single huge gain (e.g. a big XP rate) can cross several levels
    // at once and skip the gate.
    uint32 NextGatedLevel(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        uint32 curLevel = player->GetLevel();
        uint32 gate = 0;
        for (uint32 cid : ActiveChallenges(guid))
        {
            std::string rules = ChallengeRules(cid);
            if (!RuleListContains(rules, "CHALLENGE_RULES_TYPE_NO_LEVEL_PAST_REQUIREMENTS"))
                continue;
            for (Objective const& o : GetObjectives(cid))
            {
                if (!IsTrackedObjective(o.type))
                    continue;
                if (!o.v2 || o.v2 <= curLevel)
                    continue;
                if (IsObjectiveDone(guid, cid, o.key))
                    continue;
                if (!gate || o.v2 < gate)
                    gate = o.v2;
            }
        }
        return gate;
    }

    // Completion: bookkeeping + remove active + CHALLENGE_COMPLETED (0x598).
    void CompleteChallenge(Player* player, uint32 challengeID)
    {
        uint32 guid = player->GetGUID().GetCounter();
        uint32 level = 1;
        uint32 startTime = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT level, startTime FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                guid, challengeID))
        {
            Field* f = r->Fetch();
            level = f[0].Get<uint32>();
            startTime = f[1].Get<uint32>();
        }
        else
        {
            // Never record a bogus level-1/0-duration completion with rewards for
            // a challenge that is not actually active.
            LOG_WARN("module.coa_challenges",
                "CompleteChallenge ignored: challenge {} is not active for {}",
                challengeID, player->GetName());
            return;
        }

        // Rewards marked IsFirstCompletion are only given the first time this
        // level is completed (relevant only when re-completion is allowed).
        bool const firstCompletion = !HasCompletionLevel(guid, challengeID, level);

        // One transaction: recording the completion and dropping the active row
        // are the same state transition. As two separate DirectExecute a crash
        // between them left the challenge both completed and active (or, the
        // other way round, dropped with no completion and no reward).
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            trans->Append(
                "INSERT IGNORE INTO coa_challenge_completion (guid, challengeId, level, completeTime, startTime) "
                "VALUES ({}, {}, {}, UNIX_TIMESTAMP(), {})", guid, challengeID, level, startTime);
            trans->Append(
                "DELETE FROM coa_character_challenge WHERE guid = {} AND challengeId = {}", guid, challengeID);
            CharacterDatabase.DirectCommitTransaction(trans);
        }
        ClearCharChallengeCache(guid);
        RemoveChallengeSpell(player, challengeID);
        RemoveMeterAuras(player);
        RemoveHungerChallenge(player, challengeID);
        // Per-challenge clear + rebuild (see DeactivateChallenge).
        if (IsFatigueChallenge(challengeID))
        {
            ClearFatigueForChallenge(player, challengeID);
            RefreshFatigueTracking(player);
        }
        UntrackSpellbind(player);
        // Rebuild for any OTHER active challenge that still carries a spellbind
        // rule (see DeactivateChallenge).
        RefreshSpellbindTracking(player);
        RefreshInvertedBreathTracking(player);
        RefreshRegenTracking(player);
        RefreshHighRiskTracking(player);
        RefreshLootedTracking(player);
        RecomputeRequiredGameModes(player);

        // 0x598 adds the trial to the client's completed cache but does NOT
        // remove it from the active cache, so the UI would keep showing
        // "deactivate". Re-push the active list (now without this trial).
        SendActiveList(player);
        SendCriteriaState(player);

        GrantChallengeRewards(player, challengeID, level, firstCompletion);

        if (WorldSession* session = player->GetSession())
        {
            // CHALLENGE_COMPLETED record (28 bytes). Handler 0x137F60 hashes
            // the u32 at offset 4 (challengeID) and fires
            // CHALLENGE_COMPLETED(challengeID, level) from the u32 at offset 8
            // (format "%u%u"). Offsets 0/12..27 are unused; zero them.
            WorldPacket data(SMSG_COA_CHALLENGE_COMPLETED, 28);
            data << uint32(0);
            data << uint32(challengeID);
            data << uint32(level);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            session->SendPacket(&data);
        }
        LOG_INFO("module.coa_challenges", "Challenge {} ({}) completed by {} (level {})",
            challengeID, ChallengeName(challengeID), player->GetName(), level);

        AnnounceCompletion(player, challengeID, level);
        if (sConfigMgr->GetOption<bool>("CoAChallenges.SendCompletionAdded", true))
            SendCompletionAdded(player, challengeID, level);

        // Completing the last bundled challenge finishes the active custom trial
        // (record the trial completion + broadcast 0x5CB; no-op otherwise).
        TryCompleteCustomTrial(player, challengeID);
    }

    // Level of an active challenge (1 when not active / on a query miss).
    uint32 ActiveChallengeLevel(uint32 guid, uint32 challengeID)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT level FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                guid, challengeID))
            return r->Fetch()[0].Get<uint32>();
        return 1;
    }

    // Mark matching objectives done for an event value (creature/item/quest/
    // money). Only counts when the objective's level has not been reached yet.
    void MarkObjectives(Player* player, char const* type, uint32 eventValue)
    {
        uint32 guid = player->GetGUID().GetCounter();
        for (uint32 cid : ActiveChallenges(guid))
        {
            bool marked = false;
            uint32 level = 0;
            for (Objective const& o : GetObjectives(cid))
            {
                if (o.type != type || o.v1 != eventValue)
                    continue;
                if (o.v2 && player->GetLevel() >= o.v2)
                    continue;
                if (!IsObjectiveDone(guid, cid, o.key))
                {
                    SetObjectiveDone(guid, cid, o.key);
                    LOG_INFO("module.coa_challenges", "{} completed objective {} (challenge {})",
                        player->GetName(), o.key, cid);
                    // Per-objective banner (SMSG 0x59A): the 0 -> nonzero
                    // progress transition makes the client fire
                    // CHALLENGE_CRITERIA_COMPLETED. Requires the 0x599 list to
                    // have seeded this criterion first (sent on activation).
                    if (!level)
                        level = ActiveChallengeLevel(guid, cid);
                    SendCriteriaUpdatedForObjective(player, cid, level, o, true);
                    marked = true;
                }
            }
            if (marked && ObjectivesAllDone(guid, cid))
                CompleteChallenge(player, cid);
        }
    }

    // Short labels for the failure broadcast when a level-restricted objective
    // is missed (the player is killed for it).
    std::string ObjectiveFailLabel(std::string const& type)
    {
        if (type == "CHALLENGE_REQUIREMENT_TYPE_EARN_MONEY_BEFORE_LEVEL")     return "Greedy";
        if (type == "CHALLENGE_REQUIREMENT_TYPE_KILL_CREATURE_BEFORE_LEVEL")  return "Hunt Failed";
        if (type == "CHALLENGE_REQUIREMENT_TYPE_LOOT_ITEM_BEFORE_LEVEL")      return "Loot Failed";
        if (type == "CHALLENGE_REQUIREMENT_TYPE_COMPLETE_QUEST_BEFORE_LEVEL") return "Quest Failed";
        if (type == "CHALLENGE_REQUIREMENT_TYPE_DUO")  return "Duo Broken";
        if (type == "CHALLENGE_REQUIREMENT_TYPE_TRIO") return "Trio Broken";
        return "Failed Objective";
    }

    // On level up: every tracked objective whose V2 level has been reached is
    // validated. Money objectives pass only if the player is CARRYING the
    // required amount right now; anything still unmet kills the player (which
    // fails the challenge). Passed money milestones are recorded.
    // Returns false when an unmet level-restricted objective killed the player
    // (so the caller must not run the level-cap completion on a dying char).
    bool CheckObjectiveLevels(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        for (uint32 cid : ActiveChallenges(guid))
        {
            // Level-gated trials (NO_LEVEL_PAST_REQUIREMENTS) cap XP one level
            // short of every gate, so a normal player can never stand ON a gate
            // level with its objective unmet: the missed-objective death is
            // unreachable and must not fire (e.g. for a GM-forced level-up).
            std::string const rules = ChallengeRules(cid);
            bool const levelGated =
                rules.find("CHALLENGE_RULES_TYPE_NO_LEVEL_PAST_REQUIREMENTS") != std::string::npos;

            bool marked = false;
            uint32 level = 0;
            for (Objective const& o : GetObjectives(cid))
            {
                if (!IsTrackedObjective(o.type))
                    continue;
                if (!o.v2 || player->GetLevel() < o.v2)
                    continue;
                if (IsObjectiveDone(guid, cid, o.key))
                    continue;

                if (o.type == "CHALLENGE_REQUIREMENT_TYPE_EARN_MONEY_BEFORE_LEVEL"
                    && player->GetMoney() >= o.v1)
                {
                    SetObjectiveDone(guid, cid, o.key);
                    LOG_INFO("module.coa_challenges", "{} completed objective {} (challenge {}) at level {}",
                        player->GetName(), o.key, cid, player->GetLevel());
                    if (!level)
                        level = ActiveChallengeLevel(guid, cid);
                    SendCriteriaUpdatedForObjective(player, cid, level, o, true);
                    marked = true;
                    continue;
                }

                if (levelGated)
                    continue;

                LOG_INFO("module.coa_challenges", "{} missed objective {} (challenge {}) at level {}",
                    player->GetName(), o.key, cid, player->GetLevel());
                SetDeathCause(player, KillerKind::Mechanic, 0, ObjectiveFailLabel(o.type));
                player->EnvironmentalDamage(DAMAGE_EXHAUSTED, player->GetMaxHealth());
                return false;
            }

            if (marked && ObjectivesAllDone(guid, cid))
                CompleteChallenge(player, cid);
        }
        return true;
    }

    // If the failing challenge belongs to the character's active custom trial,
    // the whole trial is over: tear down every remaining bundled challenge and
    // clear the client's active-trial state (SMSG 0x5B1 empty).
    void EndActiveCustomTrialFor(Player* player, uint32 challengeID)
    {
        uint32 guid = player->GetGUID().GetCounter();
        std::string trialID = GetActiveCustomTrial(guid);
        if (trialID.empty())
            return;
        uint32 ownerGuid = TrialOwnerGuid(trialID);
        if (!ownerGuid)
        {
            SetActiveCustomTrial(player, "");
            return;
        }

        std::vector<uint32> bundled;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId FROM coa_custom_trial_entry WHERE guid = {} AND trialId = '{}'",
                ownerGuid, trialID))
            do { bundled.push_back(r->Fetch()[0].Get<uint32>()); } while (r->NextRow());

        if (std::find(bundled.begin(), bundled.end(), challengeID) == bundled.end())
            return;   // the failed challenge is not part of the active trial

        std::set<uint32> actives = ActiveChallenges(guid);
        for (uint32 cid : bundled)
            if (cid != challengeID && actives.count(cid))
                DeactivateChallenge(player, cid);

        SetActiveCustomTrial(player, "");
        SendActiveList(player);
        SendCriteriaState(player);
        LOG_INFO("module.coa_challenges", "Custom trial {} ended for {} (challenge {} failed)",
            trialID, player->GetName(), challengeID);
    }

    // Full fail sequence: bookkeeping + client updates + aura removal.
    // killerSource: guid whose death carries the killer for the broadcast
    // (defaults to the failing player; shared fate passes the member who died).
    void FailChallenge(Player* player, uint32 challengeID, uint32 level, uint32 deaths,
        ObjectGuid const& killerSource)
    {
        uint32 guid = player->GetGUID().GetCounter();
        // Capture the active custom-trial identity (if any) BEFORE any teardown,
        // so the failure announcement is attributed to the trial.
        std::string trialDisplayName, trialDisplayIcon;
        ActiveTrialDisplayFor(player, challengeID, trialDisplayName, trialDisplayIcon);
        // INSERT IGNORE + synchronous: the PK (guid, challengeId) makes a repeat
        // fail a no-op, and a sync write means HasFailure/HasPermaDeathFailure
        // (read in the same tick) and the client re-push see the row immediately.
        // Both statements are the same state transition, so they commit together
        // rather than as two independent DirectExecute.
        FailChallengeDbOnly(guid, challengeID, level, deaths);
        RemoveChallengeSpell(player, challengeID);
        RemoveMeterAuras(player);
        RemoveHungerChallenge(player, challengeID);
        // Per-challenge clear + rebuild (see DeactivateChallenge).
        if (IsFatigueChallenge(challengeID))
        {
            ClearFatigueForChallenge(player, challengeID);
            RefreshFatigueTracking(player);
        }
        UntrackSpellbind(player);
        // Rebuild for any OTHER active challenge that still carries a spellbind
        // rule (see DeactivateChallenge).
        RefreshSpellbindTracking(player);
        RefreshInvertedBreathTracking(player);
        RefreshRegenTracking(player);
        RefreshHighRiskTracking(player);
        RefreshLootedTracking(player);
        RecomputeRequiredGameModes(player);

        SendFailureAdded(player, challengeID, level);
        // Remove it from the client's active cache (otherwise the UI keeps
        // showing "Deactivate" for a challenge the server already dropped).
        SendActiveList(player);
        SendCriteriaState(player);
        SendChallengeResponse(player, SMSG_COA_CHALLENGE_STOP_RESPONSE,
            challengeID, level, 0, "CHALLENGE_STOP_OK");

        // A failed challenge that is part of an active custom trial ends the
        // whole trial (drop the other bundled challenges + clear active-trial).
        EndActiveCustomTrialFor(player, challengeID);

        LOG_INFO("module.coa_challenges", "Challenge {} ({}) failed for {} (deaths={})",
            challengeID, ChallengeName(challengeID), player->GetName(), deaths);

        // Queue the realm-wide failure announcement (flushed next world tick,
        // after OnPlayerKilledByCreature / OnPlayerPVPKill may have filled in
        // the killer).
        {
            uint32 sourceGuid = (killerSource.IsEmpty() ? player->GetGUID() : killerSource).GetCounter();
            PendingFail pending;
            pending.challengeID = challengeID;
            pending.level = level;
            pending.displayName = trialDisplayName;
            pending.displayIcon = trialDisplayIcon;
            {
                std::lock_guard<std::mutex> lock(LastKillerMutex);
                auto it = LastKiller.find(sourceGuid);
                if (it != LastKiller.end())
                {
                    pending.killerKind = it->second.kind;
                    pending.killerEntry = it->second.entry;
                    pending.killerName = it->second.name;
                }
            }
            std::lock_guard<std::mutex> lock(PendingFailMutex);
            PendingFailBroadcast[guid] = pending;
        }
    }

    // Shared fate (Duo/Trio/Vitality): one holder's death fails EVERY holder
    // in the party, including the dead player. Returns failed challenge IDs.
    void FailSharedFate(Player* dead, uint32 challengeID)
    {
        Group* group = dead->GetGroup();
        if (!group)
        {
            // Solo holder: fails alone (party of one shares fate with itself).
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT level, deaths FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                    dead->GetGUID().GetCounter(), challengeID))
            {
                Field* f = r->Fetch();
                FailChallenge(dead, challengeID, f[0].Get<uint32>(), f[1].Get<uint32>() + 1,
                    dead->GetGUID());
            }
            return;
        }

        for (Group::MemberSlotList::const_iterator itr = group->GetMemberSlots().begin();
             itr != group->GetMemberSlots().end(); ++itr)
        {
            Player* member = ObjectAccessor::FindPlayer(itr->guid);
            if (!member)
                continue;
            uint32 mguid = member->GetGUID().GetCounter();
            QueryResult r = CharacterDatabase.Query(
                "SELECT level, deaths FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                mguid, challengeID);
            if (!r)
                continue;
            Field* f = r->Fetch();
            uint32 const level = f[0].Get<uint32>();
            uint32 deaths = f[1].Get<uint32>() + (member == dead ? 1 : 0);
            if (member == dead)
                CharacterDatabase.Execute(
                    "UPDATE coa_character_challenge SET deaths = {} WHERE guid = {} AND challengeId = {}",
                    deaths, mguid, challengeID);
            // P-034: OnPlayerJustDied runs on the DEAD player's map thread.
            // FailChallenge strips and re-casts auras and pushes packets; doing
            // that to a holder updated by another map thread races his aura
            // list. Persist the transition now (no Player touched) and replay
            // the visible half on his own thread.
            if (member != dead && !OnSameMapThread(member, dead))
            {
                FailChallengeDbOnly(mguid, challengeID, level, deaths);
                QueueRemoteFail(itr->guid, challengeID, level, deaths, dead->GetGUID());
                continue;
            }
            FailChallenge(member, challengeID, level, deaths, dead->GetGUID());
        }
    }

    // Leaving/disbanding a group fails SharedFate (Duo/Trio) challenges: the
    // leaver fails, and by shared fate so do the remaining holders. `extra` is
    // the member already removed from `group` (nullptr for a disband).
    void FailSharedFateHolders(Group* group, Player* extra)
    {
        if (!sConfigMgr->GetOption<bool>("CoAChallenges.Enable", true))
            return;

        // GUIDs, not Player*: nothing here may outlive the lookup, and the
        // failure below re-reaches the player through ObjectAccessor.
        std::vector<std::pair<ObjectGuid, uint32>> targets;
        auto collect = [&targets](Player* member)
        {
            if (!member)
                return;
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT challengeId FROM coa_character_challenge WHERE guid = {}",
                    member->GetGUID().GetCounter()))
            {
                do
                {
                    uint32 cid = r->Fetch()[0].Get<uint32>();
                    if (IsSharedFate(cid))
                        targets.emplace_back(member->GetGUID(), cid);
                } while (r->NextRow());
            }
        };

        if (group)
            for (Group::MemberSlotList::const_iterator itr = group->GetMemberSlots().begin();
                 itr != group->GetMemberSlots().end(); ++itr)
                collect(ObjectAccessor::FindPlayer(itr->guid));
        collect(extra);

        // P-034: a GroupScript hook carries no map of its own. CMSG_GROUP_UNINVITE
        // and CMSG_GROUP_DISBAND are PROCESS_THREADUNSAFE (Opcodes.cpp:248, 254),
        // i.e. the world thread, but OnRemoveMember is also reached from map-side
        // code, and the hook hands us a GUID, not the Player the caller owns. No
        // holder here is provably on our thread, so EVERY one of them gets the
        // persisted transition now and the visible half on his own update tick.
        for (auto const& [mguid, cid] : targets)
        {
            QueryResult r = CharacterDatabase.Query(
                "SELECT level, deaths FROM coa_character_challenge WHERE guid = {} AND challengeId = {}",
                mguid.GetCounter(), cid);
            if (!r)
                continue;
            Field* f = r->Fetch();
            uint32 const level = f[0].Get<uint32>();
            uint32 const deaths = f[1].Get<uint32>();
            FailChallengeDbOnly(mguid.GetCounter(), cid, level, deaths);
            QueueRemoteFail(mguid, cid, level, deaths, ObjectGuid::Empty);
        }
    }

    // GM test hook: exercise the group-leave failure path for a solo player
    // (group == nullptr still fails the leaver's SharedFate challenge).
    void Test_FailSharedFateOnLeave(Player* player)
    {
        FailSharedFateHolders(nullptr, player);
    }

    void HandlePlayerDeath(Player* player)
    {
        if (!sConfigMgr->GetOption<bool>("CoAChallenges.Enable", true))
            return;
        if (!sConfigMgr->GetOption<bool>("CoAChallenges.RespondToDeath", true))
            return;

        uint32 guid = player->GetGUID().GetCounter();
        std::set<uint32> actives = ActiveChallenges(guid);

        if (QueryResult result = CharacterDatabase.Query(
                "SELECT challengeId, level, deaths FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do
            {
                Field* f = result->Fetch();
                uint32 challengeID = f[0].Get<uint32>();
                uint32 level = f[1].Get<uint32>();

                // Shared fate overrides lives counting: one death fails all.
                if (IsSharedFate(challengeID))
                {
                    FailSharedFate(player, challengeID);
                    continue;
                }

                // Total lives come from the client definition (WITHOUT_DEATH
                // MiscValue1; see CoAExport). Absent = deaths don't affect it.
                uint32 lives = LivesTotal(challengeID);
                if (!lives)
                    continue;

                uint32 deaths = f[2].Get<uint32>() + 1;

                // Synchronous: the completion/fail decision below must not race
                // the death counter write (see also SaveModeDeaths).
                CharacterDatabase.DirectExecute(
                    "UPDATE coa_character_challenge SET deaths = {} WHERE guid = {} AND challengeId = {}",
                    deaths, guid, challengeID);

                SendDeathUpdate(player, challengeID, level, deaths);

                if (deaths >= lives)
                    FailChallenge(player, challengeID, level, deaths);
            } while (result->NextRow());
        }

        // Game mode scope: a mode on WITHOUT its base challenge loses lives too
        // (same counter aura; persisted in coa_character_gamemode_lives).
        uint32 mask = CachedGameModeMask(guid);
        for (auto const& [bit, base] : GameModeBaseSnapshot())
        {
            if (!(mask & bit) || actives.count(base))
                continue;
            uint32 lives = LivesTotal(base);
            if (!lives)
                continue;

            uint32 deaths = LoadModeDeaths(guid, bit) + 1;
            SaveModeDeaths(guid, bit, deaths);
            LOG_INFO("module.coa_challenges", "Mode 0x{:X} death {} / {} for {}",
                bit, deaths, lives, player->GetName());

            if (deaths >= lives)
            {
                mask &= ~bit;
                SaveGameModeMask(guid, mask);
                // The mode failed on its own: forget the player's toggle so a
                // later recompute does not re-enable the dead mode.
                ClearPlayerToggleBit(guid, bit);
                RemoveChallengeSpell(player, base);
                if (bit == GAMEMODE_SURVIVALIST)
                    RemoveHungerChallenge(player, SURVIVALIST_HUNGER_ID);
                SendGameModeState(player, mask);
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "Your {} challenge has failed.", GameModeNameForBit(bit));
            }
        }
    }
} // namespace CoAChallenges
