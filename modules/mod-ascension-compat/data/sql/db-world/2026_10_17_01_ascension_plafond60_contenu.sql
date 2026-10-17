-- Plafond du jeu au niveau 60, lot 4 : contenu du monde (docs/PLAN-plafond-60.md § 3.1, décisions § 4 :
-- D-C1 a, D-C2 b, D-C3 a, D-C4 a, D-K2 a). Écrit le 17/10/2026 (Paris) pour le PTR (acore_world_ptr).
-- Onyxia (D-C5) n'est PAS ouverte au 60, après revue : c'est le raid du 80 (Onyxia niveau 83, créatures
-- de la carte 249 niveau 80 à 83, butin ItemLevel 232 niveau requis 80, mesuré le 17/10). Décision rendue au PO.
-- Aucun nom de base : appliquer-sql-module.sh choisit la base selon --cible.
--
-- RETOUR ARRIÈRE : ../retour/2026_10_17_01_ascension_plafond60_contenu-RETOUR.sql
--   (rangé HORS de db-world/ exprès : appliquer-sql-module.sh joue tout *.sql de db-world/ non
--   enregistré dans `updates`, il jouerait le RETOUR juste après l'aller).
--   Ce RETOUR porte les lignes d'origine LUES SUR LE PTR le 17/10/2026. Pour le Live, le rebâtir
--   depuis le dump du lot 0 du Live (outils/plafond60-dumps-lot0.sh --cible live).
--
-- ORDRE D'APPLICATION (plan § 3.1) : le 571 doit déjà être retiré de `AiPlayerbot.RandomBotMaps` et des
-- cartes du Directeur quand ce fichier est joué, sinon le journal se remplit de « forbidden map ».
-- Tout est lu au démarrage (disables, transports, areatrigger_teleport, trainer_spell, battleground_template,
-- item_loot_template) : royaume ARRÊTÉ, comme le fait appliquer-sql-module.sh.
--
-- Une seule transaction : en cas d'erreur, le client mysql sort et rien n'est écrit.
-- Les INSERT de `disables` ne sont PAS précédés d'un DELETE : aucune de ces clés n'existe sur le PTR
-- (mesuré le 17/10) ; si l'une existait, l'INSERT échoue et le fichier entier est annulé, plutôt que
-- d'écraser une ligne que le RETOUR ne saurait pas rendre.

SET NAMES utf8mb4;
START TRANSACTION;

-- 1. Norfendre fermé (D-C1/D-C3). DisableMgr.cpp : carte MAP_COMMON => flags 0 obligatoire.
--    Préalable : plus AUCUN personnage dans le 571 quand ce fichier est joué (réinitialisation des bots,
--    plan § 0.2). Le refus de carte ne joue qu'à TeleportTo : un personnage déconnecté dans le 571 s'y
--    reconnecte, et s'il y meurt, le retour au cimetière (TeleportTo vers 571) est refusé [S, lu dans le code].
--    Mesuré le 17/10 : 16 personnages au PTR dans le 571, niveau 68 et plus ; 401 bots au Live (relevé de
--    la revue indépendante). Contrôle au lot 8 :
--    SELECT COUNT(*) FROM characters WHERE map = 571  -- doit rendre 0.
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(2, 571, 0, '', '', 'Plafond 60 : Norfendre ferme');

-- 2. Donjons de l'Outreterre hors recherche de donjon (sourceType 8 = DISABLE_TYPE_LFG_MAP,
--    flags 3 = normal | héroïque). Ce sont les 9 donjons de l'Outreterre à min_level 55
--    (dungeon_access_template, lu le 17/10) ; les autres (552-555, 560, 269, 585) demandent 64 à 68.
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(8, 540, 3, '', '', 'Plafond 60 : The Shattered Halls hors LFG'),
(8, 542, 3, '', '', 'Plafond 60 : The Blood Furnace hors LFG'),
(8, 543, 3, '', '', 'Plafond 60 : Hellfire Ramparts hors LFG'),
(8, 545, 3, '', '', 'Plafond 60 : The Steamvault hors LFG'),
(8, 546, 3, '', '', 'Plafond 60 : The Underbog hors LFG'),
(8, 547, 3, '', '', 'Plafond 60 : The Slave Pens hors LFG'),
(8, 556, 3, '', '', 'Plafond 60 : Sethekk Halls hors LFG'),
(8, 557, 3, '', '', 'Plafond 60 : Mana Tombs hors LFG'),
(8, 558, 3, '', '', 'Plafond 60 : Auchenai Crypts hors LFG');

