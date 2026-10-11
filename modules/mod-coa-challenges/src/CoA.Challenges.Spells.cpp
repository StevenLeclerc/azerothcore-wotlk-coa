// mod-coa-challenges (review split): CoA.Challenges.Spells.cpp
// Mechanical split of review-CoAChallenges.cpp; no logic changes.
#include "CoA.Challenges.Review.h"
#include "AscensionChallengePveDamageDone.h"
#include "CoA.Challenges.AdventureHealthData.h"
#include "Creature.h"
#include "SpellAuraEffects.h"
#include "QuestDef.h"
#include "TemporarySummon.h"
#include <cmath>
#include <limits>
#include "AscensionCreatureOnlyDamageTaken.h"

namespace CoAChallenges
{

    char const* ChallengeResponseString(uint32 code)
    {
        switch (code)
        {
            case 0:  return "CHALLENGE_START_OK";
            case 1:  return "CHALLENGE_START_NOT_FOUND";
            case 2:  return "CHALLENGE_START_NO_REWARDS";
            case 3:  return "CHALLENGE_START_WRONG_GAME_MODE";
            case 4:  return "CHALLENGE_START_ALREADY_ACTIVE";
            case 5:  return "CHALLENGE_START_EXCLUSIVE_GROUP_ACTIVE";
            case 6:  return "CHALLENGE_START_GAME_EVENT_NOT_ACTIVE";
            case 7:  return "CHALLENGE_START_DISABLED";
            case 8:  return "CHALLENGE_START_CONDITIONS_NOT_MET";
            case 9:  return "CHALLENGE_START_PREVIOUS_LEVEL_NOT_COMPLETED";
            case 10: return "CHALLENGE_START_OUTSIDE_INTERACTION";
            case 11: return "CHALLENGE_START_NOT_PRESTIGE";
            case 12: return "CHALLENGE_START_RULE_BROKEN";
            case 13: return "CHALLENGE_START_PARTICIPATING_IN_TRIAL";
            case 14: return "CHALLENGE_START_PARTICIPATING_IN_CHALLENGE";
            default: return "CHALLENGE_START_DISABLED";
        }
    }

    void SendChallengeResponse(Player* player, uint16 opcode, uint32 challengeID, uint32 level, uint32 code, std::string const& respStr)
    {
        WorldSession* session = player->GetSession();
        if (!session)
            return;

        WorldPacket data(opcode, 64);
        data << uint32(challengeID);
        data << uint32(level);
        data << uint32(code);
        data << uint32(0);
        data << uint32(0);
        data << uint8(0);
        AppendConfigString(data, respStr);

        session->SendPacket(&data);
        LOG_INFO("module.coa_challenges", "Sent SMSG 0x{:X} to {}: challengeID={} level={} code={}",
            opcode, player->GetName(), challengeID, level, code);
    }

    // Active-list push: u32 n + n x 26-byte entries. Fires
    // CHALLENGE_ACTIVE_LIST_CHANGED (no sound).
    //
    // Layout decoded from Extensions.dll (INSERT 0x12FAB0 + LOOKUP 0x8EFA0 +
    // IsChallengeActive 0x142CD0 + handler 0x138050):
    //   wire[0]  u32: baggage (0x593 path stores player-guid-ish here; unchecked)
    //   wire[4]  u32: CHALLENGE ID — map key (FNV-1a hashed) + node+0x10
    //   wire[8]  u32: LEVEL — node+0x14, returned by IsChallengeActive
    //   wire[12] u32: low byte must be 1 (0x593 path hardcodes node+0x18 = 1)
    //   rest zeroed (matches a live 0x593-tested activation of 188/1).
    // Challenge buff spells, from the live client definitions (CoAExport).
    // Per challenge:  CoAChallenges.Spell.<id>          = union across levels
    // Multi-level:    CoAChallenges.SpellLevels.<id>    = 1
    //                 CoAChallenges.Spell.<id>.<level>  = that level's auras
    // When the SpellLevels marker is set the module uses the level entry (an
    // empty level means "no aura for this level") and never the union; without
    // it (single-level / legacy config) it uses the union.
    // NOTE: manual cancel needs no code guard — challenge auras
    // carry NO_AURA_CANCEL in Spell.dbc (core + client both refuse).
    static std::vector<uint32> ParseSpellList(std::string raw)
    {
        std::vector<uint32> out;
        size_t hash = raw.find('#');
        if (hash != std::string::npos)
            raw = raw.substr(0, hash);
        size_t start = 0;
        while (start < raw.size())
        {
            size_t end = raw.find(';', start);
            if (end == std::string::npos)
                end = raw.size();
            std::string tok = raw.substr(start, end - start);
            size_t b = tok.find_first_not_of(" \t\r\n");
            size_t e = tok.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
            {
                tok = tok.substr(b, e - b + 1);
                // A malformed id used to be swallowed here: the challenge then
                // activated with one aura silently missing, which is a different
                // challenge from the one the player thinks he is playing.
                try { out.push_back(uint32(std::stoul(tok))); }
                catch (...)
                {
                    LOG_ERROR("module.coa_challenges",
                        "Unreadable spell id '{}' in a challenge spell list, ignored", tok);
                }
            }
            start = end + 1;
        }
        return out;
    }

