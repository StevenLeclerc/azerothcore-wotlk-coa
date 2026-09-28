-- =====================================================================================
-- mod-ascension-compat — Ranger (classe 21, famille 27) : complément du nettoyage du 2026-09-28.
-- À appliquer APRÈS 2026_09_28_21_ascension_ranger_nettoyage.sql (qui reste à appliquer tel quel).
-- Moitié « donnée » des correctifs du second passage dans AscensionRangerTalents.cpp.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_proc et spell_bonus_data
-- se rechargent à chaud (« reload spell_proc », « reload spell_bonus_data ») ; spell_script_names,
-- creature_template, creature_template_model et creature_template_addon sont lus au démarrage :
-- redémarrer le monde.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_proc
-- -------------------------------------------------------------------------------------
-- 573056 Dragonhawk Tamer : « Using a Horn now summons a Dragonhawk ». Les cors (800086, 800087,
-- 800088, 806359, 806360) sont les seuls sorts non déclenchés de famille 27 en FF2 0x1, tous en
-- DmgClass 1 : 0x14000 (magie positive ou négative faite), une fois par lancement (phase CAST).
-- 800077 Barbed Quills : dégâts de Wild Strike (FF1 0x100, DmgClass 2), Flank (FF1 0x20000,
-- DmgClass 2) et Quills (FF1 0x4, DmgClass 3), une fois par cible touchée. Le travail est fait
-- par aura_ascension_ranger_barbed_quills (saignement, prolongation, propagation).
DELETE FROM `spell_proc` WHERE `SpellId` IN (573056, 800077);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(573056, 0, 27, 0,      0, 1, 81920, 0, 1, 0, 0, 0, 0, 0, 0, 0),
(800077, 0, 27, 0, 131332, 0,   272, 1, 2, 0, 0, 0, 0, 0, 0, 0);

-- -------------------------------------------------------------------------------------
-- 2. creature_template 52393 — le Dragonhawk de Dragonhawk Tamer (573058 e0, SummonProperties 61)
-- -------------------------------------------------------------------------------------
-- Décision prise sur le texte : 573056 dit « summons a Dragonhawk », Wingman 705098 et Guidance
-- 532261 comptent « each War Falcon or Dragonhawk », et 681394 « Shows how many Dragonhawks you
-- have active ». L'infobulle de 573058 (« Grove Warden », Slow Poison) n'est pas suivie : aucune
-- donnée ne porte ce PNJ ni son sort. Le Dragonhawk est une copie du War Falcon 50393 (mêlée seule,
-- même profil), avec le modèle du « Dragonhawk » 18741 (affichage 17547) et le compteur 681394.
DELETE FROM `creature_template` WHERE `entry` = 52393;
DROP TEMPORARY TABLE IF EXISTS `tmp_ranger_dragonhawk`;
CREATE TEMPORARY TABLE `tmp_ranger_dragonhawk` SELECT * FROM `creature_template` WHERE `entry` = 50393;
UPDATE `tmp_ranger_dragonhawk` SET `entry` = 52393, `name` = 'Dragonhawk';
INSERT INTO `creature_template` SELECT * FROM `tmp_ranger_dragonhawk`;
DROP TEMPORARY TABLE `tmp_ranger_dragonhawk`;

DELETE FROM `creature_template_model` WHERE `CreatureID` = 52393;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES (52393, 0, 17547, 1, 1, NULL);

DELETE FROM `creature_template_addon` WHERE `entry` = 52393;
INSERT INTO `creature_template_addon` (`entry`, `auras`) VALUES (52393, '681394');

-- -------------------------------------------------------------------------------------
-- 3. spell_bonus_data — coefficients d'AP promis par l'infobulle et appliqués nulle part
-- -------------------------------------------------------------------------------------
-- Une ligne au premier rang sert toute la chaîne (SpellMgr::GetSpellBonusData). direct_bonus 0 garde
-- l'absence de puissance des sorts d'aujourd'hui. L'AP lue suit IsRangedWeaponSpell :
--   806368 Woodland Arrow (7 rangs) et 500071 Neurotoxin Arrow, 524870 Incendiary Shot :
--     USES_RANGED_SLOT et masque arc/arbalète -> RAP, comme le « $RAP » du texte ;
--   560966 Quills (5 rangs, masque dague) et 800360 Hookshot (6 rangs) : ni l'un ni l'autre ->
--     AP de mêlée, comme le « $AP » du texte.
DELETE FROM `spell_bonus_data` WHERE `entry` IN (806368, 560966, 500071, 524870, 800360);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(806368, 0, 0, 0.35,     0, 'Ranger - Woodland Arrow chain head: $RAP*0.35'),
(560966, 0, 0, 0.3,      0, 'Ranger - Quills chain head: $AP*0.3'),
(500071, 0, 0, 0.360687, 0, 'Ranger - Neurotoxin Arrow: $RAP*0.360687'),
(524870, 0, 0, 0.27,     0, 'Ranger - Incendiary Shot: $RAP*0.27'),
(800360, 0, 0, 0.4785,   0, 'Ranger - Hookshot chain head: $AP*0.4785');

-- -------------------------------------------------------------------------------------
-- 4. spell_script_names — scripts du second passage (lus au démarrage)
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN ('spell_ascension_ranger_corrosive_shot',
    'spell_ascension_ranger_dust_toss', 'aura_ascension_ranger_barbed_quills', 'aura_ascension_ranger_elude_talents');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(572419, 'spell_ascension_ranger_corrosive_shot'),
(807820, 'spell_ascension_ranger_dust_toss'),
(800077, 'aura_ascension_ranger_barbed_quills'),
-- Coexiste avec (801345, aura_ascension_ranger_sly) du fichier 21.
(801345, 'aura_ascension_ranger_elude_talents');
