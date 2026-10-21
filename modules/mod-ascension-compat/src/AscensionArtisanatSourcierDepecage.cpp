/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T2 du Sourcier au 60 (21/10/2026), lot DEPECAGE : la Traque legendaire et la Lame de trophee.
// Conception : coa-project docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2.3 et § 2 bis (branche t2-sourcier). Ids :
// AscensionArtisanatIds.h, section « lot DEPECAGE ». Socle commun (registre, remises, marques, exclusivite) :
// AscensionArtisanatSourcierSocle.h. L'Appel du Linceul (voie Directeur) est dans AscensionArtisanatSourcierDirecteur.cpp ;
// l'option TROPHEE de Re-signer dans AscensionArtisanat.cpp (Resigner). NOUVEAU .cpp : `amont/construire.py --cible ptr
// --configurer` (ou cmake) AVANT le build (glob sans CONFIGURE_DEPENDS).
//
// TRAQUE LEGENDAIRE (voie module seule). Le sort 9113904, lance dans une zone de ZONES_TRAQUE, pose la Piste de legende
// (objet 9101096, pivot, decor) sous le lanceur ; son IA porte la session, sur le fil de sa carte (P-137) :
//   - le pivot est rendu ACTIF (WorldObject::setActive) : le groupe s'en eloigne de 3 x INDICE_ECART_MAX au plus, et un
//     objet hors de portee de vue de tout joueur n'est plus mis a jour (GameObject::IsUpdateNeeded, Map.cpp
//     UpdateNonPlayerObjects) : sans cela, l'echeance et la surveillance s'arreteraient ;
//   - INDICES_TRAQUE indices (objet 9101091, coffre sans verrou ni butin), un a la fois, chacun a INDICE_ECART_MIN-MAX m du
//     precedent, au sol (MovePosition), hors de l'eau, dans une zone de traque. Visibles du SEUL groupe du specialiste
//     (GameObjectAI::CanBeSeen, lu par WorldObject::CanSeeOrDetect, Object.cpp:1839) ; un clic d'un membre l'examine
//     (GossipHello) et fait paraitre le suivant ; le groupe recoit la direction et la distance ;
//   - au dernier, la Bete de legende (un des 6 gabarits 9105030-9105035, tire au sort) parait pres de lui, visible du seul
//     groupe (CreatureAI::CanBeSeen, Object.cpp:1826), n'engage que lui, elite 60, PV par la table du Colosse ;
//   - victoire : sa mort -> Peau de legende au specialiste (garanti / 25 %), recompenses de participant (Glas) ; son
//     cadavre reste, et se depece (butin de depecage de son gabarit) ;
//   - echec : DUREE_TRAQUE_S, regles du Filon-mere (specialiste absent, groupe mort, deconnexion), bete disparue vivante.
//
// LAME DE TROPHEE (case sourcier-c du Depecage, marqueur 9113425). Crochet de butin (MiscScript, apres le tirage du
// modele, comme ArtisanatButinRecolte) : un humain qui depece une creature rare (rang 2, 4) ou elite (1, 3) recoit
// 1 trophee (croc : bete ; ecaille : draconien ; corne : autre). Garde : jamais sur une creature grise pour lui, jamais sur
// un elite d'instance (un rare d'instance, rang 4, compte), TROPHEES_PAR_JOUR au plus (table coa_artisanat_trophee,
// migration data/sql/db-characters/2026_10_21_01_coa_artisanat_trophee.sql ; absente : la Lame est INERTE). Le trophee est
// compte quand il est POSE au butin, pas quand il est ramasse.
//
// Integration T2 (21/10, corrections des revues) : un indice ou une bete qui ne peut pas etre pose (frontiere de zone, eau)
// REND le lancement au lieu de le consommer ; un indice peut tomber dans la zone de traque de l'indice precedent ou dans
// une zone de traque ; bots refuses ; Peau remise par la file commune ; session persistee (un redemarrage rend le
// lancement). Les Betes de legende ne s'apprivoisent pas (type_flags 0, generateur).
// Journal : « coa sourcier: ... evenement=traque_legendaire pivot=<guid> ... » et « coa sourcier: trophee ... ». Au
// demarrage : « coa sourcier: depecage evenement=traque_legendaire ... PRET|INERTE » et « coa sourcier: lame de trophee ...
// ACTIVE|INERTE ». Banc (PTR seulement) : `.sourcier banc trophees <nom> [remettre]`.
//
// INERTE tant que ses donnees manquent (gabarits avec ScriptName, materiau, sort) : le sort est refuse avec « pas encore
// disponible », rien n'apparait.

#include "AscensionArtisanatIds.h"
#include "AscensionArtisanatSourcierSocle.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Formulas.h"
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
#include "Optional.h"
#include "Player.h"
#include "Random.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <vector>

using namespace Acore::ChatCommands;
using namespace CoaArtisanatIds;
using namespace CoaSourcier;

namespace
{
    constexpr char const* CODE = "traque_legendaire";
    constexpr char const* EXPEDITEUR = "La Traque légendaire";
    constexpr uint32 ROYAUME_PTR = 2;       // comme AscensionArtisanatSourcier.cpp (banc du Sourcier : PTR seulement)
    // Le groupe s'eloigne du pivot de 3 x INDICE_ECART_MAX au plus : la presence se juge autour du REPERE (indice courant,
    // puis la bete), a RAYON_PARTICIPATION plus la distance entre deux indices.
    constexpr float RAYON_TRAQUE = RAYON_PARTICIPATION + INDICE_ECART_MAX;

    // Les Betes de legende : gabarit, phrase dite au groupe a leur apparition (conception § 2.3 : « nom et phrase propres »).
    struct DefBete
    {
        uint32 entree;
        char const* phrase;
    };
    constexpr std::array<DefBete, 6> BETES =
    {{
        { 9105030, "Hurlecendre, le loup de légende, jaillit des fourrés !" },
        { 9105031, "Vieux-Mufle, l'ours de légende, se dresse de toute sa hauteur !" },
        { 9105032, "Ombre-de-givre, la panthère de légende, bondit sur vous !" },
        { 9105033, "Croc-d'ambre, le raptor de légende, siffle et charge !" },
        { 9105034, "Aile-de-brume, la chimère de légende, fond sur vous en hurlant !" },
        { 9105035, "Mâchoire-tyran, le diablosaure de légende, fait trembler le sol !" },
    }};
    static_assert(BETES.size() == PNJ_BETES_LEGENDE.size(), "une phrase par Bete de legende");

    bool Pret = false;              // donnees de la Traque presentes (fil monde, au demarrage, avant tout sort)
    bool LameActive = false;        // table des trophees et objets presents
    bool TableTrophees = false;

