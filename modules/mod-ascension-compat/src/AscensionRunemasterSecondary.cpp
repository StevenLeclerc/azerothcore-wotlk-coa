/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <limits>

namespace
{
enum RunemasterSecondarySpells : uint32
{
    SPELL_RUNEBLADE = 707141,
    SPELL_FISTS_OF_POWER = 92153,
    SPELL_FISTS_AMOUNT = 500462,
    SPELL_FISTS_HIT = 712298,
    SPELL_RIFTBLADE = 92154,
    SPELL_RIFTBLADE_COUNTER = 500468,
    SPELL_RIFTBLADE_MANA = 500466,
    SPELL_WATER_AMOUNT = 802645,
    SPELL_PRIMORDIAL_BLAST = 800732,
    SPELL_SMOLDER = 801087,
    SPELL_WARPDAGGER = 500287,
    SPELL_WARPDAGGER_TELEPORT = 500562,
    SPELL_SPELLFIRE_RUNES = 801511,
    SPELL_SPELLFIRE_CHANCE = 802661,
    SPELL_SPELLFIRE_READY = 801512,
    SPELL_UNLEASHED_TATTOOS = 804561,
    SPELL_UNLEASHED_FIRE = 807377,
    SPELL_UNLEASHED_WATER = 807378,
    SPELL_FIRE_TATTOO = 801106,
    SPELL_WATER_TATTOO = 801107,
    SPELL_ARCANE_SIGIL = 805380,
    SPELL_ARCANE_SIGIL_DOT = 807819,
    SPELL_ARCANE_SIGIL_SILENCE = 808020,
    SPELL_FROST_SIGIL_DAMAGE = 807825,
    SPELL_WATER_SIGIL_DRAIN = 807818,
    SPELL_FIRE_ENGRAVING = 653211,
    SPELL_FIREBRAND = 653210,
    SPELL_FIREBRAND_EXPLOSION = 653212,
    SPELL_AIR_ENGRAVING_PASSIVE = 653223,
    SPELL_EXPANSIVE_ENGRAVER = 807496,

    SPELL_ELEMENTAL_BURST = 802202,
    SPELL_FIST_OF_THE_ANCIENTS = 712326,
    SPELL_RUNESHROUD = 500288,
    SPELL_PHASE_OUT = 500671,
    SPELL_EYE_OF_THE_BEHOLDER = 500121,

