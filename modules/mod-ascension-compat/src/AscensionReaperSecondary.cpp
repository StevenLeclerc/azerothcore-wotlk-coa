/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionReaperTalents.h"
#include "DynamicObject.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PetDefines.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace
{
enum ReaperSecondarySpells : uint32
{
    SPELL_GHOST = 8326,
    SPELL_SPIRIT_WALKER = 561082,
    SPELL_SPIRIT_WALKER_SPEED = 561093,
    SPELL_ENDBRINGER = 800922,
    SPELL_ENDBRINGER_AMOUNT = 801341,
    SPELL_ENDBRINGER_HEAL = 520419,
    SPELL_DIRGE = 801328,
    SPELL_DIRGE_HIT = 801311,
    SPELL_REAPED_SOUL = 500363,
    SPELL_SPECTRE_HIT = 803742,
    SPELL_SPECTRE_ROOT = 803942,
    SPELL_MURDER = 500376,
    SPELL_CRIMSON_THIRST = 807415,
    SPELL_CRIMSON_STACK = 807416,
    SPELL_CRIMSON_AMOUNT = 807417,
    SPELL_CRIMSON_HEAL = 807545,

    SPELL_ANIMA_AMBUSHER = 705424,
    SPELL_ANIMA_AMBUSH = 705425,
    SPELL_GHOSTLY_WEAPON = 803997,
    SPELL_GHOSTLY_WEAPON_DAMAGE = 804474,
    SPELL_LAMENTING = 705397,
    SPELL_LAMENTING_HEAL = 807420,
    SPELL_SLAUGHTER = 500373,
    SPELL_SOULS_FOR_THE_SLAUGHTER = 705428,
    SPELL_SOULS_FOR_THE_SLAUGHTER_AURA = 504304,
    SPELL_SOULS_FOR_THE_SLAUGHTER_DAMAGE = 575847,
    SPELL_CRIMSON_DEATH = 705414,
    SPELL_SOULSLAM = 504014,
    SPELL_SOUL_PIERCER = 712484,
    SPELL_SOUL_PIERCER_REFUND = 575614,
    SPELL_SPECTRAL_SCYTHE = 500484,
    SPELL_SPECTRAL_SCYTHE_CULLING = 500576,
    SPELL_SIPHON_ANIMA = 504309,
    SPELL_SIPHON_ANIMA_AURA = 572768,
    SPELL_SIPHON_ANIMA_HEAL = 504306,
    SPELL_SOUL_INFUSION = 803031,
    SPELL_ESSENCE_INVIGORATION = 805186,
    SPELL_ESSENCE_INVIGORATION_RESTORE = 805187,
    SPELL_SPECTRE_STRIDE = 801624,
    SPELL_SCYTHE_RUSH = 500359,
    SPELL_VEILWALK = 803990,
    SPELL_GHASTLY_FORM = 704357,
    SPELL_GHASTLY_FORM_SHIELD = 704358,
    SPELL_HARVEST_TIME = 803995,
    SPELL_BLOOD_FRENZY_TALENT = 707899,
    SPELL_BLOOD_FRENZY = 803039,
    SPELL_TORMENTED_SOULS_BUFF = 500481,
    SPELL_HARD_BARGAIN = 300569,
    SPELL_HARD_BARGAIN_BUFF = 572300,
    SPELL_LIFE_TAP = 706788,
    SPELL_REDSHADE_THRESH = 525058,
    SPELL_DECIMATION_COUNT = 573289,
    SPELL_DECIMATION_WINDOW = 704215,
    SPELL_DECIMATE = 500523,
    SPELL_SPECTRAL_WARDEN = 805716,
    SPELL_WRAITHBLADE = 805258,
    SPELL_THE_END_IS_NEAR = 680996,
    SPELL_THE_END_IS_NEAR_SUMMON = 707100,
    SPELL_GRAVESITE_TALENT = 572213,
    SPELL_GRAVESITE = 804722,
    SPELL_GRAVESITE_DAMAGE = 300979,
    SPELL_CLONE_ME = 45204
};

// Spectral Warden 805716 summons this entry (effect 28, misc 100481, SummonProperties 61) and
// The End Is Near 707100 summons 850000 (misc 850000, SummonProperties 61). Neither row existed
// in acore_world.creature_template before 2026_09_30_00_ascension_reaper_creatures.sql, which
// creates both with ScriptName npc_ascension_reaper_summon (below).
// SummonProperties 61 = Category 1 (ALLY), Type 2 (GUARDIAN), Slot 0, Flags 2: Map::SummonCreature
// builds a Guardian, Minion::InitStats -> Unit::SetMinion sets its OwnerGUID to the Reaper, so
// GetCharmerOrOwnerPlayerOrPlayerItself() returns the Reaper (Life Tap below relies on it).
constexpr uint32 NPC_SPECTRAL_WARDEN = 100481;
constexpr uint32 NPC_THE_END_IS_NEAR = 850000;
// Distance past which a summon leaves its master and despawns (same leash as the Xoroth summons).
constexpr float REAPER_SUMMON_LEASH = 100.0f;

// Spell::m_scriptEventMask bits used by this file, on Slaughter and Soulslam only. 16-21 and
// 25-31 are taken elsewhere in the module (Soul Strike uses 18); 0-4 are ResourceEvent.
constexpr uint8 SOULS_FOR_THE_SLAUGHTER_EVENT = 22;
constexpr uint8 SOUL_PIERCER_EVENT = 23;
constexpr uint8 CRIMSON_DEATH_EVENT = 24;

// Harvest Time: "Casting Harvest Time with an enemy targeted within 20 yds of you". The
// 20 yards is the tooltip's own literal: no field of 707899 or 803039 carries it.
constexpr float BLOOD_FRENZY_RANGE = 20.0f;

int32 ClampToInt32(uint64 value)
{
    return int32(std::min<uint64>(value, std::numeric_limits<int32>::max()));
}

// A damage copy helper (575847, 804474, 705425) and a heal helper (504306) all carry
// SPELL_ATTR3_IGNORE_CASTER_MODIFIERS and SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS (804474
// gets the first one below, in reaper_secondary_metadata): the value copied is already the
// resolved damage, it must not go through the caster's bonuses a second time.
bool IsOwnDamageEvent(Unit* owner, ProcEventInfo& event, bool victimAlive)
{
    Player* player = owner ? owner->ToPlayer() : nullptr;
    Unit* victim = event.GetActionTarget();
    DamageInfo* damage = event.GetDamageInfo();
    return player && player->getClass() == CLASS_REAPER && player->IsAlive() && player->IsInWorld() &&
        event.GetActor() == player && victim && victim != player && (!victimAlive || victim->IsAlive()) &&
        !player->IsFriendlyTo(victim) && damage && damage->GetDamage();
}

void HealFromDamage(Player* player, uint32 reference, uint32 helper, uint32 damage)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(reference);
    if (!info || !damage)
        return;
    uint64 amount = uint64(damage) * std::clamp(info->Effects[EFFECT_0].CalcValue(player), 0, 100) / 100;
    if (amount)
        player->CastCustomSpell(helper, SPELLVALUE_BASE_POINT0,
            int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), player, TRIGGERED_FULL_MASK);
}

