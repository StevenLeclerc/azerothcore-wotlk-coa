/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace
{
enum ChronomancerSecondarySpells : uint32
{
    SPELL_MELT_REALITY = 806335,
    SPELL_MELT_COPY_VALUE = 504727,
    SPELL_MELT_COPY = 807570,
    SPELL_DESYNCHRONIZATION = 561310,
    SPELL_RESYNCHRONIZATION = 561388,
    SPELL_AHEAD_OF_THE_GAME = 560948,
    SPELL_AHEAD_COUNTER = 592009,
    SPELL_AHEAD_POWER = 592010,
    SPELL_RIPPLE = 806296,
    SPELL_RIPPLE_CHARGE = 806297,
    SPELL_RIPPLE_HEAL = 806298,
    SPELL_ECHO_FRAGMENT = 804455,
    SPELL_ARC_COLLISION = 524853,
    SPELL_ECHO_DURATION = 807711,
    SPELL_AETHER_COMPRESSION = 804436,
    SPELL_FLUX_EMITTER = 804435,
    SPELL_SINGULARITY_CORE = 804438,
    SPELL_PARADOX_CANNON = 806203,
    SPELL_DISCOVERY_STACK = 704197,
    SPELL_DISCOVERY_STRIKE = 806201,
    SPELL_THREAD_OF_ETERNITY = 572127,
    SPELL_THREAD_PARADOX = 572166,
    SPELL_THREAD_FLUX = 572168,
    SPELL_THREAD_SINGULARITY = 572170,
    SPELL_THREAD_FLUX_SLS = 807628,
    SPELL_SHIFTING_CHAOS_AOE = 801269,
    SPELL_PULSE_DRIVER = 706121,
    SPELL_ECHO_FRAGMENTS = 92120
};

// Wand of Time, all nine rows (family 28, flags1 0x100000; only seven are chained).
constexpr uint32 WandOfTimeFlag1 = 0x100000;
// Shatter Echo, its six rows (family 28, flags2 0x10, exclusive in Spell.dbc).
constexpr uint32 ShatterEchoFlag2 = 0x10;
// Decomposition, its eight rows (family 28, flags1 0x80000, exclusive in Spell.dbc).
constexpr uint32 DecompositionFlag1 = 0x80000;

Player* SecondaryChronomancer(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_CHRONOMANCER && player->IsAlive() && player->IsInWorld()
        ? player : nullptr;
}

class chronomancer_melt_periodic : public UnitScript
{
public:
    chronomancer_melt_periodic() : UnitScript("chronomancer_melt_periodic", true,
        {UNITHOOK_ON_PERIODIC_DAMAGE_RESULT}) { }

    void OnPeriodicDamageResult(Unit* target, Unit*, uint32 damage, SpellInfo const*) override
    {
        if (!target || !target->IsInWorld() || !damage)
            return;
        // Allied periodic damage also contributes. Snapshot owners before a copy can remove another mark.
        std::vector<ObjectGuid> owners;
        for (auto const& pair : target->GetAppliedAuras())
        {
            Aura* aura = pair.second->GetBase();
            if (sSpellMgr->GetFirstSpellInChain(aura->GetId()) == SPELL_MELT_REALITY &&
                std::find(owners.begin(), owners.end(), aura->GetCasterGUID()) == owners.end())
                owners.push_back(aura->GetCasterGUID());
        }
        SpellInfo const* value = sSpellMgr->GetSpellInfo(SPELL_MELT_COPY_VALUE);
        if (!value)
            return;
        for (ObjectGuid const& guid : owners)
            if (Player* player = SecondaryChronomancer(ObjectAccessor::GetUnit(*target, guid));
                player && player->GetMap() == target->GetMap() && player->InSamePhase(target) &&
                player->IsValidAttackTarget(target))
            {
                uint64 amount = uint64(damage) * std::max(0, value->Effects[EFFECT_0].CalcValue(player)) / 100;
                if (amount)
                    player->CastCustomSpell(SPELL_MELT_COPY, SPELLVALUE_BASE_POINT0,
                        int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), target, true);
            }
    }
};

