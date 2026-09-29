/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Containers.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
enum MountainSpells : uint32
{
    EarthsRage = 806068,
    CallOfTheMountain = 680406,
    MountainBuff = 680472
};

class aura_ascension_blessed_by_earth : public AuraScript
{
    PrepareAuraScript(aura_ascension_blessed_by_earth);

    bool CheckProc(ProcEventInfo& event)
    {
        Unit* owner = GetTarget();
        DamageInfo const* damage = event.GetDamageInfo();
        SpellInfo const* info = damage ? damage->GetSpellInfo() : nullptr;
        return owner->IsPlayer() && owner->getClass() == CLASS_WILDWALKER && owner->IsAlive() &&
            event.GetActor() == owner && damage && damage->GetDamage() && info && info->SpellFamilyName == 37 &&
            ((info->SpellFamilyFlags[0] & 16384) || (info->SpellFamilyFlags[2] & 256)) &&
            event.GetActionTarget() && !owner->IsFriendlyTo(event.GetActionTarget());
    }

    void Gain(AuraEffect const*, ProcEventInfo& event)
    {
        PreventDefaultAction();
        if (CheckProc(event))
            GetTarget()->CastSpell(GetTarget(), EarthsRage, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_blessed_by_earth::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_blessed_by_earth::Gain,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

class aura_ascension_mountain_threshold : public AuraScript
{
    PrepareAuraScript(aura_ascension_mountain_threshold);
    uint8 _previous = 0;

    void Changed(AuraEffect const*, AuraEffectHandleModes mode)
    {
        uint8 current = GetStackAmount();
        bool crossed = _previous < 5 && current >= 5;
        _previous = current;
        // REAL also runs when saved auras load. Loading five stacks is not a gain.
        Unit* owner = GetTarget();
        if (mode == AURA_EFFECT_HANDLE_REAL || !crossed || !owner->IsPlayer() ||
            owner->getClass() != CLASS_WILDWALKER || !owner->IsAlive() ||
            GetCasterGUID() != owner->GetGUID() || !owner->HasAura(CallOfTheMountain))
            return;
        owner->CastSpell(owner, MountainBuff, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_mountain_threshold::Changed,
            EFFECT_0, SPELL_AURA_MOD_INCREASE_SPEED, AURA_EFFECT_HANDLE_CHANGE_AMOUNT_MASK);
    }
};

// ---------------------------------------------------------------------------------
// Vague 5 : talents qui reagissent au LANCEMENT d'un sort ou a chaque cible touchee, et
// qu'aucune ligne spell_proc ne sait exprimer (nombre de cibles, pile consommee, sort
// remplace, repoussement lateral). AllSpellScript : aucune ligne spell_script_names.
// OnSpellCast part a la fin de Spell::_cast, apres les effets immediats et les
// AfterCast (Spell.cpp:4102-4129) ; OnSpellHitResult une fois par cible unique.
// ---------------------------------------------------------------------------------
enum PrimalistWave5Spells : uint32
{
    // premiers rangs des chaines spell_ranks
    SPELL_QUAKE_R1            = 803974,
    SPELL_MOUNTAIN_HAMMER_R1  = 681130,
    SPELL_TERRASURGE_R1       = 681119,
    SPELL_WILDCLAW_R1         = 800140,
    SPELL_TOTEMIC_SMASH_R1    = 800178,
    SPELL_MAGMA_FISSURE_R1    = 802793,
    SPELL_GEODE_BARRAGE_R1    = 500402,
    // talents et effets
    SPELL_QUAKING_THANE       = 561193,
    SPELL_THANES_RAGE         = 804124,
    SPELL_HEAVY_EARTH         = 706160,
    SPELL_HEAVY_EARTH_PROC    = 560142,
    SPELL_EARTHSHAPING_STACKS = 680441,
    SPELL_MOUNTAIN_MOVER_BUFF = 805644,
    SPELL_EMPOWERED_BOONS     = 706340,
    SPELL_MAGMA_FISSURE_BUFF  = 803299,
    SPELL_LITHIC_LANCE_READY  = 807048,
    SPELL_LITHIC_LANCE        = 681251,
    SPELL_PROTECTIVE_ROAR     = 802782,
    SPELL_EARTHMOTHERS_ROAR   = 301306,
    SPELL_EARTHMOTHERS_HEAL   = 301307,
    SPELL_NATURES_CALL        = 800146,
    SPELL_NATURES_CALL_TAUNT  = 680430,
    SPELL_BOULDER_DASH        = 500692,
    SPELL_BOULDER_DASH_HIT    = 500693
};

struct EmpoweredBoon
{
    uint32 boon;
    uint32 empowered;
};

// Boons lus dans Events.cpp (FURY_OF_THE_WILD_BOONS) ; Empowered lus dans Spell.dbc (545465, 545501).
constexpr std::array<EmpoweredBoon, 5> EMPOWERED_BOONS =
{{
    {500935, 523523},   // Turtle
    {500939, 523524},   // Bear
    {500943, 523526},   // Hawk
    {504856, 505369},   // Lion (505369 declenche 505220)
    {800137, 523527}    // Wolf
}};

Player* PrimalistSpellCaster(Spell const* spell)
{
    Unit* caster = spell ? spell->GetCaster() : nullptr;
    Player* player = caster ? caster->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_WILDWALKER ? player : nullptr;
}

bool IsRankOf(SpellInfo const* info, uint32 firstRank)
{
    return info && info->SpellFamilyName == 37 && sSpellMgr->GetFirstSpellInChain(info->Id) == firstRank;
}

// Gardes de Spell::EffectKnockBack (SpellEffects.cpp:5324-5341).
bool CanBeKnockedBack(Unit* target)
{
    if (!target || !target->IsAlive() || target->GetVehicle() || target->HasUnitState(UNIT_STATE_ROOT))
        return false;
    if (target->IsVehicle() && target->GetCreatureType() != CREATURE_TYPE_BEAST)
        return false;
    if (Creature* creature = target->ToCreature())
        if (creature->isWorldBoss() || creature->IsDungeonBoss() || creature->IsImmuneToKnockback() ||
            creature->GetCreatureType() == CREATURE_TYPE_GIANT)
            return false;
    return true;
}

class primalist_wave5_spells : public AllSpellScript
{
public:
    primalist_wave5_spells() : AllSpellScript("primalist_wave5_spells",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellCast(Spell* spell, Unit*, SpellInfo const* info, bool) override
    {
        Player* player = PrimalistSpellCaster(spell);
        if (!player || !info || info->SpellFamilyName != 37 || !player->IsAlive() || spell->IsTriggered())
            return;

        // 561193 Quaking Thane : « casting Mountain Hammer now grants you Thane's Rage ».
        if (IsRankOf(info, SPELL_MOUNTAIN_HAMMER_R1) && player->HasAura(SPELL_QUAKING_THANE))
            player->CastSpell(player, SPELL_THANES_RAGE, true);

        // 706160 Heavy Earth : Terrasurge consomme toutes les piles d'Earthshaping ; 560142
        // retire une pile (effet 175), rafraichit Golem Form (effet 173) et pose Heavy Earth.
        // Nombre lu AVANT la boucle : chaque lancement en retire une.
        if (IsRankOf(info, SPELL_TERRASURGE_R1) && player->HasAura(SPELL_HEAVY_EARTH))
            if (Aura const* stacks = player->GetAura(SPELL_EARTHSHAPING_STACKS, player->GetGUID()))
                for (uint8 i = 0, n = stacks->GetStackAmount(); i < n; ++i)
                    player->CastSpell(player, SPELL_HEAVY_EARTH_PROC, true);

        // 805643 Mountain Mover : Wildclaw consomme les piles. Les mods de 805644 ont deja
        // servi au calcul du cout et des degats (effets immediats de la melee).
        if (IsRankOf(info, SPELL_WILDCLAW_R1))
            player->RemoveAurasDueToSpell(SPELL_MOUNTAIN_MOVER_BUFF, player->GetGUID());

        // 706340 Empowered Boons : seulement l'Empowered du Boon actif. Pas de spell_proc :
        // 545465 donnerait les cinq a la fois.
        if (IsRankOf(info, SPELL_TOTEMIC_SMASH_R1) && player->HasAura(SPELL_EMPOWERED_BOONS))
            for (EmpoweredBoon const& boon : EMPOWERED_BOONS)
                if (player->HasAura(boon.boon, player->GetGUID()))
                    player->CastSpell(player, boon.empowered, true);

        // 802793 Magma Fissure : « Your next spell within 6 sec is instant cast ». 803299
        // recoit ProcCharges = 1 (primalist_secondary_metadata) : un seul sort en profite.
        if (IsRankOf(info, SPELL_MAGMA_FISSURE_R1))
            player->CastSpell(player, SPELL_MAGMA_FISSURE_BUFF, true);

        // 706159 Lithic Lance : le prochain Geode Barrage devient Lithic Lance. Le canal
        // vient de poser son aura 227 sur la cible (premiere pierre a 500 ms) : la retirer
        // clot le canal (Spell::UpdateChanneledTargetList), sans pierre ni gain de Rage.
        if (IsRankOf(info, SPELL_GEODE_BARRAGE_R1) && player->HasAura(SPELL_LITHIC_LANCE_READY))
            if (Unit* target = spell->m_targets.GetUnitTarget())
            {
                target->RemoveAurasDueToSpell(info->Id, player->GetGUID());
                player->RemoveAurasDueToSpell(SPELL_LITHIC_LANCE_READY);
                player->CastSpell(target, SPELL_LITHIC_LANCE, true);
            }

        // 301306 Earthmother's Roar : l'effet 1 de Protective Roar est un DUMMY (tA 0) que
        // rien ne lit. 301307 cible lui-meme le raid autour du lanceur. Pas de spell_proc sur
        // 301306 : le soin serait compte deux fois.
        if (info->Id == SPELL_PROTECTIVE_ROAR && player->HasAura(SPELL_EARTHMOTHERS_ROAR))
            player->CastSpell(player, SPELL_EARTHMOTHERS_HEAL, true);

        // 800146 Nature's Call : provoque jusqu'a 3 attaquants de l'allie cible, comme
        // Righteous Defense (SpellEffects.cpp:1033-1057). 680430 porte la provocation de 3 s.
        if (info->Id == SPELL_NATURES_CALL)
            if (Unit* ally = spell->m_targets.GetUnitTarget())
            {
                std::vector<Unit*> attackers;
                for (Unit* attacker : ally->getAttackers())
                    if (attacker && attacker->IsAlive() && player->IsValidAttackTarget(attacker) &&
                        attacker->IsInMap(player))
                        attackers.push_back(attacker);
                Acore::Containers::RandomResize(attackers, 3);
                for (Unit* attacker : attackers)
                    player->CastSpell(attacker, SPELL_NATURES_CALL_TAUNT, true);
            }
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32, uint32, bool) override
    {
        Player* player = PrimalistSpellCaster(spell);
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !info || info->SpellFamilyName != 37 || miss != SPELL_MISS_NONE || !target ||
            target == player || player->IsFriendlyTo(target))
            return;

        // 561193 Quaking Thane : « Striking at least 5 enemies with Quake ». Une fois par lancer.
        if (IsRankOf(info, SPELL_QUAKE_R1) && player->HasAura(SPELL_QUAKING_THANE))
        {
            uint64 count = spell->GetScriptValue(SPELL_QUAKING_THANE) + 1;
            spell->SetScriptValue(SPELL_QUAKING_THANE, count);
            if (count == 5)
                player->CastSpell(player, SPELL_THANES_RAGE, true);
        }

        // 802793 Magma Fissure : l'effet 1 est un SCRIPT_EFFECT (77, misc 15, miscB 5) que rien
        // ne traite. Repousser chaque ennemi touche sur le cote de la ligne. SUPPOSE : misc et
        // miscB pris tels quels comme vitesses horizontale et verticale ; a regler en jeu.
        if (IsRankOf(info, SPELL_MAGMA_FISSURE_R1) && CanBeKnockedBack(target))
        {
            float o = player->GetOrientation();
            float dx = target->GetPositionX() - player->GetPositionX();
            float dy = target->GetPositionY() - player->GetPositionY();
            float along = dx * std::cos(o) + dy * std::sin(o);
            float side = -dx * std::sin(o) + dy * std::cos(o);
            // Point de la ligne le plus proche : la cible est poussee perpendiculairement a
            // la ligne. Une cible sur la ligne meme part a gauche du lanceur.
            float originX = player->GetPositionX() + along * std::cos(o);
            float originY = player->GetPositionY() + along * std::sin(o);
            if (std::fabs(side) < 0.5f)
            {
                originX -= std::cos(o + float(M_PI) / 2.0f);
                originY -= std::sin(o + float(M_PI) / 2.0f);
            }
            SpellEffectInfo const& effect = info->Effects[EFFECT_1];
            float speedXY = float(std::max(effect.MiscValue, 1));
            float speedZ = float(std::max(effect.MiscValueB, 1));
            if (target->IsNonMeleeSpellCast(true))
                target->InterruptNonMeleeSpells(true);
            target->KnockbackFrom(originX, originY, speedXY, speedZ);
        }

        // 500693 Boulder Dash : l'effet 1 est un DUMMY (bp 20, misc 40) sans script. Une seule
        // projection par cible et par ruee : 500693 part toutes les 500 ms depuis 500692, la
        // liste des cibles deja projetees vit sur cette aura. Vitesses selon la convention de
        // EffectKnockBack (misc/10, valeur/10). SUPPOSE, a regler en jeu.
        if (info->Id == SPELL_BOULDER_DASH_HIT && CanBeKnockedBack(target))
            if (Aura* dash = player->GetAura(SPELL_BOULDER_DASH, player->GetGUID()))
            {
                // Une ruee relancee dans les 10 s rafraichit la MEME aura : sa duree remonte, ce
                // qui ouvre une nouvelle generation. Valeur stockee = generation, cle tiree du GUID
                // brut entier (joueur et creature de meme compteur ne se confondent plus).
                constexpr uint32 GEN_KEY = 0xFFFFFFFFu;
                constexpr uint32 DUR_KEY = 0xFFFFFFFEu;
                int32 duration = dash->GetDuration();
                uint64 generation = dash->GetScriptValue(GEN_KEY);
                if (!generation || (duration >= 0 && uint64(duration) > dash->GetScriptValue(DUR_KEY)))
                    dash->SetScriptValue(GEN_KEY, ++generation);
                dash->SetScriptValue(DUR_KEY, uint64(std::max(duration, 0)));
                uint64 raw = target->GetGUID().GetRawValue();
                uint32 key = uint32(raw ^ (raw >> 32));
                if (key >= DUR_KEY)
                    key -= 2;
                if (dash->GetScriptValue(key) == generation)
                    return;
                dash->SetScriptValue(key, generation);
                SpellEffectInfo const& effect = info->Effects[EFFECT_1];
                float speedXY = float(effect.MiscValue) * 0.1f;
                float speedZ = float(effect.CalcValue(player)) * 0.1f;
                if (speedXY > 0.1f || speedZ > 0.1f)
                    target->KnockbackFrom(player->GetPositionX(), player->GetPositionY(), speedXY, speedZ);
            }
    }
};

class mountain_talent_metadata : public GlobalScript
{
public:
    mountain_talent_metadata() : GlobalScript("mountain_talent_metadata", {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        // Old trigger granted Mountain on the first stack without its talent.
        if (info->Id == EarthsRage && info->SpellFamilyName == 37)
            info->Effects[EFFECT_2].Effect = 0;
    }
};
}

void AddSC_AscensionPrimalistMountain()
{
    RegisterSpellScript(aura_ascension_blessed_by_earth);
    RegisterSpellScript(aura_ascension_mountain_threshold);
    new mountain_talent_metadata();
    new primalist_wave5_spells();
}