    // Talents (passives whose aura 42 has ProcFlags 0 and no spell_proc row: dead by
    // themselves, driven from here) and what each one hands out.
    SPELL_BLADE_RIFT = 520237,
    SPELL_BLADE_RIFT_BUFF = 520238,
    SPELL_ANCIENT_WARRIOR = 520755,
    SPELL_ANCIENT_WARRIOR_RECORD = 520757,
    SPELL_GLYPH_OF_DISAPPEARANCE = 560037,
    SPELL_DISAPPEARANCE_WINDOW = 561059,
    SPELL_WINDSAGE = 705568,
    SPELL_WINDSAGE_BUFF = 705569,
    SPELL_WINDSAGE_STRIKE = 706457,
    SPELL_ELEMENTAL_CARVINGS = 705618,
    SPELL_DECODER = 706523,
    SPELL_POWER_OVERWHELMING = 707876,
    SPELL_POWER_OVERWHELMING_CRIT = 560051,
    SPELL_POWER_OVERWHELMING_RECORD = 561055,
    SPELL_BURNED_ETCHING = 500476,
    SPELL_BURNED_ETCHING_VALUES = 500475,
    SPELL_BURNED_ETCHING_DAMAGE = 500474,
    SPELL_LEY_WALKER = 500250,
    SPELL_LEY_WALKER_DISPEL = 503734,
    SPELL_COLD_ELEMENTS = 520138,
    SPELL_COLD_ELEMENTS_BUFF = 802648,
    SPELL_RUNIC_OBLITERATION = 705560,
    SPELL_RUNIC_OBLITERATION_READY = 805742,
    SPELL_RUNIC_OBLITERATION_SPELL = 805794,
    SPELL_SURGING_SLASH = 705579,
    SPELL_SURGING_SLASH_COUNTER = 572358,
    SPELL_SURGING_SLASH_TRANSFORM = 803736,
    SPELL_KIRIN_TOR_AGENT = 705634,
    SPELL_KIRIN_TOR_AGENT_AURA = 706422,
    SPELL_EARTH_WIND_AND_FIRE = 706531,
    SPELL_EARTH_WIND_AND_FIRE_STACKER = 707458,
    SPELL_WILD_STEAM = 803737,
    SPELL_LEYLINE_MAGICIAN = 706823,
    SPELL_LEYLINE_MAGICIAN_RECORD = 520146,
    SPELL_RUNEBOUND_SURGE = 707423,
    SPELL_RUNEBOUND_SURGE_BUFF = 707426,
    SPELL_EARTHEN_CODEX = 707460,
    SPELL_EARTHEN_CODEX_BUFF = 707730,
    SPELL_SIGILIST = 705586,
    SPELL_SIGILIST_BUFF = 706525,
    SPELL_LEYFROST_WINDOW = 712464,
    SPELL_ELEMENTAL_INSCRIPTION = 806737,
    SPELL_ELEMENTAL_INSCRIPTION_RECORD = 706522,
    SPELL_ETERNAL_MAGIC = 806698,
    SPELL_ETERNAL_MAGIC_CHARGES = 800192
};

bool HasTattoo(Player* player, uint32 root)
{
    for (auto const& [key, application] : player->GetAppliedAuras())
    {
        Aura* aura = application->GetBase();
        if (aura->GetCasterGUID() == player->GetGUID() && sSpellMgr->GetFirstSpellInChain(aura->GetId()) == root)
            return true;
    }
    return false;
}

uint32 KnownRank(Player* player, uint32 root)
{
    for (auto const& [id, entry] : player->GetSpellMap())
        if (player->HasActiveSpell(id) && sSpellMgr->GetFirstSpellInChain(id) == root)
            return id;
    return 0;
}

// Highest active rank of a chain (the Reaper's ActiveReapRank, AscensionReaperTalents.cpp): a
// cooldown or a button lives on the rank the player really casts, never on the rank-1 id that
// the Ascension records name literally. For a spell outside any chain, the spell itself.
uint32 ActiveRank(Player* player, uint32 root)
{
    uint32 best = 0;
    uint8 bestRank = 0;
    for (auto const& [id, entry] : player->GetSpellMap())
    {
        if (!entry || sSpellMgr->GetFirstSpellInChain(id) != root || !player->HasActiveSpell(id))
            continue;
        uint8 rank = sSpellMgr->GetSpellRank(id);
        if (!best || rank > bestRank)
        {
            best = id;
            bestRank = rank;
        }
    }
    return best;
}

uint32 ResolveRank(Player* player, uint32 spellId)
{
    return ActiveRank(player, sSpellMgr->GetFirstSpellInChain(spellId));
}

// The talent's DBC ProcChance, through the player's SPELLMOD_CHANCE_OF_SUCCESS modifiers, the way
// Aura::CalcProcChance treats a proc entry. Used where the talent is driven from a cast hook.
bool RollTalent(Player* player, uint32 talentId)
{
    SpellInfo const* talent = sSpellMgr->GetSpellInfo(talentId);
    if (!talent)
        return false;
    int32 chance = int32(talent->ProcChance);
    player->ApplySpellMod(talentId, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    return roll_chance_i(std::clamp(chance, 0, 100));
}

void CastOnSelf(Player* player, uint32 spellId)
{
    if (sSpellMgr->GetSpellInfo(spellId))
        player->CastSpell(player, spellId, true);
}

// Same rules as the core's effect 165 handler (SpellEffects.cpp, ModifyAscensionCooldown): a zero
// delta resets, a reduction at least as long as what is left resets. Applied to `rank`, which the
// caller has already resolved.
void ModifyRankCooldown(Player* player, uint32 rank, int32 delta, bool resetCategory)
{
    if (!rank)
        return;
    if (delta < 0)
    {
        uint32 remaining = player->GetSpellCooldownDelay(rank);
        if (!remaining)
            return;
        if (uint64(-int64(delta)) < remaining)
        {
            player->ModifySpellCooldown(rank, delta);
            return;
        }
    }
    else if (delta > 0)
    {
        player->ModifySpellCooldown(rank, delta);
        return;
    }
    player->RemoveSpellCooldown(rank, true);
    if (resetCategory)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(rank))
            if (uint32 category = info->GetCategory())
                player->RemoveCategoryCooldown(category);
}

// Runs an Ascension cooldown record (effect 165: MiscValue = spell, value = ms, MiscValueB = also
// the category) on the rank the player knows. The core handler would hit the literal rank-1 id:
// 520757 names 712326, 561055 names 712299, 520146 names 300578. The records are NEVER cast as
// well, or rank 1 would be shortened twice.
void ApplyCooldownRecord(Player* player, uint32 recordId)
{
    SpellInfo const* record = sSpellMgr->GetSpellInfo(recordId);
    if (!record)
        return;
    for (SpellEffectInfo const& effect : record->Effects)
        if (effect.Effect == SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN && effect.MiscValue > 0)
            ModifyRankCooldown(player, ResolveRank(player, uint32(effect.MiscValue)),
                effect.CalcValue(player), effect.MiscValueB != 0);
}

bool IsPalmSigil(uint32 id)
{
    // Every castable Palm Sigil, one by one (P-046: no chain lookup). 534803-534807 Fire,
    // 805380/807322/807323 Arcane, 805381 Frost, 805382 Earth, 807013 Wind, 500639 Water.
    return (id >= 534803 && id <= 534807) || id == 805380 || id == 807322 || id == 807323 ||
        id == 805381 || id == 805382 || id == 807013 || id == 500639;
}

bool SurgingSlashReady(Player* player)
{
    SpellInfo const* counter = sSpellMgr->GetSpellInfo(SPELL_SURGING_SLASH_COUNTER);
    Aura* aura = player->GetAura(SPELL_SURGING_SLASH_COUNTER, player->GetGUID());
    return counter && counter->StackAmount && aura && uint32(aura->GetStackAmount()) >= counter->StackAmount;
}

// Two temporary button swaps (the Reaper pattern, SyncReapTransform):
//   805742 up             -> Primordial Blast becomes 805794 Runic Obliteration;
//   572358 at full stacks -> Runeblade becomes 803736 Surging Slash.
// Targets are learned unannounced (P-144) and dropped with removeSpell(..., true), which only
// removes a PLAYERSPELL_TEMPORARY copy. A replacement is only cleared when it is ours.
void SyncTransforms(Player* player)
{
    bool leaving = !player->IsInWorld() || !player->GetSession() || player->GetSession()->PlayerLogout();
    bool alive = !leaving && player->IsAlive();
    struct Row { uint32 root; uint32 target; bool wanted; };
    Row const rows[] = {
        {SPELL_PRIMORDIAL_BLAST, SPELL_RUNIC_OBLITERATION_SPELL,
            alive && player->HasAura(SPELL_RUNIC_OBLITERATION_READY, player->GetGUID())},
        {SPELL_RUNEBLADE, SPELL_SURGING_SLASH_TRANSFORM, alive && SurgingSlashReady(player)}};
    for (Row const& row : rows)
    {
        uint32 rank = ActiveRank(player, row.root);
        bool wanted = row.wanted && sSpellMgr->GetSpellInfo(row.target);
        if (rank && wanted)
        {
            player->LearnTemporarySpellReplacementTarget(row.target);
            player->SetTemporarySpellReplacement(rank, row.target);
        }
        else if (rank && player->GetTemporarySpellReplacement(rank) == row.target)
            player->SetTemporarySpellReplacement(rank, 0);
        if (!leaving && !wanted)
            player->removeSpell(row.target, SPEC_MASK_ALL, true);
    }
}

// Deferred: the transformed spell is the one being cast; removing its window now would drop the
// temporary spell under the cast (same reason as the Reaper's Decimate).
void RemoveAuraDeferred(Player* player, uint32 auraId)
{
    player->m_Events.AddEventAtOffset([player, auraId]()
    {
        if (player->IsInWorld())
            player->RemoveAurasDueToSpell(auraId, player->GetGUID());
    }, 1ms);
}

// Firebrand keeps its duration on a new stack: "Additional applications do not refresh its
// duration". Shared by Fire Engraving and Decoder.
void ApplyFirebrand(Unit* player, Unit* target)
{
    if (Aura* brand = target->GetAura(SPELL_FIREBRAND, player->GetGUID()))
    {
        int32 const duration = brand->GetDuration();
        brand->ModStackAmount(1);
        brand->SetDuration(duration);
        return;
    }
    player->CastSpell(target, SPELL_FIREBRAND, true);
}

// Decoder 706523: "instantly trigger your Weapon Engravings". The payload of each engraving the
// player has up: Fire 653211 -> Firebrand, Water 653216 -> 653261, Earth 653221 -> 653272,
// Ice 653266 -> 653217, Arcane 653267 -> 653263. Air (653225 -> 653226) is left out: its payload
// replicates a share of a damage amount, and a trigger has none (681500/681528, the DBC helpers,
// would fire every payload whatever engraving is up).
void TriggerEngravings(Player* player, Unit* target)
{
    if (player->HasAura(SPELL_FIRE_ENGRAVING))
        ApplyFirebrand(player, target);
    std::pair<uint32, uint32> const engravings[] = {
        {653216, 653261}, {653221, 653272}, {653266, 653217}, {653267, 653263}};
    for (auto const& [carrier, payload] : engravings)
        if (player->HasAura(carrier) && sSpellMgr->GetSpellInfo(payload))
            player->CastSpell(target, payload, true);
}

void HandleTalentCasts(Player* player, Spell* spell, SpellInfo const* info, uint32 root)
{
    uint32 const id = info->Id;

    // Glyph of Disappearance 560037: "can be used 1 additional time within $561059d before
    // incurring a cooldown". First use: 561059 opens the window (its effect 195 resets Phase
    // Out natively) and the fresh cooldown goes. Use inside the window: the window closes and
    // the cooldown stays. 561059's own aura 42 (-> 561058, +90 s) has no proc row and stays inert.
    if (id == SPELL_PHASE_OUT && player->HasAura(SPELL_GLYPH_OF_DISAPPEARANCE))
    {
        if (player->HasAura(SPELL_DISAPPEARANCE_WINDOW, player->GetGUID()))
            player->RemoveAurasDueToSpell(SPELL_DISAPPEARANCE_WINDOW, player->GetGUID());
        else if (sSpellMgr->GetSpellInfo(SPELL_DISAPPEARANCE_WINDOW))
        {
            player->CastSpell(player, SPELL_DISAPPEARANCE_WINDOW, true);
            player->RemoveSpellCooldown(SPELL_PHASE_OUT, true);
        }
    }

    // Windsage 705568: "Smolder now envelops you in wind" -> 705569 (3 charges, spell_proc).
    if (root == SPELL_SMOLDER && player->HasAura(SPELL_WINDSAGE))
        CastOnSelf(player, SPELL_WINDSAGE_BUFF);

    // Elemental Inscription 806737: Smolder shortens Zenith and Primordial Fury (706522, read).
    if (root == SPELL_SMOLDER && player->HasAura(SPELL_ELEMENTAL_INSCRIPTION))
        ApplyCooldownRecord(player, SPELL_ELEMENTAL_INSCRIPTION_RECORD);

    // Eternal Magic 806698 (RUNIC spec, not the Riftblade branch above): Primordial Blast
    // refreshes $800192s1 Runeblade charges. RestoreSpellCharge caps at the maximum.
    if (root == SPELL_PRIMORDIAL_BLAST && player->HasAura(SPELL_ETERNAL_MAGIC))
        if (SpellInfo const* charges = sSpellMgr->GetSpellInfo(SPELL_ETERNAL_MAGIC_CHARGES))
            if (int32 count = charges->Effects[EFFECT_0].CalcValue(player); count > 0)
                if (uint32 rank = ActiveRank(player, SPELL_RUNEBLADE))
                    player->RestoreSpellCharge(rank, uint32(count));

    if (root == SPELL_RUNEBLADE || root == SPELL_PRIMORDIAL_BLAST)
    {
        // Ancient Warrior 520755: -2 s on Fist of the Ancients (520757) per use.
        if (player->HasAura(SPELL_ANCIENT_WARRIOR))
            ApplyCooldownRecord(player, SPELL_ANCIENT_WARRIOR_RECORD);
        // Power Overwhelming 707876: 35% -> next Runic Brand crits (560051), Runic Brand reset (561055).
        if (player->HasAura(SPELL_POWER_OVERWHELMING) && RollTalent(player, SPELL_POWER_OVERWHELMING))
        {
            CastOnSelf(player, SPELL_POWER_OVERWHELMING_CRIT);
            ApplyCooldownRecord(player, SPELL_POWER_OVERWHELMING_RECORD);
        }
    }

    // Leyline Magician 706823: Primordial Blast shortens Primordial Pulse (520146).
    if (root == SPELL_PRIMORDIAL_BLAST && player->HasAura(SPELL_LEYLINE_MAGICIAN))
        ApplyCooldownRecord(player, SPELL_LEYLINE_MAGICIAN_RECORD);

    // Elemental Carvings 705618: Fist of the Ancients unleashes one carving at random. 712327
    // (effect 184, TriggerSpell 7016 absent) is the DBC's own dead trigger, not cast.
    if (root == SPELL_FIST_OF_THE_ANCIENTS && player->HasAura(SPELL_ELEMENTAL_CARVINGS))
    {
        uint32 const carvings[] = {706520, 707142, 712300, 653253};
        CastOnSelf(player, carvings[urand(0, 3)]);
    }

    // Runic Obliteration 705560: Elemental Burst, 20% -> next Primordial Blast becomes 805794.
    if (root == SPELL_ELEMENTAL_BURST && player->HasAura(SPELL_RUNIC_OBLITERATION) &&
        RollTalent(player, SPELL_RUNIC_OBLITERATION))
        CastOnSelf(player, SPELL_RUNIC_OBLITERATION_READY);
    if (id == SPELL_RUNIC_OBLITERATION_SPELL)
        RemoveAuraDeferred(player, SPELL_RUNIC_OBLITERATION_READY);

    // Surging Slash 705579: every 3rd Runeblade turns the next one into 803736.
    if (root == SPELL_RUNEBLADE && player->HasAura(SPELL_SURGING_SLASH) &&
        sSpellMgr->GetSpellInfo(SPELL_SURGING_SLASH_COUNTER))
    {
        player->CastSpell(player, SPELL_SURGING_SLASH_COUNTER, true);
        if (SurgingSlashReady(player))
            SyncTransforms(player);
    }
    if (id == SPELL_SURGING_SLASH_TRANSFORM)
        RemoveAuraDeferred(player, SPELL_SURGING_SLASH_COUNTER);

    // Kirin Tor Agent 705634: Eye of the Beholder -> 706422 (its ticks: cooldown_percent script).
    if (id == SPELL_EYE_OF_THE_BEHOLDER && player->HasAura(SPELL_KIRIN_TOR_AGENT))
        CastOnSelf(player, SPELL_KIRIN_TOR_AGENT_AURA);

    // Ley Walker 500250: "Casting or reactivating Warpdagger" removes roots and snares.
    if ((id == SPELL_WARPDAGGER || id == SPELL_WARPDAGGER_TELEPORT) && player->HasAura(SPELL_LEY_WALKER))
        CastOnSelf(player, SPELL_LEY_WALKER_DISPEL);

    // Runebound Surge 707423: casting Runeshroud -> 707426.
    if (id == SPELL_RUNESHROUD && player->HasAura(SPELL_RUNEBOUND_SURGE))
        CastOnSelf(player, SPELL_RUNEBOUND_SURGE_BUFF);

    // Sigilist 705586 and Earthen Codex 707460: after a Palm Sigil.
    if (IsPalmSigil(id))
    {
        if (player->HasAura(SPELL_SIGILIST))
            CastOnSelf(player, SPELL_SIGILIST_BUFF);
        if (player->HasAura(SPELL_EARTHEN_CODEX))
            CastOnSelf(player, SPELL_EARTHEN_CODEX_BUFF);
    }

    // Leyfrost 712464 (from Hoarfrost crits, spell_proc 712308): "your NEXT spell" -- aura 262 has
    // no charge of its own, so the next spell its mask selects ends it.
    if (player->HasAura(SPELL_LEYFROST_WINDOW, player->GetGUID()))
        if (SpellInfo const* window = sSpellMgr->GetSpellInfo(SPELL_LEYFROST_WINDOW))
            if (info->SpellFamilyName == window->SpellFamilyName && window->Effects[EFFECT_0].SpellClassMask &&
                info->IsAffected(window->SpellFamilyName, window->Effects[EFFECT_0].SpellClassMask))
                player->RemoveAurasDueToSpell(SPELL_LEYFROST_WINDOW, player->GetGUID());

    (void)spell;
}

class runemaster_secondary_auras : public UnitScript
{
public:
    runemaster_secondary_auras() : UnitScript("runemaster_secondary_auras", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_MODIFY_SPELL_EFFECT_BASE_VALUE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !aura)
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(aura->GetId());
        if (root == SPELL_UNLEASHED_TATTOOS || root == SPELL_FIRE_TATTOO || root == SPELL_WATER_TATTOO)
            SyncUnleashed(player);
        if (aura->GetId() == SPELL_RUNIC_OBLITERATION_READY || aura->GetId() == SPELL_SURGING_SLASH_COUNTER)
            SyncTransforms(player);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !application)
            return;
        Aura* aura = application->GetBase();
        uint32 root = sSpellMgr->GetFirstSpellInChain(aura->GetId());
        if (root == SPELL_UNLEASHED_TATTOOS || root == SPELL_FIRE_TATTOO || root == SPELL_WATER_TATTOO)
            SyncUnleashed(player);
        if (aura->GetCasterGUID() != player->GetGUID())
            return;
        if (aura->GetId() == SPELL_RUNIC_OBLITERATION_READY || aura->GetId() == SPELL_SURGING_SLASH_COUNTER)
            SyncTransforms(player);
        if (root == SPELL_RIFTBLADE)
            player->RemoveAurasDueToSpell(SPELL_RIFTBLADE_COUNTER, player->GetGUID());
        if (root == SPELL_SPELLFIRE_RUNES)
            player->RemoveAurasDueToSpell(SPELL_SPELLFIRE_READY, player->GetGUID());
    }

    void ModifySpellEffectBaseValue(Unit const* caster, SpellInfo const* info, uint8 index, float& value) override
    {
        Player* player = caster ? const_cast<Unit*>(caster)->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || info->Id != SPELL_RIFTBLADE_MANA || index != EFFECT_0)
            return;
        value += std::max(0.0f, player->GetTotalAttackPowerValue(BASE_ATTACK)) * 0.3f;
        if (HasTattoo(player, SPELL_WATER_TATTOO))
            if (SpellInfo const* reference = sSpellMgr->GetSpellInfo(SPELL_WATER_AMOUNT))
                AddPct(value, reference->Effects[EFFECT_2].CalcValue(player));
    }

