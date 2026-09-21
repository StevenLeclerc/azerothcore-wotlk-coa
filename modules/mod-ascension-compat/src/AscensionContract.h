/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
//
// Shared assumption checks for the twenty-one class contracts.
//
// WHY THIS FILE EXISTS — P-047 / P-049.
//
// Every Ascension*Contracts.cpp opens on the same guard:
//
//     if (!info || info->SpellFamilyName != <n>)
//         return;
//
// and then rewrites forty to eighty records of the client Spell.dbc by hand,
// with no verification and no journal line. `/usr/bin/grep -c LOG_` over the ten
// files returned 0 on every one of them. Two failure modes follow from that, and
// both have already been paid for:
//
//  * a client import that moves one SpellFamilyName makes the guard refuse every
//    record of that class. Forty-plus talents go inert at once, the game changes
//    under the player, and neither Errors.log nor Server.log carries a line
//    (P-047, the 2026-09-19 import);
//  * a client import that turns an effect slot into something other than
//    APPLY_AURA makes every `Effects[n].ApplyAuraName = ...` written on that slot
//    inert, because SpellEffectInfo::IsAura() reads Effect, not ApplyAuraName
//    (SpellInfo.cpp:416-461). That is how Sun Down 572752 lost its aura in
//    silence (P-049).
//
// The helpers below do not add a journal line per rewrite — there are several
// hundred of them and the noise would hide the signal. They make the failure of
// an assumption visible: each one is journalled once per (contract, spell, slot),
// and AscensionContract::Report() prints the per-class tally once at startup, so
// a class whose contract matched nothing is an error line instead of silence.
//
#ifndef ASCENSION_CONTRACT_H
#define ASCENSION_CONTRACT_H

