-- Runemaster : Scrying Orb 804674 (SUMMON 28, MiscValue 50038, SummonProperties 65 = PUPPET, le
-- schema d'Eye of Kilrogg 126 -> 4277). La creature 50038 n'existe pas dans creature_template
-- (mesure le 29/09 : 50037 puis 50040) : l'invocation echoue en silence. Donnee seule, pas de
-- binaire. La possession et la vue sont natives (Puppet::InitSummon -> SetCharmedBy POSSESS).
-- Copie de 4277 (faction, drapeaux, type, vitesses, addon avec l'aura 2585 « Eye of Kilrogg
-- Passive (DND) » : furtivite et vitesse). Modele : celui de 4277 (2421), aucun modele d'orbe
-- n'est connu et on n'en invente pas.
-- SUPPOSE, a eprouver en jeu : TargetA 87 (TARGET_DEST_DEST) exige une destination envoyee par le
-- client.

DELETE FROM `creature_template` WHERE `entry` = 50038;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 50038;
DELETE FROM `creature_template_movement` WHERE `CreatureId` = 50038;
DELETE FROM `creature_template_addon` WHERE `entry` = 50038;

DROP TEMPORARY TABLE IF EXISTS `tmp_scrying_orb`;
CREATE TEMPORARY TABLE `tmp_scrying_orb` SELECT * FROM `creature_template` WHERE `entry` = 4277;
UPDATE `tmp_scrying_orb` SET `entry` = 50038, `name` = 'Scrying Orb', `subname` = '';
INSERT INTO `creature_template` SELECT * FROM `tmp_scrying_orb`;
DROP TEMPORARY TABLE `tmp_scrying_orb`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 50038, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`
FROM `creature_template_model` WHERE `CreatureID` = 4277;

DROP TEMPORARY TABLE IF EXISTS `tmp_scrying_orb_move`;
CREATE TEMPORARY TABLE `tmp_scrying_orb_move` SELECT * FROM `creature_template_movement` WHERE `CreatureId` = 4277;
UPDATE `tmp_scrying_orb_move` SET `CreatureId` = 50038;
INSERT INTO `creature_template_movement` SELECT * FROM `tmp_scrying_orb_move`;
DROP TEMPORARY TABLE `tmp_scrying_orb_move`;

DROP TEMPORARY TABLE IF EXISTS `tmp_scrying_orb_addon`;
CREATE TEMPORARY TABLE `tmp_scrying_orb_addon` SELECT * FROM `creature_template_addon` WHERE `entry` = 4277;
UPDATE `tmp_scrying_orb_addon` SET `entry` = 50038;
INSERT INTO `creature_template_addon` SELECT * FROM `tmp_scrying_orb_addon`;
DROP TEMPORARY TABLE `tmp_scrying_orb_addon`;
