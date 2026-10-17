/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T1 « Le Sourcier », tache T1-4 (docs/SPEC-artisanat-T-sourcier-dev.md ; conception
// docs/CONCEPTION-artisanat-T-sourcier.md v2, § 2) : le Filon-mere, le registre par personnage et le Coeur de foudre.
//
// 1. Registre (acore_characters.coa_artisanat_sourcier, migration data/sql/db-characters/2026_10_15_01_...) : dernier
//    lancement (jour de jeu, remise a 06:00 Paris), relance accordee (echec), second essai du jour, garanti de la semaine
//    (remise le mercredi 06:00 Paris), recompenses de participant du jour. Lu et ecrit en SYNCHRONE (DirectExecute) : quelques
//    requetes par evenement, jamais par coup ; aucune course entre une ecriture et la lecture qui suit.
// 2. « Reveiller le Filon-mere » (SORT_REVEILLER_FILON) : garde au lancement (AllSpellScript::OnSpellCheckCast, message
//    lisible), puis au lancement reussi (OnSpellCast) : la Veine foudroyee disparait, le Filon-mere (GO_FILON_MERE) apparait
//    a sa place, la recharge affichee court jusqu'a la remise.
// 3. Le Filon-mere (GameObjectAI, ScriptName SCRIPT_FILON_MERE) : 5 mineurs au plus, 3 usages chacun, butin personnel du
//    gabarit a chaque usage (Player::AutoStoreLoot) ; le Colosse d'orage sort a la moitie des usages attendus (humains du
//    groupe a 40 m au lancement) ; minage suspendu tant qu'il vit ; sa mort = succes, puis GRACE_FIN_S pour les usages
//    restants ; echec au bout de DUREE_MAX_S, groupe entier mort, specialiste tue par un monstre et absent ABSENCE_MAX_S, ou
//    specialiste deconnecte. Tout l'etat vit dans l'AI du Filon, donc sur le fil de sa carte (P-137) ; les crochets
//    d'unite (degats, soins, morts) le joignent par ObjectAccessor::GetGameObject sur LEUR carte, qui est la sienne.
// 4. Coeur de foudre : remis au seul specialiste (AddItem, fabricant = lui), garanti au 1er succes de la semaine avec
//    COMPTES_HUMAINS_GARANTI comptes humains distincts parmi les participants, sinon CHANCE_COEUR_POUR_CENT.
// 5. Recompenses de participant (humains de NIVEAU_RECOMPENSE et plus, groupe present ou credites de 2 % des PV du
//    Colosse, de la faction du specialiste) : ressources d'evenement, RECOMPENSES_PAR_JOUR fois par jour au plus.
// 6. Echec cause par un joueur (revue du 16/10, constat 12, D-PO-51) : la mort du specialiste ne compte « par un joueur »
//    que si le tueur, ou son maitre, est un HUMAIN (pas un bot) d'une AUTRE equipe ; meme alors, la relance immediate
//    n'est accordee qu'une fois par jour de jeu (colonne relance_jcj_jour, migration 2026_10_16_01) ; au-dela, ou sans
//    la colonne, l'echec est traite comme un echec ordinaire.
// 7. Coeur de foudre d'un specialiste connecte sur une autre carte (constat 14) : file d'attente par GUID, videe sur le
//    fil du joueur (prochain OnPlayerUpdate ou connexion) ; deconnecte avant : courrier en base ; hors ligne : courrier.
// Journal : « coa sourcier: ... » (lancement, refus, usage, colosse, succes, echec, remise, recompense, nettoyage).
//
// Tout identifiant non recu (AscensionArtisanatIds.h a 0) laisse la fonction INERTE : le sort est refuse avec « pas encore
// disponible », rien n'apparait.

#include "AscensionArtisanatIds.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

using namespace Acore::ChatCommands;
using namespace CoaArtisanatIds;

namespace
{
    // ---- Temps de jeu : jour et semaine, remise a 06:00 (Paris) ----------------------------------------------------

    // Decalage de Paris sur UTC, en secondes : regle europeenne (ete du dernier dimanche de mars 01:00 UTC au dernier
    // dimanche d'octobre 01:00 UTC). Le serveur peut tourner en UTC : on ne lit pas son fuseau.
    int64 DecalageParisS(time_t utc)
    {
        std::tm t{};
        gmtime_r(&utc, &t);
        auto dernierDimanche = [](int annee, int mois) -> time_t   // mois 0-11, a 01:00 UTC
        {
            std::tm d{};
            d.tm_year = annee;
            d.tm_mon = mois + 1;   // le 1er du mois suivant, moins un jour
            d.tm_mday = 0;
            d.tm_hour = 1;
            time_t const fin = timegm(&d);
            std::tm f{};
            gmtime_r(&fin, &f);
            return fin - time_t(f.tm_wday) * 86400;
        };
        time_t const debutEte = dernierDimanche(t.tm_year, 2);
        time_t const finEte = dernierDimanche(t.tm_year, 9);
        return (utc >= debutEte && utc < finEte) ? 7200 : 3600;
    }

    int64 DivBas(int64 a, int64 b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    int64 JourDeJeu(time_t utc)
    {
        return DivBas(int64(utc) + DecalageParisS(utc) - int64(HEURE_REMISE_PARIS) * 3600, 86400);
    }

    // Semaine de jeu : du mercredi 06:00 (Paris) au mercredi suivant. Le jour 6 (1970-01-07) est un mercredi.
    int64 SemaineDeJeu(int64 jour)
    {
        return DivBas(jour - 6, 7);
    }

    // Instant UTC de la prochaine remise quotidienne. Le decalage est lu a 04:00 UTC du jour suivant : apres le changement
    // d'heure (01:00 UTC), avant 06:00 Paris (04:00 ou 05:00 UTC).
    time_t ProchaineRemise(time_t utc)
    {
        int64 const demain = JourDeJeu(utc) + 1;
        time_t const repere = time_t(demain * 86400 + 4 * 3600);
        return time_t(demain * 86400 + int64(HEURE_REMISE_PARIS) * 3600 - DecalageParisS(repere));
    }

    std::string HeureParis(time_t utc)
    {
        time_t const local = utc + DecalageParisS(utc);
        std::tm t{};
        gmtime_r(&local, &t);
        return Acore::StringFormat("{:02}:{:02}", t.tm_hour, t.tm_min);
    }

    time_t Maintenant()
    {
        return GameTime::GetGameTime().count();
    }

    // ---- Registre par personnage -------------------------------------------------------------------------------------

    bool TableRegistre = false;
    bool ColonneRelanceJcj = false;   // relance_jcj_jour (migration 2026_10_16_01) ; absente : jamais de relance immediate

    struct Registre
    {
        int64 jourLance = 0;
        uint32 relanceTs = 0;       // instant (UTC) a partir duquel une relance est accordee le meme jour ; 0 : aucune
        int64 essai2Jour = 0;       // jour ou le second essai (echec ordinaire) a ete accorde
        int64 semaineGaranti = -1;  // semaine ou le Coeur garanti a ete donne
        int64 recompJour = 0;
        uint32 recompN = 0;
        int64 relanceJcjJour = 0;   // jour ou la relance immediate (mort causee par un humain adverse) a ete accordee
    };

    Registre LireRegistre(uint32 guid)
    {
        Registre r;
        if (!TableRegistre)
            return r;
        if (QueryResult q = CharacterDatabase.Query("SELECT jour_lance, relance_ts, essai2_jour, semaine_garanti, recomp_jour, recomp_n{} "
            "FROM coa_artisanat_sourcier WHERE guid = {}", ColonneRelanceJcj ? ", relance_jcj_jour" : "", guid))
        {
            Field* f = q->Fetch();
            r.jourLance = f[0].Get<int32>();
            r.relanceTs = f[1].Get<uint32>();
            r.essai2Jour = f[2].Get<int32>();
            r.semaineGaranti = f[3].Get<int32>();
            r.recompJour = f[4].Get<int32>();
            r.recompN = f[5].Get<uint8>();
            if (ColonneRelanceJcj)
                r.relanceJcjJour = f[6].Get<int32>();
        }
        return r;
    }

    void EcrireRegistre(uint32 guid, Registre const& r)
    {
        if (!TableRegistre)
            return;
        // REPLACE remet toute colonne non nommee a son defaut : relance_jcj_jour est nommee des qu'elle existe.
        if (ColonneRelanceJcj)
            CharacterDatabase.DirectExecute("REPLACE INTO coa_artisanat_sourcier (guid, jour_lance, relance_ts, essai2_jour, semaine_garanti, "
                "recomp_jour, recomp_n, relance_jcj_jour) VALUES ({}, {}, {}, {}, {}, {}, {}, {})", guid, int32(r.jourLance), r.relanceTs,
                int32(r.essai2Jour), int32(r.semaineGaranti), int32(r.recompJour), std::min<uint32>(r.recompN, 255), int32(r.relanceJcjJour));
        else
            CharacterDatabase.DirectExecute("REPLACE INTO coa_artisanat_sourcier (guid, jour_lance, relance_ts, essai2_jour, semaine_garanti, "
                "recomp_jour, recomp_n) VALUES ({}, {}, {}, {}, {}, {}, {})", guid, int32(r.jourLance), r.relanceTs, int32(r.essai2Jour),
                int32(r.semaineGaranti), int32(r.recompJour), std::min<uint32>(r.recompN, 255));
    }

    // Le lancement est-il accorde maintenant ? Une fois par jour de jeu, plus la relance accordee par un echec.
    bool LancementAccorde(Registre const& r, time_t maintenant)
    {
        return r.jourLance != JourDeJeu(maintenant) || (r.relanceTs && maintenant >= time_t(r.relanceTs));
    }

    // Recharge affichee sur le bouton : jusqu'a la relance accordee, sinon jusqu'a la remise ; rien si accorde.
    void AfficherRecharge(Player* joueur, Registre const& r)
    {
        if (!SORT_REVEILLER_FILON || !joueur->HasSpell(SORT_REVEILLER_FILON))
            return;
        time_t const maintenant = Maintenant();
        joueur->RemoveSpellCooldown(SORT_REVEILLER_FILON, true);
        if (LancementAccorde(r, maintenant))
            return;
        time_t fin = ProchaineRemise(maintenant);
        if (r.relanceTs && time_t(r.relanceTs) > maintenant && time_t(r.relanceTs) < fin)
            fin = time_t(r.relanceTs);
        uint32 const ms = uint32(std::max<int64>(1, int64(fin - maintenant))) * IN_MILLISECONDS;
        joueur->AddSpellCooldown(SORT_REVEILLER_FILON, 0, ms, true);
        WorldPacket paquet;
        joueur->BuildCooldownPacket(paquet, SPELL_COOLDOWN_FLAG_NONE, SORT_REVEILLER_FILON, ms);
        joueur->SendDirectMessage(&paquet);
    }

    // ---- Evenements actifs (exclusivite a RAYON_EXCLUSIF) : toutes cartes, donc sous verrou ----------------------------

    struct EvenementActif
    {
        ObjectGuid filon;
        uint32 carte = 0;
        uint32 instance = 0;
        float x = 0.0f, y = 0.0f;
    };
    std::mutex VerrouActifs;
    std::vector<EvenementActif> Actifs;
    std::atomic<uint32> NombreActifs{ 0 };    // lu sans verrou par les crochets frequents (soins) pour sortir tot
    bool GabaritsPresents = false;            // Filon (avec son script) et Colosse en base, lu au demarrage

    bool EvenementProche(uint32 carte, uint32 instance, float x, float y)
    {
        std::lock_guard<std::mutex> verrou(VerrouActifs);
        for (EvenementActif const& e : Actifs)
            if (e.carte == carte && e.instance == instance && (e.x - x) * (e.x - x) + (e.y - y) * (e.y - y) <= RAYON_EXCLUSIF * RAYON_EXCLUSIF)
                return true;
        return false;
    }

    void InscrireActif(GameObject* filon)
    {
        std::lock_guard<std::mutex> verrou(VerrouActifs);
        Actifs.push_back({ filon->GetGUID(), filon->GetMapId(), filon->GetInstanceId(), filon->GetPositionX(), filon->GetPositionY() });
        NombreActifs = uint32(Actifs.size());
    }

    void RetirerActif(ObjectGuid filon)
    {
        std::lock_guard<std::mutex> verrou(VerrouActifs);
        Actifs.erase(std::remove_if(Actifs.begin(), Actifs.end(), [&](EvenementActif const& e) { return e.filon == filon; }), Actifs.end());
        NombreActifs = uint32(Actifs.size());
    }

    // ---- Marques (CustomData) : relient une unite a son Filon, sur le fil de sa carte -----------------------------------

    std::string const CLE_FILON = "CoaSourcierFilon";
    struct MarqueFilon final : public DataMap::Base
    {
        ObjectGuid filon;
    };

    void Marquer(Unit* u, ObjectGuid filon)
    {
        u->CustomData.GetDefault<MarqueFilon>(CLE_FILON)->filon = filon;
    }

    bool EstHumain(Player const* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    // Donne `nombre` x `entree` a `joueur` (sur sa carte), fabricant `createur` si non vide ; ce qui ne tient pas en sac part
    // par courrier. Rend vrai si tout est parti.
    bool Donner(Player* joueur, uint32 entree, uint32 nombre, ObjectGuid createur)
    {
        if (!nombre || !sObjectMgr->GetItemTemplate(entree))
            return false;
        uint32 restant = nombre;
        while (restant)
        {
            ItemPosCountVec dest;
            uint32 pasPlace = 0;
            InventoryResult const r = joueur->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entree, restant, &pasPlace);
            uint32 const tient = (r == EQUIP_ERR_OK) ? restant : restant - pasPlace;
            if (!tient || dest.empty())
                break;
            Item* objet = joueur->StoreNewItem(dest, entree, true);
            if (!objet)
                break;
            if (createur)
            {
                objet->SetGuidValue(ITEM_FIELD_CREATOR, createur);
                objet->SetState(ITEM_CHANGED, joueur);
            }
            joueur->SendNewItem(objet, tient, true, false);
            restant -= tient;
        }
        if (!restant)
            return true;
        // Sacs pleins : courrier, comme le coeur le fait pour les butins de quete (rien ne se perd).
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft courrier("Le Filon-mère", "Vos sacs étaient pleins.");
        while (restant)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entree);
            uint32 const pile = std::min<uint32>(restant, std::max<uint32>(1, proto->GetMaxStackSize()));
            Item* objet = Item::CreateItem(entree, pile, joueur);
            if (!objet)
                break;
            if (createur)
                objet->SetGuidValue(ITEM_FIELD_CREATOR, createur);
            objet->SaveToDB(trans);
            courrier.AddItem(objet);
            restant -= pile;
        }
        courrier.SendMailTo(trans, MailReceiver(joueur, joueur->GetGUID().GetCounter()), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
        CharacterDatabase.CommitTransaction(trans);
        return !restant;
    }

    // ---- Coeurs de foudre a remettre a un specialiste connecte sur une autre carte (constat 14) -------------------------
    //
    // Le courrier par GUID seul (MailReceiver(lowGuid)) n'entre qu'en base : un joueur deja en ligne ne le voit qu'a sa
    // reconnexion (Mail.cpp:255-260, courriers charges a la connexion). On note donc la remise par GUID, sous verrou (le
    // Filon et le joueur sont sur deux fils de carte), et le joueur la prend sur SON fil : ViderRemises.
    std::mutex VerrouRemises;
    std::map<ObjectGuid, uint32> RemisesCoeur;   // specialiste -> Coeurs en attente
    std::atomic<uint32> NombreRemises{ 0 };      // lu sans verrou a chaque mise a jour de joueur pour sortir tot

    // Courrier en BASE seulement (MailReceiver par GUID) : vu a la prochaine connexion. N'ecrit sur aucun objet en memoire :
    // appelable de tout fil. Rend vrai si tous les Coeurs sont partis.
    bool PosterCoeurs(ObjectGuid specialiste, uint32 n)
    {
        if (!OBJET_COEUR_DE_FOUDRE || !n)
            return false;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft courrier("Le Filon-mère", "Le Filon-mère vous livre un Cœur de foudre.");
        uint32 poses = 0;
        for (; poses < n; ++poses)
        {
            Item* coeur = Item::CreateItem(OBJET_COEUR_DE_FOUDRE, 1, nullptr);
            if (!coeur)
                break;
            coeur->SetGuidValue(ITEM_FIELD_CREATOR, specialiste);
            coeur->SaveToDB(trans);
            courrier.AddItem(coeur);
        }
        if (!poses)
            return false;
        courrier.SendMailTo(trans, MailReceiver(specialiste.GetCounter()), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
        CharacterDatabase.CommitTransaction(trans);
        return poses == n;
    }

    void MettreEnFile(ObjectGuid specialiste)
    {
        std::lock_guard<std::mutex> verrou(VerrouRemises);
        ++RemisesCoeur[specialiste];
        NombreRemises = uint32(RemisesCoeur.size());
    }

    // Fil du joueur (mise a jour, connexion, avant deconnexion) : remet les Coeurs en attente et le previent.
    void ViderRemises(Player* j, char const* moment)
    {
        if (!NombreRemises.load(std::memory_order_relaxed) || !j)
            return;
        uint32 n = 0;
        {
            std::lock_guard<std::mutex> verrou(VerrouRemises);
            auto it = RemisesCoeur.find(j->GetGUID());
            if (it == RemisesCoeur.end())
                return;
            n = it->second;
            RemisesCoeur.erase(it);
            NombreRemises = uint32(RemisesCoeur.size());
        }
        // A la deconnexion, la sauvegarde qui suit peut ne pas avoir lieu (LogoutPlayer(save = false)) : courrier en base,
        // independant de cette sauvegarde. Sinon, dans les sacs (ou courrier en memoire s'ils sont pleins, Donner).
        bool const deconnexion = std::string_view(moment) == "deconnexion";
        bool const remis = deconnexion ? PosterCoeurs(j->GetGUID(), n)
            : (OBJET_COEUR_DE_FOUDRE && Donner(j, OBJET_COEUR_DE_FOUDRE, n, j->GetGUID()));
        LOG_INFO("module", "coa sourcier: remise differee joueur={} coeurs={} moment={} voie={} remis={}", j->GetGUID().GetCounter(), n,
            moment, deconnexion ? "courrier" : "sacs", remis ? 1 : 0);
        if (remis && !deconnexion && j->GetSession())
            ChatHandler(j->GetSession()).SendNotification("Le Filon-mère vous livre un Cœur de foudre.");
    }

    // Le Coeur garanti, puis la chance : decision pure (le banc la rejoue sans rien ecrire).
    struct DecisionCoeur
    {
        bool garanti = false;
        bool obtenu = false;
    };
    DecisionCoeur DeciderCoeur(Registre& r, int64 semaine, uint32 comptesHumains)
    {
        DecisionCoeur d;
        if (r.semaineGaranti != semaine && comptesHumains >= COMPTES_HUMAINS_GARANTI)
        {
            r.semaineGaranti = semaine;
            d.garanti = d.obtenu = true;
            return d;
        }
        d.obtenu = urand(1, 100) <= CHANCE_COEUR_POUR_CENT;
        return d;
    }

    // ---- Le Filon-mere ----------------------------------------------------------------------------------------------

    class FilonMereAI final : public GameObjectAI
    {
    public:
        explicit FilonMereAI(GameObject* go) : GameObjectAI(go) { }

        ~FilonMereAI() override
        {
            // Carte dechargee, despawn par le minuteur de securite : l'exclusivite ne doit pas survivre au Filon.
            RetirerActif(_filon);
        }

        enum class Phase : uint8 { Attente, Minage, Colosse, Grace, Fini };

        // Fil de la carte, au lancement (le specialiste est sur cette carte).
        void Demarrer(Player* specialiste, GameObject* veine)
        {
            _filon = me->GetGUID();
            _specialiste = specialiste->GetGUID();
            _equipe = specialiste->GetTeamId();
            _debut = Maintenant();
            _veine = veine ? veine->GetGUID() : ObjectGuid::Empty;
            _groupe = specialiste->GetGroup() ? specialiste->GetGroup()->GetGUID() : ObjectGuid::Empty;
            _attendus = std::max<uint32>(1, HumainsDuGroupe(specialiste, RAYON_DOSAGE));
            _seuilColosse = (std::min<uint32>(_attendus, JOUEURS_MAX) * USAGES_PAR_JOUEUR + 1) / 2;
            _phase = Phase::Minage;
            Marquer(specialiste, _filon);
            InscrireActif(me);
            LOG_INFO("module", "coa sourcier: lancement specialiste={} filon={} carte={} x={:.1f} y={:.1f} humains_attendus={} seuil_colosse={}",
                _specialiste.GetCounter(), _filon.GetCounter(), me->GetMapId(), me->GetPositionX(), me->GetPositionY(), _attendus, _seuilColosse);
        }

        bool GossipHello(Player* joueur, bool reportUse) override
        {
            // CMSG_GAMEOBJ_REPORT_USE rappelle GossipHello apres GameObject::Use (SpellHandler.cpp:370) : un clic, un usage.
            if (reportUse)
                return true;
            Miner(joueur);
            return true;   // le Filon ne s'ouvre jamais comme un coffre ordinaire
        }

        void UpdateAI(uint32 diff) override
        {
            if (_phase == Phase::Attente || _phase == Phase::Fini)
                return;
            _cumul += diff;
            if (_cumul < 1000)
                return;
            _cumul = 0;
            Surveiller();
        }

        // ---- Appels des crochets d'unite (meme carte) ----

        void Crediter(Player* joueur, uint32 montant)
        {
            // Seul ce qui est fait pendant le combat contre le Colosse compte (2 % de SES PV).
            if (!joueur || _phase != Phase::Colosse)
                return;
            _credits[joueur->GetGUID()] += montant;
            Marquer(joueur, _filon);
        }

        // `tueur` : le joueur a qui revient le coup mortel (lui, ou le maitre de ce qui a tue), nul pour un monstre. Ne compte
        // « par un joueur » qu'un HUMAIN d'une AUTRE equipe que le specialiste (D-PO-51) : un bot, ou un humain de sa faction,
        // est traite comme un monstre (regle des ABSENCE_MAX_S).
        void SpecialisteMort(Player const* tueur)
        {
            bool const humain = EstHumain(tueur);
            bool const adverse = tueur && tueur->GetTeamId() != _equipe;
            _specialisteMortA = Maintenant();
            _mortParJoueur = humain && adverse;
            LOG_INFO("module", "coa sourcier: specialiste mort filon={} specialiste={} tueur_joueur={} humain={} adverse={} par_joueur={}",
                _filon.GetCounter(), _specialiste.GetCounter(), tueur ? tueur->GetGUID().GetCounter() : 0, humain ? 1 : 0, adverse ? 1 : 0,
                _mortParJoueur ? 1 : 0);
        }

        void ColosseMort()
        {
            if (_phase == Phase::Colosse)
                Reussir();
        }

        bool EstColosse(ObjectGuid g) const { return g && g == _colosse; }
        bool EstSpecialiste(ObjectGuid g) const { return g && g == _specialiste; }
        uint32 SeuilParticipation() const { return _seuilParticipation; }

    private:
        static uint32 HumainsDuGroupe(Player* specialiste, float rayon)
        {
            uint32 n = 0;
            if (Group* g = specialiste->GetGroup())
            {
                for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                    if (Player* m = ref->GetSource())
                        if (EstHumain(m) && m->IsInWorld() && m->GetMap() == specialiste->GetMap() && m->IsAlive()
                            && m->IsWithinDist(specialiste, rayon))
                            ++n;
            }
            else if (EstHumain(specialiste))
                n = 1;
            return n;
        }

        void Dire(Player* joueur, std::string const& texte)
        {
            ChatHandler(joueur->GetSession()).SendNotification(texte);
        }

        void Miner(Player* joueur)
        {
            if (!EstHumain(joueur) || !joueur->IsAlive())
                return;
            if (_phase == Phase::Colosse)
                return Dire(joueur, "Le Colosse d'orage vous empêche de miner.");
            if (_phase != Phase::Minage && _phase != Phase::Grace)
                return Dire(joueur, "Le Filon-mère s'éteint.");
            if (joueur->GetTeamId() != _equipe)
                return Dire(joueur, "Ce filon ne répond pas à votre faction.");
            auto it = _usages.find(joueur->GetGUID());
            if (it == _usages.end() && _usages.size() >= JOUEURS_MAX)
            {
                LOG_INFO("module", "coa sourcier: usage refuse filon={} joueur={} raison=complet", _filon.GetCounter(), joueur->GetGUID().GetCounter());
                return Dire(joueur, Acore::StringFormat("Le Filon-mère ne se laisse miner que par {} personnes.", JOUEURS_MAX));
            }
            if (it != _usages.end() && it->second >= USAGES_PAR_JOUEUR)
            {
                LOG_INFO("module", "coa sourcier: usage refuse filon={} joueur={} raison=epuise", _filon.GetCounter(), joueur->GetGUID().GetCounter());
                return Dire(joueur, Acore::StringFormat("Vous avez déjà miné ce filon {} fois.", USAGES_PAR_JOUEUR));
            }
            // AutoStoreLoot jette ce qui ne tient pas (Player.cpp:14280-14284) : l'usage n'est compte qu'avec de la place.
            if (joueur->GetFreeInventorySpace() < PLACE_USAGE)
                return Dire(joueur, Acore::StringFormat("Il faut {} cases libres pour miner le Filon-mère.", PLACE_USAGE));
            uint8& n = _usages[joueur->GetGUID()];
            ++n;
            ++_usagesTotal;
            Marquer(joueur, _filon);
            if (uint32 const butin = me->GetGOInfo()->GetLootId())
                joueur->AutoStoreLoot(butin, LootTemplates_Gameobject, true);
            LOG_INFO("module", "coa sourcier: usage filon={} joueur={} usage={}/{} total={} mineurs={}", _filon.GetCounter(),
                joueur->GetGUID().GetCounter(), uint32(n), USAGES_PAR_JOUEUR, _usagesTotal, uint32(_usages.size()));
            if (_phase == Phase::Minage && !_colosseFait && _usagesTotal >= _seuilColosse)
                InvoquerColosse(joueur);
        }

        void InvoquerColosse(Player* declencheur)
        {
            _colosseFait = true;
            Position pos = me->GetPosition();
            me->MovePosition(pos, 4.0f, float(rand_norm()) * 2.0f * float(M_PI));
            uint32 const dureeMs = (DUREE_MAX_S + GRACE_FIN_S + 300) * IN_MILLISECONDS;   // filet : se retire seul
            TempSummon* colosse = me->GetMap()->SummonCreature(PNJ_COLOSSE_ORAGE, pos, nullptr, dureeMs);
            if (!colosse)
            {
                LOG_ERROR("module", "coa sourcier: colosse {} non invoque filon={} : echec", PNJ_COLOSSE_ORAGE, _filon.GetCounter());
                return Echouer("colosse_absent", false);
            }
            // Calibrage reduit (conception § 2) : PV en % du modele selon les humains du groupe presents a 40 m.
            uint32 humains = 1;
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
                humains = std::max<uint32>(1, HumainsDuGroupe(s, RAYON_DOSAGE));
            uint8 const pct = PV_COLOSSE_POUR_CENT[std::min<std::size_t>(humains, PV_COLOSSE_POUR_CENT.size()) - 1];
            float const base = colosse->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE);
            if (base > 0.0f && pct < 100)
            {
                float const dosee = std::max(1.0f, std::round(base * float(pct) / 100.0f));
                colosse->SetCreateHealth(uint32(dosee));
                colosse->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, dosee);
            }
            colosse->SetHealth(colosse->GetMaxHealth());
            _colosse = colosse->GetGUID();
            _seuilParticipation = std::max<uint32>(1, uint32(uint64(colosse->GetMaxHealth()) * PART_PARTICIPATION_POUR_MILLE / 1000));
            Marquer(colosse, _filon);
            _phase = Phase::Colosse;
            if (declencheur && colosse->IsAIEnabled && colosse->AI())
                colosse->AI()->AttackStart(declencheur);
            LOG_INFO("module", "coa sourcier: colosse filon={} colosse={} humains_presents={} pv_pct={} pv={} seuil_participation={}",
                _filon.GetCounter(), _colosse.GetCounter(), humains, uint32(pct), colosse->GetMaxHealth(), _seuilParticipation);
        }

        // Une fois par seconde.
        void Surveiller()
        {
            time_t const maintenant = Maintenant();
            if (_phase == Phase::Grace)
            {
                if (maintenant >= _graceFin)
                    Terminer("fin");
                return;
            }
            Player* s = ObjectAccessor::GetPlayer(*me, _specialiste);
            if (!s && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                return Echouer("deconnexion", false);
            // Specialiste tue par un monstre, et pas revenu (vivant, a portee) dans les 60 s.
            bool const present = s && s->IsAlive() && s->IsWithinDist(me, RAYON_PARTICIPATION);
            if (s && s->GetGroup())
                _groupe = s->GetGroup()->GetGUID();   // un groupe forme apres le lancement compte aussi
            if (present)
            {
                _specialisteMortA = 0;
                _mortParJoueur = false;   // revenu : sa mort passee ne qualifie plus un echec
            }
            else if (_specialisteMortA && !_mortParJoueur && maintenant - _specialisteMortA > time_t(ABSENCE_MAX_S))
                return Echouer("specialiste", false);
            // Tout le groupe mort (bots compris : « le groupe entier ») parmi ceux qui sont a portee.
            if (!present && !GroupeEnVie())
                return Echouer("groupe", _mortParJoueur);
            if (maintenant - _debut >= time_t(DUREE_MAX_S))
                return Echouer("temps", false);
            if (_phase == Phase::Colosse)
            {
                Creature* c = ObjectAccessor::GetCreature(*me, _colosse);
                if (!c)
                    return Echouer("colosse_disparu", false);
                if (!c->IsAlive())
                    return Reussir();
            }
        }

        // Un membre du groupe du specialiste (note au lancement), vivant, sur CETTE carte, a portee. On ne parcourt que les
        // joueurs de la carte du Filon : aucun objet d'une autre carte n'est lu (P-137).
        bool GroupeEnVie()
        {
            for (MapReference const& ref : me->GetMap()->GetPlayers())
                if (Player* m = ref.GetSource())
                {
                    if (!m->IsAlive())
                        continue;
                    // Le specialiste vivant sur la carte, ou un membre de son groupe vivant a portee.
                    if (m->GetGUID() == _specialiste)
                        return true;
                    if (_groupe && m->GetGroup() && m->GetGroup()->GetGUID() == _groupe && m->IsWithinDist(me, RAYON_PARTICIPATION))
                        return true;
                }
            return false;
        }

        void Reussir()
        {
            _phase = Phase::Grace;
            _graceFin = Maintenant() + time_t(GRACE_FIN_S);
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            int64 const semaine = SemaineDeJeu(jour);

            // Participants humains de la faction : groupe du specialiste present a 100 m, plus les credites (2 % des PV).
            std::set<ObjectGuid> participants;
            Player* s = ObjectAccessor::GetPlayer(*me, _specialiste);
            if (s)
            {
                participants.insert(s->GetGUID());
                if (Group* g = s->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (EstHumain(m) && m->IsInWorld() && m->GetMap() == me->GetMap() && m->IsWithinDist(me, RAYON_PARTICIPATION))
                                participants.insert(m->GetGUID());
            }
            for (auto const& [g, montant] : _credits)
                if (montant >= _seuilParticipation)
                    participants.insert(g);
            std::set<uint32> comptes;
            std::vector<Player*> humains;
            for (ObjectGuid g : participants)
                if (Player* p = ObjectAccessor::GetPlayer(*me, g))
                    if (EstHumain(p) && p->GetTeamId() == _equipe)
                    {
                        humains.push_back(p);
                        comptes.insert(p->GetSession()->GetAccountId());
                    }
            LOG_INFO("module", "coa sourcier: succes filon={} specialiste={} participants_humains={} comptes={} duree_s={}",
                _filon.GetCounter(), _specialiste.GetCounter(), uint32(humains.size()), uint32(comptes.size()), uint32(maintenant - _debut));

            // Le Coeur de foudre, au seul specialiste.
            Registre r = LireRegistre(_specialiste.GetCounter());
            DecisionCoeur const d = DeciderCoeur(r, semaine, uint32(comptes.size()));
            EcrireRegistre(_specialiste.GetCounter(), r);
            bool remis = false;
            char const* voie = "aucune";
            if (d.obtenu && OBJET_COEUR_DE_FOUDRE)
            {
                if (s)
                {
                    remis = Donner(s, OBJET_COEUR_DE_FOUDRE, 1, s->GetGUID());
                    voie = "sacs";
                }
                else
                    remis = RemettreCoeurAilleurs(voie);
            }
            LOG_INFO("module", "coa sourcier: remise filon={} specialiste={} garanti={} obtenu={} remis={} voie={} semaine={} comptes={}",
                _filon.GetCounter(), _specialiste.GetCounter(), d.garanti ? 1 : 0, d.obtenu ? 1 : 0, remis ? 1 : 0, voie, int32(semaine),
                uint32(comptes.size()));
            if (s && d.obtenu)
                ChatHandler(s->GetSession()).SendNotification("Le Filon-mère vous livre un Cœur de foudre.");

            // Recompenses de participant.
            for (Player* p : humains)
            {
                if (p->GetLevel() < NIVEAU_RECOMPENSE)
                    continue;
                Registre rp = (p->GetGUID() == _specialiste) ? r : LireRegistre(p->GetGUID().GetCounter());
                if (rp.recompJour != jour)
                {
                    rp.recompJour = jour;
                    rp.recompN = 0;
                }
                if (rp.recompN >= RECOMPENSES_PAR_JOUR)
                {
                    LOG_INFO("module", "coa sourcier: recompense refusee filon={} joueur={} raison=plafond_du_jour", _filon.GetCounter(),
                        p->GetGUID().GetCounter());
                    continue;
                }
                ++rp.recompN;
                EcrireRegistre(p->GetGUID().GetCounter(), rp);
                if (p->GetGUID() == _specialiste)
                    r = rp;
                uint32 const rare = urand(1, 100) <= CHANCE_RARE_POUR_CENT ? 1 : 0;
                uint32 const epique = urand(1, 1000) <= CHANCE_EPIQUE_POUR_MILLE ? 1 : 0;
                Donner(p, RESSOURCE_VERTE, RESSOURCES_VERTES, ObjectGuid::Empty);
                if (rare)
                    Donner(p, RESSOURCE_RARE, 1, ObjectGuid::Empty);
                if (epique)
                    Donner(p, RESSOURCE_EPIQUE, 1, ObjectGuid::Empty);
                LOG_INFO("module", "coa sourcier: recompense filon={} joueur={} vertes={} rare={} epique={} du_jour={}/{}", _filon.GetCounter(),
                    p->GetGUID().GetCounter(), RESSOURCES_VERTES, rare, epique, rp.recompN, RECOMPENSES_PAR_JOUR);
            }
        }

        // Specialiste hors de cette carte au succes. Connecte ailleurs : file d'attente, videe sur son fil (ViderRemises) ;
        // FindConnectedPlayer ne sert qu'a le savoir en ligne, rien n'est lu ni ecrit sur lui ici (P-137). Hors ligne :
        // courrier en base, charge a sa connexion.
        bool RemettreCoeurAilleurs(char const*& voie)
        {
            if (ObjectAccessor::FindConnectedPlayer(_specialiste))
            {
                MettreEnFile(_specialiste);
                voie = "file";
                return true;
            }
            voie = "courrier";
            return PosterCoeurs(_specialiste, 1);
        }

        // Echec : rien de legendaire ; la relance est accordee tout de suite si un joueur l'a cause, sinon au bout de 4 h
        // une fois par jour.
        void Echouer(char const* raison, bool parJoueur)
        {
            if (_phase == Phase::Fini)
                return;
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            Registre r = LireRegistre(_specialiste.GetCounter());
            char const* relance = "aucune";
            // Relance immediate : une fois par jour de jeu (D-PO-51). Deja prise, ou colonne absente : echec ordinaire.
            bool const immediate = parJoueur && ColonneRelanceJcj && r.relanceJcjJour != jour;
            if (immediate)
            {
                r.relanceTs = uint32(maintenant);
                r.relanceJcjJour = jour;
                relance = "immediate";
            }
            else if (r.essai2Jour != jour)
            {
                r.relanceTs = uint32(maintenant + time_t(RELANCE_ECHEC_S));
                r.essai2Jour = jour;
                relance = "4h";
            }
            EcrireRegistre(_specialiste.GetCounter(), r);
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
            {
                AfficherRecharge(s, r);
                ChatHandler(s->GetSession()).SendNotification(immediate ? "Le Filon-mère se referme : vous pourrez le réveiller de nouveau."
                    : "Le Filon-mère se referme dans un grondement.");
            }
            LOG_INFO("module", "coa sourcier: echec filon={} specialiste={} raison={} par_joueur={} relance={} usages={} colosse={}",
                _filon.GetCounter(), _specialiste.GetCounter(), raison, parJoueur ? 1 : 0, relance, _usagesTotal, _colosseFait ? 1 : 0);
            Terminer(raison);
        }

        // Nettoyage : le Colosse s'en va, le Filon disparait (la veine revient a son minuteur), l'exclusivite est levee.
        void Terminer(char const* raison)
        {
            _phase = Phase::Fini;
            bool colosseRetire = false;
            // Vivant (echec) ou cadavre (fin de la grace) : rien ne reste apres le Filon.
            if (Creature* c = ObjectAccessor::GetCreature(*me, _colosse))
            {
                c->DespawnOrUnsummon();
                colosseRetire = true;
            }
            RetirerActif(_filon);
            LOG_INFO("module", "coa sourcier: nettoyage filon={} raison={} colosse_retire={}", _filon.GetCounter(), raison, colosseRetire ? 1 : 0);
            me->SetRespawnTime(0);
            me->Delete();
        }

        Phase _phase = Phase::Attente;
        ObjectGuid _filon, _specialiste, _veine, _colosse, _groupe;
        TeamId _equipe = TEAM_NEUTRAL;
        time_t _debut = 0, _graceFin = 0, _specialisteMortA = 0;
        bool _mortParJoueur = false, _colosseFait = false;
        uint32 _attendus = 1, _seuilColosse = 2, _usagesTotal = 0, _cumul = 0, _seuilParticipation = 1;
        std::map<ObjectGuid, uint8> _usages;
        std::map<ObjectGuid, uint32> _credits;
    };

    FilonMereAI* FilonDe(WorldObject const& sur, ObjectGuid filon)
    {
        if (!filon)
            return nullptr;
        GameObject* go = ObjectAccessor::GetGameObject(sur, filon);
        return go ? dynamic_cast<FilonMereAI*>(go->AI()) : nullptr;
    }

    FilonMereAI* FilonDe(Unit const* u)
    {
        MarqueFilon const* m = u ? u->CustomData.Get<MarqueFilon>(CLE_FILON) : nullptr;
        return m ? FilonDe(*u, m->filon) : nullptr;
    }

    class go_coa_filon_mere final : public GameObjectScript
    {
    public:
        go_coa_filon_mere() : GameObjectScript(SCRIPT_FILON_MERE) { }

        GameObjectAI* GetAI(GameObject* go) const override
        {
            return new FilonMereAI(go);
        }
    };

    // ---- Le sort « Reveiller le Filon-mere » ------------------------------------------------------------------------

    // Vide si le lancement est permis ; sinon le message montre au joueur. `veine` : la Veine foudroyee visee.
    std::string RefusLancement(Player* j, Spell const* sort, GameObject*& veine, std::string& code)
    {
        veine = nullptr;
        auto refus = [&](char const* c, std::string m) { code = c; return m; };
        if (!GabaritsPresents || !TableRegistre)
            return refus("inerte", "Le Filon-mère n'est pas encore disponible.");
        if (j->IsInCombat())
            return refus("combat", "Impossible en combat.");
        Map* carte = j->GetMap();
        if (!carte || carte->Instanceable() || carte->IsBattlegroundOrArena())
            return refus("instance", "Impossible en instance, en champ de bataille ou en arène.");
        if (!j->IsOutdoors())
            return refus("interieur", "Il faut être dehors.");
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(j->GetZoneId());
        AreaTableEntry const* aire = sAreaTableStore.LookupEntry(j->GetAreaId());
        uint32 const drapeaux = (zone ? zone->flags : 0) | (aire ? aire->flags : 0);
        if ((drapeaux & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY)) || j->IsInSanctuary())
            return refus("sanctuaire", "Impossible dans une capitale ou un sanctuaire.");
        Registre const r = LireRegistre(j->GetGUID().GetCounter());
        time_t const maintenant = Maintenant();
        if (!LancementAccorde(r, maintenant))
        {
            time_t fin = ProchaineRemise(maintenant);
            if (r.relanceTs && time_t(r.relanceTs) > maintenant && time_t(r.relanceTs) < fin)
                fin = time_t(r.relanceTs);
            return refus("deja", Acore::StringFormat("Vous avez déjà réveillé un Filon-mère aujourd'hui. De nouveau possible à {} (heure de Paris).",
                HeureParis(fin)));
        }
        if (GameObject* cible = sort ? sort->m_targets.GetGOTarget() : nullptr)
        {
            if (cible->GetEntry() == GO_VEINE_FOUDROYEE && cible->IsWithinDistInMap(j, PORTEE_VEINE))
                veine = cible;
        }
        if (!veine && VEINE_PAR_RECHERCHE)
            veine = j->FindNearestGameObject(GO_VEINE_FOUDROYEE, PORTEE_VEINE, true);
        if (!veine || !veine->isSpawned() || veine->getLootState() != GO_READY)
            return refus("veine", "Il faut se tenir près d'une Veine foudroyée.");
        if (EvenementProche(j->GetMapId(), j->GetInstanceId(), veine->GetPositionX(), veine->GetPositionY()))
            return refus("voisin", "Un autre Filon-mère gronde déjà tout près.");
        return {};
    }

    class SourcierSorts final : public AllSpellScript
    {
    public:
        SourcierSorts() : AllSpellScript("CoaSourcierSorts", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCheckCast(Spell* sort, bool /*strict*/, SpellCastResult& res) override
        {
            if (!SORT_REVEILLER_FILON || res != SPELL_CAST_OK || !sort || sort->GetSpellInfo()->Id != SORT_REVEILLER_FILON)
                return;
            Player* j = sort->GetCaster() ? sort->GetCaster()->ToPlayer() : nullptr;
            if (!j)
            {
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
            GameObject* veine = nullptr;
            std::string code;
            std::string const message = RefusLancement(j, sort, veine, code);
            if (message.empty())
                return;
            res = SPELL_FAILED_DONT_REPORT;
            ChatHandler(j->GetSession()).SendNotification(message);
            LOG_INFO("module", "coa sourcier: refus joueur={} raison={}", j->GetGUID().GetCounter(), code);
        }

        void OnSpellCast(Spell* sort, Unit* lanceur, SpellInfo const* info, bool /*skipCheck*/) override
        {
            if (!SORT_REVEILLER_FILON || !info || info->Id != SORT_REVEILLER_FILON)
                return;
            Player* j = lanceur ? lanceur->ToPlayer() : nullptr;
            if (!j)
                return;
            GameObject* veine = nullptr;
            std::string code;
            if (!RefusLancement(j, sort, veine, code).empty())
                return;   // deja refuse par CheckCast ; un lancement force (MJ, declenche) qui ne passe pas ne fait rien
            time_t const maintenant = Maintenant();
            Registre r = LireRegistre(j->GetGUID().GetCounter());
            Position const pos = veine->GetPosition();
            GameObject* filon = j->GetMap()->SummonGameObject(GO_FILON_MERE, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                pos.GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, DUREE_MAX_S + GRACE_FIN_S + 300);
            FilonMereAI* ai = filon ? dynamic_cast<FilonMereAI*>(filon->AI()) : nullptr;
            if (!ai)
            {
                // Gabarit absent ou ScriptName different : rien n'est consomme, le lancement reste accorde.
                LOG_ERROR("module", "coa sourcier: Filon-mere {} non pose ou sans le script {} : lancement rendu", GO_FILON_MERE, SCRIPT_FILON_MERE);
                if (filon)
                {
                    filon->SetRespawnTime(0);
                    filon->Delete();
                }
                ChatHandler(j->GetSession()).SendNotification("Le Filon-mère ne répond pas.");
                AfficherRecharge(j, r);
                return;
            }
            r.jourLance = JourDeJeu(maintenant);
            r.relanceTs = 0;
            EcrireRegistre(j->GetGUID().GetCounter(), r);
            veine->DespawnOrUnsummon(0ms, Seconds(DUREE_MAX_S + GRACE_FIN_S));
            ai->Demarrer(j, veine);
            AfficherRecharge(j, r);
        }
    };

    // ---- Crochets d'unite : degats et soins credites, morts ----------------------------------------------------------

    class SourcierUnites final : public UnitScript
    {
    public:
        SourcierUnites() : UnitScript("CoaSourcierUnites", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH }) { }

        void OnDamage(Unit* attaquant, Unit* victime, uint32& degats) override
        {
            if (!PNJ_COLOSSE_ORAGE || !degats || !victime || victime->GetEntry() != PNJ_COLOSSE_ORAGE || !victime->IsCreature())
                return;
            Player* joueur = attaquant ? attaquant->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (FilonMereAI* ai = joueur ? FilonDe(victime) : nullptr)
                if (ai->EstColosse(victime->GetGUID()))
                    ai->Crediter(joueur, std::min(degats, uint32(victime->GetHealth())));
        }

        // Un soin compte s'il est donne a un participant marque (specialiste, mineur, attaquant credite) de ce Filon.
        void OnHeal(Unit* soigneur, Unit* soigne, uint32& gain) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !gain || !soigneur || !soigne || !soigne->IsPlayer())
                return;
            Player* joueur = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (FilonMereAI* ai = joueur ? FilonDe(soigne) : nullptr)
                ai->Crediter(joueur, gain);   // OnHeal passe apres ModifyHealth : `gain` est deja le soin effectif (Unit.cpp:8469-8472)
        }

        void OnUnitDeath(Unit* mort, Unit* tueur) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !mort)
                return;
            FilonMereAI* ai = FilonDe(mort);
            if (!ai)
                return;
            if (mort->IsCreature() && ai->EstColosse(mort->GetGUID()))
                ai->ColosseMort();
            // Seul le specialiste compte pour la regle des 60 s ; les autres morts sont lues par GroupeEnVie.
            else if (mort->IsPlayer() && ai->EstSpecialiste(mort->GetGUID()))
            {
                Player const* joueur = (tueur && tueur != mort) ? tueur->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                ai->SpecialisteMort(joueur && joueur != mort ? joueur : nullptr);
            }
        }
    };

    // ---- Le talent apprend le sort ; le registre s'affiche a la connexion -------------------------------------------

    void SynchroniserSortFilon(Player* j)
    {
        if (!SORT_MARQUEUR_FILON || !SORT_REVEILLER_FILON)
            return;
        bool const talent = j->HasSpell(SORT_MARQUEUR_FILON);
        if (talent && !j->HasSpell(SORT_REVEILLER_FILON))
            j->learnSpell(SORT_REVEILLER_FILON);
        else if (!talent && j->HasSpell(SORT_REVEILLER_FILON))
            j->removeSpell(SORT_REVEILLER_FILON, SPEC_MASK_ALL, false);
    }

    class SourcierJoueurs final : public PlayerScript
    {
    public:
        SourcierJoueurs() : PlayerScript("CoaSourcierJoueurs", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_FORGOT_SPELL,
            PLAYERHOOK_ON_DELETE_FROM_DB, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_BEFORE_LOGOUT }) { }

        // Coeur de foudre en attente (constat 14) : pris sur le fil du joueur. Un atomique lu a chaque mise a jour.
        void OnPlayerUpdate(Player* j, uint32 /*diff*/) override
        {
            ViderRemises(j, "mise_a_jour");
        }

        // Deconnexion avant la prise (WorldSession.cpp:720) : le Coeur part par courrier en base, rien ne reste en file.
        void OnPlayerBeforeLogout(Player* j) override
        {
            ViderRemises(j, "deconnexion");
        }

        void OnPlayerLogin(Player* j) override
        {
            ViderRemises(j, "connexion");
            if (!SORT_MARQUEUR_FILON || !EstHumain(j))
                return;
            SynchroniserSortFilon(j);
            if (j->HasSpell(SORT_REVEILLER_FILON))
                AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
        }

        void OnPlayerLearnSpell(Player* j, uint32 sort) override
        {
            if (SORT_MARQUEUR_FILON && sort == SORT_MARQUEUR_FILON)
                SynchroniserSortFilon(j);
        }

        void OnPlayerForgotSpell(Player* j, uint32 sort) override
        {
            if (SORT_MARQUEUR_FILON && sort == SORT_MARQUEUR_FILON)
                SynchroniserSortFilon(j);
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            if (TableRegistre)
                trans->Append("DELETE FROM coa_artisanat_sourcier WHERE guid = {}", guid);
        }
    };

    class SourcierMonde final : public WorldScript
    {
    public:
        SourcierMonde() : WorldScript("CoaSourcierMonde", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            TableRegistre = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                "AND TABLE_NAME = 'coa_artisanat_sourcier'"));
            ColonneRelanceJcj = TableRegistre && bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE "
                "TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'coa_artisanat_sourcier' AND COLUMN_NAME = 'relance_jcj_jour'"));
            if (TableRegistre && !ColonneRelanceJcj)
                LOG_ERROR("module", "coa sourcier: colonne relance_jcj_jour ABSENTE (migration 2026_10_16_01 non appliquee) : aucune relance "
                    "immediate apres une mort en JcJ, echec ordinaire");
            GameObjectTemplate const* go = GO_FILON_MERE ? sObjectMgr->GetGameObjectTemplate(GO_FILON_MERE) : nullptr;
            bool const scriptOk = go && go->ScriptId == sObjectMgr->GetScriptId(SCRIPT_FILON_MERE);
            GabaritsPresents = scriptOk && PNJ_COLOSSE_ORAGE && sObjectMgr->GetCreatureTemplate(PNJ_COLOSSE_ORAGE);
            LOG_INFO("module", "coa sourcier: registre {} ; sort {} marqueur {} ; filon {} (gabarit {}, script {}, type {}, butin {}) ; colosse {} ({}) ; "
                "coeur de foudre {} ; jour de jeu {} semaine {} prochaine remise {} (Paris)",
                TableRegistre ? "present" : "ABSENT (migration 2026_10_15_01 non appliquee)", SORT_REVEILLER_FILON, SORT_MARQUEUR_FILON,
                GO_FILON_MERE, go ? "present" : "absent", scriptOk ? "ok" : "ABSENT", go ? uint32(go->type) : 0, go ? go->GetLootId() : 0,
                PNJ_COLOSSE_ORAGE, PNJ_COLOSSE_ORAGE && sObjectMgr->GetCreatureTemplate(PNJ_COLOSSE_ORAGE) ? "present" : "absent",
                OBJET_COEUR_DE_FOUDRE, int32(JourDeJeu(Maintenant())), int32(SemaineDeJeu(JourDeJeu(Maintenant()))),
                HeureParis(ProchaineRemise(Maintenant())));
            if (go && go->type != GAMEOBJECT_TYPE_CHEST)
                LOG_ERROR("module", "coa sourcier: le Filon-mere {} n'est pas de type 3 (CHEST) : pas de butin a l'usage", GO_FILON_MERE);
        }
    };

    // ---- .sourcier banc ... (console comprise), royaume PTR seulement ---------------------------------------------------

    constexpr uint32 ROYAUME_PTR = 2;

    class SourcierCommandes final : public CommandScript
    {
    public:
        SourcierCommandes() : CommandScript("CoaSourcierCommandes") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable banc =
            {
                { "jour", HandleJour, SEC_ADMINISTRATOR, Console::Yes },
                { "registre", HandleRegistre, SEC_ADMINISTRATOR, Console::Yes },
                { "tirage", HandleTirage, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable sourcier = { { "banc", banc } };
            static ChatCommandTable racine = { { "sourcier", sourcier } };
            return racine;
        }

        static bool Permis(ChatHandler* handler)
        {
            if (realm.Id.Realm == ROYAUME_PTR)
                return true;
            handler->SendSysMessage("Banc du Sourcier : royaume PTR seulement.");
            return false;
        }

        // Jour et semaine de jeu, prochaine remise.
        static bool HandleJour(ChatHandler* handler)
        {
            if (!Permis(handler))
                return true;
            time_t const m = Maintenant();
            handler->PSendSysMessage("sourcier: maintenant {} (Paris), jour de jeu {}, semaine {}, prochaine remise {} (Paris), decalage {} s",
                HeureParis(m), int32(JourDeJeu(m)), int32(SemaineDeJeu(JourDeJeu(m))), HeureParis(ProchaineRemise(m)), int32(DecalageParisS(m)));
            return true;
        }

        // Registre d'un personnage (lecture seule).
        static bool HandleRegistre(ChatHandler* handler, std::string nom)
        {
            if (!Permis(handler))
                return true;
            ObjectGuid const g = sCharacterCache->GetCharacterGuidByName(nom);
            if (!g)
                return handler->PSendSysMessage("sourcier: {} inconnu.", nom), true;
            Registre const r = LireRegistre(g.GetCounter());
            handler->PSendSysMessage("sourcier: {} (guid {}) : table {} ; jour_lance={} relance_ts={} essai2_jour={} semaine_garanti={} recomp_jour={} recomp_n={} relance_jcj_jour={} ; lancement {}",
                nom, g.GetCounter(), TableRegistre ? "presente" : "ABSENTE", int32(r.jourLance), r.relanceTs, int32(r.essai2Jour),
                int32(r.semaineGaranti), int32(r.recompJour), r.recompN, ColonneRelanceJcj ? std::to_string(r.relanceJcjJour) : std::string("ABSENTE"),
                LancementAccorde(r, Maintenant()) ? "ACCORDE" : "refuse");
            return true;
        }

        // Rejoue la decision du Coeur sur n succes de la meme semaine, a partir du registre de <nom>, SANS rien ecrire.
        static bool HandleTirage(ChatHandler* handler, std::string nom, uint32 n, uint32 comptes)
        {
            if (!Permis(handler))
                return true;
            ObjectGuid const g = sCharacterCache->GetCharacterGuidByName(nom);
            if (!g || !n || n > 100000)
                return handler->SendSysMessage("sourcier: tirage <nom> <succes 1-100000> <comptes humains>"), true;
            Registre r = LireRegistre(g.GetCounter());
            int64 const semaine = SemaineDeJeu(JourDeJeu(Maintenant()));
            uint32 garantis = 0, obtenus = 0;
            for (uint32 i = 0; i < n; ++i)
            {
                DecisionCoeur const d = DeciderCoeur(r, semaine, comptes);
                garantis += d.garanti ? 1 : 0;
                obtenus += d.obtenu ? 1 : 0;
            }
            uint32 const hors = obtenus - garantis;
            handler->PSendSysMessage("sourcier: {} succes simules, {} compte(s) humain(s) : {} garanti(s), {} obtenu(s) au hasard sur {} ({:.1f} %, attendu {} %)",
                n, comptes, garantis, hors, n - garantis, n > garantis ? 100.0 * hors / (n - garantis) : 0.0, CHANCE_COEUR_POUR_CENT);
            return true;
        }
    };
}

void AddAscensionArtisanatSourcierScripts()
{
    new go_coa_filon_mere();
    new SourcierSorts();
    new SourcierUnites();
    new SourcierJoueurs();
    new SourcierMonde();
    new SourcierCommandes();
}
