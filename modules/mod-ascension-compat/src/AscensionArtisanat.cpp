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
// 4. Paliers et eveil payants (D-PO-24) : message COA_ATL « AMELIORER » (paliers) et « EVEIL », etapes et couts lus dans
//    acore_world.coa_artisanat_etape (ecrite par PartB). Reactifs et or verifies avant, preleves apres la creation.
//    CoaArtisanat.AmeliorerParMessage = 0 coupe AMELIORER. Sans table : eveil gratuit par grille, AMELIORER refuse.
// 5. Materiaux de monde (D-PO-19, D-PO-23) : vert, rare, epique ajoutes au butin des creatures qui ont une table de
//    butin et rapportent de l'experience au detenteur du butin. Taux en configuration (CoaArtisanat.Monde.*), 0 par
//    defaut ; bots exclus sauf CoaArtisanat.Monde.TauxBots.

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Formulas.h"
#include "Group.h"
#include "Item.h"
#include "LootMgr.h"
#include "Map.h"
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
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Plages reservees (CONCEPTION-artisanat-A-cadre.md, « Plages d'ids reservees »).
    constexpr uint32 ARTISANAT_OBJET_MIN = 9101200;
    constexpr uint32 ARTISANAT_OBJET_MAX = 9104999;   // D-PO-22
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

    // Emplacements recopies. BONUS_ENCHANTMENT_SLOT (bonus de chasse) se recalcule sur la piece neuve. Les
    // PROP_ENCHANTMENT_SLOT_* servent aux suffixes aleatoires, que nos pieces n'ont pas : le premier porte la signature
    // de l'artisan (lot T, D-PO-34), qui suit la piece aux paliers, a l'eveil et aux rangs.
    constexpr std::array<EnchantmentSlot, 7> EMPLACEMENTS_RECOPIES =
    {
        PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT_2,
        SOCK_ENCHANTMENT_SLOT_3, PRISMATIC_ENCHANTMENT_SLOT, PROP_ENCHANTMENT_SLOT_0
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

    // ---- Etapes payantes : paliers (AMELIORER) et eveil (EVEIL), table coa_artisanat_etape (D-PO-24) ----------------

    // Une ligne de coa_artisanat_etape, ecrite par PartB (monde.sql). `de` : la piece de depart, une etape au plus.
    struct EtapeArtisanat
    {
        uint32 vers = 0;
        bool eveil = false;                           // type 'eveil' ; sinon 'palier'
        uint8 niveau = 0;
        uint32 orCuivre = 0;
        std::array<std::pair<uint32, uint32>, 4> objets{};   // (entree, nombre), entree 0 : aucun
    };
    std::unordered_map<uint32, EtapeArtisanat> Etapes;

    // Absente ou vide : le monde demarre, l'eveil retombe sur l'appariement par grille (gratuit), AMELIORER refuse.
    void ChargerEtapes()
    {
        Etapes.clear();
        if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_artisanat_etape'"))
        {
            LOG_INFO("module", "coa artisanat: table coa_artisanat_etape absente : eveil gratuit par grille, AMELIORER refuse");
            return;
        }
        uint32 paliers = 0, eveils = 0, rejetees = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT de, vers, CAST(type AS CHAR), niveau, or_cuivre, objet1, nombre1, objet2, "
            "nombre2, objet3, nombre3, objet4, nombre4 FROM coa_artisanat_etape"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const de = f[0].Get<uint32>();
                EtapeArtisanat e;
                e.vers = f[1].Get<uint32>();
                std::string const type = f[2].Get<std::string>();
                e.eveil = type == "eveil";
                e.niveau = f[3].Get<uint8>();
                e.orCuivre = f[4].Get<uint32>();
                bool valide = (type == "eveil" || type == "palier")
                    && EstPieceArtisanat(sObjectMgr->GetItemTemplate(de)) && EstPieceArtisanat(sObjectMgr->GetItemTemplate(e.vers))
                    && de != e.vers && e.niveau >= 1 && e.niveau <= DEFAULT_MAX_LEVEL;
                if (e.orCuivre > MAX_MONEY_AMOUNT)
                    valide = false;
                for (std::size_t i = 0; i < e.objets.size(); ++i)
                {
                    uint32 const objet = f[5 + 2 * i].Get<uint32>();
                    uint32 const nombre = f[6 + 2 * i].Get<uint16>();
                    if (!objet || !nombre)
                        continue;
                    if (objet == de || objet == e.vers || !sObjectMgr->GetItemTemplate(objet))
                        valide = false;
                    // Un meme objet sur deux colonnes : refuse (le controle colonne par colonne sous-compterait).
                    for (std::size_t k = 0; k < i; ++k)
                        if (e.objets[k].first == objet)
                            valide = false;
                    e.objets[i] = { objet, nombre };
                }
                if (!valide)
                {
                    ++rejetees;
                    LOG_ERROR("module", "coa artisanat: etape rejetee de={} vers={} type={} niveau={} (piece, objet ou niveau invalide)",
                        de, e.vers, type, uint32(e.niveau));
                    continue;
                }
                if (e.eveil)
                {
                    ++eveils;
                    auto it = VersionEveillee.find(de);
                    if (it != VersionEveillee.end() && it->second != e.vers)
                        LOG_WARN("module", "coa artisanat: eveil de {} : la table dit {}, l'appariement par grille {} ; la table l'emporte",
                            de, e.vers, it->second);
                }
                else
                    ++paliers;
                Etapes[de] = e;
            } while (r->NextRow());
        }
        LOG_INFO("module", "coa artisanat: coa_artisanat_etape : {} paliers, {} eveils, {} rejetees", paliers, eveils, rejetees);
    }

    enum class SorteEtape { Eveil, Palier };

    // Refus : la cause (repondue a l'addon) et l'avis montre au joueur (vide : aucun).
    struct RefusEveil
    {
        std::string cause;
        std::string avis;
    };

    // L'etape elle-meme, commune aux messages d'addon (EVEIL, AMELIORER) et au banc d'essai. Rend la piece neuve, ou
    // nullptr et la cause du refus. Rien n'est pris avant que la piece neuve existe : pas de perte sur un refus.
    Item* Avancer(Player* joueur, Item* piece, SorteEtape sorte, RefusEveil& refus, char const* voie)
    {
        auto refuser = [&](std::string cause, std::string avis = {}) -> Item*
        {
            refus = { std::move(cause), std::move(avis) };
            return nullptr;
        };
        bool const eveil = sorte == SorteEtape::Eveil;
        if (joueur->GetTradeData())
            return refuser("ECHANGE", "Fermez la fenêtre d'échange d'abord.");
        if (piece->IsInTrade())
            return refuser("PIECE");

        // L'etape : la table d'abord ; pour l'eveil seulement, l'appariement par grille (gratuit) en repli.
        EtapeArtisanat const* etape = nullptr;
        EtapeArtisanat repli;
        if (auto it = Etapes.find(piece->GetEntry()); it != Etapes.end() && it->second.eveil == eveil)
            etape = &it->second;
        else if (eveil && it == Etapes.end())
            if (auto v = VersionEveillee.find(piece->GetEntry()); v != VersionEveillee.end())
            {
                repli.vers = v->second;
                repli.eveil = true;
                repli.niveau = NIVEAU_EVEIL;
                etape = &repli;
            }
        if (!etape)
            return refuser("PIECE", eveil ? "Cette pièce ne peut pas être éveillée." : "Cette pièce ne peut pas être améliorée.");
        if (joueur->GetLevel() < etape->niveau)
            return refuser("NIVEAU", Acore::StringFormat("Il faut être niveau {} pour {} cette pièce.", uint32(etape->niveau),
                eveil ? "éveiller" : "améliorer"));
        if (!joueur->IsAlive())
            return refuser("MORT", "Impossible en étant mort.");
        if (joueur->IsInCombat())
            return refuser("COMBAT", "Impossible en combat.");

        // Reactifs et or, banque exclue. DestroyItemCount puise la banque en dernier recours seulement : le controle
        // ci-dessous garantit assez hors banque, et ChargerEtapes refuse un meme objet sur deux colonnes.
        std::string manques, noms;
        for (auto const& [objet, nombre] : etape->objets)
        {
            if (!objet)
                continue;
            uint32 const tenus = joueur->GetItemCount(objet, false);
            if (tenus >= nombre)
                continue;
            manques += Acore::StringFormat("{}{}={}", manques.empty() ? "" : ",", objet, nombre - tenus);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(objet);
            noms += Acore::StringFormat("{}{} x{}", noms.empty() ? "" : ", ", proto ? proto->Name1 : std::to_string(objet), nombre - tenus);
        }
        if (joueur->GetMoney() < etape->orCuivre)
        {
            uint32 const manque = etape->orCuivre - uint32(joueur->GetMoney());
            manques += Acore::StringFormat("{}or={}", manques.empty() ? "" : ",", manque);
            noms += Acore::StringFormat("{}{} po {} pa {} pc", noms.empty() ? "" : ", ", manque / 10000, (manque / 100) % 100, manque % 100);
        }
        if (!manques.empty())
            return refuser("REACTIFS:" + manques, "Il manque : " + noms + ".");

        ItemPosCountVec dest;
        InventoryResult const place = joueur->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, etape->vers, 1);
        if (place != EQUIP_ERR_OK)
        {
            joueur->SendEquipError(place, nullptr, nullptr, etape->vers);
            return refuser("SACS");
        }

        // Creer d'abord, detruire ensuite : un echec a la creation ne coute rien au joueur.
        Chasses const c = Photographier(joueur, piece);
        Item* neuve = joueur->StoreNewItem(dest, etape->vers, true);
        if (!neuve)
            return refuser("SACS");
        uint32 perdus = 0;
        uint32 const recopies = Recopier(joueur, neuve, c, perdus);
        joueur->SendNewItem(neuve, 1, true, false);
        uint32 const ancienne = piece->GetEntry();
        uint64 const ancienGuid = piece->GetGUID().GetCounter();
        joueur->DestroyItem(piece->GetBagSlot(), piece->GetSlot(), true);
        std::string cout;
        for (auto const& [objet, nombre] : etape->objets)
            if (objet)
            {
                joueur->DestroyItemCount(objet, nombre, true);
                cout += Acore::StringFormat(" {}x{}", objet, nombre);
            }
        if (etape->orCuivre)
        {
            joueur->ModifyMoney(-int32(etape->orCuivre));
            cout += Acore::StringFormat(" or={}", etape->orCuivre);
        }

        LOG_INFO("module", "coa artisanat: {} par {} joueur={} piece={} ({}) -> {} ({}) emplacements={} perdus={} cout={}",
            eveil ? "eveil" : "palier", voie, joueur->GetGUID().GetCounter(), ancienGuid, ancienne,
            neuve->GetGUID().GetCounter(), etape->vers, recopies, perdus, cout.empty() ? " gratuit" : cout);
        return neuve;
    }

    Item* Eveiller(Player* joueur, Item* piece, RefusEveil& refus, char const* voie)
    {
        return Avancer(joueur, piece, SorteEtape::Eveil, refus, voie);
    }

    // Messages : « EVEIL:<sac>:<case>:<entree> » et « AMELIORER:<sac>:<case>:<entree> ».
    // Reponses : « <NOM>_OK:<entree creee> » ou « <NOM>_ERR:<cause> » (REACTIFS:<id>=<manque>,...,or=<cuivre>).
    void TraiterMessage(Player* joueur, std::string_view corps, SorteEtape sorte)
    {
        std::string const nom = sorte == SorteEtape::Eveil ? "EVEIL" : "AMELIORER";
        auto refuser = [&](std::string const& cause, std::string const& avis)
        {
            Repondre(joueur, nom + "_ERR:" + cause);
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
        Item* neuve = Avancer(joueur, piece, sorte, refus, "message");
        if (!neuve)
            return refuser(refus.cause, refus.avis);
        Repondre(joueur, nom + "_OK:" + std::to_string(neuve->GetEntry()));
    }

    struct DelaiEveil final : public DataMap::Base
    {
        Milliseconds prochain{ 0 };
        bool delaiRepondu = false;   // une seule reponse DELAI par fenetre
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

    // Ids v2 (artisanat/ids-figes-v2.json, D-PO-24) : le Heaume du Rempart (FAI-P2-07) et ses Garde-poignets (FAI-P2-01).
    // « BASE » : la piece qu'on eveille, le palier 70.
    constexpr uint32 HEAUME_PALIER10 = 9101690;
    constexpr uint32 HEAUME_BASE = 9101696;
    constexpr uint32 HEAUME_EVEILLE = 9101697;
    constexpr uint32 GARDE_POIGNETS_BASE = 9101618;
    // Etape 5 (fiche) : le set 63004 a versions melangees, 5 pieces au niveau 80 (Heaume palier 70, Gantelets palier 70,
    // Ceinture palier 10, Garde-poignets 213, Solerets 213), et le set 820 (45377, 46154).
    constexpr std::array<uint32, 7> PIECES_ETAPE5 = { HEAUME_BASE, 9101644, 9101651, 9101619, 9101632, 45377, 46154 };
    // .artisanat banc set : 4 pieces palier 10 du set 63004 (niveau requis 10), portees par un bot de niveau 20-78.
    constexpr std::array<uint32, 4> PIECES_BANC_SET = { 9101625, 9101638, 9101651, 9101612 };
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

    // Les 6 rangs de chaque metier de l'artisanat (lus par .lookup spell sur le PTR : « <Metier>, rank 1..6 »). Le
    // livre des metiers lance le rang de la competence max (P-236) : on les apprend tous.
    struct RangsMetier
    {
        uint32 competence;
        std::array<uint32, 6> rangs;
    };
    constexpr std::array<RangsMetier, 6> METIERS =
    {{
        { SKILL_FORGE, RANGS_FORGE },
        { 197, { 3908, 3909, 3910, 12180, 26790, 51309 } },    // Couture
        { 165, { 2108, 3104, 3811, 10662, 32549, 51302 } },    // Travail du cuir
        { 171, { 2259, 3101, 3464, 11611, 28596, 51304 } },    // Alchimie
        { 333, { 7411, 7412, 7413, 13920, 28029, 51313 } },    // Enchantement
        { 755, { 25229, 25230, 28894, 28895, 28897, 51311 } }, // Joaillerie
    }};
    // Les outils de recolte des 12 noeuds de la salle (item_template.RequiredSkill, rang 1), plus la griffe du coffre.
    constexpr std::array<uint32, 7> OUTILS_RECOLTE = { 5384, 5916, 5937, 16308, 23581, 23674, 5059 };

    // Apprend les 6 rangs du metier et met la competence a `valeur`/450. Un rang dont le sort n'enseigne pas cette
    // competence (SpellLearnSkill) est saute et journalise.
    void Metier(Player* joueur, RangsMetier const& m, uint16 valeur)
    {
        for (uint32 rang : m.rangs)
        {
            SpellLearnSkillNode const* noeud = sSpellMgr->GetSpellLearnSkill(rang);
            if (!noeud || noeud->skill != m.competence)
            {
                LOG_ERROR("module", "coa artisanat: banc : le sort {} n'enseigne pas la competence {}, saute", rang, m.competence);
                continue;
            }
            if (!joueur->HasSpell(rang))
                joueur->learnSpell(rang);
        }
        // Rang 6 (max 450) en dur : apprendre un rang inferieur a un rang connu laisse GetSkillStep sur ce rang-la.
        joueur->SetSkill(m.competence, uint16(m.rangs.size()), valeur, 450);
    }

    void Forge(Player* joueur, uint16 valeur)
    {
        Metier(joueur, METIERS[0], valeur);
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


    // ---- Materiaux de monde (D-PO-19, D-PO-23, D-PO-24) : chute sur toute creature qui rapporte de l'experience ----

    // Categorie de la victime, dans l'ordre de priorite de la proposition de PartB (PROPOSITION-artisanat-paliers-
    // D-PO-19.md, § 4) : boss, rare elite, rare, instance (hors boss), elite, ordinaire.
    enum CategorieMonde : uint8 { CAT_ORDINAIRE, CAT_ELITE, CAT_RARE, CAT_RARE_ELITE, CAT_INSTANCE, CAT_BOSS, CAT_NOMBRE };
    constexpr std::array<char const*, CAT_NOMBRE> NOMS_CATEGORIES = { "Ordinaire", "Elite", "Rare", "RareElite", "Instance", "Boss" };
    constexpr std::array<char const*, 3> NOMS_QUALITES = { "Vert", "Rare", "Epique" };

    struct ReglesMonde
    {
        std::array<uint32, 3> objets{};                               // vert, rare, epique ; 0 : coupe
        std::array<std::array<float, 3>, CAT_NOMBRE> chances{};       // en pour cent
        std::array<std::pair<uint8, uint8>, CAT_NOMBRE> nombreVert{}; // min, max
        float tauxBots = 0.0f;                                        // en pour cent des chances ; 0 : bots exclus
        bool actif = false;
    };
    ReglesMonde Monde;

    // Lu au demarrage et a chaque .reload config. Tout est a 0 par defaut : rien ne tombe sans configuration.
    void ChargerReglesMonde()
    {
        ReglesMonde r;
        for (std::size_t q = 0; q < 3; ++q)
            r.objets[q] = sConfigMgr->GetOption<uint32>(Acore::StringFormat("CoaArtisanat.Monde.Objet.{}", NOMS_QUALITES[q]), 0, false);
        for (std::size_t c = 0; c < CAT_NOMBRE; ++c)
        {
            for (std::size_t q = 0; q < 3; ++q)
                r.chances[c][q] = std::clamp(sConfigMgr->GetOption<float>(Acore::StringFormat("CoaArtisanat.Monde.Chance.{}.{}",
                    NOMS_CATEGORIES[c], NOMS_QUALITES[q]), 0.0f, false), 0.0f, 100.0f);
            uint32 const mini = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(Acore::StringFormat("CoaArtisanat.Monde.Vert.{}.Min",
                NOMS_CATEGORIES[c]), 1, false), 1, 20);
            uint32 const maxi = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(Acore::StringFormat("CoaArtisanat.Monde.Vert.{}.Max",
                NOMS_CATEGORIES[c]), mini, false), mini, 20);
            r.nombreVert[c] = { uint8(mini), uint8(maxi) };
        }
        r.tauxBots = std::clamp(sConfigMgr->GetOption<float>("CoaArtisanat.Monde.TauxBots", 0.0f, false), 0.0f, 100.0f);
        for (std::size_t q = 0; q < 3; ++q)
        {
            if (r.objets[q] && !sObjectMgr->GetItemTemplate(r.objets[q]))
            {
                LOG_ERROR("module", "coa artisanat: CoaArtisanat.Monde.Objet.{} = {} : objet inconnu, qualite coupee", NOMS_QUALITES[q], r.objets[q]);
                r.objets[q] = 0;
            }
            for (std::size_t c = 0; c < CAT_NOMBRE; ++c)
                r.actif = r.actif || (r.objets[q] && r.chances[c][q] > 0.0f);
        }
        Monde = r;
        LOG_INFO("module", "coa artisanat: materiaux de monde {} (objets {}/{}/{}, bots {} %)", r.actif ? "actifs" : "coupes",
            r.objets[0], r.objets[1], r.objets[2], r.tauxBots);
    }

    std::atomic<uint32> ChutesMonde{ 0 };      // objets de monde poses dans un butin depuis le demarrage
    std::atomic<uint32> ButinsMonde{ 0 };      // butins de creature examines (regles actives)
    std::atomic<uint32> EcartesGris{ 0 };      // ... ecartes : victime sans experience (grise, NO_XP...)
    std::atomic<uint32> EcartesBots{ 0 };      // ... ecartes : detenteur bot (TauxBots = 0)

    // Un objet du butin d'un detenteur bot est-il accessible a un humain du groupe ? Chacun pour soi : oui. Tour par
    // tour : non (seul le detenteur). Tirage (groupe, besoin avant cupidite) : oui au-dessus du seuil, sinon non (sous le
    // seuil, l'objet suit le tour). Maitre du butin : au-dessus du seuil si le maitre est humain (un bot ne distribue
    // jamais), sinon non.
    bool AccessibleAuxHumains(Group const* groupe, uint32 objet)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(objet);
        if (!proto)
            return false;
        bool const auDessusDuSeuil = proto->Quality >= uint32(groupe->GetLootThreshold());
        switch (groupe->GetLootMethod())
        {
            case FREE_FOR_ALL:
                return true;
            case GROUP_LOOT:
            case NEED_BEFORE_GREED:
                return auDessusDuSeuil;
            case MASTER_LOOT:
            {
                Player* maitre = ObjectAccessor::FindPlayer(groupe->GetMasterLooterGuid());
                return auDessusDuSeuil && maitre && maitre->GetSession() && !maitre->GetSession()->IsBot();
            }
            default:
                return false;
        }
    }

    class ArtisanatButinMonde final : public MiscScript
    {
    public:
        ArtisanatButinMonde() : MiscScript("CoaArtisanatButinMonde", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) { }

        // Appele par Loot::FillLoot apres le tirage du modele, avant les droits de groupe : un objet ajoute ici suit
        // les regles normales (maitre du butin, seuil). Les creatures sans table de butin (lootid 0) n'y passent pas
        // (D-PO-23 : accepte, 9,1 % des hostiles et neutres tuables).
        void OnAfterLootTemplateProcess(Loot* butin, LootTemplate const* /*modele*/, LootStore const& magasin, Player* detenteur,
            bool /*personnel*/, bool /*sansErreur*/, uint16 mode) override
        {
            ReglesMonde const& r = Monde;
            if (!r.actif || !butin || !detenteur || !mode || &magasin != &LootTemplates_Creature || !detenteur->IsInWorld())
                return;
            Creature* victime = detenteur->GetMap()->GetCreature(butin->sourceWorldObjectGUID);
            if (!victime)
                return;
            ++ButinsMonde;

            // Le personnage de reference : le detenteur du butin. Si c'est un bot (premier a frapper, tour de ramassage)
            // et qu'un joueur de son groupe a droit au butin, c'est ce joueur (le plus haut niveau, le plus exigeant) :
            // un humain entoure de bots ne perd pas ses chances. Les bots ne ramassent ni ne tirent ces objets
            // (mod-playerbots, StoreLootAction::IsLootAllowed et LootRollAction).
            Player* reference = detenteur;
            Group* groupeBot = nullptr;   // non nul : detenteur bot, objet destine a un humain de son groupe
            float facteur = 1.0f;
            auto estBot = [](Player const* p) { return p->GetSession() && p->GetSession()->IsBot(); };
            if (estBot(detenteur))
            {
                Player* humain = nullptr;
                if (Group* groupe = detenteur->GetGroup())
                    for (GroupReference* ref = groupe->GetFirstMember(); ref; ref = ref->next())
                        if (Player* membre = ref->GetSource())
                            if (!estBot(membre) && membre->IsInWorld() && membre->IsAtLootRewardDistance(victime)
                                && (!humain || membre->GetLevel() > humain->GetLevel()))
                                humain = membre;
                if (humain)
                {
                    reference = humain;
                    groupeBot = detenteur->GetGroup();
                }
                else
                    facteur = r.tauxBots / 100.0f;
            }
            if (facteur <= 0.0f)
            {
                ++EcartesBots;
                return;
            }

            // Seulement si la victime rapporte de l'experience au personnage de reference (D-PO-23 : gris juge pour
            // lui) : meme calcul que le cœur (Acore::XP::Gain : gris, NO_XP, totem, familier, bestiole, ModExperience).
            if (!Acore::XP::Gain(reference, victime))
            {
                ++EcartesGris;
                return;
            }

            CategorieMonde categorie;
            switch (victime->GetCreatureTemplate()->rank)
            {
                case CREATURE_ELITE_WORLDBOSS: categorie = CAT_BOSS; break;
                case CREATURE_ELITE_RAREELITE: categorie = CAT_RARE_ELITE; break;
                case CREATURE_ELITE_RARE:      categorie = CAT_RARE; break;
                case CREATURE_ELITE_ELITE:     categorie = CAT_ELITE; break;
                case CREATURE_ELITE_NORMAL:    categorie = CAT_ORDINAIRE; break;
                default: return;   // CREATURE_UNKNOWN (rang 5) : rien
            }
            if ((categorie == CAT_ELITE || categorie == CAT_ORDINAIRE) && victime->GetMap()->IsDungeon())
                categorie = CAT_INSTANCE;

            for (std::size_t q = 0; q < 3; ++q)
            {
                if (!r.objets[q])
                    continue;
                // Detenteur bot : l'objet n'est pose que si l'humain peut l'obtenir (tirage, chacun pour soi, maitre
                // humain). Sinon seul le bot y aurait acces, et il le laisse : l'objet serait perdu.
                if (groupeBot && !AccessibleAuxHumains(groupeBot, r.objets[q]))
                    continue;
                if (!roll_chance_f(r.chances[categorie][q] * facteur))
                    continue;
                uint8 const mini = q == 0 ? r.nombreVert[categorie].first : 1;
                uint8 const maxi = q == 0 ? r.nombreVert[categorie].second : 1;
                std::size_t const avant = butin->items.size();
                butin->AddItem(LootStoreItem(r.objets[q], 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, mini, maxi));
                if (butin->items.size() > avant)   // MAX_NR_LOOT_ITEMS atteint : rien n'est pose
                    ++ChutesMonde;
            }
        }
    };

    // ---- Talents d'artisanat et signatures (lot T0, D-PO-34) ----------------------------------------------------------
    //
    // Donnees (PartB, acore_world) : coa_artisanat_talent_def (une ligne par rang, cle = sort marqueur) et
    // coa_artisanat_signature (une ligne par enchantement de signature). Etat par personnage (ce module,
    // acore_characters) : coa_artisanat_personnage (metiers principaux, points gagnes) et coa_artisanat_talent (sorts
    // marqueurs connus). Regles : docs/PROPOSITION-artisanat-arbres-detail.md §§ 0, 1, 2, 6 et arbitrages.

    constexpr std::array<uint32, 3> METIERS_RECOLTE = { 186, 182, 393 };   // Minage, Herboristerie, Depecage
    constexpr uint32 POINTS_ETAGE_PRECEDENT = 5;
    std::string const CLE_TALENTS = "CoaArtisanatTalents";
    std::string const CLE_SIGNATURES = "CoaArtisanatSignatures";

    bool EstRecolte(uint32 metier)
    {
        return std::find(METIERS_RECOLTE.begin(), METIERS_RECOLTE.end(), metier) != METIERS_RECOLTE.end();
    }

    // Plafonds cumules par tranche de niveau (§ 1) : 10-19, 20-29, ..., 70-79, 80.
    uint32 Plafond(uint8 niveau, bool recolte)
    {
        static constexpr std::array<uint8, 8> CREATION = { 6, 13, 20, 28, 36, 44, 50, 72 };
        static constexpr std::array<uint8, 8> RECOLTE = { 4, 9, 14, 20, 26, 32, 37, 48 };
        if (niveau < 10)
            return 0;
        std::size_t const tranche = niveau >= 80 ? 7 : std::size_t(niveau / 10 - 1);
        return recolte ? RECOLTE[tranche] : CREATION[tranche];
    }

    struct TalentDef
    {
        uint32 sort = 0;
        uint32 metier = 0;
        uint32 prerequis = 0;
        uint8 etage = 0;
        uint8 niveau = 0;
        uint8 rang = 0;
        uint8 cout = 0;
        uint8 signatureRang = 0;
        std::string talent;
        std::string groupe;
        std::string voie;
        std::string forme;
        std::string requis;   // '' ou un talent (code) ou un groupe dont un talent doit etre connu (D-PO-37)
    };
    std::unordered_map<uint32, TalentDef> Talents;           // par sort marqueur
    std::unordered_map<uint32, uint32> MarqueursMetier;       // SkillLine -> sort du marqueur « metier principal »

    struct SignatureDef
    {
        uint32 enchant = 0;
        uint32 metier = 0;
        uint32 valeur = 0;
        uint32 valeurArene = 0;
        uint32 rechargeMs = 0;
        uint32 sortEffet = 0;
        uint32 pieceMasque = 0;
        uint8 rang = 0;
        uint8 seuilPct = 0;
        uint8 groupeSursaut = 0;
        uint8 pieceClasse = 0;
        std::string voie;
        std::string forme;
        std::string effet;
    };
    std::unordered_map<uint32, SignatureDef> Signatures;      // par enchantement
    bool TablesPersonnages = false;

    bool TableExiste(DatabaseWorkerPool<WorldDatabaseConnection>& base, std::string_view nom)
    {
        return bool(base.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", nom));
    }

    void ChargerTalents()
    {
        Talents.clear();
        MarqueursMetier.clear();
        Signatures.clear();
        TablesPersonnages = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'coa_artisanat_personnage'")) && bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_artisanat_talent'"));

        uint32 rejetes = 0;
        // La colonne « requis » (D-PO-37) peut manquer dans un jeu plus ancien : on la lit si elle existe.
        bool const avecRequis = bool(WorldDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'coa_artisanat_talent_def' AND COLUMN_NAME = 'requis'"));
        if (TableExiste(WorldDatabase, "coa_artisanat_talent_def"))
            if (QueryResult r = WorldDatabase.Query("SELECT sort, metier, etage, niveau, talent, rang, cout, groupe, voie, prerequis, "
                "forme, signature_rang, {} FROM coa_artisanat_talent_def", avecRequis ? "requis" : "''"))
                do
                {
                    Field* f = r->Fetch();
                    TalentDef d;
                    d.sort = f[0].Get<uint32>();
                    d.metier = f[1].Get<uint32>();
                    d.etage = f[2].Get<uint8>();
                    d.niveau = f[3].Get<uint8>();
                    d.talent = f[4].Get<std::string>();
                    d.rang = f[5].Get<uint8>();
                    d.cout = f[6].Get<uint8>();
                    d.groupe = f[7].Get<std::string>();
                    d.voie = f[8].Get<std::string>();
                    d.prerequis = f[9].Get<uint32>();
                    d.forme = f[10].Get<std::string>();
                    d.signatureRang = f[11].Get<uint8>();
                    d.requis = f[12].Get<std::string>();
                    if (!sSpellMgr->GetSpellInfo(d.sort) || !d.metier)
                    {
                        ++rejetes;
                        LOG_ERROR("module", "coa artisanat: talent rejete sort={} metier={} (sort inconnu ou metier nul)", d.sort, d.metier);
                        continue;
                    }
                    if (d.talent == "metier")
                        MarqueursMetier[d.metier] = d.sort;
                    Talents[d.sort] = d;
                } while (r->NextRow());
        // Un prerequis doit etre un talent charge, du meme arbre.
        for (auto it = Talents.begin(); it != Talents.end();)
        {
            auto p = it->second.prerequis ? Talents.find(it->second.prerequis) : Talents.end();
            if (it->second.prerequis && (p == Talents.end() || p->second.metier != it->second.metier))
            {
                ++rejetes;
                LOG_ERROR("module", "coa artisanat: talent rejete sort={} : prerequis {} absent ou d'un autre arbre", it->first, it->second.prerequis);
                it = Talents.erase(it);
            }
            else
                ++it;
        }

        uint32 signaturesRejetees = 0;
        if (TableExiste(WorldDatabase, "coa_artisanat_signature"))
            if (QueryResult r = WorldDatabase.Query("SELECT enchant, metier, voie, forme, rang, effet, valeur, valeur_arene, recharge_ms, "
                "seuil_pv_pct, groupe_sursaut, piece_classe, piece_masque, sort_effet FROM coa_artisanat_signature"))
                do
                {
                    Field* f = r->Fetch();
                    SignatureDef s;
                    s.enchant = f[0].Get<uint32>();
                    s.metier = f[1].Get<uint32>();
                    s.voie = f[2].Get<std::string>();
                    s.forme = f[3].Get<std::string>();
                    s.rang = f[4].Get<uint8>();
                    s.effet = f[5].Get<std::string>();
                    s.valeur = f[6].Get<uint32>();
                    s.valeurArene = f[7].Get<uint32>();
                    s.rechargeMs = f[8].Get<uint32>();
                    s.seuilPct = f[9].Get<uint8>();
                    s.groupeSursaut = f[10].Get<uint8>();
                    s.pieceClasse = f[11].Get<uint8>();
                    s.pieceMasque = f[12].Get<uint32>();
                    s.sortEffet = f[13].Get<uint32>();
                    if (!sSpellItemEnchantmentStore.LookupEntry(s.enchant) || (s.sortEffet && !sSpellMgr->GetSpellInfo(s.sortEffet))
                        || s.forme.empty() || !s.rang || s.seuilPct > 100)
                    {
                        ++signaturesRejetees;
                        LOG_ERROR("module", "coa artisanat: signature rejetee enchant={} (enchantement ou sort d'effet inconnu, forme ou rang vide)", s.enchant);
                        continue;
                    }
                    Signatures[s.enchant] = s;
                } while (r->NextRow());

        LOG_INFO("module", "coa artisanat: talents : {} sorts ({} marqueurs de metier), {} rejetes ; signatures : {}, {} rejetees ; tables personnages {}",
            uint32(Talents.size()), uint32(MarqueursMetier.size()), rejetes, uint32(Signatures.size()), signaturesRejetees,
            TablesPersonnages ? "presentes" : "ABSENTES");
    }

    struct EtatTalents final : public DataMap::Base
    {
        bool charge = false;
        uint32 metierC = 0;
        uint32 metierR = 0;
        uint32 gagnesC = 0;
        uint32 gagnesR = 0;
        std::set<uint32> sorts;
    };

    EtatTalents* Etat(Player* joueur)
    {
        EtatTalents* e = joueur->CustomData.GetDefault<EtatTalents>(CLE_TALENTS);
        if (e->charge || !TablesPersonnages)
            return e;
        e->charge = true;
        uint32 const guid = joueur->GetGUID().GetCounter();
        if (QueryResult r = CharacterDatabase.Query("SELECT metier_creation, metier_recolte, points_creation, points_recolte "
            "FROM coa_artisanat_personnage WHERE guid = {}", guid))
        {
            Field* f = r->Fetch();
            e->metierC = f[0].Get<uint32>();
            e->metierR = f[1].Get<uint32>();
            e->gagnesC = f[2].Get<uint32>();
            e->gagnesR = f[3].Get<uint32>();
        }
        if (QueryResult r = CharacterDatabase.Query("SELECT sort FROM coa_artisanat_talent WHERE guid = {}", guid))
            do
                e->sorts.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
        return e;
    }

    void SauverPersonnage(Player* joueur, EtatTalents const& e)
    {
        if (TablesPersonnages)
            CharacterDatabase.Execute("REPLACE INTO coa_artisanat_personnage (guid, metier_creation, metier_recolte, points_creation, "
                "points_recolte) VALUES ({}, {}, {}, {}, {})", joueur->GetGUID().GetCounter(), e.metierC, e.metierR, e.gagnesC, e.gagnesR);
    }

    uint32 Depenses(EtatTalents const& e, uint32 metier, int32 etage = -1)
    {
        uint32 total = 0;
        for (uint32 sort : e.sorts)
            if (auto it = Talents.find(sort); it != Talents.end() && it->second.metier == metier
                && (etage < 0 || it->second.etage == uint8(etage)))
                total += it->second.cout;
        return total;
    }

    uint32 Disponibles(Player* joueur, EtatTalents const& e, bool recolte)
    {
        uint32 const metier = recolte ? e.metierR : e.metierC;
        if (!metier)
            return 0;
        uint32 const plafond = std::min(recolte ? e.gagnesR : e.gagnesC, Plafond(joueur->GetLevel(), recolte));
        uint32 const depenses = Depenses(e, metier);
        return plafond > depenses ? plafond - depenses : 0;
    }

    // Les sorts marqueurs suivent la table : appris s'ils manquent, retires s'ils n'y sont plus (connexion, oubli).
    void SynchroniserMarqueurs(Player* joueur, EtatTalents const& e)
    {
        auto marqueur = [](uint32 metier) -> uint32
        {
            auto it = metier ? MarqueursMetier.find(metier) : MarqueursMetier.end();
            return it != MarqueursMetier.end() ? it->second : 0;
        };
        uint32 const marqueurC = marqueur(e.metierC), marqueurR = marqueur(e.metierR);
        for (auto const& [sort, d] : Talents)
        {
            bool const voulu = d.talent == "metier" ? (sort == marqueurC || sort == marqueurR) : e.sorts.count(sort) != 0;
            if (voulu && !joueur->HasSpell(sort))
                joueur->learnSpell(sort);
            else if (!voulu && joueur->HasSpell(sort))
                joueur->removeSpell(sort, SPEC_MASK_ALL, false);
        }
    }

    // Rend la cause du refus, vide si le talent est appris.
    std::string ApprendreTalent(Player* joueur, uint32 sort)
    {
        auto it = Talents.find(sort);
        if (it == Talents.end() || it->second.talent == "metier")
            return "INCONNU";
        TalentDef const& d = it->second;
        EtatTalents* e = Etat(joueur);
        bool const recolte = EstRecolte(d.metier);
        uint32 const principal = recolte ? e->metierR : e->metierC;
        if (!principal || principal != d.metier)
            return "METIER";
        if (e->sorts.count(sort))
            return "MAX";
        if (d.prerequis && !e->sorts.count(d.prerequis))
            return "REQUIS";
        // Un talent d'une voie exige la voie elle-meme (talent « voie:<voie> ») : pas de Forme ni de Savoir sans voie.
        if (!d.voie.empty() && d.talent != "voie:" + d.voie)
        {
            bool voie = false;
            for (uint32 connu : e->sorts)
                if (auto c = Talents.find(connu); c != Talents.end() && c->second.metier == d.metier && c->second.talent == "voie:" + d.voie)
                    voie = true;
            if (!voie)
                return "REQUIS";
        }
        // Prerequis d'un autre talent (D-PO-37) : un talent connu du meme arbre dont le code ou le groupe vaut « requis »
        // (signature rang 2 : groupe 'forme' ; Savoir : groupe 'voie' ; variante : 'voie:orage'...).
        if (!d.requis.empty())
        {
            bool trouve = false;
            for (uint32 connu : e->sorts)
                if (auto c = Talents.find(connu); c != Talents.end() && c->second.metier == d.metier
                    && (c->second.talent == d.requis || c->second.groupe == d.requis))
                    trouve = true;
            if (!trouve)
                return "REQUIS";
        }
        if (joueur->GetLevel() < d.niveau)
            return "NIVEAU";
        if (d.etage >= 2 && Depenses(*e, d.metier, d.etage - 1) < POINTS_ETAGE_PRECEDENT)
            return "ETAGE";
        if (!d.groupe.empty())
            for (uint32 connu : e->sorts)
                if (auto c = Talents.find(connu); c != Talents.end() && c->second.metier == d.metier
                    && c->second.groupe == d.groupe && c->second.talent != d.talent)
                    return "EXCLUSIF";
        if (Disponibles(joueur, *e, recolte) < d.cout)
            return "PLAFOND";
        e->sorts.insert(sort);
        if (!joueur->HasSpell(sort))
            joueur->learnSpell(sort);
        if (TablesPersonnages)
            CharacterDatabase.Execute("INSERT IGNORE INTO coa_artisanat_talent (guid, sort) VALUES ({}, {})", joueur->GetGUID().GetCounter(), sort);
        LOG_INFO("module", "coa artisanat: talent appris joueur={} sort={} talent={} rang={} metier={} reste={}",
            joueur->GetGUID().GetCounter(), sort, d.talent, uint32(d.rang), d.metier, Disponibles(joueur, *e, recolte));
        return {};
    }

    // Choix du metier principal (C : creation, R : recolte). Le changement payant viendra au lot T.
    std::string ChoisirMetier(Player* joueur, char arbre, uint32 metier)
    {
        if ((arbre != 'C' && arbre != 'R') || EstRecolte(metier) != (arbre == 'R') || !MarqueursMetier.count(metier))
            return "INCONNU";
        if (!joueur->HasSkill(metier))
            return "INCONNU";
        EtatTalents* e = Etat(joueur);
        uint32& choix = arbre == 'R' ? e->metierR : e->metierC;
        if (choix)
            return "DEJA";
        choix = metier;
        SauverPersonnage(joueur, *e);
        SynchroniserMarqueurs(joueur, *e);
        LOG_INFO("module", "coa artisanat: metier principal joueur={} arbre={} metier={}", joueur->GetGUID().GetCounter(), arbre, metier);
        return {};
    }

    void OublierTalents(Player* joueur)
    {
        EtatTalents* e = Etat(joueur);
        e->sorts.clear();
        e->metierC = e->metierR = 0;
        e->gagnesC = e->gagnesR = 0;
        SynchroniserMarqueurs(joueur, *e);
        if (TablesPersonnages)
        {
            CharacterDatabase.Execute("DELETE FROM coa_artisanat_talent WHERE guid = {}", joueur->GetGUID().GetCounter());
            CharacterDatabase.Execute("DELETE FROM coa_artisanat_personnage WHERE guid = {}", joueur->GetGUID().GetCounter());
        }
    }

    // Etat pour l'atelier : « TALENTS:<metC>:<metR>:<dispoC>:<dispoR>:<plafC>:<plafR>:<gagC>:<gagR> », puis la liste
    // des sorts connus en « TALENTS_APPRIS:<k>/<n>:<sort>,... » (decoupee sous 255 caracteres).
    void EnvoyerEtat(Player* joueur)
    {
        EtatTalents* e = Etat(joueur);
        Repondre(joueur, Acore::StringFormat("TALENTS:{}:{}:{}:{}:{}:{}:{}:{}", e->metierC, e->metierR, Disponibles(joueur, *e, false),
            Disponibles(joueur, *e, true), Plafond(joueur->GetLevel(), false), Plafond(joueur->GetLevel(), true), e->gagnesC, e->gagnesR));
        std::vector<std::string> morceaux(1);
        for (uint32 sort : e->sorts)
        {
            std::string const s = std::to_string(sort);
            if (morceaux.back().size() + s.size() + 1 > 200)
                morceaux.emplace_back();
            morceaux.back() += (morceaux.back().empty() ? "" : ",") + s;
        }
        for (std::size_t k = 0; k < morceaux.size(); ++k)
            Repondre(joueur, Acore::StringFormat("TALENTS_APPRIS:{}/{}:{}", k + 1, morceaux.size(), morceaux[k]));
    }

    // Messages de l'atelier : « TALENT:<sort> », « METIER:<C|R>:<skill> », « TALENTS? ». Reponses : TALENT_OK:<sort>:<rang>
    // ou TALENT_ERR:<cause> ; METIER_OK:<C|R>:<skill> ou METIER_ERR:<cause> ; l'etat (EnvoyerEtat).
    void TraiterTalentMessage(Player* joueur, std::string_view corps)
    {
        if (!sConfigMgr->GetOption<bool>("CoaArtisanat.TalentsParMessage", true))
            return Repondre(joueur, corps.starts_with("METIER:") ? "METIER_ERR:DESACTIVE" : "TALENT_ERR:DESACTIVE");
        if (corps == "TALENTS?")
            return EnvoyerEtat(joueur);
        int32 n = 0;
        if (corps.starts_with("TALENT:"))
        {
            if (!LireEntier(corps.substr(7), n) || n <= 0)
                return Repondre(joueur, "TALENT_ERR:FORMAT");
            std::string const cause = ApprendreTalent(joueur, uint32(n));
            if (!cause.empty())
                return Repondre(joueur, "TALENT_ERR:" + cause);
            auto it = Talents.find(uint32(n));
            return Repondre(joueur, Acore::StringFormat("TALENT_OK:{}:{}", n, it != Talents.end() ? uint32(it->second.rang) : 0));
        }
        // METIER:<C|R>:<skill>
        std::string_view reste = corps.substr(7);
        if (reste.size() < 3 || reste[1] != ':' || !LireEntier(reste.substr(2), n) || n <= 0)
            return Repondre(joueur, "METIER_ERR:FORMAT");
        std::string const cause = ChoisirMetier(joueur, reste[0], uint32(n));
        if (!cause.empty())
            return Repondre(joueur, "METIER_ERR:" + cause);
        Repondre(joueur, Acore::StringFormat("METIER_OK:{}:{}", reste[0], n));
    }

    // Signature a la fabrication : le specialiste (metier principal de creation = metier de la recette) qui connait une
    // Forme pose l'enchantement de sa forme a son rang dans PROP_ENCHANTMENT_SLOT_0, si la piece est de sa voie.
    void SignerALaFabrication(Player* joueur, Item* objet, uint32 recette)
    {
        if (Signatures.empty() || !TablesPersonnages)
            return;
        EtatTalents* e = Etat(joueur);
        if (!e->metierC)
            return;
        uint32 metierRecette = 0;
        auto bornes = sSpellMgr->GetSkillLineAbilityMapBounds(recette);
        for (auto it = bornes.first; it != bornes.second; ++it)
            metierRecette = it->second->SkillLine;
        if (metierRecette != e->metierC)
            return;
        std::string forme, voie;
        uint8 rang = 0;
        for (uint32 sort : e->sorts)
            if (auto it = Talents.find(sort); it != Talents.end() && it->second.metier == e->metierC)
            {
                if (!it->second.forme.empty())
                {
                    forme = it->second.forme;
                    voie = it->second.voie;
                }
                rang = std::max(rang, it->second.signatureRang);
            }
        if (forme.empty() || !rang)
            return;
        ItemTemplate const* proto = objet->GetTemplate();
        for (auto const& [enchant, s] : Signatures)
        {
            if (s.metier != e->metierC || s.forme != forme || (!voie.empty() && s.voie != voie) || s.rang != rang
                || proto->Class != s.pieceClasse
                || proto->SubClass >= 32 || !(s.pieceMasque & (1u << proto->SubClass)))
                continue;
            objet->SetEnchantment(PROP_ENCHANTMENT_SLOT_0, enchant, 0, 0, joueur->GetGUID());
            objet->SetState(ITEM_CHANGED, joueur);
            LOG_INFO("module", "coa artisanat: signature posee joueur={} piece={} ({}) forme={} rang={} enchant={}",
                joueur->GetGUID().GetCounter(), objet->GetGUID().GetCounter(), objet->GetEntry(), forme, uint32(rang), enchant);
            return;
        }
    }

    // Signatures actives : la meilleure de chaque (voie, forme) portee, 3 au plus (ordre des emplacements), sans cumul.
    struct SignaturesActives final : public DataMap::Base
    {
        std::vector<SignatureDef const*> actives;
        bool sale = true;                   // equipement change depuis le dernier calcul
        Milliseconds sursaut{ 0 };          // fin de la recharge partagee des sursauts de survie
        std::map<uint32, Milliseconds> recharges;   // autres signatures, par enchantement
    };
    std::atomic<uint32> DeclenchementsSignatures{ 0 };

    void RecalculerSignatures(Player* joueur)
    {
        SignaturesActives* a = joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
        a->actives.clear();
        a->sale = false;
        if (Signatures.empty())
            return;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* objet = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!objet || objet->IsBroken())
                continue;
            auto it = Signatures.find(objet->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_0));
            if (it == Signatures.end())
                continue;
            SignatureDef const* s = &it->second;
            auto meme = std::find_if(a->actives.begin(), a->actives.end(), [&](SignatureDef const* x)
            {
                return x->metier == s->metier && x->voie == s->voie && x->forme == s->forme;
            });
            if (meme != a->actives.end())
            {
                if (s->rang > (*meme)->rang)
                    *meme = s;
            }
            else if (a->actives.size() < 3)
                a->actives.push_back(s);
        }
    }

    // Egide (et tout effet « absorbe_sous_seuil ») : le coup qui ferait passer sous le seuil est absorbe a hauteur de la
    // valeur (au niveau 80, x niveau / 80 ; valeur d'arene en arene), le reste devient un bouclier (sort d'effet).
    // Recharge partagee par les sursauts de survie (groupe_sursaut).
    // Les crochets d'equipement ne couvrent pas tous les retraits (piece portee detruite) : on marque, et on recalcule
    // au premier coup suivant.
    void MarquerSignatures(Player* joueur)
    {
        joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES)->sale = true;
    }

    void DeclencherSignatures(Player* joueur, uint32& degats)
    {
        if (Signatures.empty() || !joueur->IsAlive())
            return;
        SignaturesActives* a = joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
        if (a->sale)
            RecalculerSignatures(joueur);
        if (a->actives.empty())
            return;
        Milliseconds const maintenant = GameTime::GetGameTimeMS();
        for (SignatureDef const* s : a->actives)
        {
            if (s->effet != "absorbe_sous_seuil" && s->effet != "egide")
                continue;
            uint64 const pv = joueur->GetHealth();
            uint64 const apres = pv > degats ? pv - degats : 0;
            if (apres * 100 >= uint64(joueur->GetMaxHealth()) * s->seuilPct)
                continue;
            Milliseconds& recharge = s->groupeSursaut ? a->sursaut : a->recharges[s->enchant];
            if (maintenant < recharge)
                continue;
            bool const arene = joueur->GetMap() && joueur->GetMap()->IsBattleArena();
            uint32 const valeur = uint32(uint64(arene ? s->valeurArene : s->valeur) * joueur->GetLevel() / DEFAULT_MAX_LEVEL);
            // Un coup mortel meme apres absorption : rien ne se declenche, la recharge n'est pas consommee.
            if (uint64(degats) >= pv + valeur)
                continue;
            recharge = maintenant + Milliseconds(s->rechargeMs);
            uint32 const absorbe = std::min(degats, valeur);
            degats -= absorbe;
            if (uint32 const reste = valeur - absorbe; reste && s->sortEffet)
            {
                int32 const bp = int32(reste);
                joueur->CastCustomSpell(joueur, s->sortEffet, &bp, nullptr, nullptr, true);
                // La valeur d'une signature est fixe : pas de multiplicateur d'absorption des classes
                // (SPELL_AURA_ASCENSION_MOD_ABSORB_AMOUNT_PCT, SpellAuraEffects.cpp:629-645).
                if (AuraEffect* bouclier = joueur->GetAuraEffect(s->sortEffet, EFFECT_0, joueur->GetGUID()))
                    bouclier->ChangeAmount(bp);
            }
            ++DeclenchementsSignatures;
            LOG_INFO("module", "coa artisanat: signature declenchee joueur={} enchant={} forme={} rang={} valeur={} absorbe={} reste={}",
                joueur->GetGUID().GetCounter(), s->enchant, s->forme, uint32(s->rang), valeur, absorbe, valeur - absorbe);
            return;   // un seul sursaut par coup
        }
    }

    // Signatures a declencher (Egide : sursaut de survie) sur les degats subis par un joueur.
    class ArtisanatUnit final : public UnitScript
    {
    public:
        ArtisanatUnit() : UnitScript("CoaArtisanatUnit", true) { }

        // UnitScript::DealDamage (appele pour tous les UnitScript, en tete de Unit::DealDamage, avant la soustraction des
        // PV) recoit le type de degats, contrairement a OnDamage : on exclut la mort instantanee (NODAMAGE, sorts
        // d'execution a GetHealth()), les couts en PV et les chutes (SELF_DAMAGE, attaquant = victime).
        uint32 DealDamage(Unit* attaquant, Unit* victime, uint32 degats, DamageEffectType type) override
        {
            if (degats && attaquant && victime && attaquant != victime && victime->IsPlayer() && type != NODAMAGE
                && type != SELF_DAMAGE && type != HEAL)
                DeclencherSignatures(victime->ToPlayer(), degats);
            return degats;
        }
    };

    // ---- Scripts --------------------------------------------------------------------------------------------------

    class ArtisanatWorld final : public WorldScript
    {
    public:
        ArtisanatWorld() : WorldScript("CoaArtisanatWorld", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnStartup() override
        {
            ChargerVersionsEveillees();
            ChargerEtapes();
            ChargerReglesMonde();
            ChargerTalents();
        }

        // Au demarrage, ce crochet passe avant le chargement des objets : les regles se lisent alors dans OnStartup.
        void OnAfterConfigLoad(bool rechargement) override
        {
            if (rechargement)
                ChargerReglesMonde();
        }
    };

    class ArtisanatPlayer final : public PlayerScript
    {
    public:
        ArtisanatPlayer() : PlayerScript("CoaArtisanatPlayer", { PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_CREATE_ITEM,
            PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_BEFORE_SEND_LOOT,
            PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UNEQUIP_ITEM, PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT,
            PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        // Personnage supprime : son etat d'artisan part avec lui (le guid peut etre repris apres un redemarrage).
        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            if (!TablesPersonnages)
                return;
            trans->Append("DELETE FROM coa_artisanat_talent WHERE guid = {}", guid);
            trans->Append("DELETE FROM coa_artisanat_personnage WHERE guid = {}", guid);
        }

        // Talents : les marqueurs suivent la table. Signatures : a recalculer.
        void OnPlayerLogin(Player* joueur) override
        {
            // Les bots n'ont pas d'arbre (T0) : pas de requetes a leur connexion.
            if (!Talents.empty() && !(joueur->GetSession() && joueur->GetSession()->IsBot()))
                SynchroniserMarqueurs(joueur, *Etat(joueur));
            MarquerSignatures(joueur);
        }

        void OnPlayerEquip(Player* joueur, Item* /*objet*/, uint8 /*sac*/, uint8 /*emplacement*/, bool /*maj*/) override
        {
            MarquerSignatures(joueur);
        }

        void OnPlayerUnequip(Player* joueur, Item* /*objet*/) override
        {
            MarquerSignatures(joueur);
        }

        void OnPlayerAfterSetVisibleItemSlot(Player* joueur, uint8 /*emplacement*/, Item* /*objet*/) override
        {
            MarquerSignatures(joueur);
        }

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
            // Fabrication (aucune piece consommee) : la signature du specialiste (lot T).
            if (!attente->objetDeLancement && attente->candidats.empty())
                SignerALaFabrication(joueur, objet, attente->sort);
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
            // Seuls « EVEIL: » et « AMELIORER: » sont a ce script ; les autres messages du canal COA_ATL suivent leur chemin.
            std::string_view corps = std::string_view(message).substr(PREFIXE_ADDON.size());
            bool const eveil = corps.starts_with("EVEIL:");
            bool const talent = corps.starts_with("TALENT:") || corps.starts_with("METIER:") || corps == "TALENTS?";
            if (!eveil && !talent && !corps.starts_with("AMELIORER:"))
                return true;
            // Consomme meme desactive ; jamais relaye a un autre joueur.
            if (destinataire != joueur || message.size() > 255)
                return false;
            // Un message d'addon ne passe pas par UpdateSpeakTime : un delai minimal par joueur.
            Milliseconds const maintenant = GameTime::GetGameTimeMS();
            DelaiEveil* delai = joueur->CustomData.GetDefault<DelaiEveil>(CLE_DELAI);
            if (maintenant < delai->prochain)
            {
                // Reponse explicite (l'atelier grise le bouton jusqu'a la reponse), une seule par fenetre.
                if (!delai->delaiRepondu)
                    Repondre(joueur, talent ? (corps.starts_with("METIER:") ? "METIER_ERR:DELAI" : "TALENT_ERR:DELAI")
                        : eveil ? "EVEIL_ERR:DELAI" : "AMELIORER_ERR:DELAI");
                delai->delaiRepondu = true;
                return false;
            }
            delai->prochain = maintenant + DELAI_EVEIL;
            delai->delaiRepondu = false;
            if (talent)
            {
                TraiterTalentMessage(joueur, corps);
                return false;
            }
            if (eveil && !sConfigMgr->GetOption<bool>("CoaArtisanat.EveilParMessage", true))
                Repondre(joueur, "EVEIL_ERR:DESACTIVE");
            else if (!eveil && !sConfigMgr->GetOption<bool>("CoaArtisanat.AmeliorerParMessage", true))
                Repondre(joueur, "AMELIORER_ERR:DESACTIVE");
            else
                TraiterMessage(joueur, corps.substr(eveil ? 6 : 10), eveil ? SorteEtape::Eveil : SorteEtape::Palier);
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
        { "Récolte", "niveau 80 ; Forge, Couture, Cuir, Alchimie, Enchantement, Joaillerie à 450 (ou « .artisanat essai 4 <compétence> ») ; les 6 outils et la griffe 5059" },
        { "Améliorer", "niveau 19 (redescendre efface les talents) ; Heaume palier 10 porté, coût des paliers 20 et 30 ; en sac : Heaume et Gantelets palier 70, Ceinture palier 10, Garde-poignets et Solerets 213, 45377, 46154" },
        { "Éveil", "niveau 80 ; Heaume palier 70 porté + 1 en sac ; Garde-poignets palier 70 ; coût de 2 éveils" },
        { "Châsses", "niveau 80 ; Heaume palier 70 « Fabriqué par » vous ; gemmes 41398 et 40111 ; arcane 44878 ; Garde-poignets palier 70 ; parchemin 38771 ; coût de 2 éveils" },
        { "Rangs", "niveau 80 ; Heaume éveillé porté, serti, arcane posée, « Fabriqué par » vous ; réactifs des 5 rangs" },
        { "Bascule", "niveau 80, Forge 450/450, recettes apprises" },
    }};

    // Cout cumule des etapes (eveils, ou paliers) de ces pieces (table coa_artisanat_etape), pour que le banc le
    // fournisse sans cumuler.
    std::vector<Quantite> CoutsEtapes(std::initializer_list<uint32> pieces, bool eveil, uint32& orCuivre)
    {
        std::vector<Quantite> couts;
        orCuivre = 0;
        for (uint32 piece : pieces)
        {
            auto it = Etapes.find(piece);
            if (it == Etapes.end() || it->second.eveil != eveil)
                continue;
            orCuivre += it->second.orCuivre;
            for (auto const& [objet, nombre] : it->second.objets)
            {
                if (!objet)
                    continue;
                auto q = std::find_if(couts.begin(), couts.end(), [&](Quantite const& c) { return c.entree == objet; });
                if (q != couts.end())
                    q->nombre += nombre;
                else
                    couts.push_back({ objet, nombre });
            }
        }
        return couts;
    }

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
                for (uint32 outil : OUTILS_RECOLTE)
                    cases += CasesPour(joueur, outil, 1);
                break;
            case 5:
            {
                Item const* teteP10 = tete && tete->GetEntry() == HEAUME_PALIER10 ? tete : nullptr;
                cases += teteP10 ? 0 : 1;
                for (uint32 entree : PIECES_ETAPE5)
                    cases += CasesPour(joueur, entree, 1);
                uint32 orCuivre = 0;
                liste(CoutsEtapes({ HEAUME_PALIER10, HEAUME_PALIER10 + 1 }, false, orCuivre));
                break;
            }
            case 6:
            {
                bool const basePortee = tete && tete->GetEntry() == HEAUME_BASE;
                uint32 const enSac = joueur->GetItemCount(HEAUME_BASE, false) - (basePortee ? 1 : 0);
                cases += (basePortee ? 0 : 1) + (enSac < 1 ? 1 - enSac : 0) + CasesPour(joueur, GARDE_POIGNETS_BASE, 1);
                uint32 orCuivre = 0;
                liste(CoutsEtapes({ HEAUME_BASE, GARDE_POIGNETS_BASE }, true, orCuivre));
                break;
            }
            case 7:
            {
                cases += PremierTenu(joueur, HEAUME_BASE) ? 0 : 1;
                for (uint32 entree : { GEMME_META, GEMME_ROUGE, ARCANE, GARDE_POIGNETS_BASE, PARCHEMIN })
                    cases += CasesPour(joueur, entree, 1);
                uint32 orCuivre = 0;
                liste(CoutsEtapes({ HEAUME_BASE, GARDE_POIGNETS_BASE }, true, orCuivre));
                break;
            }
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
                { "talent", HandleBancTalent, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable artisanat =
            {
                { "essai", HandleEssai, SEC_GAMEMASTER, Console::No },
                { "talent", HandleTalent, SEC_GAMEMASTER, Console::No },
                { "banc", banc },
            };
            static ChatCommandTable racine = { { "artisanat", artisanat } };
            return racine;
        }

        // ---- .artisanat essai [etape] ----------------------------------------------------------------------------

        static bool HandleEssai(ChatHandler* handler, Optional<uint32> etape, Optional<uint32> competence)
        {
            if (!BancPermis(handler))
                return true;
            if (!etape || *etape < 1 || *etape > ETAPES.size())
            {
                handler->SendSysMessage("Banc d'essai de l'artisanat : .artisanat essai <étape> [compétence, étape 4], puis téléportation dans la salle d'essai.");
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
            // Ce que coutent les eveils de la seance (Heaume et Garde-poignets), et l'or qui va avec.
            auto coutsEveil = [&]
            {
                uint32 orCuivre = 0;
                for (Quantite const& q : CoutsEtapes({ HEAUME_BASE, GARDE_POIGNETS_BASE }, true, orCuivre))
                    completer(q.entree, q.nombre);
                if (joueur->GetMoney() < OR_MINIMUM + orCuivre)
                    joueur->ModifyMoney(int32(OR_MINIMUM + orCuivre - joueur->GetMoney()));
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
                {
                    // Les 6 metiers de recolte a la meme competence (450 par defaut, ou celle donnee pour tester les
                    // seuils de butin : 1, 150, 300, 425), et les 6 outils : les 12 noeuds de la salle s'ouvrent.
                    uint16 const valeur = uint16(std::clamp<uint32>(competence.value_or(450), 1, 450));
                    niveau(80);
                    for (RangsMetier const& m : METIERS)
                        Metier(joueur, m, valeur);
                    recettes(true);
                    for (uint32 outil : OUTILS_RECOLTE)
                        completer(outil, 1);
                    break;
                }
                case 5:
                {
                    // Ameliorer : Heaume palier 10 porte, cout des paliers 20 et 30 ; puis les pieces du set (fiche).
                    niveau(19);   // fiche : refus NIVEAU a 19, puis .character level 20
                    Item* porte = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD);
                    if (!porte || porte->GetEntry() != HEAUME_PALIER10)
                        porter(creer(HEAUME_PALIER10));
                    for (uint32 entree : PIECES_ETAPE5)
                        completer(entree, 1);
                    uint32 orCuivre = 0;
                    for (Quantite const& q : CoutsEtapes({ HEAUME_PALIER10, HEAUME_PALIER10 + 1 }, false, orCuivre))
                        completer(q.entree, q.nombre);
                    if (joueur->GetMoney() < OR_MINIMUM + orCuivre)
                        joueur->ModifyMoney(int32(OR_MINIMUM + orCuivre - joueur->GetMoney()));
                    break;
                }
                case 6:
                {
                    niveau(80);
                    Item* porte = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD);
                    if (!porte || porte->GetEntry() != HEAUME_BASE)
                        porter(creer(HEAUME_BASE));
                    // Le double : un Heaume palier 70 en sac en plus de celui qui est porte.
                    uint32 const enSac = joueur->GetItemCount(HEAUME_BASE, false) - (joueur->GetItemByPos(INVENTORY_SLOT_BAG_0,
                        EQUIPMENT_SLOT_HEAD) && joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_HEAD)->GetEntry() == HEAUME_BASE ? 1 : 0);
                    for (uint32 i = enSac; i < 1; ++i)
                        creer(HEAUME_BASE);
                    completer(GARDE_POIGNETS_BASE, 1);
                    coutsEveil();
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
                    coutsEveil();
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
            handler->PSendSysMessage("banc etat: materiaux de monde {} : {} butins de creature examines, {} ecartes (gris ou sans XP), {} ecartes (bot), {} objets poses",
                Monde.actif ? "actifs" : "coupes", ButinsMonde.load(), EcartesGris.load(), EcartesBots.load(), ChutesMonde.load());
            return true;
        }

        // Une etape payante sur le banc (palier ou eveil), avec ses preuves : refus sans reactifs puis au niveau
        // requis - 1 (pour la premiere etape), cout fourni a l'exact, prelevement verifie, recopie verifiee. L'or du
        // personnage est rendu a la fin. Rend la piece neuve, ou nullptr.
        static Item* EtapeBanc(ChatHandler* handler, Player* j, Item* piece, SorteEtape sorte, std::string const& tag, bool refus)
        {
            bool const eveil = sorte == SorteEtape::Eveil;
            auto it = Etapes.find(piece->GetEntry());
            if (it == Etapes.end() || it->second.eveil != eveil)
            {
                handler->PSendSysMessage("{}: aucune etape {} en table pour {}.", tag, eveil ? "d'eveil" : "de palier", piece->GetEntry());
                return nullptr;
            }
            EtapeArtisanat const& etape = it->second;
            uint32 const de = piece->GetEntry();
            Chasses const avant = Photographier(j, piece);
            uint32 const bonusAvant = piece->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT);
            uint32 const orOrigine = j->GetMoney();
            uint8 const niveauOrigine = j->GetLevel();
            RefusEveil r;
            auto fournir = [&]
            {
                for (auto const& [objet, nombre] : etape.objets)
                    if (objet)
                        if (uint32 const reste = Completer(j, objet, nombre))
                            handler->PSendSysMessage("{}: sacs pleins, {} x{} non donnes.", tag, objet, reste);
                j->SetMoney(etape.orCuivre + 10000);
            };
            auto intacte = [&] { return j->GetItemByGuid(avant.source) ? "oui" : "non"; };

            if (refus)
            {
                for (auto const& [objet, nombre] : etape.objets)
                    if (objet)
                        j->DestroyItemCount(objet, j->GetItemCount(objet, false), true);
                j->SetMoney(0);
                Item* x = Avancer(j, piece, sorte, r, "banc");
                handler->PSendSysMessage("{}: sans reactifs -> {} (attendu REACTIFS:...), piece intacte={}", tag,
                    x ? std::string("PASSE") : r.cause, intacte());
                if (x)
                    return j->SetMoney(orOrigine), x;
                fournir();
                j->SetLevel(etape.niveau - 1);
                x = Avancer(j, piece, sorte, r, "banc");
                j->SetLevel(niveauOrigine);
                bool reactifsIntacts = j->GetMoney() == etape.orCuivre + 10000;
                for (auto const& [objet, nombre] : etape.objets)
                    reactifsIntacts = reactifsIntacts && (!objet || j->GetItemCount(objet, false) >= nombre);
                handler->PSendSysMessage("{}: niveau {} -> {} (attendu NIVEAU), piece intacte={}, reactifs intacts={}", tag,
                    uint32(etape.niveau - 1), x ? std::string("PASSE") : r.cause, intacte(), reactifsIntacts ? "oui" : "non");
                if (x)
                    return j->SetMoney(orOrigine), x;
            }

            fournir();
            std::vector<std::pair<uint32, uint32>> avantCouts;
            for (auto const& [objet, nombre] : etape.objets)
                if (objet)
                    avantCouts.emplace_back(objet, j->GetItemCount(objet, false));
            uint32 const orAvant = j->GetMoney();
            Item* neuve = Avancer(j, piece, sorte, r, "banc");
            if (!neuve)
            {
                handler->PSendSysMessage("{}: {} refuse : {}", tag, de, r.cause);
                j->SetMoney(orOrigine);
                return nullptr;
            }
            std::string couts;
            bool justes = true;
            for (auto const& [objet, avantN] : avantCouts)
            {
                uint32 attendu = 0;
                for (auto const& [o, n] : etape.objets)
                    if (o == objet)
                        attendu += n;
                uint32 const apres = j->GetItemCount(objet, false);
                uint32 const pris = avantN >= apres ? avantN - apres : 0;
                justes = justes && pris == attendu;
                couts += Acore::StringFormat(" {}:{}/{}", objet, pris, attendu);
            }
            uint32 const orPris = orAvant >= j->GetMoney() ? orAvant - uint32(j->GetMoney()) : 0;
            justes = justes && orPris == etape.orCuivre;
            couts += Acore::StringFormat(" or:{}/{}", orPris, etape.orCuivre);
            handler->PSendSysMessage("{}: {} -> {} niveau d'objet {} ; recopie {} ; ancienne detruite={} ; couts{} {}", tag, de,
                Decrire(neuve), neuve->GetTemplate()->ItemLevel, Conforme(avant, bonusAvant, neuve) ? "CONFORME" : "ECART",
                j->GetItemByGuid(avant.source) ? "non" : "oui", couts, justes ? "JUSTES" : "FAUX");
            j->SetMoney(orOrigine);
            return neuve;
        }

        // Phase 1 : Heaume palier 10 fabrique par le personnage (sans chasse), enchantement permanent (parchemin) et
        // temporaire (huile, 30 min). Phase 2 : les 6 paliers 10 -> 70 (AMELIORER). Phase 3 : eveil du palier 70. Phase 4 : rangs
        // 1-5 depuis la piece portee. Chaque etape payante : refus sans reactifs et au niveau requis - 1 (premiere
        // etape), puis cout preleve et recopie verifies. Chaque phase sauvegarde le personnage : item_instance se lit
        // entre deux phases.
        static bool HandleBancRecopie(ChatHandler* handler, std::string nom, uint32 phase)
        {
            if (!BancPermis(handler))
                return true;
            // Phase 0 : les phases 1 a 4 d'une traite (P-238 : un bot aleatoire peut etre reinitialise entre deux
            // commandes, et son inventaire part avec).
            if (phase == 0)
            {
                for (uint32 p = 1; p <= 4; ++p)
                    HandleBancRecopie(handler, nom, p);
                return true;
            }
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
                Item* heaume = CreerEnSac(j, HEAUME_PALIER10);
                if (!heaume)
                    return handler->SendSysMessage("banc recopie 1: sacs pleins."), true;
                SignerPar(j, heaume);
                Sertir(j, heaume);
                PoserEnchantement(j, heaume, PERM_ENCHANTMENT_SLOT, EnchantementDeObjet(PARCHEMIN), 0);
                PoserEnchantement(j, heaume, TEMP_ENCHANTMENT_SLOT, EnchantementDeObjet(HUILE), 30 * MINUTE * IN_MILLISECONDS);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc recopie 1: palier 10 {} ; joueur guid {}", Decrire(heaume), j->GetGUID().GetCounter());
                return true;
            }

            if (phase == 2)
            {
                Item* piece = PremierTenu(j, HEAUME_PALIER10);
                if (!piece)
                    return handler->SendSysMessage("banc recopie 2: aucun Heaume palier 10 (phase 1 d'abord)."), true;
                for (uint32 n = 0; piece && piece->GetEntry() != HEAUME_BASE && n < 6; ++n)
                    piece = EtapeBanc(handler, j, piece, SorteEtape::Palier, Acore::StringFormat("banc recopie 2: palier {}", n + 1), n == 0);
                handler->PSendSysMessage("banc recopie 2: fin sur {} (attendu {}).", piece ? piece->GetEntry() : 0, HEAUME_BASE);
                j->SaveToDB(false, false);
                return true;
            }

            if (phase == 3)
            {
                Item* heaume = PremierTenu(j, HEAUME_BASE);
                if (!heaume)
                    return handler->SendSysMessage("banc recopie 3: aucun Heaume palier 70 (phases 1 et 2 d'abord)."), true;
                // Les paliers n'ont pas de chasse : on sertit la 213 apres l'eveil, pour que les rangs recopient des gemmes.
                if (Item* eveillee = EtapeBanc(handler, j, heaume, SorteEtape::Eveil, "banc recopie 3: eveil", true))
                {
                    Sertir(j, eveillee);
                    handler->PSendSysMessage("banc recopie 3: 213 sertie {}", Decrire(eveillee));
                }
                j->SaveToDB(false, false);
                return true;
            }

            if (phase == 4)
            {
                Item* depart = PremierTenu(j, HEAUME_EVEILLE);
                if (!depart)
                    return handler->SendSysMessage("banc recopie 4: aucun Heaume eveille (phases 1 a 3 d'abord)."), true;
                // P-238 : un bot en vol ne lance rien (SPELL_FAILED_NOT_ON_TAXI) ; monte, on le fait descendre
                // (SPELL_FAILED_NOT_MOUNTED).
                if (j->IsInFlight())
                    return handler->SendSysMessage("banc recopie 4: le bot est en vol, relancer quand il sera pose."), true;
                if (j->IsMounted())
                {
                    j->Dismount();
                    j->RemoveAurasByType(SPELL_AURA_MOUNTED);
                }
                // Le casque d'origine prend la place du Heaume en sac ; il revient a la fin, sur tous les chemins.
                uint16 const placeOrigine = depart->IsEquipped() ? uint16(0) : depart->GetPos();
                if (!depart->IsEquipped() && !Porter(j, depart))
                    return handler->SendSysMessage("banc recopie 4: le Heaume eveille ne se porte pas."), true;
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
                handler->PSendSysMessage("banc recopie 4: sans reactifs -> resultat {} (attendu {} REAGENTS), piece intacte={}",
                    uint32(r), uint32(SPELL_FAILED_REAGENTS), intacte ? "oui" : "NON");
                if (!intacte)
                    return remettre(), true;
                for (Quantite const& q : REACTIFS_RANGS)
                    Completer(j, q.entree, q.nombre);
                j->SetLevel(NIVEAU_EVEIL - 1);
                r = lancer();
                j->SetLevel(NIVEAU_EVEIL);
                intacte = j->GetItemByGuid(courant) != nullptr;
                handler->PSendSysMessage("banc recopie 4: niveau 79 -> resultat {} (condition : 172), piece intacte={}, reactifs intacts={}",
                    uint32(r), intacte ? "oui" : "NON", j->GetItemCount(9100101, false) == 24 ? "oui" : "NON");
                if (!intacte)
                    return remettre(), true;

                for (uint32 rang = 1; rang <= 5; ++rang)
                {
                    Item* piece = j->GetItemByGuid(courant);
                    SpellInfo const* sort = piece ? sSpellMgr->GetSpellInfo(piece->GetTemplate()->Spells[0].SpellId) : nullptr;
                    if (!sort)
                        return handler->PSendSysMessage("banc recopie 4: rang {} sans piece ou sans sort de renforcement.", rang), remettre(), true;
                    if (!piece->IsEquipped() && !Porter(j, piece))
                        return handler->PSendSysMessage("banc recopie 4: rang {} : la piece ne se porte pas.", rang), remettre(), true;
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
                        return handler->PSendSysMessage("banc recopie 4: rang {} sort {} -> resultat {}, piece {} absente ; couts{}",
                            rang, sort->Id, uint32(r), attendu, couts), remettre(), true;
                    handler->PSendSysMessage("banc recopie 4: rang {} sort {} -> {} niveau d'objet {} ; recopie {} ; ancienne detruite={} ; couts{} {}",
                        rang, sort->Id, Decrire(neuve), neuve->GetTemplate()->ItemLevel,
                        Conforme(avant, bonusAvant, neuve) ? "CONFORME" : "ECART", j->GetItemByGuid(avant.source) ? "non" : "oui",
                        couts, coutJuste ? "JUSTES" : "FAUX");
                    courant = neuve->GetGUID();
                }
                remettre();
                return true;
            }
            handler->SendSysMessage("banc recopie <nom> <0|1|2|3|4> (0 : les quatre d'une traite)");
            return true;
        }

        // ---- Talents : .artisanat talent (MJ, sur soi) et .artisanat banc talent (bots, preuves) -------------------

        // Actions communes : etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | oublier.
        static bool ActionTalent(ChatHandler* handler, Player* j, std::string const& action, Optional<std::string> const& a,
            Optional<std::string> const& b)
        {
            EtatTalents* e = Etat(j);
            auto etat = [&]
            {
                handler->PSendSysMessage("talent: metiers C={} R={} ; gagnes C={} R={} ; disponibles C={} R={} ; plafonds C={} R={} (niveau {}) ; {} sorts connus",
                    e->metierC, e->metierR, e->gagnesC, e->gagnesR, Disponibles(j, *e, false), Disponibles(j, *e, true),
                    Plafond(j->GetLevel(), false), Plafond(j->GetLevel(), true), uint32(j->GetLevel()), uint32(e->sorts.size()));
            };
            int32 n = 0;
            if (action == "etat")
            {
                etat();
                std::string liste;
                for (uint32 sort : e->sorts)
                    liste += " " + std::to_string(sort);
                handler->PSendSysMessage("talent: sorts{}", liste.empty() ? " (aucun)" : liste);
                return true;
            }
            if (action == "points" && a && b && LireEntier(*b, n) && n >= 0 && (*a == "C" || *a == "R"))
            {
                (*a == "C" ? e->gagnesC : e->gagnesR) = uint32(n);
                SauverPersonnage(j, *e);
                etat();
                return true;
            }
            if (action == "metier" && a && b && LireEntier(*b, n) && n > 0 && a->size() == 1)
            {
                std::string const r = ChoisirMetier(j, (*a)[0], uint32(n));
                handler->PSendSysMessage("talent: metier {} {} -> {}", *a, n, r.empty() ? "OK" : r);
                return true;
            }
            if (action == "apprendre" && a && LireEntier(*a, n) && n > 0)
            {
                std::string const r = ApprendreTalent(j, uint32(n));
                handler->PSendSysMessage("talent: apprendre {} -> {} ; disponibles C={} R={}", n, r.empty() ? "OK" : r,
                    Disponibles(j, *e, false), Disponibles(j, *e, true));
                return true;
            }
            if (action == "oublier")
            {
                OublierTalents(j);
                etat();
                return true;
            }
            return false;
        }

        static bool HandleTalent(ChatHandler* handler, std::string action, Optional<std::string> a, Optional<std::string> b)
        {
            if (!BancPermis(handler))
                return true;
            Player* j = handler->GetPlayer();
            if (!j || !ActionTalent(handler, j, action, a, b))
                handler->SendSysMessage("talent etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | oublier");
            return true;
        }

        // Fabrique, par la recette (sort declenche, enclume et forge invoquees a cote), la piece `entree` ; rend la piece
        // neuve ou nullptr.
        static Item* FabriquerBanc(ChatHandler* handler, Player* j, uint32 entree)
        {
            uint32 recette = 0;
            for (uint32 id : RecettesForge())
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id); info && EntreeCreeeArtisanat(info) == entree)
                    recette = id;
            if (!recette)
                return handler->PSendSysMessage("banc talent: aucune recette ne cree {}.", entree), nullptr;
            for (uint32 focus : { 192628u, 1685u })   // enclume, forge (salle d'essai)
                j->SummonGameObject(focus, j->GetPositionX() + 1.5f, j->GetPositionY(), j->GetPositionZ(), 0.0f, 0, 0, 0, 0, 60);
            std::set<ObjectGuid> avant;
            PourChaquePieceTenue(j, entree, [&](Item* o) { avant.insert(o->GetGUID()); });
            SpellCastResult const res = j->CastSpell(j, recette, TriggerCastFlags(TRIGGERED_FULL_MASK));
            Item* neuve = nullptr;
            PourChaquePieceTenue(j, entree, [&](Item* o) { if (!avant.count(o->GetGUID())) neuve = o; });
            if (!neuve)
                handler->PSendSysMessage("banc talent: recette {} -> resultat {}, aucune piece {} creee.", recette, uint32(res), entree);
            return neuve;
        }

        // .artisanat banc talent <bot> <action> : les actions communes, plus
        //   signature : fabrique le Heaume palier 10 et les Garde-poignets palier 10, et lit la signature posee ;
        //   egide : porte les deux pieces signees, recalcule (une seule Egide active), puis deux coups qui passent sous le
        //           seuil : le premier est absorbe, le second non (recharge partagee).
        static bool HandleBancTalent(ChatHandler* handler, std::string nom, std::string action, Optional<std::string> a,
            Optional<std::string> b)
        {
            if (!BancPermis(handler))
                return true;
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            if (ActionTalent(handler, j, action, a, b))
                return true;

            if (action == "signature")
            {
                Forge(j, 450);
                for (uint32 entree : { HEAUME_PALIER10, uint32(PIECES_BANC_SET[3]) })   // Heaume et Garde-poignets, palier 10
                    if (Item* piece = FabriquerBanc(handler, j, entree))
                    {
                        uint32 const enchant = piece->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_0);
                        auto s = Signatures.find(enchant);
                        handler->PSendSysMessage("banc talent: fabrique {} (guid {}) ; emplacement 7 = {} ({}) ; fabricant={}",
                            entree, piece->GetGUID().GetCounter(), enchant,
                            s != Signatures.end() ? Acore::StringFormat("{} rang {}", s->second.forme, uint32(s->second.rang)) : std::string("aucune signature"),
                            piece->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter());
                    }
                j->SaveToDB(false, false);
                return true;
            }

            if (action == "egide")
            {
                uint32 portees = 0;
                for (uint32 entree : { HEAUME_PALIER10, uint32(PIECES_BANC_SET[3]) })
                {
                    Item* signee = nullptr;
                    PourChaquePieceTenue(j, entree, [&](Item* o)
                    {
                        if (!signee && Signatures.count(o->GetEnchantmentId(PROP_ENCHANTMENT_SLOT_0)))
                            signee = o;
                    });
                    if (signee && (signee->IsEquipped() || Porter(j, signee)))
                        ++portees;
                }
                RecalculerSignatures(j);
                SignaturesActives* sa = j->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
                handler->PSendSysMessage("banc talent: {} pieces signees portees ; {} signature(s) active(s){}", portees,
                    uint32(sa->actives.size()), sa->actives.empty() ? std::string() : Acore::StringFormat(" : {} rang {} valeur {} au 80",
                    sa->actives[0]->forme, uint32(sa->actives[0]->rang), sa->actives[0]->valeur));
                if (sa->actives.empty())
                    return true;
                sa->sursaut = Milliseconds(0);
                // L'attaquant doit etre un autre que la victime (les degats qu'on s'inflige sont exclus) : un mannequin.
                TempSummon* frappeur = j->SummonCreature(32666, j->GetPositionX() + 2.0f, j->GetPositionY(), j->GetPositionZ(), 0.0f,
                    TEMPSUMMON_TIMED_DESPAWN, 15 * IN_MILLISECONDS);
                if (!frappeur)
                    return handler->SendSysMessage("banc talent: impossible d'invoquer le mannequin 32666."), true;
                for (uint32 coup = 1; coup <= 2; ++coup)
                {
                    j->SetHealth(j->GetMaxHealth() / 2);
                    uint32 const degats = j->GetMaxHealth() / 4;          // de 50 % a 25 % : passe sous 35 %
                    uint32 const pvAvant = j->GetHealth();
                    uint32 const avant = DeclenchementsSignatures.load();
                    Unit::DealDamage(frappeur, j, degats, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    uint32 const perdus = pvAvant > j->GetHealth() ? pvAvant - j->GetHealth() : 0;
                    handler->PSendSysMessage("banc talent: coup {} : {} degats, {} PV perdus, declenchement={} (attendu {})", coup, degats,
                        perdus, DeclenchementsSignatures.load() - avant, coup == 1 ? 1 : 0);
                }
                if (uint32 const effet = sa->actives[0]->sortEffet)
                    j->RemoveAurasDueToSpell(effet);
                j->SetHealth(j->GetMaxHealth());
                return true;
            }

            handler->SendSysMessage("banc talent <bot> etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | oublier | signature | egide");
            return true;
        }

        // Les emplacements gardes : PERM, TEMP (id), SOCK1-3, prismatique, signature (PROP 0), bonus de chasse, fabricant.
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
            for (uint32 entree : PIECES_BANC_SET)
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
    new ArtisanatButinMonde();
    new ArtisanatUnit();
    new ArtisanatCommandes();
}