class reaper_ghost_speed : public UnitScript
{
public:
    reaper_ghost_speed() : UnitScript("reaper_ghost_speed", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !aura ||
            aura->GetCasterGUID() != player->GetGUID() ||
            (aura->GetId() != SPELL_GHOST && aura->GetId() != SPELL_SPIRIT_WALKER))
            return;
        if (player->HasAura(SPELL_GHOST, player->GetGUID()) &&
            player->HasAura(SPELL_SPIRIT_WALKER, player->GetGUID()) &&
            !player->HasAura(SPELL_SPIRIT_WALKER_SPEED, player->GetGUID()))
            player->CastSpell(player, SPELL_SPIRIT_WALKER_SPEED, true);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !application)
            return;
        Aura* aura = application->GetBase();
        if (aura->GetCasterGUID() == player->GetGUID() &&
            (aura->GetId() == SPELL_GHOST || aura->GetId() == SPELL_SPIRIT_WALKER))
            player->RemoveAurasDueToSpell(SPELL_SPIRIT_WALKER_SPEED, player->GetGUID());
    }
};

class reaper_secondary_hits : public AllSpellScript
{
public:
    reaper_secondary_hits() : AllSpellScript("reaper_secondary_hits",
        {ALLSPELLHOOK_ON_BEFORE_EFFECTS, ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        if (caster->IsPlayer() && caster->getClass() == CLASS_REAPER && !spell->IsTriggered() &&
            sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_MURDER)
            if (Aura const* stacks = caster->GetAura(SPELL_CRIMSON_STACK, caster->GetGUID()))
                spell->SetScriptValue(SPELL_CRIMSON_STACK, stacks->GetStackAmount());
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        if (caster->IsPlayer() && caster->getClass() == CLASS_REAPER && !spell->IsTriggered() &&
            sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_MURDER &&
            spell->GetScriptValue(SPELL_CRIMSON_STACK))
            // Damage and cost have been calculated. A projectile retains its selected stack count.
            caster->RemoveAurasDueToSpell(SPELL_CRIMSON_STACK, caster->GetGUID());

        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !player->IsAlive() || !player->IsInWorld())
            return;

        // Siphon Anima 504309: "When you summon a Spectral Scythe, you now heal for $572768s1% of
        // your damage dealt for $572768d." Both summons count, and 500576 (Spirit Culling) is
        // always triggered, so this branch does not test IsTriggered. 572768 is an aura 354
        // (no core handler): aura_ascension_reaper_siphon_anima below is its engine. No
        // spell_proc row on 504309.
        if ((info->Id == SPELL_SPECTRAL_SCYTHE || info->Id == SPELL_SPECTRAL_SCYTHE_CULLING) &&
            player->HasAura(SPELL_SIPHON_ANIMA) && sSpellMgr->GetSpellInfo(SPELL_SIPHON_ANIMA_AURA))
            player->CastSpell(player, SPELL_SIPHON_ANIMA_AURA, true);

        if (spell->IsTriggered())
            return;

        // The End Is Near 680996: "Wraithblade now creates a spectral version of yourself for
        // $707100d that casts Dirge and Murder on enemies." 680996 e0 is an aura 42 toward
        // 707100 with ProcFlags 0 and no spell_proc row: the core never fires it, this is the
        // engine. Every Wraithblade rank (805258 chain), untriggered casts only: one clone per cast.
        if (sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_WRAITHBLADE && player->HasAura(SPELL_THE_END_IS_NEAR) &&
            sSpellMgr->GetSpellInfo(SPELL_THE_END_IS_NEAR_SUMMON))
            player->CastSpell(player, SPELL_THE_END_IS_NEAR_SUMMON, true);

        // Gravesite 572213: "Casting Endbringer now marks the ground beneath you as your Gravesite
        // for $804722d." 804722 e1 is effect 27 (persistent area aura): a DynamicObject at the
        // caster. One Gravesite per Reaper: an older one is removed first.
        if (info->Id == SPELL_ENDBRINGER && player->HasAura(SPELL_GRAVESITE_TALENT) &&
            sSpellMgr->GetSpellInfo(SPELL_GRAVESITE))
        {
            player->RemoveDynObject(SPELL_GRAVESITE); // removes every one of them
            player->CastSpell(player, SPELL_GRAVESITE, true);
        }

        // Decimation 704193: "transforms your NEXT Reap into Decimate". Decimate 500523 carries no
        // remover of the 704215 window (e0 121, e1 31), so one cast ends it here. Deferred: the
        // removal drops the temporary Decimate spell (SyncReapTransform) and this spell is Decimate.
        if (info->Id == SPELL_DECIMATE && player->HasAura(SPELL_DECIMATION_WINDOW, player->GetGUID()))
            player->m_Events.AddEventAtOffset([player]()
            {
                if (player->IsInWorld())
                    player->RemoveAurasDueToSpell(SPELL_DECIMATION_WINDOW, player->GetGUID());
            }, 1ms);

        // Ghastly Form 704357: "After using Spectre Stride, Scythe Rush, or Veilwalk you now gain
        // a shield that absorbs ... for $704358d." 704358 carries amount and duration.
        if ((sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_SPECTRE_STRIDE || info->Id == SPELL_SCYTHE_RUSH ||
                info->Id == SPELL_VEILWALK) &&
            player->HasAura(SPELL_GHASTLY_FORM) && sSpellMgr->GetSpellInfo(SPELL_GHASTLY_FORM_SHIELD))
            player->CastSpell(player, SPELL_GHASTLY_FORM_SHIELD, true);

        // Blood Frenzy 707899: "Casting Harvest Time with an enemy targeted within 20 yds of you
        // now unleashes a Blood Frenzy" (803039, a DoT on the target). Harvest Time is a self
        // buff, so the enemy is the player's selection.
        if (info->Id == SPELL_HARVEST_TIME && player->HasAura(SPELL_BLOOD_FRENZY_TALENT) &&
            sSpellMgr->GetSpellInfo(SPELL_BLOOD_FRENZY))
            if (Unit* target = player->GetSelectedUnit())
                if (target != player && target->IsAlive() && player->IsValidAttackTarget(target) &&
                    player->IsWithinDistInMap(target, BLOOD_FRENZY_RANGE) && player->IsWithinLOSInMap(target))
                    player->CastSpell(target, SPELL_BLOOD_FRENZY, true);

        // Essence Invigoration 805186: "Casting abilities that require Soul Infusion now has a
        // $h% chance to restore $805187s1% of your missing health." 805187 is effect 136
        // (HEAL_PCT), which the core computes on MAXIMUM health (SpellEffects.cpp
        // EffectHealPct); its MiscValueB 1 is Ascension's "missing health" flag, which nothing
        // reads. The heal is therefore computed here on missing health, like Soul Strike's.
        if (info->CasterAuraSpell == SPELL_SOUL_INFUSION && player->HasAura(SPELL_ESSENCE_INVIGORATION))
            if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_ESSENCE_INVIGORATION))
                if (SpellInfo const* restore = sSpellMgr->GetSpellInfo(SPELL_ESSENCE_INVIGORATION_RESTORE))
                {
                    int32 chance = int32(std::min<uint32>(talent->ProcChance, 100));
                    player->ApplySpellMod(SPELL_ESSENCE_INVIGORATION, SPELLMOD_CHANCE_OF_SUCCESS, chance);
                    uint32 missing = player->GetMaxHealth() - std::min(player->GetHealth(), player->GetMaxHealth());
                    uint64 amount = uint64(missing) *
                        uint32(std::clamp(restore->Effects[EFFECT_0].CalcValue(player), 0, 100)) / 100;
                    if (amount && roll_chance_i(chance))
                    {
                        HealInfo heal(player, player, uint32(std::min<uint64>(amount, missing)), restore,
                            restore->GetSchoolMask());
                        player->HealBySpell(heal);
                    }
                }
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool critical) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        if (!player || player->getClass() != CLASS_REAPER || !player->IsAlive() || !player->IsInWorld() ||
            miss != SPELL_MISS_NONE || !target || target == player || player->IsFriendlyTo(target))
            return;
        uint32 id = spell->GetSpellInfo()->Id;
        if (id == SPELL_DIRGE_HIT && player->HasAura(SPELL_ENDBRINGER, player->GetGUID()))
            HealFromDamage(player, SPELL_ENDBRINGER_AMOUNT, SPELL_ENDBRINGER_HEAL, damage);
        if (id == SPELL_SPECTRE_HIT && target->IsAlive())
            player->CastSpell(target, SPELL_SPECTRE_ROOT, true);
        // Anima Ambusher 705424: "Spectre Stride now deals an additional
        // ${($705425d*1000/$705425T1)*($m1)}% of its damage dealt over $705425d." 705424 e0 is an
        // aura 354 (no core handler) toward 705425, a 5 s DoT ticking every second with fixed
        // damage. One tick is $m1% of the hit, five ticks make the tooltip's total. 803742 is the
        // real hit of every Spectre Stride rank (effect 148), and it is triggered, hence before
        // the IsTriggered return below. No spell_proc row on 705424 (second engine).
        if (id == SPELL_SPECTRE_HIT && damage && target->IsAlive())
            if (AuraEffect const* ambusher = player->GetAuraEffect(SPELL_ANIMA_AMBUSHER, EFFECT_0))
                if (sSpellMgr->GetSpellInfo(SPELL_ANIMA_AMBUSH))
                    if (uint64 tick = uint64(damage) * uint32(std::clamp(ambusher->GetAmount(), 0, 1000)) / 100)
                        player->CastCustomSpell(SPELL_ANIMA_AMBUSH, SPELLVALUE_BASE_POINT0, ClampToInt32(tick), target,
                            TRIGGERED_FULL_MASK);
        // Lamenting 705397: "Damage dealt by Ghostly Weapon now heals for $s1% of the damage
        // dealt." 804474 is cast triggered by aura_ascension_reaper_ghostly_weapon; the damage
        // passed here is the amount actually dealt. 807420 is the copy helper.
        if (id == SPELL_GHOSTLY_WEAPON_DAMAGE && player->HasAura(SPELL_LAMENTING))
            HealFromDamage(player, SPELL_LAMENTING, SPELL_LAMENTING_HEAL, damage);
        if (spell->IsTriggered())
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(id);
        if (root == SPELL_SLAUGHTER)
        {
            // Souls for the Slaughter 705428: "Critical strikes with Slaughter now cause your
            // auto attacks to deal an additional $504304s1% Shadow damage for $504304d." Once
            // per cast. 504304 (aura 354) is run by aura_ascension_reaper_souls_for_the_slaughter.
            if (critical && player->HasAura(SPELL_SOULS_FOR_THE_SLAUGHTER) &&
                sSpellMgr->GetSpellInfo(SPELL_SOULS_FOR_THE_SLAUGHTER_AURA) &&
                spell->TryMarkScriptEventHandled(SOULS_FOR_THE_SLAUGHTER_EVENT))
                player->CastSpell(player, SPELL_SOULS_FOR_THE_SLAUGHTER_AURA, true);
            // Crimson Death 705414: "Your Slaughter now has a $h% chance to cast an additional
            // time free of cost." The copy is the same rank, triggered (no cost, no cooldown),
            // and a triggered Slaughter returns above, so it cannot chain.
            if (target->IsAlive() && player->HasAura(SPELL_CRIMSON_DEATH) &&
                spell->TryMarkScriptEventHandled(CRIMSON_DEATH_EVENT))
                if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_CRIMSON_DEATH))
                {
                    int32 chance = int32(std::min<uint32>(talent->ProcChance, 100));
                    player->ApplySpellMod(SPELL_CRIMSON_DEATH, SPELLMOD_CHANCE_OF_SUCCESS, chance);
                    if (roll_chance_i(chance))
                        player->CastSpell(target, id, TRIGGERED_FULL_MASK);
                }
        }
        // Soul Piercer 712484: "Striking at least 1 enemy with Soulslam now refunds its Reaped
        // Souls." Soulslam needs Soul Infusion and 561290 takes the three souls; 575614 gives
        // them back (500363 once, then 524732 twice), delayed by its effects 183. Once per cast.
        if (id == SPELL_SOULSLAM && player->HasAura(SPELL_SOUL_PIERCER) &&
            sSpellMgr->GetSpellInfo(SPELL_SOUL_PIERCER_REFUND) && spell->TryMarkScriptEventHandled(SOUL_PIERCER_EVENT))
            player->CastSpell(player, SPELL_SOUL_PIERCER_REFUND, true);
        if (root == SPELL_DIRGE && player->HasAura(SPELL_ENDBRINGER, player->GetGUID()) &&
            !spell->GetScriptValue(SPELL_REAPED_SOUL))
        {
            spell->SetScriptValue(SPELL_REAPED_SOUL, 1);
            for (uint8 i = 0; i < 3; ++i)
                HandleAscensionReaperResource(player, SPELL_REAPED_SOUL, 1);
        }
        if (root == SPELL_MURDER && spell->GetScriptValue(SPELL_CRIMSON_STACK) >= 5)
            HealFromDamage(player, SPELL_CRIMSON_AMOUNT, SPELL_CRIMSON_HEAL, damage);
    }
};

