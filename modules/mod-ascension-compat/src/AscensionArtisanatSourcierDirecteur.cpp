/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T2 du Sourcier au 60 (21/10/2026), lot MINAGE : les mini-evenements de VOIE DIRECTEUR, cote module.
// Conception : coa-project docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2.0 et § 2.1 (branche t2-sourcier).
// NOUVEAU .cpp : `amont/construire.py --cible ptr --configurer` (ou cmake) AVANT le build (glob sans CONFIGURE_DEPENDS).
//
// Regle de construction : le module tient tout ce qui se possede, le Directeur ne fournit que les vagues.
//   1. Lancement (AllSpellScript) : garde (combat, instance, dehors, sec, capitale ou sanctuaire, registre commun du
//      Sourcier, autre mini-evenement a RAYON_EXCLUSIF), puis l'OBJET PIVOT de l'evenement est pose sous les pieds du
//      lanceur ; son IA porte toute la session, sur le fil de sa carte (P-137), comme le Filon-mere.
//   2. Demande au Directeur : ecrite par mod-playerbots, qui voit LUI-MEME le lancement reussi (CoaDirecteurSourcier.inc).
//      Aucun lien de code : ce module ne lit jamais l'etat du Directeur.
//   3. Menace : le pivot ADOPTE (marque) toute creature des gabarits reserves (menace et chef de l'evenement) qui parait
//      a RAYON_ADOPTION de lui et n'appartient pas deja a un autre pivot. Les invocations du Directeur n'ont pas
//      d'invocateur (Map::SummonCreature sans proprietaire, CoaDirecteur.cpp) : la proximite est le seul lien, et les
//      gabarits 9105010-9105049 ne servent qu'au Sourcier.
//   4. Refus du Directeur (service eteint ou en pause, place prise, option du jeu a 0, demande perdue) : AUCUNE creature
//      adoptee en ATTENTE_DEBUT_S -> pivot retire, lancement RENDU (champs de lancement du registre remis a leur valeur
//      d'avant), message. Un service arrete coute 3 minutes au joueur, pas sa journee.
//   5. Victoire : la mort du CHEF adopte (gabarit reserve, crochet OnUnitDeath, ou constat a la seconde suivante).
//      Recompenses : materiau legendaire au seul specialiste (garanti hebdomadaire a COMPTES_HUMAINS_GARANTI comptes,
//      sinon CHANCE_COEUR_POUR_CENT : registre et regle du Filon-mere), recompenses de participant (2 par jour, le meme
//      registre), et pour la Faille tellurique 3 Geodes de foudre que tout le groupe peut miner.
//      Lot DEPECAGE (21/10) : l'Appel du Linceul (sort 9113905, pivot 9101094, morts 9105040-9105042, Roi spectral 9105043,
//      Suaire des rois 9101283) : garde de plus, de nuit (HEURE_LINCEUL_DEBUT-FIN, Paris) et a RAYON_CIMETIERE d'un point
//      d'ame ; recompenses de participant du Glas ; le Suaire part au courrier si le specialiste a quitte la carte.
//   6. Echec : echeance, groupe entier mort, specialiste tue par un monstre et absent ABSENCE_MAX_S, deconnexion, chef
//      disparu vivant : regles du Filon-mere (relance a 4 h une fois par jour ; immediate une fois par jour si un humain
//      adverse a tue le specialiste). Le module ne retire AUCUNE creature du Directeur (un seul proprietaire, P-134) :
//      elles s'eteignent a l'echeance du service.
// Journal : « coa sourcier: ... evenement=<code> pivot=<guid> ... » (lancement, menace, adoption chef, rendu, succes, echec,
// remise, recompense, geodes, nettoyage).
//
// INERTE tant que ses donnees manquent (gabarit du pivot avec ce ScriptName, chef, menace en base ; table du registre) :
// le sort est refuse avec « pas encore disponible », rien n'apparait. Journal au demarrage : « coa sourcier: directeur ».

#include "AscensionArtisanatIds.h"
#include "AscensionArtisanatSourcierSocle.h"
#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameGraveyard.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <map>
#include <set>
#include <vector>

using namespace CoaArtisanatIds;
using namespace CoaSourcier;

