/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
//
// Runemaster (class 32, CLASS_SPIRIT_MAGE) — three Runeshroud/Runeblade talents that Spell.dbc
// describes but that nothing on the server ever executes:
//
//   705563 Shroudwalker   — Warpdagger costs no cooldown while Runeshroud is up.
//   705583 Runic Breakout — Runeshroud-gated abilities stay usable, and instant, for 3 sec
//                           after Runeshroud breaks.
//   705580 Runic Omen     — every 3rd Runeblade is empowered.
//
// Everything below is driven by values READ in Spell.dbc; no number is invented here. The field
// offsets used to read them are the ones the core itself uses (DBCfmt.h SpellEntryfmt /
// DBCStructure.h SpellEntry): SpellFamilyName is field 208, SpellFamilyFlags 209-211,
// EffectSpellClassMask 122-130 (effect-major: effect e uses 122+3e .. 124+3e).
//
#include "DBCStores.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"

namespace
{
enum RunemasterRuneSpells : uint32
{
    // Runeshroud, the level-4 class ability (AscensionCustomClassData.h: {32, 4, 500288}).
    // Effect 0 is SPELL_AURA_MOD_SHAPESHIFT form 30, effect 1 SPELL_AURA_MOD_STEALTH: the
    // "abilities requiring Runeshroud" are gated on that form, not on an aura state.
    SPELL_RUNESHROUD = 500288,
    SPELL_WARPDAGGER = 500287,
    SPELL_SHROUDWALKER = 705563,
    SPELL_RUNIC_BREAKOUT = 705583,
    // 520767 "Runic Breakout / Proc", DurationIndex 27 = 3000 ms, is the payload that carries the
    // whole sentence of the talent: effect 0 is SPELL_AURA_MOD_IGNORE_SHAPESHIFT (275) over the
    // Runeshroud kit, effect 2 an ADD_PCT_MODIFIER on SPELLMOD_CASTING_TIME of -100% over the
    // same kit. Its effect 1 is SPELL_EFFECT_ASCENSION_TRIGGER_SPELL_DELAYED, which hands 520759
    // over 50 ms later on its own -- 520759 must not be cast directly.
    SPELL_RUNIC_BREAKOUT_WINDOW = 520767,
    SPELL_RUNIC_BREAKOUT_LATE = 520759,
    SPELL_RUNIC_OMEN = 705580,
    SPELL_RUNIC_OMEN_STACK = 520285,
    SPELL_RUNIC_OMEN_EMPOWER = 705596,
    SPELL_RUNEBLADE = 707141
};

enum RunemasterRuneDurations : uint32
{
    // SpellDuration.dbc row 27 = 3000 ms. 705583 prints its window as "$/1000;520759s2 sec" and
    // 520759's effect 1 base value is 3000, so three seconds is the written number; only the
    // DurationIndex of 520759 itself was left at 0.
    DURATION_THREE_SECONDS = 27
};

bool IsRunemaster(Player const* player)
{
    return player && player->getClass() == CLASS_SPIRIT_MAGE;
}

// Shroudwalker (705563): "Your Warpdagger now incurs no cooldown if used while your Runeshroud
// is active."
//
// WHY THIS LIVES IN C++ AND NOT IN THE DBC ROW. 705563's effect 0 is an ADD_FLAT_MODIFIER of
// -25000 on SPELLMOD_EFFECT1 (MiscValue 3), family 38, class mask (0, 1<<30, 0). That mask
// selects exactly one spell in Spell.dbc: 500510 "Runeshroud / SLS", whose own effect 0 is a
// flat SPELLMOD_COOLDOWN of 0 over Warpdagger. So the DBC chain is
// 705563 -> 500510's cooldown modifier -> Warpdagger, and it is coherent but dead: the only
// thing that would ever apply 500510 is effect 178 of 520759, and effect 178 is
// &Spell::EffectNULL ("unknown Ascension effect") in SpellEffects.cpp. The row is therefore
// inert, NOT a wildcard, and it is deliberately left alone: forcing it to SPELL_AURA_DUMMY the
// way AscensionNecromancerContracts.cpp does for 552011 would change nothing observable and
// would destroy a correctly masked data path. 552011 is a different case: it is family 3 with an
// empty mask, which really does hit every Mage-family spell.
//
// The number also disagrees with the text: -25000 ms against a 30000 ms cooldown leaves 5 sec,
// while both the talent description and the tooltip say "no cooldown". The description wins here.
class spell_ascension_runemaster_shroudwalker : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_shroudwalker);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({SPELL_SHROUDWALKER, SPELL_RUNESHROUD});
    }

    void ClearCooldown()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!IsRunemaster(player) || GetSpell()->IsTriggered() ||
            GetSpellInfo()->Id != SPELL_WARPDAGGER)
            return;
        // Warpdagger carries SPELL_ATTR1_ALLOW_WHILE_STEALTHED, so SpellInfo::IsBreakingStealth()
        // is false and Runeshroud is still on the player at this point.
        if (!player->HasAura(SPELL_SHROUDWALKER) ||
            !player->HasAura(SPELL_RUNESHROUD, player->GetGUID()))
            return;
        // Spell::cast runs SendSpellCooldown long before CallScriptAfterCastHandlers, so the
        // cooldown to remove already exists. Warpdagger is category 130 and Spell.dbc holds no
        // other spell in that category, so the single entry is the whole cooldown.
        player->RemoveSpellCooldown(GetSpellInfo()->Id, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_runemaster_shroudwalker::ClearCooldown);
    }
};

