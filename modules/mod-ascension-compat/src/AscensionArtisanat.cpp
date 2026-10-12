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
#include "Config.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <array>
#include <charconv>
#include <string_view>
#include <unordered_map>
#include <vector>

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
        auto it = VersionEveillee.find(piece->GetEntry());
        if (it == VersionEveillee.end())
            return refuser("PIECE", "Cette pièce ne peut pas être éveillée.");
        if (joueur->GetLevel() < NIVEAU_EVEIL)
            return refuser("NIVEAU", "Il faut être niveau 80 pour éveiller une pièce.");
        if (!joueur->IsAlive())
            return refuser("MORT", "Impossible d'éveiller une pièce en étant mort.");
        if (joueur->IsInCombat())
            return refuser("COMBAT", "Impossible d'éveiller une pièce en combat.");

        uint32 const nouvelle = it->second;
        ItemPosCountVec dest;
        InventoryResult const place = joueur->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, nouvelle, 1);
        if (place != EQUIP_ERR_OK)
        {
            joueur->SendEquipError(place, nullptr, nullptr, nouvelle);
            return refuser("SACS", {});
        }

        // Creer d'abord, detruire ensuite : un echec a la creation ne coute rien au joueur.
        Chasses const c = Photographier(joueur, piece);
        Item* neuve = joueur->StoreNewItem(dest, nouvelle, true);
        if (!neuve)
            return refuser("SACS", {});
        uint32 perdus = 0;
        uint32 const recopies = Recopier(joueur, neuve, c, perdus);
        joueur->SendNewItem(neuve, 1, true, false);
        uint32 const ancienne = piece->GetEntry();
        uint64 const ancienGuid = piece->GetGUID().GetCounter();
        joueur->DestroyItem(bag, slot, true);

        LOG_INFO("module", "coa artisanat: eveil par message joueur={} piece={} ({}) -> {} ({}) emplacements={} perdus={}",
            joueur->GetGUID().GetCounter(), ancienGuid, ancienne, neuve->GetGUID().GetCounter(), nouvelle, recopies, perdus);
        Repondre(joueur, "EVEIL_OK:" + std::to_string(nouvelle));
    }

    struct DelaiEveil final : public DataMap::Base
    {
        Milliseconds prochain{ 0 };
    };

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
            PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

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
            RecalculerBonusDeSet(joueur);
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
}

void AddAscensionArtisanatScripts()
{
    new ArtisanatWorld();
    new ArtisanatPlayer();
}