    std::vector<uint32> GetChallengeSpells(uint32 challengeID, uint32 level)
    {
        std::string raw;
        {
            std::lock_guard<std::mutex> lock(DefMutex);
            auto it = DefCache.find(challengeID);
            if (it != DefCache.end())
            {
                auto const& sp = it->second.spells;
                auto unionSp = sp.find(0);
                if (level > 0)
                {
                    auto lv = sp.find(level);
                    if (lv != sp.end())
                        raw = lv->second;                 // per-level (may be empty)
                    else if (unionSp != sp.end())
                        raw = unionSp->second;            // no that level -> union
                }
                else if (unionSp != sp.end())
                {
                    raw = unionSp->second;
                }
            }
        }

        // DB-only: auras come from coa_challenge_spell (no conf fallback).
        return ParseSpellList(raw);
    }

    // PVE_ONLY (Adventure Mode): the challenge's "enemies deal more damage"
    // auras (SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN) are meant for creatures. The
    // core applied them to every source, so a player or a bot one-shot a
    // character in Adventure Mode. Register those spells with the core, which
    // then skips them when the source is on the player side
    // (IsPlayerSideDamageSource in Unit.cpp). A spell also used
    // by a challenge that is not PvE-only is left alone: that challenge's
    // malus would otherwise disappear in PvP too.
    void RegisterCreatureOnlyDamageTakenSpells()
    {
        auto collect = [](uint32 id)
        {
            std::set<uint32> out;
            for (uint32 s : GetChallengeSpells(id, 0))
                out.insert(s);
            for (uint32 lv = 1; lv <= ChallengeLevelCount(id); ++lv)
                for (uint32 s : GetChallengeSpells(id, lv))
                    out.insert(s);
            return out;
        };

        std::set<uint32> pveOnly, shared;
        std::set<uint32> pveOnlyChallenges;
        for (uint32 id : DefIds())
        {
            if (RuleListContains(ChallengeRules(id), "CHALLENGE_RULES_TYPE_PVE_ONLY"))
            {
                pveOnlyChallenges.insert(id);
                for (uint32 s : collect(id))
                    pveOnly.insert(s);
            }
        }
        for (uint32 id : DefIds())
        {
            if (pveOnlyChallenges.count(id))
                continue;
            for (uint32 s : collect(id))
                if (pveOnly.count(s))
                    shared.insert(s);
        }

        std::unordered_set<uint32> creatureOnly;
        for (uint32 s : pveOnly)
        {
            if (shared.count(s))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(s);
            if (!info)
                continue;
            for (SpellEffectInfo const& eff : info->Effects)
            {
                if (eff.IsAura(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN))
                {
                    creatureOnly.insert(s);
                    break;
                }
            }
        }

        size_t const count = creatureOnly.size();
        AscensionCreatureOnlyDamageTaken::SetSpells(std::move(creatureOnly));
        LOG_INFO("module.coa_challenges",
            "PvE-only challenges: {} challenge(s), {} damage-taken aura spell(s) limited to creature sources, "
            "{} spell(s) left untouched (shared with a challenge that allows PvP)",
            pveOnlyChallenges.size(), count, shared.size());
        if (!pveOnlyChallenges.empty() && !count)
            LOG_ERROR("module.coa_challenges",
                "PvE-only challenges found but none of their spells carries SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN: "
                "check coa_challenge_spell, their damage-taken malus (if any) now applies to every source");
    }

