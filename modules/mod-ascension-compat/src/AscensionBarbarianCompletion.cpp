/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "AscensionBarbarianCompletion.h"
#include "Creature.h"
#include "Item.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace AscensionBarbarian
{
Player* Owner(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_BARBARIAN ? player : nullptr;
}

Unit* Ancestor(Player* player)
{
    Unit* pet = player ? player->GetGuardianPet() : nullptr;
    // Native summon entry, not an arbitrary charm, guardian or temporary pet.
    return pet && pet->GetEntry() == 51265 && pet->GetOwnerGUID() == player->GetGUID() && pet->IsAlive() ?
        pet : nullptr;
}

bool Enraged(Unit const* unit)
{
    return unit && (unit->HasAuraState(AURA_STATE_ENRAGE) || unit->HasAura(801761) || unit->HasAura(805804));
}

void Extend(Unit* owner, uint32 id, int32 amount, int32 cap)
{
    if (Aura* aura = owner->GetAura(id, owner->GetGUID()))
    {
        int32 duration = aura->GetDuration();
        if (duration <= 0)
            return;
        duration = int32(std::min<int64>(int64(duration) + amount, cap > 0 ? cap : 3600000));
        aura->SetMaxDuration(std::max(aura->GetMaxDuration(), duration));
        aura->SetDuration(duration);
    }
}

void ApplyContracts(SpellInfo* info)
{
    if (!info || info->SpellFamilyName != 18)
        return;
    uint32 id = info->Id;
    if (id == 705186)
        info->Effects[EFFECT_0].SpellClassMask = flag96(0, 64 | 2 | 2048, 0);
    if (id == 705234 || id == 707779)
    {
        // Spears include Maiming Spear. Axe damage resolves on 806960 (word 1,
        // bit 3), while the old private mask accidentally included Throw Weapon.
        info->Effects[EFFECT_0].MiscValue = ASCENSION_STATE_MASKED_CRIT;
        info->Effects[EFFECT_0].SpellClassMask = flag96(5120, 4 | 256 | 8 | 524288, 0);
    }
    if (id == 705152 || id == 707766)
    {
        info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_ADD_PCT_MODIFIER;
        info->Effects[EFFECT_0].SpellClassMask[0] |= 33554432; // Keg Smash's Frost child
    }
    if (id == 804750)
    {
        info->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
    }
    if (id == 807047)
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
    // Amounts are computed at the actual cast/tick instead of stale periodic +1 helpers.
    if (id == 570242 || id == 560446)
    {
        info->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    }
    if (id == 705235)
    {
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
        info->Effects[EFFECT_2].ApplyAuraName = SPELL_AURA_DUMMY;
    }
    if (id == 570337 || id == 561330 || id == 706421)
        for (SpellEffectInfo& effect : info->Effects)
            if (effect.IsAura())
                effect.ApplyAuraName = SPELL_AURA_DUMMY;
    if (id == 806228)
        info->Effects[EFFECT_1].ApplyAuraName = SPELL_AURA_DUMMY;
    if (id == 570106)
        info->Effects[EFFECT_1].Effect = 0; // conditional two-handed helper
    if (id == 804862)
        info->Effects[EFFECT_1].Effect = 0; // refund the actual cost, once
    if (id == 804341)
    {
        info->Effects[EFFECT_0].Effect = 0;
        info->ProcCharges = 3;
    }
    if (id == 524684)
        info->ProcCharges = 1;
    if (id == 706487)
        info->StackAmount = 1;
    if (id == 707584)
    {
        // The old effect launches the jump before the volley; its proc is an unrelated CDR.
        info->Effects[EFFECT_0].Effect = 0;
        info->Effects[EFFECT_2].Effect = 0;
    }
    if (id == 805232)
    {
        info->Effects[EFFECT_1].BasePoints = -1;
        info->Effects[EFFECT_2].Effect = 0;
        for (uint8 i : { uint8(EFFECT_0), uint8(EFFECT_1) })
        {
            info->Effects[i].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ENEMY);
            info->Effects[i].TargetB = SpellImplicitTargetInfo();
        }
    }
    if (id == 801783)
    {
        info->Effects[EFFECT_0].BasePoints = 99; // an additional main-hand attack
        info->Effects[EFFECT_1].Effect = 0; // Might of Utgarde is gated by the owner's proc callback.
    }
    if (id == 573077 || id == 804771 || id == 500534)
        for (SpellEffectInfo& effect : info->Effects)
            if (effect.IsEffect())
                effect.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ALLY);
    if (id == 560125 || id == 560626 || id == 561333)
        info->Effects[EFFECT_0].SpellClassMask = flag96(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    if (id == 783054 || id == 782801 || id == 300870 || id == 707660 || id == 524675 || id == 500534)
    {
        info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
        info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
        info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
        info->AttributesCu |= SPELL_ATTR0_CU_IGNORE_ARMOR;
    }
    // Raging Spirit keeps its native EFFECT2 -100 on 804889 (Unbridled Rage SLS), whose
    // effect 1 is the Spirit cost modifier; 804889 is held only while 801761 is up.
    if (id == 801761)
        // Effect 1 stays the native Ancestral Combat CHANCE_OF_SUCCESS that Unified Rage
        // raises by 25; the DBC target is TARGET_SRC_CASTER with no unit target.
        info->Effects[EFFECT_1].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
    // Barbaric Rage keeps its native SPELLMOD_GLOBAL_COOLDOWN; barbarian_scaling zeroes it
    // outside the enrage aura state and barbarian_enrage_gcd recalculates it.
    if (id == 807911 && info->Effects[EFFECT_0].Effect == SPELL_EFFECT_ASCENSION_RESET_COOLDOWN &&
        info->Effects[EFFECT_0].MiscValue == 500915)
        info->Effects[EFFECT_0].MiscValueB = 1; // clear category 629: every Crush rank, not rank 1 only
    if (id == 560441)
        // Savage: +10% critical damage on Unbridled Rage, i.e. 804889 effect 0 (aura 163).
        // 0x8 (unobtainable 804338) is kept.
        info->Effects[EFFECT_0].SpellClassMask = flag96(4 | 8, 0, 0);
    if (id == 705168 && info->Effects[EFFECT_1].MiscValue == SPELLMOD_EFFECT3)
        // Ancestor's Fury: 801762 has a single effect (aura 350 Frost crit), EFFECT3 hit nothing.
        info->Effects[EFFECT_1].MiscValue = SPELLMOD_EFFECT1;
    if (id == 560879)
        // No Survivors' DURATION +5 s is family-matched on the aura itself; this debuff had no flags.
        info->SpellFamilyFlags[0] |= 0x400;
    if (id == 807183)
        // Crusher: "Removes the cast time of Crush, but its damage is reduced": Crush only,
        // not Ancestral Strike (word 1 0x100000).
        for (SpellEffIndex e : { EFFECT_0, EFFECT_1 })
            info->Effects[e].SpellClassMask[1] = 0x40;
    if (id == 804730)
        // Symbol of the Warspear: every Spear (Barbed, Maiming, Headhunter's, Impaling), as Spear().
        info->Effects[EFFECT_2].SpellClassMask = flag96(0x1400, 0x104, 0);
    if (id == 803908 && info->TargetAuraState == 28)
        // 28 = ASCENSION_TARGET_HEALTH_ABOVE_80_PERCENT is never set in UNIT_FIELD_AURASTATE;
        // the Mak'Gora proc Check already requires HealthAbovePct(80).
        info->TargetAuraState = 0;
    if (id == 801759 && info->CasterAuraState == AURA_STATE_ENRAGE)
        // Outrage: each new ENRAGE aura state makes Unit::ModifyAuraState re-run HandleAllEffects(REAL,
        // true) on owned auras with this CasterAuraState, never undone (ENRAGE is exempt on removal):
        // crit (290) and haste (192) were stacked for good. The cast requirement is kept by
        // barbarian_casts::OnSpellCheckCast (Enraged) and by the client's own DBC.
        info->CasterAuraState = 0;
    if ((id == 804889 || id == 801762) && info->Dispel == DISPEL_ENRAGE)
        // Hidden helpers of Unbridled Rage, held and removed with 801761: an enrage dispel must
        // take 801761 itself, not a helper that would leave the rage up without its modifiers.
        info->Dispel = DISPEL_NONE;
    if (id == 782801)
        info->StackAmount = 0; // Bleed() already folds the remainder into BP0
    if (id >= 578277 && id <= 578285)
        // Smash off-hand ranks 2-10: "plus X with your main-hand", the flat term is applied once.
        info->Effects[EFFECT_0].BasePoints = -1;
    if (id == 560909 && !info->ProcCharges && !info->ProcFlags)
        // Utter Devastation is consumed by the next Crush or Decapitate that uses its crit modifier.
        info->ProcCharges = 1;
    if (info->SpellFamilyFlags[1] & 0x40000)
        // Axe Twirling's JUMP_TARGETS is added to each effect's ChainTarget; a 0 left the weapon
        // percent effect one target short, so the last chained target took no damage.
        for (SpellEffectInfo& effect : info->Effects)
            if (effect.Effect && effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY && !effect.ChainTarget)
                effect.ChainTarget = 1;
    if (id == 706558 && info->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_ADD_FLAT_MODIFIER)
    {
        // Hunting for Sport only modified 804338, an Unbridled Rage nobody gets: armour
        // penetration of its own, zeroed by barbarian_scaling outside Unbridled Rage.
        SpellEffectInfo& effect = info->Effects[EFFECT_0];
        effect.ApplyAuraName = SPELL_AURA_MOD_ARMOR_PENETRATION_PCT;
        effect.MiscValue = 0;
        effect.SpellClassMask = flag96(0, 0, 0);
    }
}
}

