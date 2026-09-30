/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
enum PrimalistAbilitySpells : uint32
{
    SPELL_GEODE_BARRAGE_DAMAGE = 803138,
    SPELL_GEODE_BARRAGE_RAGE = 802885
};

Player* Primalist(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_WILDWALKER ? player : nullptr;
}

class primalist_talent_events : public UnitScript
{
public:
    primalist_talent_events() : UnitScript("primalist_talent_events", true,
        {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnDamage(Unit*, Unit* victim, uint32& damage) override
    {
        Player* player = Primalist(victim);
        if (!player || !player->IsAlive() || !damage || damage < player->GetHealth() ||
            !player->HasAura(560157) || player->HasSpellCooldown(560157))
            return;
        // DealDamage reaches this hook after mitigation and absorption. Mark the
        // native saved cooldown before casting the heal, including any nested events.
        player->AddSpellCooldown(560157, 0, 120000);
        damage = 0;
        player->CastSpell(player, 560179, true);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = Primalist(unit);
        if (!player || !player->IsAlive() || !player->IsInWorld() || !application ||
            mode == AURA_REMOVE_BY_DEATH || !player->HasAura(300728))
            return;
        Aura* aura = application->GetBase();
        if (aura->GetCasterGUID() != player->GetGUID())
            return;
        // Only the visible defenses, not their separately removed SLS helpers.
        if (aura->GetId() == 680421 || aura->GetId() == 800094 || aura->GetId() == 503630)
            player->CastSpell(player, 503716, true);
    }
};

class primalist_talent_casts : public AllSpellScript
{
public:
    primalist_talent_casts() : AllSpellScript("primalist_talent_casts",
        {ALLSPELLHOOK_ON_CRIT_CHANCE, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32, uint32, bool) override
    {
        Player* player = Primalist(spell->GetCaster());
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !target || target == player || player->IsFriendlyTo(target) || miss != SPELL_MISS_NONE ||
            info->SpellFamilyName != 37 || info->Id != SPELL_GEODE_BARRAGE_DAMAGE ||
            spell->GetScriptValue(SPELL_GEODE_BARRAGE_RAGE))
            return;
        // Each channel tick casts this damage helper. Its authored energize
        // companion rolls 30-80 internal Rage (3-8 visible Rage) per successful stone.
        spell->SetScriptValue(SPELL_GEODE_BARRAGE_RAGE, 1);
        player->CastSpell(player, SPELL_GEODE_BARRAGE_RAGE, true);
    }

    void OnSpellCritChance(Spell* spell, Unit* target, float& chance) override
    {
        Player* player = Primalist(spell->GetCaster());
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !target || !player->HasAura(805336) || info->SpellFamilyName != 37 ||
            !(info->SpellFamilyFlags[0] & 8))
            return;
        for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
            if (effect->GetCasterGUID() == player->GetGUID() && effect->GetSpellInfo()->SpellFamilyName == 37 &&
                (effect->GetSpellInfo()->SpellFamilyFlags[0] & 64))
            {
                chance = 100.0f;
                break;
            }
    }
};

SpellCastResult CheckThroatClamp(Player* player, Unit* target)
{
    Pet* pet = player ? player->GetPet() : nullptr;
    if (!pet || !pet->IsAlive() || pet->GetOwnerGUID() != player->GetGUID())
        return SPELL_FAILED_NO_PET;
    if (!target || !pet->IsInMap(target) || !pet->InSamePhase(target) || !pet->IsValidAttackTarget(target))
        return SPELL_FAILED_BAD_TARGETS;
    if (pet->HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_LOST_CONTROL))
        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
    SpellInfo const* helper = sSpellMgr->GetSpellInfo(500811);
    if (!helper || !pet->IsWithinDistInMap(target, helper->GetMaxRange(false, pet)))
        return SPELL_FAILED_OUT_OF_RANGE;
    if (!pet->IsWithinLOSInMap(target))
        return SPELL_FAILED_LINE_OF_SIGHT;
    return SPELL_CAST_OK;
}