class aura_ascension_crimson_thirst : public AuraScript
{
    PrepareAuraScript(aura_ascension_crimson_thirst);

    bool Check(ProcEventInfo& event)
    {
        Unit* player = GetTarget();
        Unit* victim = event.GetActionTarget();
        return player->IsPlayer() && player->getClass() == CLASS_REAPER && player->IsAlive() &&
            GetCaster() == player && event.GetActor() == player && victim && victim != player &&
            !player->IsFriendlyTo(victim) && event.GetDamageInfo() && event.GetDamageInfo()->GetDamage() &&
            (event.GetDamageInfo()->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL);
    }

    void Proc(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        GetTarget()->CastSpell(GetTarget(), SPELL_CRIMSON_STACK, true);
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_CRIMSON_STACK, GetTarget()->GetGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_crimson_thirst::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_crimson_thirst::Proc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_crimson_thirst::Remove,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Ghostly Weapon 803997 (Soul, 15 s active): "causing melee attacks to deal ${$m1+0}%
// additional damage as Frost damage." e0 is an aura 354 (no core handler) toward 804474, and
// 803997 has ProcFlags 0: spell_proc 803997 (auto attacks and melee techniques, effects 1-2
// out of the proc) is the window, this script the engine. The amount already carries Essence
// Harvest 707908 (+10, SPELLMOD_EFFECT1 on 803997), exactly as Pilfering does for Dirty Blades
// (AscensionRangerTalents.cpp). Lamenting's heal reads 804474 in reaper_secondary_hits.
class aura_ascension_reaper_ghostly_weapon : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_ghostly_weapon);

