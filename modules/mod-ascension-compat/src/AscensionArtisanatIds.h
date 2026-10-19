/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license:
 * https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// CoA, artisanat, lot T1 « Le Sourcier » (docs/SPEC-artisanat-T-sourcier-dev.md, T1-1, T1-4, T1-5).
//
// TOUS les identifiants que le C++ du Sourcier attend d'autres agents sont ICI, et nulle part ailleurs. Recus le 15/10/2026
// par le PO : dev-dbc eb2a980 (artisanat/ids-figes-v2.json), dev-monde d379182 / e3ddab8.
// Un identifiant a 0 n'est pas encore recu : la fonction qui en depend reste INERTE (journal au demarrage,
// « coa sourcier: ... inerte »), sans erreur ni effet. Ne jamais remplacer un 0 par une valeur supposee :
// chaque valeur se recopie depuis la livraison de l'agent nomme, relue en base ou dans le DBC.
//
// Marques : [L] lu, [M] mesure, [H] dosage propose (a eprouver), « a recevoir » : 0 tant que non livre.

#ifndef COA_ASCENSION_ARTISANAT_IDS_H
#define COA_ASCENSION_ARTISANAT_IDS_H

#include "Define.h"
#include <array>

namespace CoaArtisanatIds
{
    // ---- A recevoir de dev-dbc (SpellItemEnchantment 7000-7199, sorts 9113000-9113439) ----------------------------

    // Enchantement neutre « Extrait par un Sourcier » (emplacement 9, PROP_ENCHANTMENT_SLOT_2). Ses trois types
    // d'effet DOIVENT valoir 0 (ITEM_ENCHANTMENT_TYPE_NONE) et sa duree 0 : les charges de l'emplacement portent le
    // GUID bas du recolteur, et le coeur ne decompte des charges que pour un type COMBAT_SPELL (verdict T1-1).
    constexpr uint32 ENCHANT_EXTRAIT = 7199;                    // dev-dbc eb2a980 « enchant:sourcier:extrait »

    // Sort LANCE « Reveiller le Filon-mere » (hors combat ; la cible est une Veine foudroyee a portee, lue par le
    // module : cible gameobject explicite si le sort en a une, sinon la veine la plus proche a PORTEE_VEINE).
    constexpr uint32 SORT_REVEILLER_FILON = 9113490;            // dev-dbc eb2a980 « sort:sourcier:reveiller-filon-mere »
    // Sort MARQUEUR du talent « Reveiller le Filon-mere » (connu = talent appris). Le module apprend le sort lance a
    // qui connait le marqueur, et le retire a qui ne le connait plus.
    constexpr uint32 SORT_MARQUEUR_FILON = 9113359;             // dev-dbc eb2a980 « talent:minage:sourcier:filon-mere:1 »

    // Coeur de forge : les critiques sont comptes par le module (coups blancs et sorts de melee/distance par le crochet
    // d'objet de combat, autres sorts par le resultat de sort) ; cette aura (10 piles) ne fait que MONTRER les charges,
    // posee et empilee par le module. L'aura de 10 s est `sort_effet` de la ligne coa_artisanat_signature (effet
    // 'coeur_de_forge', 9113491) : son bonus du DBC est corrige par le module vers la valeur JcJ (valeur_arene) quand la
    // cible est controlee par un joueur, et annule en arene 1c1 contre un gladiateur.
    constexpr uint32 SORT_AURA_CHARGES_COEUR_DE_FORGE = 9113492; // dev-dbc eb2a980 « effet:forge:coeur-de-forge:charges »

    // ---- A recevoir de dev-monde (item_template, gameobject_template, creature_template) -----------------------