namespace
{
    // ---- La table des mini-evenements de voie Directeur ---------------------------------------------------------------
    //
    // Lot MINAGE : la Faille tellurique. Lot HERBORISTERIE : le Jardin d'orage (sans chef) est a part
    // (AscensionArtisanatSourcierHerboristerie.cpp). Lot DEPECAGE (21/10) : l'Appel du Linceul, garde d'heure
    // (HEURE_LINCEUL_DEBUT-FIN, Paris) et cimetiere a RAYON_CIMETIERE (`nuitCimetiere`), recompenses du Glas.
    struct DefDirecteur
    {
        char const* code;                       // texte de la demande (mod-playerbots) et journal
        uint32 sort;                            // sort LANCE
        uint32 marqueur;                        // marqueur du talent : le sort est appris a qui le connait
        uint32 pivot;                           // gameobject_template de l'objet pivot
        char const* script;                     // son ScriptName
        std::array<uint32, 3> menace;           // gabarits de vague (0 : vide)
        uint32 chef;                            // gabarit du chef : sa mort = victoire
        uint32 dureeS;                          // echeance de la session (presage et marge compris)
        uint32 materiau;                        // materiau legendaire du specialiste
        uint32 verte, rare, epique;             // recompense de participant (lot DEPECAGE : Orage pour la Faille, Glas)
        bool nuitCimetiere;                     // lot DEPECAGE : de nuit (Paris) et pres d'un cimetiere seulement
        char const* expediteur;                 // courrier quand les sacs sont pleins
        char const* messageRendu;               // refus du Directeur : lancement rendu
        char const* messageSucces;              // au specialiste, avec le materiau
        char const* messageSansMateriau;        // au specialiste, succes sans materiau (hors garanti, tirage perdu)
        char const* messageEchec;
        char const* messageRelance;             // echec cause par un humain adverse : relance immediate
    };

    constexpr std::array<DefDirecteur, 2> EVENEMENTS =
    {{
        { "faille_tellurique", SORT_OUVRIR_FAILLE_TELLURIQUE, SORT_MARQUEUR_FAILLE_TELLURIQUE, GO_FAILLE_TELLURIQUE,
          SCRIPT_FAILLE_TELLURIQUE, PNJ_MENACE_TELLURIQUE, PNJ_GARDIEN_TELLURIQUE, DUREE_TELLURIQUE_S, OBJET_COEUR_DE_FOUDRE,
          RESSOURCE_VERTE, RESSOURCE_RARE, RESSOURCE_EPIQUE, false,
          "La Faille tellurique",
          "Les forces de la terre ne répondent pas : réessayez plus tard.",
          "La faille se referme et vous livre un Cœur de foudre.",
          "La faille se referme.",
          "La faille tellurique se referme dans un grondement.",
          "La faille se referme : vous pourrez la rouvrir tout de suite." },
        // Lot DEPECAGE (21/10) : l'Appel du Linceul (conception § 2.3).
        { "appel_linceul", SORT_DEPLOYER_LINCEUL, SORT_MARQUEUR_LINCEUL, GO_LINCEUL_ANCIEN, SCRIPT_LINCEUL_ANCIEN,
          PNJ_MENACE_LINCEUL, PNJ_ROI_SPECTRAL, DUREE_LINCEUL_S, OBJET_SUAIRE_ROIS,
          RESSOURCE_GLAS_VERTE, RESSOURCE_GLAS_RARE, RESSOURCE_GLAS_EPIQUE, true,
          "L'Appel du Linceul",
          "Les morts n'entendent pas l'appel : réessayez plus tard.",
          "Le Roi spectral retourne à la poussière et vous laisse le Suaire des rois.",
          "Le Roi spectral retourne à la poussière.",
          "Le Linceul ancien se déchire.",
          "Le Linceul ancien se déchire : vous pourrez le déployer de nouveau tout de suite." },
    }};

    std::array<bool, EVENEMENTS.size()> Prets{};   // donnees presentes, lu au demarrage (fil monde, avant tout sort)
    bool GeodesPresentes = false;

    DefDirecteur const* DefParSort(uint32 sort)
    {
        for (DefDirecteur const& d : EVENEMENTS)
            if (d.sort && d.sort == sort)
                return &d;
        return nullptr;
    }

    DefDirecteur const* DefParPivot(uint32 entree)
    {
        for (DefDirecteur const& d : EVENEMENTS)
            if (d.pivot && d.pivot == entree)
                return &d;
        return nullptr;
    }

    bool EstPret(DefDirecteur const* d)
    {
        return d && Prets[std::size_t(d - EVENEMENTS.data())];
    }

    bool EstChefConnu(uint32 entree)
    {
        for (DefDirecteur const& d : EVENEMENTS)
            if (d.chef && d.chef == entree)
                return true;
        return false;
    }