private:
    static void SyncUnleashed(Player* player)
    {
        bool active = player->IsAlive() && player->HasAura(SPELL_UNLEASHED_TATTOOS, player->GetGUID());
        for (auto const& [tattoo, helper] : {std::pair{SPELL_FIRE_TATTOO, SPELL_UNLEASHED_FIRE},
            std::pair{SPELL_WATER_TATTOO, SPELL_UNLEASHED_WATER}})
            if (!active || !HasTattoo(player, tattoo))
                player->RemoveAurasDueToSpell(helper, player->GetGUID());
            else if (!player->HasAura(helper, player->GetGUID()))
                player->AddAura(helper, player);
    }
};

class runemaster_secondary_casts : public AllSpellScript
{
public:
    runemaster_secondary_casts() : AllSpellScript("runemaster_secondary_casts",
        {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !player->IsAlive() || spell->IsTriggered())
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(info->Id);
        if (root == SPELL_SMOLDER)
            player->RemoveAurasDueToSpell(SPELL_SPELLFIRE_READY, player->GetGUID());
        if (player->HasAura(SPELL_RIFTBLADE, player->GetGUID()))
        {
            if (root == SPELL_PRIMORDIAL_BLAST || root == SPELL_SMOLDER)
            {
                if (uint32 rank = KnownRank(player, SPELL_RUNEBLADE))
                    player->RestoreSpellCharge(rank);
            }
            else if (root == SPELL_RUNEBLADE)
            {
                player->CastSpell(player, SPELL_RIFTBLADE_COUNTER, true);
                if (Aura* counter = player->GetAura(SPELL_RIFTBLADE_COUNTER, player->GetGUID());
                    counter && counter->GetStackAmount() >= 3)
                {
                    player->RemoveAurasDueToSpell(SPELL_RIFTBLADE_COUNTER, player->GetGUID());
                    player->CastSpell(player, SPELL_RIFTBLADE_MANA, true);
                }
            }
        }
        if ((root == SPELL_RUNEBLADE || root == SPELL_WARPDAGGER) &&
            player->HasAura(SPELL_SPELLFIRE_RUNES, player->GetGUID()))
        {
            SpellInfo const* reference = sSpellMgr->GetSpellInfo(root == SPELL_RUNEBLADE
                ? SPELL_SPELLFIRE_CHANCE : SPELL_SPELLFIRE_RUNES);
            if (reference && roll_chance_i(reference->ProcChance))
            {
                if (uint32 rank = KnownRank(player, SPELL_SMOLDER))
                    player->RemoveSpellCooldown(rank, true);
                player->AddAura(SPELL_SPELLFIRE_READY, player);
            }
        }
        HandleTalentCasts(player, spell, info, root);
    }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32 damage, uint32, bool) override
    {
        Player* player = spell->GetCaster()->ToPlayer();
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !player->IsAlive() || !player->IsInWorld() ||
            spell->IsTriggered() || !damage ||
            miss != SPELL_MISS_NONE || !target || !target->IsAlive() || target == player ||
            player->IsFriendlyTo(target))
            return;
        uint32 root = sSpellMgr->GetFirstSpellInChain(spell->GetSpellInfo()->Id);
        if (root == SPELL_RUNEBLADE)
            FistsOfPower(player, spell, target, damage);

        // Cold Elements 520138: "Damage dealt with Elemental Burst and Primordial Blast" -- once per
        // cast. 802648 carries the instant cast (one charge, metadata) and, natively, a reset of
        // rank 1; the record then resets the rank the player really has (a reset is idempotent).
        if ((root == SPELL_ELEMENTAL_BURST || root == SPELL_PRIMORDIAL_BLAST) &&
            player->HasAura(SPELL_COLD_ELEMENTS) && !spell->GetScriptValue(SPELL_COLD_ELEMENTS))
        {
            spell->SetScriptValue(SPELL_COLD_ELEMENTS, 1);
            if (RollTalent(player, SPELL_COLD_ELEMENTS))
            {
                CastOnSelf(player, SPELL_COLD_ELEMENTS_BUFF);
                ApplyCooldownRecord(player, SPELL_COLD_ELEMENTS_BUFF);
            }
        }

        // Decoder 706523: Elemental Burst and Runeblade, 20%, once per cast.
        if ((root == SPELL_ELEMENTAL_BURST || root == SPELL_RUNEBLADE) && player->HasAura(SPELL_DECODER) &&
            !spell->GetScriptValue(SPELL_DECODER))
        {
            spell->SetScriptValue(SPELL_DECODER, 1);
            if (RollTalent(player, SPELL_DECODER))
                TriggerEngravings(player, target);
        }

        // Burned Etching 500476: while Weapon Engraving: Fire is up, Elemental Burst deals
        // $500475s1% more as Fire (500474). The +5% Firebrand chance is in the Fire Engraving roll.
        if (root == SPELL_ELEMENTAL_BURST && player->HasAura(SPELL_BURNED_ETCHING) &&
            player->HasAura(SPELL_FIRE_ENGRAVING))
            if (SpellInfo const* values = sSpellMgr->GetSpellInfo(SPELL_BURNED_ETCHING_VALUES))
                if (sSpellMgr->GetSpellInfo(SPELL_BURNED_ETCHING_DAMAGE))
                {
                    uint64 amount = uint64(damage) *
                        uint64(std::clamp(values->Effects[EFFECT_0].CalcValue(player), 0, 100)) / 100;
                    if (amount)
                        player->CastCustomSpell(SPELL_BURNED_ETCHING_DAMAGE, SPELLVALUE_BASE_POINT0,
                            int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())), target,
                            TRIGGERED_FULL_MASK);
                }
    }