class spell_ascension_throat_clamp : public SpellScript
{
    PrepareSpellScript(spell_ascension_throat_clamp);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({500811}); }
    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    SpellCastResult CheckCast() { return CheckThroatClamp(Primalist(GetCaster()), GetExplTargetUnit()); }

    void Handle(SpellEffIndex)
    {
        Player* player = Primalist(GetCaster());
        Unit* target = GetHitUnit();
        if (CheckThroatClamp(player, target) == SPELL_CAST_OK)
            player->GetPet()->CastSpell(target, 500811, false); // Native dash, interrupt and school lockout.
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_throat_clamp::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_throat_clamp::Handle, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};
// ---------------------------------------------------------------------------------
// Vague 5. Les lignes spell_proc sont dans 2026_09_29_31_ascension_primalist_procs.sql et
// les noms de scripts dans 2026_09_29_33_ascension_primalist_scripts.sql : un DoCheckProc
// n'est jamais appele sans entree spell_proc (Aura::GetProcEffectMask, SpellAuras.cpp:2150).
// ---------------------------------------------------------------------------------
enum PrimalistWave5TalentSpells : uint32
{
    SPELL_THREE_HAMMERS_DAMAGE = 520468,
    SPELL_TOTEMIC_SMASH_R1     = 800178,
    SPELL_GEODE                = 804002,
    SPELL_EARTHEN_AVATAR       = 680421,
    SPELL_EMERALD_WILL         = 500302,
    SPELL_EMERALD_WILL_HEAL    = 525015,
    SPELL_SON_OF_URSOC_FORM    = 806048,
    SPELL_SACRED_GROVE         = 800180,
    SPELL_SACRED_GROVE_DATA    = 800186,
    SPELL_SACRED_GROVE_TICK    = 800179
};

constexpr uint32 SON_OF_URSOC_CREATURE = 421493;   // absente de creature_template (MESURE)
constexpr uint32 SON_OF_URSOC_DISPLAY  = 328642;   // present dans CreatureDisplayInfo.dbc (MESURE)
constexpr int32 MOUNTAIN_AVATAR_DURATION = 5000;   // « for 5 sec » ; 712446 bp 4999

// 520464 Mineralization : « While at or below 35% health, your spells and abilities ».
// Le seuil n'existe que dans le CasterAuraState de 520465, que le lancement declenche ignore.
class aura_ascension_primalist_mineralization : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_mineralization);

    bool Check(ProcEventInfo& eventInfo)
    {
        Unit* owner = GetTarget();
        return Primalist(owner) && owner->IsAlive() && eventInfo.GetActor() == owner && !owner->HealthAbovePct(35);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_mineralization::Check);
    }
};

// 520469 Shattering : seuls les degats des marteaux 520468 comptent (sort sans drapeau de famille).
class aura_ascension_primalist_shattering : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_shattering);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info &&
            info->Id == SPELL_THREE_HAMMERS_DAMAGE;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_shattering::Check);
    }
};

// 503718 Infused With Power : « Effectively healing allies » - soin effectif non nul.
class aura_ascension_primalist_infused_with_power : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_infused_with_power);

    bool Check(ProcEventInfo& eventInfo)
    {
        HealInfo const* heal = eventInfo.GetHealInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && heal && heal->GetEffectiveHeal();
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_infused_with_power::Check);
    }
};

// 500937 Totemic Echoes : le bit 16 de FF2 porte aussi Brutalize 560314, Totemic Crash 803534 et
// l'eclat 500945. Seuls les 7 rangs de Totemic Smash, et seulement leur cible principale : la phase
// HIT passe une fois par ennemi touche par l'eclat.
class aura_ascension_primalist_totemic_echoes : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_totemic_echoes);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info && spell &&
            info->SpellFamilyName == 37 && sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_TOTEMIC_SMASH_R1 &&
            eventInfo.GetActionTarget() && spell->m_targets.GetUnitTarget() == eventInfo.GetActionTarget();
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_totemic_echoes::Check);
    }
};

