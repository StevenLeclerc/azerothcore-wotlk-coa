-- Primalist (classe 31, famille 37) : correctifs de la revue du 23/10/2026
-- (docs/REVUE-classe-primalist-20261023.md, constats C-xx cites ligne par ligne).
-- Idempotent : UPDATE a valeur fixe ou OU binaire, DELETE puis INSERT.
-- spell_script_names n'est lu qu'au demarrage : appliquer AVANT de relancer le monde, avec le binaire
-- qui porte AscensionPrimalistTalents.cpp, AscensionPrimalistMountain.cpp et
-- AscensionPrimalistSpiritBeast.cpp de la meme revue. spell_proc, spell_bonus_data et
-- spell_linked_spell (retrait seulement) : au demarrage (ou .reload) ; les lignes spell_proc de C-13 et C-14 ne valent
-- qu'avec leurs scripts. A appliquer a la main (Updates.EnableDatabases = 0).
-- ProcFlags : 0x4 auto melee, 0x10 sort de melee, 0x4000 magie positif, 0x10000 magie negatif,
-- 0x40000 periodique inflige. HitMask 2 = critique.

-- ---------------------------------------------------------------------------------------------
-- C-16 Stonefaced 706201 / Nature's Will 560140 : « Casting Seismic abilities ». ProcFlags 272
-- (melee et tir) ; Seismic Spike, Crash, Tremor et Wave sont de DmgClass 1 (magie), seul Seismic
-- Smash (melee) passait. Masques et phase 1 (lancement) conserves. 0x4000 en plus du rapport :
-- Seismic Wave soigne et blesse, son lancement peut compter comme positif ; les masques ne laissent
-- passer que les sorts Seismic.
UPDATE `spell_proc` SET `ProcFlags` = 81936 WHERE `SpellId` IN (706201, 560140);

-- C-17 Rupturer 706208 : Seismic Spike (magie, direct) et Seismic Crash (periodique), pas de melee.
UPDATE `spell_proc` SET `ProcFlags` = 327680 WHERE `SpellId` = 706208;
-- C-17 Eternally Chosen 802888 : Primal Rush et Spirit Charge sont de DmgClass 1 ; phase 1 conservee.
UPDATE `spell_proc` SET `ProcFlags` = 81920 WHERE `SpellId` = 802888;

-- C-26 Earthmaker 560150 / Quakeformer 680419 : Seismic Smash (mot 2 du masque, 0x400) manquait.
UPDATE `spell_proc` SET `SpellFamilyMask2` = `SpellFamilyMask2` | 1024 WHERE `SpellId` IN (560150, 680419);