class spell_ascension_melt_copy : public SpellScript
{
    PrepareSpellScript(spell_ascension_melt_copy);

    void Select(std::list<WorldObject*>& targets)
    {
        Unit* original = GetExplTargetUnit();
        targets.remove_if([original](WorldObject* object)
        {
            Unit* target = object ? object->ToUnit() : nullptr;
            return !target || target == original || !target->IsAlive();
        });
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ascension_melt_copy::Select,
            EFFECT_0, TARGET_UNIT_DEST_AREA_ENEMY);
    }
};

class aura_ascension_desynchronization : public AuraScript
{
    PrepareAuraScript(aura_ascension_desynchronization);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_RESYNCHRONIZATION}); }

    void End(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = SecondaryChronomancer(GetCaster());
        Unit* target = GetTarget();
        if (player && target->IsAlive() && target->IsInWorld() && player->GetMap() == target->GetMap() &&
            player->InSamePhase(target) && player->IsValidAttackTarget(target) &&
            GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            player->CastSpell(target, SPELL_RESYNCHRONIZATION, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_desynchronization::End,
            EFFECT_0, SPELL_AURA_MOD_DISARM, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_ahead_of_the_game : public AuraScript
{
    PrepareAuraScript(aura_ascension_ahead_of_the_game);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_AHEAD_COUNTER, SPELL_AHEAD_POWER}); }

    bool Check(ProcEventInfo& event)
    {
        return SecondaryChronomancer(GetTarget()) && event.GetActor() == GetTarget() &&
            ((event.GetDamageInfo() && event.GetDamageInfo()->GetDamage()) ||
                (event.GetHealInfo() && event.GetHealInfo()->GetEffectiveHeal()));
    }

    void Count(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        Unit* player = GetTarget();
        player->CastSpell(player, SPELL_AHEAD_COUNTER, true);
        if (Aura const* counter = player->GetAura(SPELL_AHEAD_COUNTER, player->GetGUID());
            counter && counter->GetStackAmount() >= 10)
        {
            player->RemoveAurasDueToSpell(SPELL_AHEAD_COUNTER, player->GetGUID());
            player->CastSpell(player, SPELL_AHEAD_POWER, true);
        }
    }

    void Clear(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_AHEAD_COUNTER, GetTarget()->GetGUID());
        GetTarget()->RemoveAurasDueToSpell(SPELL_AHEAD_POWER, GetTarget()->GetGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_ahead_of_the_game::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_ahead_of_the_game::Count,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ahead_of_the_game::Clear,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_ripple_release : public AuraScript
{
    PrepareAuraScript(aura_ascension_ripple_release);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_RIPPLE_CHARGE, SPELL_RIPPLE_HEAL}); }

    void Start(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_RIPPLE_CHARGE, GetTarget()->GetGUID());
    }

    void End(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* player = GetTarget();
        AuraEffect const* timer = GetEffect(EFFECT_1);
        if (SecondaryChronomancer(player) && player == GetCaster() && timer && timer->GetTickNumber() >= 6 &&
            GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            // Six native 250 ms ticks meet the minimum. Their authored spell modifier scales this heal.
            player->CastSpell(player, SPELL_RIPPLE_HEAL, true);
        player->RemoveAurasDueToSpell(SPELL_RIPPLE_CHARGE, player->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_ripple_release::Start,
            EFFECT_0, SPELL_AURA_MECHANIC_IMMUNITY_MASK, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_ripple_release::End,
            EFFECT_0, SPELL_AURA_MECHANIC_IMMUNITY_MASK, AURA_EFFECT_HANDLE_REAL);
    }
};