    constexpr uint32 OBJET_COEUR_DE_FOUDRE = 9101240;           // dev-monde d379182 (qualite 5, non lie, pile 1)
    // Les Oeuvres legendaires : ni signature a part, et le recolteur du materiau legendaire consomme en emplacement 9.
    // Cuirasse du Maitre armurier : forme eveillee 9104700 (fabriquee, recette 9111180), puis rangs 1-5 9104701-9104705
    // obtenus par renforcement (dev-monde 06ec7fd, D-PO-46 ; renforts 9112300-9112304 en spellid_2, lueur 9113493 en spellid_1).
    constexpr std::array<uint32, 6> OEUVRES = { 9104700, 9104701, 9104702, 9104703, 9104704, 9104705 };
    // Filon-mere : gameobject_template de type 3 (CHEST) SANS verrou (data0 = 0), data1 = son butin personnel
    // (gameobject_loot_template), ScriptName = SCRIPT_FILON_MERE, entree dans 9101000-9101099 (refusee aux bots,
    // mod-playerbots LootObjectStack / LootAction).
    constexpr uint32 GO_FILON_MERE = 9101090;                   // dev-monde d379182 (coffre sans verrou, go_coa_filon_mere)
    // Niveau et experience du Colosse : donnees de dev-monde (creature_template). Plafond 60 (D-A5 (a), PLAN-plafond-60.md
    // § 3.5) : 60/60 elite, exp 0 (il etait 80/80, exp 2 ; le LevelScaling ne fait jamais descendre une creature).
    constexpr uint32 PNJ_COLOSSE_ORAGE = 9105000;               // dev-monde d379182 (elite)

    // ---- Fixes par ce module ou mesures ---------------------------------------------------------------------------

    constexpr char const* SCRIPT_FILON_MERE = "go_coa_filon_mere";   // gameobject_template.ScriptName du Filon-mere
    constexpr uint32 GO_VEINE_FOUDROYEE = 9101000;              // [M] gameobject_template 9101000 « Veine foudroyee »
    // Ressources d'evenement de la Faille d'orage, recompense de participant [M item_template, acore_world_ptr].
    constexpr uint32 RESSOURCE_VERTE = 9100101;                 // Fragment de foudre, qualite 2
    constexpr uint32 RESSOURCE_RARE = 9100106;                  // Coeur d'orage, qualite 3
    constexpr uint32 RESSOURCE_EPIQUE = 9100107;                // Oeil de la tempete, qualite 4
    // Ressources d'evenement de la Nuit des morts (famille Glas) [M item_template, acore_world et acore_world_ptr, 19/10].
    constexpr uint32 RESSOURCE_GLAS_VERTE = 9100111;            // Os maudit, qualite 2
    constexpr uint32 RESSOURCE_GLAS_RARE = 9100116;             // Essence sepulcrale, qualite 3

    // ---- Gisements d'evenement et recolte (19/10, docs/CONCEPTION-artisanat-gisements-evenement.md) -----------------
    // Chance globale : toute recolte d'un noeud classique (minage, herboristerie) ou tout depecage peut donner une
    // ressource d'evenement verte ou rare (jamais l'epique). Minage -> Orage, depecage -> Glas, herboristerie -> l'une ou
    // l'autre au hasard. Taux : CoaArtisanat.Recolte.Evenement.* (0 par defaut : rien). Talent de l'arbre de recolte
    // (code du gabarit « sourcier-b », nom « Pressentiment », 3 rangs) : la verte passe a Talent.Rang1/2/3 % (25 % au
    // rang 3), la rare dans la meme proportion que la base.
    constexpr char const* TALENT_PRESSENTIMENT = "sourcier-b";
    // Depecage des betes et dragons d'evenement et des rares du Directeur : creature_template.skinloot = l'une de ces
    // deux entrees de skinning_loot_template (pose SQL du kit). La ligne unique de chaque entree porte le cuir gabarit ;
    // ce module le remplace par le cuir ordinaire du niveau de la creature (son niveau est impose a l'invocation) et
    // ajoute, pour un humain, la peau a theme (CoaArtisanat.Depecage.Theme.Chance.*).
    constexpr uint32 DEPECAGE_ORAGE = 9101300;                  // skinning_loot_template : theme Orage
    constexpr uint32 DEPECAGE_GLAS = 9101301;                   // skinning_loot_template : theme Glas
    constexpr uint32 CUIR_GABARIT = 2318;                       // Light Leather, la ligne de ces deux entrees
    constexpr uint32 PEAU_TEMPETE = 9101204;                    // Peau de tempete (Orage) [M item_template]
    constexpr uint32 FOURRURE_SPECTRALE = 9101205;              // Fourrure spectrale (Glas) [M item_template]
    // Cuir ordinaire par niveau de la creature (dernier niveau de la tranche, cuir) : butins de depecage de 3.3.5.
    struct CuirParNiveau { uint8 jusqua; uint32 cuir; };
    constexpr std::array<CuirParNiveau, 7> CUIRS_PAR_NIVEAU =
    {{
        { 15, 2318 },    // Light Leather
        { 25, 2319 },    // Medium Leather
        { 35, 4234 },    // Heavy Leather
        { 45, 4304 },    // Thick Leather
        { 62, 8170 },    // Rugged Leather
        { 70, 21887 },   // Knothide Leather
        { 255, 33568 },  // Borean Leather
    }};
    // Type d'arene du 1 contre 1 contre un gladiateur : CoaArene1v1::TYPE_ARENE (mod-playerbots,
    // src/Ai/Coa/CoaArene1v1.h [L]), recopie ici pour ne pas lier ce module a mod-playerbots.
    constexpr uint8 TYPE_ARENE_1C1 = 1;