    // Challenge tiers carry their "you deal less damage to monsters" malus as
    // SPELL_AURA_ASCENSION_MOD_PVE_DAMAGE_DONE_PCT (341): Adventure Mode tier 1
    // (302054) -26%, tier 10 (302613) -31%... The core honours aura 341 only
    // on a closed list (copied spells such as Frozen Waters 271942 carry it
    // too), so without this registration the malus had no effect at all.
    // Register every spell that a challenge lists (any challenge, any level,
    // the union row included) and that carries aura 341. The core keeps its
    // own conditions: victim not controlled by a player, school mask.
    void RegisterPveDamageDoneChallengeSpells()
    {
        std::unordered_set<uint32> registered;
        std::set<uint32> challenges;
        for (uint32 id : DefIds())
        {
            std::set<uint32> listed;
            for (uint32 s : GetChallengeSpells(id, 0))
                listed.insert(s);
            for (uint32 lv = 1; lv <= ChallengeLevelCount(id); ++lv)
                for (uint32 s : GetChallengeSpells(id, lv))
                    listed.insert(s);

            for (uint32 s : listed)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(s);
                if (!info)
                    continue;
                for (SpellEffectInfo const& eff : info->Effects)
                {
                    if (eff.IsAura(SPELL_AURA_ASCENSION_MOD_PVE_DAMAGE_DONE_PCT))
                    {
                        registered.insert(s);
                        challenges.insert(id);
                        break;
                    }
                }
            }
        }

