/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
// Stormbringer (class 16, family 22): the Electrified Water Elemental 310603 and its two
// talents. The creature itself, and the Thunder Orb 50074 of Binder of Storms 707542, are data:
// 2026_09_30_31_ascension_creatures_stormbringer.sql. Written on 2026-09-30.
//
// Read in Spell.dbc (server copy, /opt/coa/server/data/dbc), 2026-09-30:
//   524954 Static Electricity: e0 aura 42 -> 573451, ProcFlags 0. "Critical strikes with
//          Torrential Wrath now spawn an Electrified Water Elemental ... When an Electrified
//          Water Elemental dissipates, the cooldown of your Drown is reduced by 2 sec."
//          573451 is effect 178 (EffectNULL in this core): the summon 573438 is cast here.
//   573436 Electrified Waters: e0 aura 42 -> 573437 (effect 64 -> 573438), ProcFlags 0.
//          "Your direct critical strikes against an enemy affected by Drown spawn an
//          Electrified Water Elemental ... for $573438d." spell_proc 573436 + the AuraScript
//          below (the Drown check); the default action is replaced by a direct 573438.
//   573438 Electrified Waters [Summon]: effect 28, MiscValue 310603, SummonProperties 61
//          (Category 1 ALLY, Type 2 GUARDIAN), duration 20 s.
//   573442 Electrified [Proc Onto Player]: effect 165 (ASCENSION_MODIFY_COOLDOWN), MiscValue
//          806407 (Drown rank 1), -2000 ms. The native effect only touches the named id; Drown
//          has six ranks in spell_ranks (806407, 572760..572764), so the reduction is applied
//          here over the whole chain, with the DBC's value and target.
#include "Creature.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PetDefines.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <list>
#include <vector>

namespace
{
enum StormbringerSummonSpells : uint32
{
    SPELL_STATIC_ELECTRICITY = 524954,
    SPELL_ELECTRIFIED_WATERS = 573436,
    SPELL_ELECTRIFIED_WATERS_SUMMON = 573438,
    SPELL_ELECTRIFIED_DROWN_COOLDOWN = 573442,
    SPELL_DROWN = 806407
};

constexpr uint32 NPC_ELECTRIFIED_WATER_ELEMENTAL = 310603;
constexpr uint32 STORMBRINGER_FAMILY = 22;
// Leash, as the Reaper and Xoroth summons.
constexpr float ELEMENTAL_LEASH = 100.0f;
// SUPPOSED (design choice, no tooltip gives one): at most three elementals per Stormbringer.
// Electrified Waters fires on every direct critical against a drowned enemy, and each
// dissipation shortens Drown: without a ceiling the pair feeds itself. The oldest one leaves,
// silently (no Drown reduction), when a fourth arrives.
constexpr size_t ELEMENTAL_CAP = 3;
constexpr uint8 MAX_CHAIN_WALK = 16;

// Same record list as AscensionStormbringerTalents.cpp IsTorrentialWrath (anonymous namespace
// there): Torrential Wrath is not chained in spell_ranks, each rank is named.
bool IsTorrentialWrath(uint32 id) { return id == 804017 || (id >= 503352 && id <= 503360); }

// 573442's record applied over the whole Drown chain (same shape as ReduceCooldownOverChain in
// AscensionStormbringerEvents.cpp): a reduction at least as long as what is left clears it.
void ApplyDrownReduction(Player* player)
{
    SpellInfo const* record = sSpellMgr->GetSpellInfo(SPELL_ELECTRIFIED_DROWN_COOLDOWN);
    if (!player || !record)
        return;
    SpellEffectInfo const& effect = record->Effects[EFFECT_0];
    if (effect.Effect != SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN || effect.MiscValue <= 0)
        return;
    int32 delta = effect.CalcValue(player);
    if (delta >= 0)
        return;
    uint32 reduction = uint32(-int64(delta));
    uint32 current = sSpellMgr->GetFirstSpellInChain(uint32(effect.MiscValue));
    for (uint8 step = 0; current && step < MAX_CHAIN_WALK; ++step)
    {
        if (uint32 remaining = player->GetSpellCooldownDelay(current))
        {
            if (reduction >= remaining)
                player->RemoveSpellCooldown(current, true);
            else
                player->ModifySpellCooldown(current, delta);
        }
        current = sSpellMgr->GetNextSpellInChain(current);
    }
}

void SummonElemental(Player* player)
{
    if (player && sSpellMgr->GetSpellInfo(SPELL_ELECTRIFIED_WATERS_SUMMON))
        player->CastSpell(player, SPELL_ELECTRIFIED_WATERS_SUMMON, true);
}

// Health and weapon damage follow the master, rescaled every 2 s. SUPPOSED values, read
// nowhere: 40% of the master's maximum health, and per swing (2.0 s, Frost, dmgschool 4 in
// creature_template) 2 to 3 damage per level plus 20 to 30% of the master's Frost spell power
// (Torrential Wrath and Conduction are Frost). The master's armor is copied.
void ScaleElemental(Creature* unit, Player* player)
{
    uint32 maximum = std::max(1u, uint32(player->GetMaxHealth() * 0.4f));
    float percent = unit->GetMaxHealth() ? unit->GetHealthPct() / 100.0f : 1.0f;
    float power = float(std::max(0, player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST)));
    float level = float(player->GetLevel());
    unit->SetLevel(player->GetLevel());
    unit->SetMaxHealth(maximum);
    unit->SetHealth(std::max(1u, uint32(maximum * percent)));
    unit->SetArmor(player->GetArmor());
    unit->SetAttackTime(BASE_ATTACK, 2000);
    unit->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, level * 2.0f + power * 0.2f);
    unit->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, level * 3.0f + power * 0.3f);
    unit->UpdateDamagePhysical(BASE_ATTACK);
}