-- 3. Quêtes de liaison vers l'Outreterre (sourceType 1).
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(1, 9407,  0, '', '', 'Plafond 60 : Through the Dark Portal (H)'),
(1, 10119, 0, '', '', 'Plafond 60 : Through the Dark Portal (A)'),
(1, 9441,  0, '', '', 'Plafond 60 : Envoy to the Mag''har'),
(1, 11451, 0, '', '', 'Plafond 60 : Alicia''s Poem');

-- 4. Téléporteurs d'ingénierie vers l'Outreterre (sourceType 0, flags 1 = SPELL_DISABLE_PLAYER,
--    DisableMgr.h). 36890 « Area52 Transporter », 36941 « Toshley's Station Transporter » (Spell.dbc du PTR).
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(0, 36890, 1, '', '', 'Plafond 60 : Area52 Transporter'),
(0, 36941, 1, '', '', 'Plafond 60 : Toshley''s Station Transporter');

-- 5. Porte des Ténèbres : seule voie terrestre de 0 vers 530. Ne PAS toucher 4409 (draeneï/elfes de sang).
DELETE FROM `areatrigger_teleport` WHERE `ID` = 4354 AND `target_map` = 530;

-- 6. Transports vers le Norfendre (D-C3 a). Le guid 9 (Auberdine - Azuremyst) est gardé.
--    10 Northspear, 11 Cloudkisser, 12 The Mighty Wind, 17 The Kraken.
DELETE FROM `transports` WHERE (`guid`, `entry`) IN ((10, 181688), (11, 181689), (12, 186238), (17, 190536));

-- 7. Dresseur 200001 (D-C2 b, métiers à 375) : les 14 rangs Grand maître, identifiés par leur nom
--    dans le Spell.dbc du PTR (« Grand Master … »), et les recettes 36954 « Dimensional Ripper - Area 52 »,
--    36955 « Ultrasafe Transporter - Toshley's Station ». Jamais de DELETE du dresseur entier.
--    45380 Scribe, 50299 First Aid, 50301 Herbalist, 50307 Skinner, 50309 Miner, 51293 Fishing,
--    51295 Cook, 51298 Blacksmith, 51301 Leatherworker, 51303 Alchemist, 51305 Engineer, 51308 Tailor,
--    51310 Jewelcrafter, 51312 Enchanter.
DELETE FROM `trainer_spell` WHERE `TrainerId` = 200001 AND `SpellId` IN
(45380, 50299, 50301, 50307, 50309, 51293, 51295, 51298, 51301, 51303, 51305, 51308, 51310, 51312,
 36954, 36955);

