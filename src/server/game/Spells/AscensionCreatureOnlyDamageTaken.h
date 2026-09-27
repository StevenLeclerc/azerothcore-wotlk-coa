/* Conquest of Azeroth, GNU AGPL v3. */
#ifndef ASCENSION_CREATURE_ONLY_DAMAGE_TAKEN_H
#define ASCENSION_CREATURE_ONLY_DAMAGE_TAKEN_H

#include "Define.h"
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

// Spells whose SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN effects only count damage
// dealt by creatures: never by a player, a playerbot, what they charm or own
// (pets, guardians, totems), nor their wild summons. Unit::SpellDamageBonusTaken
// and Unit::MeleeDamageBonusTaken skip such an effect when the source is on the
// player side (IsPlayerSideDamageSource in Unit.cpp) or unknown.
//
// The set is filled by mod-coa-challenges from its PvE-only challenge
// definitions (Adventure Mode 211/425), at startup and on
// `.coa challenges reload`. Header-only: the storage is a function-local
// static of an inline function, unique in the program.
namespace AscensionCreatureOnlyDamageTaken
{
    struct Registry
    {
        std::shared_mutex lock;
        std::unordered_set<uint32> spells;
    };

    inline Registry& GetRegistry()
    {
        static Registry registry;
        return registry;
    }

    inline void SetSpells(std::unordered_set<uint32> spells)
    {
        Registry& r = GetRegistry();
        std::unique_lock<std::shared_mutex> guard(r.lock);
        r.spells.swap(spells);
    }

    inline bool IsCreatureOnly(uint32 spellId)
    {
        Registry& r = GetRegistry();
        std::shared_lock<std::shared_mutex> guard(r.lock);
        return r.spells.count(spellId) != 0;
    }
}

#endif
