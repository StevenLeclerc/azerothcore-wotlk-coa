/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"

namespace
{
enum RunemasterFractureSpells : uint32
{
    // 803018 "Fracture", Spell.dbc read on 2026-09-20: SpellFamilyName 38, SchoolMask 16
    // (Frost), DmgClass 2 (melee), EquippedItemClass 2, SpellLevel 57, no rank chain in
    // `spell_ranks`, no row in `spell_script_names`, no row in `spell_proc`.
    //   Effect 0 SPELL_EFFECT_SCHOOL_DAMAGE, BasePoints 920 (value 921), DieSides 189,
    //            EffectRealPointsPerLevel 0.35 with BaseLevel = SpellLevel = 57 and
    //            MaxLevel 0, so +8 at level 80 -- the record's only scaling term
    //   Effect 1 SPELL_EFFECT_DUMMY, EffectTriggerSpell 803067 -> inert, see below
    //   Effect 2 SPELL_EFFECT_POWER_BURN (62), BasePoints 24 (value 25), MiscValue 0
    //            (POWER_MANA), EffectValueMultiplier 0.0f so it adds no damage
    // Effects 0 and 2 are executed natively, but the tooltip's two attack-power terms
    // ($AP*1.5 on the damage, $AP*.35 on the mana burn) are NOT implemented anywhere:
    // Effects[i].BonusMultiplier is 0.0 for all three effects, 803018 is absent from
    // AscensionStockCoefficientData.h and has no `spell_bonus_data` row, and that generated
    // header's own banner says CoA authors its coefficients in tooltip text, never in the
    // DBC column. Out of this file's scope; see the SQL file.
    SPELL_FRACTURE = 803018
};

// "Damage dealt is guaranteed to critically strike against frozen targets."
//
// The crit of a damaging spell is decided once, in Spell::DoAllEffectOnLaunchTarget:
//   targetInfo.crit = roll_chance_f(...);  then  sScriptMgr->OnSpellCalculatedTarget(...)
// so ALLSPELLHOOK_ON_CALCULATED_TARGET is the only hook that runs after the roll and can
// make it unconditional. Two nearby mechanisms were rejected on purpose:
//
//   * SPELLVALUE_FORCED_CRIT_RESULT only adds SPELL_HIT_TYPE_CRIT to the packet
//     (Spell.cpp, "override with forced crit, only visual result"): the damage has
//     already been computed from targetInfo.crit, so it would show a crit worth normal
//     damage.
//   * ALLSPELLHOOK_ON_CRIT_CHANCE runs BEFORE Unit::SpellTakenCritChance, which can still
//     subtract from the chance; "100" there is not a guarantee.
//
// The data record the client shipped for this clause is 803125 "Versus Frozen": a hidden
// passive of family 38 whose effect 0 is SPELL_AURA_OVERRIDE_CLASS_SCRIPTS, MiscValue 20000
// (private "crit chance versus aura state" selector), MiscValueB 4 (AURA_STATE_FROZEN),
// BasePoints 99 and EffectSpellClassMask (0, 0, 0x04000000) -- which selects Fracture and
// 500272 Echo, and nothing else in family 38. It is inert twice over, both measured:
//   * nothing grants or triggers 803125 (no EffectTriggerSpell in Spell.dbc, no row in
//     AscensionCustomClassData.h, no reference anywhere in the module);
//   * 20000 is a raw private selector; Unit::GetAscensionConditionalCombatModifier only
//     knows the reviewed 21000-series, and AscensionConditionalCombatRules -- which is what
//     rewrites 20000 into 21004 ASCENSION_STATE_MASKED_GUARANTEED_CRIT for spells such as
//     582310 Kingdom Hearts -- has no family 38 entry at all.
// Reviving that path would need both of those, in two files this script does not own; the
// hook below stands on its own and cannot double-count with it, since a crit is a boolean.
class runemaster_fracture_casts : public AllSpellScript
{
public:
    runemaster_fracture_casts() : AllSpellScript("runemaster_fracture_casts",
        {ALLSPELLHOOK_ON_CALCULATED_TARGET}) { }

    void OnSpellCalculatedTarget(Spell* spell, Unit* target, TargetInfo& hit) override
    {
        // This hook runs for every target of every spell in the world: class first.
        Unit* caster = spell->GetCaster();
        if (!caster->IsPlayer() || caster->getClass() != CLASS_SPIRIT_MAGE)
            return;
        // No SpellFamilyName guard (P-055). The identity test below is exact and stricter;
        // a family guard could only ever disarm it in silence if the DBC were re-imported.
        SpellInfo const* info = spell->GetSpellInfo();
        if (info->Id != SPELL_FRACTURE || hit.crit || !target)
            return;
        // Only SPELL_MISS_NONE and a landed reflect reach this hook at all: Spell.cpp:8484-8490
        // returns before the crit roll (8541) for anything else. The guard exists for the
        // reflect -- there the unit handed to the hook is the Runemaster himself (8488), and
        // without it a frozen Runemaster would guarantee a critical against his own head.
        if (hit.missCondition != SPELL_MISS_NONE)
            return;
        // "Frozen" is the native aura state: SpellInfo::GetAuraState (SpellInfo.cpp:2193-2197)
        // gives AURA_STATE_FROZEN to any Frost spell carrying SPELL_AURA_MOD_ROOT or
        // SPELL_AURA_MOD_STUN, which covers this class's own freezes -- 807822 Cryobrand,
        // 803780 Hoarfrost, 806998 Frost Sigil, 805730 Glacial Rune, 804060 Permafrost Rune,
        // 800309 Icy Shackles and 524930 Ice Rune, all read in Spell.dbc.
        // Two-argument form on purpose. The three-argument one honours EVERY caster-side
        // SPELL_AURA_ABILITY_IGNORE_AURASTATE whose class mask selects this spell
        // (Unit.cpp:7997-8006 returns true before it ever looks at the target), and in family 38
        // that is not only 712487 "Leyfrost" (dead: ProcFlags 0, no `spell_proc` row, nothing
        // casts it) but also 520759 "Runic Breakout", effect 0 aura 262, EffectSpellClassMask
        // (0x80, 0, 0x04002000) -- and 0x04002000 & Fracture's SpellFamilyFlags 0x04000000 is
        // non-zero, so SpellInfo::IsAffected says yes. AscensionRunemasterRunes.cpp:188 puts
        // 520759 on the Runemaster (via 520767, effect 183, trigger 520759) for three seconds
        // after every Runeshroud break. That aura is a Runeshroud gate, not a grant of Frozen:
        // honouring it would hand out a free guaranteed-crit window on any target, which the
        // tooltip never promises.
        // AURA_STATE_FROZEN is not in PER_CASTER_AURA_STATE_MASK (SharedDefines.h:1395-1396),
        // so a target frozen by somebody else still counts -- the tooltip says "frozen
        // targets", not "frozen by you".
        if (!target->HasAuraState(AURA_STATE_FROZEN))
            return;
        // Crit immunity keeps the last word, on the same terms the core gives its own
        // guaranteed-crit path in Unit::SpellTakenCritChance.
        if (target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE) <= -100)
            return;
        hit.crit = true;
    }
};
}

void AddSC_AscensionRunemasterFracture()
{
    new runemaster_fracture_casts();
}
