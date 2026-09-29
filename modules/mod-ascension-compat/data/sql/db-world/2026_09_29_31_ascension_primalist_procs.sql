-- Primalist (classe 31, famille 37) : vague 5 du nettoyage, lignes spell_proc.
-- Chaque talent ci-dessous porte une aura 42 (PROC_TRIGGER_SPELL) avec ProcFlags = 0 dans
-- Spell.dbc et aucune ligne spell_proc : SpellMgr::LoadSpellProcs ne genere rien
-- (SpellMgr.cpp:2248-2250) et Aura::GetProcEffectMask rend 0 (SpellAuras.cpp:2150-2154).
-- Chance = 0 partout : le coeur reprend alors le ProcChance du DBC. Charges = 0 : le coeur
-- reprend le ProcCharges du DBC (SpellMgr.cpp:2112-2113).
-- ProcFlags : 0x4 auto melee, 0x8 subi auto melee, 0x10 sort melee, 0x20 subi sort melee,
-- 0x40 auto distance, 0x80 subi auto distance, 0x100 sort distance, 0x200 subi sort distance,
-- 0x400 sort sans classe positif, 0x1000 sans classe negatif, 0x4000 magie positif,
-- 0x10000 magie negatif, 0x40000 periodique inflige.
-- 69972 = 0x11154 (degats directs), 87312 = 0x15510 (tout sort inflige), 279552 = 0x44400.
-- HitMask : 2 critique, 52 = MISS|DODGE|PARRY. AttributesMask 2 = PROC_ATTR_TRIGGERED_CAN_PROC.
-- Les talents a condition (seuil de vie, id de sort, rang) ont un AuraScript DoCheckProc dans
-- AscensionPrimalist*.cpp, enregistre par 2026_09_29_33_ascension_primalist_scripts.sql.
-- Lu au demarrage ou par `.reload spell_proc`. A appliquer a la main (Updates.EnableDatabases = 0).

DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN (
 574323,680395,504214,504198,680964,300742,680410,504732,504226,560548,300733,520466,681364,504874,
 300690,301087,500294,500937,300738,300867,503717,504196,537212,555723,560150,560154,560169,560182,
 560510,561191,680412,680419,706138,503630,704402,520464,680444,520469,503718,300731,300735,
 706171,706216,800141,707367,706176,807558,706344,706622,805643,800185,707616,802595);

INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
-- Tremors / Mountain Giant / Lifebinder : periodique, auto melee, soins
(574323,0,0,0,0,0,262144,1,2,0,0,0,0,0,0,0),
(680395,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0),
(504214,0,0,0,0,0,279552,2,2,0,0,0,0,0,0,0),
-- Degats directs, auto-attaques
(504198,0,0,0,0,0,69972,1,2,0,0,0,0,0,0,0),
(680964,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0),
(300742,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0),
(680410,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0),
-- Thorns : physique, inflige et subi, melee et distance (0x3FC) ; 504784 part sur l'autre unite
(504732,1,0,0,0,0,1020,1,2,0,0,0,0,0,0,0),
(504226,0,0,0,0,0,69972,1,2,2,0,0,0,0,0,0),
-- Resources of the Earth : critiques de degats seulement (lecture litterale de l'infobulle)
(560548,0,0,0,0,0,69972,1,2,2,0,0,0,0,0,0),
(300733,0,0,0,0,0,279552,2,2,0,0,0,0,0,0,0),
-- Judgement of The Three Hammers : degats directs, 20 % du DBC
(520466,0,0,0,0,0,69972,1,2,0,0,0,0,0,0,0),
-- Spiritbound : attaques subies evitees
(681364,0,0,0,0,0,680,0,0,52,0,0,0,0,0,0),
-- Bloody Claws (aura posee sur le familier par l'effet 190)
(504874,0,0,0,0,0,332116,1,2,0,0,0,0,0,0,0),
-- Famille 37, filtre par masque (EffectSpellClassMask vaut 0 sur ces talents)
(300690,0,37,0,0,16,87312,0,1,0,0,0,0,0,0,0),
(301087,0,37,0,0,16,69972,1,2,2,0,0,0,0,0,0),
(500294,0,37,0,0,80,69972,1,2,0,0,0,0,0,0,0),
-- Totemic Echoes : Totemic Smash (masque2 16, classe melee). DoCheckProc : rangs de la chaine
-- 800178 et cible principale seulement (exclut Brutalize 560314, Crash 803534, AoE 500945).
(500937,0,37,0,0,16,16,1,2,0,0,0,0,0,0,0),
(300738,0,37,0,8,0,87312,2,2,0,0,0,0,0,0,0),
(300867,0,37,0,256,0,87312,0,2,0,0,0,0,0,0,0),
(503717,0,37,16,0,0,87312,0,1,0,0,0,0,0,0,0),
(504196,0,37,524288,0,0,69972,1,2,2,0,0,0,0,0,0),
-- DisableEffectsMask 4 : e2 relancerait un second Seismic Grasp
(537212,0,37,0,0,262144,87312,1,2,0,0,4,0,0,0,0),
(555723,0,37,0,4194304,0,87312,1,2,0,0,0,0,0,0,0),
(560150,0,37,16,4194304,262400,349456,1,2,0,0,0,0,0,0,0),
(560154,0,37,0,512,0,87312,0,1,0,2,0,0,0,0,0),
(560169,0,37,64,0,0,262144,1,2,0,0,0,0,0,0,0),
-- Geode Barrage frappe par un sort d'aide declenche : TRIGGERED_CAN_PROC
(560182,0,37,4096,32,0,87312,1,2,0,2,0,0,0,0,0),
(560510,0,37,64,0,0,262144,1,2,0,0,0,0,0,0,0),
(561191,0,37,0,0,536870912,87312,0,1,0,0,0,0,0,0,0),
(680412,0,37,4096,32,0,87312,1,2,0,2,0,0,0,0,0),
(680419,0,37,524304,4194304,262144,87312,0,1,0,0,0,0,0,0,0),
(706138,0,37,16,4194304,262144,349456,3,2,0,0,0,0,0,0,0),
-- Rock Barrier : chaque coup subi consomme une des 10 charges du DBC et lance Geode sur l'attaquant
(503630,0,0,0,0,0,680,1,0,0,0,0,0,0,0,0),
-- One With The Earth : lancement de Stoneshard
(704402,0,37,0,512,0,87312,0,1,0,0,0,0,0,0,0),
-- Mineralization : tout sort de famille 37 au lancement ; DoCheckProc : vie <= 35 %
(520464,0,37,0,0,0,87312,0,1,0,0,0,0,0,0,0),
-- Cataclysm, moitie Stoneshard (la moitie Earthshaping est en C++) ; 10 % du DBC
(680444,0,37,0,512,0,87312,0,1,0,0,0,0,0,0,0),
-- Shattering : degats de chaque marteau 520468 (declenche, classe melee) ; DoCheckProc : id 520468
(520469,0,0,0,0,0,16,1,2,0,2,0,0,0,0,0),
-- Infused With Power : soins de Hand of the Earthmother (0,8,0) ou Hammer of Life 806073 (0,0,0x100000),
-- ce dernier declenche ; 20 % du DBC ; DoCheckProc : soin effectif > 0
(503718,0,37,0,8,1048576,16384,2,2,0,2,0,0,0,0,0),
-- Infusion of Neptulon : soin critique -> 300735 ; 300735 perd une charge par attaque (3 charges)
(300731,0,0,0,0,0,17408,2,2,2,0,0,0,0,0,0),
(300735,0,0,0,0,0,20,0,2,0,0,0,0,0,0,3),
-- Primal Strikes : critiques des techniques melee et distance
(706171,1,0,0,0,0,272,1,2,2,0,0,0,0,0,0),
-- Primal Power : lancement de Seismic Wave (classe magie) ; 805463 recoit 1 charge en C++
(706216,0,37,0,256,0,81920,0,1,0,0,0,0,0,0,0),
-- Rips and Tears : DoT du joueur ; les ticks du familier passent par un UnitScript
(800141,0,37,0,0,0,262144,1,2,0,0,0,0,0,0,0),
-- Spiritual Frenzy : lancement de Primal Shred (classe melee) ; DoCheckProc : chaine 500940
(707367,0,37,0,0,32,16,0,1,0,0,0,0,0,0,0),
-- Son of Ursoc : lancement de Savage Frenzy (classe aucune) ; e1/e2 sont des mods, pas des procs
(706176,0,37,0,16384,0,5120,0,1,0,0,6,0,0,0,0),
-- Douse : sorts Seismic touchant un ennemi (melee, distance, magie negatif), degats seuls
(807558,0,37,80,4194560,1024,65808,1,2,0,0,0,0,0,0,0),
-- Heavy Handed : soin de Hand of the Earthmother (magie positif), phase HIT (cible alliee)
(706344,0,37,0,8,0,16384,2,2,0,0,0,0,0,0,0),
-- Raging Earth : Quake ; une fissure par seconde au plus, donc une par Quake
(706622,0,37,0,0,256,16,1,2,0,0,0,0,0,1000,0),
-- Mountain Mover : attaque evitee (MISS|DODGE|PARRY) ; e0 et e1 ne sont pas des procs
(805643,0,0,0,0,0,168,0,0,52,0,3,0,0,0,0),
-- Primal Shaman's Mask : degats directs, 10 % du DBC
(800185,0,0,0,0,0,65812,1,2,0,0,0,0,0,0,0),
-- Mountain Avatar : degats de Geode Barrage 803138 ou de Geode 804002 (magie, declenches) ;
-- AuraScript : donner OU prolonger Earthen Avatar, jamais les deux
(707616,0,37,4096,0,4096,65536,1,2,0,2,0,0,0,0,0),
-- Rylak's Blessing : Rylak's Bite (classe melee ; exclut Life 801163, classe magie)
(802595,0,37,0,0,64,16,1,2,0,0,0,0,0,0,0);
