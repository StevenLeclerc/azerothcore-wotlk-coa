/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Config.h"
#include "Creature.h"
#include "EventMap.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include <algorithm>

namespace
{
enum ZephyrSpells : uint32
{
    SPELL_RAGING_ZEPHYR = 704201,
    SPELL_ZEPHYR_PERIODIC = 725390,
    SPELL_ZEPHYR_PULL = 704210
};

enum ZephyrEntries : uint32
{
    NPC_RAGING_ZEPHYR = 4078281,
    EVENT_ZEPHYR_OWNER_CHECK = 1
};

bool HasZephyrOwner(Creature* zephyr)
{
    if (!zephyr || zephyr->GetEntry() != NPC_RAGING_ZEPHYR || !zephyr->IsAlive() || !zephyr->IsInWorld())
        return false;
    Player* owner = zephyr->GetCharmerOrOwnerPlayerOrPlayerItself();
    return owner && owner->GetGUID() == zephyr->GetOwnerGUID() && owner->getClass() == CLASS_STORMBRINGER &&
        owner->IsAlive() && owner->IsInWorld() && owner->HasActiveSpell(SPELL_RAGING_ZEPHYR) &&
        owner->GetMap() == zephyr->GetMap() && owner->InSamePhase(zephyr);
}

struct npc_ascension_stormbringer_zephyr : ScriptedAI
{
    explicit npc_ascension_stormbringer_zephyr(Creature* creature) : ScriptedAI(creature) { }
    EventMap events;

    void AttackStart(Unit*) override { }
    void MoveInLineOfSight(Unit*) override { }
    void EnterEvadeMode(EvadeReason) override { }

    void IsSummonedBy(WorldObject* summoner) override
    {
        Player* owner = summoner ? summoner->ToPlayer() : nullptr;
        if (!owner || owner->getClass() != CLASS_STORMBRINGER || !owner->HasActiveSpell(SPELL_RAGING_ZEPHYR))
        {
            me->DespawnOrUnsummon();
            return;
        }
        me->SetOwnerGUID(owner->GetGUID());
        me->SetFaction(owner->GetFaction());
        me->SetLevel(owner->GetLevel());
        me->SetReactState(REACT_PASSIVE);
        me->GetMotionMaster()->Clear();
        me->GetMotionMaster()->MoveIdle();
        events.ScheduleEvent(EVENT_ZEPHYR_OWNER_CHECK, Milliseconds(500));
    }

    void UpdateAI(uint32 diff) override
    {
        events.Update(diff);
        if (events.ExecuteEvent() != EVENT_ZEPHYR_OWNER_CHECK)
            return;
        if (!HasZephyrOwner(me))
        {
            me->DespawnOrUnsummon();
            return;
        }
        events.ScheduleEvent(EVENT_ZEPHYR_OWNER_CHECK, Milliseconds(500));
    }
};

class spell_ascension_raging_zephyr : public SpellScript
{
    PrepareSpellScript(spell_ascension_raging_zephyr);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_ZEPHYR_PERIODIC, SPELL_ZEPHYR_PULL}); }

    bool Load() override
    {
        Player* player = GetCaster()->ToPlayer();
        return player && player->getClass() == CLASS_STORMBRINGER;
    }

    void Summon(SpellEffIndex index)
    {
        PreventHitDefaultEffect(index);
        Player* player = GetCaster()->ToPlayer();
        WorldLocation const* destination = GetHitDest();
        int32 duration = GetSpellInfo()->GetDuration();
        player->ApplySpellMod(SPELL_RAGING_ZEPHYR, SPELLMOD_DURATION, duration);
        if (!destination || duration <= 0 || !player->HasActiveSpell(SPELL_RAGING_ZEPHYR))
            return;
        // Each charge creates an independent, stationary tornado; native Guardians follow the owner.
        TempSummon* zephyr = player->SummonCreature(NPC_RAGING_ZEPHYR, *destination,
            TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, uint32(duration));
        if (!zephyr)
            return;
        zephyr->SetUInt32Value(UNIT_CREATED_BY_SPELL, SPELL_RAGING_ZEPHYR);
        zephyr->CastSpell(zephyr, SPELL_ZEPHYR_PERIODIC, true);
        if (Aura* aura = zephyr->GetAura(SPELL_ZEPHYR_PERIODIC, zephyr->GetGUID()))
        {
            // The copied helper lasts eight seconds; the active summon promises twelve.
            // Preserve native periodic timing, root and school immunity for the summon lifetime.
            aura->SetMaxDuration(duration);
            aura->SetDuration(duration);
        }
        else
            zephyr->DespawnOrUnsummon();
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_ascension_raging_zephyr::Summon, EFFECT_0, SPELL_EFFECT_SUMMON);
    }
};

class spell_ascension_zephyr_pull : public SpellScript
{
    PrepareSpellScript(spell_ascension_zephyr_pull);

    void CheckTarget(SpellEffIndex index)
    {
        Creature* zephyr = GetCaster()->ToCreature();
        Unit* target = GetHitUnit();
        if (!HasZephyrOwner(zephyr) || !target || !target->IsAlive() || !target->IsInWorld() ||
            target->GetMap() != zephyr->GetMap() || !zephyr->InSamePhase(target) ||
            !zephyr->IsValidAttackTarget(target) || target->IsInFlight() || target->GetVehicle() ||
            target->GetTransport())
        {
            PreventHitDefaultEffect(index);
            return;
        }
        if (Player* player = target->ToPlayer())
            if (player->IsBeingTeleported())
                PreventHitDefaultEffect(index);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_zephyr_pull::CheckTarget,
            EFFECT_0, SPELL_EFFECT_PULL_TOWARDS);
    }
};