    bool ZoneDeTraque(uint32 zone)
    {
        return std::find(ZONES_TRAQUE.begin(), ZONES_TRAQUE.end(), zone) != ZONES_TRAQUE.end();
    }

    bool EstBete(uint32 entree)
    {
        return std::find(PNJ_BETES_LEGENDE.begin(), PNJ_BETES_LEGENDE.end(), entree) != PNJ_BETES_LEGENDE.end();
    }

    char const* PhraseDe(uint32 entree)
    {
        for (DefBete const& b : BETES)
            if (b.entree == entree)
                return b.phrase;
        return "Une bête de légende surgit !";
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

    // Direction cardinale de (dx, dy) en coordonnees du monde (+x = nord, +y = ouest dans WoW).
    char const* Direction(float dx, float dy)
    {
        static constexpr std::array<char const*, 8> NOMS = { "le nord", "le nord-ouest", "l'ouest", "le sud-ouest", "le sud",
                                                            "le sud-est", "l'est", "le nord-est" };
        float a = std::atan2(dy, dx);   // 0 = nord (+x), +pi/2 = ouest (+y)
        if (a < 0.0f)
            a += 2.0f * float(M_PI);
        uint32 const k = uint32(std::lround(a / (float(M_PI) / 4.0f))) % 8;
        return NOMS[k];
    }

    // ---- L'indice et la bete : vus du seul groupe de la piste --------------------------------------------------------
    //
    // Definis AVANT la piste (elle les pose et les ouvre). Chacun garde SA copie de qui le voit (specialiste, groupe),
    // tenue a jour par la piste : le cadavre de la bete doit rester visible du groupe (pour le depecer) apres la fin de
    // la piste, qui n'existe plus. L'examen d'un indice passe par une fonction definie apres la piste.
    bool ExaminerPar(GameObject* indice, ObjectGuid pivot, Player* j);

    // Qui voit : le specialiste, les membres de son groupe (bots compris : ils se battent), un MJ. Personne tant que
    // l'objet n'est pas ouvert (specialiste vide : pose puis Ouvrir, meme fil, meme instant).
    bool Voit(Player const* p, ObjectGuid specialiste, ObjectGuid groupe)
    {
        if (!p || !specialiste)
            return false;
        if (p->IsGameMaster() || p->GetGUID() == specialiste)
            return true;
        Group const* g = p->GetGroup();
        return groupe && g && g->GetGUID() == groupe;
    }

    // L'indice : examine par un clic.

    class IndiceTraqueAI final : public GameObjectAI
    {
    public:
        explicit IndiceTraqueAI(GameObject* go) : GameObjectAI(go) { }

        void Ouvrir(ObjectGuid pivot, ObjectGuid specialiste, ObjectGuid groupe)
        {
            _pivot = pivot;
            _specialiste = specialiste;
            _groupe = groupe;
            me->UpdateObjectVisibility();
        }

        void MajGroupe(ObjectGuid groupe)
        {
            _groupe = groupe;
            me->UpdateObjectVisibility();
        }

        bool CanBeSeen(Player const* seer) override
        {
            return Voit(seer, _specialiste, _groupe);
        }

        bool GossipHello(Player* joueur, bool reportUse) override
        {
            // CMSG_GAMEOBJ_REPORT_USE rappelle GossipHello apres GameObject::Use (SpellHandler.cpp:370) : un clic, un examen.
            if (reportUse || !joueur)
                return true;
            if (!ExaminerPar(me, _pivot, joueur))
                ChatHandler(joueur->GetSession()).SendNotification("Ces traces ne vous disent rien.");
            return true;
        }

    private:
        ObjectGuid _pivot, _specialiste, _groupe;
    };

    // ---- La Bete de legende : vue du seul groupe, n'engage que lui ---------------------------------------------------------

    class BeteLegendeAI final : public ScriptedAI
    {
    public:
        explicit BeteLegendeAI(Creature* c) : ScriptedAI(c) { }

        void Ouvrir(ObjectGuid specialiste, ObjectGuid groupe)
        {
            _specialiste = specialiste;
            _groupe = groupe;
            me->UpdateObjectVisibility();
        }

        void MajGroupe(ObjectGuid groupe)
        {
            _groupe = groupe;
            me->UpdateObjectVisibility();
        }

        bool CanBeSeen(Player const* seer) override
        {
            return Voit(seer, _specialiste, _groupe);
        }

        // N'engage que le groupe (et ses familiers) : un passant qui ne la voit pas n'est jamais attaque.
        bool Permis(Unit const* u) const
        {
            Player const* p = u ? u->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            return p && Voit(p, _specialiste, _groupe);
        }

        void MoveInLineOfSight(Unit* qui) override
        {
            if (Permis(qui))
                ScriptedAI::MoveInLineOfSight(qui);
        }

        void UpdateAI(uint32 /*diff*/) override
        {
            if (!UpdateVictim())
                return;
            DoMeleeAttackIfReady();
        }

    private:
        ObjectGuid _specialiste, _groupe;
    };

    // ---- La session, dans l'IA de la Piste de legende (pivot) ----------------------------------------------------------

    class PisteLegendeAI final : public GameObjectAI
    {
    public:
        explicit PisteLegendeAI(GameObject* go) : GameObjectAI(go) { }

        ~PisteLegendeAI() override
        {
            RetirerActif(_pivot);   // carte dechargee, despawn du filet : l'exclusivite ne survit pas au pivot
        }

        enum class Phase : uint8 { Attente, Indices, Bete, Fini };

        // Fil de la carte, au lancement. -> faux si le premier indice n'a pas pu etre pose (rien n'est alors consomme).
        bool Demarrer(Player* specialiste, Registre const& avant)
        {
            _pivot = me->GetGUID();
            _specialiste = specialiste->GetGUID();
            _equipe = specialiste->GetTeamId();
            _debut = Maintenant();
            _groupe = specialiste->GetGroup() ? specialiste->GetGroup()->GetGUID() : ObjectGuid::Empty;
            _avantJour = avant.jourLance;
            _avantRelance = avant.relanceTs;
            _repere = me->GetPosition();
            _phase = Phase::Indices;
            me->setActive(true);   // mis a jour loin de tout joueur (voir l'en-tete)
            if (!PoserIndice(me->GetPosition()))
            {
                _phase = Phase::Fini;
                me->setActive(false);
                return false;
            }
            Marquer(specialiste, _pivot);
            InscrireActif(me);
            OuvrirSession(_pivot, _specialiste, CODE, avant);
            LOG_INFO("module", "coa sourcier: lancement evenement={} specialiste={} pivot={} carte={} zone={} x={:.1f} y={:.1f} duree_s={} indices={}",
                CODE, _specialiste.GetCounter(), _pivot.GetCounter(), me->GetMapId(), me->GetZoneId(), me->GetPositionX(),
                me->GetPositionY(), DUREE_TRAQUE_S, INDICES_TRAQUE);
            DireAuGroupe("Une piste légendaire s'ouvre devant vous : suivez les indices !");
            return true;
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

        // Qui examine un indice : ceux qui le voient (Voit), tant que la piste court.
        bool Autorise(Player const* p) const
        {
            return _phase != Phase::Fini && Voit(p, _specialiste, _groupe);
        }

        // Un indice examine (GossipHello de l'indice, fil de la carte). -> vrai si l'examen compte.
        bool Examiner(Player* j, GameObject* indice)
        {
            if (_phase != Phase::Indices || !j || !indice || indice->GetGUID() != _indice || !j->IsAlive() || !Autorise(j))
                return false;
            Position const ici = indice->GetPosition();
            indice->SetRespawnTime(0);
            indice->Delete();
            _indice.Clear();
            ++_examines;
            if (EstHumain(j))
                _acteurs.insert(j->GetGUID());
            Marquer(j, _pivot);
            LOG_INFO("module", "coa sourcier: indice evenement={} pivot={} joueur={} examines={}/{}", CODE, _pivot.GetCounter(),
                j->GetGUID().GetCounter(), _examines, INDICES_TRAQUE);
            if (_examines >= INDICES_TRAQUE)
            {
                LeverBete(ici, j);
                return true;
            }
            if (!PoserIndice(ici))
                Rendre("indice_absent");   // le terrain, pas le joueur : le lancement est rendu (revue DEPECAGE 3)
            return true;
        }

        bool EstSpecialiste(ObjectGuid g) const { return g && g == _specialiste; }

        void Crediter(Player* joueur, uint32 montant)
        {
            if (!joueur || !_creditOuvert || _phase != Phase::Bete)
                return;
            _credits[joueur->GetGUID()] += montant;
            Marquer(joueur, _pivot);
        }

        bool EstBeteDeSession(ObjectGuid g) const { return g && g == _bete; }

        void BeteMorte()
        {
            if (_phase == Phase::Bete)
                Reussir();
        }

        void SpecialisteMort(Player const* tueur)
        {
            bool const humain = EstHumain(tueur);
            bool const adverse = tueur && tueur->GetTeamId() != _equipe;
            _specialisteMortA = Maintenant();
            _mortParJoueur = humain && adverse;
            LOG_INFO("module", "coa sourcier: specialiste mort evenement={} pivot={} specialiste={} tueur_joueur={} humain={} adverse={} par_joueur={}",
                CODE, _pivot.GetCounter(), _specialiste.GetCounter(), tueur ? tueur->GetGUID().GetCounter() : 0, humain ? 1 : 0,
                adverse ? 1 : 0, _mortParJoueur ? 1 : 0);
        }

    private:
        void Seconde()
        {
            time_t const maintenant = Maintenant();
            Player* s = ObjectAccessor::GetPlayer(*me, _specialiste);
            if (!s && !ObjectAccessor::FindConnectedPlayer(_specialiste))
                return Echouer("deconnexion", false);
            if (s && s->GetGroup() && s->GetGroup()->GetGUID() != _groupe)
            {
                _groupe = s->GetGroup()->GetGUID();   // un groupe forme apres le lancement compte aussi : indice et bete
                PropagerGroupe();                     // le voient des maintenant
            }
            bool const present = s && s->IsAlive() && s->IsWithinDist2d(&_repere, RAYON_TRAQUE);
            if (present)
            {
                _specialisteMortA = 0;
                _mortParJoueur = false;
            }
            else if (_specialisteMortA && !_mortParJoueur && maintenant - _specialisteMortA > time_t(ABSENCE_MAX_S))
                return Echouer("specialiste", false);
            if (!present && !GroupeEnVie())
                return Echouer("groupe", _mortParJoueur);
            if (maintenant - _debut >= time_t(DUREE_TRAQUE_S))
                return Echouer("temps", _specialisteMortA && _mortParJoueur);
            if (_phase == Phase::Indices && _indice && !ObjectAccessor::GetGameObject(*me, _indice))
                return Echouer("indice_disparu", false);
            if (_phase == Phase::Bete)
            {
                Creature* c = ObjectAccessor::GetCreature(*me, _bete);
                if (!c)
                    return Echouer("bete_disparue", false);
                if (!c->IsAlive())
                    return Reussir();
                _repere = c->GetPosition();
            }
        }

        void PropagerGroupe()
        {
            if (GameObject* i = ObjectAccessor::GetGameObject(*me, _indice))
                if (IndiceTraqueAI* ai = dynamic_cast<IndiceTraqueAI*>(i->AI()))
                    ai->MajGroupe(_groupe);
            if (Creature* c = ObjectAccessor::GetCreature(*me, _bete))
                if (BeteLegendeAI* ai = dynamic_cast<BeteLegendeAI*>(c->AI()))
                    ai->MajGroupe(_groupe);
        }

        // Le specialiste vivant sur la carte, ou un membre de son groupe vivant pres du repere (carte du pivot, P-137).
        bool GroupeEnVie()
        {
            for (MapReference const& ref : me->GetMap()->GetPlayers())
                if (Player* m = ref.GetSource())
                {
                    if (!m->IsAlive())
                        continue;
                    if (m->GetGUID() == _specialiste)
                        return true;
                    if (_groupe && m->GetGroup() && m->GetGroup()->GetGUID() == _groupe && m->IsWithinDist2d(&_repere, RAYON_TRAQUE))
                        return true;
                }
            return false;
        }

        // Un point a INDICE_ECART_MIN-MAX m de `depuis`, au sol, hors de l'eau, dans une zone de traque, hors capitale et
        // sanctuaire ; 8 essais a pleine distance, puis 4 a distance reduite ; jamais a moins de la moitie de l'ecart minimal
        // (MovePosition rend le point de depart quand la pente l'arrete : Object.cpp:2917-2934).
        bool ChercherPoint(Position const& depuis, Position& point)
        {
            Map* carte = me->GetMap();
            uint32 zoneDepart = 0, aireDepart = 0;
            carte->GetZoneAndAreaId(me->GetPhaseMask(), zoneDepart, aireDepart, depuis.GetPositionX(), depuis.GetPositionY(),
                depuis.GetPositionZ());
            if (!ZoneDeTraque(zoneDepart))
                zoneDepart = 0;
            for (uint32 essai = 0; essai < 12; ++essai)
            {
                float const reduit = essai < 8 ? 1.0f : 0.5f;
                Position pos = depuis;
                me->MovePosition(pos, frand(INDICE_ECART_MIN, INDICE_ECART_MAX) * reduit, float(rand_norm()) * 2.0f * float(M_PI));
                if (pos.GetExactDist2d(&depuis) < INDICE_ECART_MIN * 0.5f)
                    continue;
                if (carte->IsInWater(me->GetPhaseMask(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), 2.0f))
                    continue;
                uint32 zone = 0, aire = 0;
                carte->GetZoneAndAreaId(me->GetPhaseMask(), zone, aire, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
                // Une zone de traque, ou celle du point de depart (le joueur l'a lance dans une zone de traque, et la piste
                // peut longer une frontiere : revue DEPECAGE 3).
                if (!ZoneDeTraque(zone) && zone != zoneDepart)
                    continue;
                AreaTableEntry const* z = sAreaTableStore.LookupEntry(zone);
                AreaTableEntry const* a = sAreaTableStore.LookupEntry(aire);
                if (((z ? z->flags : 0) | (a ? a->flags : 0)) & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY))
                    continue;
                point = pos;
                return true;
            }
            return false;
        }

        bool PoserIndice(Position const& depuis)
        {
            Position pos;
            if (!ChercherPoint(depuis, pos))
            {
                LOG_INFO("module", "coa sourcier: indice non pose evenement={} pivot={} raison=sol_eau_ou_zone examines={}", CODE,
                    _pivot.GetCounter(), _examines);
                return false;
            }
            uint32 const reste = uint32(std::max<int64>(60, int64(DUREE_TRAQUE_S) - int64(Maintenant() - _debut)));
            GameObject* indice = me->GetMap()->SummonGameObject(GO_INDICE_TRAQUE, pos.GetPositionX(), pos.GetPositionY(),
                pos.GetPositionZ(), pos.GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, reste + 60);
            IndiceTraqueAI* ai = indice ? dynamic_cast<IndiceTraqueAI*>(indice->AI()) : nullptr;
            if (!ai)
            {
                LOG_ERROR("module", "coa sourcier: indice {} non pose ou sans le script {} pivot={}", GO_INDICE_TRAQUE, SCRIPT_INDICE_TRAQUE,
                    _pivot.GetCounter());
                if (indice)
                {
                    indice->SetRespawnTime(0);
                    indice->Delete();
                }
                return false;
            }
            ai->Ouvrir(_pivot, _specialiste, _groupe);
            _indice = indice->GetGUID();
            _repere = pos;
            float const d = pos.GetExactDist2d(&depuis);
            DireAuGroupe(Acore::StringFormat("Indice {} sur {} : la piste part vers {}, à {} mètres environ.", _examines + 1,
                INDICES_TRAQUE, Direction(pos.GetPositionX() - depuis.GetPositionX(), pos.GetPositionY() - depuis.GetPositionY()),
                uint32(std::lround(d / 10.0f)) * 10));
            LOG_INFO("module", "coa sourcier: indice pose evenement={} pivot={} indice={} rang={} distance={:.0f} x={:.1f} y={:.1f}", CODE,
                _pivot.GetCounter(), _indice.GetCounter(), _examines + 1, d, pos.GetPositionX(), pos.GetPositionY());
            return true;
        }

        void LeverBete(Position const& ici, Player* declencheur)
        {
            DefBete const& b = BETES[urand(0, uint32(BETES.size()) - 1)];
            Position pos = ici;
            me->MovePosition(pos, 8.0f, float(rand_norm()) * 2.0f * float(M_PI));
            uint32 const reste = uint32(std::max<int64>(60, int64(DUREE_TRAQUE_S) - int64(Maintenant() - _debut)));
            TempSummon* bete = me->GetMap()->SummonCreature(b.entree, pos, nullptr, (reste + 300) * IN_MILLISECONDS);
            BeteLegendeAI* ai = bete ? dynamic_cast<BeteLegendeAI*>(bete->AI()) : nullptr;
            if (!ai)
            {
                LOG_ERROR("module", "coa sourcier: bete {} non invoquee ou sans le script {} pivot={} : echec", b.entree, SCRIPT_BETE_LEGENDE,
                    _pivot.GetCounter());
                if (bete)
                    bete->DespawnOrUnsummon();
                return Rendre("bete_absente");   // panne de donnees, pas faute du joueur (revue DEPECAGE 3)
            }
            // Calibrage reduit (table du Colosse) : PV en % du modele selon les humains du groupe presents a 40 m.
            uint32 humains = 1;
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
                humains = std::max<uint32>(1, HumainsDuGroupe(s, RAYON_DOSAGE));
            uint8 const pct = PV_COLOSSE_POUR_CENT[std::min<std::size_t>(humains, PV_COLOSSE_POUR_CENT.size()) - 1];
            float const base = bete->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE);
            if (base > 0.0f && pct < 100)
            {
                float const dosee = std::max(1.0f, std::round(base * float(pct) / 100.0f));
                bete->SetCreateHealth(uint32(dosee));
                bete->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, dosee);
            }
            bete->SetHealth(bete->GetMaxHealth());
            _bete = bete->GetGUID();
            _repere = bete->GetPosition();
            _seuilParticipation = std::max<uint32>(1, uint32(uint64(bete->GetMaxHealth()) * PART_PARTICIPATION_POUR_MILLE / 1000));
            _creditOuvert = true;
            _phase = Phase::Bete;
            Marquer(bete, _pivot);
            ai->Ouvrir(_specialiste, _groupe);
            if (declencheur && bete->IsAIEnabled)
                ai->AttackStart(declencheur);
            DireAuGroupe(PhraseDe(b.entree));
            LOG_INFO("module", "coa sourcier: bete evenement={} pivot={} bete={} entree={} humains_presents={} pv_pct={} pv={} seuil_participation={}",
                CODE, _pivot.GetCounter(), _bete.GetCounter(), b.entree, humains, uint32(pct), bete->GetMaxHealth(), _seuilParticipation);
        }

        // Participants humains de la faction : groupe du specialiste present pres du repere, credites (2 % des PV de la
        // bete, infliges ou soignes), et ceux qui ont examine un indice. -> joueurs presents sur la carte, et leurs comptes.
        std::vector<Player*> Participants(std::set<uint32>& comptes)
        {
            std::set<ObjectGuid> participants(_acteurs.begin(), _acteurs.end());
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
            {
                participants.insert(s->GetGUID());
                if (Group* g = s->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (EstHumain(m) && m->IsInWorld() && m->GetMap() == me->GetMap() && m->IsWithinDist2d(&_repere, RAYON_PARTICIPATION))
                                participants.insert(m->GetGUID());
            }
            for (auto const& [g, montant] : _credits)
                if (montant >= _seuilParticipation)
                    participants.insert(g);
            std::vector<Player*> humains;
            for (ObjectGuid g : participants)
                if (Player* p = ObjectAccessor::GetPlayer(*me, g))
                    if (EstHumain(p) && p->GetTeamId() == _equipe)
                    {
                        humains.push_back(p);
                        comptes.insert(p->GetSession()->GetAccountId());
                    }
            return humains;
        }

        void Reussir()
        {
            if (_phase != Phase::Bete)
                return;
            _phase = Phase::Fini;   // une seule fois, meme si le crochet de mort et le constat arrivent dans la meme seconde
            time_t const maintenant = Maintenant();
            int64 const jour = JourDeJeu(maintenant);
            int64 const semaine = SemaineDeJeu(jour);
            std::set<uint32> comptes;
            std::vector<Player*> humains = Participants(comptes);
            LOG_INFO("module", "coa sourcier: succes evenement={} pivot={} specialiste={} participants_humains={} comptes={} duree_s={}",
                CODE, _pivot.GetCounter(), _specialiste.GetCounter(), uint32(humains.size()), uint32(comptes.size()),
                uint32(maintenant - _debut));

            Player* s = ObjectAccessor::GetPlayer(*me, _specialiste);
            Registre r = LireRegistre(_specialiste.GetCounter());
            DecisionCoeur const d = DeciderCoeur(r, semaine, uint32(comptes.size()));
            EcrireRegistre(_specialiste.GetCounter(), r);
            bool remis = false;
            char const* voie = "aucune";
            if (d.obtenu)
                voie = RemettreMateriau(s, _specialiste, OBJET_PEAU_LEGENDE, EXPEDITEUR, remis);
            LOG_INFO("module", "coa sourcier: remise evenement={} pivot={} specialiste={} materiau={} garanti={} obtenu={} remis={} voie={} semaine={} comptes={}",
                CODE, _pivot.GetCounter(), _specialiste.GetCounter(), OBJET_PEAU_LEGENDE, d.garanti ? 1 : 0, d.obtenu ? 1 : 0,
                remis ? 1 : 0, voie, int32(semaine), uint32(comptes.size()));
            if (s)
                ChatHandler(s->GetSession()).SendNotification(d.obtenu ? "La bête de légende est tombée : sa Peau de légende est à vous."
                    : "La bête de légende est tombée.");

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
                    LOG_INFO("module", "coa sourcier: recompense refusee evenement={} pivot={} joueur={} raison=plafond_du_jour", CODE,
                        _pivot.GetCounter(), p->GetGUID().GetCounter());
                    continue;
                }
                ++rp.recompN;
                EcrireRegistre(p->GetGUID().GetCounter(), rp);
                if (p->GetGUID() == _specialiste)
                    r = rp;
                uint32 const rare = urand(1, 100) <= CHANCE_RARE_POUR_CENT ? 1 : 0;
                uint32 const epique = urand(1, 1000) <= CHANCE_EPIQUE_POUR_MILLE ? 1 : 0;
                Donner(p, RESSOURCE_GLAS_VERTE, RESSOURCES_VERTES, ObjectGuid::Empty, EXPEDITEUR);
                if (rare)
                    Donner(p, RESSOURCE_GLAS_RARE, 1, ObjectGuid::Empty, EXPEDITEUR);
                if (epique)
                    Donner(p, RESSOURCE_GLAS_EPIQUE, 1, ObjectGuid::Empty, EXPEDITEUR);
                LOG_INFO("module", "coa sourcier: recompense evenement={} pivot={} joueur={} vertes={} rare={} epique={} du_jour={}/{}", CODE,
                    _pivot.GetCounter(), p->GetGUID().GetCounter(), RESSOURCES_VERTES, rare, epique, rp.recompN, RECOMPENSES_PAR_JOUR);
            }
            Terminer("succes");
        }

