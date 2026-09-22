// mod-coa-challenges (review split): CoA.Challenges.Hunger.cpp
// Mechanical split of review-CoAChallenges.cpp; no logic changes.
#include "CoA.Challenges.Review.h"

namespace CoAChallenges
{

    std::mutex HungerMutex;
    std::unordered_map<uint32, HungerClock> HungerAccum;
    std::unordered_set<uint32> HungerGuids;

    // Lock-free mirror of HungerGuids.size(), so HungerUpdate (called for every
    // player AND every bot on every map tick) can bail out without taking the
    // global mutex when nobody on the realm has a hunger/thirst challenge.
    // Every mutation of HungerGuids publishes it while holding HungerMutex.
    static std::atomic<size_t> HungerTrackedCount{ 0 };
    static void PublishHungerTrackedLocked()
    {
        HungerTrackedCount.store(HungerGuids.size(), std::memory_order_relaxed);
    }

    bool IsHungerChallenge(uint32 challengeID)
    {
        if (challengeID == SURVIVALIST_HUNGER_ID)
            return true;
        // DB-only, and the 3-argument form: building a
        // "CoAChallenges.Survivalist.<id>" key here would just allocate on a
        // per-tick, per-challenge hot path for a value nothing reads.
        return DefField<bool>(challengeID, &ChallengeDef::survivalist, false);
    }

    // Mark the player's counters clean and copy them out. HungerMutex held.
    void TakeHungerFlushLocked(uint32 guid, std::vector<HungerFlushRow>& out)
    {
        auto it = HungerAccum.find(guid);
        if (it == HungerAccum.end() || !it->second.dirty)
            return;
        for (auto const& [challengeID, st] : it->second.state)
            out.push_back(HungerFlushRow{ challengeID, st.hunger, st.thirst });
        it->second.dirty = false;
    }

    void CommitHungerFlush(uint32 guid, std::vector<HungerFlushRow> const& rows)
    {
        if (rows.empty())
            return;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        for (auto const& row : rows)
        {
            if (row.challengeID == SURVIVALIST_HUNGER_ID)
                trans->Append(
                    "REPLACE INTO coa_character_survival (guid, hunger, thirst) VALUES ({}, {}, {})",
                    guid, row.hunger, row.thirst);
            else
                trans->Append(
                    "UPDATE coa_character_challenge SET hunger = {}, thirst = {} WHERE guid = {} AND challengeId = {}",
                    row.hunger, row.thirst, guid, row.challengeID);
        }
        CharacterDatabase.CommitTransaction(trans);
    }

    // Activation REPLACEs the row with hunger/thirst = 0; seed the cache.
    void TrackHunger(Player* player, uint32 challengeID)
    {
        if (!IsHungerChallenge(challengeID))
            return;
        uint32 guid = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(HungerMutex);
        HungerGuids.insert(guid);
        PublishHungerTrackedLocked();
        HungerAccum[guid].state[challengeID] = HungerState{ 0, 0 };
    }

    // Survivalist gamemode: start (or resume) global hunger for this player.
    void TrackSurvivalist(Player* player)
    {
        if (!player)
            return;
        uint32 guid = player->GetGUID().GetCounter();
        int32 hunger = 0, thirst = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT hunger, thirst FROM coa_character_survival WHERE guid = {}", guid))
        {
            Field* f = r->Fetch();
            hunger = f[0].Get<int32>();
            thirst = f[1].Get<int32>();
        }
        std::lock_guard<std::mutex> lock(HungerMutex);
        HungerGuids.insert(guid);
        PublishHungerTrackedLocked();
        HungerAccum[guid].state[SURVIVALIST_HUNGER_ID] = HungerState{ hunger, thirst };
    }

    void UntrackHunger(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        std::vector<HungerFlushRow> rows;
        {
            std::lock_guard<std::mutex> lock(HungerMutex);
            TakeHungerFlushLocked(guid, rows);
            HungerGuids.erase(guid);
            PublishHungerTrackedLocked();
            HungerAccum.erase(guid);
        }
        CommitHungerFlush(guid, rows);
    }

    // Drop the in-memory hunger cache for a character without flushing stale
    // values (used by the GM test reset).
    void Test_ClearHungerCache(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(HungerMutex);
        HungerGuids.erase(guid);
        PublishHungerTrackedLocked();
        HungerAccum.erase(guid);
    }

    void WarnHunger(Player* player, char const* kind, int32 value)
    {
        if (WorldSession* session = player->GetSession())
        {
            // PSendSysMessage forwards its first argument to fmt as a FORMAT
            // string: a pre-formatted buffer would make any '{' coming from a
            // name or a DB label throw inside fmt. Let fmt do the formatting.
            ChatHandler(session).PSendSysMessage(
                "Challenge: your {} is critical ({})! Eat or drink soon.", kind, value);
        }
    }