    bool CheckProc(ProcEventInfo& event)
    {
        SpellInfo const* source = event.GetSpellInfo();
        return IsOwnDamageEvent(GetTarget(), event, true) &&
            (!source || (source->Id != SPELL_GHOSTLY_WEAPON_DAMAGE && source->Id != SPELL_SOULS_FOR_THE_SLAUGHTER_DAMAGE));
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 1000)) / 100;
        if (amount && sSpellMgr->GetSpellInfo(SPELL_GHOSTLY_WEAPON_DAMAGE))
            GetTarget()->CastCustomSpell(SPELL_GHOSTLY_WEAPON_DAMAGE, SPELLVALUE_BASE_POINT0, ClampToInt32(amount),
                event.GetActionTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_ghostly_weapon::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_reaper_ghostly_weapon::Proc, EFFECT_0, AuraType(354));
    }
};

// Souls for the Slaughter 504304 (5 s, posed by reaper_secondary_hits on a Slaughter crit):
// "your auto attacks deal an additional $504304s1% Shadow damage". e0 aura 354 toward 575847,
// a copy helper. spell_proc 504304: auto attacks only.
class aura_ascension_reaper_souls_for_the_slaughter : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_souls_for_the_slaughter);

    bool CheckProc(ProcEventInfo& event)
    {
        return IsOwnDamageEvent(GetTarget(), event, true) && !event.GetSpellInfo() &&
            (event.GetTypeMask() & PROC_FLAG_DONE_MELEE_AUTO_ATTACK);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 1000)) / 100;
        if (amount && sSpellMgr->GetSpellInfo(SPELL_SOULS_FOR_THE_SLAUGHTER_DAMAGE))
            GetTarget()->CastCustomSpell(SPELL_SOULS_FOR_THE_SLAUGHTER_DAMAGE, SPELLVALUE_BASE_POINT0,
                ClampToInt32(amount), event.GetActionTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_souls_for_the_slaughter::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_reaper_souls_for_the_slaughter::Proc, EFFECT_0, AuraType(354));
    }
};