// Runic Omen (705580): "Every 3rd cast of Runeblade now deals $705596s1% increased damage."
//
// 520285 "Runic Omen / Stack" (StackAmount 3, 10 sec) is the counter; 705596 "Runic Omen / Proc"
// is the reward and holds the only written number: an ADD_PCT_MODIFIER of +30 on SPELLMOD_DAMAGE
// with class mask (0, 0, 262144), which Spell.dbc matches to the eleven Runeblade ranks
// (707141, 707143-707148, 573444-573447) and to nothing else. The modifier therefore needs no
// help from this script beyond being applied and consumed.
//
// WHICH CAST IS EMPOWERED. 705596's own tooltip says "Damage of next Runeblade increased", and
// 520285's says "At 3 Stacks, Runeblade is empowered". Both are honoured by counting here, in
// AfterCast: the damage of the cast in hand was already resolved in HandleLaunchPhase, so the
// buff earned now lands on the following Runeblade. Because the empowered cast also adds a
// stack, the empowered casts are the 4th, 7th, 10th ... -- a period of exactly three, with one
// cast of ramp-up at the very start.
//
// WHAT IS LEFT OUT. The client tooltip of 705580 adds "and increases all magic damage you deal
// to the target by ${$w2}% for $d sec". No spell in Spell.dbc carries that debuff: 705580's
// effect 1 is a bare DUMMY worth 1 and its duration is -1, and 705596's effect 1 is a
// PROC_TRIGGER_SPELL pointing at 520756 "Runic Omen / Deprecated", which has no effects at all.
// Nothing is invented for it, so the rider is not implemented.
class spell_ascension_runemaster_runic_omen : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_runic_omen);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({SPELL_RUNIC_OMEN, SPELL_RUNIC_OMEN_STACK, SPELL_RUNIC_OMEN_EMPOWER});
    }

    void Count()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!IsRunemaster(player) || GetSpell()->IsTriggered() || !player->IsAlive() ||
            sSpellMgr->GetFirstSpellInChain(GetSpellInfo()->Id) != SPELL_RUNEBLADE ||
            !player->HasAura(SPELL_RUNIC_OMEN))
            return;
        SpellInfo const* stackInfo = sSpellMgr->GetSpellInfo(SPELL_RUNIC_OMEN_STACK);
        if (!stackInfo || !stackInfo->StackAmount)
            return;
        player->CastSpell(player, SPELL_RUNIC_OMEN_STACK, true);
        Aura* counter = player->GetAura(SPELL_RUNIC_OMEN_STACK, player->GetGUID());
        if (!counter || uint32(counter->GetStackAmount()) < stackInfo->StackAmount)
            return;
        player->RemoveAurasDueToSpell(SPELL_RUNIC_OMEN_STACK, player->GetGUID());
        player->CastSpell(player, SPELL_RUNIC_OMEN_EMPOWER, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_ascension_runemaster_runic_omen::Count);
    }
};

