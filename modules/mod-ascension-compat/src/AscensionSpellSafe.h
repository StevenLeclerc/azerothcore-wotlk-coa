/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#ifndef ASCENSION_SPELL_SAFE_H
#define ASCENSION_SPELL_SAFE_H

// sSpellMgr->GetSpellInfo(<id>) rend nullptr des que l'id n'est plus dans
// Spell.dbc (ou dans la table spell_dbc qui le complete). Le code de classe
// dereference tres souvent le retour sans le tester, parfois depuis une lambda
// de TaskScheduler : le plantage arrive alors plusieurs secondes apres le sort,
// sur un fil de carte, sans une ligne de journal, et le coredump ne designe
// qu'une lambda anonyme. P-047 a mesure qu'un import du Spell.dbc client
// modifie 335 sorts CoA : l'hypothese « cet id existera toujours » n'en est pas
// une.
//
// Ce fichier ne change rien quand le sort est la : Get() rend exactement ce que
// sSpellMgr rend. Quand il manque, il journalise UNE fois par id (LOG_ERROR,
// categorie module.ascension_compat) et l'appelant retombe sur une valeur par
// defaut explicite, ou saute l'effet. Le meme choix que le helper maison
// Radius() (AscensionTinker.cpp:113), qui rend 10.0f quand le sort manque.

#include "Log.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <mutex>
#include <unordered_set>

namespace AscensionSpellSafe
{
// Rend le SpellInfo, ou nullptr. Journalise une seule fois par id manquant :
// appele depuis plusieurs fils de MapUpdate, d'ou le verrou autour du set.
inline SpellInfo const* Get(uint32 id)
{
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
        return info;

    static std::mutex missingMutex;
    static std::unordered_set<uint32> missing;
    {
        std::lock_guard<std::mutex> guard(missingMutex);
        if (!missing.insert(id).second)
            return nullptr;
    }
    LOG_ERROR("module.ascension_compat",
        "AscensionSpellSafe: le sort {} est absent de Spell.dbc; l'effet de classe qui en depend est degrade.",
        id);
    return nullptr;
}

inline int32 Duration(uint32 id, int32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->GetDuration() : fallback;
}

inline uint32 MaxAffectedTargets(uint32 id, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->MaxAffectedTargets : fallback;
}

inline uint32 StackAmount(uint32 id, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->StackAmount : fallback;
}

inline uint32 SpellLevel(uint32 id, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->SpellLevel : fallback;
}

inline uint32 ProcChance(uint32 id, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->ProcChance : fallback;
}

// slot est un index d'effet (EFFECT_0..EFFECT_2) : SpellInfo::Effects est un
// tableau de MAX_SPELL_EFFECTS elements, jamais nul, donc seul le SpellInfo
// lui-meme est a tester.
inline float EffectRadius(uint32 id, uint8 slot, Unit* caster, float fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->Effects[slot].CalcRadius(caster) : fallback;
}

inline int32 EffectValue(uint32 id, uint8 slot, Unit const* caster, int32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->Effects[slot].CalcValue(caster) : fallback;
}

inline uint32 EffectAmplitude(uint32 id, uint8 slot, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->Effects[slot].Amplitude : fallback;
}

inline uint32 EffectChainTarget(uint32 id, uint8 slot, uint32 fallback)
{
    SpellInfo const* info = Get(id);
    return info ? info->Effects[slot].ChainTarget : fallback;
}
}

#endif