// Siphon Anima 572768 (10 s, posed by reaper_secondary_hits on a Spectral Scythe summon):
// "you now heal for $572768s1% of your damage dealt". e0 aura 354 toward 504306, a heal copy
// helper. spell_proc 572768: direct damage done (auto attacks, melee techniques, magic). The
// killing blow still heals, hence no "alive" requirement on the victim.
class aura_ascension_reaper_siphon_anima : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_siphon_anima);

    bool CheckProc(ProcEventInfo& event)
    {
        return IsOwnDamageEvent(GetTarget(), event, false);
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint32(std::clamp(effect->GetAmount(), 0, 100)) / 100;
        if (amount && sSpellMgr->GetSpellInfo(SPELL_SIPHON_ANIMA_HEAL))
            GetTarget()->CastCustomSpell(SPELL_SIPHON_ANIMA_HEAL, SPELLVALUE_BASE_POINT0, ClampToInt32(amount),
                GetTarget(), true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_siphon_anima::CheckProc);
        OnEffectProc += AuraEffectProcFn(aura_ascension_reaper_siphon_anima::Proc, EFFECT_0, AuraType(354));
    }
};

// Hard Bargain 300569: "While Tormented Souls is active on you, your melee attack speed is
// increased by $572300s1% and your Dreadwake costs $/10;572300s2 less Runic Power." 572300
// has DurationIndex 2 (300000010 ms, i.e. unbounded), so it lives exactly as long as 500481:
// posed with it (new application or refresh, Unit.cpp OnAuraApply) and taken away with it.
// Not saved (metadata below): a relog rebuilds it on the next Tormented Souls.
//
// Life Tap 706788 e0 (aura 4, amount 200): Spectral Warden "now heals you equal to $s1% of the
// damage it deals". Not through HealFromDamage, which caps at 100%. The Warden is a
// Guardian owned by the Reaper: see NPC_SPECTRAL_WARDEN.
class reaper_tormented_souls_events : public UnitScript
{
public:
    reaper_tormented_souls_events() : UnitScript("reaper_tormented_souls_events", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_DAMAGE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !aura || aura->GetId() != SPELL_TORMENTED_SOULS_BUFF ||
            aura->GetCasterGUID() != player->GetGUID() || !player->IsInWorld() || !player->IsAlive() ||
            !player->HasAura(SPELL_HARD_BARGAIN) || player->HasAura(SPELL_HARD_BARGAIN_BUFF, player->GetGUID()) ||
            !sSpellMgr->GetSpellInfo(SPELL_HARD_BARGAIN_BUFF))
            return;
        player->CastSpell(player, SPELL_HARD_BARGAIN_BUFF, true);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !application)
            return;
        Aura* aura = application->GetBase();
        if (aura && aura->GetId() == SPELL_TORMENTED_SOULS_BUFF && aura->GetCasterGUID() == player->GetGUID())
            player->RemoveAurasDueToSpell(SPELL_HARD_BARGAIN_BUFF, player->GetGUID());
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !damage || !attacker->IsCreature() || attacker->GetEntry() != NPC_SPECTRAL_WARDEN ||
            !victim || victim == attacker)
            return;
        Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!owner || owner->getClass() != CLASS_REAPER || !owner->IsAlive() || !owner->IsInWorld() || owner == victim)
            return;
        AuraEffect const* lifeTap = owner->GetAuraEffect(SPELL_LIFE_TAP, EFFECT_0);
        SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_LIFE_TAP);
        if (!lifeTap || !info)
            return;
        uint64 amount = uint64(damage) * uint32(std::clamp(lifeTap->GetAmount(), 0, 1000)) / 100;
        if (!amount)
            return;
        HealInfo heal(owner, owner, uint32(ClampToInt32(amount)), info, SPELL_SCHOOL_MASK_SHADOW);
        owner->HealBySpell(heal);
    }
};