// 547213 Totemic Crash : « At the end of the duration, if you are within 5 yds of the target ».
// Periode 1999 ms pour 2000 ms de duree : le seul tick est la fin.
class aura_ascension_primalist_totemic_crash : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_totemic_crash);

    void Tick(AuraEffect const*)
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsAlive() || !caster->IsInMap(GetTarget()) ||
            !caster->IsWithinDist(GetTarget(), 5.0f))
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_primalist_totemic_crash::Tick,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 707616 Mountain Avatar : e0 -> 712446 (effet 178, Spell::EffectNULL : ne donne rien),
// e1 -> 712465 (effet 177, prolonge 680421 de 5 s, natif). On donne OU on prolonge.
class aura_ascension_primalist_mountain_avatar : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_mountain_avatar);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        DamageInfo const* damage = eventInfo.GetDamageInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info && damage &&
            damage->GetDamage() && (info->Id == SPELL_GEODE_BARRAGE_DAMAGE || info->Id == SPELL_GEODE);
    }

    void Grant(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        _granted = false;
        Unit* owner = GetTarget();
        if (owner->HasAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
            return;
        owner->CastSpell(owner, SPELL_EARTHEN_AVATAR, true);
        if (Aura* avatar = owner->GetAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
        {
            avatar->SetMaxDuration(MOUNTAIN_AVATAR_DURATION);
            avatar->SetDuration(MOUNTAIN_AVATAR_DURATION);
            _granted = true;
        }
    }

    void Extend(AuraEffect const*, ProcEventInfo&)
    {
        Unit* owner = GetTarget();
        if (_granted || !owner->HasAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
            PreventDefaultAction();
        _granted = false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_mountain_avatar::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_mountain_avatar::Grant,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_mountain_avatar::Extend,
            EFFECT_1, SPELL_AURA_PROC_TRIGGER_SPELL);
    }

    bool _granted = false;
};

// 680421 Earthen Avatar : « immune to stun, root and snare » - aucun des trois effets DBC
// (auras 79, 138, 87) ne l'accorde. Un seul id pour le talent et la version de 5 s.
class aura_ascension_primalist_earthen_avatar : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_earthen_avatar);

    void Immune(Unit* target, bool apply)
    {
        for (uint32 mechanic : {MECHANIC_STUN, MECHANIC_ROOT, MECHANIC_SNARE})
            target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, mechanic, apply);
    }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* target = GetTarget();
        Immune(target, true);
        target->RemoveAurasWithMechanic((1 << MECHANIC_STUN) | (1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE),
            AURA_REMOVE_BY_DEFAULT, GetId());
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        Immune(GetTarget(), false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_primalist_earthen_avatar::Apply,
            EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_primalist_earthen_avatar::Remove,
            EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 806048 Son of Ursoc : aura 56 vers la creature 421493, absente de creature_template ;
// HandleAuraTransform pose alors l'affichage 16358 (cochon rose). On pose l'affichage de
// l'ours, et la liberation des ralentissements que l'infobulle promet et qu'aucun effet ne porte.
// Au retrait, HandleAuraTransform restaure l'affichage d'origine.
class aura_ascension_primalist_son_of_ursoc : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_son_of_ursoc);

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* target = GetTarget();
        if (!sObjectMgr->GetCreatureTemplate(SON_OF_URSOC_CREATURE))
            target->SetDisplayId(SON_OF_URSOC_DISPLAY);
        target->RemoveMovementImpairingAuras(true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_primalist_son_of_ursoc::Apply,
            EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
    }
};

// 500302 Emerald Will : « Successfully dispelling a poison or disease effect with Soothing
// Touch ». Le coeur n'a pas de masque de coup « dissipation reussie » : on compte avant et apres.
class spell_ascension_primalist_soothing_touch : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_soothing_touch);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_EMERALD_WILL_HEAL}); }
    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    uint32 Count(Unit* target)
    {
        DispelChargesList list;
        target->GetDispellableAuraList(GetCaster(), (1 << DISPEL_DISEASE) | (1 << DISPEL_POISON), list,
            GetSpellInfo());
        uint32 charges = 0;
        for (auto const& entry : list)
            charges += entry.second;
        return charges;
    }

    void Before(SpellMissInfo miss)
    {
        Unit* target = GetHitUnit();
        _before = miss == SPELL_MISS_NONE && target && GetCaster()->HasAura(SPELL_EMERALD_WILL) ? Count(target) : 0;
    }

    void After()
    {
        Unit* target = GetHitUnit();
        if (_before && target && Count(target) < _before)
            GetCaster()->CastSpell(GetCaster(), SPELL_EMERALD_WILL_HEAL, true);
        _before = 0;
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ascension_primalist_soothing_touch::Before);
        AfterHit += SpellHitFn(spell_ascension_primalist_soothing_touch::After);
    }

    uint32 _before = 0;
};

// 800180 Sacred Grove : l'aura de zone (e1, DUMMY, passee en MOD_HEALING_PCT par
// primalist_secondary_metadata) ne portait rien. 800186 (tick toutes les
// 2000 ms vers 800179, +25 % de soins recus) existe mais n'est pose par rien ; le poser lui-meme
// propagerait ses trois APPLY_AREA_AURA_RAID en cascade. On le lit, on ne le pose pas.
// 800179 : e0 HEAL_PCT et e1 ENERGIZE_PCT mana sur la cible, e2 ENERGIZE_PCT Rage avec tA 0 -
// Spell.cpp:2033-2062 lui donne la cible explicite (SpellInfo.cpp:822) : rien a verser a la main.
class aura_ascension_primalist_sacred_grove : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_sacred_grove);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_SACRED_GROVE_DATA, SPELL_SACRED_GROVE_TICK});
    }

    void CalcPeriodic(AuraEffect const*, bool& isPeriodic, int32& amplitude)
    {
        isPeriodic = true;
        amplitude = sSpellMgr->AssertSpellInfo(SPELL_SACRED_GROVE_DATA)->Effects[EFFECT_0].Amplitude;
        if (amplitude <= 0)
            amplitude = 2000;
    }

    void Tick(AuraEffect const*)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (caster && Primalist(caster) && target->IsAlive() && caster->IsInMap(target))
            caster->CastSpell(target, SPELL_SACRED_GROVE_TICK, true);
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_primalist_sacred_grove::CalcPeriodic,
            EFFECT_1, SPELL_AURA_MOD_HEALING_PCT);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_primalist_sacred_grove::Tick,
            EFFECT_1, SPELL_AURA_MOD_HEALING_PCT);
    }
};