    // Nom de l'effet (colonne coa_artisanat_signature.effet) de la facette Coeur de forge, fixe avec PartB.
    constexpr char const* EFFET_COEUR_DE_FORGE = "coeur_de_forge";
    // Prefixe du code de talent (coa_artisanat_talent_def.talent) d'une facette d'Oeuvre : « facette:<forme> », ou
    // <forme> est la forme de la ligne coa_artisanat_signature de la facette (ex. « facette:coeur_de_forge »).
    constexpr char const* PREFIXE_TALENT_FACETTE = "facette:";

    // Creatures du Directeur (mod-playerbots, EV_ENTREE_MIN/MAX de CoaDirecteur) : au butin des materiaux de monde, celles
    // de rang 2 (rare elite) et 4 (rare) sont classees elite ordinaire, dehors comme en donjon ; rangs 0, 1, 3 inchanges
    // (D-PO-48, precise par le PO le 16/10). Comprend aussi
    // 9105000-9105099 (Colosse d'orage...). Meme plage que DIRECTEUR_EVENEMENT_ENTREE_MIN/MAX (AscensionCompat.cpp) et
    // EV_ENTREE_MIN/MAX (mod-playerbots, CoaDirecteur.cpp) : a changer ensemble.
    constexpr uint32 PNJ_DIRECTEUR_MIN = 9100000;
    constexpr uint32 PNJ_DIRECTEUR_MAX = 9109999;

    // ---- Valeur JcJ des bonus « Moisson » de la Nuit (D-PO-49 ; sorts de dev-monde 176cbbf ; valeurs du PO, 16/10) ----
    //
    // Auras declenchees a la mort d'un ennemi (aura 42 des bonus 6 et 8 pieces) : la valeur JcJ s'applique a toute la duree
    // de l'aura si, au moment ou elle est posee, le porteur est en combat contre une unite controlee par un joueur, ou en
    // champ de bataille ou en arene (meme regle que les soins de Coeur de forge). Le declenchement a lieu dans Unit::Kill
    // AVANT l'arret du combat de la victime (proc a Unit.cpp:14842, setDeathState a :14897 qui appelle CombatStop) : un
    // joueur tue compte donc encore. En arene 1c1 contre un gladiateur : aura laissee, montants a 0. Degats d'Ombre
    // declenches : valeur JcJ si la cible est controlee par un joueur, 0 en arene 1c1. Non ramenes : le soin de 5 % (9112073)
    // et le mana de 4 % (9112081), effets instantanes hors aura.
    //
    // Plafond 60 (PLAN-plafond-60.md § 3.5, D-PO-53) : valeurs fixes redosees au 60 en meme temps que les donnees de
    // dev-monde (generer.py e2b4618, MOISSON : au60() = x 0,2) : +120 caracteristique / puissance des sorts -> 24, +250
    // puissance d'attaque -> 50, degats d'Ombre 1 200 / 900 / 1 400 -> 240 / 180 / 280 ; valeurs JcJ dans la meme
    // proportion qu'au 80 (2/3, 0,64, 1/2 : toutes entieres) ; pourcentages (8 % de degats, 6 % de hate) inchanges. Si
    // les donnees posees different, le temoin du demarrage le dit (« moisson jcj : n/11 ... INERTE ») et la valeur JcJ
    // reste inactive : changer les deux ensemble.
    struct MoissonAura
    {
        uint32 sort;
        uint8 effet;        // index de l'effet d'aura a ramener
        uint32 auraType;    // type d'aura attendu (temoin au demarrage)
        int32 normal;       // valeur du DBC (BasePoints + 1)
        int32 jcj;
    };
    constexpr std::array<MoissonAura, 8> MOISSON_AURAS =
    {{
        { 9112072, 0, 79, 8, 4 },        // degats physiques en %
        { 9112073, 1, 29, 24, 16 },      // Force (80 : 120 / 80)
        { 9112074, 0, 99, 50, 32 },      // puissance d'attaque (80 : 250 / 160)
        { 9112074, 1, 124, 50, 32 },     // puissance d'attaque a distance (80 : 250 / 160)
        { 9112077, 0, 138, 6, 3 },       // hate de melee en %
        { 9112078, 0, 29, 24, 16 },      // Agilite (80 : 120 / 80)
        { 9112081, 1, 13, 24, 16 },      // puissance des sorts, degats (80 : 120 / 80)
        { 9112081, 2, 135, 24, 16 },     // puissance des sorts, soins (80 : 120 / 80)
    }};
    struct MoissonDegats
    {
        uint32 sort;
        int32 normal;
        int32 jcj;
    };
    constexpr std::array<MoissonDegats, 3> MOISSON_DEGATS =
    {{
        { 9112076, 240, 120 },           // 80 : 1 200 / 600
        { 9112079, 180, 90 },            // 80 : 900 / 450
        { 9112083, 280, 140 },           // 80 : 1 400 / 700
    }};