// Gravesite 572213: "Direct critical strikes made against enemies within a Gravesite now deals
// 300979 Shadow damage." 572213 e0 is an aura 42 (PROC_TRIGGER_SPELL) toward 300979 with
// ProcFlags 0: spell_proc 572213 (2026_09_30_00_ascension_reaper_creatures.sql) opens the window
// on direct hits only (auto attacks, melee, none and magic damage classes; no periodic flag),
// HitMask critical, and TRIGGERED_CAN_PROC because Dirge's real hits (801311) are triggered. This
// check keeps the enemies in the Reaper's own Gravesite (its DynamicObject 804722, radius read
// from the object, i.e. from the DBC) or, per the tooltip's "still active while in another
// Reaper's Gravesite", carrying 804722's area aura from any caster. The default proc action
// then casts 300979 on the victim: one cast per hit event. 300979 cannot proc it again
// (Aura::GetProcEffectMask refuses a spell triggered by the aura itself); the damage copies
// below are excluded so that one swing does not pay twice.
bool InGravesite(Unit* reaper, Unit* victim)
{
    if (DynamicObject* site = reaper->GetDynObject(SPELL_GRAVESITE))
        if (site->IsInMap(victim) && site->IsWithinDistInMap(victim, site->GetRadius(), true, false, true))
            return true;
    return victim->HasAura(SPELL_GRAVESITE);
}

class aura_ascension_reaper_gravesite : public AuraScript
{
    PrepareAuraScript(aura_ascension_reaper_gravesite);

    bool CheckProc(ProcEventInfo& event)
    {
        if (!IsOwnDamageEvent(GetTarget(), event, true) || !(event.GetHitMask() & PROC_HIT_CRITICAL))
            return false;
        if (SpellInfo const* source = event.GetSpellInfo())
            if (source->Id == SPELL_GRAVESITE_DAMAGE || source->Id == SPELL_GHOSTLY_WEAPON_DAMAGE ||
                source->Id == SPELL_SOULS_FOR_THE_SLAUGHTER_DAMAGE || source->Id == SPELL_ANIMA_AMBUSH)
                return false;
        return InGravesite(GetTarget(), event.GetActionTarget());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_reaper_gravesite::CheckProc);
    }
};