-- 8. Œil du cyclone ouvert au 60 (D-C4 a ; même geste qu'Armageddon, sql/05).
UPDATE `battleground_template` SET `MinLvl` = 60 WHERE `ID` = 7 AND `MinLvl` = 61;

-- 9. Téléportations vers l'Outreterre (carte 530, qu'on ne peut pas fermer : zones de départ elfes de sang
--    et draeneï). sourceType 0, flags 1 = SPELL_DISABLE_PLAYER : Spell::prepare (Spell.cpp:3532) refuse le
--    sort quels que soient les drapeaux de déclenchement, donc aussi quand mod-teleport-actionbar le lance
--    depuis l'objet (CastFromItem). 33 sorts, tous présents dans le Spell.dbc du PTR ET du Live (lu le 17/10).
--    - Mages : 33690 et 35715 « Teleport: Shattrath » sont enseignés AU NIVEAU 60 (trainer_spell 26 et 27,
--      ReqLevel 60) et par l'Arcane Tome 1000537 (RequiredLevel 60) : voie ouverte mesurée.
--      33691 et 35717 « Portal: Shattrath » (niveau 65) : fermés par cohérence.
--    - Pierres de retraite (Stone of Retreat, objets RequiredLevel 0 qui apprennent le sort par 55884
--      « Learning ») dont la cible (spell_target_position) est en Outreterre : 102178-102201, 777016-777019,
--      777022 (et donc les objets 1175644-1175646, qui apprennent 102182, 777019, 777018).
--    Gardés : 777014 (Silvermoon) et 777015 (Exodar), zones de départ. Non couverts (supposé sans canal à
--    60) : Blessed Medallion of Karabor 41234 (RequiredLevel 68 à 70), objets de quête de l'Outreterre.
--    Au 17/10, 0 personnage du PTR connaît l'un de ces sorts et 0 de ces objets existe (mesuré).
INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`) VALUES
(0, 33690, 1, '', '', 'Plafond 60 : Teleport: Shattrath (mage, dresseur 26, niveau 60 ; Arcane Tome 1000537)'),
(0, 35715, 1, '', '', 'Plafond 60 : Teleport: Shattrath (mage, dresseurs 26 et 27, niveau 60)'),
(0, 33691, 1, '', '', 'Plafond 60 : Portal: Shattrath (mage, niveau 65 ; Arcane Tome 1000524)'),
(0, 35717, 1, '', '', 'Plafond 60 : Portal: Shattrath (mage, niveau 65)'),
(0, 102178, 1, '', '', 'Plafond 60 : Stone of Retreat: Allerian Stronghold'),
(0, 102179, 1, '', '', 'Plafond 60 : Stone of Retreat: Altar of Shatar'),
(0, 102180, 1, '', '', 'Plafond 60 : Stone of Retreat: Cenarion Refuge'),
(0, 102181, 1, '', '', 'Plafond 60 : Stone of Retreat: Cosmowrench'),
(0, 102182, 1, '', '', 'Plafond 60 : Stone of Retreat: Evergrove'),
(0, 102183, 1, '', '', 'Plafond 60 : Stone of Retreat: Falcon Watch'),
(0, 102184, 1, '', '', 'Plafond 60 : Stone of Retreat: Garadar'),
(0, 102185, 1, '', '', 'Plafond 60 : Stone of Retreat: Honor Hold'),
(0, 102186, 1, '', '', 'Plafond 60 : Stone of Retreat: Ogri''la'),
(0, 102187, 1, '', '', 'Plafond 60 : Stone of Retreat: Orebor Harborage'),
(0, 102188, 1, '', '', 'Plafond 60 : Stone of Retreat: Sanctum of the Stars'),
(0, 102189, 1, '', '', 'Plafond 60 : Stone of Retreat: Shadowmoon Village'),
(0, 102190, 1, '', '', 'Plafond 60 : Stone of Retreat: Stonebreaker Hold'),
(0, 102191, 1, '', '', 'Plafond 60 : Stone of Retreat: Swamprat Post'),
(0, 102192, 1, '', '', 'Plafond 60 : Stone of Retreat: Sylvanaar'),
(0, 102193, 1, '', '', 'Plafond 60 : Stone of Retreat: Telaar'),
(0, 102194, 1, '', '', 'Plafond 60 : Stone of Retreat: Telredor'),
(0, 102195, 1, '', '', 'Plafond 60 : Stone of Retreat: Temple of Telhamat'),
(0, 102196, 1, '', '', 'Plafond 60 : Stone of Retreat: The Stormspire'),
(0, 102197, 1, '', '', 'Plafond 60 : Stone of Retreat: Thrallmar'),
(0, 102198, 1, '', '', 'Plafond 60 : Stone of Retreat: Thunderlord Stronghold'),
(0, 102199, 1, '', '', 'Plafond 60 : Stone of Retreat: Toshley''s Station'),
(0, 102200, 1, '', '', 'Plafond 60 : Stone of Retreat: Wildhammer Stronghold'),
(0, 102201, 1, '', '', 'Plafond 60 : Stone of Retreat: Zabra''jin'),
(0, 777016, 1, '', '', 'Plafond 60 : Stone of Retreat: Shattrath'),
(0, 777017, 1, '', '', 'Plafond 60 : Stone of Retreat: Area 52'),
(0, 777018, 1, '', '', 'Plafond 60 : Stone of Retreat: Altar of Sha''tar'),
(0, 777019, 1, '', '', 'Plafond 60 : Stone of Retreat: Sanctum of the Stars'),
(0, 777022, 1, '', '', 'Plafond 60 : Stone of Retreat: Area 52 (objet Stormspire, Deprecated)');

-- 10. Coffres Manastorm de fin de partie (D-K2 a). AscensionManastormRules.h::CacheForLevel :
--     97883 = joueur au-dessus de 59 hors fin de partie ; 1278050 (profondeur < 25) et 1278051 (>= 25)
--     = fin de partie, déclenchée dès que le groupe atteint max(60, MaxPlayerLevel) (AscensionManastorm.cpp).
--     Chaque coffre est un seul groupe 1 à chances égales (Chance 0), tiré par OnBeforeLootEqualChanced,
--     qui ne garde que l'utilisable au plus près du niveau de l'ouvreur. Même forme gardée ici.
--     Borne commune : RequiredLevel <= 60 ET ItemLevel <= 66 (même seuil que le coffre de la Faille, D-D2) ;
--     les verts RequiredLevel 60 d'ItemLevel 67-90 sont des objets de l'Outreterre.

-- 10a. 97883 : on garde ses lignes à RequiredLevel <= 60 et ItemLevel <= 66 (630 sur 2 592 au PTR).
DELETE `l` FROM `item_loot_template` `l`
LEFT JOIN `item_template` `i` ON `i`.`entry` = `l`.`Item`
WHERE `l`.`Entry` = 97883
  AND (`i`.`entry` IS NULL OR `i`.`RequiredLevel` > 60 OR `i`.`ItemLevel` > 66);

-- 10b. Les deux coffres de fin de partie sont rebâtis.
DELETE FROM `item_loot_template` WHERE `Entry` IN (1278050, 1278051);

-- Réservoir « objets qui tombent vraiment » (S) : armes et armures (class 2, 4) portables,
-- présents dans creature_loot_template ou reference_loot_template (ligne d'objet, pas de référence,
-- sans quête), entry < 100000 (hors objets CoA), sans rang d'honneur ni réputation requis.
--
-- 1278050 « Normal Dungeons » (original : 207 bleus du palier 80) :
--   les bleus (Quality 3) RequiredLevel 55-60, ItemLevel <= 66, du coffre 97883 (déjà réduit en 10a)
--   ou du réservoir S.
INSERT INTO `item_loot_template`
  (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`)
