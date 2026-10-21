/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T2 du Sourcier au 60 (21/10/2026), lot HERBORISTERIE : Jardin d'orage et Floraison des tombes.
// Conception : coa-project docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2.2 (branche t2-sourcier). Ids :
// AscensionArtisanatIds.h, section « lot HERBORISTERIE ». Socle commun (registre, remises, marques, exclusivite) :
// AscensionArtisanatSourcierSocle.h. NOUVEAU .cpp : `amont/construire.py --cible ptr --configurer` (ou cmake) AVANT le
// build (glob sans CONFIGURE_DEPENDS). Les deux evenements sont dans un seul .cpp (la conception en prevoyait deux) : ils
// partagent la session commune (participants, remise du materiau, recompenses, echec, relance).
//
// JARDIN D'ORAGE (voie module + Directeur). Le sort 9113901 plante la Graine-tempete (creature 9105020, pivot) sous le
// lanceur ; son IA porte la session, sur le fil de sa carte (P-137) :
//   - JAUGE en pour mille, montree par les PV de la Graine (la cibler affiche la jauge) et dite au groupe toutes les
//     JAUGE_ANNONCE_S : depart JAUGE_DEPART, +JAUGE_POUSSE toutes les JAUGE_POUSSE_PERIODE_S ;
//   - les elementaires ATTIRES (gabarits reserves 9105021-9105023, envoyes en `charger_vers` sur la Graine par le
//     Directeur, jardin_orage.py) sont ADOPTES a RAYON_ADOPTION ; un attire adopte a RAYON_GRAINE de la Graine s'y
//     dissout (le module le retire : c'est l'objectif de l'evenement, seule exception a « le module ne retire aucune
//     creature du Directeur ») et la jauge recule de JAUGE_PERTE ;
//   - refus du Directeur : aucun attire en ATTENTE_DEBUT_S -> lancement RENDU (regle du socle) ;
//   - echec : jauge a 0, ou regles du Filon-mere (specialiste absent, groupe mort, deconnexion) ;
//   - victoire : jauge > 0 a DUREE_JARDIN_S -> Fleur-tempete au specialiste (garanti / 25 %), recompenses de
//     participant (Orage), et la Fleur-tempete epanouie (objet 9101092) que chaque participant cueille UNE fois.
// FLORAISON DES TOMBES (voie module seule). Le sort 9113902, lance a RAYON_CIMETIERE d'un point d'ame (game_graveyard)
// de la carte, pose le Champ de Fleurs de tombe (objet 9101093, pivot) :
//   - toutes les AME_PERIODE_S, une Ame egaree (objet 9101097, verrou 5190) parait a AME_ECART_MIN-MAX m du champ et
//     s'efface au bout de AME_VIE_S ; on la capture par le sort d'ouverture de son verrou (9113903, 2 s d'incantation :
//     le cas LOCK_KEY_SPELL, mecanisme de nos noeuds D-PO-35). La capture est comptee au crochet OnSpellCast (apres
//     l'effet, donc l'incantation menee a terme) ; un clic sans le sort (CMSG_GAMEOBJ_USE) ne compte jamais ;
//   - a la CAPTURES_AME_ANCIENNE-ieme capture, l'Ame ancienne (9105026, elite 60, PV par la table du Colosse) se leve ;
//   - victoire : sa mort -> Fleur d'eternite au specialiste, recompenses de participant (Glas) ;
//   - echec : DUREE_FLORAISON_S, regles du Filon-mere ; pas de refus du Directeur (il n'y est pour rien).
// Integration T2 (21/10, corrections des revues) : un attire n'est adopte que si sa destination est CETTE Graine et que la
// Graine est le pivot actif le plus proche ; exclusivite de RAYON_EXCLUSIF_DIRECTEUR pour le Jardin ; l'Ame ancienne
// laisse au moins AME_ANCIENNE_DELAI_S ; bots refuses ; materiau remis par la file commune ; session persistee (un
// redemarrage rend le lancement, le Directeur arrete ses vagues quand la Graine n'est plus : sourcier_fin.py).
// Journal : « coa sourcier: ... evenement=jardin_orage|floraison_tombes pivot=<guid> ... ». Au demarrage :
// « coa sourcier: herboristerie evenement=... PRET|INERTE ».
//
// INERTE tant que ses donnees manquent (gabarits avec ScriptName, materiau, sorts) : le sort est refuse avec « pas encore
// disponible », rien n'apparait.

