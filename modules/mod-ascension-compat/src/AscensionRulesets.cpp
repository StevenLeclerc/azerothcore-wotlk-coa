/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "CharacterCache.h"
#include "Config.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "WorldSession.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
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
//   * Uninsured — meaning unable to pay — loses equipment instead, and that
//     piece goes to the killer: it lands in their bags, or in their mailbox if
//     their bags are full or they have logged out. A piece that merely vanished
//     was a tax on the loser, not a prize for the winner.
//
// The insurance is automatic rather than bought in advance. There is no client
// frame to sell a policy from, and an opt-in the player cannot see is an opt-in
// nobody uses. Paying when you can and bleeding when you cannot is the same
// bargain, minus the interface we do not have.
//
// The piece is handed over rather than left on the corpse, and that is a
// measured choice, not a shortcut — see CONCEPTION-butin-cadavre.md. In short:
// WorldSession::HandleLootOpcode (LootHandler.cpp:296-300) refuses any GUID that
// is not a creature or a vehicle, so no client can right-click a player corpse
// open. The only door the core opens onto a player corpse is
// Player::RemovedInsignia (Player.cpp:8039), reached solely from
// Spell::EffectSkinPlayerCorpse (SpellEffects.cpp:5928) with spell 22027, which
// returns immediately outside a battleground, needs the victim still online AND
// still a ghost, and needs the caster to know a spell no character on this realm
// has. And mod-playerbots loots only through CMSG_LOOT
// (src/Ai/Base/Actions/LootAction.cpp:102), which that same handler rejects for a
// corpse — so a mercenary could never collect.
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

// La sanction est DECIDEE a la mort, ARMEE au relachement, APPLIQUEE plus tard,
// sur un tick de joueur ordinaire. Aucune des trois sequences du coeur n'est
// touchee.
//
// 1. Unit::Kill (OnPlayerPVPKill / OnPlayerKilledByCreature, Unit.cpp:14896 et
//    :14903). On y est APRES le SetHealth(0) de securite de Unit::setDeathState
//    (Unit.cpp:11903) et AVANT Player::KillPlayer (PlayerUpdates.cpp:324).
//    Retirer les auras d'une piece d'equipement y ramene un cadavre a 1 PV
//    (SpellAuraEffects.cpp:4625) : le client croit le joueur vivant et ferme sa
//    fenetre de relachement. C'est P-031. Ce crochet ne fait que NOTER.
//
// 2. OnPlayerReleasedGhost (Player.cpp:4635). C'est la derniere instruction de
//    BuildPlayerRepop, mais BuildPlayerRepop n'est que la MOITIE du
//    relachement : WorldSession::HandleRepopRequestOpcode (MiscHandler.cpp:90)
//    enchaine sur Player::RepopAtGraveyard (Player.cpp:5088), qui remet
//    m_deathTimer a zero, teleporte au cimetiere et envoie
//    SMSG_DEATH_RELEASE_LOC. Ce crochet tombe donc ENTRE les deux moities : ce
//    qu'on y fait s'intercale dans une sequence inachevee. Il ne fait plus
//    qu'ARMER la dette — une ecriture dans la table ci-dessous, aucun paquet,
//    aucun etat de joueur, aucune base.
//
// 3. OnPlayerUpdate (PlayerUpdates.cpp:315). Tick de joueur ordinaire, hors de
//    toute sequence du coeur : la mort est finalisee, le relachement est fini,
//    la teleportation au cimetiere est acquittee. C'est la, et seulement la,
//    qu'on touche l'inventaire, l'argent et le courrier.
//
// Ce qu'il faut retenir du tueur pour solder la dette : son niveau pour
// l'exemption d'ecart, son GUID pour lui remettre la piece.
//
// Le GUID reste vide quand il n'y a personne a qui donner : un tueur PNJ, ou
// le joueur lui-meme. Ce dernier cas n'est pas theorique — Unit::Kill appelle
// OnPlayerPVPKill(killer, killed) sans le garde `victim != killer` que porte
// le bloc des hauts faits juste au-dessus (Unit.cpp:14882-14887), donc une
// chute ou une noyade passe par ce crochet avec killer == victime.
struct Debt
{
    ObjectGuid Killer;
    uint8 KillerLevel = 0;
    bool Armed = false;       // le joueur a relache son corps
    uint32 ElapsedMs = 0;     // temps ecoule depuis l'armement
};