private:
    static void FistsOfPower(Player* player, Spell* spell, Unit* target, uint32 damage)
    {
        if (!player->GetWeaponForAttack(OFF_ATTACK, true) ||
            !player->HasAura(SPELL_FISTS_OF_POWER, player->GetGUID()) || spell->GetScriptValue(SPELL_FISTS_HIT))
            return;
        SpellInfo const* reference = sSpellMgr->GetSpellInfo(SPELL_FISTS_AMOUNT);
        SpellInfo const* helper = sSpellMgr->GetSpellInfo(SPELL_FISTS_HIT);
        if (!reference || !helper)
            return;
        spell->SetScriptValue(SPELL_FISTS_HIT, 1);
        uint64 amount = uint64(damage) * std::clamp(reference->Effects[EFFECT_0].CalcValue(player), 0, 100) / 100;
        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(std::min<uint64>(amount, std::numeric_limits<int32>::max())));
        values.AddSpellMod(SPELLVALUE_MELEE_ATTACK_TYPE, OFF_ATTACK);
        player->CastSpell(targets, helper, &values, TRIGGERED_FULL_MASK);
    }
};

// Palm Sigils: "your next instance of direct Magic damage". Shared test of the three sigils
// whose effect 0 is Ascension's private aura 354 (no core handler): Arcane, Frost, Water.
bool PalmSigilCheck(Unit* player, Unit* caster, ProcEventInfo& event)
{
    DamageInfo const* damage = event.GetDamageInfo();
    Unit* target = event.GetActionTarget();
    return player->IsPlayer() && player->getClass() == CLASS_SPIRIT_MAGE && player->IsAlive() &&
        caster == player && event.GetActor() == player && target && target->IsAlive() &&
        target != player && !player->IsFriendlyTo(target) && damage && damage->GetDamage() &&
        damage->GetDamageType() == SPELL_DIRECT_DAMAGE && (damage->GetSchoolMask() & SPELL_SCHOOL_MASK_MAGIC);
}