#include "DBCStores.h"
#include "Log.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace AscensionContract
{
namespace Detail
{
struct ClassTally
{
    // MatchesFamily runs on every record of Spell.dbc - 209 510 of them on the
    // live client file - times ten contracts, so the lookup is kept to a pointer
    // comparison on the string literal the call site passes, with the textual
    // comparison only as a fallback.
    char const* id;
    std::string contract;
    uint32 family;
    uint32 matched; // records that passed the entry guard and were rewritten
    uint32 refused; // distinct assumptions this contract found false
};

struct Registry
{
    std::mutex lock;
    std::vector<ClassTally> tallies;
    std::set<std::string> journalled;
};

inline Registry& Store()
{
    static Registry registry;
    return registry;
}

// The caller holds Registry::lock.
inline ClassTally& TallyFor(Registry& registry, char const* contract, uint32 family)
{
    for (ClassTally& tally : registry.tallies)
        if (tally.id == contract || tally.contract == contract)
            return tally;
    registry.tallies.push_back(ClassTally{contract, contract, family, 0, 0});
    return registry.tallies.back();
}

inline std::string Key(char const* contract, uint32 spellId, uint32 slot, char const* check)
{
    return std::string(contract) + '/' + check + '/' + std::to_string(spellId) + '/' + std::to_string(slot);
}

// Counts the refusal against its contract and answers whether this exact failure
// still has to be journalled. Contracts are applied from the world thread at load
// time, but the lock costs nothing there and keeps a later reload honest.
inline bool FirstFailure(char const* contract, std::string const& key)
{
    Registry& registry = Store();
    std::lock_guard<std::mutex> guard(registry.lock);
    // A contract runs the same branch on every record it matches, so the counter
    // has to follow the journal and count distinct failures, not calls.
    if (!registry.journalled.insert(key).second)
        return false;
    for (ClassTally& tally : registry.tallies)
        if (tally.id == contract || tally.contract == contract)
        {
            ++tally.refused;
            break;
        }
    return true;
}

// SpellEffectInfo::IsAura() also demands a non-zero ApplyAuraName, which is
// exactly what a contract is about to write, so the question asked here is only
// whether the effect is one that carries an aura at all (SpellInfo.cpp:416-461).
inline bool CarriesAura(SpellEffectInfo const& effect)
{
    return effect.IsUnitOwnedAuraEffect() || effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA;
}
} // namespace Detail

// Entry guard of a class contract, in place of `info->SpellFamilyName != n`.
// It registers the contract on the very first call — even when nothing matches —
// so that Report() can tell a class that was never reached from a class that has
// no records of its own.
inline bool MatchesFamily(SpellInfo const* info, uint32 family, char const* contract)
{
    Detail::Registry& registry = Detail::Store();
    std::lock_guard<std::mutex> guard(registry.lock);
    Detail::ClassTally& tally = Detail::TallyFor(registry, contract, family);
    if (!info || info->SpellFamilyName != family)
        return false;
    ++tally.matched;
    return true;
}

// True when the slot can actually carry the aura a contract is about to write on
// it. When it cannot, the write is inert and the caller is told so, once. Slots
// that are empty (Effect 0) or already a plain DUMMY effect are not reported:
// the client file is full of leftover aura names on those, and a contract that
// dummies them changes nothing either way.
inline bool ExpectAuraSlot(SpellInfo const* info, uint8 slot, char const* contract)
{
    if (!info || slot >= MAX_SPELL_EFFECTS)
        return false;
    SpellEffectInfo const& effect = info->Effects[slot];
    if (Detail::CarriesAura(effect))
        return true;
    if (effect.Effect && effect.Effect != SPELL_EFFECT_DUMMY &&
        Detail::FirstFailure(contract, Detail::Key(contract, info->Id, slot, "aura-slot")))
        LOG_ERROR("module.ascension_compat",
            "Ascension contract {}: spell {} slot {} carries effect {}, which is not an aura effect. "
            "The aura this contract writes there does nothing (P-049).",
            contract, info->Id, slot, effect.Effect);
    return false;
}

// Replaces `info->DurationEntry = sSpellDurationStore.LookupEntry(index)`. A row
// that is not in SpellDuration.dbc used to be written as a null pointer, and
// SpellInfo::GetDuration() reads a null entry as a duration of zero
// (SpellInfo.cpp:2916-2928): the aura lands already expired, without a word.
// On failure the record keeps whatever duration the client gave it.
inline bool SetDuration(SpellInfo* info, uint32 index, char const* contract)
{
    if (!info)
        return false;
    if (SpellDurationEntry const* entry = sSpellDurationStore.LookupEntry(index))
    {
        info->DurationEntry = entry;
        return true;
    }
    if (Detail::FirstFailure(contract, Detail::Key(contract, info->Id, 0, "duration")))
        LOG_ERROR("module.ascension_compat",
            "Ascension contract {}: SpellDuration row {} is missing, spell {} keeps its client duration.",
            contract, index, info->Id);
    return false;
}

// Printed once, after the spell store is loaded. This is the line that P-047 was
// missing: a class contract that matched no record at all says so out loud.
inline void Report()
{
    Detail::Registry& registry = Detail::Store();
    std::lock_guard<std::mutex> guard(registry.lock);
    if (registry.tallies.empty())
    {
        LOG_ERROR("module.ascension_compat",
            "Ascension contracts: not one class contract ran. The custom classes are running on stock records.");
        return;
    }
    // Un contrat dont le hook n'est plus cable ne s'enregistre jamais et sort donc
    // de cette boucle sans une ligne : c'est ce compte, compare aux dix fichiers
    // Ascension*Contracts.cpp, qui rend cette disparition-la visible.
    LOG_INFO("module.ascension_compat",
        "Ascension contracts: {} class contracts registered.", uint32(registry.tallies.size()));
    for (Detail::ClassTally const& tally : registry.tallies)
    {
        if (!tally.matched)
            LOG_ERROR("module.ascension_compat",
                "Ascension contract {}: no spell of SpellFamilyName {} reached it. The class is unarmed - "
                "check that family against Spell.dbc after the last client import (P-047).",
                tally.contract, tally.family);
        else if (tally.refused)
            LOG_ERROR("module.ascension_compat",
                "Ascension contract {}: {} records of family {} rewritten, {} assumptions refused (listed above).",
                tally.contract, tally.matched, tally.family, tally.refused);
        else
            LOG_INFO("module.ascension_compat",
                "Ascension contract {}: {} records of family {} rewritten, every assumption held.",
                tally.contract, tally.matched, tally.family);
    }
}
} // namespace AscensionContract

#endif // ASCENSION_CONTRACT_H