std::mutex PendingMutex;
std::unordered_map<ObjectGuid, Debt> Pending;   // victime -> dette

// Portillon a cout nul pour OnPlayerUpdate : ce crochet tourne pour CHAQUE
// joueur et chaque bot a chaque tick. Tant que personne ne doit rien, on ne
// prend pas le verrou. Compte les entrees armees, pas les entrees totales.
std::atomic<uint32> ArmedDebts{0};

// Le verdict se prend ICI, a l'instant de la mort. PenaltyApplies teste le
// champ de bataille, l'arene, la zone sans PvP et la carte : le rejouer au
// relachement laisserait echapper celui qui meurt en plein champ et relache
// depuis un sanctuaire.
void Remember(Player* victim, Unit const* killer)
{
    if (!victim)
        return;
    if (!sConfigMgr->GetOption<bool>("AscensionCompat.HighRiskDeathPenalty", true))
        return;
    if (!PenaltyApplies(victim))
        return;

    Debt debt;
    debt.KillerLevel = killer ? uint8(killer->GetLevel()) : uint8(0);
    if (killer && killer->IsPlayer() && killer->GetGUID() != victim->GetGUID())
        debt.Killer = killer->GetGUID();

    std::lock_guard<std::mutex> lock(PendingMutex);
    // Mourir de nouveau avant d'avoir solde la precedente remplace la dette :
    // une seule sanction par mort, la derniere. Si l'ancienne etait armee, le
    // compteur doit la perdre, sinon le portillon reste ouvert pour rien.
    auto it = Pending.find(victim->GetGUID());
    if (it != Pending.end() && it->second.Armed)
        ArmedDebts.fetch_sub(1, std::memory_order_relaxed);
    Pending[victim->GetGUID()] = debt;
}

// Appele depuis OnPlayerReleasedGhost. N'ecrit QUE dans la table : pas un
// paquet, pas un champ de joueur, pas une requete. Voir le commentaire de
// Debt, point 2.
void Arm(Player* victim)
{
    if (!victim)
        return;
    std::lock_guard<std::mutex> lock(PendingMutex);
    auto it = Pending.find(victim->GetGUID());
    if (it == Pending.end() || it->second.Armed)
        return;
    it->second.Armed = true;
    it->second.ElapsedMs = 0;
    ArmedDebts.fetch_add(1, std::memory_order_relaxed);
}

// Une dette jamais soldee (resurrection par un tiers avant relachement,
// deconnexion brutale) ne doit pas survivre a la session. Indexee par
// ObjectGuid et non par Player*, une entree perimee ne peut pas dereferencer
// un objet detruit.
void Forget(Player* victim)
{
    if (!victim)
        return;
    std::lock_guard<std::mutex> lock(PendingMutex);
    auto it = Pending.find(victim->GetGUID());
    if (it == Pending.end())
        return;
    if (it->second.Armed)
        ArmedDebts.fetch_sub(1, std::memory_order_relaxed);
    Pending.erase(it);
}

// Ou la piece perdue a fini, pour le libelle des messages.
enum class Spoils
{
    Destroyed,   // personne a qui la donner, ou option desactivee
    Bags,        // directement dans les sacs du tueur
    Mail         // sacs pleins, tueur hors ligne ou sur une autre carte
};