namespace
{
using namespace AscensionBarbarian;

class barbarian_scaling : public UnitScript
{
public:
    barbarian_scaling() : UnitScript("barbarian_scaling", true, { UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE }) { }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        Player const* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_BARBARIAN || info->SpellFamilyName != 18)
            return;
        if (!index && Family(info, 1, 262144) && player->HasAura(570242))
            value += player->GetStat(STAT_AGILITY) * 0.2f;
        if (player->HasAura(560446) && ((!index && Family(info, 1, 1048576)) ||
            (index == 2 && Family(info, 1, 64))))
            value += player->GetStat(STAT_STRENGTH) * 0.3f;
        if (!index && info->Id == 560521 && player->HasAura(705235))
            value += player->GetStat(STAT_AGILITY) * 0.1f;
        if (info->Id == 255846 && index == 1)
            value += player->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.15f;
        if (info->Id == 804143 && !index)
            value += player->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.055f;
        if ((info->Id == 801753 || info->Id == 803908) && index == 1)
            value += player->GetTotalAttackPowerValue(BASE_ATTACK);
        // "While enraged": passives are applied whatever the aura state, and losing enrage
        // does not undo them (Unit::ModifyAuraState); barbarian_enrage_gcd recalculates.
        if (info->Id == 804337 && !index && !player->HasAuraState(AURA_STATE_ENRAGE))
            value = 0.0f;
        if (info->Id == 706558 && !index && !player->HasAura(801761))
            value = 0.0f;
    }
};

