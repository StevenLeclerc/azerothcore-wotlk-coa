/* Conquest of Azeroth, GNU AGPL v3. */
#ifndef ASCENSION_CHALLENGE_PVE_DAMAGE_DONE_H
#define ASCENSION_CHALLENGE_PVE_DAMAGE_DONE_H

#include "Define.h"
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

// Challenge tier spells whose SPELL_AURA_ASCENSION_MOD_PVE_DAMAGE_DONE_PCT
// (341) effects Unit::GetAscensionNormalTuningDamageMultiplier honours, on top
// of the reviewed class tuning records 887000-887090. Aura 341 was also copied
// onto spells that must not enable it (Frozen Waters 271942, Fire and Ice
// 1582385): the core therefore keeps a closed list, and this registry only
// opens it to spells listed by a challenge (Adventure Mode 211/425 tiers,
// e.g. 302054 tier 1 -26%, 302613 tier 10 -31%).
//
// The set is filled by mod-coa-challenges from coa_challenge_spell (every
// challenge, every level), at startup and on `.coa challenges reload`.
// Header-only: the storage is a function-local static of an inline function,
// unique in the program.
namespace AscensionChallengePveDamageDone
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

    inline bool IsChallengeSpell(uint32 spellId)
    {
        Registry& r = GetRegistry();
        std::shared_lock<std::shared_mutex> guard(r.lock);
        return r.spells.count(spellId) != 0;
    }
}

#endif