class aura_ascension_echo_duration : public AuraScript
{
    PrepareAuraScript(aura_ascension_echo_duration);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_ECHO_DURATION}); }

    bool Load() override
    {
        return GetCaster() == GetUnitOwner() && GetCaster() && GetCaster()->IsPlayer() &&
            GetCaster()->getClass() == CLASS_CHRONOMANCER;
    }

    void Sync(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* player = GetTarget();
        Aura* companion = player->GetAura(SPELL_ECHO_DURATION, player->GetGUID());
        if (!companion)
            companion = player->AddAura(SPELL_ECHO_DURATION, player);
        if (companion && companion->GetStackAmount() != GetStackAmount())
            companion->SetStackAmount(GetStackAmount());
    }

    void Clear(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_ECHO_DURATION, GetTarget()->GetGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_echo_duration::Sync,
            EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_echo_duration::Clear,
            EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Discovery: "At 3 stacks, your next Wand of Time or Artificer's Wand causes an additional ...".
// The spell_proc row fires both effects on every hit; effect 0 (the stack) runs first
// (Aura::TriggerProcOnEvent), so the strike waits for the full stack count and spends it.
class aura_ascension_chronomancer_discovery : public AuraScript
{
    PrepareAuraScript(aura_ascension_chronomancer_discovery);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_DISCOVERY_STACK, SPELL_DISCOVERY_STRIKE});
    }

    void Strike(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Aura* discovery = caster->GetAura(SPELL_DISCOVERY_STACK, caster->GetGUID());
        if (!discovery || discovery->GetStackAmount() < std::max<uint32>(1, discovery->GetSpellInfo()->StackAmount))
            return;
        Unit* victim = event.GetActionTarget();
        if (!victim || victim == caster || !victim->IsAlive())
            return;
        // "$806201m1 + $spa*0.212 + $spi*0.25 + $RAP*0.1 Arcane Damage": the strike is a plain school
        // damage by contract (BonusMultiplier 0), so every term is counted once, here. CalcValue without a
        // caster: the target's own CalcValue adds the per-level part and the effect modifiers again.
        SpellInfo const* strike = sSpellMgr->GetSpellInfo(SPELL_DISCOVERY_STRIKE);
        double bonus = caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE) * 0.212 +
            caster->GetStat(STAT_SPIRIT) * 0.25 + caster->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.1;
        int64 amount = int64(strike->Effects[EFFECT_0].CalcValue(nullptr)) + int64(std::max(0.0, bonus));
        caster->CastCustomSpell(SPELL_DISCOVERY_STRIKE, SPELLVALUE_BASE_POINT0,
            int32(std::clamp<int64>(amount, 0, std::numeric_limits<int32>::max())), victim, true, nullptr, effect);
        discovery->Remove();
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_ascension_chronomancer_discovery::Strike,
            EFFECT_1, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Shifting Chaos: "Your Chromatic Shard and Anomaly Spikes now deal an additional $s1% their damage
// dealt as Chromatic Damage all nearby enemies." Aura 354 has no handler of its own.
class aura_ascension_shifting_chaos : public AuraScript
{
    PrepareAuraScript(aura_ascension_shifting_chaos);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_SHIFTING_CHAOS_AOE}); }

    void Echo(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = event.GetActionTarget();
        DamageInfo* damage = event.GetDamageInfo();
        if (!damage || !damage->GetDamage() || !victim || victim == caster || !victim->IsAlive() ||
            effect->GetAmount() <= 0)
            return;
        uint64 amount = uint64(damage->GetDamage()) * uint64(std::min(effect->GetAmount(), 100)) / 100;
        if (amount)
            caster->CastCustomSpell(SPELL_SHIFTING_CHAOS_AOE, SPELLVALUE_BASE_POINT0,
                int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), victim, true, nullptr, effect);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(aura_ascension_shifting_chaos::Echo, EFFECT_0, SPELL_AURA_ANY);
    }
};

// Singularity Core: "Empower your Wand attacks ...". Without a spell_proc row the core built one from
// the aura 108 effects (family 28, flags2 0x10): only Shatter Echo procced, the wand shot (5019,
// family 0) never did. The row (0x40 wand shot | 0x100 ranged spells, family 0) sends every ranged
// hit here. Pulse Driver: "allows Shatter Echo to trigger its damage" (its own 707559 stays unused).
class aura_ascension_singularity_core : public AuraScript
{
    PrepareAuraScript(aura_ascension_singularity_core);