// Electrified Water Elemental 310603: "to aid you in combat". Melee on the master's target,
// follows otherwise. Despawns as soon as the master is dead, gone, out of the map or beyond
// 100 yards; one spawned without a Stormbringer master (".npc add") despawns at once. When it
// leaves while its master is still there (end of the 20 s, or killed), Drown is shortened by
// 573442 -- the tooltip's "dissipates". Not when replaced by the ceiling above, not when the
// master is the one who left.
struct npc_ascension_stormbringer_water_elemental : public ScriptedAI
{
    explicit npc_ascension_stormbringer_water_elemental(Creature* creature) : ScriptedAI(creature) { }

    ObjectGuid _master;
    uint32 _scaleTimer = 2000;
    bool _silent = false;
    bool _paid = false;

    Player* Master() const
    {
        Player* player = _master ? ObjectAccessor::GetPlayer(*me, _master) : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || !player->IsAlive() || !player->IsInWorld() ||
            !me->IsWithinDistInMap(player, ELEMENTAL_LEASH))
            return nullptr;
        return player;
    }

    void IsSummonedBy(WorldObject* summoner) override
    {
        Player* player = summoner ? summoner->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || !player->IsAlive() || me->GetOwnerGUID() != player->GetGUID())
        {
            _silent = true;
            me->DespawnOrUnsummon(1ms);
            return;
        }
        _master = player->GetGUID();

        // Ceiling: the ones with the least time left go first.
        std::list<Creature*> summons;
        player->GetAllMinionsByEntry(summons, NPC_ELECTRIFIED_WATER_ELEMENTAL);
        std::vector<Creature*> others;
        for (Creature* summon : summons)
            if (summon != me && summon->IsAlive() && summon->IsInWorld() && summon->IsSummon())
                others.push_back(summon);
        if (others.size() >= ELEMENTAL_CAP)
        {
            std::sort(others.begin(), others.end(), [](Creature* a, Creature* b)
                { return a->ToTempSummon()->GetTimer() < b->ToTempSummon()->GetTimer(); });
            for (size_t i = 0; i + ELEMENTAL_CAP <= others.size(); ++i)
            {
                if (auto* ai = dynamic_cast<npc_ascension_stormbringer_water_elemental*>(others[i]->AI()))
                    ai->_silent = true;
                others[i]->DespawnOrUnsummon(1ms);
            }
        }

        me->SetFaction(player->GetFaction());
        ScaleElemental(me, player);
        me->SetFullHealth();
        me->SetReactState(REACT_PASSIVE); // targets are the master's, chosen in UpdateAI
    }

    void OnDespawn() override
    {
        if (_silent || _paid)
            return;
        _paid = true;
        if (Player* player = _master ? ObjectAccessor::GetPlayer(*me, _master) : nullptr)
            if (player->IsInWorld() && player->IsAlive() && player->getClass() == CLASS_STORMBRINGER)
                ApplyDrownReduction(player);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        if (!me->IsAlive())
            return;
        me->CombatStop(true);
        if (Player* player = Master())
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                me->GetMotionMaster()->Clear(false);
                me->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, me->GetFollowAngle(), MOTION_SLOT_ACTIVE);
            }
    }

    Unit* MasterTarget(Player* player) const
    {
        Unit* target = player->GetVictim();
        if (!target && player->IsInCombat())
            target = player->GetSelectedUnit();
        if (!target || target == me || !target->IsAlive() || !player->IsValidAttackTarget(target) ||
            !me->IsValidAttackTarget(target) || !target->IsInMap(me))
            return nullptr;
        return target;
    }

    void UpdateAI(uint32 diff) override
    {
        Player* player = Master();
        if (!player)
        {
            // The master left or died: no Drown to shorten.
            _silent = true;
            me->DespawnOrUnsummon();
            return;
        }

        if (_scaleTimer <= diff)
        {
            ScaleElemental(me, player);
            _scaleTimer = 2000;
        }
        else
            _scaleTimer -= diff;

        Unit* target = MasterTarget(player);
        if (!target)
        {
            if (me->GetVictim())
                me->AttackStop();
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, me->GetFollowAngle(), MOTION_SLOT_ACTIVE);
            return;
        }
        if (me->GetVictim() != target)
            AttackStart(target);
        DoMeleeAttackIfReady();
    }
};