-- C-38 (cosmetique) : 3 avertissements de demarrage. 806070/806071 : ProcFlags 4 seul, les masques
-- de type et de phase ne sont pas lus. 801242 : l'effet 1 n'est pas une aura (DisableEffectsMask 3 -> 1,
-- l'effet 0 reste coupe, comportement inchange).
UPDATE `spell_proc` SET `SpellTypeMask` = 0, `SpellPhaseMask` = 0 WHERE `SpellId` IN (806070, 806071);
UPDATE `spell_proc` SET `DisableEffectsMask` = 1 WHERE `SpellId` = 801242;

-- ---------------------------------------------------------------------------------------------
-- C-13 Rexxar's Might et C-14 Legacy of Rexxar : lignes neuves.
DELETE FROM `spell_proc` WHERE `SpellId` IN (806559, 806561, 800184, 504227);
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
-- 806559 : critiques PHYSIQUES du joueur (auto, techniques de melee, et techniques physiques de
-- DmgClass magie comme Seismic Spike) -> 806561 sur le familier (natif, tA 5)
(806559,1,0,0,0,0,65556,1,2,2,0,0,0,0,0,0),
-- 806561 (sur le familier) : prochain coup du familier, une charge ; e1 DUMMY coupe ;
-- aura_ascension_primalist_rexxars_might_pet lance 806562 avec la PA du joueur
(806561,0,0,0,0,0,20,1,2,0,0,2,0,0,0,1),
-- 800184 : critiques des sorts de melee (Wildclaw, Rylak's Bite, joueur) et des periodiques
-- (saignement de Primal Shred, familier) ; aiguillage dans aura_ascension_primalist_legacy_of_rexxar
(800184,0,0,0,0,0,262160,1,2,2,0,0,0,0,0,0),
-- 504227 Misha's Rage : auto-attaques -> 560975, « only once per second »
(504227,0,0,0,0,0,4,0,0,0,0,0,0,0,1000,0);

-- ---------------------------------------------------------------------------------------------
-- C-08 Earthmother's Protection 560298 : son support 504853 (e2 = cout de Hand of the Earthmother,
-- que 560298 ramene a -50 %) n'etait jamais pose. Pose par aura_ascension_primalist_rock_barrier (503630),
-- seulement avec le talent et a la duree de Rock Barrier. Pas de lien spell_linked_spell : sans condition,
-- il affichait l'icone chez tout Primalist. Le DELETE retire le lien d'une version anterieure de ce fichier.
DELETE FROM `spell_linked_spell` WHERE `spell_trigger` = 503630 AND `spell_effect` = 504853 AND `type` = 2;

-- ---------------------------------------------------------------------------------------------
-- Coefficients (une ligne sur le rang 1 couvre la chaine spell_ranks, SpellMgr::GetSpellBonusData).
DELETE FROM `spell_bonus_data` WHERE `entry` IN (300693, 520468, 681089, 301203, 560975);
INSERT INTO `spell_bonus_data` (`entry`,`direct_bonus`,`dot_bonus`,`ap_bonus`,`ap_dot_bonus`,`comments`) VALUES
-- C-07 : le terme $STA*.5 est ajoute en C++ (primalist_mountain_fury_stamina)
(300693,0,0,0.22,0,'Local Primalist: Seismic Smash (5 ranks), "$AP*.22" ($STA*.5 in C++)'),
-- C-25
-- 520468 est d'ecole physique : la puissance des sorts (ecole 126) n'y entre pas, seul $AP*0.278 compte
-- (lecture de SpellBaseDamageBonusDone, non mesure). La valeur SP est gardee, conforme a l'infobulle.
(520468,0.325,0,0.278,0,'Local Primalist: Judgement of the Three Hammers hammer, "$SP*0.325+$AP*0.278"'),
(681089,0,0,0,0.08,'Local Primalist: Raging Earth fissure tick (706622), "$AP*0.08"'),
-- C-15 : 301203 est dans StockCoefficientSpells (BonusMultiplier remis a 0)
(301203,0.35,0,0.25,0,'Local Primalist: Running on Instinct heal, "$AP*0.25+$SP*0.35"'),
-- C-14 : PA du lanceur (le familier pour sa copie de Misha's Rage)
(560975,0,0,0.125,0,'Local Primalist: Misha''s Rage extra hit, "$AP*0.125"');

-- ---------------------------------------------------------------------------------------------
-- Scripts (binaire de la meme revue).
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
 'spell_ascension_primalist_cooldown_chain','spell_ascension_primalist_cooldown_percent',
 'spell_ascension_primalist_fury_of_the_earthmother','aura_ascension_primalist_stone_skin',
 'aura_ascension_primalist_rockslide','aura_ascension_primalist_rexxars_might_pet',
 'aura_ascension_primalist_legacy_of_rexxar','spell_hun_primalist_pet_scaling',
 'aura_ascension_primalist_primal_shred_crit','aura_ascension_primalist_rock_barrier');
INSERT INTO `spell_script_names` (`spell_id`,`ScriptName`) VALUES
-- C-05 / C-06 : effet 165 sur toute la chaine de rangs
(680411,'spell_ascension_primalist_cooldown_chain'),
(681365,'spell_ascension_primalist_cooldown_chain'),
-- 572826 Spirits of Life (pose par Eternally Chosen, C-17) : e1 = 165 a -5000 ms sur Primal Rush 500696
(572826,'spell_ascension_primalist_cooldown_chain'),
-- C-24 : effet 192 (pourcentage de recharge)
(807249,'spell_ascension_primalist_cooldown_percent'),
(560306,'spell_ascension_primalist_cooldown_percent'),
-- C-09
(520134,'spell_ascension_primalist_fury_of_the_earthmother'),
-- C-10
(573308,'aura_ascension_primalist_stone_skin'),
-- C-22
(560154,'aura_ascension_primalist_rockslide'),
-- C-13
(806561,'aura_ascension_primalist_rexxars_might_pet'),
-- C-14
(800184,'aura_ascension_primalist_legacy_of_rexxar'),
-- C-14 : saignements de Primal Shred, rangs 1 a 9 (TriggerSpell de l'effet 0 de 500940, 502725..502732)
(504669,'aura_ascension_primalist_primal_shred_crit'),(504670,'aura_ascension_primalist_primal_shred_crit'),
(504671,'aura_ascension_primalist_primal_shred_crit'),(504672,'aura_ascension_primalist_primal_shred_crit'),
(504673,'aura_ascension_primalist_primal_shred_crit'),(504674,'aura_ascension_primalist_primal_shred_crit'),
(504675,'aura_ascension_primalist_primal_shred_crit'),(504676,'aura_ascension_primalist_primal_shred_crit'),
(504684,'aura_ascension_primalist_primal_shred_crit'),
-- C-08 : Rock Barrier pose 504853 avec Earthmother's Protection
(503630,'aura_ascension_primalist_rock_barrier'),
-- C-20 / C-21 : second script de 34902. Son nom DOIT trier apres 'spell_hun_generic_scaling'
-- (ordre de lecture de la table = ordre des crochets de calcul), voir AscensionPrimalistSpiritBeast.cpp.
(34902,'spell_hun_primalist_pet_scaling');