// Highest rank of `first` the master knows (spell_ranks chain), 0 when none.
uint32 HighestKnownRank(Player const* player, uint32 first)
{
    for (uint32 id = sSpellMgr->GetLastSpellInChain(first); id; id = sSpellMgr->GetPrevSpellInChain(id))
        if (player->HasSpell(id))
            return id;
    return 0;
}

// Health and damage follow the master, rescaled every 2 s. No tooltip gives a number: the
// Warden gets half of the master's health and weapon damage, the clone ("a spectral version of
// yourself") half of the health and all of the weapon damage, both hands (Dirge strikes with
// both). Values chosen here, not read anywhere.
void ScaleReaperSummon(Creature* unit, Player* player)
{
    bool clone = unit->GetEntry() == NPC_THE_END_IS_NEAR;
    float share = clone ? 1.0f : 0.5f;
    uint32 maximum = std::max(1u, player->GetMaxHealth() / 2);
    float percent = unit->GetMaxHealth() ? unit->GetHealthPct() / 100.0f : 1.0f;
    unit->SetLevel(player->GetLevel());
    unit->SetMaxHealth(maximum);
    unit->SetHealth(std::max(1u, uint32(maximum * percent)));
    unit->SetArmor(player->GetArmor());
    unit->SetAttackTime(BASE_ATTACK, player->GetAttackTime(BASE_ATTACK));
    unit->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, player->GetFloatValue(UNIT_FIELD_MINDAMAGE) * share);
    unit->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, player->GetFloatValue(UNIT_FIELD_MAXDAMAGE) * share);
    unit->UpdateDamagePhysical(BASE_ATTACK);
    if (clone)
    {
        // Guardian::UpdateDamagePhysical ignores the off hand: the fields are written here.
        float offMin = player->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE) * share;
        float offMax = player->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE) * share;
        unit->SetAttackTime(OFF_ATTACK, player->GetAttackTime(OFF_ATTACK));
        unit->SetBaseWeaponDamage(OFF_ATTACK, MINDAMAGE, offMin);
        unit->SetBaseWeaponDamage(OFF_ATTACK, MAXDAMAGE, offMax);
        unit->SetStatFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE, offMin);
        unit->SetStatFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE, offMax);
    }
}

// Spectral Warden 100481 (805716): "a spectral guardian to fight along side you ... Threat
// generated by the Spectral Warden is redirected to the Reaper." Melee on the master's target,
// threat redirected to the master (ThreatMgr redirect, as the Hellfire Imps of
// AscensionXorothSummons.cpp). Life Tap's heal is in reaper_tormented_souls_events::OnDamage.
//
// The End Is Near 850000 (707100): "a spectral version of yourself ... that casts Dirge and
// Murder on enemies." Appearance by the native clone, as Mirror Image does (pet_mage.cpp):
// the master casts Clone Me! 45204 on it (SPELL_AURA_CLONE_CASTER, then copies of both weapons
// by spell_gen_clone). It casts the master's highest known rank of Dirge 801328 and Murder
// 500376, triggered for power and cooldown (a creature has no Runic Power), one cast per
// global cooldown of the spell (StartRecoveryTime, DBC). It does not melee.
//
// Both despawn as soon as the master is dead, gone, out of the map or beyond 100 yards; a summon
// that was not summoned by a Reaper (".npc add") has no master and despawns at its first update.
struct npc_ascension_reaper_summon : public ScriptedAI
{
    explicit npc_ascension_reaper_summon(Creature* creature) : ScriptedAI(creature) { }

    ObjectGuid _master;
    uint32 _dirge = 0;
    uint32 _murder = 0;
    uint32 _scaleTimer = 2000;
    uint32 _castTimer = 0;
    bool _dirgeNext = false;

    bool IsClone() const { return me->GetEntry() == NPC_THE_END_IS_NEAR; }

