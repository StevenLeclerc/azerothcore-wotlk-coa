-- P-336 (28/10/2026) : les « Upgrade Kit » d'Ascension ne font rien sur ce serveur, et piegent le joueur.
--
-- Chaque kit (62 sorts d'usage, effet 53 ENCHANT_ITEM) pose l'enchantement 20912, un parametre fictif
-- d'Ascension que le client affiche « +0 stam » dans l'emplacement d'enchantement permanent : il ne monte
-- AUCUN palier, et il ecrase un vrai enchantement. Sur ce serveur, une piece d'artisanat change de palier
-- par l'Atelier (fiche de personnage, bouton Atelier, onglet « Mon equipement », bouton « Ameliorer »),
-- message COA_ATL AMELIORER, table coa_artisanat_etape (D-PO-24).
-- Remontee du 28/10 : un joueur a fabrique 4 kits (recettes 93788, 93790...) et les a poses sur ses
-- Spallieres du Traqueur de foudre (palier 10), sans effet.
--
--   1. retire de l'entraineur 200001 (« Book of Artisans ») les 67 recettes qui fabriquent un kit ;
--   2. desactive pour les joueurs (disables, sourceType 0, flags 1) ces 67 recettes, deja apprises par
--      721 personnages, et les 62 sorts d'usage des kits deja fabriques.
-- Listes lues dans Spell.dbc : recette = effet 24 (CREATE_ITEM, champs 71+e / ItemType 107+e) vers un objet
-- dont le nom contient « Upgrade Kit » (temoin 93790 -> 85464) ; usage = effet 53 a MiscValue (110+e) 20912.
-- Idempotent. A chaud : .reload trainer, puis .reload disables.

DELETE FROM `trainer_spell`
WHERE `TrainerId` = 200001
  AND `SpellId` IN (
    93788, 93790, 93800, 93867, 93885, 93894, 93902, 93982, 94032, 94034, 94039, 94062, 94074, 
    94076, 94082, 94084, 94086, 94088, 94090, 94110, 94126, 94128, 94129, 94130, 94200, 94215, 
    94219, 94231, 94352, 94354, 94360, 94381, 94387, 94389, 94411, 94449, 94489, 94513, 94517, 
    94521, 94525, 94559, 94581, 94590, 94596, 94643, 94645, 94647, 94649, 94671, 94713, 94715, 
    94717, 94719, 94721, 94723, 94725, 94727, 94802, 117614, 424401, 424402, 1015015, 1015016, 
    1015017, 1015018, 1015019);

DELETE FROM `disables` WHERE `sourceType` = 0 AND `entry` IN (
    93788, 93790, 93800, 93867, 93885, 93894, 93902, 93982, 94032, 94034, 94039, 94062, 94074, 
    94076, 94082, 94084, 94086, 94088, 94090, 94110, 94126, 94128, 94129, 94130, 94200, 94215, 
    94219, 94231, 94352, 94354, 94360, 94381, 94387, 94389, 94411, 94449, 94489, 94513, 94517, 
    94521, 94525, 94559, 94581, 94590, 94596, 94643, 94645, 94647, 94649, 94671, 94713, 94715, 
    94717, 94719, 94721, 94723, 94725, 94727, 94802, 117614, 424401, 424402, 1015015, 1015016, 
    1015017, 1015018, 1015019,
    93789, 93791, 93801, 93868, 93886, 93895, 93903, 93983, 94033, 94035, 94040, 94044, 94063, 
    94075, 94077, 94083, 94085, 94087, 94089, 94091, 94111, 94127, 94201, 94216, 94220, 94232, 
    94353, 94355, 94361, 94382, 94388, 94390, 94412, 94450, 94490, 94514, 94518, 94522, 94526, 
    94560, 94582, 94591, 94597, 94644, 94646, 94648, 94650, 94672, 94714, 94716, 94718, 94720, 
    94722, 94724, 94726, 94728, 94803, 424400, 1015005, 1015006, 1015007, 1015008, 1015009);

INSERT INTO `disables` (`sourceType`, `entry`, `flags`, `params_0`, `params_1`, `comment`)
SELECT 0, s.`ID`, 1, '', '', s.`motif` FROM (
    SELECT 93788 AS `ID`, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)' AS `motif`
    UNION ALL
    SELECT 93790, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93800, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93867, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93885, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93894, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93902, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93982, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94032, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94034, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94039, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94062, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94074, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94076, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94082, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94084, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94086, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94088, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94090, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94110, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94126, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94128, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94129, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94130, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94200, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94215, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94219, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94231, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94352, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94354, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94360, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94381, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94387, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94389, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94411, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94449, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94489, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94513, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94517, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94521, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94525, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94559, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94581, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94590, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94596, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94643, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94645, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94647, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94649, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94671, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94713, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94715, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94717, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94719, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94721, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94723, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94725, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94727, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 94802, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 117614, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 424401, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 424402, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 1015015, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 1015016, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 1015017, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 1015018, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 1015019, 'P-336 : recette de kit Ascension (+0 stam, aucun palier)'
    UNION ALL
    SELECT 93789, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93791, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93801, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93868, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93886, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93895, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93903, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 93983, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94033, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94035, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94040, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94044, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94063, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94075, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94077, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94083, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94085, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94087, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94089, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94091, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94111, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94127, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94201, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94216, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94220, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94232, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94353, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94355, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94361, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94382, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94388, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94390, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94412, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94450, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94490, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94514, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94518, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94522, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94526, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94560, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94582, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94591, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94597, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94644, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94646, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94648, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94650, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94672, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94714, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94716, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94718, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94720, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94722, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94724, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94726, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94728, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 94803, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 424400, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 1015005, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 1015006, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 1015007, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 1015008, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
    UNION ALL
    SELECT 1015009, 'P-336 : usage de kit Ascension (enchantement 20912, +0 stam)'
) s;