int32 PalmSigilAmount(ProcEventInfo& event, AuraEffect const* effect)
{
    uint64 amount = uint64(event.GetDamageInfo()->GetDamage()) * uint64(std::clamp(effect->GetAmount(), 0, 100)) / 100;
    return int32(std::min<uint64>(amount, std::numeric_limits<int32>::max()));
}

// Consumption is the proc entry's single charge (spell_proc Charges 1), not a Remove(): Elements
// of Fury 707413 (SPELLMOD_CHARGES +1) then really gives a second hit. The payloads are cast with
// the aura effect as origin, so Aura::GetProcEffectMask refuses to let them re-trigger the sigil.
class aura_ascension_arcane_palm_sigil : public AuraScript
{
    PrepareAuraScript(aura_ascension_arcane_palm_sigil);

    bool Check(ProcEventInfo& event) { return PalmSigilCheck(GetTarget(), GetCaster(), event); }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Unit* player = GetTarget();
        Unit* target = event.GetActionTarget();
        player->CastCustomSpell(SPELL_ARCANE_SIGIL_DOT, SPELLVALUE_BASE_POINT0, PalmSigilAmount(event, effect),
            target, TRIGGERED_FULL_MASK, nullptr, effect);
        player->CastSpell(target, SPELL_ARCANE_SIGIL_SILENCE, true, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_arcane_palm_sigil::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_arcane_palm_sigil::Proc, EFFECT_0, AuraType(354));
    }
};