// 573436 Electrified Waters. spell_proc opens direct hits (auto attack, melee, none and magic
// damage classes), damage type, hit phase, critical only, untriggered only; this keeps the
// enemies carrying any rank of Drown from this Stormbringer. The authored chain 573437 ->
// 573438 is replaced by a direct 573438 (573437's effect 64 aimed at the enemy would make the
// enemy the explicit target of a self summon).
class aura_ascension_stormbringer_electrified_waters : public AuraScript
{
    PrepareAuraScript(aura_ascension_stormbringer_electrified_waters);

    bool CheckProc(ProcEventInfo& event)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        Unit* victim = event.GetActionTarget();
        if (!player || player->getClass() != CLASS_STORMBRINGER || event.GetActor() != player || !victim ||
            victim == player || !victim->IsAlive() || !(event.GetHitMask() & PROC_HIT_CRITICAL))
            return false;
        return victim->GetAuraOfRankedSpell(SPELL_DROWN, player->GetGUID()) != nullptr;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*event*/)
    {
        PreventDefaultAction();
        SummonElemental(GetTarget() ? GetTarget()->ToPlayer() : nullptr);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_stormbringer_electrified_waters::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_stormbringer_electrified_waters::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// 524954 Static Electricity: a critical Torrential Wrath (untriggered cast, one summon per
// cast) spawns the elemental. Torrential Wrath has a single target; the per-cast marker is
// kept anyway, as in AscensionStormbringerTalents.cpp (FirstForCast).
class stormbringer_static_electricity : public AllSpellScript
{
public:
    stormbringer_static_electricity() : AllSpellScript("stormbringer_static_electricity", {ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 /*damage*/, uint32, bool critical) override
    {
        if (!critical || miss != SPELL_MISS_NONE || !spell || !target)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || info->SpellFamilyName != STORMBRINGER_FAMILY || !IsTorrentialWrath(info->Id) || spell->IsTriggered())
            return;
        Player* player = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_STORMBRINGER || target == player || player->IsFriendlyTo(target) ||
            !player->HasAura(SPELL_STATIC_ELECTRICITY) || spell->GetScriptValue(SPELL_STATIC_ELECTRICITY))
            return;
        spell->SetScriptValue(SPELL_STATIC_ELECTRICITY, 1);
        SummonElemental(player);
    }
};
}

void AddSC_AscensionStormbringerSummons()
{
    RegisterCreatureAI(npc_ascension_stormbringer_water_elemental);
    RegisterSpellScript(aura_ascension_stormbringer_electrified_waters);
    new stormbringer_static_electricity();
}