    // ---- Dosages [H] (spec T1-4, conception § 2) ------------------------------------------------------------------

    constexpr uint32 SEUIL_CHARGES_COEUR_DE_FORGE = 10;         // critiques pour declencher l'aura
    constexpr float PORTEE_VEINE = 10.0f;                       // m, veine visee sans cible explicite
    // Repli (PO, 15/10) : si le client refuse de cibler la Veine (un coffre), le sort 9113490 cible le joueur et le module
    // prend la Veine 9101000 la plus proche a PORTEE_VEINE. Vrai : la cible explicite d'abord, la recherche sinon.
    constexpr bool VEINE_PAR_RECHERCHE = true;
    constexpr float RAYON_EXCLUSIF = 100.0f;                    // m, un seul mini-evenement autour d'un point
    constexpr float RAYON_DOSAGE = 40.0f;                       // m, humains du groupe comptes pour les PV du Colosse
    constexpr float RAYON_PARTICIPATION = 100.0f;               // m, groupe compte present a la fin
    constexpr uint32 JOUEURS_MAX = 5;
    constexpr uint32 USAGES_PAR_JOUEUR = 3;
    constexpr uint32 PLACE_USAGE = 3;                           // cases libres exigees par usage (butin du Filon : 2 objets au plus)
    constexpr uint32 DUREE_MAX_S = 10 * 60;                     // echec si le Colosse vit encore
    constexpr uint32 GRACE_FIN_S = 120;                         // usages restants apres la mort du Colosse
    constexpr uint32 ABSENCE_MAX_S = 60;                        // specialiste tue par un monstre et non revenu
    constexpr uint32 RELANCE_ECHEC_S = 4 * 3600;                // echec ordinaire : relance au bout de 4 h, 1 fois par jour
    constexpr uint32 HEURE_REMISE_PARIS = 6;                    // remise a zero quotidienne et hebdomadaire, 06:00 (Paris)
    constexpr uint32 CHANCE_COEUR_POUR_CENT = 25;               // hors garanti hebdomadaire
    constexpr uint32 COMPTES_HUMAINS_GARANTI = 2;               // comptes humains distincts pour le garanti
    constexpr uint32 PART_PARTICIPATION_POUR_MILLE = 20;        // 2 % des PV du Colosse, inflige ou soigne
    // Plafond 60 (D-A5 (a)) : plafond moins 10, meme logique qu'au 80 (il etait 70).
    constexpr uint8 NIVEAU_RECOMPENSE = 50;
    constexpr uint32 RECOMPENSES_PAR_JOUR = 2;
    constexpr uint32 RESSOURCES_VERTES = 2;
    constexpr uint32 CHANCE_RARE_POUR_CENT = 50;
    constexpr uint32 CHANCE_EPIQUE_POUR_MILLE = 30;
    // PV du Colosse en % de son modele, selon le nombre d'humains du groupe presents a RAYON_DOSAGE (1 a 5 et plus).
    constexpr std::array<uint8, 5> PV_COLOSSE_POUR_CENT = { 40, 55, 70, 85, 100 };