// Palm Sigil: Frost 805381: "$s1% of the damage dealt as Frost damage and slow". 807825 deals the
// damage and, by its own effect 1, applies the 808022 slow; the sigil's aura 42 (effect 1, also
// 808022) is disabled in its proc entry so the slow is not applied twice.
class aura_ascension_frost_palm_sigil : public AuraScript
{
    PrepareAuraScript(aura_ascension_frost_palm_sigil);

    bool Check(ProcEventInfo& event) { return PalmSigilCheck(GetTarget(), GetCaster(), event); }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        GetTarget()->CastCustomSpell(SPELL_FROST_SIGIL_DAMAGE, SPELLVALUE_BASE_POINT0, PalmSigilAmount(event, effect),
            event.GetActionTarget(), TRIGGERED_FULL_MASK, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_frost_palm_sigil::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_frost_palm_sigil::Proc, EFFECT_0, AuraType(354));
    }
};

// Palm Sigil: Water 500639: "drain the target's mana equal to $s1% of the damage dealt" (807818,
// POWER_DRAIN, base value set to the amount).
class aura_ascension_runemaster_water_palm_sigil : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_water_palm_sigil);

    bool Check(ProcEventInfo& event) { return PalmSigilCheck(GetTarget(), GetCaster(), event); }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        if (int32 amount = PalmSigilAmount(event, effect))
            GetTarget()->CastCustomSpell(SPELL_WATER_SIGIL_DRAIN, SPELLVALUE_BASE_POINT0, amount,
                event.GetActionTarget(), TRIGGERED_FULL_MASK, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_water_palm_sigil::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_runemaster_water_palm_sigil::Proc, EFFECT_0, AuraType(354));
    }
};