    bool CheckProc(ProcEventInfo& event)
    {
        SpellInfo const* spell = event.GetSpellInfo();
        if (!spell || !(event.GetTypeMask() & PROC_FLAG_DONE_SPELL_RANGED_DMG_CLASS))
        {
            // The ranged auto shot (0x40): only a wand's, not a bow's or a gun's.
            Player* player = GetTarget()->ToPlayer();
            Item* ranged = player ? player->GetWeaponForAttack(RANGED_ATTACK, true) : nullptr;
            return ranged && ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
        }
        if (spell->SpellFamilyName != 28)
            return false;
        if (spell->SpellFamilyFlags[1] & WandOfTimeFlag1)
            return true;
        if (spell->SpellFamilyFlags[2] & ShatterEchoFlag2)
            return GetTarget()->HasAura(SPELL_PULSE_DRIVER);
        return false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_singularity_core::CheckProc);
    }
};

// Crystal Cannon (Specialization): "Casting Discordance now unleashes the Crystal Cannon, launching 2
// quick lesser Wand attacks at the enemy". The HIT phase row fires once per target of the chain; only
// the explicit target of the cast may launch the cannon.
class aura_ascension_crystal_cannon_spec : public AuraScript
{
    PrepareAuraScript(aura_ascension_crystal_cannon_spec);

    bool CheckProc(ProcEventInfo& event)
    {
        Spell const* spell = event.GetProcSpell();
        Unit* victim = event.GetActionTarget();
        return spell && victim && spell->m_targets.GetUnitTarget() == victim;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_crystal_cannon_spec::CheckProc);
    }
};

class chronomancer_secondary_casts : public AllSpellScript
{
public:
    chronomancer_secondary_casts() : AllSpellScript("chronomancer_secondary_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        // Every spell that requires Echo Fragments spends them all: Arc Collision, the six Shatter
        // Echo rows ("for each Echo Fragment consumed"), Flux Emitter, Singularity Core and Paradox
        // Cannon; plus Aether Compression, a Continuum that requires none but gains per fragment
        // ("Continuum spells per Echo Fragment consumed"); and Decomposition under Echo Fragments 92120
        // ("Damage increased by $804455s1% for each Echo Fragment consumed"), which requires none either.
        // Their bonus is already resolved: damage at launch (Spell.cpp:3993), self-aura duration and the
        // instant (speed 0) Decomposition DoT in handle_immediate (4079), all before this.
        if (SecondaryChronomancer(caster) && !spell->IsTriggered() && info->SpellFamilyName == 28 &&
            (info->CasterAuraSpell == SPELL_ECHO_FRAGMENT || info->Id == SPELL_AETHER_COMPRESSION ||
             ((info->SpellFamilyFlags[1] & DecompositionFlag1) && caster->HasAura(SPELL_ECHO_FRAGMENTS))))
            caster->RemoveAurasDueToSpell(SPELL_ECHO_FRAGMENT, caster->GetGUID());
    }

    // Thread of Eternity: "You may consume this debuff with Wand of Time to gain an additional effect
    // based on your active Continuum Spell." The designed carriers (807579, 807628, 807760) are
    // granted by nothing; their triggered spells are cast here, and each removes the mark itself.
    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool) override
    {
        Player* player = SecondaryChronomancer(spell->GetCaster());
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !target || target == player || miss != SPELL_MISS_NONE || spell->IsTriggered() ||
            info->SpellFamilyName != 28 || !(info->SpellFamilyFlags[1] & WandOfTimeFlag1) ||
            !target->IsAlive() || !target->HasAura(SPELL_THREAD_OF_ETERNITY, player->GetGUID()))
            return;
        ObjectGuid guid = player->GetGUID();
        if (player->HasAura(SPELL_SINGULARITY_CORE, guid))
            player->CastSpell(target, SPELL_THREAD_SINGULARITY, true); // 2 x 804437, then 804437 + removal
        else if (player->HasAura(SPELL_FLUX_EMITTER, guid))
        {
            SpellInfo const* carrier = sSpellMgr->GetSpellInfo(SPELL_THREAD_FLUX_SLS);
            int32 percent = carrier ? carrier->Effects[EFFECT_0].CalcValue(player) : 0;
            uint64 amount = uint64(damage) * uint64(std::clamp(percent, 0, 100)) / 100;
            if (amount)
                player->CastCustomSpell(SPELL_THREAD_FLUX, SPELLVALUE_BASE_POINT0,
                    int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), target, true);
        }
        else if (player->HasAura(SPELL_PARADOX_CANNON, guid))
            player->CastSpell(target, SPELL_THREAD_PARADOX, true);
    }
};