// Runic Breakout (705583): "Abilities requiring Runeshroud can now be used for 3 sec and have no
// cast time after Runeshroud breaks."
//
// This one cannot be a SpellScript: the trigger is the disappearance of an aura, not a cast. The
// shape is the one AscensionRunemasterSecondary.cpp's runemaster_secondary_auras already uses,
// and the class test comes first so the ~700 players in world leave after one comparison.
class runemaster_runes_auras : public UnitScript
{
public:
    runemaster_runes_auras() : UnitScript("runemaster_runes_auras", true, {UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!IsRunemaster(player) || !application)
            return;
        Aura* aura = application->GetBase();
        if (aura->GetId() != SPELL_RUNESHROUD || aura->GetCasterGUID() != player->GetGUID())
            return;
        // A corpse does not get a grace window; same exclusion as the Primordialism branch of
        // runemaster_talent_events in AscensionRunemasterTalents.cpp.
        if (mode == AURA_REMOVE_BY_DEATH || !player->IsAlive() || !player->IsInWorld() ||
            !player->HasAura(SPELL_RUNIC_BREAKOUT))
            return;
        player->CastSpell(player, SPELL_RUNIC_BREAKOUT_WINDOW, true);
    }
};

class runemaster_runes_metadata : public GlobalScript
{
public:
    runemaster_runes_metadata() : GlobalScript("runemaster_runes_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    // No SpellFamilyName guard: every id below is matched one by one. A family test in a pass
    // like this one excludes spells in silence, without a single log line.
    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info)
            return;
        if (info->Id == SPELL_RUNIC_OMEN_EMPOWER)
        {
            // "Damage of next Runeblade increased by ${$w1}%" -- one Runeblade, so one charge.
            // Spell.dbc leaves ProcCharges at 0, which would let the +30% ride every Runeblade
            // for the aura's whole 10 sec. With one charge, Aura::CalcMaxCharges reports it as a
            // charged aura and Player::RemoveSpellMods drops it at the end of the cast that used
            // it -- the native "next spell only" path, and 705596 has no spell_proc row, so the
            // "don't handle spells with spell_proc entry defined" bail-out there does not apply.
            info->ProcCharges = 1;
        }
        if (info->Id == SPELL_RUNIC_BREAKOUT_LATE && !info->GetDuration())
        {
            // 520759's DurationIndex is 0, so the aura it applies expires on the tick it is
            // created and does nothing at all. Its written window is three seconds: 705583
            // prints "$/1000;520759s2 sec" and 520759's effect 1 base value is 3000.
            if (SpellDurationEntry const* duration = sSpellDurationStore.LookupEntry(DURATION_THREE_SECONDS))
                info->DurationEntry = duration;
        }
        if (info->Id == SPELL_RUNIC_OMEN_STACK || info->Id == SPELL_RUNIC_OMEN_EMPOWER ||
            info->Id == SPELL_RUNIC_BREAKOUT_WINDOW || info->Id == SPELL_RUNIC_BREAKOUT_LATE)
        {
            // Short-lived bookkeeping auras; they must not survive a logout.
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
    }
};
} // namespace

void AddSC_AscensionRunemasterRunes()
{
    new runemaster_runes_auras();
    new runemaster_runes_metadata();
    RegisterSpellScript(spell_ascension_runemaster_shroudwalker);
    RegisterSpellScript(spell_ascension_runemaster_runic_omen);
}
