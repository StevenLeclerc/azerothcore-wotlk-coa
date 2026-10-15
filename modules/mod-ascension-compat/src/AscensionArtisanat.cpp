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

#include "AscensionArtisanatIds.h"
#include "Battleground.h"
#include "CharacterCache.h"
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
#include "Mail.h"
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
#include "StringConvert.h"
#include "Tokenize.h"
#include "Realm.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <map>
#include <mutex>
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
    std::string const CLE_DELAI_EXTRAIT = "CoaArtisanatDelaiExtrait";
    constexpr Milliseconds DELAI_EVEIL{ 500 };
    // EXTRAIT? (D-PO, retour de dev-addon 83b2ebc) : hors du delai partage de 500 ms ; son propre garde-fou anti-rafale,
    // EXTRAIT_PAR_FENETRE demandes par seconde (l'Atelier s'en tient a 1 par case et par seconde, 4 par seconde au total).
    constexpr Milliseconds FENETRE_EXTRAIT{ 1000 };
    constexpr uint32 EXTRAIT_PAR_FENETRE = 8;

    // Emplacements recopies. BONUS_ENCHANTMENT_SLOT (bonus de chasse) se recalcule sur la piece neuve. Les
    // PROP_ENCHANTMENT_SLOT_* servent aux suffixes aleatoires, que nos pieces n'ont pas (0 piece de 9101200-9104999 avec
    // RandomProperty ou RandomSuffix, mesure le 15/10) : le 7 porte la signature de l'artisan (lot T, D-PO-34), le 8 la
    // facette d'une Oeuvre legendaire, le 9 le recolteur (GUID bas dans les charges, ENCHANT_EXTRAIT ; lot T1, T1-5).
    // Tous suivent la piece aux paliers, a l'eveil et aux rangs.
    constexpr std::array<EnchantmentSlot, 9> EMPLACEMENTS_RECOPIES =
    {
        PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT_2,
        SOCK_ENCHANTMENT_SLOT_3, PRISMATIC_ENCHANTMENT_SLOT, PROP_ENCHANTMENT_SLOT_0, PROP_ENCHANTMENT_SLOT_1,
        PROP_ENCHANTMENT_SLOT_2
    };
    constexpr EnchantmentSlot EMPLACEMENT_SIGNATURE = PROP_ENCHANTMENT_SLOT_0;   // 7
    constexpr EnchantmentSlot EMPLACEMENT_FACETTE = PROP_ENCHANTMENT_SLOT_1;     // 8
    constexpr EnchantmentSlot EMPLACEMENT_EXTRAIT = PROP_ENCHANTMENT_SLOT_2;     // 9

    bool EstOeuvre(uint32 entree)
    {
        return entree && std::find(CoaArtisanatIds::OEUVRES.begin(), CoaArtisanatIds::OEUVRES.end(), entree)
            != CoaArtisanatIds::OEUVRES.end();
    }

    // Un enchantement peut-il porter un GUID dans ses charges sans que le coeur les decompte ? Verdict T1-1 : le coeur ne
    // touche aux charges d'un emplacement PROP que pour un type ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL (Player.cpp:7664-7733)
    // et ne vide un emplacement que si sa duree est non nulle (UpdateEnchantTime, Player::AddEnchantmentDuration). On
    // exige donc des types tous a 0. Rend vide si oui, sinon la raison.
    std::string RaisonEnchantementNonNeutre(uint32 enchant)
    {
        SpellItemEnchantmentEntry const* e = sSpellItemEnchantmentStore.LookupEntry(enchant);
        if (!e)
            return "inconnu du DBC serveur";
        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
            if (e->type[i] != ITEM_ENCHANTMENT_TYPE_NONE)
                return Acore::StringFormat("effet {} de type {} (0 exige)", uint32(i), uint32(e->type[i]));
        return {};
    }

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
        // Oeuvre (T1-5) : les Coeurs de foudre tenus avant la consommation (objet, recolteur) ; celui qui a disparu a la
        // creation est celui que la recette a pris (pile de 1 : un objet par Coeur).
        std::vector<std::pair<ObjectGuid, ObjectGuid>> materiauxLegendaires;

        void Vider()
        {
            sort = 0;
            entreeCreee = 0;
            objetDeLancement = false;
            candidats.clear();
            materiauxLegendaires.clear();
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

    struct RafaleExtrait final : public DataMap::Base
    {
        Milliseconds debut{ 0 };
        uint32 nombre = 0;
    };

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
        for (uint8 s = PERM_ENCHANTMENT_SLOT; s <= EMPLACEMENT_EXTRAIT; ++s)
            if (uint32 const id = objet->GetEnchantmentId(EnchantmentSlot(s)))
                texte += Acore::StringFormat(" [{}]={}/{}ms/{}ch", uint32(s), id, objet->GetEnchantmentDuration(EnchantmentSlot(s)),
                    objet->GetEnchantmentCharges(EnchantmentSlot(s)));
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

    // Points gagnes par les jalons de niveau (D-PO-42, b1) : calcules, jamais stockes, donc retroactifs et hors de portee
    // d'un oubli. Creation : 1 au 10 puis 1 a chaque niveau impair (6 en 10-19, 36 au 80). Recolte : 1 au 10 puis 1 tous
    // les 3 niveaux (4 en 10-19, 24 au 80). Toujours sous Plafond().
    uint32 Jalons(uint8 niveau, bool recolte)
    {
        if (niveau < 10)
            return 0;
        return recolte ? 1 + (niveau - 10) / 3 : 1 + (niveau - 9) / 2;
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
        uint32 dureeMs = 0;
        uint8 rang = 0;
        uint8 seuilPct = 0;
        uint8 groupeSursaut = 0;
        uint8 pieceClasse = 0;
        std::string voie;
        std::string forme;
        std::string effet;
    };
    std::unordered_map<uint32, SignatureDef> Signatures;      // par enchantement
    // La facette Coeur de forge (effet EFFET_COEUR_DE_FORGE) : son aura (sort_effet) est lue a chaque coup par les crochets
    // de degats et de soins ; 0 : pas de ligne, rien n'est lu.
    uint32 AuraCoeurDeForge = 0;
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
        AuraCoeurDeForge = 0;
        if (TableExiste(WorldDatabase, "coa_artisanat_signature"))
            if (QueryResult r = WorldDatabase.Query("SELECT enchant, metier, voie, forme, rang, effet, valeur, valeur_arene, recharge_ms, "
                "seuil_pv_pct, groupe_sursaut, piece_classe, piece_masque, sort_effet, duree_ms FROM coa_artisanat_signature"))
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
                    s.dureeMs = f[14].Get<uint32>();
                    bool const coeur = s.effet == CoaArtisanatIds::EFFET_COEUR_DE_FORGE;
                    // Coeur de forge : un pourcentage (au plus 100), une aura a poser (sort_effet), une recharge.
                    if (!sSpellItemEnchantmentStore.LookupEntry(s.enchant) || (s.sortEffet && !sSpellMgr->GetSpellInfo(s.sortEffet))
                        || s.forme.empty() || !s.rang || s.seuilPct > 100
                        || (coeur && (!s.sortEffet || s.valeur > 100 || s.valeurArene > 100)))
                    {
                        ++signaturesRejetees;
                        LOG_ERROR("module", "coa artisanat: signature rejetee enchant={} (enchantement ou sort d'effet inconnu, forme ou rang vide, "
                            "ou coeur_de_forge sans aura ou au-dela de 100 %)", s.enchant);
                        continue;
                    }
                    if (coeur)
                    {
                        if (AuraCoeurDeForge && AuraCoeurDeForge != s.sortEffet)
                            LOG_ERROR("module", "coa artisanat: deux auras coeur_de_forge ({} et {}) : seule la premiere est lue",
                                AuraCoeurDeForge, s.sortEffet);
                        else
                            AuraCoeurDeForge = s.sortEffet;
                    }
                    Signatures[s.enchant] = s;
                } while (r->NextRow());

        LOG_INFO("module", "coa artisanat: talents : {} sorts ({} marqueurs de metier), {} rejetes ; signatures : {}, {} rejetees ; tables personnages {}",
            uint32(Talents.size()), uint32(MarqueursMetier.size()), rejetes, uint32(Signatures.size()), signaturesRejetees,
            TablesPersonnages ? "presentes" : "ABSENTES");

        // Lot T1 (Sourcier) : ce qui attend un identifiant d'un autre agent reste inerte, et le dit.
        using namespace CoaArtisanatIds;
        if (!ENCHANT_EXTRAIT)
            LOG_INFO("module", "coa sourcier: ENCHANT_EXTRAIT = 0 (dev-dbc) : emplacement 9 non pose a la fabrication, inerte");
        else if (std::string const raison = RaisonEnchantementNonNeutre(ENCHANT_EXTRAIT); !raison.empty())
            LOG_ERROR("module", "coa sourcier: ENCHANT_EXTRAIT {} non neutre ({}) : emplacement 9 non pose", ENCHANT_EXTRAIT, raison);
        if (!AuraCoeurDeForge)
            LOG_INFO("module", "coa sourcier: coeur de forge inerte (aucune ligne coa_artisanat_signature effet 'coeur_de_forge')");
        if (!OBJET_COEUR_DE_FOUDRE || !OEUVRES[0])
            LOG_INFO("module", "coa sourcier: Coeur de foudre {} / Oeuvre {} : fabrication de l'Oeuvre inerte (0 = non recu)",
                OBJET_COEUR_DE_FOUDRE, OEUVRES[0]);
    }

    bool EnchantExtraitUtilisable()
    {
        return CoaArtisanatIds::ENCHANT_EXTRAIT && RaisonEnchantementNonNeutre(CoaArtisanatIds::ENCHANT_EXTRAIT).empty();
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
        // gagnes = sources hors jalons (MJ, puis premieres fois et grimoires).
        uint32 const plafond = std::min(Jalons(joueur->GetLevel(), recolte) + (recolte ? e.gagnesR : e.gagnesC),
            Plafond(joueur->GetLevel(), recolte));
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

    // Oubli (D-PO-42, b2) : sorts et metiers principaux vides, la reserve gagnee reste (« celui qui cherche n'a jamais rien
    // perdu »). `effacer` (MJ) supprime tout, points compris.
    void OublierTalents(Player* joueur, bool effacer = false)
    {
        EtatTalents* e = Etat(joueur);
        e->sorts.clear();
        e->metierC = e->metierR = 0;
        if (effacer)
            e->gagnesC = e->gagnesR = 0;
        SynchroniserMarqueurs(joueur, *e);
        if (TablesPersonnages)
        {
            CharacterDatabase.Execute("DELETE FROM coa_artisanat_talent WHERE guid = {}", joueur->GetGUID().GetCounter());
            if (effacer)
                CharacterDatabase.Execute("DELETE FROM coa_artisanat_personnage WHERE guid = {}", joueur->GetGUID().GetCounter());
            else
                SauverPersonnage(joueur, *e);
        }
    }

    // Etat pour l'atelier : « TALENTS:<metC>:<metR>:<dispoC>:<dispoR>:<plafC>:<plafR>:<gagC>:<gagR> », puis la liste
    // des sorts connus en « TALENTS_APPRIS:<k>/<n>:<sort>,... » (decoupee sous 255 caracteres). <gag> = jalons + gagnes.
    void EnvoyerEtat(Player* joueur)
    {
        EtatTalents* e = Etat(joueur);
        uint8 const niveau = joueur->GetLevel();
        Repondre(joueur, Acore::StringFormat("TALENTS:{}:{}:{}:{}:{}:{}:{}:{}", e->metierC, e->metierR, Disponibles(joueur, *e, false),
            Disponibles(joueur, *e, true), Plafond(niveau, false), Plafond(niveau, true), Jalons(niveau, false) + e->gagnesC,
            Jalons(niveau, true) + e->gagnesR));
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
            if (auto it = Talents.find(sort); it != Talents.end() && it->second.metier == e->metierC
                && !it->second.talent.starts_with(CoaArtisanatIds::PREFIXE_TALENT_FACETTE))
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

    // Metier (SkillLine) d'une recette, 0 si elle n'est rattachee a aucun.
    uint32 MetierDeRecette(uint32 recette)
    {
        uint32 metier = 0;
        auto bornes = sSpellMgr->GetSkillLineAbilityMapBounds(recette);
        for (auto it = bornes.first; it != bornes.second; ++it)
            metier = it->second->SkillLine;
        return metier;
    }

    // Emplacement 9 : GUID bas du recolteur dans les charges de l'enchantement neutre ENCHANT_EXTRAIT (verdict T1-1). La
    // piece n'est pas portee a la creation ; si elle l'etait, l'enchantement est neutre : rien a appliquer.
    bool PoserExtrait(Player* joueur, Item* objet, uint32 recolteurBas)
    {
        if (!recolteurBas || !EnchantExtraitUtilisable())
            return false;
        objet->SetEnchantment(EMPLACEMENT_EXTRAIT, CoaArtisanatIds::ENCHANT_EXTRAIT, 0, recolteurBas, joueur->GetGUID());
        objet->SetState(ITEM_CHANGED, joueur);
        return true;
    }

    // Facette d'une Oeuvre (emplacement 8) : le talent « facette:<forme> » que connait l'artisan, specialiste du metier de
    // la recette ; l'enchantement est la ligne coa_artisanat_signature de cette forme et de ce metier (rang le plus haut).
    // Rend l'enchantement pose, 0 sinon.
    uint32 PoserFacette(Player* joueur, Item* objet, uint32 recette)
    {
        if (Signatures.empty() || !TablesPersonnages)
            return 0;
        EtatTalents* e = Etat(joueur);
        uint32 const metier = MetierDeRecette(recette);
        if (!metier || metier != e->metierC)
            return 0;
        std::string forme;
        for (uint32 sort : e->sorts)
            if (auto it = Talents.find(sort); it != Talents.end() && it->second.metier == metier
                && it->second.talent.starts_with(CoaArtisanatIds::PREFIXE_TALENT_FACETTE))
                forme = it->second.talent.substr(std::string_view(CoaArtisanatIds::PREFIXE_TALENT_FACETTE).size());
        if (forme.empty())
            return 0;
        SignatureDef const* choisie = nullptr;
        for (auto const& [enchant, s] : Signatures)
            if (s.metier == metier && s.forme == forme && (!choisie || s.rang > choisie->rang))
                choisie = &s;
        if (!choisie)
        {
            LOG_ERROR("module", "coa sourcier: facette {} sans ligne coa_artisanat_signature (metier {}) : Oeuvre sans facette", forme, metier);
            return 0;
        }
        objet->SetEnchantment(EMPLACEMENT_FACETTE, choisie->enchant, 0, 0, joueur->GetGUID());
        objet->SetState(ITEM_CHANGED, joueur);
        return choisie->enchant;
    }

    // Signatures actives (T1-5) : d'abord la facette de l'Oeuvre portee (emplacement 8, elle prend une des 3 places), puis
    // les signatures (emplacement 7) dans l'ordre des emplacements d'equipement ; 3 au plus. Une facette ou une signature de
    // meme nature (metier, voie, forme) qu'une autre deja retenue ne s'y ajoute pas : la plus forte (valeur) des deux reste.
    struct SignaturesActives final : public DataMap::Base
    {
        std::vector<SignatureDef const*> actives;
        bool sale = true;                   // equipement change depuis le dernier calcul
        Milliseconds sursaut{ 0 };          // fin de la recharge partagee des sursauts de survie
        std::map<uint32, Milliseconds> recharges;   // autres signatures, par enchantement
        uint32 chargesCoeur = 0;            // critiques comptes pour Coeur de forge
        uint32 egides = 0;                  // declenchements de sursaut de ce joueur (banc de garde G1)
        uint32 coeurs = 0;                  // declenchements de Coeur de forge de ce joueur (banc de garde G1)
    };
    std::atomic<uint32> DeclenchementsSignatures{ 0 };
    std::atomic<uint32> DeclenchementsCoeur{ 0 };

    void RecalculerSignatures(Player* joueur)
    {
        SignaturesActives* a = joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
        a->actives.clear();
        a->sale = false;
        if (Signatures.empty())
            return;
        std::vector<SignatureDef const*> facettes;
        for (EnchantmentSlot const emplacement : { EMPLACEMENT_FACETTE, EMPLACEMENT_SIGNATURE })
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* objet = joueur->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!objet || objet->IsBroken())
                    continue;
                auto it = Signatures.find(objet->GetEnchantmentId(emplacement));
                if (it == Signatures.end())
                    continue;
                SignatureDef const* s = &it->second;
                auto meme = std::find_if(a->actives.begin(), a->actives.end(), [&](SignatureDef const* x)
                {
                    return x->metier == s->metier && x->voie == s->voie && x->forme == s->forme;
                });
                if (meme != a->actives.end())
                {
                    // Deux signatures : le rang (regle d'avant T1). Une facette et une signature : la valeur.
                    bool const facetteEnJeu = emplacement == EMPLACEMENT_FACETTE
                        || std::find(facettes.begin(), facettes.end(), *meme) != facettes.end();
                    if (facetteEnJeu ? s->valeur > (*meme)->valeur : s->rang > (*meme)->rang)
                        *meme = s;
                }
                else if (a->actives.size() < 3)
                {
                    a->actives.push_back(s);
                    if (emplacement == EMPLACEMENT_FACETTE)
                        facettes.push_back(s);
                }
            }
    }

    // Arene 1 contre 1 contre un gladiateur (conception § 5, revue JcJ n° 7) : les effets d'artisanat y sont neutralises.
    // Arene de type TYPE_ARENE_1C1 dont un des combattants est un bot (le gladiateur ; un banc bot contre bot aussi).
    bool NeutraliseEnArene1c1(Player const* joueur)
    {
        Battleground* bg = joueur ? joueur->GetBattleground() : nullptr;
        if (!bg || !bg->isArena() || bg->GetArenaType() != CoaArtisanatIds::TYPE_ARENE_1C1)
            return false;
        for (auto const& [guid, p] : bg->GetPlayers())
            if (p && p->GetSession() && p->GetSession()->IsBot())
                return true;
        return false;
    }

    // Valeur JcJ (T1-5, revue JcJ n° 2) : des que l'autre partie est controlee par un joueur (un bot est un joueur), en arene,
    // en champ de bataille comme en monde ouvert. Remplace l'ancien test « carte d'arene ».
    bool ControleParUnJoueur(Unit const* u)
    {
        return u && u->GetCharmerOrOwnerPlayerOrPlayerItself();
    }

    // Les crochets d'equipement ne couvrent pas tous les retraits (piece portee detruite) : on marque, et on recalcule
    // au premier coup suivant.
    void MarquerSignatures(Player* joueur)
    {
        joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES)->sale = true;
    }

    SignaturesActives* Actives(Player* joueur)
    {
        SignaturesActives* a = joueur->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
        if (a->sale)
            RecalculerSignatures(joueur);
        return a;
    }

    // Egide (et tout effet « absorbe_sous_seuil ») : le coup qui ferait passer sous le seuil est absorbe a hauteur de la
    // valeur (au niveau 80, x niveau / 80 ; valeur JcJ si l'attaquant est controle par un joueur), le reste devient un
    // bouclier (sort d'effet). Recharge partagee par les sursauts de survie (groupe_sursaut).
    void DeclencherSignatures(Player* joueur, Unit* attaquant, uint32& degats)
    {
        if (Signatures.empty() || !joueur->IsAlive())
            return;
        SignaturesActives* a = Actives(joueur);
        if (a->actives.empty() || NeutraliseEnArene1c1(joueur))
            return;
        Milliseconds const maintenant = GameTime::GetGameTimeMS();
        bool const jcj = ControleParUnJoueur(attaquant);
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
            uint32 const valeur = uint32(uint64(jcj ? s->valeurArene : s->valeur) * joueur->GetLevel() / DEFAULT_MAX_LEVEL);
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
            ++a->egides;
            LOG_INFO("module", "coa artisanat: signature declenchee joueur={} enchant={} forme={} rang={} valeur={} jcj={} absorbe={} reste={}",
                joueur->GetGUID().GetCounter(), s->enchant, s->forme, uint32(s->rang), valeur, jcj ? 1 : 0, absorbe, valeur - absorbe);
            return;   // un seul sursaut par coup
        }
    }

    // Coeur de forge (T1-5) : chaque critique (compte par les crochets de ArtisanatPlayer et ArtisanatSorts) charge la
    // facette ; a SEUIL_CHARGES_COEUR_DE_FORGE, l'aura (sort_effet, duree_ms) est posee et la recharge (recharge_ms)
    // commence ; pendant la recharge, rien ne se compte. Facette non active (hors des 3 places, piece cassee), arene 1c1
    // contre un gladiateur : rien. Les charges se voient dans l'aura SORT_AURA_CHARGES_COEUR_DE_FORGE (une pile par charge).
    void ChargerCoeurDeForge(Player* joueur)
    {
        if (!AuraCoeurDeForge || !joueur->IsAlive())
            return;
        SignaturesActives* a = Actives(joueur);
        auto it = std::find_if(a->actives.begin(), a->actives.end(), [](SignatureDef const* s)
        {
            return s->effet == CoaArtisanatIds::EFFET_COEUR_DE_FORGE;
        });
        if (it == a->actives.end() || NeutraliseEnArene1c1(joueur))
            return;
        SignatureDef const* s = *it;
        Milliseconds const maintenant = GameTime::GetGameTimeMS();
        Milliseconds& recharge = a->recharges[s->enchant];
        if (maintenant < recharge)
            return;
        uint32 const piles = CoaArtisanatIds::SORT_AURA_CHARGES_COEUR_DE_FORGE;
        if (++a->chargesCoeur < CoaArtisanatIds::SEUIL_CHARGES_COEUR_DE_FORGE)
        {
            if (piles)
            {
                Aura* aura = joueur->GetAura(piles, joueur->GetGUID());
                if (!aura)
                    aura = joueur->AddAura(piles, joueur);
                if (aura)
                    aura->SetStackAmount(uint8(std::min<uint32>(a->chargesCoeur, 255)));
            }
            return;
        }
        a->chargesCoeur = 0;
        if (piles)
            joueur->RemoveAurasDueToSpell(piles);
        recharge = maintenant + Milliseconds(s->rechargeMs);
        joueur->CastSpell(joueur, s->sortEffet, true);
        if (s->dureeMs)
            if (Aura* aura = joueur->GetAura(s->sortEffet, joueur->GetGUID()))
            {
                aura->SetMaxDuration(int32(s->dureeMs));
                aura->SetDuration(int32(s->dureeMs));
            }
        ++DeclenchementsCoeur;
        ++a->coeurs;
        LOG_INFO("module", "coa artisanat: coeur de forge declenche joueur={} enchant={} aura={} duree_ms={} recharge_ms={} valeur={} valeur_jcj={}",
            joueur->GetGUID().GetCounter(), s->enchant, s->sortEffet, s->dureeMs, s->rechargeMs, s->valeur, s->valeurArene);
    }

    // Valeur JcJ d'un SOIN (PO, 15/10 : un soin sur un allie en JcE garde la valeur pleine, donjons et raids compris) :
    // en champ de bataille ou en arene, ou si le soigne est en combat JcJ. Indicateur retenu : CombatManager::HasPvPCombat()
    // du soigne (Combat/CombatManager.cpp:195-201) : une reference de combat JcJ n'existe qu'entre deux unites controlees
    // par un joueur (CombatManager.cpp:235-236, bots compris), elle expire d'elle-meme apres le dernier echange
    // (PvPCombatReference::Update) ; le drapeau JcJ seul dirait « attaquable », pas « en combat contre un joueur ».
    bool SoinEnJcJ(Unit const* soigneur, Unit const* soigne)
    {
        Map const* carte = soigneur->FindMap();
        if (carte && carte->IsBattlegroundOrArena())
            return true;
        // Un allie PNJ (escorte, convoi) n'a que des references JcE : on lit aussi son combat contre des joueurs.
        return soigne && (soigne->GetCombatManager().HasPvPCombat() || soigne->GetCombatManager().HasPvECombatWithPlayers());
    }

    // Coeur de forge pour un coup ou un soin de `source` sur `cible`, en dix-milliemes : le multiplicateur qui ramene ce que
    // l'aura du DBC a deja applique (ses effets MOD_DAMAGE_PERCENT_DONE / MOD_HEALING_DONE_PERCENT, lus sur l'aura portee)
    // a la valeur voulue : valeur JcJ (valeur_arene) pour un coup sur une cible controlee par un joueur ou pour un soin en JcJ
    // (SoinEnJcJ), 0 en arene 1c1 contre un gladiateur, sinon la valeur. 10000 : rien a corriger. Une aura sans effet de jeu (DUMMY) donne la valeur entiere.
    // Fils de carte : ne lit que source et cible.
    uint32 CorrectionCoeurDeForge(Unit const* source, Unit const* cible, bool soin)
    {
        if (!AuraCoeurDeForge || !source || !source->IsPlayer())
            return 10000;
        Aura const* aura = source->GetAura(AuraCoeurDeForge);
        if (!aura)
            return 10000;
        int32 deja = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* e = aura->GetEffect(i))
                if (e->GetAuraType() == (soin ? SPELL_AURA_MOD_HEALING_DONE_PERCENT : SPELL_AURA_MOD_DAMAGE_PERCENT_DONE))
                    deja += e->GetAmount();
        int32 voulu = 0;
        if (!NeutraliseEnArene1c1(source->ToPlayer()))
            for (auto const& [enchant, s] : Signatures)
                if (s.effet == CoaArtisanatIds::EFFET_COEUR_DE_FORGE && s.sortEffet == AuraCoeurDeForge)
                {
                    voulu = int32((soin ? SoinEnJcJ(source, cible) : ControleParUnJoueur(cible)) ? s.valeurArene : s.valeur);
                    break;
                }
        if (voulu == deja || 100 + deja <= 0)
            return 10000;
        return uint32(std::max<int64>(0, int64(100 + voulu) * 10000 / (100 + deja)));
    }

    template <typename T>
    void AppliquerCorrection(T& montant, uint32 dixMilliemes)
    {
        if (dixMilliemes != 10000 && montant > 0)
            montant = T(uint64(montant) * dixMilliemes / 10000);
    }

    // ---- Banc de garde G1 (T2-1, docs/BANC-artisanat-G1-garde.md) : compteurs par bot sur une fenetre de mesure ----------
    //
    // Une mesure par bot : degats infliges (le bot ou ce qu'il controle), soins donnes (effectifs), degats subis (apres
    // absorptions, Egide comprise), morts. L'adversaire du banc (ADVERSAIRE_GARDE, marque) voit ses coups de melee multiplies
    // par `pression` %. Ecrit par les crochets d'unite (fils de carte), lu et remis a zero par la commande (fil monde) : verrou.
    // Rien n'est compte tant qu'aucune mesure n'est ouverte (GardesOuvertes, lu sans verrou).
    constexpr uint32 ADVERSAIRE_GARDE = 30888;   // « QA Test Dummy 80 Hostile Low Damage » [M creature_template : 80, normal, hostile]
    std::string const CLE_ADVERSAIRE_GARDE = "CoaArtisanatAdversaireGarde";
    struct AdversaireGarde final : public DataMap::Base
    {
        uint32 pression = 100;
    };
    struct MesureGarde
    {
        ObjectGuid adversaire;
        Milliseconds debut{ 0 }, fin{ 0 };
        bool ouverte = false;
        uint64 degats = 0, soins = 0, soinsBruts = 0, subis = 0;
        uint32 morts = 0, egides0 = 0, coeurs0 = 0, pression = 100;
        std::vector<ObjectGuid> pieces;
        std::vector<std::pair<uint8, ObjectGuid>> origine;   // (emplacement, piece d'origine renvoyee en sac par equiper)
    };
    std::mutex VerrouGarde;
    std::unordered_map<uint32, MesureGarde> Gardes;   // par GUID bas du bot
    std::atomic<uint32> GardesOuvertes{ 0 };
    // Fin la plus tardive des mesures ouvertes (ms de jeu) : au-dela, les crochets sortent sans verrou, meme si un bot
    // deconnecte a laisse sa mesure ouverte (GardesOuvertes ne redescend alors qu'a « lire » ou « rendre »).
    std::atomic<int64> FinGardes{ 0 };

    bool GardeEnCours()
    {
        return GardesOuvertes.load(std::memory_order_relaxed)
            && GameTime::GetGameTimeMS().count() < FinGardes.load(std::memory_order_relaxed);
    }

    // Le joueur (bot) a qui revient l'action d'une unite, s'il a une mesure ouverte et non echue.
    template <typename Fn>
    void CompterGarde(Unit const* u, Fn&& fn)
    {
        Player const* p = u ? u->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (!p)
            return;
        Milliseconds const maintenant = GameTime::GetGameTimeMS();
        std::lock_guard<std::mutex> verrou(VerrouGarde);
        auto it = Gardes.find(p->GetGUID().GetCounter());
        if (it != Gardes.end() && it->second.ouverte && maintenant < it->second.fin)
            fn(it->second);
    }

    // Signatures a declencher (Egide : sursaut de survie) sur les degats subis par un joueur ; bonus de Coeur de forge.
    class ArtisanatUnit final : public UnitScript
    {
    public:
        // DealDamage n'a pas d'interrupteur de crochet : ScriptMgr::DealDamage appelle tous les UnitScript (UnitScript.cpp:69).
        ArtisanatUnit() : UnitScript("CoaArtisanatUnit", true, { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
            UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL,
            UNITHOOK_ON_UNIT_DEATH }) { }

        // Banc G1 : apres DealDamage (Egide comprise) et l'AI de la victime, avant la soustraction (Unit.cpp:991-1006).
        void OnDamage(Unit* attaquant, Unit* victime, uint32& degats) override
        {
            if (!GardeEnCours() || !degats || attaquant == victime)
                return;
            // Seuls les coups echanges avec l'adversaire du banc comptent (pas un monstre de passage, pas l'adversaire d'un
            // autre bot).
            if (victime)
                CompterGarde(attaquant, [&](MesureGarde& m) { if (victime->GetGUID() == m.adversaire) m.degats += degats; });
            if (victime && victime->IsPlayer() && attaquant)
                CompterGarde(victime, [&](MesureGarde& m) { if (attaquant->GetGUID() == m.adversaire) m.subis += degats; });
        }

        // Banc G1 : soin effectif, apres ModifyHealth (Unit.cpp:8469-8472).
        void OnHeal(Unit* soigneur, Unit* /*soigne*/, uint32& gain) override
        {
            if (GardeEnCours() && gain)
                CompterGarde(soigneur, [&](MesureGarde& m) { m.soins += gain; });
        }

        void OnUnitDeath(Unit* mort, Unit* /*tueur*/) override
        {
            if (GardeEnCours() && mort && mort->IsPlayer())
                CompterGarde(mort, [&](MesureGarde& m) { ++m.morts; });
        }

        // UnitScript::DealDamage (appele pour tous les UnitScript, en tete de Unit::DealDamage, avant la soustraction des
        // PV) recoit le type de degats, contrairement a OnDamage : on exclut la mort instantanee (NODAMAGE, sorts
        // d'execution a GetHealth()), les couts en PV et les chutes (SELF_DAMAGE, attaquant = victime).
        uint32 DealDamage(Unit* attaquant, Unit* victime, uint32 degats, DamageEffectType type) override
        {
            if (degats && attaquant && victime && attaquant != victime && victime->IsPlayer() && type != NODAMAGE
                && type != SELF_DAMAGE && type != HEAL)
                DeclencherSignatures(victime->ToPlayer(), attaquant, degats);
            return degats;
        }

        void ModifyMeleeDamage(Unit* cible, Unit* attaquant, uint32& degats) override
        {
            // Banc G1 : pression de l'adversaire marque (ses coups de melee x pression %).
            if (GardeEnCours() && attaquant && attaquant->GetEntry() == ADVERSAIRE_GARDE
                && attaquant->IsCreature())
                if (AdversaireGarde const* a = attaquant->CustomData.Get<AdversaireGarde>(CLE_ADVERSAIRE_GARDE))
                    degats = uint32(uint64(degats) * a->pression / 100);
            AppliquerCorrection(degats, CorrectionCoeurDeForge(attaquant, cible, false));
        }

        void ModifySpellDamageTaken(Unit* cible, Unit* attaquant, int32& degats, SpellInfo const* /*sort*/) override
        {
            AppliquerCorrection(degats, CorrectionCoeurDeForge(attaquant, cible, false));
        }

        // Le tic d'un soin sur la duree passe AUSSI par ce crochet, juste avant ModifyHealReceived et dans le meme fil
        // (SpellAuraEffects.cpp:6827-6828, arguments (soigne, soigneur)) : on ne le corrige pas ici, on le signale.
        void ModifyPeriodicDamageAurasTick(Unit* cible, Unit* attaquant, uint32& degats, SpellInfo const* sort) override
        {
            TicDeSoin = 0;
            if (EstTicDeSoin(sort))
            {
                TicDeSoin = sort->Id;   // valable pour l'appel ModifyHealReceived suivant, du meme sort seulement
                return;
            }
            AppliquerCorrection(degats, CorrectionCoeurDeForge(attaquant, cible, false));
        }

        // Deux appelants, deux ordres : Unit::HealBySpell (Unit.cpp:8761) passe (soigneur, soigne) ; le tic de soin
        // (SpellAuraEffects.cpp:6828) passe (soigne, soigneur), annonce par TicDeSoinEnCours.
        void ModifyHealReceived(Unit* a, Unit* b, uint32& soin, SpellInfo const* sort) override
        {
            bool const tic = TicDeSoin && sort && sort->Id == TicDeSoin;
            TicDeSoin = 0;
            // OBS_MOD_HEALTH : le coeur n'applique pas les bonus du soigneur a ce tic (SpellAuraEffects.cpp:6809-6812) ;
            // l'aura du DBC n'a rien ajoute, on ne corrige rien.
            if (tic && !sort->HasAura(SPELL_AURA_PERIODIC_HEAL))
                return;
            Unit* soigneur = tic ? b : a;
            Unit* soigne = tic ? a : b;
            AppliquerCorrection(soin, CorrectionCoeurDeForge(soigneur, soigne, true));
            // Banc G1 : soin brut (surplus compris), qui mesure la puissance de soin quand le soigneur se soigne lui-meme.
            if (GardeEnCours() && soin)
                CompterGarde(soigneur, [&](MesureGarde& m) { m.soinsBruts += soin; });
        }

    private:
        static inline thread_local uint32 TicDeSoin = 0;

        // Les deux auras que HandlePeriodicHealAurasTick traite (SpellAuraEffects.cpp:1268-1270). Un sort qui porte aussi une
        // aura de degats periodiques ne se distingue pas par son SpellInfo : traite comme des degats (19 sorts, ecart accepte).
        static bool EstTicDeSoin(SpellInfo const* sort)
        {
            return sort && (sort->HasAura(SPELL_AURA_PERIODIC_HEAL) || sort->HasAura(SPELL_AURA_OBS_MOD_HEALTH))
                && !sort->HasAura(SPELL_AURA_PERIODIC_DAMAGE) && !sort->HasAura(SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
                && !sort->HasAura(SPELL_AURA_PERIODIC_LEECH);
        }
    };

    // Un sort dont le coeur fait passer le resultat par Player::CastItemCombatSpell (Spell.cpp:2929-2931, :2952-2954) : ses
    // critiques sont comptes par le crochet d'objet de combat, pas ici (sinon deux charges pour un critique).
    bool PasseParObjetDeCombat(SpellInfo const* info)
    {
        return (info->DmgClass == SPELL_DAMAGE_CLASS_MELEE || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
            && !info->HasAttribute(SPELL_ATTR0_CANCELS_AUTO_ATTACK_COMBAT) && !info->HasAttribute(SPELL_ATTR4_SUPPRESS_WEAPON_PROCS);
    }

    // Critiques des sorts (degats ou soins directs, une fois par cible touchee) pour Coeur de forge. Les tics periodiques ne
    // passent pas par ce crochet : ils ne chargent pas (accepte en T1).
    class ArtisanatSorts final : public AllSpellScript
    {
    public:
        ArtisanatSorts() : AllSpellScript("CoaArtisanatSorts", { ALLSPELLHOOK_ON_HIT_RESULT }) { }

        void OnSpellHitResult(Spell* sort, Unit* /*cible*/, uint8 rate, uint32 degats, uint32 soins, bool critique) override
        {
            if (!critique || !AuraCoeurDeForge || rate != SPELL_MISS_NONE || (!degats && !soins) || !sort)
                return;
            Player* joueur = sort->GetCaster() ? sort->GetCaster()->ToPlayer() : nullptr;
            SpellInfo const* info = sort->GetSpellInfo();
            if (!joueur || !info || info->Id == AuraCoeurDeForge || (degats && PasseParObjetDeCombat(info)))
                return;
            ChargerCoeurDeForge(joueur);
        }
    };

    // Oeuvre legendaire (T1-5) : fabricant = l'artisan (pose par le coeur), facette en 8, recolteur du Coeur de foudre
    // effectivement consomme en 9. Journal « coa sourcier: oeuvre ... ».
    void FabriquerOeuvre(Player* joueur, Item* objet, RecopieEnAttente const& attente)
    {
        ObjectGuid recolteur;
        uint32 consommes = 0;
        for (auto const& [coeur, createur] : attente.materiauxLegendaires)
            if (!joueur->GetItemByGuid(coeur))
            {
                ++consommes;
                if (!recolteur)
                    recolteur = createur;
            }
        uint32 const facette = PoserFacette(joueur, objet, attente.sort);
        bool const extrait = PoserExtrait(joueur, objet, recolteur.GetCounter());
        LOG_INFO("module", "coa sourcier: oeuvre fabriquee joueur={} piece={} ({}) recette={} facette={} coeurs_consommes={} recolteur={} extrait={}",
            joueur->GetGUID().GetCounter(), objet->GetGUID().GetCounter(), objet->GetEntry(), attente.sort, facette, consommes,
            recolteur.GetCounter(), extrait ? "pose" : (recolteur ? "NON POSE (ENCHANT_EXTRAIT)" : "aucun recolteur"));
    }

    // « EXTRAIT?<sac>:<case> » -> « EXTRAIT:<sac>:<case>:<nom> » ; <nom> vide : pas d'emplacement 9 (l'Atelier n'ajoute
    // aucune ligne) ; « un Sourcier disparu » : le personnage n'existe plus. Position comme EVEIL (PositionServeur).
    void TraiterExtrait(Player* joueur, std::string_view corps)
    {
        std::size_t const sep = corps.find(':');
        int32 sac = 0, casse = 0;
        uint8 bag = 0, slot = 0;
        if (sep == std::string_view::npos || !LireEntier(corps.substr(0, sep), sac) || !LireEntier(corps.substr(sep + 1), casse))
            return Repondre(joueur, "EXTRAIT_ERR:FORMAT");
        Item* objet = PositionServeur(joueur, sac, casse, bag, slot) ? joueur->GetItemByPos(bag, slot) : nullptr;
        if (!objet)
            return Repondre(joueur, Acore::StringFormat("EXTRAIT_ERR:PIECE:{}:{}", sac, casse));
        std::string nom;
        if (CoaArtisanatIds::ENCHANT_EXTRAIT && objet->GetEnchantmentId(EMPLACEMENT_EXTRAIT) == CoaArtisanatIds::ENCHANT_EXTRAIT)
            if (uint32 const bas = objet->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT))
                if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(bas), nom))
                    nom = "un Sourcier disparu";
        Repondre(joueur, Acore::StringFormat("EXTRAIT:{}:{}:{}", sac, casse, nom));
    }

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
            PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_CAN_CAST_ITEM_COMBAT_SPELL }) { }

        // Coeur de forge : coups blancs et sorts de melee ou de distance (Player::CastItemCombatSpell, appele pour CHAQUE
        // objet porte) ; on ne compte que pour la piece qui porte la facette, une fois par coup. Ne refuse jamais rien.
        bool OnPlayerCanCastItemCombatSpell(Player* joueur, Unit* /*cible*/, WeaponAttackType /*type*/, uint32 /*procVictime*/,
            uint32 resultat, Item* objet, ItemTemplate const* /*proto*/) override
        {
            if (AuraCoeurDeForge && (resultat & PROC_HIT_CRITICAL) && objet)
                if (auto it = Signatures.find(objet->GetEnchantmentId(EMPLACEMENT_FACETTE));
                    it != Signatures.end() && it->second.effet == CoaArtisanatIds::EFFET_COEUR_DE_FORGE)
                    ChargerCoeurDeForge(joueur);
            return true;
        }

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

            // Oeuvre : le materiau legendaire est un reactif ; on note chaque exemplaire tenu et son recolteur.
            if (EstOeuvre(creee) && CoaArtisanatIds::OBJET_COEUR_DE_FOUDRE)
                for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && uint32(info->Reagent[i]) == CoaArtisanatIds::OBJET_COEUR_DE_FOUDRE && info->ReagentCount[i])
                    {
                        PourChaquePieceTenue(joueur, CoaArtisanatIds::OBJET_COEUR_DE_FOUDRE, [&](Item* coeur)
                        {
                            attente->materiauxLegendaires.emplace_back(coeur->GetGUID(), coeur->GetGuidValue(ITEM_FIELD_CREATOR));
                        });
                        break;
                    }

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
            // Fabrication (aucune piece consommee) : la signature du specialiste (lot T) ; une Oeuvre n'en porte pas a part,
            // elle porte sa facette (emplacement 8) et le recolteur du materiau legendaire consomme (emplacement 9).
            if (!attente->objetDeLancement && attente->candidats.empty())
            {
                if (EstOeuvre(objet->GetEntry()))
                    FabriquerOeuvre(joueur, objet, *attente);
                else
                    SignerALaFabrication(joueur, objet, attente->sort);
            }
            attente->Vider();
        }

        void OnPlayerLevelChanged(Player* joueur, uint8 ancien) override
        {
            if (!RecalculSuspendu)
                RecalculerBonusDeSet(joueur);
            // Jalons (D-PO-42, b3) : le compteur de l'atelier bouge tout de suite. Jamais pour un bot : Etat() lance deux
            // requetes synchrones a son premier appel.
            if (!Talents.empty() && std::max(ancien, joueur->GetLevel()) >= 10 && joueur->GetSession() && !joueur->GetSession()->IsBot())
                EnvoyerEtat(joueur);
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
            bool const extrait = corps.starts_with("EXTRAIT?");
            if (!eveil && !talent && !extrait && !corps.starts_with("AMELIORER:"))
                return true;
            // Consomme meme desactive ; jamais relaye a un autre joueur.
            if (destinataire != joueur || message.size() > 255)
                return false;
            Milliseconds const maintenant = GameTime::GetGameTimeMS();
            // EXTRAIT? (infobulle, lecture seule) a son propre delai, court : il ne doit ni bloquer ni etre bloque par
            // les commandes de l'atelier. Au-dela : EXTRAIT_ERR:DELAI, une fois par fenetre.
            if (extrait)
            {
                RafaleExtrait* d = joueur->CustomData.GetDefault<RafaleExtrait>(CLE_DELAI_EXTRAIT);
                if (maintenant >= d->debut + FENETRE_EXTRAIT)
                {
                    d->debut = maintenant;
                    d->nombre = 0;
                }
                // Toujours une reponse : au-dela du garde-fou, EXTRAIT_ERR:DELAI (l'Atelier redemandera).
                if (++d->nombre > EXTRAIT_PAR_FENETRE)
                    Repondre(joueur, "EXTRAIT_ERR:DELAI");
                else
                    TraiterExtrait(joueur, corps.substr(8));
                return false;
            }
            // Un message d'addon ne passe pas par UpdateSpeakTime : un delai minimal par joueur.
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
                { "extrait", HandleBancExtrait, SEC_ADMINISTRATOR, Console::Yes },
                { "palier", HandleBancPalier, SEC_ADMINISTRATOR, Console::Yes },
                { "garde", HandleBancGarde, SEC_ADMINISTRATOR, Console::Yes },
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

        // Actions communes : etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | oublier (points gardes) |
        // effacer (tout, points compris). Chaque changement renvoie l'etat a l'atelier.
        static bool ActionTalent(ChatHandler* handler, Player* j, std::string const& action, Optional<std::string> const& a,
            Optional<std::string> const& b)
        {
            EtatTalents* e = Etat(j);
            auto etat = [&]
            {
                handler->PSendSysMessage("talent: metiers C={} R={} ; jalons C={} R={} ; gagnes C={} R={} ; disponibles C={} R={} ; plafonds C={} R={} (niveau {}) ; {} sorts connus",
                    e->metierC, e->metierR, Jalons(j->GetLevel(), false), Jalons(j->GetLevel(), true), e->gagnesC, e->gagnesR,
                    Disponibles(j, *e, false), Disponibles(j, *e, true),
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
                EnvoyerEtat(j);
                etat();
                return true;
            }
            if (action == "metier" && a && b && LireEntier(*b, n) && n > 0 && a->size() == 1)
            {
                std::string const r = ChoisirMetier(j, (*a)[0], uint32(n));
                handler->PSendSysMessage("talent: metier {} {} -> {}", *a, n, r.empty() ? "OK" : r);
                EnvoyerEtat(j);
                return true;
            }
            if (action == "apprendre" && a && LireEntier(*a, n) && n > 0)
            {
                std::string const r = ApprendreTalent(j, uint32(n));
                handler->PSendSysMessage("talent: apprendre {} -> {} ; disponibles C={} R={}", n, r.empty() ? "OK" : r,
                    Disponibles(j, *e, false), Disponibles(j, *e, true));
                EnvoyerEtat(j);
                return true;
            }
            // MJ, lot T1 (spec § 1 : talents legendaires appris par commande de MJ) : inscrit un talent SANS controle (metier,
            // etage, plafond), dans la table comme un talent appris, pour que SynchroniserMarqueurs ne le retire pas a la
            // connexion suivante (ce que ferait un simple .learn d'un sort de coa_artisanat_talent_def).
            if (action == "forcer" && a && LireEntier(*a, n) && n > 0)
            {
                if (!Talents.count(uint32(n)))
                {
                    handler->PSendSysMessage("talent: forcer {} -> INCONNU (absent de coa_artisanat_talent_def)", n);
                    return true;
                }
                e->sorts.insert(uint32(n));
                if (!j->HasSpell(uint32(n)))
                    j->learnSpell(uint32(n));
                if (TablesPersonnages)
                    CharacterDatabase.Execute("INSERT IGNORE INTO coa_artisanat_talent (guid, sort) VALUES ({}, {})", j->GetGUID().GetCounter(), n);
                LOG_INFO("module", "coa artisanat: talent force (MJ) joueur={} sort={}", j->GetGUID().GetCounter(), n);
                handler->PSendSysMessage("talent: forcer {} -> OK (sans controle)", n);
                EnvoyerEtat(j);
                return true;
            }
            if (action == "oublier" || action == "effacer")
            {
                OublierTalents(j, action == "effacer");
                EnvoyerEtat(j);
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
                handler->SendSysMessage("talent etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | forcer <sort> | oublier | effacer");
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
            // Les recettes de Forge exigent le marteau (TotemCategory : SPELL_FAILED_TOTEM_CATEGORY = 130 sans lui).
            Completer(j, 5956, 1);
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

            handler->SendSysMessage("banc talent <bot> etat | points <C|R> <n> | metier <C|R> <skill> | apprendre <sort> | forcer <sort> | oublier | effacer | signature | egide");
            return true;
        }

        // ---- .artisanat banc garde <bot> ... : banc de garde G1 (T2-1, docs/BANC-artisanat-G1-garde.md) --------------------
        //
        // equiper <entree,...>     : cree et porte ces pieces (rang 5 : niveau d'objet egal avec et sans) ; les pieces d'une
        //                            mesure precedente sont detruites d'abord ;
        // signer <enchant,...|aucun> : vide les emplacements 7 et 8 des pieces du banc, puis pose chaque enchantement : une
        //                            facette (effet coeur_de_forge) en 8 sur l'Oeuvre portee, une signature en 7 sur la piece
        //                            suivante ; rend les signatures ACTIVES (moteur : 3 au plus, meme nature = une) ;
        // lancer <s> [pression %]  : vie et mana pleines, invoque l'adversaire ADVERSAIRE_GARDE (PV portes a 1e9, coups x
        //                            pression %), qui attaque le bot ; ouvre la mesure pour <s> secondes (10-1800) ;
        // lire                     : la ligne de mesure (« coa garde: ... ») ; ferme la mesure et retire l'adversaire si echue ;
        // rendre                   : ferme, retire l'adversaire, detruit les pieces du banc.
        // PTR et bots seulement (Cible). Les compteurs ne voient que le bot et ce qu'il controle.
        static bool HandleBancGarde(ChatHandler* handler, std::string nom, std::string action, Optional<std::string> a,
            Optional<std::string> b)
        {
            if (!BancPermis(handler))
                return true;
            // lire et rendre doivent passer en combat ou mort (fin de mesure) : un bot en ligne suffit.
            Player* j = nullptr;
            if (action == "lire" || action == "rendre")
            {
                j = ObjectAccessor::FindPlayerByName(nom, true);
                if (!j || !j->GetSession() || !j->GetSession()->IsBot())
                    return handler->PSendSysMessage("banc garde: {} n'est pas un bot en ligne.", nom), true;
            }
            else
                j = Cible(handler, nom);
            if (!j)
                return true;
            uint32 const cle = j->GetGUID().GetCounter();
            auto retirerAdversaire = [&](MesureGarde& m)
            {
                if (Creature* c = m.adversaire ? ObjectAccessor::GetCreature(*j, m.adversaire) : nullptr)
                    c->DespawnOrUnsummon();
                m.adversaire.Clear();
            };
            auto fermer = [&](MesureGarde& m)
            {
                if (m.ouverte)
                {
                    m.ouverte = false;
                    --GardesOuvertes;
                }
            };
            auto lireListe = [](std::string const& texte, std::vector<uint32>& ids) -> bool
            {
                ids.clear();
                for (std::string_view jeton : Acore::Tokenize(texte, ',', false))
                {
                    Optional<uint32> v = Acore::StringTo<uint32>(jeton);
                    if (!v || !*v)
                        return false;
                    ids.push_back(*v);
                }
                return !ids.empty();
            };
            // Copie de la mesure sous verrou ; les objets du jeu se touchent hors verrou (fil monde, cartes a l'arret).
            auto copie = [&]() { std::lock_guard<std::mutex> v(VerrouGarde); return Gardes[cle]; };
            auto ecrire = [&](MesureGarde const& m) { std::lock_guard<std::mutex> v(VerrouGarde); Gardes[cle] = m; };

            if (action == "equiper")
            {
                std::vector<uint32> entrees;
                if (!a || !lireListe(*a, entrees))
                    return handler->SendSysMessage("banc garde: equiper <entree,entree,...>"), true;
                MesureGarde m = copie();
                if (m.ouverte)
                    return handler->SendSysMessage("banc garde: une mesure est ouverte (lire ou rendre d'abord)."), true;
                for (ObjectGuid g : m.pieces)
                    if (Item* o = j->GetItemByGuid(g))
                        j->DestroyItem(o->GetBagSlot(), o->GetSlot(), true);
                m.pieces.clear();
                std::string texte;
                m.origine.clear();
                for (uint32 entree : entrees)
                {
                    Item* o = CreerEnSac(j, entree);
                    uint16 dest = 0;
                    Item* avant = nullptr;
                    if (o && j->CanEquipItem(NULL_SLOT, dest, o, true) == EQUIP_ERR_OK)
                        avant = j->GetItemByPos(dest);
                    bool const porte = o && Porter(j, o);
                    if (porte && avant)
                        m.origine.emplace_back(uint8(dest & 255), avant->GetGUID());
                    if (o)
                        m.pieces.push_back(o->GetGUID());
                    texte += Acore::StringFormat(" {}:{}", entree, !o ? "SACS/INCONNU" : porte ? "porte" : "NON PORTE");
                }
                ecrire(m);
                MarquerSignatures(j);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc garde: {} piece(s) :{}", uint32(m.pieces.size()), texte);
                return true;
            }

            if (action == "signer")
            {
                if (!a)
                    return handler->SendSysMessage("banc garde: signer <enchant,...|aucun>"), true;
                MesureGarde const m = copie();
                std::vector<Item*> pieces;
                for (ObjectGuid g : m.pieces)
                    if (Item* o = j->GetItemByGuid(g); o && o->IsEquipped())
                        pieces.push_back(o);
                std::vector<uint32> ids;
                if (*a != "aucun" && !lireListe(*a, ids))
                    return handler->SendSysMessage("banc garde: signer <enchant,...|aucun>"), true;
                for (Item* o : pieces)
                {
                    PoserEnchantement(j, o, EMPLACEMENT_SIGNATURE, 0, 0);
                    PoserEnchantement(j, o, EMPLACEMENT_FACETTE, 0, 0);
                }
                std::string texte;
                std::size_t suivante = 0;
                for (uint32 id : ids)
                {
                    auto s = Signatures.find(id);
                    if (s == Signatures.end())
                    {
                        texte += Acore::StringFormat(" {}:INCONNU", id);
                        continue;
                    }
                    Item* cible = nullptr;
                    EnchantmentSlot emplacement = EMPLACEMENT_SIGNATURE;
                    if (s->second.effet == CoaArtisanatIds::EFFET_COEUR_DE_FORGE)
                    {
                        emplacement = EMPLACEMENT_FACETTE;
                        for (Item* o : pieces)
                            if (EstOeuvre(o->GetEntry()))
                                cible = o;
                    }
                    else
                        while (suivante < pieces.size() && !cible)
                        {
                            Item* o = pieces[suivante++];
                            if (!o->GetEnchantmentId(EMPLACEMENT_SIGNATURE))
                                cible = o;
                        }
                    if (!cible)
                    {
                        texte += Acore::StringFormat(" {}:SANS PIECE", id);
                        continue;
                    }
                    PoserEnchantement(j, cible, emplacement, id, 0);
                    texte += Acore::StringFormat(" {}:{}@{}", id, uint32(emplacement), cible->GetEntry());
                }
                RecalculerSignatures(j);
                SignaturesActives* sa = j->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
                std::string actives;
                for (SignatureDef const* d : sa->actives)
                    actives += Acore::StringFormat("{}{}", actives.empty() ? "" : ",", d->enchant);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc garde: pose{} ; actives={}", texte.empty() ? " (aucune)" : texte,
                    actives.empty() ? "aucune" : actives);
                return true;
            }

            if (action == "lancer")
            {
                Optional<uint32> secondes = a ? Acore::StringTo<uint32>(*a) : Optional<uint32>();
                Optional<uint32> pression = b ? Acore::StringTo<uint32>(*b) : Optional<uint32>(100);
                if (!secondes || *secondes < 10 || *secondes > 1800 || !pression || *pression < 10 || *pression > 5000)
                    return handler->SendSysMessage("banc garde: lancer <secondes 10-1800> [pression % 10-5000]"), true;
                MesureGarde m = copie();
                if (m.ouverte)
                    return handler->SendSysMessage("banc garde: une mesure est deja ouverte (lire ou rendre)."), true;
                retirerAdversaire(m);
                if (j->IsMounted())
                {
                    j->Dismount();
                    j->RemoveAurasByType(SPELL_AURA_MOUNTED);
                }
                // Rien ne passe d'un bras a l'autre : auras et recharges d'artisanat, recharges de classe, ressource.
                SignaturesActives* avant = j->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
                for (auto const& [id, def] : Signatures)
                    if (def.sortEffet)
                        j->RemoveAurasDueToSpell(def.sortEffet);
                if (CoaArtisanatIds::SORT_AURA_CHARGES_COEUR_DE_FORGE)
                    j->RemoveAurasDueToSpell(CoaArtisanatIds::SORT_AURA_CHARGES_COEUR_DE_FORGE);
                avant->sursaut = Milliseconds(0);
                avant->recharges.clear();
                avant->chargesCoeur = 0;
                j->RemoveAllSpellCooldown();
                j->SetFullHealth();
                Powers const ressource = j->getPowerType();
                j->SetPower(ressource, (ressource == POWER_RAGE || ressource == POWER_RUNIC_POWER) ? 0 : j->GetMaxPower(ressource));
                // L'adversaire part a la fin exacte de la fenetre : le bot sort de combat, « lire » le trouve parti.
                TempSummon* adv = j->SummonCreature(ADVERSAIRE_GARDE, j->GetPositionX() + 3.0f, j->GetPositionY(), j->GetPositionZ(), 0.0f,
                    TEMPSUMMON_TIMED_DESPAWN, *secondes * IN_MILLISECONDS);
                if (!adv)
                    return handler->PSendSysMessage("banc garde: adversaire {} non invoque.", ADVERSAIRE_GARDE), true;
                adv->SetMaxHealth(1000000000);
                adv->SetFullHealth();
                adv->CustomData.GetDefault<AdversaireGarde>(CLE_ADVERSAIRE_GARDE)->pression = *pression;
                if (adv->IsAIEnabled && adv->AI())
                    adv->AI()->AttackStart(j);
                SignaturesActives* sa = j->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
                Milliseconds const maintenant = GameTime::GetGameTimeMS();
                m.adversaire = adv->GetGUID();
                m.debut = maintenant;
                m.fin = maintenant + Seconds(*secondes);
                m.degats = m.soins = m.soinsBruts = m.subis = 0;
                m.morts = 0;
                m.egides0 = sa->egides;
                m.coeurs0 = sa->coeurs;
                m.pression = *pression;
                m.ouverte = true;
                ecrire(m);
                ++GardesOuvertes;
                int64 fin = FinGardes.load();
                while (fin < m.fin.count() && !FinGardes.compare_exchange_weak(fin, m.fin.count())) { }
                handler->PSendSysMessage("banc garde: mesure ouverte {} s, adversaire {} (guid {}), pression {} %", *secondes,
                    ADVERSAIRE_GARDE, adv->GetGUID().GetCounter(), *pression);
                return true;
            }

            if (action == "lire" || action == "rendre")
            {
                MesureGarde m = copie();
                Milliseconds const maintenant = GameTime::GetGameTimeMS();
                bool const echue = m.ouverte && maintenant >= m.fin;
                Milliseconds const finEffective = std::min(maintenant, m.fin);
                uint32 const dureeMs = m.debut.count() ? uint32(std::max<int64>(0, (finEffective - m.debut).count())) : 0;
                SignaturesActives* sa = j->CustomData.GetDefault<SignaturesActives>(CLE_SIGNATURES);
                if (sa->sale)
                    RecalculerSignatures(j);
                std::string actives;
                for (SignatureDef const* d : sa->actives)
                    actives += Acore::StringFormat("{}{}", actives.empty() ? "" : ",", d->enchant);
                double const s = dureeMs / 1000.0;
                // CustomData remis a zero (bot reconnecte, P-238) : compteurs de declenchements inferieurs au depart.
                bool const reinit = sa->egides < m.egides0 || sa->coeurs < m.coeurs0;
                Creature const* adv = m.adversaire ? ObjectAccessor::GetCreature(*j, m.adversaire) : nullptr;
                std::string const etatAdv = !adv ? "parti" : !adv->IsAlive() ? "mort"
                    : Acore::StringFormat("{}{}", adv->IsInCombat() ? "combat" : "hors_combat", adv->GetVictim() == j ? "_bot" : "_autre");
                std::string const ligne = Acore::StringFormat("coa garde: bot={} classe={} etat={} actives={} duree_ms={} degats={} soins={} "
                    "soins_bruts={} subis={} morts={} egides={} coeurs={} pression={} adversaire={} reinit={} dps={:.1f} hps={:.1f} "
                    "hps_brut={:.1f} dtps={:.1f}", cle, uint32(j->getClass()), m.ouverte ? (echue ? "echue" : "en_cours") : "fermee",
                    actives.empty() ? "aucune" : actives, dureeMs, m.degats, m.soins, m.soinsBruts, m.subis, m.morts,
                    reinit ? 0 : sa->egides - m.egides0, reinit ? 0 : sa->coeurs - m.coeurs0, m.pression, etatAdv, reinit ? 1 : 0,
                    s > 0 ? m.degats / s : 0.0, s > 0 ? m.soins / s : 0.0, s > 0 ? m.soinsBruts / s : 0.0, s > 0 ? m.subis / s : 0.0);
                if (echue || action == "rendre")
                {
                    fermer(m);
                    retirerAdversaire(m);
                    LOG_INFO("module", "{}", ligne);
                }
                if (action == "rendre")
                {
                    for (ObjectGuid g : m.pieces)
                        if (Item* o = j->GetItemByGuid(g))
                            j->DestroyItem(o->GetBagSlot(), o->GetSlot(), true);
                    m.pieces.clear();
                    // L'equipement d'origine reprend sa place (s'il est encore en sac et l'emplacement libre).
                    uint32 remises = 0;
                    for (auto const& [emplacement, g] : m.origine)
                        if (Item* o = j->GetItemByGuid(g); o && !o->IsEquipped() && !j->GetItemByPos(INVENTORY_SLOT_BAG_0, emplacement))
                            remises += Porter(j, o) ? 1 : 0;
                    m.origine.clear();
                    j->SaveToDB(false, false);
                    handler->PSendSysMessage("banc garde: pieces du banc detruites, {} piece(s) d'origine remise(s).", remises);
                }
                // Ecrire sous verrou en gardant les compteurs : les crochets ont pu compter depuis la copie.
                {
                    std::lock_guard<std::mutex> v(VerrouGarde);
                    MesureGarde& g = Gardes[cle];
                    g.ouverte = m.ouverte;
                    g.adversaire = m.adversaire;
                    g.pieces = m.pieces;
                    g.origine = m.origine;
                }
                handler->PSendSysMessage("{}", ligne);
                return true;
            }
            handler->SendSysMessage("banc garde <bot> equiper <entree,...> | signer <enchant,...|aucun> | lancer <s> [pression %] | lire | rendre");
            return true;
        }

        // ---- .artisanat banc palier <bot> <entree> [garder] : un palier sur une piece QUELCONQUE (D-PO-45, armes) -----------
        //
        // Cree la piece <entree> en sac (fabricant = le bot), y pose un enchantement permanent, un temporaire de 30 min, des
        // gemmes si elle a des chasses (aucune piece de palier n'en a au 15/10 : non eprouve) et, si ENCHANT_EXTRAIT est
        // utilisable, l'emplacement 9 (GUID du bot) ; les emplacements 7 et 8 restent vides ; puis la meme epreuve
        // que « recopie 2 » : refus sans reactifs, refus au niveau requis - 1, cout fourni a l'exact, prelevement et recopie
        // verifies (CONFORME = memes enchantements, charges des emplacements 7-9 comprises, meme fabricant). La piece neuve est
        // detruite a la fin, sauf « garder ». Il faut une etape « palier » en table pour <entree>, et un bot au niveau de
        // l'etape (le banc ne change pas son niveau, hors du refus a niveau - 1).
        static bool HandleBancPalier(ChatHandler* handler, std::string nom, uint32 entree, Optional<std::string> option)
        {
            if (!BancPermis(handler))
                return true;
            bool const garder = option && *option == "garder";
            if (option && !garder)
                return handler->PSendSysMessage("banc palier: option inconnue « {} » (seule : garder).", *option), true;
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entree);
            if (!EstPieceArtisanat(proto))
                return handler->PSendSysMessage("banc palier: {} n'est pas une piece de l'artisanat (armure ou arme de {}-{}).", entree,
                    ARTISANAT_OBJET_MIN, ARTISANAT_OBJET_MAX), true;
            auto it = Etapes.find(entree);
            if (it == Etapes.end() || it->second.eveil)
                return handler->PSendSysMessage("banc palier: aucune etape de palier en table pour {} ({}).", entree, proto->Name1), true;
            if (j->GetLevel() < it->second.niveau)
                return handler->PSendSysMessage("banc palier: {} est niveau {}, l'etape de {} demande {}.", nom, uint32(j->GetLevel()),
                    entree, uint32(it->second.niveau)), true;
            Item* piece = CreerEnSac(j, entree);
            if (!piece)
                return handler->SendSysMessage("banc palier: sacs pleins."), true;
            SignerPar(j, piece);
            Sertir(j, piece);
            PoserEnchantement(j, piece, PERM_ENCHANTMENT_SLOT, EnchantementDeObjet(PARCHEMIN), 0);
            PoserEnchantement(j, piece, TEMP_ENCHANTMENT_SLOT, EnchantementDeObjet(HUILE), 30 * MINUTE * IN_MILLISECONDS);
            if (EnchantExtraitUtilisable())
                PoserExtrait(j, piece, j->GetGUID().GetCounter());
            handler->PSendSysMessage("banc palier: {} « {} » classe {} sous-classe {} niveau d'objet {} : {}", entree, proto->Name1,
                proto->Class, proto->SubClass, proto->ItemLevel, Decrire(piece));
            ObjectGuid const guidDepart = piece->GetGUID();   // la piece de depart est detruite si le palier passe
            // Reactifs tenus avant : EtapeBanc les vide (refus) puis les fournit ; on remet le bot dans son etat a la fin.
            std::vector<std::pair<uint32, uint32>> tenus;
            for (auto const& [objet, nombre] : it->second.objets)
                if (objet)
                    tenus.emplace_back(objet, j->GetItemCount(objet, false));
            Item* neuve = EtapeBanc(handler, j, piece, SorteEtape::Palier, "banc palier", true);
            if (neuve)
            {
                handler->PSendSysMessage("banc palier: {} -> {} « {} » niveau d'objet {}", entree, neuve->GetEntry(),
                    neuve->GetTemplate()->Name1, neuve->GetTemplate()->ItemLevel);
                LOG_INFO("module", "coa artisanat: banc palier bot={} de={} vers={} piece={}", j->GetGUID().GetCounter(), entree,
                    neuve->GetEntry(), neuve->GetGUID().GetCounter());
                if (!garder)
                    j->DestroyItem(neuve->GetBagSlot(), neuve->GetSlot(), true);
            }
            else if (Item* reste = j->GetItemByGuid(guidDepart); reste && !garder)
                j->DestroyItem(reste->GetBagSlot(), reste->GetSlot(), true);
            uint32 ecarts = 0;
            for (auto const& [objet, avant] : tenus)
            {
                uint32 const maintenant = j->GetItemCount(objet, false);
                if (maintenant > avant)
                    j->DestroyItemCount(objet, maintenant - avant, true);
                else if (maintenant < avant)
                    ecarts += Completer(j, objet, avant);
            }
            if (ecarts)
                handler->PSendSysMessage("banc palier: sacs pleins, {} reactif(s) non rendu(s) au bot.", ecarts);
            j->SaveToDB(false, false);
            return true;
        }

        // ---- .artisanat banc extrait <bot> ... : banc de l'emplacement 9 (T1-1) ----------------------------------------
        //
        // poser <enchant> [recolteur] : cree un Heaume palier 10 en sac, pose <enchant> en 9 avec le GUID bas <recolteur>
        //                               (defaut : le bot) dans les charges, sauvegarde ;
        // lire                        : chaque piece tenue qui a un emplacement 9 : memoire et item_instance, comparees ;
        // porter                      : porte la premiere (pour le combat : le bot se bat seul ; relire ensuite) ;
        // recopie                     : palier suivant (AMELIORER, cout fourni) de la premiere ; recopie verifiee, charges
        //                               comprises ;
        // courrier <nom>              : envoie la premiere par courrier a <nom> ; relire avec « objet <guid> » ;
        // objet <guid>                : item_instance d'un objet, ou qu'il soit (courrier, autre personnage) ;
        // retirer                     : detruit les pieces de l'artisanat tenues qui ont un emplacement 9.
        // Aucune ecriture hors du bot (sauf courrier, au destinataire). Royaume PTR seulement.

        // Emplacement 9 lu dans item_instance.enchantments (12 x « id duree charges »). Faux si l'objet n'y est pas.
        static bool Emplacement9EnBase(uint32 guid, uint32& id, uint32& charges, uint32& proprietaire)
        {
            QueryResult r = CharacterDatabase.Query("SELECT enchantments, owner_guid FROM item_instance WHERE guid = {}", guid);
            if (!r)
                return false;
            std::vector<std::string_view> jetons = Acore::Tokenize(r->Fetch()[0].Get<std::string_view>(), ' ', false);
            proprietaire = r->Fetch()[1].Get<uint32>();
            std::size_t const base = std::size_t(EMPLACEMENT_EXTRAIT) * MAX_ENCHANTMENT_OFFSET;
            if (jetons.size() < base + 3)
                return id = charges = 0, true;
            id = Acore::StringTo<uint32>(jetons[base]).value_or(0);
            charges = Acore::StringTo<uint32>(jetons[base + 2]).value_or(0);
            return true;
        }

        static std::vector<Item*> PiecesAvecEmplacement9(Player* j)
        {
            std::vector<Item*> pieces;
            for (uint32 entree = ARTISANAT_OBJET_MIN; entree <= ARTISANAT_OBJET_MAX; ++entree)
                if (EstPieceArtisanat(sObjectMgr->GetItemTemplate(entree)))
                    PourChaquePieceTenue(j, entree, [&](Item* o) { if (o->GetEnchantmentId(EMPLACEMENT_EXTRAIT)) pieces.push_back(o); });
            return pieces;
        }

        static bool HandleBancExtrait(ChatHandler* handler, std::string nom, std::string action, Optional<std::string> a,
            Optional<std::string> b)
        {
            if (!BancPermis(handler))
                return true;
            if (action == "objet")
            {
                int32 g = 0;
                uint32 id = 0, charges = 0, proprietaire = 0;
                if (!a || !LireEntier(*a, g) || g <= 0)
                    return handler->SendSysMessage("banc extrait: objet <guid de l'objet>"), true;
                if (!Emplacement9EnBase(uint32(g), id, charges, proprietaire))
                    return handler->PSendSysMessage("banc extrait: objet {} absent de item_instance.", g), true;
                handler->PSendSysMessage("banc extrait: objet {} en base : proprietaire={} emplacement 9 = {} charges={}", g, proprietaire, id, charges);
                return true;
            }
            Player* j = Cible(handler, nom);
            if (!j)
                return true;
            int32 n = 0;

            if (action == "poser")
            {
                if (!a || !LireEntier(*a, n) || n <= 0)
                    return handler->SendSysMessage("banc extrait: poser <enchant> [guid bas du recolteur]"), true;
                uint32 const enchant = uint32(n);
                std::string const raison = RaisonEnchantementNonNeutre(enchant);
                // Un type COMBAT_SPELL decompterait les charges : refuse. Inconnu du DBC : accepte (rien ne l'applique), dit.
                if (!raison.empty() && raison != "inconnu du DBC serveur")
                    return handler->PSendSysMessage("banc extrait: enchantement {} refuse : {}.", enchant, raison), true;
                int32 r = int32(j->GetGUID().GetCounter());
                if (b && (!LireEntier(*b, r) || r <= 0))
                    return handler->SendSysMessage("banc extrait: recolteur = un guid bas positif."), true;
                Item* piece = CreerEnSac(j, HEAUME_PALIER10);
                if (!piece)
                    return handler->SendSysMessage("banc extrait: sacs pleins."), true;
                piece->SetEnchantment(EMPLACEMENT_EXTRAIT, enchant, 0, uint32(r), j->GetGUID());
                piece->SetState(ITEM_CHANGED, j);
                j->SaveToDB(false, false);
                uint32 id = 0, charges = 0, proprietaire = 0;
                bool const enBase = Emplacement9EnBase(piece->GetGUID().GetCounter(), id, charges, proprietaire);
                handler->PSendSysMessage("banc extrait: pose {} ; enchantement {}{} ; base (ecriture asynchrone, relire) : {}",
                    Decrire(piece), enchant, raison.empty() ? " neutre" : " (" + raison + ")",
                    enBase ? Acore::StringFormat("{} charges={}", id, charges) : std::string("pas encore ecrite"));
                LOG_INFO("module", "coa sourcier: banc emplacement 9 pose bot={} piece={} enchant={} charges={}",
                    j->GetGUID().GetCounter(), piece->GetGUID().GetCounter(), enchant, r);
                return true;
            }

            std::vector<Item*> pieces = PiecesAvecEmplacement9(j);
            if (action == "lire")
            {
                handler->PSendSysMessage("banc extrait: {} piece(s) tenue(s) avec un emplacement 9.", uint32(pieces.size()));
                for (Item* p : pieces)
                {
                    uint32 id = 0, charges = 0, proprietaire = 0;
                    bool const enBase = Emplacement9EnBase(p->GetGUID().GetCounter(), id, charges, proprietaire);
                    bool const egal = enBase && id == p->GetEnchantmentId(EMPLACEMENT_EXTRAIT)
                        && charges == p->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT);
                    handler->PSendSysMessage("banc extrait: {} {} ; memoire {} charges={} ; base {} ; {}", Decrire(p),
                        p->IsEquipped() ? "portee" : "en sac", p->GetEnchantmentId(EMPLACEMENT_EXTRAIT),
                        p->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT),
                        enBase ? Acore::StringFormat("{} charges={}", id, charges) : std::string("ABSENTE"),
                        egal ? "IDENTIQUES" : "ECART (sauvegarde en retard ? relire apres saveall)");
                }
                return true;
            }
            if (pieces.empty())
                return handler->SendSysMessage("banc extrait: aucune piece avec un emplacement 9 (poser d'abord)."), true;
            Item* premiere = pieces.front();

            if (action == "porter")
            {
                bool const ok = premiere->IsEquipped() || Porter(j, premiere);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc extrait: {} {}", Decrire(premiere), ok ? "portee" : "NE SE PORTE PAS");
                return true;
            }
            if (action == "recopie")
            {
                uint32 const charges = premiere->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT);
                uint32 const id = premiere->GetEnchantmentId(EMPLACEMENT_EXTRAIT);
                Item* neuve = EtapeBanc(handler, j, premiere, SorteEtape::Palier, "banc extrait: recopie", false);
                if (neuve)
                    handler->PSendSysMessage("banc extrait: piece neuve emplacement 9 = {} charges={} (avant {} charges={}) : {}",
                        neuve->GetEnchantmentId(EMPLACEMENT_EXTRAIT), neuve->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT), id, charges,
                        neuve->GetEnchantmentId(EMPLACEMENT_EXTRAIT) == id && neuve->GetEnchantmentCharges(EMPLACEMENT_EXTRAIT) == charges
                        ? "IDENTIQUES" : "ECART");
                j->SaveToDB(false, false);
                return true;
            }
            if (action == "courrier")
            {
                ObjectGuid const dest = a ? sCharacterCache->GetCharacterGuidByName(*a) : ObjectGuid::Empty;
                if (!dest || dest == j->GetGUID())
                    return handler->SendSysMessage("banc extrait: courrier <nom d'un autre personnage existant>"), true;
                if (premiere->IsEquipped())
                    return handler->SendSysMessage("banc extrait: la piece est portee ; la mettre en sac d'abord."), true;
                uint32 const guidPiece = premiere->GetGUID().GetCounter();
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                j->MoveItemFromInventory(premiere->GetBagSlot(), premiere->GetSlot(), true);
                premiere->DeleteFromInventoryDB(trans);
                premiere->SaveToDB(trans);
                MailDraft draft("Banc emplacement 9", "");
                draft.AddItem(premiere);
                draft.SendMailTo(trans, MailReceiver(ObjectAccessor::FindConnectedPlayer(dest), dest.GetCounter()), MailSender(j), MAIL_CHECK_MASK_COPIED);
                CharacterDatabase.CommitTransaction(trans);
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc extrait: piece {} envoyee a {} (guid {}) ; relire : .artisanat banc extrait - objet {}",
                    guidPiece, *a, dest.GetCounter(), guidPiece);
                return true;
            }
            if (action == "retirer")
            {
                uint32 detruites = 0;
                for (Item* p : pieces)
                {
                    j->DestroyItem(p->GetBagSlot(), p->GetSlot(), true);
                    ++detruites;
                }
                j->SaveToDB(false, false);
                handler->PSendSysMessage("banc extrait: {} piece(s) detruite(s).", detruites);
                return true;
            }
            handler->SendSysMessage("banc extrait <bot> poser <enchant> [recolteur] | lire | porter | recopie | courrier <nom> | retirer ; banc extrait - objet <guid>");
            return true;
        }

        // Les emplacements gardes : PERM, TEMP (id), SOCK1-3, prismatique, signature (PROP 0), bonus de chasse, fabricant.
        static bool Conforme(Chasses const& avant, uint32 bonusAvant, Item const* apres)
        {
            for (std::size_t i = 0; i < EMPLACEMENTS_RECOPIES.size(); ++i)
            {
                EnchantmentSlot const slot = EMPLACEMENTS_RECOPIES[i];
                if (avant.emplacements[i].id != apres->GetEnchantmentId(slot))
                    return false;
                // Signature, facette, recolteur : les charges aussi (le 9 y porte le GUID du recolteur).
                if (slot >= PROP_ENCHANTMENT_SLOT_0 && avant.emplacements[i].charges != apres->GetEnchantmentCharges(slot))
                    return false;
            }
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
    new ArtisanatSorts();
    new ArtisanatCommandes();
}
