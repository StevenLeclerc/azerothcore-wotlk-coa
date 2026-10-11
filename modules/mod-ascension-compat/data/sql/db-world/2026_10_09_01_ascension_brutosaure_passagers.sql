-- Mighty Caravan Brutosaur (sort et creature 499937) : passagers PNJ.
-- La creature etait importee comme monture ordinaire (VehicleId = 0) : aucun
-- siege, donc aucun passager. On lui donne le vehicule 8226 (copie du 312 du
-- Traveler's Tundra Mammoth, sieges 10001/10002 tournes de +/-90 degres en
-- roulis : les os d'attache du modele couchaient les PNJ ; DBC serveur ET client,
-- outils/ajouter-sieges-brutosaure.py, P-188) et deux PNJ, sur le modele de 32633.
-- PREREQUIS : Vehicle.dbc portant 8226 dans /opt/coa/server/data/dbc.
--   499960 : commissaire-priseur, faction 35 => hotel des ventes NEUTRE
--            (AllowTwoSide.Interaction.Auction = 0 ; le Brutosaure est commun
--            aux deux factions).
--   499961 : marchand-reparateur, copie de Gnimo (32639), memes articles.
-- Les deux reprennent npc_traveler_mammoth_vendor : ils se retirent et
-- disparaissent quand le cavalier met pied a terre.
-- Les nouvelles entrees creature_template exigent un redemarrage du monde
-- (.reload creature_template ne charge pas une entree inconnue).

UPDATE `creature_template` SET `VehicleId` = 8226 WHERE `entry` = 499937;

DELETE FROM `npc_spellclick_spells` WHERE `npc_entry` = 499937;
INSERT INTO `npc_spellclick_spells` (`npc_entry`, `spell_id`, `cast_flags`, `user_type`)
SELECT 499937, `spell_id`, `cast_flags`, `user_type` FROM `npc_spellclick_spells` WHERE `npc_entry` = 32633;

-- Gabarits : copie de Gnimo (32639), puis ajustements.
DELETE FROM `creature_template` WHERE `entry` IN (499960, 499961);
DROP TEMPORARY TABLE IF EXISTS `tmp_brutosaure`;
CREATE TEMPORARY TABLE `tmp_brutosaure` AS SELECT * FROM `creature_template` WHERE `entry` = 32639;
UPDATE `tmp_brutosaure` SET `entry` = 499960, `name` = 'Caravan Auctioneer', `subname` = 'Auctioneer',
    `faction` = 35, `npcflag` = 2097152, `VerifiedBuild` = NULL;
INSERT INTO `creature_template` SELECT * FROM `tmp_brutosaure`;
UPDATE `tmp_brutosaure` SET `entry` = 499961, `name` = 'Caravan Trader', `subname` = 'Traveling Trader',
    `npcflag` = (SELECT `npcflag` FROM `creature_template` WHERE `entry` = 32639);
INSERT INTO `creature_template` SELECT * FROM `tmp_brutosaure`;
DROP TEMPORARY TABLE `tmp_brutosaure`;

DELETE FROM `creature_template_model` WHERE `CreatureID` IN (499960, 499961);
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(499960, 0, 7993, 1, 1, NULL),   -- modele d'Auctioneer Beardo (8661)
(499961, 0, 28282, 1, 1, NULL);  -- modele de Gnimo (32639)

DELETE FROM `npc_vendor` WHERE `entry` = 499961;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 499961, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, NULL FROM `npc_vendor` WHERE `entry` = 32639;

-- Repliques de depart (SAY_DISMISS = groupe 0 du script).
DELETE FROM `creature_text` WHERE `CreatureID` IN (499960, 499961);
INSERT INTO `creature_text` (`CreatureID`, `GroupID`, `ID`, `Text`, `Type`, `Language`, `Probability`, `Emote`, `Duration`, `Sound`, `BroadcastTextId`, `TextRange`, `comment`) VALUES
(499960, 0, 0, 'Auction closed! I''ll find my own way back.', 12, 0, 100, 0, 0, 0, 0, 0, 'Caravan Auctioneer - dismount'),
(499961, 0, 0, 'Leaving already? Fine, I''ll walk home.', 12, 0, 100, 0, 0, 0, 0, 0, 'Caravan Trader - dismount');

DELETE FROM `vehicle_template_accessory` WHERE `entry` = 499937;
INSERT INTO `vehicle_template_accessory` (`entry`, `accessory_entry`, `seat_id`, `minion`, `description`, `summontype`, `summontimer`) VALUES
(499937, 499960, 1, 0, 'Mighty Caravan Brutosaur - Auctioneer', 6, 30000),
(499937, 499961, 2, 0, 'Mighty Caravan Brutosaur - Vendor & Repairer', 6, 30000);
