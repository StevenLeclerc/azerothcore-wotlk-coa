/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T2 du Sourcier (21/10/2026, lot MINAGE) : le SOCLE commun des mini-evenements.
// Conception : coa-project docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2.0.
//
// Ce que le Filon-mere (AscensionArtisanatSourcier.cpp, en service depuis T1) tenait dans son espace de noms anonyme et
// que les autres mini-evenements reutilisent tel quel : temps de jeu (jour et semaine, remise a 06:00 Paris), registre
// par personnage (UN lancement de Sourcier par jour, tous evenements confondus : une seule ligne, un seul metier principal
// de recolte, D-PO-43), exclusivite a RAYON_EXCLUSIF autour d'un point, marque d'unite vers son objet pivot, remise
// d'objets (sacs, puis courrier), Coeurs de foudre en file pour un specialiste sur une autre carte, decision du garanti.
// Les DEFINITIONS restent dans AscensionArtisanatSourcier.cpp (deplacees de l'espace anonyme vers CoaSourcier, sans
// changer une ligne de leur corps hors AfficherRecharge, qui pose la recharge sur TOUS les sorts lances de Sourcier
// connus du joueur, et Donner, qui recoit l'expediteur du courrier).
//
// FIL : tout ce qui touche un Player ou un objet se fait sur le fil de sa carte (P-137). Les tables sous verrou
// (exclusivite, remises en file) sont les seules lues de plusieurs fils.

#ifndef COA_ASCENSION_ARTISANAT_SOURCIER_SOCLE_H
#define COA_ASCENSION_ARTISANAT_SOURCIER_SOCLE_H

#include "AscensionArtisanatIds.h"
#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <ctime>
#include <string>

class GameObject;
class Player;
class Unit;
class WorldObject;

namespace CoaSourcier
{
    // ---- Temps de jeu (Paris) ----
    int64 DecalageParisS(time_t utc);
    int64 JourDeJeu(time_t utc);
    int64 SemaineDeJeu(int64 jour);
    time_t ProchaineRemise(time_t utc);
    std::string HeureParis(time_t utc);
    time_t Maintenant();

    // ---- Registre par personnage (acore_characters.coa_artisanat_sourcier) ----
    extern bool TableRegistre;
    extern bool ColonneRelanceJcj;
    struct Registre
    {
        int64 jourLance = 0;
        uint32 relanceTs = 0;
        int64 essai2Jour = 0;
        int64 semaineGaranti = -1;
        int64 recompJour = 0;
        uint32 recompN = 0;
        int64 relanceJcjJour = 0;
    };
    Registre LireRegistre(uint32 guid);
    void EcrireRegistre(uint32 guid, Registre const& r);
    bool LancementAccorde(Registre const& r, time_t maintenant);
    // Recharge affichee sur le bouton de CHAQUE sort lance de Sourcier que le joueur connait (le registre est commun).
    void AfficherRecharge(Player* joueur, Registre const& r);

    // ---- Exclusivite (toutes cartes, sous verrou) ----
    extern std::atomic<uint32> NombreActifs;
    // Integration T2 (21/10) : `rayon` (RAYON_EXCLUSIF par defaut ; RAYON_EXCLUSIF_DIRECTEUR pour un lancement de voie
    // Directeur, plus large que deux rayons d'adoption : deux pivots ne s'adoptent jamais leurs creatures).
    bool EvenementProche(uint32 carte, uint32 instance, float x, float y, float rayon = CoaArtisanatIds::RAYON_EXCLUSIF);
    // Vrai si `pivot` est, parmi les evenements actifs DE VOIE DIRECTEUR de la carte, le plus proche de (x, y) (ou le
    // seul) : une creature du Directeur n'est adoptee que par le pivot le plus proche d'elle.
    bool PivotLePlusProche(ObjectGuid pivot, uint32 carte, uint32 instance, float x, float y);
    // T2 HERBORISTERIE (21/10) : WorldObject, le pivot du Jardin d'orage est une creature (la Graine-tempete).
    // Integration T2 : `voieDirecteur` (Faille tellurique, Appel du Linceul) : seuls ces pivots se disputent les creatures
    // du Directeur (PivotLePlusProche ne compare qu'eux).
    void InscrireActif(WorldObject* pivot, bool voieDirecteur = false);
    void RetirerActif(ObjectGuid pivot);

    // ---- Marque d'unite -> objet pivot (CustomData, fil de la carte) ----
    extern std::string const CLE_FILON;
    struct MarqueFilon final : public DataMap::Base
    {
        ObjectGuid filon;    // l'objet pivot de l'evenement (Filon-mere, Faille tellurique...)
    };
    void Marquer(Unit* u, ObjectGuid pivot);
    ObjectGuid PivotDe(Unit const* u);

    bool EstHumain(Player const* p);

    // Donne `nombre` x `entree` a `joueur` (sur sa carte), fabricant `createur` si non vide ; ce qui ne tient pas part par
    // courrier, expediteur `expediteur`. Rend vrai si tout est parti.
    bool Donner(Player* joueur, uint32 entree, uint32 nombre, ObjectGuid createur, char const* expediteur = "Le Filon-mère");

    // ---- Coeur de foudre d'un specialiste hors de la carte ----
    bool PosterCoeurs(ObjectGuid specialiste, uint32 n);
    void MettreEnFile(ObjectGuid specialiste);

    // ---- Integration T2 (21/10) : tout materiau legendaire d'un specialiste hors de la carte du pivot ----
    // Courrier en BASE seulement (MailReceiver par GUID) : vu a la prochaine connexion. Appelable de tout fil.
    bool PosterMateriauLegendaire(ObjectGuid specialiste, uint32 entree, uint32 n, char const* expediteur);
    // File (sous verrou) videe sur le fil du joueur (ViderRemises : sacs, ou courrier a la deconnexion).
    void MettreEnFileObjet(ObjectGuid specialiste, uint32 entree, uint32 n, char const* expediteur);
    // La remise du materiau legendaire, quelle que soit la place du specialiste : `surCarte` (non nul : sur la carte du
    // pivot) -> sacs ; connecte ailleurs -> file ; hors ligne -> courrier. -> la voie (« sacs », « file », « courrier »).
    char const* RemettreMateriau(Player* surCarte, ObjectGuid specialiste, uint32 entree, char const* expediteur, bool& remis);

    // ---- Integration T2 (21/10) : sessions persistees (acore_characters.coa_artisanat_sourcier_session) ----
    // Une ligne par session ouverte (etat « en_cours ») ; fermee avec sa raison (succes, rendu, echec...). Au demarrage du
    // monde, toute ligne encore « en_cours » est une session tuee par un redemarrage : son lancement est RENDU (champs de
    // lancement du registre remis a leur valeur d'avant) et elle passe « interrompu ». Le Directeur (exploitation/directeur/
    // sourcier_fin.py) lit cette table pour arreter ses vagues quand la session du module est finie. Table absente :
    // rien n'est ecrit (comportement d'avant).
    extern bool TableSessions;
    void OuvrirSession(ObjectGuid pivot, ObjectGuid specialiste, char const* evenement, Registre const& avant);
    void FermerSession(ObjectGuid pivot, char const* etat);
    void RendreSessionsInterrompues();

    // ---- Le materiau legendaire : garanti hebdomadaire a COMPTES_HUMAINS_GARANTI comptes, sinon la chance ----
    struct DecisionCoeur
    {
        bool garanti = false;
        bool obtenu = false;
    };
    DecisionCoeur DeciderCoeur(Registre& r, int64 semaine, uint32 comptesHumains);
}

#endif