    // Per-player ticker (map thread). The counters are authoritative in
    // memory and flushed to the DB periodically / on logout. Rise: thirst
    // +1/10s, hunger +1/16s. Fall: while the eat/drink regen aura is present,
    // -FoodRestore / -DrinkRestore every FoodTickMs / DrinkTickMs (default
    // 1 point each; drinking ticks ~3x faster than eating).
    // Tracked set is maintained on activate/stop/fail/login/logout.
    void HungerUpdate(Player* player, uint32 diff)
    {
        // Nobody tracked: one relaxed atomic load and out. Everything below
        // (five config lookups, two aura-list scans, the global mutex) used to
        // run for every player and every bot on every tick.
        if (HungerTrackedCount.load(std::memory_order_relaxed) == 0)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        {
            // Cheap membership pre-check, before the config reads and the aura
            // scans. The authoritative check stays in the main locked block
            // below: the set can change between the two.
            std::lock_guard<std::mutex> lock(HungerMutex);
            if (HungerGuids.find(guid) == HungerGuids.end())
                return;
        }

        uint32 foodSpell = HungerFoodSpell();
        uint32 drinkSpell = HungerDrinkSpell();
        // Bounded like FatigueMax (Fatigue.cpp): 0 would clamp every counter to
        // 0 and make `hunger >= max` true on the first tick (death loop), and a
        // value above INT32_MAX would make (int32)maxV negative, i.e. std::clamp
        // called with hi < lo -> undefined behaviour.
        uint32 maxV = std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("CoAChallenges.HungerMax", 100), 1u, 100000u);
        uint32 foodRestore = sConfigMgr->GetOption<uint32>("CoAChallenges.FoodRestore", 1);
        uint32 drinkRestore = sConfigMgr->GetOption<uint32>("CoAChallenges.DrinkRestore", 1);
        bool eating = player->HasAuraType(SPELL_AURA_MOD_REGEN);
        bool drinking = player->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);

        struct Changed { uint32 challengeID; int32 hunger; int32 thirst; };
        std::vector<Changed> changed;
        std::vector<HungerFlushRow> flushRows;
        bool starved = false;
        bool flush = false;
        bool thirstTick = false, hungerTick = false;

        {
            std::lock_guard<std::mutex> lock(HungerMutex);
            if (HungerGuids.find(guid) == HungerGuids.end())
                return;
            HungerClock& c = HungerAccum[guid];

            c.thirstMs += diff;
            c.hungerMs += diff;
            c.foodMs += diff;
            c.drinkMs += diff;
            c.flushMs += diff;

            uint32 thirstMs = sConfigMgr->GetOption<uint32>("CoAChallenges.ThirstSecondsPerPoint", 10) * 1000;
            uint32 hungerMs = sConfigMgr->GetOption<uint32>("CoAChallenges.HungerSecondsPerPoint", 16) * 1000;
            uint32 foodTickMs = sConfigMgr->GetOption<uint32>("CoAChallenges.FoodTickMs", HUNGER_FOOD_TICK_MS);
            uint32 drinkTickMs = sConfigMgr->GetOption<uint32>("CoAChallenges.DrinkTickMs", HUNGER_DRINK_TICK_MS);
            bool foodTick = false, drinkTick = false;
            if (c.thirstMs >= thirstMs)
            {
                c.thirstMs -= thirstMs;
                thirstTick = true;
            }
            if (c.hungerMs >= hungerMs)
            {
                c.hungerMs -= hungerMs;
                hungerTick = true;
            }
            if (c.foodMs >= foodTickMs)
            {
                c.foodMs -= foodTickMs;
                foodTick = true;
            }
            if (c.drinkMs >= drinkTickMs)
            {
                c.drinkMs -= drinkTickMs;
                drinkTick = true;
            }
            if (c.flushMs >= HUNGER_FLUSH_MS)
            {
                c.flushMs -= HUNGER_FLUSH_MS;
                flush = true;
            }

            if ((thirstTick || hungerTick || foodTick || drinkTick)
                && player->IsAlive() && player->IsInWorld())
            {
                for (auto& [challengeID, st] : c.state)
                {
                    if (!IsHungerChallenge(challengeID))
                        continue;
                    int32 oldHunger = st.hunger, oldThirst = st.thirst;

                    if (hungerTick && !eating)
                        st.hunger += 1;
                    if (thirstTick && !drinking)
                        st.thirst += 1;
                    if (foodTick && eating)
                        st.hunger -= (int32)foodRestore;
                    if (drinkTick && drinking)
                        st.thirst -= (int32)drinkRestore;

                    st.hunger = std::clamp(st.hunger, 0, (int32)maxV);
                    st.thirst = std::clamp(st.thirst, 0, (int32)maxV);

                    if (st.hunger >= (int32)maxV || st.thirst >= (int32)maxV)
                        starved = true;

                    if (st.hunger != oldHunger || st.thirst != oldThirst)
                    {
                        c.dirty = true;
                        changed.push_back(Changed{ challengeID, st.hunger, st.thirst });
                    }
                }
            }

            if (starved || flush)
                TakeHungerFlushLocked(guid, flushRows);
        }

        CommitHungerFlush(guid, flushRows);

        // Warn at 75% / 90% of the configured max (not the literals 75/90, which
        // only line up when HungerMax == 100).
        int32 const warn75 = int32(maxV * 3 / 4);
        int32 const warn90 = int32(maxV * 9 / 10);
        for (auto const& ch : changed)
        {
            SetMeterAura(player, foodSpell, ch.hunger);
            SetMeterAura(player, drinkSpell, ch.thirst);
            if (hungerTick && (ch.hunger == warn75 || ch.hunger == warn90))
                WarnHunger(player, "hunger", ch.hunger);
            if (thirstTick && (ch.thirst == warn75 || ch.thirst == warn90))
                WarnHunger(player, "thirst", ch.thirst);
        }

        if (starved)
        {
            LOG_INFO("module.coa_challenges", "Player {} starved (hunger/thirst at max)",
                player->GetName());
            // Exhaustion damage (not raw Kill) so the death recap shows
            // a cause. No durability loss (only FALL applies it).
            SetDeathCause(player, KillerKind::Mechanic, 0, "Starved");
            player->EnvironmentalDamage(DAMAGE_EXHAUSTED, player->GetMaxHealth());
        }
    }

    // Re-sync tracking after login. Loads the active DB row(s) into the
    // in-memory cache; untracks when no hunger challenge remains.
    void RefreshHungerTracking(Player* player)
    {
        if (!player)
            return;
        uint32 guid = player->GetGUID().GetCounter();

        // Build the fresh state (DB reads) BEFORE taking HungerMutex: holding the
        // lock across synchronous queries stalls every other player's hunger tick.
        HungerClock fresh;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT challengeId, hunger, thirst FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 challengeID = f[0].Get<uint32>();
                if (IsHungerChallenge(challengeID))
                    fresh.state[challengeID] = HungerState{ f[1].Get<int32>(), f[2].Get<int32>() };
            } while (r->NextRow());
        }
        // Survivalist gamemode: run hunger globally for this character. Read the
        // cached mask (the login recompute just wrote it) instead of re-querying
        // the async-updated row.
        if (CachedGameModeMask(guid) & GAMEMODE_SURVIVALIST)
        {
            int32 hunger = 0, thirst = 0;
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT hunger, thirst FROM coa_character_survival WHERE guid = {}", guid))
            {
                Field* f = r->Fetch();
                hunger = f[0].Get<int32>();
                thirst = f[1].Get<int32>();
            }
            fresh.state[SURVIVALIST_HUNGER_ID] = HungerState{ hunger, thirst };
        }

        std::lock_guard<std::mutex> lock(HungerMutex);
        if (fresh.state.empty())
        {
            HungerGuids.erase(guid);
            HungerAccum.erase(guid);
        }
        else
        {
            HungerAccum[guid] = std::move(fresh);
            HungerGuids.insert(guid);
        }
        PublishHungerTrackedLocked();
    }

    // Drop ONE challenge from the cache (stop/fail). Must not re-read the DB:
    // the active row is deleted via async Execute, so a follow-up SELECT can
    // still see it and resurrect a stale (maxed) counter -> death loop.
    void RemoveHungerChallenge(Player* player, uint32 challengeID)
    {
        if (!IsHungerChallenge(challengeID))
            return;
        uint32 guid = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(HungerMutex);
        auto it = HungerAccum.find(guid);
        if (it == HungerAccum.end())
            return;
        it->second.state.erase(challengeID);
        if (it->second.state.empty())
        {
            HungerGuids.erase(guid);
            PublishHungerTrackedLocked();
            HungerAccum.erase(guid);
        }
    }

    // Whether the character still has ANY hunger counter tracked. The meter
    // auras are shared by every hunger challenge, so a caller that dropped one
    // of them must ask before taking the icons away.
    bool HasTrackedHunger(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(HungerMutex);
        return HungerGuids.find(guid) != HungerGuids.end();
    }

    // Apply the meter aura stacks from the cached values. Needed right after
    // login/resurrect: HungerUpdate only pushes an aura when the value
    // changes, so without this the stacks would lag until the first tick.
    void SyncMeterAuras(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        int32 hunger = 0, thirst = 0;
        {
            std::lock_guard<std::mutex> lock(HungerMutex);
            auto it = HungerAccum.find(guid);
            if (it == HungerAccum.end())
                return;
            for (auto const& [challengeID, st] : it->second.state)
            {
                hunger = std::max(hunger, st.hunger);
                thirst = std::max(thirst, st.thirst);
            }
        }
        SetMeterAura(player, HungerFoodSpell(), hunger);
        SetMeterAura(player, HungerDrinkSpell(), thirst);
    }
} // namespace CoAChallenges