    Player* Master() const
    {
        Player* player = _master ? ObjectAccessor::GetPlayer(*me, _master) : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !player->IsAlive() || !player->IsInWorld() ||
            !me->IsWithinDistInMap(player, REAPER_SUMMON_LEASH))
            return nullptr;
        return player;
    }

    void IsSummonedBy(WorldObject* summoner) override
    {
        Player* player = summoner ? summoner->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_REAPER || !player->IsAlive() || me->GetOwnerGUID() != player->GetGUID())
        {
            me->DespawnOrUnsummon(1ms);
            return;
        }
        _master = player->GetGUID();

        // One of each per master: an older copy (cooldown reduced below the duration) goes away.
        std::vector<Creature*> older;
        for (Unit* controlled : player->m_Controlled)
            if (controlled != me && controlled->GetEntry() == me->GetEntry() && controlled->IsCreature())
                older.push_back(controlled->ToCreature());
        for (Creature* old : older)
            old->DespawnOrUnsummon(1ms);

        me->SetFaction(player->GetFaction());
        ScaleReaperSummon(me, player);
        me->SetFullHealth();
        me->SetReactState(REACT_PASSIVE); // targets are the master's, chosen in UpdateAI
        if (IsClone())
        {
            _dirge = HighestKnownRank(player, SPELL_DIRGE);
            _murder = HighestKnownRank(player, SPELL_MURDER);
            _dirgeNext = !_murder;
            me->SetCanDualWield(true);
            if (sSpellMgr->GetSpellInfo(SPELL_CLONE_ME))
                player->CastSpell(me, SPELL_CLONE_ME, true);
        }
        else
            me->GetThreatMgr().RegisterRedirectThreat(SPELL_SPECTRAL_WARDEN, _master, 100);
    }

    // No native evade: it strips every aura (the clone would lose its appearance) and walks home.
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
            me->DespawnOrUnsummon();
            return;
        }

        if (_scaleTimer <= diff)
        {
            ScaleReaperSummon(me, player);
            _scaleTimer = 2000;
        }
        else
            _scaleTimer -= diff;

        Unit* target = MasterTarget(player);
        if (!target)
        {
            // AttackStop, not CombatStop: enemies still hitting the Warden keep their combat (its
            // threat goes to the master anyway); it simply stops swinging and comes back.
            if (me->GetVictim())
                me->AttackStop();
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, me->GetFollowAngle(), MOTION_SLOT_ACTIVE);
            return;
        }
        if (me->GetVictim() != target)
            AttackStart(target);

        if (!IsClone())
        {
            DoMeleeAttackIfReady();
            return;
        }

        if (_castTimer > diff)
        {
            _castTimer -= diff;
            return;
        }
        _castTimer = 0;
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        SpellInfo const* dirge = _dirge ? sSpellMgr->GetSpellInfo(_dirge) : nullptr;
        SpellInfo const* murder = _murder ? sSpellMgr->GetSpellInfo(_murder) : nullptr;
        bool dirgeReady = dirge && me->IsWithinMeleeRange(target);
        bool murderReady = murder && me->IsWithinDistInMap(target, murder->GetMaxRange(false, me)) &&
            me->IsWithinLOSInMap(target);
        SpellInfo const* chosen = nullptr;
        if (dirgeReady && (_dirgeNext || !murderReady))
            chosen = dirge;
        else if (murderReady)
            chosen = murder;
        if (!chosen)
            return;

        TriggerCastFlags flags = TriggerCastFlags(TRIGGERED_IGNORE_POWER_AND_REAGENT_COST |
            TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_CASTER_AURAS);
        if (me->CastSpell(target, chosen, flags) == SPELL_CAST_OK)
        {
            _dirgeNext = chosen != dirge;
            _castTimer = std::max<uint32>(chosen->StartRecoveryTime, 1000);
        }
        else
            _castTimer = 500;
    }
};

class reaper_secondary_metadata : public GlobalScript
{
public:
    reaper_secondary_metadata() : GlobalScript("reaper_secondary_metadata", {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->SpellFamilyName != 36)
            return;
        if (info->Id == SPELL_ENDBRINGER)
        {
            info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
            info->ProcFlags = 0; // Do not also execute the obsolete leap/resource proc.
        }
        if (info->Id == SPELL_ENDBRINGER_HEAL)
            // This heal copies resolved weapon damage; native healing-taken effects still apply.
            info->DmgClass = SPELL_DAMAGE_CLASS_NONE;
        // Ghostly Weapon's copy helper already carries SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS
        // but, unlike 575847, 705425 and 807420, not IGNORE_CASTER_MODIFIERS: the copied melee
        // damage would take the caster's bonuses a second time.
        if (info->Id == SPELL_GHOSTLY_WEAPON_DAMAGE && info->DmgClass == SPELL_DAMAGE_CLASS_MELEE)
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        // Session-bound auras: Hard Bargain follows 500481, the two Reap transformations and the
        // Decimation counter drive a temporary spell replacement that a relog does not keep.
        if (info->Id == SPELL_SPIRIT_WALKER_SPEED || info->Id == SPELL_CRIMSON_STACK ||
            info->Id == SPELL_HARD_BARGAIN_BUFF || info->Id == SPELL_REDSHADE_THRESH ||
            info->Id == SPELL_DECIMATION_COUNT || info->Id == SPELL_DECIMATION_WINDOW)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
    }
};
}

void AddSC_AscensionReaperSecondary()
{
    new reaper_ghost_speed();
    new reaper_secondary_hits();
    new reaper_secondary_metadata();
    new reaper_tormented_souls_events();
    RegisterSpellScript(aura_ascension_crimson_thirst);
    RegisterSpellScript(aura_ascension_reaper_ghostly_weapon);
    RegisterSpellScript(aura_ascension_reaper_souls_for_the_slaughter);
    RegisterSpellScript(aura_ascension_reaper_siphon_anima);
    RegisterSpellScript(aura_ascension_reaper_gravesite);
    RegisterCreatureAI(npc_ascension_reaper_summon);
}