#include "AscensionArtisanatIds.h"
#include "AscensionArtisanatSourcierSocle.h"
#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameGraveyard.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
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
#include "TemporarySummon.h"
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
    // ---- Les deux evenements --------------------------------------------------------------------------------------

    enum class Genre : uint8 { Jardin, Floraison };

    struct DefHerbo
    {
        Genre genre;
        char const* code;                       // journal (et texte de la demande au Directeur pour le Jardin)
        uint32 sort;                            // sort LANCE
        uint32 marqueur;                        // marqueur du talent : le sort est appris a qui le connait
        uint32 materiau;                        // materiau legendaire du specialiste
        uint32 verte, rare, epique;             // recompense de participant
        char const* expediteur;                 // courrier quand les sacs sont pleins
        char const* messageRendu;               // refus du Directeur (Jardin seulement)
        char const* messageSucces;              // au specialiste, avec le materiau
        char const* messageSansMateriau;        // au specialiste, succes sans materiau (hors garanti, tirage perdu)
        char const* messageEchec;
        char const* messageRelance;             // echec cause par un humain adverse : relance immediate
    };

    constexpr std::array<DefHerbo, 2> EVENEMENTS =
    {{
        { Genre::Jardin, "jardin_orage", SORT_PLANTER_GRAINE_TEMPETE, SORT_MARQUEUR_JARDIN, OBJET_FLEUR_TEMPETE,
          RESSOURCE_VERTE, RESSOURCE_RARE, RESSOURCE_EPIQUE, "Le Jardin d'orage",
          "La Graine-tempête ne germe pas : l'orage ne répond pas. Réessayez plus tard.",
          "La Graine-tempête éclot et vous livre une Fleur-tempête.",
          "La Graine-tempête éclot.",
          "La Graine-tempête se flétrit.",
          "La Graine-tempête se flétrit : vous pourrez en planter une autre tout de suite." },
        { Genre::Floraison, "floraison_tombes", SORT_FAIRE_ECLORE_TOMBES, SORT_MARQUEUR_FLORAISON, OBJET_FLEUR_ETERNITE,
          RESSOURCE_GLAS_VERTE, RESSOURCE_GLAS_RARE, RESSOURCE_GLAS_EPIQUE, "La Floraison des tombes",
          "",
          "L'Âme ancienne s'apaise et vous laisse une Fleur d'éternité.",
          "L'Âme ancienne s'apaise.",
          "Les fleurs des tombes se referment.",
          "Les fleurs des tombes se referment : vous pourrez les faire éclore de nouveau tout de suite." },
    }};

    DefHerbo const& DEF_JARDIN = EVENEMENTS[0];
    DefHerbo const& DEF_FLORAISON = EVENEMENTS[1];

    std::array<bool, EVENEMENTS.size()> Prets{};   // donnees presentes, lu au demarrage (fil monde, avant tout sort)

    DefHerbo const* DefParSort(uint32 sort)
    {
        for (DefHerbo const& d : EVENEMENTS)
            if (d.sort && d.sort == sort)
                return &d;
        return nullptr;
    }

    bool EstPret(DefHerbo const* d)
    {
        return d && Prets[std::size_t(d - EVENEMENTS.data())];
    }

    bool EstAttire(uint32 entree)
    {
        return std::find(PNJ_ATTIRES_JARDIN.begin(), PNJ_ATTIRES_JARDIN.end(), entree) != PNJ_ATTIRES_JARDIN.end();
    }

    uint32 HumainsDuGroupe(Player* specialiste, float rayon)
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

    // ---- La session commune (mixin des deux IA de pivot) -------------------------------------------------------------
    //
    // Tout est lu et ecrit sur le fil de la carte du pivot : les crochets d'unite et de sort qui la joignent sont sur LEUR
    // carte, qui est la sienne (marque CLE_FILON, resolue sur la meme carte).

    class SessionHerbo
    {
    public:
        explicit SessionHerbo(WorldObject* moi, DefHerbo const& def) : _moi(moi), _def(def) { }
        virtual ~SessionHerbo() = default;

        void Crediter(Player* joueur, uint32 montant)
        {
            if (!joueur || !_creditOuvert || _fini)
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
                _def.code, _pivot.GetCounter(), _specialiste.GetCounter(), tueur ? tueur->GetGUID().GetCounter() : 0, humain ? 1 : 0,
                adverse ? 1 : 0, _mortParJoueur ? 1 : 0);
        }

        bool EstSpecialiste(ObjectGuid g) const { return g && g == _specialiste; }
        TeamId Equipe() const { return _equipe; }
        // Une unite de cette session dont les degats subis creditent (attire adopte, Ame ancienne).
        virtual bool CrediteSur(Unit const* victime) const = 0;
        // Mort d'une creature de la session (crochet OnUnitDeath, meme carte).
        virtual void CreatureMorte(Creature* /*c*/) { }

    protected:
        char const* Code() const { return _def.code; }

        void DemarrerCommun(Player* specialiste, Registre const& avant)
        {
            _pivot = _moi->GetGUID();
            _specialiste = specialiste->GetGUID();
            _equipe = specialiste->GetTeamId();
            _debut = Maintenant();
            _groupe = specialiste->GetGroup() ? specialiste->GetGroup()->GetGUID() : ObjectGuid::Empty;
            _avantJour = avant.jourLance;
            _avantRelance = avant.relanceTs;
            Marquer(specialiste, _pivot);
            InscrireActif(_moi);
            OuvrirSession(_pivot, _specialiste, Code(), avant);
        }

        // Regles du Filon-mere, une fois par seconde : -> raison d'echec, ou nullptr. `parJoueur` : la relance immediate.
        char const* SurveillerSpecialiste(time_t maintenant, bool& parJoueur)
        {
            parJoueur = false;
            Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste);
            if (!s && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                return "deconnexion";
            bool const present = s && s->IsAlive() && s->IsWithinDist(_moi, RAYON_PARTICIPATION);
            if (s && s->GetGroup())
                _groupe = s->GetGroup()->GetGUID();   // un groupe forme apres le lancement compte aussi
            if (present)
            {
                _specialisteMortA = 0;
                _mortParJoueur = false;
            }
            else if (_specialisteMortA && !_mortParJoueur && maintenant - _specialisteMortA > time_t(ABSENCE_MAX_S))
                return "specialiste";
            if (!present && !GroupeEnVie())
            {
                parJoueur = _mortParJoueur;
                return "groupe";
            }
            return nullptr;
        }

        // Le specialiste vivant sur la carte, ou un membre de son groupe vivant a portee (carte du pivot seulement, P-137).
        bool GroupeEnVie()
        {
            for (MapReference const& ref : _moi->GetMap()->GetPlayers())
                if (Player* m = ref.GetSource())
                {
                    if (!m->IsAlive())
                        continue;
                    if (m->GetGUID() == _specialiste)
                        return true;
                    if (_groupe && m->GetGroup() && m->GetGroup()->GetGUID() == _groupe && m->IsWithinDist(_moi, RAYON_PARTICIPATION))
                        return true;
                }
            return false;
        }

        // Participants humains de la faction : groupe du specialiste present a RAYON_PARTICIPATION, credites (degats ou
        // soins au-dela du seuil), et `_acteurs` (captureurs d'ames). -> joueurs presents sur la carte, et leurs comptes.
        std::vector<Player*> Participants(std::set<uint32>& comptes)
        {
            std::set<ObjectGuid> participants(_acteurs.begin(), _acteurs.end());
            if (Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste))
            {
                participants.insert(s->GetGUID());
                if (Group* g = s->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (EstHumain(m) && m->IsInWorld() && m->GetMap() == _moi->GetMap() && m->IsWithinDist(_moi, RAYON_PARTICIPATION))
                                participants.insert(m->GetGUID());
            }
            for (auto const& [g, montant] : _credits)
                if (montant >= _seuilParticipation)
                    participants.insert(g);
            std::vector<Player*> humains;
            for (ObjectGuid g : participants)
                if (Player* p = ObjectAccessor::GetPlayer(*_moi, g))
                    if (EstHumain(p) && p->GetTeamId() == _equipe)
                    {
                        humains.push_back(p);
                        comptes.insert(p->GetSession()->GetAccountId());
                    }
            return humains;
        }

        // Victoire : materiau legendaire au seul specialiste (garanti hebdomadaire / chance, registre commun), puis les
        // recompenses de participant. -> les participants humains (pour la cueillette de la Fleur-tempete).
        std::vector<Player*> Recompenser()
        {
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            int64 const semaine = SemaineDeJeu(jour);
            std::set<uint32> comptes;
            std::vector<Player*> humains = Participants(comptes);
            LOG_INFO("module", "coa sourcier: succes evenement={} pivot={} specialiste={} participants_humains={} comptes={} duree_s={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), uint32(humains.size()), uint32(comptes.size()),
                uint32(maintenant - _debut));

            Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste);
            Registre r = LireRegistre(_specialiste.GetCounter());
            DecisionCoeur const d = DeciderCoeur(r, semaine, uint32(comptes.size()));
            EcrireRegistre(_specialiste.GetCounter(), r);
            bool remis = false;
            char const* voie = "aucune";
            if (d.obtenu)
                voie = RemettreMateriau(s, _specialiste, _def.materiau, _def.expediteur, remis);
            LOG_INFO("module", "coa sourcier: remise evenement={} pivot={} specialiste={} materiau={} garanti={} obtenu={} remis={} voie={} semaine={} comptes={}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), _def.materiau, d.garanti ? 1 : 0, d.obtenu ? 1 : 0,
                remis ? 1 : 0, voie, int32(semaine), uint32(comptes.size()));
            if (s)
                ChatHandler(s->GetSession()).SendNotification(d.obtenu ? _def.messageSucces : _def.messageSansMateriau);

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
                Donner(p, _def.verte, RESSOURCES_VERTES, ObjectGuid::Empty, _def.expediteur);
                if (rare)
                    Donner(p, _def.rare, 1, ObjectGuid::Empty, _def.expediteur);
                if (epique)
                    Donner(p, _def.epique, 1, ObjectGuid::Empty, _def.expediteur);
                LOG_INFO("module", "coa sourcier: recompense evenement={} pivot={} joueur={} vertes={} rare={} epique={} du_jour={}/{}", Code(),
                    _pivot.GetCounter(), p->GetGUID().GetCounter(), RESSOURCES_VERTES, rare, epique, rp.recompN, RECOMPENSES_PAR_JOUR);
            }
            return humains;
        }

        // Echec : relance immediate une fois par jour si un humain adverse l'a cause, sinon au bout de 4 h une fois par jour.
        void EchouerCommun(char const* raison, bool parJoueur, std::string const& detail)
        {
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
            if (Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste))
            {
                AfficherRecharge(s, r);
                ChatHandler(s->GetSession()).SendNotification(immediate ? _def.messageRelance : _def.messageEchec);
            }
            LOG_INFO("module", "coa sourcier: echec evenement={} pivot={} specialiste={} raison={} par_joueur={} relance={} {}",
                Code(), _pivot.GetCounter(), _specialiste.GetCounter(), raison, parJoueur ? 1 : 0, relance, detail);
        }

        // Refus du Directeur : les deux champs de LANCEMENT du registre reprennent leur valeur d'avant ce lancement.
        void RendreCommun(char const* raison)
        {
            Registre r = LireRegistre(_specialiste.GetCounter());
            r.jourLance = _avantJour;
            r.relanceTs = _avantRelance;
            EcrireRegistre(_specialiste.GetCounter(), r);
            if (Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste))
            {
                AfficherRecharge(s, r);
                if (*_def.messageRendu)
                    ChatHandler(s->GetSession()).SendNotification(_def.messageRendu);
            }
            LOG_INFO("module", "coa sourcier: rendu evenement={} pivot={} specialiste={} raison={} attente_s={}", Code(), _pivot.GetCounter(),
                _specialiste.GetCounter(), raison, uint32(Maintenant() - _debut));
        }

        // Message systeme au specialiste et aux membres de son groupe a RAYON_PARTICIPATION du pivot.
        void DireAuGroupe(std::string const& texte)
        {
            std::set<ObjectGuid> dits;
            if (Player* s = ObjectAccessor::GetPlayer(*_moi, _specialiste))
            {
                ChatHandler(s->GetSession()).SendSysMessage(texte);
                dits.insert(s->GetGUID());
                if (Group* g = s->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (EstHumain(m) && !dits.count(m->GetGUID()) && m->IsInWorld() && m->GetMap() == _moi->GetMap()
                                && m->IsWithinDist(_moi, RAYON_PARTICIPATION))
                            {
                                ChatHandler(m->GetSession()).SendSysMessage(texte);
                                dits.insert(m->GetGUID());
                            }
            }
        }

        WorldObject* _moi;
        DefHerbo const& _def;
        bool _fini = false;
        bool _creditOuvert = false;
        ObjectGuid _pivot, _specialiste, _groupe;
        TeamId _equipe = TEAM_NEUTRAL;
        time_t _debut = 0, _specialisteMortA = 0;
        int64 _avantJour = 0;
        uint32 _avantRelance = 0;
        bool _mortParJoueur = false;
        uint32 _seuilParticipation = 1;
        std::map<ObjectGuid, uint32> _credits;
        std::set<ObjectGuid> _acteurs;
    };

    SessionHerbo* SessionDe(Unit const* u)
    {
        ObjectGuid const p = PivotDe(u);
        if (!p)
            return nullptr;
        // Le pivot du Jardin est une creature, celui de la Floraison un objet ; une marque d'un autre mini-evenement
        // (Filon-mere, Faille tellurique) ne se resout pas ici (l'IA n'est pas une SessionHerbo).
        if (p.IsGameObject())
        {
            GameObject* go = ObjectAccessor::GetGameObject(*u, p);
            return go ? dynamic_cast<SessionHerbo*>(go->AI()) : nullptr;
        }
        if (p.IsCreature())
        {
            Creature* c = ObjectAccessor::GetCreature(*u, p);
            return c ? dynamic_cast<SessionHerbo*>(c->AI()) : nullptr;
        }
        return nullptr;
    }

    // ---- La Fleur-tempete epanouie : cueillie une fois par participant -----------------------------------------------

    class FleurTempeteAI final : public GameObjectAI
    {
    public:
        explicit FleurTempeteAI(GameObject* go) : GameObjectAI(go) { }

        // Fil de la carte, a la pose (Jardin reussi).
        void Ouvrir(TeamId equipe, ObjectGuid groupe, std::vector<Player*> const& participants, ObjectGuid pivot)
        {
            _equipe = equipe;
            _groupe = groupe;
            _pivot = pivot;
            for (Player* p : participants)
                _permis.insert(p->GetGUID());
            _ouverte = true;
        }

        bool GossipHello(Player* joueur, bool reportUse) override
        {
            // CMSG_GAMEOBJ_REPORT_USE rappelle GossipHello apres GameObject::Use (SpellHandler.cpp:370) : un clic, une cueillette.
            if (reportUse || !joueur)
                return true;
            auto dire = [&](char const* t) { ChatHandler(joueur->GetSession()).SendNotification(t); };
            if (!_ouverte || !EstHumain(joueur) || !joueur->IsAlive())
                return true;
            if (joueur->GetTeamId() != _equipe)
                return dire("Cette fleur ne s'ouvre pas pour votre faction."), true;
            bool const membre = _groupe && joueur->GetGroup() && joueur->GetGroup()->GetGUID() == _groupe;
            if (!membre && !_permis.count(joueur->GetGUID()))
                return dire("Cette fleur a éclos pour ceux qui l'ont défendue."), true;
            if (_cueillie.count(joueur->GetGUID()))
                return dire("Vous avez déjà cueilli cette fleur."), true;
            // AutoStoreLoot jette ce qui ne tient pas (Player.cpp:14280-14284) : la cueillette n'est comptee qu'avec de la place.
            if (joueur->GetFreeInventorySpace() < PLACE_CUEILLETTE)
                return dire("Il faut 3 cases libres pour cueillir la Fleur-tempête."), true;
            _cueillie.insert(joueur->GetGUID());
            if (uint32 const butin = me->GetGOInfo()->GetLootId())
                joueur->AutoStoreLoot(butin, LootTemplates_Gameobject, true);
            bool const pur = urand(1, 100) <= CHANCE_PUR_FLEUR_POUR_CENT && Donner(joueur, PUR_FLEUR_TEMPETE, 1, joueur->GetGUID(),
                "Le Jardin d'orage");
            LOG_INFO("module", "coa sourcier: cueillette evenement=jardin_orage pivot={} fleur={} joueur={} pur={} cueillettes={}",
                _pivot.GetCounter(), me->GetGUID().GetCounter(), joueur->GetGUID().GetCounter(), pur ? 1 : 0, uint32(_cueillie.size()));
            return true;
        }

    private:
        bool _ouverte = false;
        TeamId _equipe = TEAM_NEUTRAL;
        ObjectGuid _groupe, _pivot;
        std::set<ObjectGuid> _permis, _cueillie;
    };

    class go_coa_fleur_tempete final : public GameObjectScript
    {
    public:
        go_coa_fleur_tempete() : GameObjectScript(SCRIPT_FLEUR_TEMPETE) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new FleurTempeteAI(go); }
    };

    // ---- JARDIN D'ORAGE : l'IA de la Graine-tempete -----------------------------------------------------------------

    class GraineTempeteAI final : public CreatureAI, public SessionHerbo
    {
    public:
        explicit GraineTempeteAI(Creature* c) : CreatureAI(c), SessionHerbo(c, DEF_JARDIN) { }

        ~GraineTempeteAI() override
        {
            RetirerActif(_pivot);   // despawn du filet, carte dechargee : l'exclusivite ne survit pas a la Graine
        }

        enum class Phase : uint8 { Attente, Appel, Croissance, Fini };

        // Fil de la carte, au lancement.
        void Demarrer(Player* specialiste, Registre const& avant)
        {
            me->SetReactState(REACT_PASSIVE);
            me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
            me->SetControlled(true, UNIT_STATE_ROOT);
            me->SetRegeneratingHealth(false);
            // La jauge = les PV : 1000 PV au plus, la barre de vie de la cible montre le pourcentage.
            me->SetCreateHealth(JAUGE_MAX);
            me->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(JAUGE_MAX));
            _jauge = JAUGE_DEPART;
            AppliquerJauge();
            DemarrerCommun(specialiste, avant);
            _phase = Phase::Appel;
            LOG_INFO("module", "coa sourcier: lancement evenement={} specialiste={} pivot={} carte={} x={:.1f} y={:.1f} attente_max_s={} duree_s={} jauge={}",
                Code(), _specialiste.GetCounter(), _pivot.GetCounter(), me->GetMapId(), me->GetPositionX(), me->GetPositionY(),
                ATTENTE_DEBUT_S, DUREE_JARDIN_S, _jauge);
        }

        // Inattaquable : sa mort ne devrait jamais arriver ; si elle arrive (effet de zone hors regles), la session echoue
        // proprement au lieu de rester suspendue jusqu'au despawn du filet.
        void JustDied(Unit* /*tueur*/) override
        {
            if (_phase == Phase::Appel)
                Rendre("graine_morte");
            else if (_phase == Phase::Croissance)
                Echouer("graine_morte", false);
        }

        void MoveInLineOfSight(Unit* /*qui*/) override { }
        void AttackStart(Unit* /*qui*/) override { }
        void EnterEvadeMode(EvadeReason /*pourquoi*/) override { }

        void UpdateAI(uint32 diff) override
        {
            if (_phase == Phase::Attente || _phase == Phase::Fini)
                return;
            _cumul += diff;
            if (_cumul < 1000)
                return;
            _cumul = 0;
            Seconde();
        }

        bool CrediteSur(Unit const* victime) const override
        {
            return victime && victime->IsCreature() && _adoptes.count(victime->GetGUID());
        }

    private:
        void AppliquerJauge()
        {
            uint32 const max = std::max<uint32>(1, me->GetMaxHealth());
            me->SetHealth(std::max<uint32>(1, uint32(uint64(max) * std::min(_jauge, JAUGE_MAX) / JAUGE_MAX)));
        }

        void Seconde()
        {
            time_t const maintenant = Maintenant();
            Adopter();
            if (_phase == Phase::Appel)
            {
                if (!ObjectAccessor::GetPlayer(*me, _specialiste) && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                    return Rendre("deconnexion");
                if (maintenant - _debut >= time_t(ATTENTE_DEBUT_S))
                    return Rendre("directeur_absent");
                AppliquerJauge();
                return;
            }
            bool parJoueur = false;
            if (char const* raison = SurveillerSpecialiste(maintenant, parJoueur))
                return Echouer(raison, parJoueur);
            Dissoudre();
            if (_jauge == 0)
                return Echouer("jauge", false);
            if (++_secondesPousse >= JAUGE_POUSSE_PERIODE_S)
            {
                _secondesPousse = 0;
                _jauge = std::min(JAUGE_MAX, _jauge + JAUGE_POUSSE);
            }
            AppliquerJauge();
            if (maintenant - _debut >= time_t(DUREE_JARDIN_S))
                return Reussir();
            if (++_secondesAnnonce >= JAUGE_ANNONCE_S)
            {
                _secondesAnnonce = 0;
                uint32 const reste = uint32(std::max<int64>(0, int64(DUREE_JARDIN_S) - int64(maintenant - _debut)));
                DireAuGroupe(Acore::StringFormat("Graine-tempête : {} %, éclosion dans {} min {:02} s.", _jauge / 10, reste / 60, reste % 60));
            }
        }

        // Les attires a RAYON_ADOPTION de la Graine (carte de la Graine seulement, P-137). Un attire deja marque par un
        // AUTRE pivot est laisse ; un cadavre n'est jamais adopte.
        void Adopter()
        {
            std::vector<uint32> const entrees(PNJ_ATTIRES_JARDIN.begin(), PNJ_ATTIRES_JARDIN.end());
            std::list<Creature*> trouvees;
            me->GetCreatureListWithEntryInGrid(trouvees, entrees, RAYON_ADOPTION);
            for (Creature* c : trouvees)
            {
                if (!c->IsAlive() || PivotDe(c))
                    continue;
                // Sa destination est-elle CETTE Graine ? `charger_vers` pose le point de retour de l'attire sur la Graine
                // (CoaDirecteur.cpp ChargerVers) ; un attire en route vers une autre Graine passe sans etre pris.
                if (c->GetHomePosition().GetExactDist2d(me) > GRAINE_DESTINATION_ECART)
                    continue;
                Marquer(c, _pivot);
                _adoptes.insert(c->GetGUID());
                ++_adoptesTotal;
                if (_phase == Phase::Appel)
                {
                    _phase = Phase::Croissance;
                    _creditOuvert = true;
                    // Participation : le quart des PV du premier attire, inflige ou soigne (les attires sont nombreux et
                    // petits ; le seuil du Filon, 2 % d'un elite, ne s'y transpose pas).
                    _seuilParticipation = std::max<uint32>(1, c->GetMaxHealth() / 4);
                    LOG_INFO("module", "coa sourcier: menace evenement={} pivot={} premiere={} apres_s={} seuil_participation={}", Code(),
                        _pivot.GetCounter(), c->GetEntry(), uint32(Maintenant() - _debut), _seuilParticipation);
                    DireAuGroupe("L'orage a senti la Graine-tempête : défendez-la jusqu'à son éclosion !");
                }
            }
        }

        // Un attire adopte arrive a RAYON_GRAINE (a l'horizontale) : il se dissout dans la Graine, la jauge recule.
        void Dissoudre()
        {
            for (auto it = _adoptes.begin(); it != _adoptes.end();)
            {
                Creature* c = ObjectAccessor::GetCreature(*me, *it);
                if (!c || !c->IsAlive())
                {
                    it = _adoptes.erase(it);   // tue, ou retire par le Directeur : rien
                    continue;
                }
                if (c->GetExactDist2d(me) > RAYON_GRAINE)
                {
                    ++it;
                    continue;
                }
                uint32 const avant = _jauge;
                _jauge = _jauge > JAUGE_PERTE ? _jauge - JAUGE_PERTE : 0;
                ++_arrives;
                LOG_INFO("module", "coa sourcier: attire arrive evenement={} pivot={} attire={} entree={} jauge={}->{} arrives={}", Code(),
                    _pivot.GetCounter(), c->GetGUID().GetCounter(), c->GetEntry(), avant, _jauge, _arrives);
                c->DespawnOrUnsummon();
                it = _adoptes.erase(it);
                me->TextEmote("La foudre frappe la Graine-tempête !", nullptr, false);
            }
        }

        void Reussir()
        {
            if (_phase != Phase::Croissance)
                return;
            _phase = Phase::Fini;
            _fini = true;
            std::vector<Player*> const humains = Recompenser();
            PoserFleur(humains);
            Terminer("succes");
        }

        void PoserFleur(std::vector<Player*> const& humains)
        {
            GameObject* fleur = me->GetMap()->SummonGameObject(GO_FLEUR_TEMPETE_EPANOUIE, me->GetPositionX(), me->GetPositionY(),
                me->GetPositionZ(), me->GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, FLEUR_VIE_S);
            FleurTempeteAI* ai = fleur ? dynamic_cast<FleurTempeteAI*>(fleur->AI()) : nullptr;
            if (!ai)
            {
                LOG_ERROR("module", "coa sourcier: fleur {} non posee ou sans le script {} pivot={}", GO_FLEUR_TEMPETE_EPANOUIE, SCRIPT_FLEUR_TEMPETE,
                    _pivot.GetCounter());
                if (fleur)
                {
                    fleur->SetRespawnTime(0);
                    fleur->Delete();
                }
                return;
            }
            ai->Ouvrir(_equipe, _groupe, humains, _pivot);
            LOG_INFO("module", "coa sourcier: fleur evenement={} pivot={} fleur={} permis={} vie_s={}", Code(), _pivot.GetCounter(),
                fleur->GetGUID().GetCounter(), uint32(humains.size()), FLEUR_VIE_S);
        }

        void Echouer(char const* raison, bool parJoueur)
        {
            if (_phase == Phase::Fini)
                return;
            _phase = Phase::Fini;
            _fini = true;
            EchouerCommun(raison, parJoueur, Acore::StringFormat("jauge={} adoptes={} arrives={}", _jauge, _adoptesTotal, _arrives));
            Terminer(raison);
        }

        void Rendre(char const* raison)
        {
            if (_phase == Phase::Fini)
                return;
            _phase = Phase::Fini;
            _fini = true;
            RendreCommun(raison);
            Terminer(raison);
        }

        // Le module ne retire aucun attire encore en route (le Directeur les leve a sa propre echeance) : seule la Graine
        // s'en va.
        void Terminer(char const* raison)
        {
            _phase = Phase::Fini;
            _fini = true;
            RetirerActif(_pivot);
            LOG_INFO("module", "coa sourcier: nettoyage evenement={} pivot={} raison={} jauge={}", Code(), _pivot.GetCounter(), raison, _jauge);
            FermerSession(_pivot, raison);
            me->DespawnOrUnsummon();
        }

        Phase _phase = Phase::Attente;
        uint32 _jauge = 0, _cumul = 0, _secondesPousse = 0, _secondesAnnonce = 0, _adoptesTotal = 0, _arrives = 0;
        std::set<ObjectGuid> _adoptes;
    };

    class npc_coa_graine_tempete final : public CreatureScript
    {
    public:
        npc_coa_graine_tempete() : CreatureScript(SCRIPT_GRAINE_TEMPETE) { }
        CreatureAI* GetAI(Creature* c) const override { return new GraineTempeteAI(c); }
    };

    // ---- FLORAISON DES TOMBES : l'IA du Champ de Fleurs de tombe ------------------------------------------------------

    class ChampFleursTombeAI final : public GameObjectAI, public SessionHerbo
    {
    public:
        explicit ChampFleursTombeAI(GameObject* go) : GameObjectAI(go), SessionHerbo(go, DEF_FLORAISON) { }

        ~ChampFleursTombeAI() override
        {
            RetirerActif(_pivot);
        }

        enum class Phase : uint8 { Attente, Ames, AmeAncienne, Fini };

        void Demarrer(Player* specialiste, Registre const& avant)
        {
            DemarrerCommun(specialiste, avant);
            _phase = Phase::Ames;
            _prochaineAme = _debut + 3;   // la premiere tout de suite, ou presque
            LOG_INFO("module", "coa sourcier: lancement evenement={} specialiste={} pivot={} carte={} x={:.1f} y={:.1f} duree_s={} captures={}",
                Code(), _specialiste.GetCounter(), _pivot.GetCounter(), me->GetMapId(), me->GetPositionX(), me->GetPositionY(),
                DUREE_FLORAISON_S, CAPTURES_AME_ANCIENNE);
        }

        bool GossipHello(Player* /*joueur*/, bool /*reportUse*/) override
        {
            return true;   // decor : rien ne s'ouvre
        }

        void UpdateAI(uint32 diff) override
        {
            if (_phase == Phase::Attente || _phase == Phase::Fini)
                return;
            _cumul += diff;
            if (_cumul < 1000)
                return;
            _cumul = 0;
            Seconde();
        }

        bool CrediteSur(Unit const* victime) const override
        {
            return victime && _chef && victime->GetGUID() == _chef;
        }

        void CreatureMorte(Creature* c) override
        {
            if (c && _chef && c->GetGUID() == _chef && _phase == Phase::AmeAncienne)
                Reussir();
        }

        bool AmesOuvertes() const { return _phase == Phase::Ames; }

        // Crochet OnSpellCast du sort de capture, fil de la carte : `ame` est une Ame egaree de CE champ (marque verifiee
        // par l'appelant). -> vrai si la capture compte.
        bool Capturer(Player* joueur, GameObject* ame)
        {
            if (_phase != Phase::Ames || !joueur || !ame || !_ames.count(ame->GetGUID()))
                return false;
            if (!EstHumain(joueur) || !joueur->IsAlive() || joueur->GetTeamId() != _equipe)
                return false;
            _ames.erase(ame->GetGUID());
            ame->SetRespawnTime(0);
            ame->Delete();
            ++_captures;
            _acteurs.insert(joueur->GetGUID());
            Marquer(joueur, _pivot);
            LOG_INFO("module", "coa sourcier: capture evenement={} pivot={} joueur={} captures={}/{}", Code(), _pivot.GetCounter(),
                joueur->GetGUID().GetCounter(), _captures, CAPTURES_AME_ANCIENNE);
            if (_captures >= CAPTURES_AME_ANCIENNE)
                LeverAmeAncienne(joueur);
            else
                DireAuGroupe(Acore::StringFormat("Âme égarée capturée : {} sur {}.", _captures, CAPTURES_AME_ANCIENNE));
            return true;
        }

    private:
        void Seconde()
        {
            time_t const maintenant = Maintenant();
            bool parJoueur = false;
            if (char const* raison = SurveillerSpecialiste(maintenant, parJoueur))
                return Echouer(raison, parJoueur);
            // L'Ame ancienne levee a toujours AME_ANCIENNE_DELAI_S pour tomber (revue HERBORISTERIE 4).
            time_t echeance = _debut + time_t(DUREE_FLORAISON_S);
            if (_leveeA)
                echeance = std::max(echeance, _leveeA + time_t(AME_ANCIENNE_DELAI_S));
            if (maintenant >= echeance)
                return Echouer("temps", _specialisteMortA && _mortParJoueur);
            if (_phase == Phase::Ames)
            {
                // Les ames effacees a leur echeance (AME_VIE_S) quittent la liste.
                for (auto it = _ames.begin(); it != _ames.end();)
                    it = ObjectAccessor::GetGameObject(*me, *it) ? std::next(it) : _ames.erase(it);
                if (maintenant >= _prochaineAme)
                {
                    _prochaineAme = maintenant + time_t(AME_PERIODE_S);
                    PoserAme();
                }
                return;
            }
            if (_phase == Phase::AmeAncienne)
            {
                Creature* c = ObjectAccessor::GetCreature(*me, _chef);
                if (!c)
                    return Echouer("ame_disparue", false);
                if (!c->IsAlive())
                    return Reussir();
            }
        }

        // Une Ame egaree a AME_ECART_MIN-MAX m du champ, au sol (MovePosition : sol et collisions), hors de l'eau : 4 essais.
        void PoserAme()
        {
            for (uint32 essai = 0; essai < 4; ++essai)
            {
                Position pos = me->GetPosition();
                me->MovePosition(pos, frand(AME_ECART_MIN, AME_ECART_MAX), float(rand_norm()) * 2.0f * float(M_PI));
                if (me->GetMap()->IsInWater(me->GetPhaseMask(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), 2.0f))
                    continue;
                GameObject* ame = me->GetMap()->SummonGameObject(GO_AME_EGAREE, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                    pos.GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, AME_VIE_S);
                if (!ame)
                    break;
                ame->CustomData.GetDefault<MarqueFilon>(CLE_FILON)->filon = _pivot;
                _ames.insert(ame->GetGUID());
                ++_amesPosees;
                LOG_INFO("module", "coa sourcier: ame evenement={} pivot={} ame={} posees={} en_vie={}", Code(), _pivot.GetCounter(),
                    ame->GetGUID().GetCounter(), _amesPosees, uint32(_ames.size()));
                return;
            }
            LOG_INFO("module", "coa sourcier: ame non posee evenement={} pivot={} raison=eau_ou_pose", Code(), _pivot.GetCounter());
        }

        void EffacerAmes()
        {
            for (ObjectGuid g : _ames)
                if (GameObject* ame = ObjectAccessor::GetGameObject(*me, g))
                {
                    ame->SetRespawnTime(0);
                    ame->Delete();
                }
            _ames.clear();
        }

        void LeverAmeAncienne(Player* declencheur)
        {
            EffacerAmes();
            Position pos = me->GetPosition();
            me->MovePosition(pos, 4.0f, float(rand_norm()) * 2.0f * float(M_PI));
            uint32 const reste = uint32(std::max<int64>(AME_ANCIENNE_DELAI_S, int64(DUREE_FLORAISON_S) - int64(Maintenant() - _debut)));
            TempSummon* ame = me->GetMap()->SummonCreature(PNJ_AME_ANCIENNE, pos, nullptr, (reste + 300) * IN_MILLISECONDS);
            if (!ame)
            {
                LOG_ERROR("module", "coa sourcier: ame ancienne {} non invoquee pivot={} : echec", PNJ_AME_ANCIENNE, _pivot.GetCounter());
                return Echouer("ame_absente", false);
            }
            // Calibrage reduit (table du Colosse) : PV en % du modele selon les humains du groupe presents a 40 m.
            uint32 humains = 1;
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
                humains = std::max<uint32>(1, HumainsDuGroupe(s, RAYON_DOSAGE));
            uint8 const pct = PV_COLOSSE_POUR_CENT[std::min<std::size_t>(humains, PV_COLOSSE_POUR_CENT.size()) - 1];
            float const base = ame->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE);
            if (base > 0.0f && pct < 100)
            {
                float const dosee = std::max(1.0f, std::round(base * float(pct) / 100.0f));
                ame->SetCreateHealth(uint32(dosee));
                ame->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, dosee);
            }
            ame->SetHealth(ame->GetMaxHealth());
            _chef = ame->GetGUID();
            _seuilParticipation = std::max<uint32>(1, uint32(uint64(ame->GetMaxHealth()) * PART_PARTICIPATION_POUR_MILLE / 1000));
            _creditOuvert = true;
            Marquer(ame, _pivot);
            _phase = Phase::AmeAncienne;
            _leveeA = Maintenant();
            if (declencheur && ame->IsAIEnabled && ame->AI())
                ame->AI()->AttackStart(declencheur);
            DireAuGroupe("Les âmes captives réveillent l'Âme ancienne !");
            LOG_INFO("module", "coa sourcier: ame ancienne evenement={} pivot={} ame={} humains_presents={} pv_pct={} pv={} seuil_participation={}",
                Code(), _pivot.GetCounter(), _chef.GetCounter(), humains, uint32(pct), ame->GetMaxHealth(), _seuilParticipation);
        }

        void Reussir()
        {
            if (_phase != Phase::AmeAncienne)
                return;
            _phase = Phase::Fini;
            _fini = true;
            Recompenser();
            Terminer("succes");
        }

        void Echouer(char const* raison, bool parJoueur)
        {
            if (_phase == Phase::Fini)
                return;
            _phase = Phase::Fini;
            _fini = true;
            EchouerCommun(raison, parJoueur, Acore::StringFormat("captures={} ames_posees={} ame_ancienne={}", _captures, _amesPosees,
                _chef ? 1 : 0));
            Terminer(raison);
        }

        // L'Ame ancienne vivante (echec) s'en va ; son cadavre (succes) reste a son minuteur. Les ames restantes et le
        // champ disparaissent.
        void Terminer(char const* raison)
        {
            _phase = Phase::Fini;
            _fini = true;
            bool ameRetiree = false;
            if (Creature* c = ObjectAccessor::GetCreature(*me, _chef))
                if (c->IsAlive())
                {
                    c->DespawnOrUnsummon();
                    ameRetiree = true;
                }
            EffacerAmes();
            RetirerActif(_pivot);
            FermerSession(_pivot, raison);
            LOG_INFO("module", "coa sourcier: nettoyage evenement={} pivot={} raison={} captures={} ame_retiree={}", Code(), _pivot.GetCounter(),
                raison, _captures, ameRetiree ? 1 : 0);
            me->SetRespawnTime(0);
            me->Delete();
        }

        Phase _phase = Phase::Attente;
        uint32 _cumul = 0, _captures = 0, _amesPosees = 0;
        time_t _prochaineAme = 0, _leveeA = 0;
        ObjectGuid _chef;
        std::set<ObjectGuid> _ames;
    };

    class go_coa_champ_fleurs_tombe final : public GameObjectScript
    {
    public:
        go_coa_champ_fleurs_tombe() : GameObjectScript(SCRIPT_CHAMP_FLEURS_TOMBE) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new ChampFleursTombeAI(go); }
    };

    // L'Ame egaree : jamais ouverte comme un coffre (pas de butin), ni par un clic sans le sort de capture.
    class AmeEgareeAI final : public GameObjectAI
    {
    public:
        explicit AmeEgareeAI(GameObject* go) : GameObjectAI(go) { }
        bool GossipHello(Player* /*joueur*/, bool /*reportUse*/) override { return true; }
    };

    class go_coa_ame_egaree final : public GameObjectScript
    {
    public:
        go_coa_ame_egaree() : GameObjectScript(SCRIPT_AME_EGAREE) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new AmeEgareeAI(go); }
    };

    ChampFleursTombeAI* ChampDeAme(GameObject const* ame)
    {
        MarqueFilon const* m = ame ? ame->CustomData.Get<MarqueFilon>(CLE_FILON) : nullptr;
        if (!m || !m->filon.IsGameObject())
            return nullptr;
        GameObject* champ = ObjectAccessor::GetGameObject(*ame, m->filon);
        return champ ? dynamic_cast<ChampFleursTombeAI*>(champ->AI()) : nullptr;
    }

    // ---- Les sorts lances --------------------------------------------------------------------------------------------

    // Un point d'ame (game_graveyard, toutes factions) de la carte du joueur a RAYON_CIMETIERE au plus. Table chargee au
    // demarrage, lue seulement.
    bool PresDUnCimetiere(Player const* j)
    {
        for (auto const& [id, c] : sGraveyard->GetGraveyardData())
            if (c.Map == j->GetMapId() && j->GetExactDist2d(c.x, c.y) <= RAYON_CIMETIERE)
                return true;
        return false;
    }

    // Vide si le lancement est permis ; sinon le message montre au joueur.
    std::string RefusLancement(Player* j, DefHerbo const* d, std::string& code)
    {
        auto refus = [&](char const* c, std::string m) { code = c; return m; };
        if (!EstPret(d) || !TableRegistre)
            return refus("inerte", "Ce mini-événement n'est pas encore disponible.");
        if (!EstHumain(j))
            return refus("bot", "Ce mini-événement est réservé aux joueurs.");
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
        if (d->genre == Genre::Floraison && !PresDUnCimetiere(j))
            return refus("cimetiere", "Les fleurs des tombes ne poussent que près d'un cimetière.");
        Registre const r = LireRegistre(j->GetGUID().GetCounter());
        time_t const maintenant = Maintenant();
        if (!LancementAccorde(r, maintenant))
        {
            time_t fin = ProchaineRemise(maintenant);
            if (r.relanceTs && time_t(r.relanceTs) > maintenant && time_t(r.relanceTs) < fin)
                fin = time_t(r.relanceTs);
            return refus("deja", Acore::StringFormat("Vous avez déjà lancé un mini-événement du Sourcier aujourd'hui. De nouveau "
                "possible à {} (heure de Paris).", HeureParis(fin)));
        }
        // Le Jardin est de voie Directeur : exclusivite large (deux Graines ne se disputent pas leurs attires).
        if (EvenementProche(j->GetMapId(), j->GetInstanceId(), j->GetPositionX(), j->GetPositionY(),
                d->genre == Genre::Jardin ? RAYON_EXCLUSIF_DIRECTEUR : RAYON_EXCLUSIF))
            return refus("voisin", "Un autre mini-événement gronde déjà tout près.");
        return {};
    }

    // Capture : la cible est une Ame egaree d'un champ en phase des ames, le lanceur un humain de la faction du specialiste.
    std::string RefusCapture(Player* j, GameObject* ame, ChampFleursTombeAI*& champ)
    {
        champ = nullptr;
        if (!ame || ame->GetEntry() != GO_AME_EGAREE)
            return "Il faut viser une Âme égarée.";
        champ = ChampDeAme(ame);
        if (!champ || !champ->AmesOuvertes())
            return "Cette âme s'efface déjà.";
        if (!EstHumain(j))
            return "Cette âme ne se laisse pas prendre.";
        if (j->GetTeamId() != champ->Equipe())
            return "Cette âme ne répond pas à votre faction.";
        return {};
    }

    class SourcierHerboSorts final : public AllSpellScript
    {
    public:
        SourcierHerboSorts() : AllSpellScript("CoaSourcierHerboristerieSorts", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCheckCast(Spell* sort, bool /*strict*/, SpellCastResult& res) override
        {
            if (res != SPELL_CAST_OK || !sort)
                return;
            uint32 const id = sort->GetSpellInfo()->Id;
            DefHerbo const* d = DefParSort(id);
            if (!d && id != SORT_CAPTURER_AME)
                return;
            Player* j = sort->GetCaster() ? sort->GetCaster()->ToPlayer() : nullptr;
            if (!j)
            {
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
            std::string message;
            std::string code;
            if (d)
                message = RefusLancement(j, d, code);
            else
            {
                ChampFleursTombeAI* champ = nullptr;
                message = RefusCapture(j, sort->m_targets.GetGOTarget(), champ);
                code = "capture";
            }
            if (message.empty())
                return;
            res = SPELL_FAILED_DONT_REPORT;
            ChatHandler(j->GetSession()).SendNotification(message);
            LOG_INFO("module", "coa sourcier: refus evenement={} joueur={} raison={}", d ? d->code : "capture_ame", j->GetGUID().GetCounter(), code);
        }

        void OnSpellCast(Spell* sort, Unit* lanceur, SpellInfo const* info, bool /*skipCheck*/) override
        {
            if (!info)
                return;
            Player* j = lanceur ? lanceur->ToPlayer() : nullptr;
            if (!j)
                return;
            if (info->Id == SORT_CAPTURER_AME)
            {
                // Apres les effets (Spell::cast : handle_immediate puis ce crochet) : l'incantation de 2 s est menee a terme.
                GameObject* ame = sort ? sort->m_targets.GetGOTarget() : nullptr;
                ChampFleursTombeAI* champ = nullptr;
                if (!RefusCapture(j, ame, champ).empty() || !champ->Capturer(j, ame))
                    LOG_INFO("module", "coa sourcier: capture refusee evenement=floraison_tombes joueur={} ame={}", j->GetGUID().GetCounter(),
                        ame ? ame->GetGUID().GetCounter() : 0);
                return;
            }
            DefHerbo const* d = DefParSort(info->Id);
            if (!d)
                return;
            std::string code;
            if (!RefusLancement(j, d, code).empty())
                return;   // deja refuse par CheckCast ; un lancement force qui ne passe pas la garde ne fait rien
            Registre const avant = LireRegistre(j->GetGUID().GetCounter());
            bool demarre = false;
            if (d->genre == Genre::Jardin)
            {
                TempSummon* graine = j->GetMap()->SummonCreature(PNJ_GRAINE_TEMPETE, j->GetPosition(), nullptr,
                    (DUREE_JARDIN_S + 120) * IN_MILLISECONDS);
                GraineTempeteAI* ai = graine ? dynamic_cast<GraineTempeteAI*>(graine->AI()) : nullptr;
                if (ai)
                {
                    ai->Demarrer(j, avant);
                    demarre = true;
                }
                else if (graine)
                    graine->DespawnOrUnsummon();
            }
            else
            {
                GameObject* champ = j->GetMap()->SummonGameObject(GO_CHAMP_FLEURS_TOMBE, j->GetPositionX(), j->GetPositionY(),
                    j->GetPositionZ(), j->GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, DUREE_FLORAISON_S + 120);
                ChampFleursTombeAI* ai = champ ? dynamic_cast<ChampFleursTombeAI*>(champ->AI()) : nullptr;
                if (ai)
                {
                    ai->Demarrer(j, avant);
                    demarre = true;
                }
                else if (champ)
                {
                    champ->SetRespawnTime(0);
                    champ->Delete();
                }
            }
            if (!demarre)
            {
                // Rien n'est consomme. Pour le Jardin, la demande au Directeur est peut-etre deja partie (mod-playerbots la
                // voit au meme crochet) : ses attires viendraient sans Graine, sans recompense.
                LOG_ERROR("module", "coa sourcier: pivot non pose ou sans son script : lancement rendu evenement={}", d->code);
                ChatHandler(j->GetSession()).SendNotification("Rien ne germe ici.");
                AfficherRecharge(j, avant);
                return;
            }
            Registre r = avant;
            r.jourLance = JourDeJeu(Maintenant());
            r.relanceTs = 0;
            EcrireRegistre(j->GetGUID().GetCounter(), r);
            AfficherRecharge(j, r);
        }
    };

    // ---- Crochets d'unite : credits, morts ---------------------------------------------------------------------------

    class SourcierHerboUnites final : public UnitScript
    {
    public:
        SourcierHerboUnites() : UnitScript("CoaSourcierHerboristerieUnites", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH }) { }

        void OnDamage(Unit* attaquant, Unit* victime, uint32& degats) override
        {
            if (!degats || !victime || !victime->IsCreature() || !NombreActifs.load(std::memory_order_relaxed))
                return;
            uint32 const e = victime->GetEntry();
            if (e != PNJ_AME_ANCIENNE && !EstAttire(e))
                return;
            Player* joueur = attaquant ? attaquant->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (SessionHerbo* s = joueur ? SessionDe(victime) : nullptr)
                if (s->CrediteSur(victime))
                    s->Crediter(joueur, std::min(degats, uint32(victime->GetHealth())));
        }

        void OnHeal(Unit* soigneur, Unit* soigne, uint32& gain) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !gain || !soigneur || !soigne || !soigne->IsPlayer())
                return;
            Player* joueur = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (SessionHerbo* s = joueur ? SessionDe(soigne) : nullptr)
                s->Crediter(joueur, gain);
        }

        void OnUnitDeath(Unit* mort, Unit* tueur) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !mort)
                return;
            SessionHerbo* s = SessionDe(mort);
            if (!s)
                return;
            if (Creature* c = mort->ToCreature())
                s->CreatureMorte(c);
            else if (mort->IsPlayer() && s->EstSpecialiste(mort->GetGUID()))
            {
                Player const* joueur = (tueur && tueur != mort) ? tueur->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                s->SpecialisteMort(joueur && joueur != mort ? joueur : nullptr);
            }
        }
    };

    // ---- Le talent apprend le sort -----------------------------------------------------------------------------------

    void Synchroniser(Player* j, DefHerbo const& d)
    {
        if (!d.marqueur || !d.sort)
            return;
        bool const talent = j->HasSpell(d.marqueur);
        if (talent && !j->HasSpell(d.sort))
            j->learnSpell(d.sort);
        else if (!talent && j->HasSpell(d.sort))
            j->removeSpell(d.sort, SPEC_MASK_ALL, false);
    }

    class SourcierHerboJoueurs final : public PlayerScript
    {
    public:
        SourcierHerboJoueurs() : PlayerScript("CoaSourcierHerboristerieJoueurs", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEARN_SPELL,
            PLAYERHOOK_ON_FORGOT_SPELL }) { }

        void OnPlayerLogin(Player* j) override
        {
            if (!EstHumain(j))
                return;
            bool connu = false;
            for (DefHerbo const& d : EVENEMENTS)
            {
                Synchroniser(j, d);
                connu = connu || (d.sort && j->HasSpell(d.sort));
            }
            if (connu)
                AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
        }

        void OnPlayerLearnSpell(Player* j, uint32 sort) override
        {
            if (!EstHumain(j))
                return;   // un bot n'apprend jamais le sort lance (revue HERBORISTERIE 8)
            for (DefHerbo const& d : EVENEMENTS)
                if (d.marqueur && sort == d.marqueur)
                {
                    Synchroniser(j, d);
                    if (j->HasSpell(d.sort))
                        AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
                }
        }

        void OnPlayerForgotSpell(Player* j, uint32 sort) override
        {
            for (DefHerbo const& d : EVENEMENTS)
                if (d.marqueur && sort == d.marqueur)
                    Synchroniser(j, d);
        }
    };

    // ---- Le demarrage : donnees presentes ? ---------------------------------------------------------------------------

    class SourcierHerboMonde final : public WorldScript
    {
    public:
        SourcierHerboMonde() : WorldScript("CoaSourcierHerboristerieMonde", { WORLDHOOK_ON_STARTUP }) { }

        static bool GoAvecScript(uint32 entree, char const* script, uint32 type)
        {
            GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(entree);
            return go && go->type == type && go->ScriptId == sObjectMgr->GetScriptId(script);
        }

        void OnStartup() override
        {
            bool const sortJardin = sSpellMgr->GetSpellInfo(SORT_PLANTER_GRAINE_TEMPETE);
            CreatureTemplate const* graine = sObjectMgr->GetCreatureTemplate(PNJ_GRAINE_TEMPETE);
            bool const graineOk = graine && graine->ScriptID == sObjectMgr->GetScriptId(SCRIPT_GRAINE_TEMPETE);
            uint32 attires = 0;
            for (uint32 e : PNJ_ATTIRES_JARDIN)
                attires += sObjectMgr->GetCreatureTemplate(e) ? 1 : 0;
            bool const fleurOk = GoAvecScript(GO_FLEUR_TEMPETE_EPANOUIE, SCRIPT_FLEUR_TEMPETE, GAMEOBJECT_TYPE_CHEST);
            bool const fleurTempete = sObjectMgr->GetItemTemplate(OBJET_FLEUR_TEMPETE);
            Prets[0] = sortJardin && graineOk && attires == PNJ_ATTIRES_JARDIN.size() && fleurOk && fleurTempete;
            LOG_INFO("module", "coa sourcier: herboristerie evenement=jardin_orage sort {} ({}) marqueur {} ; graine {} ({}) ; attires {}/{} ; "
                "fleur epanouie {} ({}) ; materiau {} ({}) ; {}", SORT_PLANTER_GRAINE_TEMPETE, sortJardin ? "present" : "ABSENT",
                SORT_MARQUEUR_JARDIN, PNJ_GRAINE_TEMPETE, graineOk ? "ok" : "ABSENTE ou sans script", attires, PNJ_ATTIRES_JARDIN.size(),
                GO_FLEUR_TEMPETE_EPANOUIE, fleurOk ? "ok" : "ABSENTE ou sans script", OBJET_FLEUR_TEMPETE, fleurTempete ? "present" : "ABSENT",
                Prets[0] ? "PRET" : "INERTE");

            bool const sortFloraison = sSpellMgr->GetSpellInfo(SORT_FAIRE_ECLORE_TOMBES);
            bool const sortCapture = sSpellMgr->GetSpellInfo(SORT_CAPTURER_AME);
            bool const champOk = GoAvecScript(GO_CHAMP_FLEURS_TOMBE, SCRIPT_CHAMP_FLEURS_TOMBE, GAMEOBJECT_TYPE_GENERIC);
            bool const ameOk = GoAvecScript(GO_AME_EGAREE, SCRIPT_AME_EGAREE, GAMEOBJECT_TYPE_CHEST);
            GameObjectTemplate const* ameGo = sObjectMgr->GetGameObjectTemplate(GO_AME_EGAREE);
            bool const verrouOk = ameGo && ameGo->GetLockId() == VERROU_AME_EGAREE && sLockStore.LookupEntry(VERROU_AME_EGAREE);
            bool const ancienne = sObjectMgr->GetCreatureTemplate(PNJ_AME_ANCIENNE);
            bool const fleurEternite = sObjectMgr->GetItemTemplate(OBJET_FLEUR_ETERNITE);
            Prets[1] = sortFloraison && sortCapture && champOk && ameOk && verrouOk && ancienne && fleurEternite;
            LOG_INFO("module", "coa sourcier: herboristerie evenement=floraison_tombes sort {} ({}) marqueur {} ; capture {} ({}) ; champ {} ({}) ; "
                "ame egaree {} ({}, verrou {} {}) ; ame ancienne {} ({}) ; materiau {} ({}) ; cimetieres {} ; {}", SORT_FAIRE_ECLORE_TOMBES,
                sortFloraison ? "present" : "ABSENT", SORT_MARQUEUR_FLORAISON, SORT_CAPTURER_AME, sortCapture ? "present" : "ABSENT",
                GO_CHAMP_FLEURS_TOMBE, champOk ? "ok" : "ABSENT ou sans script", GO_AME_EGAREE, ameOk ? "ok" : "ABSENTE ou sans script",
                VERROU_AME_EGAREE, verrouOk ? "ok" : "ABSENT", PNJ_AME_ANCIENNE, ancienne ? "presente" : "ABSENTE", OBJET_FLEUR_ETERNITE,
                fleurEternite ? "present" : "ABSENT", sGraveyard->GetGraveyardData().size(), Prets[1] ? "PRET" : "INERTE");
        }
    };
}

void AddAscensionArtisanatSourcierHerboristerieScripts()
{
    new npc_coa_graine_tempete();
    new go_coa_fleur_tempete();
    new go_coa_champ_fleurs_tombe();
    new go_coa_ame_egaree();
    new SourcierHerboSorts();
    new SourcierHerboUnites();
    new SourcierHerboJoueurs();
    new SourcierHerboMonde();
}