    // ---- Astrolabe de la Forge, arbre complet (18/10/2026, chantier « 16 talents a venir ») -------------------------
    //
    // Effets des talents du gabarit de creation (docs/PROPOSITION-artisanat-arbres-detail.md § 2 ; talents.json).
    // Les talents sont reconnus par leur CODE (coa_artisanat_talent_def.talent), jamais par un id : les ids sont ceux
    // de outils/artisanat/talents.py --jeu t2, figes dans artisanat/ids-figes-v2.json.

    // « De maitre » (Coup de maitre) : enchantement NEUTRE (types 0, duree 0) en emplacement 10 (PROP_ENCHANTMENT_SLOT_3,
    // libre sur nos pieces : aucun suffixe aleatoire). Les caracteristiques de la piece portee gagnent BONUS_DE_MAITRE_POUR_CENT
    // par le crochet OnPlayerApplyItemModsBefore (symetrique a la pose et au retrait). 0 : Coup de maitre inerte.
    constexpr uint32 ENCHANT_DE_MAITRE = 7197;                  // outils/artisanat-astrolabe-dbc.py « enchant:artisanat:de-maitre »

    // Composants (item_template du PTR, lu le 18/10) : 9101220-9101230 composants de base et intermediaires (qualite 1-2),
    // 9101231-9101239 composants d'artisan, ceux des rangs 1-5 (Plaques, Lanieres, Trames d'orage, qualite 2-4, non lies).
    constexpr uint32 COMPOSANT_MIN = 9101220;
    constexpr uint32 COMPOSANT_ARTISAN_MIN = 9101231;
    constexpr uint32 COMPOSANT_MAX = 9101239;
    // Materiaux de monde (D-PO-24, generer.py ID_MONDE) : Poussiere d'echo, Fragment de resonance, Larme d'Azeroth.
    constexpr uint32 MONDE_MIN = 9101212;
    constexpr uint32 MONDE_MAX = 9101214;
    constexpr uint32 MONDE_RARE = 9101213;                      // Fragment de resonance (qualite 3)
    // Un materiau « classique » est un reactif qui n'est pas a nous : hors de 9100000-9109999 (ressources d'evenement,
    // materiaux, composants, pieces).
    constexpr uint32 NOS_OBJETS_MIN = 9100000;
    constexpr uint32 NOS_OBJETS_MAX = 9109999;

    // Re-signer : 2 materiaux de monde rares et 1 composant d'artisan (le premier du metier : rangs 1-2) [H, au PO].
    // La variante « 1 trophee de la Lame de trophee » attend T2-3 (talent b) : non codee.
    constexpr uint32 RESIGNER_RARES = 2;
    constexpr std::array<std::pair<uint32, uint32>, 3> RESIGNER_COMPOSANT = {{ { 164, 9101231 }, { 165, 9101234 }, { 197, 9101237 } }};

    // Dosages (talents.json, D-PO-34 § 2), en points de pourcentage.
    constexpr uint32 PCT_MAIN_ECONOME = 5;          // par rang : materiaux classiques, recettes de son metier
    constexpr uint32 PCT_ETABLI = 5;                // par rang : chance d'un composant de plus
    constexpr uint32 PCT_EPARGNE = 15;              // or des paliers des pieces de son metier
    constexpr uint32 PCT_SAVOIR = 5;                // par rang : materiaux de monde des paliers des pieces de sa voie
    constexpr uint32 PCT_SERIE = 10;                // chance qu'une piece fabriquee coute 1 composant de moins
    constexpr uint32 PCT_COUP_DE_MAITRE = 10;       // chance d'une piece « de maitre »
    constexpr uint32 BONUS_DE_MAITRE_POUR_CENT = 5; // caracteristiques d'une piece de maitre
    constexpr uint32 PCT_ECONOMIE = 5;              // par rang : or des paliers et materiaux classiques
    constexpr uint32 PCT_MAITRISE = 5;              // par rang : chance d'un composant d'artisan en double
    constexpr uint32 MAIN_CONSTANTE_POUR_CENT = 125;// effet des signatures posees par un artisan qui la connait
    // Bastion d'orage : allies proteges (les plus blesses, en pourcentage de PV) et portee.
    constexpr uint32 BASTION_ALLIES = 2;
    constexpr float BASTION_PORTEE = 10.0f;