// 301307 Earthmother's Roar (« heals allies equal to $301307s1% of their missing health ») and
// 800179 Sacred Grove tick (800180 : « regain $800179s1% of their missing health, Rage, and
// mana ») : HEAL_PCT (136) and ENERGIZE_PCT (137) give a share of the MAXIMUM. Decision of the
// commissioner (2026-09-30) : the share is taken on what is MISSING, per target.
//  - HEAL_PCT is resolved at launch (EffectHealPct, SpellEffects.cpp) with its done/taken
//    bonuses ; OnHit runs before the heal is dealt (Spell::DoAllEffectOnTarget) and scales it by
//    missing / maximum, read at that moment. A full-health target gets 0 (no heal is dealt).
//  - ENERGIZE_PCT is resolved on hit : the default handler is prevented and the same checks
//    as EffectEnergizePct are made, on max - current.
// 800186's own aura text says « maximum » : it is never applied (see 800180 above), the
// player reads 800180 and 301306.
class spell_ascension_primalist_missing_pct : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_missing_pct);

    void ScaleHeal()
    {
        Unit* target = GetHitUnit();
        int32 heal = GetHitHeal();
        if (!target || heal <= 0)
            return;
        uint32 const maximum = target->GetMaxHealth();
        uint32 const current = target->GetHealth();
        if (!maximum || !target->IsAlive() || current >= maximum)
        {
            SetHitHeal(0);
            return;
        }
        SetHitHeal(int32(uint64(heal) * (maximum - current) / maximum));
    }

    void Energize(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !target->IsAlive())
            return;
        if (target->HasUnitState(UNIT_STATE_ISOLATED))
        {
            caster->SendSpellDamageImmune(target, GetSpellInfo()->Id);
            return;
        }
        int32 const misc = GetSpellInfo()->Effects[effIndex].MiscValue;
        if (misc < 0 || misc >= int8(MAX_POWERS))
            return;
        Powers const power = Powers(misc);
        if (target->IsPlayer() && !target->CanReceivePowerFromSpell(power) &&
            !GetSpellInfo()->HasAttribute(SPELL_ATTR7_ONLY_IN_SPELLBOOK_UNTIL_LEARNED))
            return;
        uint32 const maximum = target->GetMaxPower(power);
        uint32 const current = target->GetPower(power);
        int32 const pct = GetEffectValue();
        if (!maximum || current >= maximum || pct <= 0)
            return;
        if (uint32 gain = CalculatePct(maximum - current, pct))
            caster->EnergizeBySpell(target, GetSpellInfo()->Id, gain, power);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_primalist_missing_pct::ScaleHeal);
        // 301307 has no ENERGIZE_PCT : an unmatched effect hook only writes an error at start.
        if (SpellInfo const* script = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            if (script->HasEffect(SPELL_EFFECT_ENERGIZE_PCT))
                OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_missing_pct::Energize, EFFECT_ALL,
                    SPELL_EFFECT_ENERGIZE_PCT);
    }
};
}

void AddSC_AscensionPrimalistTalents()
{
    new primalist_talent_events();
    new primalist_talent_casts();
    RegisterSpellScript(spell_ascension_throat_clamp);
    RegisterSpellScript(aura_ascension_primalist_mineralization);
    RegisterSpellScript(aura_ascension_primalist_shattering);
    RegisterSpellScript(aura_ascension_primalist_infused_with_power);
    RegisterSpellScript(aura_ascension_primalist_totemic_echoes);
    RegisterSpellScript(aura_ascension_primalist_totemic_crash);
    RegisterSpellScript(aura_ascension_primalist_mountain_avatar);
    RegisterSpellScript(aura_ascension_primalist_earthen_avatar);
    RegisterSpellScript(aura_ascension_primalist_son_of_ursoc);
    RegisterSpellScript(spell_ascension_primalist_soothing_touch);
    RegisterSpellScript(aura_ascension_primalist_sacred_grove);
    RegisterSpellScript(spell_ascension_primalist_missing_pct);
}