        // Echec : relance immediate une fois par jour si un humain adverse l'a cause, sinon au bout de 4 h une fois par jour.
        void Echouer(char const* raison, bool parJoueur)
        {
            if (_phase == Phase::Fini || _phase == Phase::Attente)
                return;
            _phase = Phase::Fini;
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
                ChatHandler(s->GetSession()).SendNotification(immediate ? "La piste se perd : vous pourrez en suivre une autre tout de suite."
                    : "La piste légendaire se perd.");
            }
            LOG_INFO("module", "coa sourcier: echec evenement={} pivot={} specialiste={} raison={} par_joueur={} relance={} examines={} bete={}",
                CODE, _pivot.GetCounter(), _specialiste.GetCounter(), raison, parJoueur ? 1 : 0, relance, _examines, _bete ? 1 : 0);
            Terminer(raison);
        }

        // Panne de terrain ou de donnees (indice ou bete impossibles a poser) : les deux champs de LANCEMENT du registre
        // reprennent leur valeur d'avant ce lancement (meme regle que le refus du Directeur, RendreCommun).
        void Rendre(char const* raison)
        {
            if (_phase == Phase::Fini || _phase == Phase::Attente)
                return;
            _phase = Phase::Fini;
            Registre r = LireRegistre(_specialiste.GetCounter());
            r.jourLance = _avantJour;
            r.relanceTs = _avantRelance;
            EcrireRegistre(_specialiste.GetCounter(), r);
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
            {
                AfficherRecharge(s, r);
                ChatHandler(s->GetSession()).SendNotification("La piste se perd dans un terrain impraticable : vous pourrez en suivre une autre.");
            }
            LOG_INFO("module", "coa sourcier: rendu evenement={} pivot={} specialiste={} raison={} examines={}", CODE, _pivot.GetCounter(),
                _specialiste.GetCounter(), raison, _examines);
            Terminer(raison);
        }

        // La bete vivante (echec) s'en va ; son cadavre (succes) reste a son minuteur, et se depece. L'indice courant et la
        // piste disparaissent.
        void Terminer(char const* raison)
        {
            _phase = Phase::Fini;
            bool beteRetiree = false;
            if (Creature* c = ObjectAccessor::GetCreature(*me, _bete))
                if (c->IsAlive())
                {
                    c->DespawnOrUnsummon();
                    beteRetiree = true;
                }
            if (GameObject* i = ObjectAccessor::GetGameObject(*me, _indice))
            {
                i->SetRespawnTime(0);
                i->Delete();
            }
            RetirerActif(_pivot);
            FermerSession(_pivot, raison);
            LOG_INFO("module", "coa sourcier: nettoyage evenement={} pivot={} raison={} examines={} bete_retiree={}", CODE, _pivot.GetCounter(),
                raison, _examines, beteRetiree ? 1 : 0);
            me->setActive(false);
            me->SetRespawnTime(0);
            me->Delete();
        }

        // Message systeme au specialiste et aux membres humains de son groupe sur la carte du pivot.
        void DireAuGroupe(std::string const& texte)
        {
            std::set<ObjectGuid> dits;
            if (Player* s = ObjectAccessor::GetPlayer(*me, _specialiste))
            {
                ChatHandler(s->GetSession()).SendSysMessage(texte);
                dits.insert(s->GetGUID());
                if (Group* g = s->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (EstHumain(m) && !dits.count(m->GetGUID()) && m->IsInWorld() && m->GetMap() == me->GetMap())
                            {
                                ChatHandler(m->GetSession()).SendSysMessage(texte);
                                dits.insert(m->GetGUID());
                            }
            }
        }

        Phase _phase = Phase::Attente;
        ObjectGuid _pivot, _specialiste, _groupe, _indice, _bete;
        TeamId _equipe = TEAM_NEUTRAL;
        time_t _debut = 0, _specialisteMortA = 0;
        int64 _avantJour = 0;
        uint32 _avantRelance = 0;
        bool _mortParJoueur = false, _creditOuvert = false;
        uint32 _cumul = 0, _examines = 0, _seuilParticipation = 1;
        Position _repere;
        std::map<ObjectGuid, uint32> _credits;
        std::set<ObjectGuid> _acteurs;
    };

    PisteLegendeAI* PisteDe(WorldObject const* objet, ObjectGuid pivot)
    {
        if (!objet || !pivot.IsGameObject())
            return nullptr;
        GameObject* go = ObjectAccessor::GetGameObject(*objet, pivot);
        return go ? dynamic_cast<PisteLegendeAI*>(go->AI()) : nullptr;
    }

    PisteLegendeAI* SessionDe(Unit const* u)
    {
        return u ? PisteDe(u, PivotDe(u)) : nullptr;
    }

    bool ExaminerPar(GameObject* indice, ObjectGuid pivot, Player* j)
    {
        PisteLegendeAI* piste = PisteDe(indice, pivot);
        return piste && piste->Examiner(j, indice);
    }

    class go_coa_piste_legende final : public GameObjectScript
    {
    public:
        go_coa_piste_legende() : GameObjectScript(SCRIPT_PISTE_LEGENDE) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new PisteLegendeAI(go); }
    };

    class go_coa_indice_traque final : public GameObjectScript
    {
    public:
        go_coa_indice_traque() : GameObjectScript(SCRIPT_INDICE_TRAQUE) { }
        GameObjectAI* GetAI(GameObject* go) const override { return new IndiceTraqueAI(go); }
    };

    class npc_coa_bete_legende final : public CreatureScript
    {
    public:
        npc_coa_bete_legende() : CreatureScript(SCRIPT_BETE_LEGENDE) { }
        CreatureAI* GetAI(Creature* c) const override { return new BeteLegendeAI(c); }
    };

    // ---- Le sort lance -----------------------------------------------------------------------------------------------

    // Vide si le lancement est permis ; sinon le message montre au joueur.
    std::string RefusLancement(Player* j, std::string& code)
    {
        auto refus = [&](char const* c, std::string m) { code = c; return m; };
        if (!Pret || !TableRegistre)
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
        if (!ZoneDeTraque(j->GetZoneId()))
            return refus("zone", "Aucune piste légendaire ici : les bêtes de légende hantent les terres sauvages de niveau 48 à 60.");
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
        if (EvenementProche(j->GetMapId(), j->GetInstanceId(), j->GetPositionX(), j->GetPositionY()))
            return refus("voisin", "Un autre mini-événement gronde déjà tout près.");
        return {};
    }

    class SourcierDepecageSorts final : public AllSpellScript
    {
    public:
        SourcierDepecageSorts() : AllSpellScript("CoaSourcierDepecageSorts", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST, ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCheckCast(Spell* sort, bool /*strict*/, SpellCastResult& res) override
        {
            if (res != SPELL_CAST_OK || !sort || sort->GetSpellInfo()->Id != SORT_SUIVRE_PISTE)
                return;
            Player* j = sort->GetCaster() ? sort->GetCaster()->ToPlayer() : nullptr;
            if (!j)
            {
                res = SPELL_FAILED_DONT_REPORT;
                return;
            }
            std::string code;
            std::string const message = RefusLancement(j, code);
            if (message.empty())
                return;
            res = SPELL_FAILED_DONT_REPORT;
            ChatHandler(j->GetSession()).SendNotification(message);
            LOG_INFO("module", "coa sourcier: refus evenement={} joueur={} raison={}", CODE, j->GetGUID().GetCounter(), code);
        }

        void OnSpellCast(Spell* /*sort*/, Unit* lanceur, SpellInfo const* info, bool /*skipCheck*/) override
        {
            if (!info || info->Id != SORT_SUIVRE_PISTE)
                return;
            Player* j = lanceur ? lanceur->ToPlayer() : nullptr;
            if (!j)
                return;
            std::string code;
            if (!RefusLancement(j, code).empty())
                return;   // deja refuse par CheckCast ; un lancement force qui ne passe pas la garde ne fait rien
            Registre const avant = LireRegistre(j->GetGUID().GetCounter());
            GameObject* piste = j->GetMap()->SummonGameObject(GO_PISTE_LEGENDE, j->GetPositionX(), j->GetPositionY(), j->GetPositionZ(),
                j->GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, DUREE_TRAQUE_S + 120);
            PisteLegendeAI* ai = piste ? dynamic_cast<PisteLegendeAI*>(piste->AI()) : nullptr;
            if (!ai || !ai->Demarrer(j, avant))
            {
                // Rien n'est consomme, le lancement reste accorde.
                LOG_ERROR("module", "coa sourcier: piste {} non posee, sans le script {} ou premier indice impossible : lancement rendu evenement={}",
                    GO_PISTE_LEGENDE, SCRIPT_PISTE_LEGENDE, CODE);
                if (piste)
                {
                    piste->SetRespawnTime(0);
                    piste->Delete();
                }
                ChatHandler(j->GetSession()).SendNotification("Aucune piste ne part d'ici : essayez un peu plus loin.");
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

    // ---- Crochets d'unite : credits sur la bete, morts ---------------------------------------------------------------

    class SourcierDepecageUnites final : public UnitScript
    {
    public:
        SourcierDepecageUnites() : UnitScript("CoaSourcierDepecageUnites", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH }) { }

        void OnDamage(Unit* attaquant, Unit* victime, uint32& degats) override
        {
            if (!degats || !victime || !victime->IsCreature() || !EstBete(victime->GetEntry()) || !NombreActifs.load(std::memory_order_relaxed))
                return;
            Player* joueur = attaquant ? attaquant->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (PisteLegendeAI* s = joueur ? SessionDe(victime) : nullptr)
                if (s->EstBeteDeSession(victime->GetGUID()))
                    s->Crediter(joueur, std::min(degats, uint32(victime->GetHealth())));
        }

        void OnHeal(Unit* soigneur, Unit* soigne, uint32& gain) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !gain || !soigneur || !soigne || !soigne->IsPlayer())
                return;
            Player* joueur = soigneur->GetCharmerOrOwnerPlayerOrPlayerItself();
            if (PisteLegendeAI* s = joueur ? SessionDe(soigne) : nullptr)
                s->Crediter(joueur, gain);
        }

        void OnUnitDeath(Unit* mort, Unit* tueur) override
        {
            if (!NombreActifs.load(std::memory_order_relaxed) || !mort)
                return;
            PisteLegendeAI* s = SessionDe(mort);
            if (!s)
                return;
            if (mort->IsCreature() && s->EstBeteDeSession(mort->GetGUID()))
                s->BeteMorte();
            else if (mort->IsPlayer() && s->EstSpecialiste(mort->GetGUID()))
            {
                Player const* joueur = (tueur && tueur != mort) ? tueur->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
                s->SpecialisteMort(joueur && joueur != mort ? joueur : nullptr);
            }
        }
    };

    // ---- La Lame de trophee : crochet de butin de depecage ----------------------------------------------------------------

    struct CompteTrophees
    {
        int64 jour = 0;
        uint32 n = 0;
    };

    CompteTrophees LireTrophees(uint32 guid)
    {
        CompteTrophees c;
        if (!TableTrophees)
            return c;
        if (QueryResult q = CharacterDatabase.Query("SELECT jour, n FROM coa_artisanat_trophee WHERE guid = {}", guid))
        {
            c.jour = q->Fetch()[0].Get<int32>();
            c.n = q->Fetch()[1].Get<uint8>();
        }
        return c;
    }

    void EcrireTrophees(uint32 guid, CompteTrophees const& c)
    {
        if (TableTrophees)
            CharacterDatabase.DirectExecute("REPLACE INTO coa_artisanat_trophee (guid, jour, n) VALUES ({}, {}, {})", guid, int32(c.jour),
                std::min<uint32>(c.n, 255));
    }

    uint32 TropheeDe(CreatureTemplate const* t)
    {
        switch (t->type)
        {
            case CREATURE_TYPE_BEAST: return TROPHEE_CROC;
            case CREATURE_TYPE_DRAGONKIN: return TROPHEE_ECAILLE;
            default: return TROPHEE_CORNE;
        }
    }

    // Vide si la creature donne un trophee a `j` ; sinon la raison (journal).
    char const* RefusTrophee(Player const* j, Creature const* c)
    {
        CreatureTemplate const* t = c->GetCreatureTemplate();
        uint32 const rang = t->rank;
        bool const rare = rang == CREATURE_ELITE_RARE || rang == CREATURE_ELITE_RAREELITE;
        bool const elite = rang == CREATURE_ELITE_ELITE || rang == CREATURE_ELITE_RAREELITE || rang == CREATURE_ELITE_WORLDBOSS;
        if (!rare && !elite)
            return "ordinaire";
        if (c->GetMap()->Instanceable() && rang != CREATURE_ELITE_RARE)
            return "elite_instance";
        if (c->GetLevel() <= Acore::XP::GetGrayLevel(j->GetLevel()))
            return "grise";
        return "";
    }

    class SourcierLameTrophee final : public MiscScript
    {
    public:
        SourcierLameTrophee() : MiscScript("CoaSourcierLameTrophee", { MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS }) { }

        // Apres le tirage du modele (Loot::FillLoot, LootMgr.cpp:563), avant les droits de groupe : comme les peaux a theme.
        void OnAfterLootTemplateProcess(Loot* butin, LootTemplate const* /*modele*/, LootStore const& magasin, Player* detenteur,
            bool /*personnel*/, bool /*sansErreur*/, uint16 mode) override
        {
            if (!LameActive || &magasin != &LootTemplates_Skinning || !butin || !detenteur || !mode || !detenteur->IsInWorld()
                || !EstHumain(detenteur) || !butin->sourceWorldObjectGUID.IsCreature() || !detenteur->HasSpell(SORT_MARQUEUR_LAME_TROPHEE))
                return;
            Creature* c = detenteur->GetMap()->GetCreature(butin->sourceWorldObjectGUID);
            if (!c || c->GetCreatureTemplate()->GetRequiredLootSkill() != SKILL_SKINNING)
                return;   // le meme magasin sert au minage et a l'herboristerie de certaines creatures
            uint32 const guid = detenteur->GetGUID().GetCounter();
            char const* refus = RefusTrophee(detenteur, c);
            CompteTrophees compte;
            if (!*refus)   // le registre n'est lu que pour une creature qui donnerait un trophee
            {
                int64 const jour = JourDeJeu(Maintenant());
                compte = LireTrophees(guid);
                if (compte.jour != jour)
                {
                    compte.jour = jour;
                    compte.n = 0;
                }
                if (compte.n >= TROPHEES_PAR_JOUR)
                    refus = "plafond_du_jour";
            }
            uint32 const objet = TropheeDe(c->GetCreatureTemplate());
            if (*refus)
            {
                LOG_DEBUG("module", "coa sourcier: trophee refuse joueur={} creature={} rang={} raison={}", guid, c->GetEntry(),
                    uint32(c->GetCreatureTemplate()->rank), refus);
                return;
            }
            std::size_t const avant = butin->items.size();
            butin->AddItem(LootStoreItem(objet, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
            if (butin->items.size() <= avant)
            {
                LOG_INFO("module", "coa sourcier: trophee non pose joueur={} creature={} objet={} raison=butin_plein", guid, c->GetEntry(), objet);
                return;
            }
            ++compte.n;
            EcrireTrophees(guid, compte);
            LOG_INFO("module", "coa sourcier: trophee joueur={} creature={} niveau={} rang={} objet={} du_jour={}/{}", guid, c->GetEntry(),
                uint32(c->GetLevel()), uint32(c->GetCreatureTemplate()->rank), objet, compte.n, TROPHEES_PAR_JOUR);
        }
    };

    // ---- Le talent apprend le sort ; menage a la suppression d'un personnage -----------------------------------------------

    void Synchroniser(Player* j)
    {
        bool const talent = j->HasSpell(SORT_MARQUEUR_TRAQUE);
        if (talent && !j->HasSpell(SORT_SUIVRE_PISTE))
            j->learnSpell(SORT_SUIVRE_PISTE);
        else if (!talent && j->HasSpell(SORT_SUIVRE_PISTE))
            j->removeSpell(SORT_SUIVRE_PISTE, SPEC_MASK_ALL, false);
    }

    class SourcierDepecageJoueurs final : public PlayerScript
    {
    public:
        SourcierDepecageJoueurs() : PlayerScript("CoaSourcierDepecageJoueurs", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEARN_SPELL,
            PLAYERHOOK_ON_FORGOT_SPELL, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        void OnPlayerLogin(Player* j) override
        {
            if (!EstHumain(j))
                return;
            Synchroniser(j);
            if (j->HasSpell(SORT_SUIVRE_PISTE))
                AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
        }

        void OnPlayerLearnSpell(Player* j, uint32 sort) override
        {
            if (sort != SORT_MARQUEUR_TRAQUE || !EstHumain(j))
                return;
            Synchroniser(j);
            if (j->HasSpell(SORT_SUIVRE_PISTE))
                AfficherRecharge(j, LireRegistre(j->GetGUID().GetCounter()));
        }

        void OnPlayerForgotSpell(Player* j, uint32 sort) override
        {
            if (sort == SORT_MARQUEUR_TRAQUE)
                Synchroniser(j);
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            if (TableTrophees)
                trans->Append("DELETE FROM coa_artisanat_trophee WHERE guid = {}", guid);
        }
    };

    // ---- Le demarrage : donnees presentes ? ---------------------------------------------------------------------------

    class SourcierDepecageMonde final : public WorldScript
    {
    public:
        SourcierDepecageMonde() : WorldScript("CoaSourcierDepecageMonde", { WORLDHOOK_ON_STARTUP }) { }

        static bool GoAvecScript(uint32 entree, char const* script, uint32 type)
        {
            GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(entree);
            return go && go->type == type && go->ScriptId == sObjectMgr->GetScriptId(script);
        }

        void OnStartup() override
        {
            bool const sort = sSpellMgr->GetSpellInfo(SORT_SUIVRE_PISTE);
            bool const piste = GoAvecScript(GO_PISTE_LEGENDE, SCRIPT_PISTE_LEGENDE, GAMEOBJECT_TYPE_GENERIC);
            bool const indice = GoAvecScript(GO_INDICE_TRAQUE, SCRIPT_INDICE_TRAQUE, GAMEOBJECT_TYPE_CHEST);
            uint32 betes = 0;
            for (uint32 e : PNJ_BETES_LEGENDE)
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(e))
                    betes += t->ScriptID == sObjectMgr->GetScriptId(SCRIPT_BETE_LEGENDE) ? 1 : 0;
            bool const peau = sObjectMgr->GetItemTemplate(OBJET_PEAU_LEGENDE);
            Pret = sort && piste && indice && betes == PNJ_BETES_LEGENDE.size() && peau;
            LOG_INFO("module", "coa sourcier: depecage evenement={} sort {} ({}) marqueur {} ; piste {} ({}) ; indice {} ({}) ; betes {}/{} ; "
                "materiau {} ({}) ; zones {} ; {}", CODE, SORT_SUIVRE_PISTE, sort ? "present" : "ABSENT", SORT_MARQUEUR_TRAQUE, GO_PISTE_LEGENDE,
                piste ? "ok" : "ABSENTE ou sans script", GO_INDICE_TRAQUE, indice ? "ok" : "ABSENT ou sans script", betes,
                PNJ_BETES_LEGENDE.size(), OBJET_PEAU_LEGENDE, peau ? "present" : "ABSENT", ZONES_TRAQUE.size(), Pret ? "PRET" : "INERTE");

            TableTrophees = bool(CharacterDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                "AND TABLE_NAME = 'coa_artisanat_trophee'"));
            uint32 trophees = 0;
            for (uint32 t : TROPHEES)
                trophees += sObjectMgr->GetItemTemplate(t) ? 1 : 0;
            LameActive = TableTrophees && trophees == TROPHEES.size();
            LOG_INFO("module", "coa sourcier: lame de trophee marqueur {} ; table coa_artisanat_trophee {} ; trophees {}/{} ; {} par jour ; {}",
                SORT_MARQUEUR_LAME_TROPHEE, TableTrophees ? "presente" : "ABSENTE (migration 2026_10_21_01 non appliquee)", trophees,
                TROPHEES.size(), TROPHEES_PAR_JOUR, LameActive ? "ACTIVE" : "INERTE");
        }
    };

    // ---- Banc (PTR seulement) : compteur de trophees d'un personnage ------------------------------------------------------

    class SourcierDepecageCommandes final : public CommandScript
    {
    public:
        SourcierDepecageCommandes() : CommandScript("CoaSourcierDepecageCommandes") { }

        ChatCommandTable GetCommands() const override
        {
            // Fusionne avec « sourcier banc » de AscensionArtisanatSourcier.cpp (ChatCommand.cpp, LoadCommandsIntoMap).
            static ChatCommandTable racine =
            {
                { "sourcier banc trophees", HandleTrophees, SEC_ADMINISTRATOR, Console::Yes },
            };
            return racine;
        }

        // `.sourcier banc trophees <nom> [remettre]` : lit (ou remet a zero) le compteur du jour de la Lame de trophee.
        static bool HandleTrophees(ChatHandler* handler, std::string nom, Optional<std::string> remettre)
        {
            if (realm.Id.Realm != ROYAUME_PTR)
                return handler->SendSysMessage("Banc du Sourcier : royaume PTR seulement."), true;
            ObjectGuid const g = sCharacterCache->GetCharacterGuidByName(nom);
            if (!g)
                return handler->PSendSysMessage("sourcier: {} inconnu.", nom), true;
            if (remettre && *remettre == "remettre")
            {
                EcrireTrophees(g.GetCounter(), CompteTrophees{});
                return handler->PSendSysMessage("sourcier: compteur de trophees de {} remis a zero.", nom), true;
            }
            CompteTrophees const c = LireTrophees(g.GetCounter());
            int64 const jour = JourDeJeu(Maintenant());
            handler->PSendSysMessage("sourcier: {} (guid {}) : table {} ; lame {} ; jour {} (aujourd'hui {}) ; trophees {}/{}", nom, g.GetCounter(),
                TableTrophees ? "presente" : "ABSENTE", LameActive ? "ACTIVE" : "INERTE", int32(c.jour), int32(jour),
                c.jour == jour ? c.n : 0, TROPHEES_PAR_JOUR);
            return true;
        }
    };
}

void AddAscensionArtisanatSourcierDepecageScripts()
{
    new go_coa_piste_legende();
    new go_coa_indice_traque();
    new npc_coa_bete_legende();
    new SourcierDepecageSorts();
    new SourcierDepecageUnites();
    new SourcierLameTrophee();
    new SourcierDepecageJoueurs();
    new SourcierDepecageMonde();
    new SourcierDepecageCommandes();
}