SELECT 1278050, `i`.`entry`, 0, 0, 0, 1, 1, 1, 1, 'Manastorm level-cap equipment (plafond 60)'
FROM `item_template` `i`
WHERE `i`.`Quality` = 3 AND `i`.`RequiredLevel` BETWEEN 55 AND 60 AND `i`.`ItemLevel` <= 66
  AND (`i`.`entry` IN (SELECT `Item` FROM `item_loot_template` WHERE `Entry` = 97883)
       OR (`i`.`class` IN (2, 4) AND `i`.`InventoryType` > 0 AND `i`.`entry` < 100000
           AND `i`.`RequiredHonorRank` = 0 AND `i`.`RequiredReputationFaction` = 0
           AND (`i`.`entry` IN (SELECT `Item` FROM `creature_loot_template`  WHERE `Reference` = 0 AND `QuestRequired` = 0)
             OR `i`.`entry` IN (SELECT `Item` FROM `reference_loot_template` WHERE `Reference` = 0 AND `QuestRequired` = 0))));

-- 1278051 « Heroic Dungeons » (original : 126 bleus ItemLevel 200 + 1 354 épiques jusqu'à 232) :
--   les bleus de 1278050 d'ItemLevel >= 62 (le haut de la tranche),
--   et les épiques (Quality 4) RequiredLevel 55-60, ItemLevel 58-66, du réservoir S
--   (essentiellement les épiques de raid de l'ancien monde à ItemLevel 63-66, ensembles T1 compris).
-- Mesuré au PTR le 17/10, en transaction annulée : 97883 = 630 lignes (380 bleus, 250 verts) ;
-- 1278050 = 477 bleus ; 1278051 = 51 bleus + 220 épiques. Aucun objet orphelin, RequiredLevel 55-60.
INSERT INTO `item_loot_template`
  (`Entry`, `Item`, `Reference`, `Chance`, `QuestRequired`, `LootMode`, `GroupId`, `MinCount`, `MaxCount`, `Comment`)
SELECT 1278051, `i`.`entry`, 0, 0, 0, 1, 1, 1, 1, 'Manastorm level-cap equipment (plafond 60)'
FROM `item_template` `i`
WHERE (`i`.`Quality` = 3 AND `i`.`ItemLevel` >= 62
       AND `i`.`entry` IN (SELECT `Item` FROM `item_loot_template` WHERE `Entry` = 1278050))
   OR (`i`.`Quality` = 4 AND `i`.`RequiredLevel` BETWEEN 55 AND 60 AND `i`.`ItemLevel` BETWEEN 58 AND 66
       AND `i`.`class` IN (2, 4) AND `i`.`InventoryType` > 0 AND `i`.`entry` < 100000
       AND `i`.`RequiredHonorRank` = 0 AND `i`.`RequiredReputationFaction` = 0
       AND (`i`.`entry` IN (SELECT `Item` FROM `creature_loot_template`  WHERE `Reference` = 0 AND `QuestRequired` = 0)
         OR `i`.`entry` IN (SELECT `Item` FROM `reference_loot_template` WHERE `Reference` = 0 AND `QuestRequired` = 0)));

COMMIT;