        size_t const count = registered.size();
        AscensionChallengePveDamageDone::SetSpells(std::move(registered));
        LOG_INFO("module.coa_challenges",
            "Challenge PvE damage-done malus: {} spell(s) carrying aura 341 enabled, from {} challenge(s)",
            count, challenges.size());
    }

    // ---- Adventure Mode: monster health ----------------------------------------
    // Each tier's main aura ("Adventure Mode N!", 302053 ... 302882) promises
    // "Monsters have X% more health" in its tooltip, and nothing applied it. The
    // percentage only exists in that tooltip, which the server does not load:
    // CoA.Challenges.AdventureHealthData.h is generated from Spell.dbc by
    // coa-project/outils/generer-vie-monstres-aventure.py.
    //
    // A creature fighting an Adventure player (or what he controls or summons)
    // gets a flat TOTAL_VALUE health modifier worth X% of its maximum health,
    // the highest X among the Adventure players on its threat list at engage
    // and among those who hit it; never cumulated. The health percentage is
    // kept. The modifier goes on leaving combat (evade, reset, death). A flat
    // modifier survives the recalculations driven by health auras, and the
    // level scaling (AscensionCompatLevelScalingScript) only reselects a level
    // out of combat at full health, when the modifier is gone.
    //
    // All the state lives on the creature (Object::CustomData), touched only by
    // hooks that run on the creature's map thread: no global state.
    namespace
    {
        constexpr bool AdventureHealthTableSorted()
        {
            for (size_t i = 1; i < AdventureHealthTable.size(); ++i)
                if (AdventureHealthTable[i - 1].spellId >= AdventureHealthTable[i].spellId)
                    return false;
            return true;
        }
        static_assert(AdventureHealthTableSorted(), "AdventureHealthTable must be sorted by spell id");

        // A std::string, not a char const*: DataMap::Get takes std::string const&,
        // and a literal this long would be heap-allocated on every call
        // (CoAChallengesCreature checks it on every creature update).
        std::string const AdventureHealthKey("coa_challenges.adventure_health");

        struct AdventureHealthState : DataMap::Base
        {
            float extra = 0.0f;     // TOTAL_VALUE added to UNIT_MOD_HEALTH
            uint32 pct = 0;         // the tier percentage it stands for
            float baseValue = 0.0f; // UNIT_MOD_HEALTH BASE_VALUE it was calibrated on
        };

        // Directeur event creatures set their own level and health (same range
        // as AscensionCompatLevelScalingScript::CanScale).
        constexpr uint32 DIRECTEUR_EVENT_ENTRY_MIN = 9100000;
        constexpr uint32 DIRECTEUR_EVENT_ENTRY_MAX = 9109999;

        // Changes the maximum health and keeps the health percentage (64-bit
        // arithmetic, no integer percentage truncation).
        void ChangeMaxHealthKeepingPct(Creature* creature, float extra, bool apply)
        {
            bool const alive = creature->IsAlive() && creature->GetHealth();
            uint64 const oldMax = creature->GetMaxHealth();
            uint64 const oldHealth = creature->GetHealth();
            creature->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, extra, apply);
            uint64 const newMax = creature->GetMaxHealth();
            if (!alive || !oldMax || !newMax)
                return;
            uint64 health = (oldHealth * newMax + oldMax / 2) / oldMax;
            health = std::clamp<uint64>(health, 1, newMax);
            creature->SetHealth(uint32(health));
        }

        // The player whose Adventure tier counts for this unit: the unit itself, the
        // charmer/owner player (pets, guardians, totems, charmed units, followed
        // through owner chains), or the player summoner of a wild summon that is
        // not hostile to him.
        Player* AdventureHealthPlayer(Unit* unit)
        {
            if (!unit)
                return nullptr;
            if (Player* player = unit->ToPlayer())
                return player;
            if (Player* player = unit->GetAffectingPlayer())
                return player;
            if (TempSummon* summon = unit->ToTempSummon())
                if (Unit* summoner = summon->GetSummonerUnit())
                    if (summoner != unit && !unit->IsHostileTo(summoner))
                        return summoner->GetAffectingPlayer();
            return nullptr;
        }
    }

    uint32 AdventureExtraHealthPctOfSpell(uint32 spellId)
    {
        auto it = std::lower_bound(AdventureHealthTable.begin(), AdventureHealthTable.end(), spellId,
            [](AdventureHealthEntry const& entry, uint32 id) { return entry.spellId < id; });
        return (it != AdventureHealthTable.end() && it->spellId == spellId) ? it->extraHealthPct : 0;
    }

    // Highest "more health" percentage among the tier auras the unit's player
    // carries (the tier main aura is SPELL_AURA_MOD_XP_PCT, so that list is a
    // few entries long). 0 outside Adventure Mode.
    uint32 AdventureExtraHealthPct(Unit* source)
    {
        Player* player = AdventureHealthPlayer(source);
        if (!player)
            return 0;
        uint32 best = 0;
        for (AuraEffect const* effect : player->GetAuraEffectsByType(SPELL_AURA_MOD_XP_PCT))
            best = std::max(best, AdventureExtraHealthPctOfSpell(effect->GetId()));
        return best;
    }

    // Adventure Mode: creature damage against an Adventure player, on top of the
    // tier's own damage-taken auras. CoA classes hit 1.3 to 4 times harder than
    // the classic classes the AzerothCore creature tables were sized for, and
    // carry about 1.8 times their health at level 2: measured on 2026-10-07,
    // a level 2 Primalist at tier 10 lost 15% of his health per single monster,
    // against 70% at tier 1 on Ascension. Set to 6 on 2026-10-07, back to 1
    // (the tooltip, nothing more) the next day at the player's request: the
    // hook stays as a lever, neutral by default. Cached: read at startup and on
    // config reload (a missing key would otherwise be logged on every hit).
    namespace
    {
        float AdventureCreatureDamageMult = 1.0f;
    }

    void LoadAdventureCreatureDamageConfig()
    {
        float mult = sConfigMgr->GetOption<float>("CoAChallenges.Adventure.CreatureDamageMultiplier", 1.0f);
        if (!(mult >= 0.1f && mult <= 50.0f))
        {
            LOG_ERROR("module.coa_challenges",
                "CoAChallenges.Adventure.CreatureDamageMultiplier = {} out of [0.1, 50], 1 used", mult);
            mult = 1.0f;
        }
        AdventureCreatureDamageMult = mult;
        LOG_INFO("module.coa_challenges", "Adventure creature damage multiplier: x{:.2f}", mult);
    }

    // Multiplier for damage dealt by <attacker> to <victim>: the configured value
    // when the victim is a player in Adventure Mode and the attacker is a
    // creature on no player's side (the player side test of the core's
    // IsPlayerSideDamageSource, Unit.cpp); 1 otherwise. Players, their pets,
    // guardians, totems, charmed units and friendly summons are never scaled,
    // nor is damage with no known source.
    float AdventureCreatureDamageMultiplier(Unit* victim, Unit* attacker)
    {
        if (AdventureCreatureDamageMult == 1.0f || !victim || !attacker || attacker == victim || !victim->IsPlayer())
            return 1.0f;
        // A player mind-controlled by a creature (Lady Deathwhisper) is neither
        // controlled by a player nor owned by one: test the player first.
        if (attacker->IsPlayer() || attacker->IsCharmedOwnedByPlayerOrPlayer() || attacker->IsControlledByPlayer())
            return 1.0f;
        if (TempSummon* summon = attacker->ToTempSummon())
            if (Unit* summoner = summon->GetSummonerUnit())
                if (summoner->IsCharmedOwnedByPlayerOrPlayer() && !attacker->IsHostileTo(summoner))
                    return 1.0f;
        return AdventureExtraHealthPct(victim) ? AdventureCreatureDamageMult : 1.0f;
    }

    // A quest with nothing to do in its log: no item to bring, no creature or
    // gameobject credit, no player kill, no area to explore or event to
    // trigger, no reputation and no money required. The item the quest hands
    // out at the start (StartItem) is a delivery, not an objective: 1 116
    // quests require only that item (6385 Doras the Wind Rider Master).
    bool QuestHasNoObjective(Quest const* quest)
    {
        for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i] && quest->RequiredItemId[i] != quest->GetSrcItemId())
                return false;
        for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i])
                return false;
        return !quest->GetPlayersSlain()
            && !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT)
            && !quest->GetRepObjectiveFaction() && !quest->GetRepObjectiveFaction2()
            && quest->GetRewOrReqMoney() >= 0;
    }

    void ScaleAdventureCreatureDamage(Unit* victim, Unit* attacker, uint32& damage)
    {
        if (!damage)
            return;
        float const mult = AdventureCreatureDamageMultiplier(victim, attacker);
        if (mult == 1.0f)
            return;
        double const scaled = std::round(double(damage) * mult);
        damage = uint32(std::clamp(scaled, 1.0, double(std::numeric_limits<int32>::max())));
    }

    // Never players or what a player controls, pets, totems, triggers, critters,
    // companions, owned units, Directeur event creatures, scripted private
    // instances (what the level scaling's CanScale leaves out), quest givers and
    // gossip NPCs (scripted quest fights), nor what a player summoned (quest
    // encounters).
    bool AdventureHealthEligible(Creature const* creature)
    {
        if (!creature || !creature->IsAlive() || !creature->IsInWorld())
            return false;
        if (creature->GetEntry() >= DIRECTEUR_EVENT_ENTRY_MIN && creature->GetEntry() <= DIRECTEUR_EVENT_ENTRY_MAX)
            return false;
        if (creature->GetMap()->IsScriptedPrivateInstance())
            return false;
        if (creature->IsCharmedOwnedByPlayerOrPlayer() || creature->IsControlledByPlayer() ||
            creature->IsPet() || creature->IsTotem() || creature->IsTrigger() || creature->IsCritter() ||
            creature->GetCreatureType() == CREATURE_TYPE_NON_COMBAT_PET || creature->GetCharmerOrOwnerGUID())
            return false;
        if (creature->IsQuestGiver() || creature->IsGossip())
            return false;
        if (TempSummon const* summon = creature->ToTempSummon())
            if (summon->GetSummonerGUID().IsPlayer())
                return false;
        return true;
    }

    uint32 AdventureHealthAppliedPct(Creature const* creature)
    {
        if (!creature)
            return 0;
        AdventureHealthState const* state = creature->CustomData.Get<AdventureHealthState>(AdventureHealthKey);
        return state ? state->pct : 0;
    }

    void RestoreAdventureHealth(Unit* unit)
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return;
        AdventureHealthState* state = creature->CustomData.Get<AdventureHealthState>(AdventureHealthKey);
        if (!state)
            return;
        float const extra = state->extra;
        creature->CustomData.Erase(AdventureHealthKey);
        ChangeMaxHealthKeepingPct(creature, extra, false);
    }

    void ApplyAdventureHealth(Creature* creature, uint32 pct)
    {
        if (!pct || !AdventureHealthEligible(creature))
            return;
        AdventureHealthState* state = creature->CustomData.Get<AdventureHealthState>(AdventureHealthKey);
        float const oldExtra = state ? state->extra : 0.0f;
        if (state && state->pct >= pct)
            return;                                     // no cumulation; never downgraded

        // A creature whose maximum health was set outside the stat model (a
        // script calling SetMaxHealth) would be reset to the model by the
        // recalculation: leave it alone (a lower tier already applied stays).
        float const modelled = creature->GetTotalAuraModValue(UNIT_MOD_HEALTH);
        uint32 const maxHealth = creature->GetMaxHealth();
        if (!maxHealth || std::fabs(double(modelled) - double(maxHealth)) > std::max(1.0, double(maxHealth) * 1e-6))
            return;
        float const totalPct = creature->GetPctModifierValue(UNIT_MOD_HEALTH, TOTAL_PCT);
        if (totalPct <= 0.0f)
            return;
        // Maximum health without our modifier: TOTAL_VALUE is multiplied by
        // TOTAL_PCT (Unit::GetTotalAuraModValue).
        double const baseMax = double(maxHealth) - double(oldExtra) * totalPct;
        if (baseMax < 1.0 || baseMax * (100.0 + pct) / 100.0 >= double(std::numeric_limits<uint32>::max()))
            return;

        float const extra = float(baseMax * pct / 100.0 / totalPct);
        if (!state)
        {
            state = new AdventureHealthState();
            creature->CustomData.Set(AdventureHealthKey, state);
        }
        state->extra = extra;
        state->pct = pct;
        state->baseValue = creature->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE);
        // Upgrade = one step by the difference, the health percentage kept.
        ChangeMaxHealthKeepingPct(creature, extra - oldExtra, true);
    }

    // Called on every update of a creature carrying the modifier while in
    // combat. A Creature::SelectLevel / UpdateEntry (scripted transformation)
    // writes SetMaxHealth/SetHealth(new base) directly, then sets BASE_VALUE
    // (which recomputes the model only when the base changed), and leaves our
    // TOTAL_VALUE behind, calibrated on the former template. Detected as a
    // BASE_VALUE other than the one we calibrated on, or a maximum health no
    // longer matching the stat model: drop the stale modifier (the model then
    // gives the new template's health) and recompute it on the new base.
    void ResyncAdventureHealth(Creature* creature)
    {
        AdventureHealthState* state = creature->CustomData.Get<AdventureHealthState>(AdventureHealthKey);
        if (!state)
            return;
        float const modelled = creature->GetTotalAuraModValue(UNIT_MOD_HEALTH);
        uint32 const maxHealth = creature->GetMaxHealth();
        bool const baseChanged =
            std::fabs(creature->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE) - state->baseValue) > 0.5f;
        if (!baseChanged &&
            std::fabs(double(modelled) - double(maxHealth)) <= std::max(1.0, double(maxHealth) * 1e-6))
            return;
        float const extra = state->extra;
        uint32 const pct = state->pct;
        creature->CustomData.Erase(AdventureHealthKey);
        uint32 const health = creature->GetHealth();
        creature->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, extra, false);
        if (creature->IsAlive() && health)
            creature->SetHealth(std::min(health, creature->GetMaxHealth()));
        ApplyAdventureHealth(creature, pct);
    }

    void ApplyAdventureHealthOnEngage(Creature* creature, Unit* victim)
    {
        if (!AdventureHealthEligible(creature))
            return;
        uint32 pct = AdventureExtraHealthPct(victim);
        for (ThreatReference const* ref : creature->GetThreatMgr().GetUnsortedThreatList())
            pct = std::max(pct, AdventureExtraHealthPct(ref->GetVictim()));
        ApplyAdventureHealth(creature, pct);
    }

    void ApplyAdventureHealthOnDamage(Unit* attacker, Unit* victim)
    {
        Creature* creature = victim ? victim->ToCreature() : nullptr;
        if (!creature || !attacker || attacker == victim)
            return;
        if (uint32 pct = AdventureExtraHealthPct(attacker))
            ApplyAdventureHealth(creature, pct);
    }

    // Startup check, logged: every generated tier spell must exist in the
    // server's Spell.dbc and be listed by a challenge.
    void CheckAdventureHealthTable()
    {
        std::set<uint32> listed;
        for (uint32 id : DefIds())
        {
            for (uint32 s : GetChallengeSpells(id, 0))
                listed.insert(s);
            for (uint32 lv = 1; lv <= ChallengeLevelCount(id); ++lv)
                for (uint32 s : GetChallengeSpells(id, lv))
                    listed.insert(s);
        }
        uint32 known = 0, inChallenge = 0;
        for (AdventureHealthEntry const& entry : AdventureHealthTable)
        {
            if (sSpellMgr->GetSpellInfo(entry.spellId))
                ++known;
            else
                LOG_ERROR("module.coa_challenges", "Adventure monster health: tier {} spell {} unknown to the core",
                    entry.tier, entry.spellId);
            if (listed.count(entry.spellId))
                ++inChallenge;
        }
        LOG_INFO("module.coa_challenges",
            "Adventure monster health: {} tier spell(s) in the generated table, {} known to the core, "
            "{} listed by a challenge (tier 1 +{}%, tier 100 +{}%)",
            AdventureHealthTable.size(), known, inChallenge,
            AdventureHealthTable.front().extraHealthPct, AdventureHealthTable.back().extraHealthPct);
        if (inChallenge != AdventureHealthTable.size())
            LOG_ERROR("module.coa_challenges",
                "Adventure monster health: {} generated tier spell(s) are listed by no challenge: "
                "regenerate CoA.Challenges.AdventureHealthData.h or check coa_challenge_spell",
                AdventureHealthTable.size() - inChallenge);
    }

    void ApplyChallengeSpell(Player* player, uint32 challengeID, uint32 level)
    {
        if (!player)
            return;
        for (uint32 spell : GetChallengeSpells(challengeID, level))
        {
            if (!spell)
                continue;
            if (!sSpellMgr->GetSpellInfo(spell))
            {
                LOG_WARN("module.coa_challenges", "Challenge {}: spell {} unknown to core, aura not applied",
                    challengeID, spell);
                continue;
            }
            player->CastSpell(player, spell, true);
            LOG_INFO("module.coa_challenges", "Applied challenge aura {} to {} (challenge {} level {})",
                spell, player->GetName(), challengeID, level);
        }
    }

    // Remove every aura the challenge could have applied (union + every
    // per-level entry): the active level isn't always known at removal time.
    void RemoveChallengeSpell(Player* player, uint32 challengeID)
    {
        if (!player)
            return;

        auto collect = [](uint32 id, std::vector<uint32>& out)
        {
            for (uint32 s : GetChallengeSpells(id, 0))
                out.push_back(s);
            for (uint32 lv = 1; lv <= ChallengeLevelCount(id); ++lv)
                for (uint32 s : GetChallengeSpells(id, lv))
                    out.push_back(s);
        };

        std::vector<uint32> spells;
        collect(challengeID, spells);

        // Auras the challenge shares with a still-active challenge or an active
        // game-mode base must survive: otherwise stopping one challenge silently
        // strips an aura another one still needs.
        uint32 guid = player->GetGUID().GetCounter();
        std::set<uint32> keep;
        for (uint32 other : ActiveChallenges(guid))
        {
            if (other == challengeID)
                continue;
            std::vector<uint32> otherSpells;
            collect(other, otherSpells);
            keep.insert(otherSpells.begin(), otherSpells.end());
        }
        uint32 modeMask = CachedGameModeMask(guid);
        for (auto const& [bit, base] : GameModeBaseSnapshot())
        {
            if (!(modeMask & bit))
                continue;
            std::vector<uint32> baseSpells;
            collect(base, baseSpells);
            keep.insert(baseSpells.begin(), baseSpells.end());
        }

        std::sort(spells.begin(), spells.end());
        spells.erase(std::unique(spells.begin(), spells.end()), spells.end());
        for (uint32 spell : spells)
        {
            if (!spell || keep.count(spell))
                continue;
            player->RemoveAurasDueToSpell(spell);
            LOG_INFO("module.coa_challenges", "Removed challenge aura {} from {} (challenge {})",
                spell, player->GetName(), challengeID);
        }
    }

    // Death strips auras; resurrect must restore active challenge auras.
    void ReapplyActiveSpells(Player* player)
    {
        uint32 guid = player->GetGUID().GetCounter();
        if (QueryResult actives = CharacterDatabase.Query(
                "SELECT challengeId, level FROM coa_character_challenge WHERE guid = {}", guid))
        {
            do
            {
                Field* f = actives->Fetch();
                ApplyChallengeSpell(player, f[0].Get<uint32>(), f[1].Get<uint32>());
            } while (actives->NextRow());
        }
    }

    // Login reconciliation: drop challenge auras that have no backing active
    // challenge (or game-mode base). ReapplyActiveSpells only ADDS auras, so an
    // orphan left by a row deleted out-of-band (old repack build, crash, manual
    // DB surgery, characters->world migration) would otherwise persist forever.
    // Kept: every spell of an active challenge and of the base challenge of
    // every active game mode (those apply without a coa_character_challenge row).
    void StripOrphanChallengeAuras(Player* player)
    {
        if (!player)
            return;

        auto collect = [](uint32 challengeID, std::set<uint32>& out)
        {
            for (uint32 s : GetChallengeSpells(challengeID, 0))
                if (s)
                    out.insert(s);
            for (uint32 lv = 1; lv <= ChallengeLevelCount(challengeID); ++lv)
                for (uint32 s : GetChallengeSpells(challengeID, lv))
                    if (s)
                        out.insert(s);
        };

        std::vector<uint32> const ids = DefIds();
        uint32 guid = player->GetGUID().GetCounter();

        std::set<uint32> keep;
        for (uint32 cid : ActiveChallenges(guid))
            collect(cid, keep);

        uint32 mask = CachedGameModeMask(guid);
        for (uint32 cid : ids)
        {
            uint32 gm = RequiredGameMode(cid);
            if (gm && (mask & gm))
                collect(cid, keep);
        }

        std::set<uint32> allSpells;
        for (uint32 cid : ids)
            collect(cid, allSpells);

        for (uint32 spell : allSpells)
        {
            if (keep.count(spell) || !player->HasAura(spell))
                continue;
            player->RemoveAurasDueToSpell(spell);
            LOG_INFO("module.coa_challenges", "Stripped orphan challenge aura {} from {}",
                spell, player->GetName());
        }
    }

    // Defaults must match conf/mod-coa-challenges.conf.dist (the conf promises
    // that an omitted key falls back to the code default).
    uint32 HungerFoodSpell()
    {
        return sConfigMgr->GetOption<uint32>("CoAChallenges.HungerFoodSpell", 93175);
    }

    uint32 HungerDrinkSpell()
    {
        return sConfigMgr->GetOption<uint32>("CoAChallenges.HungerDrinkSpell", 93176);
    }

    // Meter auras must go away with the challenge (stop/fail) and must
    // never linger without an active hunger challenge (stale relog, etc.).
    void RemoveMeterAuras(Player* player)
    {
        if (!player)
            return;
        player->RemoveAurasDueToSpell(HungerFoodSpell());
        player->RemoveAurasDueToSpell(HungerDrinkSpell());
    }

    void SetMeterAura(Player* player, uint32 spell, int32 value)
    {
        if (!spell)
            return;
        if (value <= 0)
        {
            player->RemoveAurasDueToSpell(spell);
            return;
        }
        if (!sSpellMgr->GetSpellInfo(spell))
        {
            LOG_WARN("module.coa_challenges", "Meter spell {} unknown to core", spell);
            return;
        }
        // Cast first (full visible pipeline, proven by 93181); raw AddAura
        // inside SetAuraStack left some spells invisible client-side.
        if (!player->HasAura(spell))
            player->CastSpell(player, spell, true);
        // Stacks are capped by the client's aura model (255), not by the literal
        // 100: with HungerMax > 100 the meter must keep tracking the real value.
        player->SetAuraStack(spell, player, (uint32)std::min(value, 255));
        if (Aura* aura = player->GetAura(spell))
            LOG_INFO("module.coa_challenges", "Meter aura {} on {}: stacks={}",
                spell, player->GetName(), (uint32)aura->GetStackAmount());
        else
            LOG_WARN("module.coa_challenges", "Meter aura {} failed to apply on {}",
                spell, player->GetName());
    }

    // Total lives for a challenge/mode (coa_challenge_definition.lives). Used by the
    // death handling; the lives COUNTER AURA display is deferred (needs a
    // client Spell.dbc edit).
    uint32 LivesTotal(uint32 challengeID)
    {
        return DefField<uint32>(challengeID, &ChallengeDef::lives, 0u);
    }

    // Per-mode deaths (mode on without its base challenge). Kept in its own
    // table so coa_character_challenge is untouched.
} // namespace CoAChallenges