// CoA (21/10/2026, R-20261021-1505, coa-project/docs/DIAG-bot-envol-kattailu-20261020.md) : Whirlpool (526362)
// projette les personnages pris dans son tourbillon par 526365 (KNOCK_BACK, vz 15 : ~5,8 m, ~1,6 s en l'air). La
// projection reste telle quelle (decision du commanditaire) ; seul un repere visuel s'y ajoute, pour qu'un joueur
// comprenne qu'un personnage en l'air est pris dans un sort et non qu'il vole. Au moment ou 526365 frappe un
// personnage (joueur ou bot) que le coeur va effectivement projeter, on pose sur lui l'aura cosmetique 47335
// « Cyclone Impact Visual (Blue) » (Spell.dbc client = serveur, md5 identique) : un seul effet APPLY_AURA DUMMY,
// aucun gestionnaire dans le coeur ni dans ce module, aucune ligne spell_script_names / spell_proc / spell_linked_spell,
// cachee de la barre d'auras et du journal de combat (Attributes 0x180, AttributesEx 0x10000000), non dissipable
// (DispelType 0). Son SpellVisual 10556 porte le StateKit 9711 : Spells\CycloneWater_State, echelle 3, au pied du
// personnage, donc il le suit pendant la projection ; modele, peau et 4 textures presents dans le client (common-2,
// common, patch-N). Lancee par la cible sur elle-meme, sans cout, sans portee, sans declenchement. Duree ramenee a
// AscensionCompat.WhirlpoolAirborneVisualMs (2000 par defaut ; 3000 dans le DBC). Option
// AscensionCompat.WhirlpoolAirborneVisual (defaut 1), relue a chaque projection. Lie par spell_script_names
// (data/sql/db-world/2026_10_21_01_ascension_whirlpool_visuel.sql) : sans cette ligne, rien ne change.
enum WhirlpoolVisualSpells : uint32
{
    SPELL_WHIRLPOOL_KNOCK_UP = 526365,
    SPELL_WHIRLPOOL_AIRBORNE_VISUAL = 47335
};

class spell_ascension_whirlpool_airborne_visual : public SpellScript
{
    PrepareSpellScript(spell_ascension_whirlpool_airborne_visual);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return spellInfo->Id == SPELL_WHIRLPOOL_KNOCK_UP &&
            spellInfo->Effects[EFFECT_0].Effect == SPELL_EFFECT_KNOCK_BACK &&
            ValidateSpellInfo({SPELL_WHIRLPOOL_AIRBORNE_VISUAL});
    }

    // Runs before the default knockback handler of the same effect: reproduce the core's own refusals for a player
    // (Spell::EffectKnockBack) so the cyclone is only shown on a character that is really thrown up.
    void ShowAirborneVisual(SpellEffIndex /*effIndex*/)
    {
        if (!sConfigMgr->GetOption<bool>("AscensionCompat.Enable", true) ||
            !sConfigMgr->GetOption<bool>("AscensionCompat.WhirlpoolAirborneVisual", true))
            return;

        Player* target = GetHitPlayer();
        if (!target || !target->IsAlive() || !target->IsInWorld() || target->IsBeingTeleported() ||
            target->GetVehicle() || target->IsInFlight() || target->HasUnitState(UNIT_STATE_ROOT))
            return;

        int32 const duration = int32(std::clamp<uint32>(
            sConfigMgr->GetOption<uint32>("AscensionCompat.WhirlpoolAirborneVisualMs", 2000), 500, 5000));

        // A second Whirlpool tick on a character still in the air refreshes the same aura (one aura per character).
        Aura* aura = target->GetAura(SPELL_WHIRLPOOL_AIRBORNE_VISUAL, target->GetGUID());
        if (!aura)
            aura = target->AddAura(SPELL_WHIRLPOOL_AIRBORNE_VISUAL, target);
        if (!aura)
            return;
        aura->SetMaxDuration(duration);
        aura->SetDuration(duration);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_whirlpool_airborne_visual::ShowAirborneVisual,
            EFFECT_0, SPELL_EFFECT_KNOCK_BACK);
    }
};

class stormbringer_zephyr_metadata : public GlobalScript
{
public:
    stormbringer_zephyr_metadata() : GlobalScript("stormbringer_zephyr_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->SpellFamilyName != 22)
            return;
        if (info->Id == SPELL_RAGING_ZEPHYR)
            info->Effects[EFFECT_1].Effect = 0; // Inert copied area dummy; the tornado owns the periodic aura.
        if (info->Id == SPELL_ZEPHYR_PULL)
            info->Effects[EFFECT_1].Effect = 0; // Stale self-kill otherwise destroys the tornado on its first pulse.
    }
};
}

void AddSC_AscensionStormbringerZephyr()
{
    RegisterCreatureAI(npc_ascension_stormbringer_zephyr);
    RegisterSpellScript(spell_ascension_raging_zephyr);
    RegisterSpellScript(spell_ascension_zephyr_pull);
    RegisterSpellScript(spell_ascension_whirlpool_airborne_visual);
    new stormbringer_zephyr_metadata();
}