// Fire Engraving (equip spell of enchant 1000): direct damage has a 30% chance to apply Firebrand.
// The proc flags come from spell_proc; this script keeps the existing Firebrand's duration, since
// "Additional applications do not refresh its duration".
//
// THE ROLL LIVES HERE, not in the proc entry. Expansive Engraver (807496) says "While Air Engraving
// is active, Fire Engraving's trigger chance is increased by $s1 percentage points" — a chance that
// depends on ANOTHER aura being up. A spell modifier cannot express that: it applies whenever its
// own aura is present, with no second condition. 807496 authors no modifier either; its three
// effects are DUMMY. So `spell_proc` is set to Chance 100 and the real roll is below.
// COUPLING, deliberate and one-way: with the row at 100 and this script missing, Fire would proc on
// every hit. The companion SQL must therefore be applied AFTER the binary that carries this code.
class aura_ascension_runemaster_fire_engraving : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_fire_engraving);

    bool Check(ProcEventInfo& event)
    {
        Unit* player = GetTarget();
        DamageInfo const* damage = event.GetDamageInfo();
        Unit* target = event.GetActionTarget();
        if (!player->IsPlayer() || player->getClass() != CLASS_SPIRIT_MAGE || event.GetActor() != player ||
            !target || target == player || !target->IsAlive() || !damage || !damage->GetDamage() ||
            damage->GetDamageType() == DOT)
            return false;
        // Base chance from the DBC, never from the proc entry: the entry is pinned at 100 so that
        // this roll is the only one. Reading the DBC keeps the tooltip and the code on one number.
        SpellInfo const* engraving = GetSpellInfo();
        int32 chance = int32(engraving->ProcChance);
        // The core's own roll (Aura::CalcProcChance) applies SPELLMOD_CHANCE_OF_SUCCESS, but to the
        // entry's 100, where it changes nothing. Applied here, on the real base, it carries Zenith
        // 712325/712389, Master Engraver 705605, Volcanic Etching 705615 (all FLAT +chance on
        // 653211). Without a Spell* argument ApplySpellMod falls back on m_spellModTakingSpell
        // (Player.cpp:10338): a charged +chance mod on 653211 would be consumed there. None exists today.
        player->ToPlayer()->ApplySpellMod(engraving->Id, SPELLMOD_CHANCE_OF_SUCCESS, chance);
        if (player->HasAura(SPELL_EXPANSIVE_ENGRAVER) && player->HasAura(SPELL_AIR_ENGRAVING_PASSIVE))
            if (SpellInfo const* talent = sSpellMgr->GetSpellInfo(SPELL_EXPANSIVE_ENGRAVER))
                chance += talent->Effects[EFFECT_0].CalcValue(player);
        // Burned Etching 500476: "your chance to trigger Weapon Engraving: Fire is increased by
        // $500475s2%". 500475 is never applied (its aura 354 has no handler); its value is read.
        if (player->HasAura(SPELL_BURNED_ETCHING))
            if (SpellInfo const* values = sSpellMgr->GetSpellInfo(SPELL_BURNED_ETCHING_VALUES))
                chance += values->Effects[EFFECT_1].CalcValue(player);
        return roll_chance_i(std::clamp(chance, 0, 100));
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& event)
    {
        PreventDefaultAction();
        ApplyFirebrand(GetTarget(), event.GetActionTarget());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_fire_engraving::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_runemaster_fire_engraving::Proc, EFFECT_0,
                                         SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Firebrand: "When this effect expires, it explodes, dealing ... Fire damage per stack."
class aura_ascension_runemaster_firebrand : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_firebrand);

    void Explode(AuraEffect const* /*effect*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE || !caster || !target->IsAlive())
            return;
        for (uint8 stack = GetStackAmount(); stack > 0; --stack)
            caster->CastSpell(target, SPELL_FIREBRAND_EXPLOSION, true);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_runemaster_firebrand::Explode, EFFECT_0,
                                                SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// Windsage 705569 (from Smolder): "your next 3 Runeblades strike an additional time equal to
// $705569s1% of the damage dealt as Nature damage". Effect 0 is aura 354 (no core handler); the
// three uses are the proc entry's Charges 3.
class aura_ascension_runemaster_windsage : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_windsage);

    bool Check(ProcEventInfo& event)
    {
        Unit* player = GetTarget();
        DamageInfo const* damage = event.GetDamageInfo();
        Unit* target = event.GetActionTarget();
        SpellInfo const* info = event.GetSpellInfo();
        return player->IsPlayer() && player->getClass() == CLASS_SPIRIT_MAGE && player->IsAlive() &&
            event.GetActor() == player && info && sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_RUNEBLADE &&
            target && target != player && target->IsAlive() && !player->IsFriendlyTo(target) &&
            damage && damage->GetDamage() && damage->GetDamageType() != DOT;
    }

    void Proc(AuraEffect const* effect, ProcEventInfo& event)
    {
        PreventDefaultAction();
        if (int32 amount = PalmSigilAmount(event, effect))
            GetTarget()->CastCustomSpell(SPELL_WINDSAGE_STRIKE, SPELLVALUE_BASE_POINT0, amount,
                event.GetActionTarget(), TRIGGERED_FULL_MASK, nullptr, effect);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_windsage::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_runemaster_windsage::Proc, EFFECT_0, AuraType(354));
    }
};

// Primordial Echoes 705572: "critical strikes dealt with Primordial Blast have a $h% chance to
// strike an additional time". The DBC path (547525, effect 183 after bp 500 ms) always relaunches
// rank 6 (502827) and the family mask also selects Stratus, Ignis, Lithos, Hydros and Runic
// Obliteration. Here: only the Primordial Blast chain, and the rank that actually crit, after the
// delay 547525 carries.
class aura_ascension_runemaster_primordial_echoes : public AuraScript
{
    PrepareAuraScript(aura_ascension_runemaster_primordial_echoes);

    bool Check(ProcEventInfo& event)
    {
        Unit* player = GetTarget();
        SpellInfo const* info = event.GetSpellInfo();
        Unit* target = event.GetActionTarget();
        return player->IsPlayer() && player->getClass() == CLASS_SPIRIT_MAGE && player->IsAlive() &&
            event.GetActor() == player && info && sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_PRIMORDIAL_BLAST &&
            target && target != player && target->IsAlive();
    }

    void Proc(AuraEffect const* /*effect*/, ProcEventInfo& event)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;
        uint32 const spellId = event.GetSpellInfo()->Id;
        ObjectGuid const targetGuid = event.GetActionTarget()->GetGUID();
        int32 delay = 500;
        if (SpellInfo const* trigger = sSpellMgr->GetSpellInfo(547525))
            delay = std::max(0, trigger->Effects[EFFECT_0].CalcValue());
        player->m_Events.AddEventAtOffset([player, spellId, targetGuid]()
        {
            if (!player->IsInWorld() || !player->IsAlive())
                return;
            Unit* target = ObjectAccessor::GetUnit(*player, targetGuid);
            if (!target || !target->IsAlive() || !player->IsValidAttackTarget(target))
                return;
            player->CastSpell(target, spellId, true);
        }, Milliseconds(delay));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_runemaster_primordial_echoes::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_runemaster_primordial_echoes::Proc, EFFECT_0,
                                         SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Blade Rift 520237: "While stealthed, using Warpdagger increases your Magic Damage dealt". The
// stealth is read before the cast: the cast itself may end Runeshroud before any after-cast hook.
class spell_ascension_runemaster_blade_rift : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_blade_rift);

    bool _stealthed = false;

    void Remember()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        _stealthed = player && player->getClass() == CLASS_SPIRIT_MAGE && !GetSpell()->IsTriggered() &&
            player->HasAura(SPELL_BLADE_RIFT) && player->HasAura(SPELL_RUNESHROUD, player->GetGUID());
    }

    void Grant()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (_stealthed && player && player->IsAlive())
            CastOnSelf(player, SPELL_BLADE_RIFT_BUFF);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_ascension_runemaster_blade_rift::Remember);
        AfterCast += SpellCastFn(spell_ascension_runemaster_blade_rift::Grant);
    }
};