    // Lot DEPECAGE : courrier en BASE (MailReceiver par GUID bas) d'un materiau legendaire autre que le Coeur de foudre
    // (qui a sa propre file, PosterCoeurs) au specialiste absent de la carte du pivot a la victoire. Vu a la connexion.
    bool PosterMateriau(ObjectGuid destinataire, uint32 entree, char const* expediteur)
    {
        if (!entree || !sObjectMgr->GetItemTemplate(entree))
            return false;
        Item* objet = Item::CreateItem(entree, 1, nullptr);
        if (!objet)
            return false;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft courrier(expediteur, "Votre récolte légendaire.");
        objet->SetGuidValue(ITEM_FIELD_CREATOR, destinataire);
        objet->SaveToDB(trans);
        courrier.AddItem(objet);
        courrier.SendMailTo(trans, MailReceiver(destinataire.GetCounter()), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    // Lot DEPECAGE : l'heure de Paris (0-23) de `utc` (DecalageParisS : regle europeenne, independante du fuseau du serveur).
    uint32 HeureDeParis(time_t utc)
    {
        int64 const local = int64(utc) + DecalageParisS(utc);
        return uint32(((local % 86400) + 86400) % 86400 / 3600);
    }

    bool DeNuit(time_t utc)
    {
        uint32 const h = HeureDeParis(utc);
        return HEURE_LINCEUL_DEBUT > HEURE_LINCEUL_FIN ? (h >= HEURE_LINCEUL_DEBUT || h < HEURE_LINCEUL_FIN)
                                                       : (h >= HEURE_LINCEUL_DEBUT && h < HEURE_LINCEUL_FIN);
    }

    // Un point d'ame (game_graveyard, toutes factions) de la carte du joueur a RAYON_CIMETIERE au plus (meme garde que la
    // Floraison des tombes ; table chargee au demarrage, lue seulement).
    bool PresDUnCimetiere(Player const* j)
    {
        for (auto const& [id, c] : sGraveyard->GetGraveyardData())
            if (c.Map == j->GetMapId() && j->GetExactDist2d(c.x, c.y) <= RAYON_CIMETIERE)
                return true;
        return false;
    }

    // ---- La session, dans l'IA de l'objet pivot ----------------------------------------------------------------------

    class SessionDirecteurAI final : public GameObjectAI
    {
    public:
        explicit SessionDirecteurAI(GameObject* go) : GameObjectAI(go), _def(DefParPivot(go->GetEntry())) { }

        ~SessionDirecteurAI() override
        {
            // Carte dechargee, despawn du filet : l'exclusivite ne survit pas au pivot.
            RetirerActif(_pivot);
        }

        enum class Phase : uint8 { Attente, Appel, Combat, Fini };

        // Fil de la carte, au lancement. `avant` : le registre AVANT ce lancement (pour le rendre au refus du Directeur).
        void Demarrer(Player* specialiste, Registre const& avant)
        {
            _pivot = me->GetGUID();
            _specialiste = specialiste->GetGUID();
            _equipe = specialiste->GetTeamId();
            _debut = Maintenant();
            _groupe = specialiste->GetGroup() ? specialiste->GetGroup()->GetGUID() : ObjectGuid::Empty;
            _avantJour = avant.jourLance;
            _avantRelance = avant.relanceTs;
            _phase = Phase::Appel;
            Marquer(specialiste, _pivot);
            InscrireActif(me);
            LOG_INFO("module", "coa sourcier: lancement evenement={} specialiste={} pivot={} carte={} x={:.1f} y={:.1f} attente_max_s={} duree_s={}",
                Code(), _specialiste.GetCounter(), _pivot.GetCounter(), me->GetMapId(), me->GetPositionX(), me->GetPositionY(),
                ATTENTE_DEBUT_S, _def ? _def->dureeS : 0);
        }

        bool GossipHello(Player* /*joueur*/, bool /*reportUse*/) override
        {
            return true;   // decor : rien ne s'ouvre
        }

        void UpdateAI(uint32 diff) override
        {
            if (_phase == Phase::Attente || _phase == Phase::Fini)
                return;
            _cumulAdoption += diff;
            if (_cumulAdoption >= ADOPTION_PERIODE_MS)
            {
                _cumulAdoption = 0;
                Adopter();
                if (_phase == Phase::Fini)
                    return;
            }
            _cumul += diff;
            if (_cumul < 1000)
                return;
            _cumul = 0;
            Surveiller();
        }

        // ---- Appels des crochets d'unite (meme carte) ----

        void Crediter(Player* joueur, uint32 montant)
        {
            if (!joueur || _phase != Phase::Combat)
                return;
            _credits[joueur->GetGUID()] += montant;
            Marquer(joueur, _pivot);
        }

        void SpecialisteMort(Player const* tueur)
        {
            bool const humain = EstHumain(tueur);
            bool const adverse = tueur && tueur->GetTeamId() != _equipe;
            _specialisteMortA = Maintenant();
            _mortParJoueur = humain && adverse;
            LOG_INFO("module", "coa sourcier: specialiste mort evenement={} pivot={} specialiste={} tueur_joueur={} humain={} adverse={} par_joueur={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), tueur ? tueur->GetGUID().GetCounter() : 0, humain ? 1 : 0,
                adverse ? 1 : 0, _mortParJoueur ? 1 : 0);
        }

        void ChefMort()
        {
            if (_phase == Phase::Combat)
                Reussir();
        }

        bool EstChef(ObjectGuid g) const { return g && g == _chef; }
        bool EstSpecialiste(ObjectGuid g) const { return g && g == _specialiste; }

    private:
        char const* Code() const { return _def ? _def->code : "inconnu"; }

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

        // Toutes les ADOPTION_PERIODE_MS : les creatures des gabarits reserves de l'evenement a RAYON_ADOPTION du pivot.
        // Seule la carte du pivot est lue (P-137). Une creature deja marquee pour un AUTRE pivot est laissee.
        void Adopter()
        {
            if (!_def)
                return;
            std::vector<uint32> entrees;
            for (uint32 e : _def->menace)
                if (e)
                    entrees.push_back(e);
            if (_def->chef)
                entrees.push_back(_def->chef);
            std::list<Creature*> trouvees;
            me->GetCreatureListWithEntryInGrid(trouvees, entrees, RAYON_ADOPTION);
            for (Creature* c : trouvees)
            {
                ObjectGuid const p = PivotDe(c);
                if (p == _pivot)
                    continue;
                // Un cadavre non adopte n'est pris que s'il est un chef (tue entre deux recherches) ; une creature d'un
                // autre pivot, jamais.
                if (p || (!c->IsAlive() && c->GetEntry() != _def->chef))
                    continue;
                Marquer(c, _pivot);
                ++_adoptees;
                if (c->GetEntry() == _def->chef && !_chef)
                {
                    _chef = c->GetGUID();
                    LOG_INFO("module", "coa sourcier: chef evenement={} pivot={} chef={} entree={} pv={} vivant={}", Code(),
                        _pivot.GetCounter(), _chef.GetCounter(), c->GetEntry(), c->GetMaxHealth(), c->IsAlive() ? 1 : 0);
                    _seuilParticipation = std::max<uint32>(1, uint32(uint64(c->GetMaxHealth()) * PART_PARTICIPATION_POUR_MILLE / 1000));
                }
                if (_phase == Phase::Appel)
                {
                    _phase = Phase::Combat;
                    LOG_INFO("module", "coa sourcier: menace evenement={} pivot={} premiere={} apres_s={}", Code(), _pivot.GetCounter(),
                        c->GetEntry(), uint32(Maintenant() - _debut));
                }
            }
            if (_phase == Phase::Combat && _chef)
                if (Creature* c = ObjectAccessor::GetCreature(*me, _chef))
                    if (!c->IsAlive())
                        Reussir();
        }

        // Une fois par seconde.
        void Surveiller()
        {
            time_t const maintenant = Maintenant();
            Player* s = ObjectAccessor::GetPlayer(*me, _specialiste);
            if (_phase == Phase::Appel)
            {
                if (!s && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                    return Rendre("deconnexion");
                if (maintenant - _debut >= time_t(ATTENTE_DEBUT_S))
                    return Rendre("directeur_absent");
                return;
            }
            if (!s && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                return Echouer("deconnexion", false);
            bool const present = s && s->IsAlive() && s->IsWithinDist(me, RAYON_PARTICIPATION);
            if (s && s->GetGroup())
                _groupe = s->GetGroup()->GetGUID();
            if (present)
            {
                _specialisteMortA = 0;
                _mortParJoueur = false;
            }
            else if (_specialisteMortA && !_mortParJoueur && maintenant - _specialisteMortA > time_t(ABSENCE_MAX_S))
                return Echouer("specialiste", false);
            if (!present && !GroupeEnVie())
                return Echouer("groupe", _mortParJoueur);
            if (maintenant - _debut >= time_t(_def ? _def->dureeS : 0))
                return Echouer("temps", false);
            if (_chef)
            {
                Creature* c = ObjectAccessor::GetCreature(*me, _chef);
                if (!c)
                    return Echouer("chef_disparu", false);   // retire vivant (fermeture du Directeur) : pas de victoire
                if (!c->IsAlive())
                    return Reussir();
            }
        }

        bool GroupeEnVie()
        {
            for (MapReference const& ref : me->GetMap()->GetPlayers())
                if (Player* m = ref.GetSource())
                {
                    if (!m->IsAlive())
                        continue;
                    if (m->GetGUID() == _specialiste)
                        return true;
                    if (_groupe && m->GetGroup() && m->GetGroup()->GetGUID() == _groupe && m->IsWithinDist(me, RAYON_PARTICIPATION))
                        return true;
                }
            return false;
        }

        void Reussir()
        {
            if (_phase != Phase::Combat)
                return;
            _phase = Phase::Fini;   // une seule fois, meme si le crochet de mort et le constat arrivent dans la meme seconde
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            int64 const semaine = SemaineDeJeu(jour);

            // Participants humains de la faction : groupe du specialiste present a 100 m, plus les credites (2 % des PV
            // du chef, infliges ou soignes).
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
            LOG_INFO("module", "coa sourcier: succes evenement={} pivot={} specialiste={} participants_humains={} comptes={} adoptees={} duree_s={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), uint32(humains.size()), uint32(comptes.size()), _adoptees,
                uint32(maintenant - _debut));

            // Le materiau legendaire, au seul specialiste.
            uint32 const materiau = _def ? _def->materiau : 0;
            Registre r = LireRegistre(_specialiste.GetCounter());
            DecisionCoeur const d = DeciderCoeur(r, semaine, uint32(comptes.size()));
            EcrireRegistre(_specialiste.GetCounter(), r);
            bool remis = false;
            char const* voie = "aucune";
            if (d.obtenu && materiau)
            {
                if (s)
                {
                    remis = Donner(s, materiau, 1, s->GetGUID(), _def->expediteur);
                    voie = "sacs";
                }
                else if (materiau == OBJET_COEUR_DE_FOUDRE && ObjectAccessor::FindConnectedPlayer(_specialiste))
                {
                    MettreEnFile(_specialiste);   // videe sur le fil du joueur (AscensionArtisanatSourcier.cpp, ViderRemises)
                    remis = true;
                    voie = "file";
                }
                else if (materiau == OBJET_COEUR_DE_FOUDRE)
                {
                    remis = PosterCoeurs(_specialiste, 1);
                    voie = "courrier";
                }
                else   // lot DEPECAGE : Suaire des rois, specialiste hors de la carte
                {
                    remis = PosterMateriau(_specialiste, materiau, _def->expediteur);
                    voie = "courrier";
                }
            }
            LOG_INFO("module", "coa sourcier: remise evenement={} pivot={} specialiste={} materiau={} garanti={} obtenu={} remis={} voie={} semaine={} comptes={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), materiau, d.garanti ? 1 : 0, d.obtenu ? 1 : 0, remis ? 1 : 0, voie,
                int32(semaine), uint32(comptes.size()));
            if (s)
                ChatHandler(s->GetSession()).SendNotification(!_def ? "Le mini-événement prend fin." : d.obtenu ? _def->messageSucces
                    : _def->messageSansMateriau);

            // Recompenses de participant (registre commun : RECOMPENSES_PAR_JOUR par jour, tous mini-evenements confondus).
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
                    LOG_INFO("module", "coa sourcier: recompense refusee evenement={} pivot={} joueur={} raison=plafond_du_jour", Code(),
                        _pivot.GetCounter(), p->GetGUID().GetCounter());
                    continue;
                }
                ++rp.recompN;
                EcrireRegistre(p->GetGUID().GetCounter(), rp);
                if (p->GetGUID() == _specialiste)
                    r = rp;
                uint32 const rare = urand(1, 100) <= CHANCE_RARE_POUR_CENT ? 1 : 0;
                uint32 const epique = urand(1, 1000) <= CHANCE_EPIQUE_POUR_MILLE ? 1 : 0;
                char const* exp = _def ? _def->expediteur : "Le Sourcier";
                Donner(p, _def ? _def->verte : RESSOURCE_VERTE, RESSOURCES_VERTES, ObjectGuid::Empty, exp);
                if (rare)
                    Donner(p, _def ? _def->rare : RESSOURCE_RARE, 1, ObjectGuid::Empty, exp);
                if (epique)
                    Donner(p, _def ? _def->epique : RESSOURCE_EPIQUE, 1, ObjectGuid::Empty, exp);
                LOG_INFO("module", "coa sourcier: recompense evenement={} pivot={} joueur={} vertes={} rare={} epique={} du_jour={}/{}", Code(),
                    _pivot.GetCounter(), p->GetGUID().GetCounter(), RESSOURCES_VERTES, rare, epique, rp.recompN, RECOMPENSES_PAR_JOUR);
            }

            if (_def && _def->pivot == GO_FAILLE_TELLURIQUE)
                PoserGeodes();
            Terminer("succes");
        }

        // Faille tellurique : 3 Geodes de foudre (gabarit existant, butin ordinaire, verrou de Minage) autour du pivot,
        // GEODES_VIE_S. Les bots ne les visent pas (plage 9101000-9101099, LootObjectStack.cpp).
        void PoserGeodes()
        {
            if (!GeodesPresentes)
            {
                LOG_ERROR("module", "coa sourcier: geodes {} absentes en base : non posees pivot={}", GO_GEODES_FOUDRE, _pivot.GetCounter());
                return;
            }
            uint32 posees = 0;
            float const depart = float(rand_norm()) * 2.0f * float(M_PI);
            for (uint32 k = 0; k < GEODES_NOMBRE; ++k)
            {
                Position pos = me->GetPosition();
                me->MovePosition(pos, GEODES_ECART, depart + float(k) * 2.0f * float(M_PI) / float(GEODES_NOMBRE));
                if (me->GetMap()->SummonGameObject(GO_GEODES_FOUDRE, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                        pos.GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, GEODES_VIE_S))
                    ++posees;
            }
            LOG_INFO("module", "coa sourcier: geodes pivot={} posees={}/{} vie_s={}", _pivot.GetCounter(), posees, GEODES_NOMBRE, GEODES_VIE_S);
        }

        // Echec : rien de legendaire ; relance immediate une fois par jour si un humain adverse l'a cause, sinon au bout de
        // 4 h une fois par jour (regle du Filon-mere, D-PO-51).
        void Echouer(char const* raison, bool parJoueur)
        {
            if (_phase == Phase::Fini)
                return;
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            Registre r = LireRegistre(_specialiste.GetCounter());
            char const* relance = "aucune";
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
                if (_def)
                    ChatHandler(s->GetSession()).SendNotification(immediate ? _def->messageRelance : _def->messageEchec);
            }
            LOG_INFO("module", "coa sourcier: echec evenement={} pivot={} specialiste={} raison={} par_joueur={} relance={} adoptees={} chef={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), raison, parJoueur ? 1 : 0, relance, _adoptees, _chef ? 1 : 0);
            Terminer(raison);
        }

        // Refus du Directeur : aucune menace. Les deux champs de LANCEMENT du registre reprennent leur valeur d'avant ce
        // lancement (le reste, relu, n'est pas touche : un participant a pu y ecrire entre-temps).
        void Rendre(char const* raison)
        {
            if (_phase == Phase::Fini)
                return;
            Registre r = LireRegistre(_specialiste.GetCounter());
            r.jourLance = _avantJour;
            r.relanceTs = _avantRelance;
            EcrireRegistre(_specialiste.GetCounter(), r);
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
            {
                AfficherRecharge(s, r);
                if (_def)
                    ChatHandler(s->GetSession()).SendNotification(_def->messageRendu);
            }
            LOG_INFO("module", "coa sourcier: rendu evenement={} pivot={} specialiste={} raison={} attente_s={}", Code(), _pivot.GetCounter(),
                _specialiste.GetCounter(), raison, uint32(Maintenant() - _debut));
            Terminer(raison);
        }

        void Terminer(char const* raison)
        {
            _phase = Phase::Fini;
            RetirerActif(_pivot);
            LOG_INFO("module", "coa sourcier: nettoyage evenement={} pivot={} raison={}", Code(), _pivot.GetCounter(), raison);
            me->SetRespawnTime(0);
            me->Delete();
        }

        DefDirecteur const* _def = nullptr;
        Phase _phase = Phase::Attente;
        ObjectGuid _pivot, _specialiste, _groupe, _chef;
        TeamId _equipe = TEAM_NEUTRAL;
        time_t _debut = 0, _specialisteMortA = 0;
        int64 _avantJour = 0;
        uint32 _avantRelance = 0;
        bool _mortParJoueur = false;
        uint32 _cumul = 0, _cumulAdoption = 0, _adoptees = 0, _seuilParticipation = 1;
        std::map<ObjectGuid, uint32> _credits;
    };

    SessionDirecteurAI* SessionDe(Unit const* u)
    {
        ObjectGuid const p = PivotDe(u);
        if (!p)
            return nullptr;
        GameObject* go = ObjectAccessor::GetGameObject(*u, p);
        return go ? dynamic_cast<SessionDirecteurAI*>(go->AI()) : nullptr;
    }

    class go_coa_sourcier_directeur final : public GameObjectScript
    {
    public:
        explicit go_coa_sourcier_directeur(char const* nom) : GameObjectScript(nom) { }

        GameObjectAI* GetAI(GameObject* go) const override
        {
            return new SessionDirecteurAI(go);
        }
    };

    // ---- Le sort lance ---------------------------------------------------------------------------------------------------

    // Vide si le lancement est permis ; sinon le message montre au joueur.
    std::string RefusLancement(Player* j, DefDirecteur const* d, std::string& code)
    {
        auto refus = [&](char const* c, std::string m) { code = c; return m; };
        if (!EstPret(d) || !TableRegistre)
            return refus("inerte", "Ce mini-événement n'est pas encore disponible.");
        if (j->IsInCombat())
            return refus("combat", "Impossible en combat.");
        Map* carte = j->GetMap();
        if (!carte || carte->Instanceable() || carte->IsBattlegroundOrArena())
            return refus("instance", "Impossible en instance, en champ de bataille ou en arène.");
        if (!j->IsOutdoors())
            return refus("interieur", "Il faut être dehors.");
        if (j->IsFlying() || j->IsInWater())
            return refus("sol", "Il faut avoir les pieds sur la terre ferme.");
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(j->GetZoneId());
        AreaTableEntry const* aire = sAreaTableStore.LookupEntry(j->GetAreaId());
        uint32 const drapeaux = (zone ? zone->flags : 0) | (aire ? aire->flags : 0);
        if ((drapeaux & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY)) || j->IsInSanctuary())
            return refus("sanctuaire", "Impossible dans une capitale ou un sanctuaire.");
        time_t const maintenant = Maintenant();
        if (d->nuitCimetiere && !DeNuit(maintenant))
            return refus("heure", Acore::StringFormat("Le Linceul ne se déploie que la nuit, de {}:00 à {:02}:00 (heure de Paris).",
                HEURE_LINCEUL_DEBUT, HEURE_LINCEUL_FIN));
        if (d->nuitCimetiere && !PresDUnCimetiere(j))
            return refus("cimetiere", "Le Linceul ne se déploie que près d'un cimetière.");
        Registre const r = LireRegistre(j->GetGUID().GetCounter());
        if (!LancementAccorde(r, maintenant))
        {
            time_t fin = ProchaineRemise(maintenant);
            if (r.relanceTs && time_t(r.relanceTs) > maintenant && time_t(r.relanceTs) < fin)
                fin = time_t(r.relanceTs);
            return refus("deja", Acore::StringFormat("Vous avez déjà lancé un mini-événement du Sourcier aujourd'hui. De nouveau "
                "possible à {} (heure de Paris).", HeureParis(fin)));
        }
        if (EvenementProche(j->GetMapId(), j->GetInstanceId(), j->GetPositionX(), j->GetPositionY()))
            return refus("voisin", "Un autre mini-événement gronde déjà tout près.");
        return {};
    }

    class SourcierDirecteurSorts final : public AllSpellScript
    {
    public:
        SourcierDirecteurSorts() : AllSpellScript("CoaSourcierDirecteurSorts", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCheckCast(Spell* sort, bool /*strict*/, SpellCastResult& res) override
        {
            if (res != SPELL_CAST_OK || !sort)
                return;
            DefDirecteur const* d = DefParSort(sort->GetSpellInfo()->Id);
            if (!d)
                return;
            Player* j = sort->GetCaster() ? sort->GetCaster()->ToPlayer() : nullptr;
            if (!j)
            {
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
            std::string code;
            std::string const message = RefusLancement(j, d, code);
            if (message.empty())
                return;
            res = SPELL_FAILED_DONT_REPORT;
            ChatHandler(j->GetSession()).SendNotification(message);
            LOG_INFO("module", "coa sourcier: refus evenement={} joueur={} raison={}", d->code, j->GetGUID().GetCounter(), code);
        }

        void OnSpellCast(Spell* /*sort*/, Unit* lanceur, SpellInfo const* info, bool /*skipCheck*/) override
        {
            DefDirecteur const* d = info ? DefParSort(info->Id) : nullptr;
            if (!d)
                return;
            Player* j = lanceur ? lanceur->ToPlayer() : nullptr;
            if (!j)
                return;
            std::string code;
            if (!RefusLancement(j, d, code).empty())
                return;   // deja refuse par CheckCast ; un lancement force qui ne passe pas la garde ne fait rien
            Registre const avant = LireRegistre(j->GetGUID().GetCounter());
            GameObject* pivot = j->GetMap()->SummonGameObject(d->pivot, j->GetPositionX(), j->GetPositionY(), j->GetPositionZ(),
                j->GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, d->dureeS + 120);
            SessionDirecteurAI* ai = pivot ? dynamic_cast<SessionDirecteurAI*>(pivot->AI()) : nullptr;
            if (!ai)
            {
                // Rien n'est consomme, le lancement reste accorde. La demande au Directeur, elle, est peut-etre deja
                // partie (mod-playerbots la voit au meme crochet) : ses vagues viendraient sans session, sans recompense.
                LOG_ERROR("module", "coa sourcier: pivot {} non pose ou sans le script {} : lancement rendu evenement={}", d->pivot,
                    d->script, d->code);
                if (pivot)
                {
                    pivot->SetRespawnTime(0);
                    pivot->Delete();
                }
                ChatHandler(j->GetSession()).SendNotification("Le sol ne répond pas.");
                AfficherRecharge(j, avant);
                return;
            }
            Registre r = avant;
            r.jourLance = JourDeJeu(Maintenant());
            r.relanceTs = 0;
            EcrireRegistre(j->GetGUID().GetCounter(), r);
            ai->Demarrer(j, avant);
            AfficherRecharge(j, r);
        }
    };

    // ---- Crochets d'unite : credits sur le chef, morts ------------------------------------------------------------------

    class SourcierDirecteurUnites final : public UnitScript
    {
    public:
        SourcierDirecteurUnites() : UnitScript("CoaSourcierDirecteurUnites", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH }) { }

        void OnDamage(Unit* attaquant, Unit* victime, uint32& degats) override
        {
            if (!degats || !victime || !victime->IsCreature() || !EstChefConnu(victime->GetEntry()))
                return;
            Player* joueur = attaquant ? attaquant->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (SessionDirecteurAI* ai = joueur ? SessionDe(victime) : nullptr)
                if (ai->EstChef(victime->GetGUID()))
                    ai->Crediter(joueur, std::min(degats, uint32(victime->GetHealth())));
        }

        void OnHeal(Unit* soigneur, Unit* soigne, uint32& gain) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !gain || !soigneur || !soigne || !soigne->IsPlayer())
                return;
            Player* joueur = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (SessionDirecteurAI* ai = joueur ? SessionDe(soigne) : nullptr)
                ai->Crediter(joueur, gain);
        }

        void OnUnitDeath(Unit* mort, Unit* tueur) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !mort)
                return;
            SessionDirecteurAI* ai = SessionDe(mort);
            if (!ai)
                return;
            if (mort->IsCreature() && ai->EstChef(mort->GetGUID()))
                ai->ChefMort();
            else if (mort->IsPlayer() && ai->EstSpecialiste(mort->GetGUID()))
            {
                Player const* joueur = (tueur && tueur != mort) ? tueur->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                ai->SpecialisteMort(joueur && joueur != mort ? joueur : nullptr);
            }
        }
    };

    // ---- Le talent apprend le sort ----------------------------------------------------------------------------------------

    void Synchroniser(Player* j, DefDirecteur const& d)
    {
        if (!d.marqueur || !d.sort)
            return;
        bool const talent = j->HasSpell(d.marqueur);
        if (talent && !j->HasSpell(d.sort))
            j->learnSpell(d.sort);
        else if (!talent && j->HasSpell(d.sort))
            j->removeSpell(d.sort, SPEC_MASK_ALL, false);
    }

    class SourcierDirecteurJoueurs final : public PlayerScript
    {
    public:
        SourcierDirecteurJoueurs() : PlayerScript("CoaSourcierDirecteurJoueurs", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEARN_SPELL,
            PLAYERHOOK_ON_FORGOT_SPELL }) { }

        void OnPlayerLogin(Player* j) override
        {
            if (!EstHumain(j))
                return;
            bool connu = false;
            for (DefDirecteur const& d : EVENEMENTS)
            {
                Synchroniser(j, d);
                connu = connu || (d.sort && j->HasSpell(d.sort));
            }
            if (connu)
                AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
        }

        void OnPlayerLearnSpell(Player* j, uint32 sort) override
        {
            for (DefDirecteur const& d : EVENEMENTS)
                if (d.marqueur && sort == d.marqueur)
                {
                    Synchroniser(j, d);
                    if (EstHumain(j) && j->HasSpell(d.sort))
                        AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
                }
        }

        void OnPlayerForgotSpell(Player* j, uint32 sort) override
        {
            for (DefDirecteur const& d : EVENEMENTS)
                if (d.marqueur && sort == d.marqueur)
                    Synchroniser(j, d);
        }
    };

    class SourcierDirecteurMonde final : public WorldScript
    {
    public:
        SourcierDirecteurMonde() : WorldScript("CoaSourcierDirecteurMonde", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            GeodesPresentes = GO_GEODES_FOUDRE && sObjectMgr->GetGameObjectTemplate(GO_GEODES_FOUDRE);
            for (std::size_t i = 0; i < EVENEMENTS.size(); ++i)
            {
                DefDirecteur const& d = EVENEMENTS[i];
                GameObjectTemplate const* go = d.pivot ? sObjectMgr->GetGameObjectTemplate(d.pivot) : nullptr;
                bool const scriptOk = go && go->ScriptId == sObjectMgr->GetScriptId(d.script);
                bool const chef = d.chef && sObjectMgr->GetCreatureTemplate(d.chef);
                uint32 menace = 0, attendues = 0;
                for (uint32 e : d.menace)
                    if (e)
                    {
                        ++attendues;
                        menace += sObjectMgr->GetCreatureTemplate(e) ? 1 : 0;
                    }
                bool const sort = d.sort && sSpellMgr->GetSpellInfo(d.sort);
                Prets[i] = scriptOk && chef && menace == attendues && sort && d.materiau && sObjectMgr->GetItemTemplate(d.materiau);
                LOG_INFO("module", "coa sourcier: directeur evenement={} sort {} ({}) marqueur {} ; pivot {} (gabarit {}, script {}) ; chef {} ({}) ; "
                    "menace {}/{} ; materiau {} ; geodes {} ({}) ; {}", d.code, d.sort, sort ? "present" : "ABSENT", d.marqueur, d.pivot,
                    go ? "present" : "absent", scriptOk ? "ok" : "ABSENT", d.chef, chef ? "present" : "absent", menace, attendues, d.materiau,
                    GO_GEODES_FOUDRE, GeodesPresentes ? "presentes" : "absentes", Prets[i] ? "PRET" : "INERTE");
            }
        }
    };
}

void AddAscensionArtisanatSourcierDirecteurScripts()
{
    for (DefDirecteur const& d : EVENEMENTS)
        new go_coa_sourcier_directeur(d.script);
    new SourcierDirecteurSorts();
    new SourcierDirecteurUnites();
    new SourcierDirecteurJoueurs();
    new SourcierDirecteurMonde();
}