class chronomancer_secondary_metadata : public GlobalScript
{
public:
    chronomancer_secondary_metadata() : GlobalScript("chronomancer_secondary_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->Id == SPELL_MELT_COPY && info->SpellFamilyName == 28)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        if (info->SpellFamilyName == 28 && info->Id == SPELL_THREAD_OF_ETERNITY)
        {
            // "Damage taken from Wand of Time is increased by $s2%": effect 1 targets Discordance
            // (flags0 0x2000000), which applies the mark every tick and stacked it to +240%.
            SpellEffectInfo& bonus = info->Effects[EFFECT_1];
            if (bonus.ApplyAuraName == SPELL_AURA_MOD_DAMAGE_FROM_CASTER && bonus.SpellClassMask == flag96(0x2000000, 0, 0))
                bonus.SpellClassMask = flag96(0, WandOfTimeFlag1, 0);
            // The text announces a single +30%, no stacks: each application refreshes.
            info->StackAmount = 0;
        }
        if (info->SpellFamilyName == 28 && (info->Id == SPELL_THREAD_FLUX || info->Id == SPELL_SHIFTING_CHAOS_AOE))
        {
            // Both carry a share of a hit that already went through power, modifiers and the crit roll.
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        // Thread of Eternity with Paradox Cannon: "Your next Shatter Echo's damage is increased by $w1%".
        // Without charges the modifier stayed for the whole 6 s; one charge is spent by the first
        // Shatter Echo it applies to (RemoveSpellMods), as for Paradox and Converge the Infinite.
        if (info->Id == SPELL_THREAD_PARADOX && info->SpellFamilyName == 28 && !info->ProcCharges)
            info->ProcCharges = 1;
        // Discovery strike: effect 121 dealt main-hand weapon damage + 1, with no spell power, spirit or
        // ranged attack power. Made a plain arcane hit whose amount is computed in the Discovery script.
        if (info->Id == SPELL_DISCOVERY_STRIKE && info->SpellFamilyName == 28 &&
            info->Effects[EFFECT_0].Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG)
        {
            info->Effects[EFFECT_0].Effect = SPELL_EFFECT_SCHOOL_DAMAGE;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
            info->_InitializeExplicitTargetMask();
        }
        if (info->Id == SPELL_ECHO_DURATION || info->Id == SPELL_AHEAD_COUNTER || info->Id == SPELL_RIPPLE_CHARGE)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
    }
};
}

void AddSC_AscensionChronomancerSecondary()
{
    new chronomancer_melt_periodic();
    new chronomancer_secondary_casts();
    new chronomancer_secondary_metadata();
    RegisterSpellScript(spell_ascension_melt_copy);
    RegisterSpellScript(aura_ascension_desynchronization);
    RegisterSpellScript(aura_ascension_ahead_of_the_game);
    RegisterSpellScript(aura_ascension_ripple_release);
    RegisterSpellScript(aura_ascension_echo_duration);
    RegisterSpellScript(aura_ascension_chronomancer_discovery);
    RegisterSpellScript(aura_ascension_shifting_chaos);
    RegisterSpellScript(aura_ascension_singularity_core);
    RegisterSpellScript(aura_ascension_crystal_cannon_spec);
}
