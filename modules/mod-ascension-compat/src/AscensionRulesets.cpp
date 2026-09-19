/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "Config.h"
#include "Chat.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"

#include <string>
#include <vector>

namespace
{
enum RulesetSpells : uint32
{
    SPELL_SELECT_WAR_MODE = 84420,
    SPELL_SELECT_HIGH_RISK = 84421,
    SPELL_SELECT_PVE = 84422,
    SPELL_HIGH_RISK = 1004019,
    SPELL_WAR_MODE = 1004119,
    SPELL_PVE = 9931032
};

// Applies the aura set a selection spell stands for, without running its cast requirements.
void ApplyRuleset(Player* player, uint32 selectionId)
{
    player->RemoveAurasDueToSpell(SPELL_HIGH_RISK);
    player->RemoveAurasDueToSpell(SPELL_WAR_MODE);
    player->RemoveAurasDueToSpell(SPELL_PVE);
    if (selectionId == SPELL_SELECT_HIGH_RISK)
        player->CastSpell(player, SPELL_HIGH_RISK, true);
    else
    {
        // C_Player:GetRuleset distinguishes PvE by this additional marker.
        player->CastSpell(player, SPELL_WAR_MODE, true);
        if (selectionId == SPELL_SELECT_PVE)
            player->CastSpell(player, SPELL_PVE, true);
    }
}

class spell_ascension_ruleset_select : public SpellScript
{
    PrepareSpellScript(spell_ascension_ruleset_select);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_HIGH_RISK, SPELL_WAR_MODE, SPELL_PVE});
    }

    bool Load() override { return GetCaster()->ToPlayer() != nullptr; }

    SpellCastResult CheckCast()
    {
        // The selection spell descriptions require a rested area, including inns.
        Player* player = GetCaster()->ToPlayer();
        return player && player->HasPlayerFlag(PLAYER_FLAGS_RESTING) ? SPELL_CAST_OK : SPELL_FAILED_NOT_HERE;
    }

    void Select(SpellEffIndex)
    {
        Player* player = GetCaster()->ToPlayer();
        uint32 id = GetSpellInfo()->Id;
        if (!player || (id != SPELL_SELECT_WAR_MODE && id != SPELL_SELECT_HIGH_RISK && id != SPELL_SELECT_PVE))
            return;

        ApplyRuleset(player, id);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_ruleset_select::CheckCast);
        OnEffectHit += SpellEffectFn(spell_ascension_ruleset_select::Select, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

class ruleset_aura_metadata : public GlobalScript
{
public:
    ruleset_aura_metadata() : GlobalScript("ruleset_aura_metadata", {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (info->Id == SPELL_HIGH_RISK || info->Id == SPELL_WAR_MODE || info->Id == SPELL_PVE)
            info->AttributesEx3 |= SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD;
    }
};

// Makes the High Risk ruleset mean something on the server.
//
// The three rulesets are a player-facing choice already: War Mode, High Risk
// and PvE, picked from the client's frame in a rested area. Until now their
// auras were pure markers — the character selection screen reads 1004019 to
// draw the flag, and nothing else in the server looks at any of them.
//
// High Risk now carries the FFA flag. Between two player-controlled units the
// core only allows an attack when the target is PvP flagged, or when both are
// FFA (Unit::_IsValidAttackTarget), and Unit::GetReactionTo returns REP_HOSTILE
// for two FFA units. So a High Risk character can be fought by anyone else in
// High Risk, of either faction, which is what the ruleset name promises.
//
// The flag is re-asserted on the player tick rather than set once, because
// Player::UpdateArea recomputes it from area flags on every zone change and the
// OnPlayerUpdateArea hook fires *before* that recomputation. Crossing an area
// boundary therefore drops the flag for one tick.
//
// Sanctuaries and friendly capital cities are spared without a line of code:
// UpdateFFAPvPState refuses the flag wherever pvpInfo.IsInNoPvPArea is set, and
// the core sets it for those areas.
// The price of High Risk: dying in the open world costs you something.
//
// The selection spell promises it in as many words: "Any death while in the open
// world with High-Risk Mode active will cause you to drop equipped gear, or Fel
// Com gold if your gear is insured, and items from your bag." Nothing enforced
// any of it, so until now High Risk was an advantage with no matching risk —
// the opposite of what its name sells.
//
// Three rules, in this order:
//
//   * A level gap of HighRiskDeathLevelGap or more costs nothing. Being ganked
//     by someone far above you, or squashing someone far below you, is not the
//     fight the mode is about. This protects the newcomer and makes ganking
//     pointless at the same time.
//   * Insurance first. The player pays gold scaled to the item level of what
//     they are wearing, and keeps every piece. Better gear, higher premium:
//     the rule stays meaningful at 80 without being crushing at 11.
//   * Uninsured — meaning unable to pay — loses equipment instead.
//
// The insurance is automatic rather than bought in advance. There is no client
// frame to sell a policy from, and an opt-in the player cannot see is an opt-in
// nobody uses. Paying when you can and bleeding when you cannot is the same
// bargain, minus the interface we do not have.
namespace HighRisk
{
bool PenaltyApplies(Player* player)
{
    if (!player || !player->IsInWorld() || player->IsGameMaster())
        return false;
    if (!player->HasAura(SPELL_HIGH_RISK))
        return false;
    // Open world only, as the spell says: no battlegrounds, no arenas, no
    // instances, and nothing inside a sanctuary.
    if (player->InBattleground() || player->InArena() || player->pvpInfo.IsInNoPvPArea)
        return false;
    Map const* map = player->GetMap();
    return map && !map->IsDungeon() && !map->IsBattlegroundOrArena();
}

// Premium in copper, from the item level of everything worn. An empty set costs
// nothing, which is the honest answer: there is nothing to insure.
uint32 Premium(Player* player)
{
    uint32 levels = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (ItemTemplate const* proto = item->GetTemplate())
                levels += proto->ItemLevel;

    uint32 perLevel = sConfigMgr->GetOption<uint32>("AscensionCompat.HighRiskPremiumPerItemLevel", 100);
    return levels * perLevel;
}

// Copper rendered the way the client does it, dropping the units that are zero:
// "6 silver", not "0 gold 6 silver 0 copper".
std::string MoneyText(uint32 copper)
{
    uint32 g = copper / 10000, s = (copper % 10000) / 100, c = copper % 100;
    std::string out;
    if (g) out += std::to_string(g) + " gold";
    if (s) out += (out.empty() ? "" : " ") + std::to_string(s) + " silver";
    if (c || out.empty()) out += (out.empty() ? "" : " ") + std::to_string(c) + " copper";
    return out;
}

void Apply(Player* victim, uint8 killerLevel)
{
    if (!sConfigMgr->GetOption<bool>("AscensionCompat.HighRiskDeathPenalty", true))
        return;
    if (!PenaltyApplies(victim))
        return;

    uint32 gap = sConfigMgr->GetOption<uint32>("AscensionCompat.HighRiskDeathLevelGap", 4);
    if (gap && killerLevel)
    {
        int32 diff = int32(victim->GetLevel()) - int32(killerLevel);
        if (uint32(std::abs(diff)) >= gap)
        {
            ChatHandler(victim->GetSession()).PSendSysMessage(
                "High Risk: no loss, the level gap was too wide.");
            return;
        }
    }

    uint32 premium = Premium(victim);
    if (premium && victim->GetMoney() >= premium)
    {
        victim->ModifyMoney(-int32(premium), false);
        ChatHandler(victim->GetSession()).PSendSysMessage(
            "High Risk: your gear was insured. Premium paid: {}.",
            MoneyText(premium));
        return;
    }

    // Cannot pay: equipment answers for it. One piece at random, so the loss is
    // real without emptying a character in a single death.
    std::vector<uint8> worn;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            worn.push_back(slot);

    if (worn.empty())
    {
        ChatHandler(victim->GetSession()).PSendSysMessage(
            "High Risk: nothing to insure and nothing to lose.");
        return;
    }

    uint8 slot = worn[urand(0, worn.size() - 1)];
    std::string name = "a piece of equipment";
    if (Item const* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        if (ItemTemplate const* proto = item->GetTemplate())
            name = proto->Name1;

    // KNOWN LIMITATION: the piece is destroyed, not handed to the killer.
    // Moving it onto the corpse needs the corpse loot path, which is a system of
    // its own; this keeps the risk real without pretending to be that system.
    victim->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    ChatHandler(victim->GetSession()).PSendSysMessage(
        "High Risk: you could not cover the {} premium. You lost {}.",
        MoneyText(premium), name);
}
}

class ruleset_high_risk_death : public PlayerScript
{
public:
    ruleset_high_risk_death() : PlayerScript("ruleset_high_risk_death",
        {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE}) { }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        HighRisk::Apply(killed, killer ? killer->GetLevel() : 0);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        HighRisk::Apply(killed, killer ? killer->GetLevel() : 0);
    }
};

class ruleset_high_risk_ffa : public PlayerScript
{
public:
    ruleset_high_risk_ffa() : PlayerScript("ruleset_high_risk_ffa", {PLAYERHOOK_ON_UPDATE}) { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!sConfigMgr->GetOption<bool>("AscensionCompat.HighRiskIsFFA", true))
            return;
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            return;

        bool highRisk = player->HasAura(SPELL_HIGH_RISK);
        if (highRisk == player->pvpInfo.IsInFFAPvPArea)
            return;

        // Setting IsInFFAPvPArea is a deliberate abuse of that field, and it is
        // contained: the core reads it only in UpdateFFAPvPState and
        // SetFFAPvPTimer. Holding it true also keeps the 30 second unflag timer
        // from starting, which is what a standing ruleset needs.
        player->pvpInfo.IsInFFAPvPArea = highRisk;
        player->UpdateFFAPvPState(false);
    }
};

class ruleset_player_spells : public PlayerScript
{
public:
    ruleset_player_spells() : PlayerScript("ruleset_player_spells", {PLAYERHOOK_ON_LOGIN}) { }

    void OnPlayerLogin(Player* player) override
    {
        // CastSpellByID still requires these UI actions to be known on the server.
        for (uint32 id : {SPELL_SELECT_WAR_MODE, SPELL_SELECT_HIGH_RISK, SPELL_SELECT_PVE})
            if (!player->HasSpell(id))
                player->learnSpell(id, false);

        // Character creation grants no ruleset, and the client's selection frame is level and
        // rested-area gated, so a character without one can never leave C_Player.Ruleset.None
        // on its own. Default to the only harmless ruleset until the player picks another.
        // The PvE set is 1004119 + 9931032, so it carries the War Mode name, description and
        // SPELL_AURA_MOD_XP_PCT 15 that any explicit PvE selection already applies; a realm that does not
        // want that applied without a player action can turn the default off here.
        if (!sConfigMgr->GetOption<bool>("AscensionCompat.RulesetLoginDefault", true))
            return;

        // A character evicted from an instance at login is already out of the world, mid far-teleport:
        // CharacterHandler guards its own login-time cast with the same test for that reason. Leave the
        // default to the next login instead of casting at a unit the client is unloading.
        if (!player->IsInWorld())
            return;

        if (!player->HasAura(SPELL_HIGH_RISK) && !player->HasAura(SPELL_WAR_MODE) && !player->HasAura(SPELL_PVE))
            ApplyRuleset(player, SPELL_SELECT_PVE);
    }
};
}

void AddSC_AscensionRulesets()
{
    RegisterSpellScript(spell_ascension_ruleset_select);
    new ruleset_aura_metadata();
    new ruleset_player_spells();
    new ruleset_high_risk_ffa();
    new ruleset_high_risk_death();
}
