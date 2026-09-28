-- =====================================================================================
-- mod-ascension-compat — Ranger (classe 21, famille 27) : troisième passage du nettoyage du 2026-09-28.
-- À appliquer APRÈS 2026_09_28_21_ascension_ranger_nettoyage.sql puis
-- 2026_09_28_58_ascension_ranger_complements.sql. Moitié « donnée » des correctifs de
-- AscensionRangerSecondary.cpp et AscensionRangerTalents.cpp de ce passage.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_bonus_data se recharge à
-- chaud (« reload spell_bonus_data ») ; spell_script_names, creature_template et
-- creature_template_model sont lus au démarrage : redémarrer le monde.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_bonus_data — coefficients promis par l'infobulle et appliqués nulle part
-- -------------------------------------------------------------------------------------
-- 704314 Quick-Witted (riposte de 704311) : « ${$704314m2+$704314ppl2+$AP*1} ». Sans le coefficient,
--   1 point de dégâts. Ni USES_RANGED_SLOT ni masque d'arme à distance : AP de mêlée, comme « $AP ».
-- 806342 Whipvine Arrow : « ${$m2+0+$RAP*.33} ». attr0 0x10012 (USES_RANGED_SLOT) : RAP lue.
-- 586212 Quel'dorei Poison (tick) : « +$RAP*.15 » par tick ; la RAP est imposée par
--   UseRangedAttackPowerForDamage (ranger_secondary_contracts), sinon l'AP de mêlée serait lue.
-- direct_bonus / dot_bonus 0 : aucune puissance des sorts, comme aujourd'hui (BonusMultiplier 0).
DELETE FROM `spell_bonus_data` WHERE `entry` IN (704314, 806342, 586212);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(704314, 0, 0, 1.0,  0,    'Ranger - Quick-Witted stab: $AP*1'),
(806342, 0, 0, 0.33, 0,    'Ranger - Whipvine Arrow: $RAP*.33'),
(586212, 0, 0, 0,    0.15, 'Ranger - Quel''dorei Poison tick: $RAP*.15');

-- -------------------------------------------------------------------------------------
-- 2. spell_script_names — scripts de ce passage (lus au démarrage)
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN ('spell_ascension_ranger_whipvine_arrow',
    'aura_ascension_ranger_onslaught_elude_benefits', 'aura_ascension_ranger_skirmish_brutal_shot');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(806342, 'spell_ascension_ranger_whipvine_arrow'),
(801951, 'aura_ascension_ranger_onslaught_elude_benefits'),
(802039, 'aura_ascension_ranger_skirmish_brutal_shot');

-- -------------------------------------------------------------------------------------
-- 3. creature_template 503001 — le faucon de Falcon Scout 561395 (e0 SUMMON, SummonProperties 1301,
--    catégorie 3 PUPPET). La créature n'existait nulle part : rien n'apparaissait, pas de possession.
-- -------------------------------------------------------------------------------------
-- Copie du War Falcon 50264 (même modèle 22633), unit_flags 256 comme l'Eye of Kilrogg 4277, un
-- puppet qui marche. Déplacement au sol : pas de ligne creature_template_movement. Pas d'aura de
-- compagnon (creature_template_addon) : le scout ne compte pas pour Wingman ni Guidance.
DELETE FROM `creature_template_model` WHERE `CreatureID` = 503001;
DELETE FROM `creature_template_movement` WHERE `CreatureId` = 503001;
DELETE FROM `creature_template_addon` WHERE `entry` = 503001;
DELETE FROM `creature_template` WHERE `entry` = 503001;
DROP TEMPORARY TABLE IF EXISTS `tmp_ranger_falcon_scout`;
CREATE TEMPORARY TABLE `tmp_ranger_falcon_scout` SELECT * FROM `creature_template` WHERE `entry` = 50264;
UPDATE `tmp_ranger_falcon_scout` SET `entry` = 503001, `name` = 'Falcon Scout', `subname` = '', `unit_flags` = 256,
    `AIName` = '', `ScriptName` = '';
INSERT INTO `creature_template` SELECT * FROM `tmp_ranger_falcon_scout`;
DROP TEMPORARY TABLE `tmp_ranger_falcon_scout`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES (503001, 0, 22633, 1, 1, NULL);
