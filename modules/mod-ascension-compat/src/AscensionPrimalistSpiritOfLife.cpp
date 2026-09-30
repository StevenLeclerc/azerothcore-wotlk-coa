/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
// Primalist (class 31, family 37): Spirit of Life 840002, summoned by 572853 (Primordial Earth
// 806536, one spirit) and 572826 (Eternally Chosen 802888, two spirits). The creature is data:
// 2026_09_30_33_ascension_creatures_primalist.sql. Written on 2026-09-30.
//
// Read in Spell.dbc (server copy), 2026-09-30:
//   806536 / 802888: "summon N Spirit of Life that immediately casts a lesser Spirit Charge on
//     a nearby ally, healing them for ${$572830m2+$572830ppl2+$spn*.2+$AP*.2} and dealing
//     ${$572830m3+$572830ppl3+$spn*.2+$AP*.2} Nature damage to nearby enemies."
//   572853 / 572826: effect 28, MiscValue 840002, SummonProperties 61 (ALLY / GUARDIAN),
//     duration index 28 = 5 s.
//   572830 Lesser Spirit Charge: e0 effect 149 (charge) and e1 HEAL on TARGET_UNIT_NEARBY_PARTY
//     (the core's nearest party member of the Primalist, the spirit itself excluded as a
//     non-controllable guardian), e2 SCHOOL_DAMAGE on TARGET_UNIT_DEST_AREA_ENEMY.
//     BasePoints 54 / 61, DieSides 21 / 1, RealPointsPerLevel 9 / 9 from level 22.
//   572829 "Spirits of Life [Death]" is an instakill: the authored spirit casts once and dies.
//   572827 "Spirits of Life [Aura]" (periodic 572830 every 1.5 s) is NOT used: a first tick at
//     1.5 s is not "immediately", and its two spell modifiers only act on a player.
// Third-party record (db.exil.es mirror, hertigservices/ascension-data, npcs.jsonl): 840002
// "casts" 572830 with a 16 s cooldown, i.e. once in a 5 s life.
//
// The spirit is a guardian with no spell power of its own, so the tooltip's owner terms are
// added to the base points here: master's Nature spell power x 0.2 and melee attack power x 0.2.
// The DBC base, die and per-level terms stay the core's (CalcValue, at the spirit's level,
// which SummonGuardian sets to the master's).
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include <algorithm>

namespace
{
constexpr uint32 SPELL_LESSER_SPIRIT_CHARGE = 572830;

// Spell::SetSpellValue stores CalcBaseValue(value) (value - 1 when DieSides != 0), and
// CalcValue then adds the per-level term and the die: pass the raw base plus the bonus.
int32 BasePointsWithBonus(SpellEffectInfo const& effect, float bonus)
{
    return effect.BasePoints + (effect.DieSides ? 1 : 0) + int32(std::max(0.0f, bonus));
}

// Casts once on its first update (not inside the summoning spell), then leaves after a
// second, the time of the charge. A spirit without a living Primalist owner leaves at once.
struct npc_ascension_primalist_spirit_of_life : public ScriptedAI
{
    explicit npc_ascension_primalist_spirit_of_life(Creature* creature) : ScriptedAI(creature) { }

    ObjectGuid _master;
    bool _cast = false;

    void IsSummonedBy(WorldObject* summoner) override
    {
        Player* player = summoner ? summoner->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_WILDWALKER || !player->IsAlive() || me->GetOwnerGUID() != player->GetGUID())
        {
            _cast = true;
            me->DespawnOrUnsummon(1ms);
            return;
        }
        _master = player->GetGUID();
        me->SetFaction(player->GetFaction());
        me->SetReactState(REACT_PASSIVE);
    }

    void AttackStart(Unit* /*who*/) override { }
    void MoveInLineOfSight(Unit* /*who*/) override { }
    void EnterEvadeMode(EvadeReason /*why*/) override { }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (_cast)
            return;
        _cast = true;

        Player* player = _master ? ObjectAccessor::GetPlayer(*me, _master) : nullptr;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_LESSER_SPIRIT_CHARGE);
        if (!player || !player->IsAlive() || !player->IsInWorld() || !info ||
            info->Effects[EFFECT_1].Effect != SPELL_EFFECT_HEAL || info->Effects[EFFECT_2].Effect != SPELL_EFFECT_SCHOOL_DAMAGE)
        {
            me->DespawnOrUnsummon();
            return;
        }

        float attackPower = player->GetTotalAttackPowerValue(BASE_ATTACK) * 0.2f;
        float heal = float(player->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_NATURE)) * 0.2f + attackPower;
        float damage = float(player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE)) * 0.2f + attackPower;

        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT1, BasePointsWithBonus(info->Effects[EFFECT_1], heal));
        values.AddSpellMod(SPELLVALUE_BASE_POINT2, BasePointsWithBonus(info->Effects[EFFECT_2], damage));
        SpellCastTargets targets;
        // Damage (e2, radius 5 yd) around the Primalist, whose melee critical called the spirit:
        // the spirit itself appears up to 10 yd away (572853 TARGET_DEST_CASTER_RADIUS), where
        // a 5 yd circle would often miss the enemy being fought.
        targets.SetDst(*player);
        me->CastSpell(targets, info, &values, TRIGGERED_FULL_MASK);
        me->DespawnOrUnsummon(1s);
    }
};
}

void AddSC_AscensionPrimalistSpiritOfLife()
{
    RegisterCreatureAI(npc_ascension_primalist_spirit_of_life);
}