// Remet la piece au tueur au lieu de la detruire.
//
// C'est le chemin d'echange du coeur, pas un second systeme :
// MoveItemFromInventory, puis CanStoreItem + MoveItemToInventory, sont
// exactement les trois appels qu'enchainent WorldSession::HandleAcceptTradeOpcode
// (TradeHandler.cpp:500) et WorldSession::moveItems (TradeHandler.cpp:135 et 151).
// L'objet LUI-MEME voyage, avec ses enchantements, sa durabilite et ses
// proprietes aleatoires ; sa ligne de character_inventory est reecrite par
// CHAR_REP_INVENTORY_ITEM (un REPLACE, clef primaire = l'objet) a la prochaine
// sauvegarde du tueur, donc sans duplication possible.
//
// Sacs pleins, tueur hors ligne ou ailleurs : le courrier, comme
// WorldSession::HandleSendMailOpcode (MailHandler.cpp:337-346) detache un objet
// d'un sac pour l'attacher a une lettre.
//
// PARALLELISME. Ce code tourne dans le fil de la carte de la victime :
// HighRisk::Tick est appele depuis OnPlayerUpdate, donc depuis Player::Update,
// donc depuis Map::Update, et MapUpdate.Threads vaut 6 sur ce royaume. Le
// coeur classe justement l'echange
// (CMSG_ACCEPT_TRADE, Opcodes.cpp:413) et le courrier (CMSG_SEND_MAIL, :699)
// PROCESS_THREADUNSAFE parce qu'ils touchent un autre joueur. On ne touche donc
// l'objet Player du tueur QUE s'il partage la carte de la victime — meme carte,
// meme fil, meme surete que le reste du crochet. Sinon la lettre part par GUID
// seul, et MailDraft::SendMailTo n'ecrit alors qu'en base (Mail.cpp:196-258).
//
// L'appelant a deja lu le nom de la piece : apres MoveItemToInventory le
// pointeur peut avoir ete fusionne dans une pile et ne doit plus etre lu.
Spoils HandToKiller(Player* victim, Item* item, ObjectGuid killerGuid, std::string const& itemName, std::string& killerName)
{
    if (!item)
        return Spoils::Destroyed;

    uint8 bag = item->GetBagSlot();
    uint8 slot = item->GetSlot();

    if (!killerGuid || !sConfigMgr->GetOption<bool>("AscensionCompat.HighRiskLootToKiller", true))
    {
        victim->DestroyItem(bag, slot, true);
        return Spoils::Destroyed;
    }

    // Un personnage efface entre la mort et le relachement n'a plus de boite
    // aux lettres : la lettre partirait vers un destinataire inexistant.
    CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(killerGuid);
    if (!cache)
    {
        victim->DestroyItem(bag, slot, true);
        return Spoils::Destroyed;
    }
    killerName = cache->Name;

    // `killer` n'est retenu que s'il est joignable SANS sortir du fil de la
    // carte : hors de ce cas on le remet a nullptr et plus une seule ligne
    // ci-dessous ne le dereference.
    Player* killer = ObjectAccessor::FindConnectedPlayer(killerGuid);
    if (killer && (!killer->IsInWorld() || killer->FindMap() != victim->FindMap()))
        killer = nullptr;

    // Detache la piece. RemoveItem retire les sorts d'equipement et les
    // enchantements comme le ferait DestroyItem (PlayerStorage.cpp:3039) ; on
    // est sur un tick de joueur ordinaire, hors de la fenetre de mort qui a
    // produit P-031 et hors de la sequence de relachement qui a produit P-040.
    victim->MoveItemFromInventory(bag, slot, true);

    if (killer)
    {
        ItemPosCountVec dest;
        if (killer->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
        {
            killer->MoveItemToInventory(dest, item, true, true);
            if (killer->GetSession())
                ChatHandler(killer->GetSession()).PSendSysMessage(
                    "High Risk: you took {} from {}'s corpse.", itemName, victim->GetName());
            return Spoils::Bags;
        }
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->DeleteFromInventoryDB(trans);
    if (item->GetState() == ITEM_UNCHANGED)
        item->FSetState(ITEM_CHANGED);
    item->SetOwnerGUID(killerGuid);
    item->SaveToDB(trans);

    MailDraft("High Risk spoils", "Taken from the corpse of " + victim->GetName() + ".")
        .AddItem(item)
        .SendMailTo(trans,
                    MailReceiver(killer, killerGuid.GetCounter()),
                    MailSender(MAIL_NORMAL, victim->GetGUID().GetCounter(), MAIL_STATIONERY_DEFAULT),
                    MAIL_CHECK_MASK_COPIED);
    CharacterDatabase.CommitTransaction(trans);

    if (killer && killer->GetSession())
        ChatHandler(killer->GetSession()).PSendSysMessage(
            "High Risk: {} was taken from {}'s corpse and sent to your mailbox.",
            itemName, victim->GetName());

    return Spoils::Mail;
}

// Verdict deja rendu par Remember : on ne rejoue pas PenaltyApplies.
void Settle(Player* victim, Debt const& debt)
{
    if (!victim || !victim->GetSession())
        return;

    uint32 gap = sConfigMgr->GetOption<uint32>("AscensionCompat.HighRiskDeathLevelGap", 4);
    if (gap && debt.KillerLevel)
    {
        int32 diff = int32(victim->GetLevel()) - int32(debt.KillerLevel);
        if (uint32(std::abs(diff)) >= gap)
        {
            ChatHandler(victim->GetSession()).PSendSysMessage(
                "High Risk: no loss, the level gap was too wide.");
            return;
        }
    }

    // Une prime nulle veut dire qu'il n'y a rien a assurer, pas qu'on est
    // insolvable. Le test d'origine, `premium && GetMoney() >= premium`,
    // envoyait ce cas a la perte de piece : invisible sur un personnage nu,
    // que le test worn.empty() plus bas rattrapait, mais pas sur un personnage
    // entierement vetu d'objets de niveau d'objet 0.
    uint32 premium = Premium(victim);
    if (!premium)
    {
        ChatHandler(victim->GetSession()).PSendSysMessage(
            "High Risk: nothing to insure and nothing to lose.");
        return;
    }

    if (victim->GetMoney() >= premium)
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

    // Garde-fou : une prime non nulle implique au moins une piece portee.
    if (worn.empty())
        return;

    uint8 slot = worn[urand(0, worn.size() - 1)];
    Item* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
        return;

    // Le nom se lit avant le transfert : apres MoveItemToInventory le pointeur
    // peut avoir ete fusionne dans une pile existante et detruit.
    std::string name = "a piece of equipment";
    if (ItemTemplate const* proto = item->GetTemplate())
        name = proto->Name1;

    std::string killerName;
    std::string cost = MoneyText(premium);
    switch (HandToKiller(victim, item, debt.Killer, name, killerName))
    {
        case Spoils::Bags:
            ChatHandler(victim->GetSession()).PSendSysMessage(
                "High Risk: you could not cover the {} premium. {} looted {} from your corpse.",
                cost, killerName, name);
            break;
        case Spoils::Mail:
            ChatHandler(victim->GetSession()).PSendSysMessage(
                "High Risk: you could not cover the {} premium. {} was taken from your corpse and sent to {}.",
                cost, name, killerName);
            break;
        case Spoils::Destroyed:
            ChatHandler(victim->GetSession()).PSendSysMessage(
                "High Risk: you could not cover the {} premium. You lost {}.",
                cost, name);
            break;
    }
}

// Le tick qui solde la dette, hors de toute sequence du coeur.
//
// Trois conditions, dans cet ordre, et chacune paie sa place :
//
//  * IsInWorld / GetSession / PlayerLogout : le joueur doit exister vraiment.
//    BuildPlayerRepop est aussi appele depuis WorldSession::LogoutPlayer
//    (WorldSession.cpp:732), donc une dette peut s'armer sur un joueur en
//    train de partir.
//  * !IsBeingTeleported() : RepopAtGraveyard teleporte au cimetiere. Une
//    teleportation proche n'est acquittee qu'au retour du MSG_MOVE_TELEPORT_ACK
//    du client (MovementHandler.cpp, HandleMoveTeleportAck) ; jusque-la
//    mSemaphoreTeleport_Near est arme et le joueur est a cheval entre deux
//    positions. On n'y touche ni l'inventaire ni le courrier.
//  * un delai de grace, pour laisser partir le lot de mise a jour de fin de
//    tick qui porte l'etat de fantome (UNIT_FIELD_HEALTH, PLAYER_FLAGS,
//    UNIT_FIELD_BYTES_1) avant d'envoyer autre chose au client.
void Tick(Player* player, uint32 diff)
{
    if (!player || !player->IsInWorld() || !player->GetSession() || player->GetSession()->PlayerLogout())
        return;
    if (player->IsBeingTeleported())
        return;

    // Lu hors du verrou : sConfigMgr a le sien, et imbriquer deux verrous pour
    // une option relue a chaque tick ne se justifie pas.
    uint32 delay = sConfigMgr->GetOption<uint32>("AscensionCompat.HighRiskSettleDelayMs", 2000);

    Debt debt;
    {
        std::lock_guard<std::mutex> lock(PendingMutex);
        auto it = Pending.find(player->GetGUID());
        if (it == Pending.end() || !it->second.Armed)
            return;

        it->second.ElapsedMs += diff;
        if (it->second.ElapsedMs < delay)
            return;

        debt = it->second;
        Pending.erase(it);
        ArmedDebts.fetch_sub(1, std::memory_order_relaxed);
    }

    Settle(player, debt);
}
}

class ruleset_high_risk_death : public PlayerScript
{
public:
    ruleset_high_risk_death() : PlayerScript("ruleset_high_risk_death",
        {PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
         PLAYERHOOK_ON_PLAYER_RELEASED_GHOST, PLAYERHOOK_ON_UPDATE,
         PLAYERHOOK_ON_LOGOUT}) { }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        // Rien ne doit toucher la victime ici : voir HighRisk::Debt, point 1.
        HighRisk::Remember(killed, killer);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        HighRisk::Remember(killed, killer);
    }

    // Derniere instruction de BuildPlayerRepop, donc AU MILIEU du relachement :
    // RepopAtGraveyard n'a pas encore tourne. On se contente d'armer.
    void OnPlayerReleasedGhost(Player* player) override
    {
        HighRisk::Arm(player);
    }

    // Tick de joueur ordinaire : c'est ici, et nulle part ailleurs, que la
    // sanction s'applique.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!HighRisk::ArmedDebts.load(std::memory_order_relaxed))
            return;
        HighRisk::Tick(player, diff);
    }

    void OnPlayerLogout(Player* player) override
    {
        HighRisk::Forget(player);
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

// Filet de securite : un cadavre ne doit jamais avoir de vie.
//
// Ce n'est pas une regle de royaume, et ce script n'a rien a faire dans un
// fichier qui s'appelle Rulesets. Il y est pour une raison de chantier : y
// ajouter une classe ne demande ni nouveau fichier, ni ligne dans MP_loader,
// ni reconfiguration cmake. A deplacer le jour ou la cause racine est nommee.
//
// L'INVARIANT, que le coeur pose lui-meme. Unit::setDeathState fait
// SetHealth(0) juste apres RemoveAllAurasOnDeath(), avec ce commentaire
// (Unit.cpp:11901-11903) :
//
//     // without this when removing IncreaseMaxHealth aura player may stuck with 1 hp
//     // do not why since in IncreaseMaxHealth currenthealth is checked
//     SetHealth(0);
//
// LE TROU. Ce filet n'existe que dans la branche `s == JustDied`. Or le bloc du
// haut de la meme fonction (Unit.cpp:11871-11884) rejoue RemoveAllAurasOnDeath()
// pour TOUT etat autre qu'Alive et JustRespawned — Corpse compris. Player::
// KillPlayer (Player.cpp:4729) fait justement setDeathState(Corpse) : la purge
// d'auras y tourne une SECONDE fois, sans filet derriere.
//
// CE QUI PASSE PAR CE TROU. Toute aura SPELL_AURA_MOD_INCREASE_HEALTH (34) ou
// _2 (250) appliquee au joueur entre sa mort et KillPlayer :
//   * a l'application, SpellAuraEffects.cpp:4618 fait ModifyHealth(+montant),
//     et Unit::ModifyHealth n'a AUCUNE garde de mort ;
//   * au retrait, SpellAuraEffects.cpp:4625 fait SetHealth(1).
// Et sScriptMgr->OnPlayerUpdate (PlayerUpdates.cpp:315) tourne AVANT le
// KillPlayer de la ligne 323 : chaque tick de module voit donc une fois le
// joueur en DeathState::JustDied. mod-ascension-compat en compte 19, dont au
// moins un (xoroth_player::OnPlayerUpdate -> AscensionXoroth::Refresh, toutes
// les 250 ms) qui pose et retire des auras sans jamais tester IsAlive().
//
// CE QUE CA DONNE EN JEU. Un cadavre a vie non nulle fait sortir le client
// 3.3.5 de PLAYER_DEAD : la fenetre de mort se ferme d'elle-meme. Le serveur,
// lui, garde le joueur mort — roote par KillPlayer (Player.cpp:4725, jamais
// suivi d'un SendMoveRoot(false) tant qu'il n'y a ni relachement ni
// resurrection) et invisible pour les creatures (IsAlive() faux). Le joueur
// tourne sur lui-meme et ne fait rien d'autre. C'est P-031, et c'est P-041.
//
// CONDITIONS, etroites a dessein :
//   * getDeathState() == Corpse : KillPlayer est passe, la mort est finalisee ;
//   * pas de PLAYER_FLAGS_GHOST : BuildPlayerRepop pose le drapeau (via l'aura
//     8326, Player.cpp:4599) AVANT son SetHealth(1) (Player.cpp:4620), donc un
//     fantome legitime est exclu ;
//   * vie > 0.
//
// ET C'EST BRUYANT. Chaque correction est journalisee avec le nom du joueur et
// la vie trouvee. Ce filet repare le symptome ; le journal est ce qui permettra
// de nommer la cause au lieu de la masquer. Le vrai correctif est d'une ligne
// et il est dans le coeur : correctifs-a-porter/coeur-cadavre-sans-vie.patch.
class ruleset_dead_player_health_guard : public PlayerScript
{
    // Une seule ligne de journal par cadavre, pas une par tick.
    //
    // Trouve en me relisant : si la source remonte la vie a CHAQUE tick, la
    // version sans garde-fou ecrivait vingt lignes par seconde et par joueur
    // dans Errors.log. Un correctif qui noie le journal detruit justement ce
    // qui doit servir a nommer la cause.
    //
    // Le portillon atomique evite de prendre le verrou tant qu'aucun cadavre
    // n'a ete signale — ce hook tourne pour chaque joueur et chaque bot, a
    // chaque tick.
    static std::mutex ReportedMutex;
    static std::unordered_map<ObjectGuid, bool> Reported;
    static std::atomic<uint32> ReportedCount;

    static void Clear(ObjectGuid guid)
    {
        if (!ReportedCount.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> lock(ReportedMutex);
        if (Reported.erase(guid))
            ReportedCount.fetch_sub(1, std::memory_order_relaxed);
    }

    static bool FirstReport(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(ReportedMutex);
        if (!Reported.emplace(guid, true).second)
            return false;
        ReportedCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

public:
    ruleset_dead_player_health_guard() : PlayerScript("ruleset_dead_player_health_guard",
        {PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT}) { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!player || !player->IsInWorld())
            return;

        if (player->getDeathState() != DeathState::Corpse)
        {
            // Sorti de l'etat de cadavre : la prochaine anomalie sera de
            // nouveau journalisee.
            Clear(player->GetGUID());
            return;
        }

        if (player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            return;

        uint32 health = player->GetHealth();
        if (!health)
            return;

        if (!sConfigMgr->GetOption<bool>("AscensionCompat.DeadPlayerHealthGuard", true))
            return;

        player->SetHealth(0);

        if (FirstReport(player->GetGUID()))
            LOG_ERROR("module.ascension_compat",
                "Dead player health guard: {} ({}) was a corpse carrying {} hp with no ghost flag; forced back to 0. "
                "Something raised the health of a dead player - see P-041.",
                player->GetName(), player->GetGUID().ToString(), health);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            Clear(player->GetGUID());
    }
};

std::mutex ruleset_dead_player_health_guard::ReportedMutex;
std::unordered_map<ObjectGuid, bool> ruleset_dead_player_health_guard::Reported;
std::atomic<uint32> ruleset_dead_player_health_guard::ReportedCount{0};
}

void AddSC_AscensionRulesets()
{
    RegisterSpellScript(spell_ascension_ruleset_select);
    new ruleset_aura_metadata();
    new ruleset_player_spells();
    new ruleset_high_risk_ffa();
    new ruleset_high_risk_death();
    new ruleset_dead_player_health_guard();
}