// Keeps Barbaric Rage's global cooldown modifier in step with the enrage aura state.
// RecalculateAmount re-registers the effect and resends the client modifier once.
class barbarian_enrage_gcd : public UnitScript
{
public:
    barbarian_enrage_gcd() : UnitScript("barbarian_enrage_gcd", true, { UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    static void Sync(Player* player)
    {
        if (AuraEffect* gcd = player->GetAuraEffect(804337, EFFECT_0))
            gcd->RecalculateAmount();
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (Player* player = Owner(unit))
            if (aura && aura->GetSpellInfo()->GetAuraState() == AURA_STATE_ENRAGE)
                Sync(player);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode /*mode*/) override
    {
        if (Player* player = Owner(unit))
            if (application && application->GetBase()->GetSpellInfo()->GetAuraState() == AURA_STATE_ENRAGE)
                Sync(player);
    }
};

// Grisly Meal and Killing Spree gate on markers nothing applies (804755 from the unlearned
// 804754 proc, 850025 from the unlearned, proc-less 561324): grant them on the killing blow.
class barbarian_kills : public PlayerScript
{
public:
    barbarian_kills() : PlayerScript("barbarian_kills",
        { PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_CREATURE_KILLED_BY_PET }) { }

    static bool Knows(Player* player, std::initializer_list<uint32> ids)
    {
        return std::any_of(ids.begin(), ids.end(), [player](uint32 id) { return player->HasSpell(id); });
    }

    static void Reward(Player* killer, Unit* killed, bool byPet)
    {
        Player* player = Owner(killer);
        // OnPlayerPVPKill also arrives with killer == victim (falling, drowning).
        if (!player || !killed || killed == player || !player->IsAlive() ||
            killed->GetCharmerOrOwnerPlayerOrPlayerItself() == player)
            return;
        // "After you kill an enemy that yields experience or honor".
        if (Knows(player, { 804765, 807946, 807947, 807948 }) && player->isHonorOrXPTarget(killed))
            player->CastSpell(player, 804755, true);
        // "Within 6 sec of scoring a killing blow".
        if (!byPet && Knows(player, { 850021, 850038, 850039, 850040, 850041, 850042, 850043 }))
            player->CastSpell(player, 850025, true);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override { Reward(killer, killed, false); }
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override { Reward(killer, killed, false); }
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override { Reward(owner, killed, true); }
};

class aura_ascension_barbarian_lifecycle : public AuraScript
{
    PrepareAuraScript(aura_ascension_barbarian_lifecycle);

    void Apply(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        if (effect->GetEffIndex() != EFFECT_0)
            return;
        if (GetId() == 805804)
            if (Unit* caster = GetCaster())
                caster->AddAura(805843, GetTarget());
        if (Family(GetSpellInfo(), 0, 262144))
            if (Player* caster = Owner(GetCaster()))
            {
                uint32 helper = caster->HasAura(561332) ? 561333 : caster->HasAura(560586) ? 560626 : 0;
                if (helper)
                    caster->CastSpell(GetTarget(), helper, true);
            }
        Player* player = Owner(GetTarget());
        if (!player)
            return;
        uint32 id = GetId();
        // Killing Spree: "or applying Born in Blood" (a refresh re-opens the window too).
        // AddAura, not CastSpell: this also runs while auras load at login.
        if (id == 560521 && barbarian_kills::Knows(player, { 850021, 850038, 850039, 850040, 850041, 850042, 850043 }))
            player->AddAura(850025, player);
        if (id == 801761 && player->HasAura(707410))
            player->CastSpell(player, 521240, true);
        if (id == 801761)
        {
            // 804889 carries the Spirit cost (Raging Spirit) and the crit damage (Savage).
            player->AddAura(804889, player);
            // Ancestor's Fury's Frost crit rides on 801762; it lives as long as 801761,
            // whose duration Unstoppable Rage and Extend() lengthen.
            if (player->HasAura(705168))
                if (Aura* crit = player->AddAura(801762, player))
                {
                    crit->SetMaxDuration(-1);
                    crit->SetDuration(-1);
                }
            if (AuraEffect* arp = player->GetAuraEffect(706558, EFFECT_0))
                arp->RecalculateAmount();
        }
        if (id == 707410)
            if (AuraEffect* cost = player->GetAuraEffect(804889, EFFECT_1))
                cost->RecalculateAmount();
        if (id == 560933 && player->HasAura(560910))
            player->CastSpell(player, 560909, true);
        // Rapid Throw exists only as the transformed Throw Weapon: learned unannounced, the client
        // sees it only in the slots SetTemporarySpellReplacement rewrites (P-144). Announced, it kept
        // a spellbook entry of its own for as long as the talent was up, proc or not.
        if ((id == 705198 || id == 706487) && player->GetSpellMap().find(807482) == player->GetSpellMap().end())
            player->LearnTemporarySpellReplacementTarget(807482);
        if (id == 706487)
        {
            for (auto const& [sid, entry] : player->GetSpellMap())
                if (player->HasActiveSpell(sid) &&
                    Family(sSpellMgr->GetSpellInfo(sid), 1, 262144) && sid != 807236)
                    player->SetTemporarySpellReplacement(sid, 807482);
        }
    }

    void Remove(AuraEffect const* effect, AuraEffectHandleModes /*mode*/)
    {
        if (effect->GetEffIndex() != EFFECT_0)
            return;
        if (GetId() == 805804)
            GetTarget()->RemoveAurasDueToSpell(805843, GetCasterGUID());
        if (Family(GetSpellInfo(), 0, 262144))
        {
            GetTarget()->RemoveAurasDueToSpell(560626, GetCasterGUID());
            GetTarget()->RemoveAurasDueToSpell(561333, GetCasterGUID());
        }
        Player* player = Owner(GetTarget());
        if (!player)
            return;
        uint32 id = GetId();
        if (id == 706487 || id == 705198)
            for (auto const& [sid, entry] : player->GetSpellMap())
            {
                (void)entry;
                if (player->GetTemporarySpellReplacement(sid) == 807482)
                    player->SetTemporarySpellReplacement(sid, 0);
            }
        if (id == 705198)
            player->RemoveAurasDueToSpell(706487);
        if (id == 705198)
            player->removeSpell(807482, SPEC_MASK_ALL, true);
        if (id == 804141)
            player->RemoveAurasDueToSpell(572531);
        if (id == 707410)
            if (AuraEffect* cost = player->GetAuraEffect(804889, EFFECT_1))
                cost->RecalculateAmount();
        if (id == 801761)
        {
            player->RemoveAurasDueToSpell(804889);
            player->RemoveAurasDueToSpell(801762);
            // HasAura(801761) is already false here: _UnapplyAura erases it before the effects.
            if (AuraEffect* arp = player->GetAuraEffect(706558, EFFECT_0))
                arp->RecalculateAmount();
        }
        if (id == 800637 || id == 355562)
            player->RemoveAurasDueToSpell(800645);
    }

    void Periodic(AuraEffect const* effect, bool& /*periodic*/, int32& amplitude)
    {
        if (GetId() == 560521 && effect->GetEffIndex() <= EFFECT_1)
            if (Unit* caster = GetCaster())
                if (caster->HasAura(705235))
                    amplitude = int32(amplitude / 1.25f);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_barbarian_lifecycle::Apply,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_barbarian_lifecycle::Remove,
            EFFECT_ALL, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_barbarian_lifecycle::Periodic,
            EFFECT_ALL, SPELL_AURA_ANY);
    }
};

class barbarian_casts : public AllSpellScript
{
public:
    barbarian_casts() : AllSpellScript("barbarian_casts", { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CALCULATED_TARGET,
        ALLSPELLHOOK_ON_SUCCESSFUL_INTERRUPT, ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_BEFORE_EFFECTS }) { }

    void OnSpellBeforeEffects(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        Player* player = Owner(caster);
        if (!player || spell->IsTriggered() || !Family(info, 1, 2048) || !spell->TryMarkScriptEventHandled(26))
            return;
        // Native validation, miss refunds and base-cost spending have already run.
        bool landed = std::any_of(spell->GetUniqueTargetInfo()->begin(), spell->GetUniqueTargetInfo()->end(),
            [](TargetInfo const& hit) { return hit.missCondition == SPELL_MISS_NONE; });
        if (!landed)
            return;
        uint32 extra = std::min<uint32>(player->GetPower(POWER_ENERGY) / 2,
            std::max(0, info->Effects[EFFECT_2].CalcValue(player)));
        player->ModifyPower(POWER_ENERGY, -int32(extra));
        spell->SetScriptExtraPowerSpent(extra);
    }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& result) override
    {
        Player* player = Owner(spell->GetCaster());
        if (player && !spell->IsTriggered() &&
            (spell->GetSpellInfo()->Id == 801759 || spell->GetSpellInfo()->Id == 803499) && !Enraged(player))
            result = SPELL_FAILED_CASTER_AURASTATE;
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skip*/) override
    {
        Player* player = Owner(caster);
        if (!player || spell->IsTriggered())
            return;
        auto talent = [&](uint32 passive, uint32 child)
        {
            if (player->HasAura(passive))
                player->CastSpell(player, child, true);
        };
        uint32 id = info->Id;
        // "You may only have 1 Spirit active". The Spirits' effect 0 is an enemy area aura
        // that never reaches the caster's lifecycle Apply, so the rule is kept at the cast.
        if (Spirit(id))
            for (uint32 spirit : { 707763u, 707764u, 707775u, 712467u, 712468u })
                if (spirit != id)
                    player->RemoveOwnedAura(spirit);
        // Breath of the North has no cooldown: one breath per emptied Tankard.
        if (info->CasterAuraSpell == 806055)
            player->RemoveAurasDueToSpell(806055);
        if (Family(info, 1, 16777216))
        {
            talent(805812, 570106);
            Item* weapon = player->GetWeaponForAttack(BASE_ATTACK, true);
            if (player->HasAura(805812) && weapon && weapon->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                player->CastSpell(player, 570222, true);
        }
        if (id == 801761)
        {
            talent(561359, 560933);
            talent(705202, 560521);
            if (player->HasAura(572799))
                if (Unit* pet = Ancestor(player))
                    player->CastSpell(pet, 804771, true);
        }
        if (Family(info, 1, 8388608))
            talent(560205, 560521);
        if (Spear(info) && Enraged(player) && player->HasAura(560564))
        {
            player->CastSpell(player, 804341, true);
            // These client-authored charges use the native category-charge system.
            player->RestoreSpellChargeCategory(60, 3);
        }
        if (Family(info, 1, 262144) && player->HasAura(705198) && roll_chance_i(25))
            player->CastSpell(player, 706487, true);
        if (id == 807482)
        {
            player->RemoveAurasDueToSpell(706487);
            player->CastSpell(player, 560521, true);
        }
        if (id == 560883 && player->HasAura(712429) && !player->HasSpellCooldown(712429))
        {
            player->RemoveSpellCooldown(id, true);
            player->AddSpellCooldown(712429, 0, 90000);
        }
        if (Family(info, 1, 536870912))
        {
            talent(705162, 560946);
            // Ancestral Roar: "and 1 Fill Level stack" under the Fill Level specialization.
            if (player->HasAura(500061))
                player->CastSpell(player, 805813, true);
        }
        if (Family(info, 0, 268435456))
        {
            uint32 chance = player->HasAura(681466) ? 60 : player->HasAura(681063) ? 30 : 0;
            if (chance && roll_chance_i(chance))
                player->RemoveSpellCooldown(id, true);
        }
    }

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& hit) override
    {
        Player* player = Owner(spell->GetCaster());
        if (!player || !target || target == player)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        float multiplier = 1.0f;
        if (Family(info, 1, 2048) && !target->IsControlledByPlayer())
            multiplier *= 1.0f + spell->GetScriptExtraPowerSpent() / 100.0f;
        if (Family(info, 1, 1048576))
            if (Item* weapon = player->GetWeaponForAttack(BASE_ATTACK, true))
                multiplier *= weapon->GetTemplate()->Delay / 2700.0f;
        if ((Whirl(info->Id) || info->Id == 805232) && player->HasAura(804750) && target->HealthBelowPct(35))
            multiplier *= 2.0f;
        if (Family(info, 1, 64))
        {
            if (player->HasAura(807047) && !target->IsControlledByPlayer())
                multiplier *= 1.15f;
            if (player->HasAura(804745) &&
                (target->HasAura(520523) || target->HasAura(520710) || target->HasAura(520711)))
                hit.crit = true;
        }
        hit.damage = int32(hit.damage * multiplier);
        hit.damageBeforeTakenMods = int32(hit.damageBeforeTakenMods * multiplier);
    }

    void OnSpellSuccessfulInterrupt(Spell* spell, Unit* /*target*/) override
    {
        Player* player = Owner(spell->GetCaster());
        if (player && spell->GetSpellInfo()->Id == 802792 && !spell->IsTriggered() && player->HasAura(705196) &&
            spell->TryMarkScriptEventHandled(25))
        {
            player->CastSpell(player, 804862, true);
            player->ModifyPower(POWER_ENERGY, std::max(0, spell->GetPowerCost()) / 2);
        }
    }
};
}

void AddAscensionBarbarianCompletionScripts()
{
    new barbarian_scaling();
    new barbarian_enrage_gcd();
    new barbarian_kills();
    new barbarian_casts();
    RegisterSpellScript(aura_ascension_barbarian_lifecycle);
}