// Earth, Wind, and Fire 706531: "Striking at least 5 enemies with Runic Explosion (712324) now
// unleashes Wild Steam". The 5 is 707458's StackAmount (the DBC's dummy counter). Counted once
// per cast in the target selection, cast after it.
class spell_ascension_runemaster_runic_explosion : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_runic_explosion);

    bool _unleash = false;

    void Count(std::list<WorldObject*>& targets)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        SpellInfo const* stacker = sSpellMgr->GetSpellInfo(SPELL_EARTH_WIND_AND_FIRE_STACKER);
        _unleash = player && player->getClass() == CLASS_SPIRIT_MAGE && player->IsAlive() &&
            player->HasAura(SPELL_EARTH_WIND_AND_FIRE) && stacker && stacker->StackAmount &&
            targets.size() >= stacker->StackAmount;
    }

    void Unleash()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (_unleash && player && player->IsAlive())
            CastOnSelf(player, SPELL_WILD_STEAM);
        _unleash = false;
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_ascension_runemaster_runic_explosion::Count,
            EFFECT_0, TARGET_UNIT_DEST_AREA_ENEMY);
        AfterCast += SpellCastFn(spell_ascension_runemaster_runic_explosion::Unleash);
    }
};

// Kirin Tor Agent ticks (807980, 807629, 807979, from 706422) and Protective Warding 800753: each
// effect is Ascension's private effect 192 (EffectNULL in the core), MiscValue = the spell,
// value = percent. SUPPOSED meaning: the percent of that spell's full cooldown, taken off what
// remains. The rank is resolved; a spell the player does not know is skipped.
class spell_ascension_runemaster_cooldown_percent : public SpellScript
{
    PrepareSpellScript(spell_ascension_runemaster_cooldown_percent);

    void Handle(SpellEffIndex effIndex)
    {
        Player* player = GetHitUnit() ? GetHitUnit()->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || GetCaster() != player)
            return;
        SpellEffectInfo const& effect = GetSpellInfo()->Effects[effIndex];
        if (effect.Effect != 192 || effect.MiscValue <= 0)
            return;
        uint32 rank = ResolveRank(player, uint32(effect.MiscValue));
        SpellInfo const* info = rank ? sSpellMgr->GetSpellInfo(rank) : nullptr;
        if (!info)
            return;
        uint64 full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        uint64 cut = full * uint64(std::clamp(effect.CalcValue(player), 0, 100)) / 100;
        if (cut)
            ModifyRankCooldown(player, rank, -int32(std::min<uint64>(cut, std::numeric_limits<int32>::max())), false);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_runemaster_cooldown_percent::Handle, EFFECT_ALL,
            SPELL_EFFECT_ANY);
    }
};

class runemaster_secondary_metadata : public GlobalScript
{
public:
    runemaster_secondary_metadata() : GlobalScript("runemaster_secondary_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->SpellFamilyName != 38)
            return;
        // Payloads whose amount is already resolved from a damage dealt: no spell power again,
        // no crit of their own, no second pass of the caster's or the target's modifiers.
        if (info->Id == SPELL_FISTS_HIT || info->Id == SPELL_ARCANE_SIGIL_DOT ||
            info->Id == SPELL_FROST_SIGIL_DAMAGE || info->Id == SPELL_WATER_SIGIL_DRAIN ||
            info->Id == SPELL_WINDSAGE_STRIKE || info->Id == SPELL_BURNED_ETCHING_DAMAGE)
        {
            info->AttributesEx2 |= SPELL_ATTR2_CANT_CRIT;
            info->AttributesEx3 |= SPELL_ATTR3_IGNORE_CASTER_MODIFIERS;
            info->AttributesEx4 |= SPELL_ATTR4_IGNORE_DAMAGE_TAKEN_MODIFIERS;
            info->AscensionInheritsResolvedAmount = true;
            info->Effects[EFFECT_0].BonusMultiplier = 0.0f;
        }
        if (info->Id == SPELL_UNLEASHED_FIRE || info->Id == SPELL_UNLEASHED_WATER ||
            info->Id == SPELL_SPELLFIRE_READY || info->Id == SPELL_RIFTBLADE_COUNTER ||
            info->Id == SPELL_DISAPPEARANCE_WINDOW || info->Id == SPELL_RUNIC_OBLITERATION_READY ||
            info->Id == SPELL_SURGING_SLASH_COUNTER)
        {
            info->AttributesCu &= ~SPELL_ATTR0_CU_FORCE_AURA_SAVING;
            info->AttributesCu |= SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED;
        }
        // Cold Elements 802648: "Next Coldrune Blast is instant cast" -- one use. ProcCharges 0
        // would let the -100% cast time ride every Frigid Blast for the whole 10 s. It has no
        // spell_proc row, so the charged modifier is dropped natively after the cast that used it.
        if (info->Id == SPELL_COLD_ELEMENTS_BUFF && !info->ProcCharges)
            info->ProcCharges = 1;
        // Power Overwhelming 560051: "Your next Runic Brand is guaranteed to critically strike".
        // DurationIndex 0 makes the aura vanish on the tick it lands; it lasts until used instead
        // (row 21, the passives' -1). SUPPOSED: two charges, because one Runic Brand is two
        // 712322 hits (712299's two effect-142 triggers, one per weapon).
        if (info->Id == SPELL_POWER_OVERWHELMING_CRIT && !info->GetDuration())
        {
            if (SpellDurationEntry const* duration = sSpellDurationStore.LookupEntry(21))
                info->DurationEntry = duration;
            if (!info->ProcCharges)
                info->ProcCharges = 2;
        }
    }
};
}

void AddSC_AscensionRunemasterSecondary()
{
    new runemaster_secondary_auras();
    new runemaster_secondary_casts();
    new runemaster_secondary_metadata();
    RegisterSpellScript(aura_ascension_arcane_palm_sigil);
    RegisterSpellScript(aura_ascension_frost_palm_sigil);
    RegisterSpellScript(aura_ascension_runemaster_water_palm_sigil);
    RegisterSpellScript(aura_ascension_runemaster_fire_engraving);
    RegisterSpellScript(aura_ascension_runemaster_firebrand);
    RegisterSpellScript(aura_ascension_runemaster_windsage);
    RegisterSpellScript(aura_ascension_runemaster_primordial_echoes);
    RegisterSpellScript(spell_ascension_runemaster_blade_rift);
    RegisterSpellScript(spell_ascension_runemaster_runic_explosion);
    RegisterSpellScript(spell_ascension_runemaster_cooldown_percent);
}
