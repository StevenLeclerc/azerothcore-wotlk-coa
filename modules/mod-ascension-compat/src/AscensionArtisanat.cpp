/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, refonte de l'artisanat, lot 1 (docs/SUIVI-artisanat.md, docs/CONCEPTION-artisanat-A-cadre.md).
//
// 1. Recopie des gemmes et enchantements. Une piece de l'artisanat change de version par un sort qui cree la
//    version suivante et consomme l'ancienne : soit comme objet de lancement (« Utiliser : Renforcer », detruit
//    par TakeCastItem APRES les effets), soit comme reactif (eveil par l'atelier, detruit par TakeReagents AVANT
//    les effets). Le cœur cree l'objet vierge (Spell::DoCreateItem). On photographie donc les candidats au
//    moment de OnPlayerSpellCast, avant toute consommation, et on recopie dans OnPlayerCreateItem sur la piece
//    neuve : l'objet de lancement s'il est une piece, sinon le reactif qui a disparu de l'inventaire entre les
//    deux crochets. Rien ne se recopie si l'ancienne piece n'a pas ete consommee (pas de duplication).
// 2. Bonus de set qui suivent le niveau (D-PO-07). Player::GiveLevel ne recalcule pas les auras deja posees :
//    au passage de niveau, on recalcule les auras de set 63001-63015 avec le niveau courant.
// 3. Eveil par message d'addon COA_ATL « EVEIL » (D-PO-10 : voie retenue, la voie par sort est ecartee ; format
//    D-PO-09). Allume par defaut ; CoaArtisanat.EveilParMessage = 0 le coupe (reponse EVEIL_ERR:DESACTIVE).

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Realm.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Plages reservees (CONCEPTION-artisanat-A-cadre.md, « Plages d'ids reservees »).
    constexpr uint32 ARTISANAT_OBJET_MIN = 9101200;
    constexpr uint32 ARTISANAT_OBJET_MAX = 9102999;
    constexpr uint32 ARTISANAT_SET_MIN = 63001;
    constexpr uint32 ARTISANAT_SET_MAX = 63015;
    constexpr uint32 ARTISANAT_GRILLE_MIN = 708;
    constexpr uint32 ARTISANAT_GRILLE_MAX = 1999;
    constexpr uint8 NIVEAU_EVEIL = 80;
    constexpr uint32 ITEM_LEVEL_EVEILLE = 213;

    constexpr std::string_view PREFIXE_ADDON = "COA_ATL\t";   // CONCEPTION-artisanat-C-atelier.md, messages d'addon
    std::string const CLE_DONNEES = "CoaArtisanatRecopie";
    std::string const CLE_DELAI = "CoaArtisanatDelai";
    constexpr Milliseconds DELAI_EVEIL{ 500 };

    // Emplacements recopies. Les PROP_ENCHANTMENT_SLOT_* (suffixe aleatoire) appartiennent au gabarit de la piece
    // et ne se transportent pas ; BONUS_ENCHANTMENT_SLOT (bonus de chasse) se recalcule sur la piece neuve.
    constexpr std::array<EnchantmentSlot, 6> EMPLACEMENTS_RECOPIES =
    {
        PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT_2,
        SOCK_ENCHANTMENT_SLOT_3, PRISMATIC_ENCHANTMENT_SLOT
    };

    bool EstObjetArtisanat(uint32 entry)
    {
        return entry >= ARTISANAT_OBJET_MIN && entry <= ARTISANAT_OBJET_MAX;
    }

    bool EstPieceArtisanat(ItemTemplate const* proto)
    {
        return proto && EstObjetArtisanat(proto->ItemId)
            && (proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON);
    }

    struct Enchantement
    {
        uint32 id = 0;
        uint32 duree = 0;
        uint32 charges = 0;
    };

    struct Chasses
    {
        ObjectGuid source;
        uint32 entreeSource = 0;
        ObjectGuid fabricant;
        std::array<Enchantement, EMPLACEMENTS_RECOPIES.size()> emplacements{};

        bool Vide() const
        {
            for (Enchantement const& e : emplacements)
                if (e.id)
                    return false;
            return true;
        }
    };

    // Le Player tient la duree restante des enchantements temporaires des pieces PORTEES dans sa propre liste
    // (m_enchantDuration), et ne la reporte dans l'objet qu'a la sauvegarde ou au retrait. On la reporte avant
    // de lire, sinon la piece neuve recevrait la duree de la derniere sauvegarde.
    Chasses Photographier(Player* joueur, Item const* piece)
    {
        joueur->UpdateEnchantmentDurations();
        Chasses c;
        c.source = piece->GetGUID();
        c.entreeSource = piece->GetEntry();
        c.fabricant = piece->GetGuidValue(ITEM_FIELD_CREATOR);
        for (std::size_t i = 0; i < EMPLACEMENTS_RECOPIES.size(); ++i)
        {
            EnchantmentSlot const slot = EMPLACEMENTS_RECOPIES[i];
            c.emplacements[i] = { piece->GetEnchantmentId(slot), piece->GetEnchantmentDuration(slot),
                piece->GetEnchantmentCharges(slot) };
        }
        return c;
    }

    // Recopie sur la piece neuve. Commune aux deux voies d'eveil (sort, message) et au renforcement.
    // Rend le nombre d'emplacements recopies ; `perdus` compte les gemmes sans chasse sur la piece neuve.
    uint32 Recopier(Player* joueur, Item* cible, Chasses const& c, uint32& perdus)
    {
        perdus = 0;
        ItemTemplate const* proto = cible->GetTemplate();
        bool const portee = cible->IsEquipped();
        uint32 recopies = 0;

        // Chasse prismatique (boucle de ceinture...) : la premiere chasse sans couleur, si la piece porte
        // l'enchantement prismatique. Meme regle que WorldSession::HandleSocketOpcode.
        bool prismatique = false;
        for (std::size_t i = 0; i < EMPLACEMENTS_RECOPIES.size(); ++i)
            if (EMPLACEMENTS_RECOPIES[i] == PRISMATIC_ENCHANTMENT_SLOT && c.emplacements[i].id)
                prismatique = true;
        uint32 premierePrismatique = 0;
        while (premierePrismatique < MAX_GEM_SOCKETS && proto->Socket[premierePrismatique].Color)
            ++premierePrismatique;

        for (std::size_t i = 0; i < EMPLACEMENTS_RECOPIES.size(); ++i)
        {
            Enchantement const& e = c.emplacements[i];
            if (!e.id)
                continue;
            EnchantmentSlot const slot = EMPLACEMENTS_RECOPIES[i];
            if (slot >= SOCK_ENCHANTMENT_SLOT && slot <= SOCK_ENCHANTMENT_SLOT_3
                && !proto->Socket[slot - SOCK_ENCHANTMENT_SLOT].Color
                && !(prismatique && uint32(slot - SOCK_ENCHANTMENT_SLOT) == premierePrismatique))
            {
                ++perdus;
                continue;
            }
            if (portee)
                joueur->ApplyEnchantment(cible, slot, false);
            cible->SetEnchantment(slot, e.id, e.duree, e.charges, joueur->GetGUID());
            if (portee)
                joueur->ApplyEnchantment(cible, slot, true);   // reprend aussi la duree (apply_dur)
            ++recopies;
        }

        // Bonus de chasse : meme regle que WorldSession::HandleSocketOpcode.
        uint32 const bonus = cible->GemsFitSockets() ? proto->socketBonus : 0;
        if (bonus != cible->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT))
        {
            if (portee)
                joueur->ApplyEnchantment(cible, BONUS_ENCHANTMENT_SLOT, false);
            cible->SetEnchantment(BONUS_ENCHANTMENT_SLOT, bonus, 0, 0, joueur->GetGUID());
            if (portee)
                joueur->ApplyEnchantment(cible, BONUS_ENCHANTMENT_SLOT, true);
        }

        // Le fabricant reste celui de la piece d'origine (le cœur met celui qui lance le sort).
        if (c.fabricant && proto->HasSignature())
            cible->SetGuidValue(ITEM_FIELD_CREATOR, c.fabricant);

        cible->SetState(ITEM_CHANGED, joueur);
        return recopies;
    }

    // Ce qu'on a photographie dans OnPlayerSpellCast, en attente de la creation de l'objet.
    struct RecopieEnAttente final : public DataMap::Base
    {
        uint32 sort = 0;
        uint32 entreeCreee = 0;
        Milliseconds instant{ 0 };
        bool objetDeLancement = false;       // la source est l'objet de lancement (renforcement)
        std::vector<Chasses> candidats;       // sinon : les pieces reactives presentes avant la consommation

        void Vider()
        {
            sort = 0;
            entreeCreee = 0;
            objetDeLancement = false;
            candidats.clear();
        }
    };

    // L'objet de lancement n'est detruit apres les effets que si Spell::TakeCastItem le decide : on refait son
    // calcul a l'identique (sort non declenche sans objet, charges du gabarit negatives, charges de l'exemplaire
    // epuisees apres usage). Sans quoi la piece reste, et recopier dupliquerait gemmes et enchantements.
    bool ObjetDeLancementConsomme(Spell const* sort, Item const* objet)
    {
        if (sort->HasTriggeredCastFlag(TRIGGERED_IGNORE_CAST_ITEM))
            return false;
        ItemTemplate const* proto = objet->GetTemplate();
        bool consommable = false, epuise = false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            if (!proto->Spells[i].SpellId || !proto->Spells[i].SpellCharges)
                continue;
            if (proto->Spells[i].SpellCharges < 0)
                consommable = true;
            int32 charges = objet->GetSpellCharges(i);
            if (charges)
                (charges > 0) ? --charges : ++charges;
            epuise = (charges == 0);
        }
        return consommable && epuise;
    }

    // Les endroits que Player::HasItemCount/DestroyItemCount parcourent pour un reactif (banque exclue).
    template <typename Fn>
    void PourChaquePieceTenue(Player* joueur, uint32 entree, Fn&& fn)
    {
        auto voir = [&](Item* objet)
        {
            if (objet && objet->GetEntry() == entree && !objet->IsInTrade())
                fn(objet);
        };
        for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            voir(joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
            voir(joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 sac = INVENTORY_SLOT_BAG_START; sac < INVENTORY_SLOT_BAG_END; ++sac)
            if (Bag* b = joueur->GetBagByPos(sac))
                for (uint32 j = 0; j < b->GetBagSize(); ++j)
                    voir(b->GetItemByPos(uint8(j)));
    }

    uint32 EntreeCreeeArtisanat(SpellInfo const* info)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effet = info->Effects[i];
            if ((effet.Effect == SPELL_EFFECT_CREATE_ITEM || effet.Effect == SPELL_EFFECT_CREATE_ITEM_2)
                && EstPieceArtisanat(sObjectMgr->GetItemTemplate(effet.ItemType)))
                return effet.ItemType;
        }
        return 0;
    }

    void RecalculerBonusDeSet(Player* joueur)
    {
        for (ItemSetEffect const* eff : joueur->ItemSetEff)
        {
            if (!eff || eff->setid < ARTISANAT_SET_MIN || eff->setid > ARTISANAT_SET_MAX)
                continue;
            for (SpellInfo const* sort : eff->spells)
            {
                if (!sort)
                    continue;
                Aura* aura = joueur->GetAura(sort->Id, joueur->GetGUID());
                if (!aura)
                    continue;   // forme incompatible : le cœur la reposera au changement de forme, au bon niveau
                bool recalculable = true;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (AuraEffect const* effet = aura->GetEffect(i))
                        recalculable = recalculable && effet->CanBeRecalculated();
                if (recalculable)
                {
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                        if (AuraEffect* effet = aura->GetEffect(i))
                            effet->RecalculateAmount(joueur);
                }
                else if (joueur->IsAlive())
                {
                    // Absorption, controle... : le cœur interdit le recalcul, on repose le sort entier.
                    joueur->ApplyEquipSpell(sort, nullptr, false);
                    joueur->ApplyEquipSpell(sort, nullptr, true);
                }
                LOG_INFO("module", "coa artisanat: bonus de set recalcule joueur={} set={} sort={} niveau={} mode={}",
                    joueur->GetGUID().GetCounter(), eff->setid, sort->Id, uint32(joueur->GetLevel()),
                    recalculable ? "recalcul" : (joueur->IsAlive() ? "repose" : "mort, inchange"));
            }
        }
    }

    // ---- Eveil par message d'addon (repli de D-PO-01) ----------------------------------------------------------

    // Version eveillee de chaque piece de base, lue dans item_template au demarrage : une base porte une grille
    // d'heritage (ScalingStatDistribution 708-1999) ; sa version eveillee est l'unique piece de la plage, du meme
    // nom, du meme emplacement, de la meme classe et du meme set, au niveau d'objet 213, sans grille.
    std::unordered_map<uint32, uint32> VersionEveillee;

    void ChargerVersionsEveillees()
    {
        VersionEveillee.clear();
        std::unordered_map<uint32, std::vector<uint32>> candidats;
        ItemTemplateContainer const* gabarits = sObjectMgr->GetItemTemplateStore();
        std::vector<ItemTemplate const*> bases, eveillees;
        for (auto const& [entree, proto] : *gabarits)
        {
            if (!EstPieceArtisanat(&proto))
                continue;
            if (proto.ScalingStatDistribution >= ARTISANAT_GRILLE_MIN && proto.ScalingStatDistribution <= ARTISANAT_GRILLE_MAX)
                bases.push_back(&proto);
            else if (!proto.ScalingStatDistribution && proto.ItemLevel == ITEM_LEVEL_EVEILLE)
                eveillees.push_back(&proto);
        }
        uint32 ambigues = 0, orphelines = 0;
        for (ItemTemplate const* b : bases)
        {
            uint32 trouvee = 0, nombre = 0;
            for (ItemTemplate const* e : eveillees)
                if (e->Name1 == b->Name1 && e->InventoryType == b->InventoryType && e->Class == b->Class
                    && e->SubClass == b->SubClass && e->ItemSet == b->ItemSet)
                {
                    trouvee = e->ItemId;
                    ++nombre;
                }
            if (nombre == 1)
                VersionEveillee[b->ItemId] = trouvee;
            else if (nombre == 0)
                ++orphelines;
            else
            {
                ++ambigues;
                LOG_ERROR("module", "coa artisanat: piece de base {} : {} versions eveillees candidates, eveil refuse",
                    b->ItemId, nombre);
            }
        }
        LOG_INFO("module", "coa artisanat: {} pieces de base, {} versions eveillees appariees, {} sans version, {} ambigues",
            uint32(bases.size()), uint32(VersionEveillee.size()), orphelines, ambigues);
    }

    void Repondre(Player* joueur, std::string const& texte)
    {
        WorldPacket packet;
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, joueur->GetGUID(), joueur->GetGUID(),
            std::string(PREFIXE_ADDON) + texte, 0, joueur->GetName(), joueur->GetName(), 0, false);
        joueur->SendDirectMessage(&packet);
    }

    bool LireEntier(std::string_view texte, int32& valeur)
    {
        auto [fin, erreur] = std::from_chars(texte.data(), texte.data() + texte.size(), valeur);
        return erreur == std::errc() && fin == texte.data() + texte.size();
    }

    // Position client -> position serveur. `sac` = -1 : equipement, `casse` = emplacement 1-19 (GetInventoryItemID) ;
    // `sac` = 0 : sac a dos, 1-4 : sacs, `casse` a partir de 1 (GetContainerItemInfo).
    bool PositionServeur(Player* joueur, int32 sac, int32 casse, uint8& bagServeur, uint8& slotServeur)
    {
        if (sac == -1)
        {
            if (casse < 1 || casse > EQUIPMENT_SLOT_END)
                return false;
            bagServeur = INVENTORY_SLOT_BAG_0;
            slotServeur = uint8(EQUIPMENT_SLOT_START + casse - 1);
            return true;
        }
        if (sac == 0)
        {
            if (casse < 1 || casse > INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START)
                return false;
            bagServeur = INVENTORY_SLOT_BAG_0;
            slotServeur = uint8(INVENTORY_SLOT_ITEM_START + casse - 1);
            return true;
        }
        if (sac >= 1 && sac <= INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START)
        {
            bagServeur = uint8(INVENTORY_SLOT_BAG_START + sac - 1);
            Bag* b = joueur->GetBagByPos(bagServeur);
            if (!b || casse < 1 || uint32(casse) > b->GetBagSize())
                return false;
            slotServeur = uint8(casse - 1);
            return true;
        }
        return false;
    }

    // Refus de l'eveil : la cause (repondue a l'addon) et l'avis montre au joueur (vide : aucun).
    struct RefusEveil
    {
        char const* cause = nullptr;
        std::string_view avis;
    };

    // L'eveil lui-meme, commun au message d'addon et au banc d'essai (.artisanat banc). Rend la piece neuve, ou
    // nullptr et la cause du refus.
    Item* Eveiller(Player* joueur, Item* piece, RefusEveil& refus, char const* voie)
    {
        if (piece->IsInTrade())
            return refus = { "PIECE", {} }, nullptr;
        auto it = VersionEveillee.find(piece->GetEntry());
        if (it == VersionEveillee.end())
            return refus = { "PIECE", "Cette pièce ne peut pas être éveillée." }, nullptr;
        if (joueur->GetLevel() < NIVEAU_EVEIL)
            return refus = { "NIVEAU", "Il faut être niveau 80 pour éveiller une pièce." }, nullptr;
        if (!joueur->IsAlive())
            return refus = { "MORT", "Impossible d'éveiller une pièce en étant mort." }, nullptr;
        if (joueur->IsInCombat())
            return refus = { "COMBAT", "Impossible d'éveiller une pièce en combat." }, nullptr;

        uint32 const nouvelle = it->second;
        ItemPosCountVec dest;
        InventoryResult const place = joueur->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, nouvelle, 1);
        if (place != EQUIP_ERR_OK)
        {
            joueur->SendEquipError(place, nullptr, nullptr, nouvelle);
            return refus = { "SACS", {} }, nullptr;
        }

        // Creer d'abord, detruire ensuite : un echec a la creation ne coute rien au joueur.
        Chasses const c = Photographier(joueur, piece);
        Item* neuve = joueur->StoreNewItem(dest, nouvelle, true);
        if (!neuve)
            return refus = { "SACS", {} }, nullptr;
        uint32 perdus = 0;
        uint32 const recopies = Recopier(joueur, neuve, c, perdus);
        joueur->SendNewItem(neuve, 1, true, false);
        uint32 const ancienne = piece->GetEntry();
        uint64 const ancienGuid = piece->GetGUID().GetCounter();
        joueur->DestroyItem(piece->GetBagSlot(), piece->GetSlot(), true);

        LOG_INFO("module", "coa artisanat: eveil par {} joueur={} piece={} ({}) -> {} ({}) emplacements={} perdus={}",
            voie, joueur->GetGUID().GetCounter(), ancienGuid, ancienne, neuve->GetGUID().GetCounter(), nouvelle, recopies, perdus);
        return neuve;
    }

    // Message : « EVEIL:<sac>:<case>:<entree> ». Reponses : « EVEIL_OK:<entree eveillee> » ou « EVEIL_ERR:<cause> ».
    void TraiterEveil(Player* joueur, std::string_view corps)
    {
        auto refuser = [&](char const* cause, std::string_view avis)
        {
            Repondre(joueur, std::string("EVEIL_ERR:") + cause);
            if (!avis.empty())
                ChatHandler(joueur->GetSession()).SendNotification(avis);
        };

        std::array<std::string_view, 3> champs;
        for (std::size_t i = 0; i < champs.size(); ++i)
        {
            std::size_t const sep = corps.find(':');
            if (i + 1 < champs.size() && sep == std::string_view::npos)
                return refuser("FORMAT", {});
            champs[i] = i + 1 < champs.size() ? corps.substr(0, sep) : corps;
            corps.remove_prefix(i + 1 < champs.size() ? sep + 1 : corps.size());
        }
        int32 sac = 0, casse = 0, entree = 0;
        if (!LireEntier(champs[0], sac) || !LireEntier(champs[1], casse) || !LireEntier(champs[2], entree))
            return refuser("FORMAT", {});

        uint8 bag = 0, slot = 0;
        if (!PositionServeur(joueur, sac, casse, bag, slot))
            return refuser("PIECE", {});
        Item* piece = joueur->GetItemByPos(bag, slot);
        if (!piece || int32(piece->GetEntry()) != entree || piece->IsInTrade())
            return refuser("PIECE", {});
        RefusEveil refus;
        Item* neuve = Eveiller(joueur, piece, refus, "message");
        if (!neuve)
            return refuser(refus.cause, refus.avis);
        Repondre(joueur, "EVEIL_OK:" + std::to_string(neuve->GetEntry()));
    }

    struct DelaiEveil final : public DataMap::Base
    {
        Milliseconds prochain{ 0 };
    };

    // ---- Recolte des bots (preuve de l'exclusion des noeuds 9101000-9101099, mod-playerbots IsLootPossible) -------

    constexpr uint32 NOEUD_MIN = 9101000;
    constexpr uint32 NOEUD_MAX = 9101099;
    std::atomic<uint32> RecoltesBots{ 0 };            // objets pris par un bot dans le butin d'un gameobject
    std::atomic<uint32> RecoltesBotsNoeuds{ 0 };      // ... d'un de nos noeuds : doit rester a 0
    std::atomic<uint32> RecoltesJoueursNoeuds{ 0 };   // ... d'un de nos noeuds, par un joueur
    std::atomic<uint32> OuverturesBots{ 0 };          // butins de gameobjects ouverts par un bot (prise ou non)
    std::atomic<uint32> OuverturesBotsNoeuds{ 0 };    // ... sur un de nos noeuds : doit rester a 0

    // ---- Banc d'essai (D-PO-15) : royaume PTR seulement ------------------------------------------------------------

    // RealmID du PTR (/opt/coa/server/ptr/etc/worldserver.conf) ; le Live est le 1. Le banc ne s'ouvre nulle part
    // ailleurs : il donne des objets et change le niveau.
    constexpr uint32 ROYAUME_PTR = 2;
    bool BancPermis(ChatHandler* handler)
    {
        if (realm.Id.Realm == ROYAUME_PTR)
            return true;
        handler->SendSysMessage("Banc d'essai de l'artisanat : royaume PTR seulement.");
        return false;
    }

    // Salle d'essai de PartB (artisanat/genere/essai/salle.json, game_tele 424 « GMIsland ») : tout y est a 19 m
    // au plus de ce point (formateur, enclume, forge, banquier, mannequin, noeuds a reapparition de 10 s).
    constexpr uint32 SALLE_CARTE = 1;
    constexpr float SALLE_X = 16226.2f, SALLE_Y = 16257.0f, SALLE_Z = 13.2f, SALLE_O = 1.65f;

    constexpr uint32 SKILL_FORGE = 164;
    // Les 6 rangs de la Forge (Apprenti 2018 ... Grand maitre 51300). Le livre des metiers d'Ascension lance le sort du
    // rang de la competence max (Ranks[450] = 51300, patch-B ProfessionUtil.lua) : sans lui, la Forge ne s'ouvre pas (P-236).
    constexpr std::array<uint32, 6> RANGS_FORGE = { 2018, 3100, 3538, 9785, 29844, 51300 };
    constexpr uint32 RECETTE_MIN = 9111000;
    constexpr uint32 RECETTE_MAX = 9113999;
    constexpr uint32 OR_MINIMUM = 1000000;           // 100 po

    constexpr uint32 HEAUME_BASE = 9101510;
    constexpr uint32 HEAUME_EVEILLE = 9101511;
    constexpr uint32 GARDE_POIGNETS_BASE = 9101468;
    constexpr uint32 GEMME_META = 41398;              // Relentless Earthsiege Diamond
    constexpr uint32 GEMME_ROUGE = 40111;             // Bold Cardinal Ruby
    constexpr uint32 ARCANE = 44878;                  // Arcanum of the Stalwart Protector
    constexpr uint32 PARCHEMIN = 38771;               // Scroll of Enchant Bracer - Minor Stamina
    constexpr uint32 HUILE = 20748;                   // Brilliant Mana Oil : enchantement temporaire du banc
    constexpr uint32 SAC_VIDE = 41599;                // Frostweave Bag, 20 cases

    struct Quantite
    {
        uint32 entree;
        uint32 nombre;
    };

    // Fiche de seance, § 1 (docs/LOT1-artisanat-seance.md).
    constexpr std::array<Quantite, 12> INGREDIENTS_SEANCE =
    {{
        { 5956, 1 }, { 2840, 4 }, { 9101200, 80 }, { 9100101, 80 }, { 9100106, 20 }, { 9100107, 1 },
        { 36913, 2 }, { 41163, 1 }, { 37663, 1 }, { 9101230, 1 }, { 9101210, 2 }, { 9101208, 2 }
    }};
    // Fiche de seance, § 8 : les cinq rangs du Heaume (9111550-9111554).
    constexpr std::array<Quantite, 6> REACTIFS_RANGS =
    {{
        { 9100101, 24 }, { 9100106, 10 }, { 9100107, 1 }, { 9101231, 2 }, { 9101232, 2 }, { 9101233, 1 }
    }};

    // Complete jusqu'a `nombre` (ne cumule pas d'une etape a l'autre). Rend ce qui n'a pas pu etre donne.
    uint32 Completer(Player* joueur, uint32 entree, uint32 nombre)
    {
        uint32 const tenus = joueur->GetItemCount(entree, false);
        if (tenus >= nombre)
            return 0;
        uint32 const manque = nombre - tenus;
        // AddItem range ce qui tient et rend vrai meme si tout n'a pas tenu : on recompte toujours.
        joueur->AddItem(entree, manque);
        uint32 const apres = joueur->GetItemCount(entree, false);
        return apres >= nombre ? 0 : nombre - apres;
    }

    // Cases libres des sacs ordinaires (sac a dos et sacs sans specialite), la ou Completer et CreerEnSac rangent.
    uint32 CasesLibres(Player* joueur)
    {
        uint32 libres = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++libres;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* sac = joueur->GetBagByPos(slot))
                if (!sac->GetTemplate()->BagFamily)
                    libres += sac->GetFreeSlots();
        return libres;
    }

    // Cases que prend le complement de `entree` jusqu'a `nombre` (sans compter les piles entamees : par exces).
    uint32 CasesPour(Player* joueur, uint32 entree, uint32 nombre)
    {
        uint32 const tenus = joueur->GetItemCount(entree, false);
        if (tenus >= nombre)
            return 0;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entree);
        uint32 const pile = proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1;
        return (nombre - tenus + pile - 1) / pile;
    }

    // Pose des sacs vides (SAC_VIDE) dans les emplacements de sac libres tant qu'il manque des cases. Rend le nombre pose.
    uint32 PoserSacs(Player* joueur, uint32 besoin)
    {
        uint32 poses = 0;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END && CasesLibres(joueur) < besoin; ++slot)
        {
            if (joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
            uint16 dest = 0;
            if (joueur->CanEquipNewItem(slot, dest, SAC_VIDE, false) != EQUIP_ERR_OK)
                continue;
            if (Item* sac = joueur->EquipNewItem(dest, SAC_VIDE, true))
            {
                joueur->SendNewItem(sac, 1, true, false);
                ++poses;
            }
        }
        return poses;
    }

    Item* CreerEnSac(Player* joueur, uint32 entree)
    {
        ItemPosCountVec dest;
        if (joueur->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entree, 1) != EQUIP_ERR_OK)
            return nullptr;
        Item* objet = joueur->StoreNewItem(dest, entree, true);
        if (objet)
            joueur->SendNewItem(objet, 1, true, false);
        return objet;
    }

    // Met en place une piece tenue en sac ; ce qui occupait l'emplacement prend sa place dans le sac.
    bool Porter(Player* joueur, Item* objet)
    {
        uint16 dest = 0;
        if (joueur->CanEquipItem(NULL_SLOT, dest, objet, true) != EQUIP_ERR_OK)
            return false;
        joueur->SwapItem(objet->GetPos(), dest);
        return objet->IsEquipped();
    }

    void PoserEnchantement(Player* joueur, Item* objet, EnchantmentSlot slot, uint32 id, uint32 duree)
    {
        bool const portee = objet->IsEquipped();
        if (portee)
            joueur->ApplyEnchantment(objet, slot, false);
        objet->SetEnchantment(slot, id, duree, 0, joueur->GetGUID());
        if (portee)
            joueur->ApplyEnchantment(objet, slot, true);
        objet->SetState(ITEM_CHANGED, joueur);
    }

    // Enchantement que pose un objet d'enchantement (parchemin, arcane, huile), lu dans son sort.
    uint32 EnchantementDeObjet(uint32 entree)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entree);
        if (!proto)
            return 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (SpellInfo const* sort = sSpellMgr->GetSpellInfo(proto->Spells[i].SpellId))
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                    if (sort->Effects[e].Effect == SPELL_EFFECT_ENCHANT_ITEM
                        || sort->Effects[e].Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                        return uint32(sort->Effects[e].MiscValue);
        return 0;
    }

    uint32 EnchantementDeGemme(uint32 entree)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entree);
        if (!proto || !proto->GemProperties)
            return 0;
        GemPropertiesEntry const* gemme = sGemPropertiesStore.LookupEntry(proto->GemProperties);
        return gemme ? gemme->spellitemenchantement : 0;
    }

    // Sertit chaque chasse (meta : 41398, sinon 40111) et recalcule le bonus de chasse.
    void Sertir(Player* joueur, Item* objet)
    {
        ItemTemplate const* proto = objet->GetTemplate();
        for (uint32 i = 0; i < MAX_GEM_SOCKETS; ++i)
            if (uint32 const couleur = proto->Socket[i].Color)
                PoserEnchantement(joueur, objet, EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + i),
                    EnchantementDeGemme((couleur & SOCKET_COLOR_META) ? GEMME_META : GEMME_ROUGE), 0);
        PoserEnchantement(joueur, objet, BONUS_ENCHANTMENT_SLOT, objet->GemsFitSockets() ? proto->socketBonus : 0, 0);
    }

    void SignerPar(Player* joueur, Item* objet)
    {
        if (objet->GetTemplate()->HasSignature())
        {
            objet->SetGuidValue(ITEM_FIELD_CREATOR, joueur->GetGUID());
            objet->SetState(ITEM_CHANGED, joueur);
        }
    }

    // Recettes de Forge du lot 1 : sorts de la plage reservee rattaches a la ligne 164 (SkillLineAbility).
    std::vector<uint32> RecettesForge()
    {
        std::vector<uint32> recettes;
        for (uint32 id = RECETTE_MIN; id <= RECETTE_MAX; ++id)
        {
            if (!sSpellMgr->GetSpellInfo(id))
                continue;
            auto bornes = sSpellMgr->GetSkillLineAbilityMapBounds(id);
            for (auto it = bornes.first; it != bornes.second; ++it)
                if (it->second->SkillLine == SKILL_FORGE)
                {
                    recettes.push_back(id);
                    break;
                }
        }
        return recettes;
    }

    // Comme .character level : le crochet de niveau passe. Rend faux si un script a refuse le niveau.
    bool MettreAuNiveau(Player* joueur, uint8 niveau)
    {
        if (joueur->GetLevel() == niveau)
            return true;
        joueur->GiveLevel(niveau);
        joueur->InitTalentForLevel();
        joueur->SetUInt32Value(PLAYER_XP, 0);
        return joueur->GetLevel() == niveau;
    }

    void Forge(Player* joueur, uint16 valeur)
    {
        for (uint32 rang : RANGS_FORGE)
            if (!joueur->HasSpell(rang))
                joueur->learnSpell(rang);
        // Rang 6 (max 450) en dur : apprendre un rang inferieur a un rang connu laisse GetSkillStep sur ce rang-la.
        joueur->SetSkill(SKILL_FORGE, uint16(RANGS_FORGE.size()), valeur, 450);
    }

    // Texte d'une piece pour les comptes rendus : entree, guid, fabricant, emplacements non vides.
    std::string Decrire(Item const* objet)
    {
        std::string texte = Acore::StringFormat("{} (guid {}) fabricant={}", objet->GetEntry(),
            objet->GetGUID().GetCounter(), objet->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter());
        for (uint8 s = PERM_ENCHANTMENT_SLOT; s <= PRISMATIC_ENCHANTMENT_SLOT; ++s)
            if (uint32 const id = objet->GetEnchantmentId(EnchantmentSlot(s)))
                texte += Acore::StringFormat(" [{}]={}/{}ms", uint32(s), id, objet->GetEnchantmentDuration(EnchantmentSlot(s)));
        return texte;
    }

    Item* PremierTenu(Player* joueur, uint32 entree)
    {
        Item* trouve = nullptr;
        PourChaquePieceTenue(joueur, entree, [&](Item* objet) { if (!trouve) trouve = objet; });
        return trouve;
    }

    // Bonus de set suspendu : temoin du banc, pour mesurer ce que le cœur fait seul au passage de niveau.
    bool RecalculSuspendu = false;

    // ---- Scripts --------------------------------------------------------------------------------------------------

    class ArtisanatWorld final : public WorldScript
    {
    public:
        ArtisanatWorld() : WorldScript("CoaArtisanatWorld", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            ChargerVersionsEveillees();
        }
    };

    class ArtisanatPlayer final : public PlayerScript
    {
    public:
        ArtisanatPlayer() : PlayerScript("CoaArtisanatPlayer", { PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_CREATE_ITEM,
            PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_BEFORE_SEND_LOOT }) { }

        void OnPlayerSpellCast(Player* joueur, Spell* sort, bool /*skipCheck*/) override
        {
            // Un sort sans rapport (proc « a l'incantation », sort declenche) peut passer par ce crochet entre la
            // photographie et la creation : il ne doit pas l'effacer. La garde instant + entree suffit contre un
            // etat perime.
            SpellInfo const* info = sort->GetSpellInfo();
            uint32 const creee = EntreeCreeeArtisanat(info);
            if (!creee)
                return;

            RecopieEnAttente* attente = joueur->CustomData.GetDefault<RecopieEnAttente>(CLE_DONNEES);
            attente->Vider();
            attente->sort = info->Id;
            attente->entreeCreee = creee;
            attente->instant = GameTime::GetGameTimeMS();

            // Renforcement : la piece est l'objet de lancement.
            if (Item* objet = sort->m_CastItem)
            {
                if (EstPieceArtisanat(objet->GetTemplate()) && objet->GetOwnerGUID() == joueur->GetGUID()
                    && ObjetDeLancementConsomme(sort, objet))
                {
                    attente->objetDeLancement = true;
                    attente->candidats.push_back(Photographier(joueur, objet));
                    return;
                }
            }

            // Eveil par sort : la piece est un reactif. Toutes les pieces tenues de cette entree sont candidates ;
            // celle qui aura disparu a la creation est celle que TakeReagents a consommee.
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if (info->Reagent[i] <= 0 || info->ReagentCount[i] == 0)
                    continue;
                uint32 const reactif = uint32(info->Reagent[i]);
                if (!EstPieceArtisanat(sObjectMgr->GetItemTemplate(reactif)))
                    continue;
                PourChaquePieceTenue(joueur, reactif, [&](Item* piece)
                {
                    attente->candidats.push_back(Photographier(joueur, piece));
                });
            }
        }

        void OnPlayerCreateItem(Player* joueur, Item* objet, uint32 /*count*/) override
        {
            RecopieEnAttente* attente = joueur->CustomData.Get<RecopieEnAttente>(CLE_DONNEES);
            if (!attente || !attente->entreeCreee)
                return;
            // Meme tour de monde que la photographie, et la piece attendue : sinon c'est une autre creation.
            if (attente->instant != GameTime::GetGameTimeMS() || objet->GetEntry() != attente->entreeCreee)
            {
                attente->Vider();
                return;
            }

            Chasses const* source = nullptr;
            if (attente->objetDeLancement)
                source = &attente->candidats.front();
            else
                for (Chasses const& c : attente->candidats)
                    if (!joueur->GetItemByGuid(c.source))
                    {
                        source = &c;
                        break;
                    }

            if (source && !source->Vide())
            {
                uint32 perdus = 0;
                uint32 const recopies = Recopier(joueur, objet, *source, perdus);
                LOG_INFO("module", "coa artisanat: recopie joueur={} sort={} piece={} ({}) -> {} ({}) emplacements={} perdus={}",
                    joueur->GetGUID().GetCounter(), attente->sort, source->source.GetCounter(), source->entreeSource,
                    objet->GetGUID().GetCounter(), objet->GetEntry(), recopies, perdus);
            }
            else if (source && source->fabricant && objet->GetTemplate()->HasSignature())
            {
                objet->SetGuidValue(ITEM_FIELD_CREATOR, source->fabricant);
                objet->SetState(ITEM_CHANGED, joueur);
            }
            attente->Vider();
        }

        void OnPlayerLevelChanged(Player* joueur, uint8 /*ancien*/) override
        {
            if (!RecalculSuspendu)
                RecalculerBonusDeSet(joueur);
        }

        // L'ouverture compte, pas seulement la prise : un bot peut ouvrir un noeud et ne rien prendre (objet non
        // autorise, sacs pleins), le noeud est consomme quand meme.
        void OnPlayerBeforeSendLoot(Player* joueur, ObjectGuid source, Loot* /*butin*/) override
        {
            if (!source.IsGameObject() || !joueur->GetSession() || !joueur->GetSession()->IsBot())
                return;
            ++OuverturesBots;
            if (source.GetEntry() >= NOEUD_MIN && source.GetEntry() <= NOEUD_MAX)
            {
                ++OuverturesBotsNoeuds;
                LOG_ERROR("module", "coa artisanat: un bot a ouvert un noeud de l'artisanat joueur={} noeud={} entree={}",
                    joueur->GetGUID().GetCounter(), source.GetCounter(), source.GetEntry());
            }
        }

        void OnPlayerLootItem(Player* joueur, Item* /*objet*/, uint32 /*count*/, ObjectGuid source) override
        {
            if (!source.IsGameObject())
                return;
            bool const bot = joueur->GetSession() && joueur->GetSession()->IsBot();
            bool const noeud = source.GetEntry() >= NOEUD_MIN && source.GetEntry() <= NOEUD_MAX;
            if (bot)
                ++RecoltesBots;
            if (!noeud)
                return;
            ++(bot ? RecoltesBotsNoeuds : RecoltesJoueursNoeuds);
            if (bot)
                LOG_ERROR("module", "coa artisanat: un bot a recolte un noeud de l'artisanat joueur={} noeud={} entree={}",
                    joueur->GetGUID().GetCounter(), source.GetCounter(), source.GetEntry());
        }

        bool OnPlayerCanUseChat(Player* joueur, uint32 type, uint32 langue, std::string& message, Player* destinataire) override
        {
            if (type != CHAT_MSG_WHISPER || langue != LANG_ADDON || !message.starts_with(PREFIXE_ADDON))
                return true;
            // Seul « EVEIL: » est a ce script ; les autres messages du canal COA_ATL suivent leur chemin.
            std::string_view corps = std::string_view(message).substr(PREFIXE_ADDON.size());
            if (!corps.starts_with("EVEIL:"))
                return true;
            // Consomme meme desactive ; jamais relaye a un autre joueur.
            if (destinataire != joueur || message.size() > 255)
                return false;
            // Un message d'addon ne passe pas par UpdateSpeakTime : un delai minimal par joueur.
            Milliseconds const maintenant = GameTime::GetGameTimeMS();
            DelaiEveil* delai = joueur->CustomData.GetDefault<DelaiEveil>(CLE_DELAI);
            if (maintenant < delai->prochain)
                return false;
            delai->prochain = maintenant + DELAI_EVEIL;
            if (!sConfigMgr->GetOption<bool>("CoaArtisanat.EveilParMessage", true))
                Repondre(joueur, "EVEIL_ERR:DESACTIVE");
            else
                TraiterEveil(joueur, corps.substr(6));
            return false;
        }
    };

    // ---- Commandes : .artisanat essai (fiche de seance) et .artisanat banc (preuves serveur) ----------------------

    struct Etape
    {
        char const* titre;
        char const* prepare;
    };

    // Une etape par section de docs/LOT1-artisanat-seance.md.
    constexpr std::array<Etape, 9> ETAPES =
    {{
        { "Préparer", "niveau 80, Forge 1/450, 100 po, marteau et ingrédients du § 1" },
        { "Maître", "comme 1, recettes de Forge du lot 1 désapprises" },
        { "Fabriquer", "niveau 80, Forge 450/450, recettes apprises, marteau, ingrédients, 100 po" },
        { "Récolte", "niveau 80, Forge 450/450, recettes, Pic 5384, griffe 5059" },
        { "Niveau", "niveau 20 (redescendre efface les talents) ; en sac : Heaume, Solerets, Gantelets, Ceinture de base, Garde-poignets et Solerets 213, 45377, 46154" },
        { "Éveil", "niveau 80 ; Heaume de base porté + 2 en sac ; Garde-poignets de base" },
        { "Châsses", "niveau 80 ; Heaume de base « Fabriqué par » vous ; gemmes 41398 et 40111 ; arcane 44878 ; Garde-poignets de base ; parchemin 38771" },
        { "Rangs", "niveau 80 ; Heaume éveillé porté, serti, arcane posée, « Fabriqué par » vous ; réactifs des 5 rangs" },
        { "Bascule", "niveau 80, Forge 450/450, recettes apprises" },
    }};

    // Cases de sac que prend l'etape `etape` de .artisanat essai : meme logique que HandleEssai, sans rien donner.
    // Une piece creee puis portee prend une case (l'ancienne piece revient a sa place) ; une piece tenue, aucune.
    uint32 CasesEssai(Player* joueur, uint32 etape)
    {
        uint32 cases = 0;
        auto liste = [&](auto const& quantites)
        {
            for (Quantite const& q : quantites)
                cases += CasesPour(joueur, q.entree, q.nombre);
        };
        Item const* tete = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD);
        switch (etape)
        {
            case 1:
            case 2:
            case 3:
                liste(INGREDIENTS_SEANCE);
                break;
            case 4:
                cases += CasesPour(joueur, 5384, 1) + CasesPour(joueur, 5059, 1);
                break;
            case 5:
                for (uint32 entree : { HEAUME_BASE, 9101475u, 9101482u, 9101489u, 9101469u, 9101476u, 45377u, 46154u })
                    cases += CasesPour(joueur, entree, 1);
                break;
            case 6:
            {
                bool const basePortee = tete && tete->GetEntry() == HEAUME_BASE;
                uint32 const enSac = joueur->GetItemCount(HEAUME_BASE, false) - (basePortee ? 1 : 0);
                cases += (basePortee ? 0 : 1) + (enSac < 2 ? 2 - enSac : 0) + CasesPour(joueur, GARDE_POIGNETS_BASE, 1);
                break;
            }
            case 7:
                cases += PremierTenu(joueur, HEAUME_BASE) ? 0 : 1;
                for (uint32 entree : { GEMME_META, GEMME_ROUGE, ARCANE, GARDE_POIGNETS_BASE, PARCHEMIN })
                    cases += CasesPour(joueur, entree, 1);
                break;
            case 8:
                cases += PremierTenu(joueur, HEAUME_EVEILLE) ? 0 : 1;
                liste(REACTIFS_RANGS);
                break;
        }
        return cases;
    }

    class ArtisanatCommandes final : public CommandScript
    {
    public:
        ArtisanatCommandes() : CommandScript("CoaArtisanatCommandes") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable banc =
            {
                { "etat", HandleBancEtat, SEC_ADMINISTRATOR, Console::Yes },
                { "recopie", HandleBancRecopie, SEC_ADMINISTRATOR, Console::Yes },
                { "set", HandleBancSet, SEC_ADMINISTRATOR, Console::Yes },
                { "nettoyer", HandleBancNettoyer, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable artisanat =
            {
                { "essai", HandleEssai, SEC_GAMEMASTER, Console::No },
                { "banc", banc },
            };
            static ChatCommandTable racine = { { "artisanat", artisanat } };
            return racine;
        }

        // ---- .artisanat essai [etape] ----------------------------------------------------------------------------

        static bool HandleEssai(ChatHandler* handler, Optional<uint32> etape)
        {
            if (!BancPermis(handler))
                return true;
            if (!etape || *etape < 1 || *etape > ETAPES.size())
            {
                handler->SendSysMessage("Banc d'essai de l'artisanat : .artisanat essai <étape>, puis téléportation dans la salle d'essai.");
                for (std::size_t i = 0; i < ETAPES.size(); ++i)
                    handler->PSendSysMessage("  {} — {} : {}", i + 1, ETAPES[i].titre, ETAPES[i].prepare);
                return true;
            }
            Player* joueur = handler->GetPlayer();
            if (!joueur || joueur->IsInCombat() || !joueur->IsAlive())
            {
                handler->SendSysMessage("Hors combat et en vie seulement.");
                return true;
            }

            // Avant de rien donner : les cases qu'il faut, des sacs vides s'il en manque, sinon refus (rien n'est change).
            uint32 const besoin = CasesEssai(joueur, *etape);
            uint32 const sacs = CasesLibres(joueur) < besoin ? PoserSacs(joueur, besoin) : 0;
            if (sacs)
                handler->PSendSysMessage("{} sac(s) vide(s) de 20 cases posé(s) pour faire de la place.", sacs);
            if (uint32 const libres = CasesLibres(joueur); libres < besoin)
            {
                handler->PSendSysMessage(sacs ? "Faites de la place : il faut {} cases ({} libres, sacs posés compris). Rien d'autre n'a été donné."
                    : "Faites de la place : il faut {} cases ({} libres). Rien n'a été donné.", besoin, libres);
                return true;
            }

            std::vector<std::string> manques;
            auto completer = [&](uint32 entree, uint32 nombre)
            {
                if (uint32 const reste = Completer(joueur, entree, nombre))
                    manques.push_back(Acore::StringFormat("{} x{}", entree, reste));
            };
            auto creer = [&](uint32 entree) -> Item*
            {
                Item* objet = CreerEnSac(joueur, entree);
                if (!objet)
                    manques.push_back(Acore::StringFormat("{} (sacs pleins)", entree));
                return objet;
            };
            auto porter = [&](Item* objet)
            {
                if (objet && !objet->IsEquipped() && !Porter(joueur, objet))
                    manques.push_back(Acore::StringFormat("{} non porté (laissé en sac)", objet->GetEntry()));
            };
            auto niveau = [&](uint8 cible)
            {
                if (!MettreAuNiveau(joueur, cible))
                    manques.push_back(Acore::StringFormat("niveau {} refusé (resté {})", uint32(cible), uint32(joueur->GetLevel())));
            };
            auto recettes = [&](bool apprendre)
            {
                for (uint32 id : RecettesForge())
                {
                    if (apprendre && !joueur->HasSpell(id))
                        joueur->learnSpell(id);
                    else if (!apprendre && joueur->HasSpell(id))
                        joueur->removeSpell(id, SPEC_MASK_ALL, false);
                }
            };
            auto ingredients = [&]
            {
                for (Quantite const& q : INGREDIENTS_SEANCE)
                    completer(q.entree, q.nombre);
                if (joueur->GetMoney() < OR_MINIMUM)
                    joueur->ModifyMoney(int32(OR_MINIMUM - joueur->GetMoney()));
            };

            switch (*etape)
            {
                case 1:
                case 2:
                    niveau(80);
                    Forge(joueur, 1);
                    ingredients();
                    if (*etape == 2)
                        recettes(false);
                    break;
                case 3:
                case 9:
                    niveau(80);
                    Forge(joueur, 450);
                    recettes(true);
                    if (*etape == 3)
                        ingredients();
                    break;
                case 4:
                    niveau(80);
                    Forge(joueur, 450);
                    recettes(true);
                    completer(5384, 1);
                    completer(5059, 1);
                    break;
                case 5:
                    niveau(20);
                    for (uint32 entree : { HEAUME_BASE, 9101475u, 9101482u, 9101489u, 9101469u, 9101476u, 45377u, 46154u })
                        completer(entree, 1);
                    break;
                case 6:
                {
                    niveau(80);
                    Item* porte = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD);
                    if (!porte || porte->GetEntry() != HEAUME_BASE)
                        porter(creer(HEAUME_BASE));
                    // Le double : deux Heaumes de base en sac en plus de celui qui est porte.
                    uint32 const enSac = joueur->GetItemCount(HEAUME_BASE, false) - (joueur->GetItemByPos(INVENTORY_SLOT_BAG_0,
                        EQUIPMENT_SLOT_HEAD) && joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD)->GetEntry() == HEAUME_BASE ? 1 : 0);
                    for (uint32 i = enSac; i < 2; ++i)
                        creer(HEAUME_BASE);
                    completer(GARDE_POIGNETS_BASE, 1);
                    break;
                }
                case 7:
                    niveau(80);
                    // Un Heaume de base signe suffit : on n'en cree un que s'il n'y en a aucun.
                    if (!PremierTenu(joueur, HEAUME_BASE))
                        if (Item* heaume = creer(HEAUME_BASE))
                            SignerPar(joueur, heaume);
                    for (uint32 entree : { GEMME_META, GEMME_ROUGE, ARCANE, GARDE_POIGNETS_BASE, PARCHEMIN })
                        completer(entree, 1);
                    break;
                case 8:
                {
                    niveau(80);
                    // Le Heaume eveille deja tenu est repris (sans cumul), sinon cree ; il est mis en etat puis porte.
                    Item* heaume = PremierTenu(joueur, HEAUME_EVEILLE);
                    if (!heaume)
                        heaume = creer(HEAUME_EVEILLE);
                    if (heaume)
                    {
                        SignerPar(joueur, heaume);
                        Sertir(joueur, heaume);
                        PoserEnchantement(joueur, heaume, PERM_ENCHANTMENT_SLOT, EnchantementDeObjet(ARCANE), 0);
                        porter(heaume);
                    }
                    for (Quantite const& q : REACTIFS_RANGS)
                        completer(q.entree, q.nombre);
                    break;
                }
            }

            joueur->TeleportTo(SALLE_CARTE, SALLE_X, SALLE_Y, SALLE_Z, SALLE_O);
            handler->PSendSysMessage("Étape {} « {} » prête : {}.", *etape, ETAPES[*etape - 1].titre, ETAPES[*etape - 1].prepare);
            for (std::string const& m : manques)
                handler->PSendSysMessage("  non fait : {}", m);
            LOG_INFO("module", "coa artisanat: banc d'essai etape={} joueur={} manques={}", *etape,
                joueur->GetGUID().GetCounter(), uint32(manques.size()));
            return true;
        }

        // ---- .artisanat banc ... (console comprise) : preuves serveur sur un personnage en ligne, un bot ----------

        static Player* Cible(ChatHandler* handler, std::string const& nom)
        {
            Player* joueur = ObjectAccessor::FindPlayerByName(nom, true);
            if (!joueur)
                handler->PSendSysMessage("banc: {} n'est pas en ligne.", nom);
            else if (!joueur->GetSession() || !joueur->GetSession()->IsBot())
            {
                // Le banc detruit et donne des objets, change le niveau : jamais sur le personnage d'un joueur.
                handler->PSendSysMessage("banc: {} n'est pas un bot.", nom);
                return nullptr;
            }
            else if (joueur->IsInCombat() || !joueur->IsAlive() || joueur->IsBeingTeleported())
            {
                handler->PSendSysMessage("banc: {} est en combat, mort ou en téléportation.", nom);
                return nullptr;
            }
            return joueur;
        }

        static bool HandleBancEtat(ChatHandler* handler)
        {
            if (!BancPermis(handler))
                return true;
            handler->PSendSysMessage("banc etat: depuis le demarrage, bots : {} butins de gameobjects ouverts (dont noeuds {}-{} : {}), {} objets pris (dont noeuds : {}) ; joueurs : {} objets pris sur ces noeuds",
                OuverturesBots.load(), NOEUD_MIN, NOEUD_MAX, OuverturesBotsNoeuds.load(), RecoltesBots.load(),
                RecoltesBotsNoeuds.load(), RecoltesJoueursNoeuds.load());
            return true;
        }

        // Phase 1 : Heaume de base fabrique par le personnage, serti, enchantement permanent (parchemin) et temporaire
        // (huile, 30 min). Phase 2 : eveil (refus au niveau 79, puis eveil). Phase 3 : rangs 1-5 depuis la piece portee
        // (refus sans reactifs et au niveau 79, puis cout preleve et recopie a chaque rang). Chaque phase sauvegarde
        // le personnage : item_instance se lit entre deux phases.
        static bool HandleBancRecopie(ChatHandler* handler, std::string nom, uint32 phase)
        {
            if (!BancPermis(handler))
                return true;
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            if (j->GetLevel() != NIVEAU_EVEIL)
            {
                handler->PSendSysMessage("banc recopie: {} doit etre niveau 80 (il est {}).", nom, uint32(j->GetLevel()));
                return true;
            }

            if (phase == 1)
            {
                Item* heaume = CreerEnSac(j, HEAUME_BASE);
                if (!heaume)
                    return handler->SendSysMessage("banc recopie 1: sacs pleins."), true;
                SignerPar(j, heaume);
                Sertir(j, heaume);
                PoserEnchantement(j, heaume, PERM_ENCHANTMENT_SLOT, EnchantementDeObjet(PARCHEMIN), 0);
                PoserEnchantement(j, heaume, TEMP_ENCHANTMENT_SLOT, EnchantementDeObjet(HUILE), 30 * MINUTE * IN_MILLISECONDS);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc recopie 1: base {} ; joueur guid {}", Decrire(heaume), j->GetGUID().GetCounter());
                return true;
            }

            if (phase == 2)
            {
                Item* heaume = PremierTenu(j, HEAUME_BASE);
                if (!heaume)
                    return handler->SendSysMessage("banc recopie 2: aucun Heaume de base (phase 1 d'abord)."), true;
                Chasses const avant = Photographier(j, heaume);
                uint32 const bonusAvant = heaume->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT);
                RefusEveil refus;
                j->SetLevel(NIVEAU_EVEIL - 1);
                Item* rate = Eveiller(j, heaume, refus, "banc");
                j->SetLevel(NIVEAU_EVEIL);
                handler->PSendSysMessage("banc recopie 2: niveau 79 -> {} (attendu NIVEAU), piece intacte={}",
                    rate ? "EVEILLE" : refus.cause, j->GetItemByGuid(avant.source) ? "oui" : "non");
                Item* neuve = rate ? rate : Eveiller(j, heaume, refus, "banc");
                if (!neuve)
                    return handler->PSendSysMessage("banc recopie 2: eveil refuse : {}", refus.cause), true;
                handler->PSendSysMessage("banc recopie 2: eveillee {} ; recopie {} ; base detruite={}", Decrire(neuve),
                    Conforme(avant, bonusAvant, neuve) ? "CONFORME" : "ECART", j->GetItemByGuid(avant.source) ? "non" : "oui");
                j->SaveToDB(false, false);
                return true;
            }

            if (phase == 3)
            {
                Item* depart = PremierTenu(j, HEAUME_EVEILLE);
                if (!depart)
                    return handler->SendSysMessage("banc recopie 3: aucun Heaume eveille (phases 1 et 2 d'abord)."), true;
                // Le casque d'origine prend la place du Heaume en sac ; il revient a la fin, sur tous les chemins.
                uint16 const placeOrigine = depart->IsEquipped() ? uint16(0) : depart->GetPos();
                if (!depart->IsEquipped() && !Porter(j, depart))
                    return handler->SendSysMessage("banc recopie 3: le Heaume eveille ne se porte pas."), true;
                auto remettre = [&]
                {
                    if (placeOrigine)
                    {
                        Item* origine = j->GetItemByPos(placeOrigine);
                        if (origine && !EstObjetArtisanat(origine->GetEntry()))
                        {
                            if (Item* tete = j->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD))
                                j->SwapItem(tete->GetPos(), placeOrigine);
                            else
                                Porter(j, origine);
                        }
                    }
                    j->SaveToDB(false, false);
                };

                // La piece suivie par son guid : un sort qui passe consomme l'objet de lancement (TakeCastItem).
                ObjectGuid courant = depart->GetGUID();
                TriggerCastFlags const drapeaux = TriggerCastFlags(TRIGGERED_CAST_DIRECTLY | TRIGGERED_IGNORE_GCD
                    | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD);
                auto lancer = [&]() -> SpellCastResult
                {
                    Item* piece = j->GetItemByGuid(courant);
                    SpellInfo const* sort = piece ? sSpellMgr->GetSpellInfo(piece->GetTemplate()->Spells[0].SpellId) : nullptr;
                    return sort ? j->CastSpell(j, sort, drapeaux, piece) : SPELL_FAILED_UNKNOWN;
                };

                // Refus : sans reactifs, puis au niveau 79. Un refus qui n'a pas lieu consomme la piece : on s'arrete.
                for (Quantite const& q : REACTIFS_RANGS)
                    if (uint32 const n = j->GetItemCount(q.entree, false))
                        j->DestroyItemCount(q.entree, n, true);
                SpellCastResult r = lancer();
                bool intacte = j->GetItemByGuid(courant) != nullptr;
                handler->PSendSysMessage("banc recopie 3: sans reactifs -> resultat {} (attendu {} REAGENTS), piece intacte={}",
                    uint32(r), uint32(SPELL_FAILED_REAGENTS), intacte ? "oui" : "NON");
                if (!intacte)
                    return remettre(), true;
                for (Quantite const& q : REACTIFS_RANGS)
                    Completer(j, q.entree, q.nombre);
                j->SetLevel(NIVEAU_EVEIL - 1);
                r = lancer();
                j->SetLevel(NIVEAU_EVEIL);
                intacte = j->GetItemByGuid(courant) != nullptr;
                handler->PSendSysMessage("banc recopie 3: niveau 79 -> resultat {} (condition : 172), piece intacte={}, reactifs intacts={}",
                    uint32(r), intacte ? "oui" : "NON", j->GetItemCount(9100101, false) == 24 ? "oui" : "NON");
                if (!intacte)
                    return remettre(), true;

                for (uint32 rang = 1; rang <= 5; ++rang)
                {
                    Item* piece = j->GetItemByGuid(courant);
                    SpellInfo const* sort = piece ? sSpellMgr->GetSpellInfo(piece->GetTemplate()->Spells[0].SpellId) : nullptr;
                    if (!sort)
                        return handler->PSendSysMessage("banc recopie 3: rang {} sans piece ou sans sort de renforcement.", rang), remettre(), true;
                    if (!piece->IsEquipped() && !Porter(j, piece))
                        return handler->PSendSysMessage("banc recopie 3: rang {} : la piece ne se porte pas.", rang), remettre(), true;
                    uint32 const attendu = EntreeCreeeArtisanat(sort);
                    std::vector<ObjectGuid> dejaLa;
                    PourChaquePieceTenue(j, attendu, [&](Item* o) { dejaLa.push_back(o->GetGUID()); });
                    std::array<uint32, MAX_SPELL_REAGENTS> avantReactifs{};
                    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                        if (sort->Reagent[i] > 0)
                            avantReactifs[i] = j->GetItemCount(uint32(sort->Reagent[i]), false);
                    Chasses const avant = Photographier(j, piece);
                    uint32 const bonusAvant = piece->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT);
                    r = lancer();
                    Item* neuve = nullptr;    // la piece apparue au lancement, pas une restee d'un passage precedent
                    PourChaquePieceTenue(j, attendu, [&](Item* o)
                    {
                        if (!neuve && std::find(dejaLa.begin(), dejaLa.end(), o->GetGUID()) == dejaLa.end())
                            neuve = o;
                    });
                    std::string couts;
                    bool coutJuste = true;
                    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    {
                        if (sort->Reagent[i] <= 0)
                            continue;
                        uint32 const pris = avantReactifs[i] - j->GetItemCount(uint32(sort->Reagent[i]), false);
                        coutJuste = coutJuste && pris == sort->ReagentCount[i];
                        couts += Acore::StringFormat(" {}:{}/{}", sort->Reagent[i], pris, sort->ReagentCount[i]);
                    }
                    if (r != SPELL_CAST_OK || !neuve)
                        return handler->PSendSysMessage("banc recopie 3: rang {} sort {} -> resultat {}, piece {} absente ; couts{}",
                            rang, sort->Id, uint32(r), attendu, couts), remettre(), true;
                    handler->PSendSysMessage("banc recopie 3: rang {} sort {} -> {} niveau d'objet {} ; recopie {} ; ancienne detruite={} ; couts{} {}",
                        rang, sort->Id, Decrire(neuve), neuve->GetTemplate()->ItemLevel,
                        Conforme(avant, bonusAvant, neuve) ? "CONFORME" : "ECART", j->GetItemByGuid(avant.source) ? "non" : "oui",
                        couts, coutJuste ? "JUSTES" : "FAUX");
                    courant = neuve->GetGUID();
                }
                remettre();
                return true;
            }
            handler->SendSysMessage("banc recopie <nom> <1|2|3>");
            return true;
        }

        // Les emplacements gardes : PERM, TEMP (id), SOCK1-3, bonus de chasse, fabricant.
        static bool Conforme(Chasses const& avant, uint32 bonusAvant, Item const* apres)
        {
            for (std::size_t i = 0; i < EMPLACEMENTS_RECOPIES.size(); ++i)
                if (avant.emplacements[i].id != apres->GetEnchantmentId(EMPLACEMENTS_RECOPIES[i]))
                    return false;
            return bonusAvant == apres->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT)
                && avant.fabricant == apres->GetGuidValue(ITEM_FIELD_CREATOR);
        }

        // Bonus de set au passage de niveau : 4 pieces de base du set 63004 portees, montant des deux auras (9112012
        // a 2 pieces, 9112013 a 4) au niveau N, a N+1 (GiveLevel, crochet actif), puis temoin a N+2 crochet suspendu
        // (ce que fait le cœur seul), puis recalcul. On ne fait que MONTER : redescendre remet les talents a zero
        // (InitTalentForLevel). Le personnage garde donc N+2 ; son equipement d'origine revient.
        static bool HandleBancSet(ChatHandler* handler, std::string nom)
        {
            if (!BancPermis(handler))
                return true;
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            uint8 const n = j->GetLevel();
            if (n < 20 || n + 2 > NIVEAU_EVEIL)
                return handler->PSendSysMessage("banc set: {} doit etre entre les niveaux 20 et 78 (il est {}).", nom, uint32(n)), true;
            // Suivies par guid : un passage de niveau peut faire agir d'autres scripts sur l'inventaire.
            std::vector<std::pair<ObjectGuid, uint16>> pieces;   // piece, sa place en sac avant d'etre portee
            for (uint32 entree : { 9101475u, 9101482u, 9101489u, GARDE_POIGNETS_BASE })
                if (Item* objet = CreerEnSac(j, entree))
                    pieces.emplace_back(objet->GetGUID(), objet->GetPos());
            uint32 portees = 0;
            for (auto const& [g, place] : pieces)
                if (Item* p = j->GetItemByGuid(g))
                    portees += Porter(j, p) ? 1 : 0;
            auto montants = [&]
            {
                std::string texte = Acore::StringFormat(" (niveau {})", uint32(j->GetLevel()));
                for (uint32 id : { 9112012u, 9112013u })
                {
                    Aura const* aura = j->GetAura(id, j->GetGUID());
                    AuraEffect const* effet = aura ? aura->GetEffect(EFFECT_0) : nullptr;
                    texte += effet ? Acore::StringFormat(" {}={}", id, effet->GetAmount()) : Acore::StringFormat(" {}=absente", id);
                }
                return texte;
            };
            handler->PSendSysMessage("banc set: {} classe {} : {} pieces sur 4 portees ; depart :{}", nom,
                uint32(j->getClass()), portees, montants());
            if (portees == 4)
            {
                bool const monte1 = MettreAuNiveau(j, n + 1);
                handler->PSendSysMessage("banc set: +1, crochet actif{} :{}", monte1 ? "" : " (NIVEAU REFUSE)", montants());
                RecalculSuspendu = true;
                bool const monte2 = MettreAuNiveau(j, n + 2);
                RecalculSuspendu = false;
                handler->PSendSysMessage("banc set: +2, crochet SUSPENDU (le coeur seul){} :{}", monte2 ? "" : " (NIVEAU REFUSE)", montants());
                RecalculerBonusDeSet(j);
                handler->PSendSysMessage("banc set: apres recalcul :{}", montants());
            }

            // Remise en etat : l'equipement d'origine revient a sa place, les pieces du banc disparaissent.
            uint32 rendues = 0, restees = 0;
            for (auto const& [g, place] : pieces)
            {
                Item* p = j->GetItemByGuid(g);
                if (!p)
                    continue;
                if (p->IsEquipped())
                    j->SwapItem(p->GetPos(), place);
                if (p->IsEquipped())
                    ++restees;     // echange refuse : la piece du banc reste portee, on ne la detruit pas
                else
                {
                    j->DestroyItem(p->GetBagSlot(), p->GetSlot(), true);
                    ++rendues;
                }
            }
            j->SaveToDB(false, false);
            handler->PSendSysMessage("banc set: {} pieces retirees, {} restees portees (echange refuse) ; niveau {}.",
                rendues, restees, uint32(j->GetLevel()));
            return true;
        }

        static bool HandleBancNettoyer(ChatHandler* handler, std::string nom)
        {
            if (!BancPermis(handler))
                return true;
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            uint32 detruits = 0;
            for (uint32 entree = ARTISANAT_OBJET_MIN; entree <= ARTISANAT_OBJET_MAX; ++entree)
                if (sObjectMgr->GetItemTemplate(entree))
                    if (uint32 const n = j->GetItemCount(entree, false))
                    {
                        j->DestroyItemCount(entree, n, true);
                        detruits += n;
                    }
            for (Quantite const& q : REACTIFS_RANGS)
                if (uint32 const n = j->GetItemCount(q.entree, false))
                {
                    j->DestroyItemCount(q.entree, n, true);
                    detruits += n;
                }
            j->SaveToDB(false, false);
            handler->PSendSysMessage("banc nettoyer: {} objets detruits.", detruits);
            return true;
        }
    };
}

void AddAscensionArtisanatScripts()
{
    new ArtisanatWorld();
    new ArtisanatPlayer();
    new ArtisanatCommandes();
}