    // ---- Lot T (18/10/2026) : arbre de recolte complet et autres metiers ---------------------------------------------
    //
    // Noeuds d'evenement : 9101000-9101089 (le Filon-mere 9101090 a son propre script). Materiau, rare, verte, competence
    // de creation, metier de recolte et specialite sont LUS en base au demarrage (butin a reference et conditions de
    // outils/artisanat/generer.py), jamais recopies ici. Seuls figurent les ids que la base ne relie pas entre eux.
    constexpr uint32 NOEUD_RECOLTE_MIN = 9101000;
    constexpr uint32 NOEUD_RECOLTE_MAX = 9101089;

    // Materiaux purs (outils/artisanat-lot-t.py, artisanat/ids-figes-v2.json « objet:pur:<metier>:<code> ») : qualite 3,
    // pile de 1, non lies ; chacun est le pur d'UN materiau de metier (celui du noeud qui le donne, Main pure).
    struct MateriauPur
    {
        uint32 pur;
        uint32 base;          // materiau de metier du noeud (9101200-9101211)
    };
    constexpr std::array<MateriauPur, 12> MATERIAUX_PURS =
    {{
        { 9101241, 9101200 }, { 9101242, 9101201 }, { 9101243, 9101210 }, { 9101244, 9101211 },   // Minage
        { 9101245, 9101206 }, { 9101246, 9101207 }, { 9101247, 9101208 }, { 9101248, 9101209 },   // Herboristerie
        { 9101249, 9101204 }, { 9101250, 9101205 }, { 9101251, 9101202 }, { 9101252, 9101203 },   // Depecage
    }};
    // Correspondance metier de creation -> materiaux purs utilisables (detail du 13/10, § 4) : Forge et Ingenierie les
    // minerais, Joaillerie les geodes et cristaux, Cuir les peaux, Couture la soie et le linceul, Alchimie et Calligraphie
    // les fleurs, Enchantement les echos et les ames.
    struct PursDuMetier
    {
        uint32 metier;
        std::array<uint32, 2> purs;
    };
    constexpr std::array<PursDuMetier, 8> PURS_DU_METIER =
    {{
        { 164, { 9101241, 9101242 } }, { 202, { 9101241, 9101242 } }, { 755, { 9101243, 9101244 } },
        { 165, { 9101249, 9101250 } }, { 197, { 9101251, 9101252 } }, { 171, { 9101245, 9101246 } },
        { 773, { 9101245, 9101246 } }, { 333, { 9101247, 9101248 } },
    }};

    // Dosages de l'arbre de recolte (artisanat/talents.json, gabarit_recolte ; detail du 13/10, § 4).
    constexpr uint32 PCT_MAIN_SURE_RANG = 50;       // rang 1 : echec divise par 2 ; rang 2 : supprime
    constexpr uint32 PCT_PAS_VIF = 30;              // temps de recolte -30 %
    constexpr uint32 PCT_SAC_RECOLTEUR = 10;        // chance d'un materiau classique de plus
    constexpr uint32 PCT_OEIL_EXERCE_RANG = 1;      // points de chance de rare par rang
    constexpr uint32 PCT_RENDEMENT = 15;            // materiaux classiques
    constexpr uint32 PCT_PAS_DE_LOUP_RANG = 10;     // par rang : chance relative du gardien surgissant en moins
    constexpr uint32 PCT_PIEGE_REDUIT = 50;         // Paratonnerre / Sceau mortuaire : degats des pieges de sa specialite
    constexpr uint32 PCT_MAIN_PURE_RANG = 1;        // par rang : chance d'un materiau pur (noeud non garde)
    // Flair : un point d'interet sur la carte et la mini-carte vers le noeud de sa famille le plus proche.
    constexpr float FLAIR_PORTEE = 300.0f;          // m
    constexpr float FLAIR_DEJA_VU = 60.0f;          // m : plus pres, le noeud se voit, rien n'est signale
    constexpr uint32 FLAIR_PERIODE_MS = 10000;
}

#endif
