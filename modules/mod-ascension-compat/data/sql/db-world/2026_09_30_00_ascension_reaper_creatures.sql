-- =====================================================================================
-- mod-ascension-compat — Reaper (classe 30, famille 36) : les deux créatures invoquées qui
-- n'existaient pas, et le proc de Gravesite. 2026-09-30.
--
-- Idempotent (DELETE puis INSERT). Updates.EnableDatabases = 0 : à appliquer à la main, AVANT
-- de démarrer le worldserver qui porte npc_ascension_reaper_summon et
-- aura_ascension_reaper_gravesite (AscensionReaperSecondary.cpp) : creature_template.ScriptName
-- et spell_script_names ne se lisent qu'au démarrage. Sans le binaire, la ligne spell_proc de
-- 572213 ferait partir 300979 sur TOUT coup critique, sans le contrôle « dans la Gravesite » :
-- appliquer ce fichier AVEC le binaire, jamais avant.
--
-- MESURÉ le 2026-09-29 : creature_template n'a ni 100481 ni 850000, creature_template_model
-- non plus ; spell_proc et spell_script_names n'ont rien sur 572213.
--
-- 100481  Spectral Warden : Spell.dbc 805716 e0 = effet 28, MiscValue 100481, MiscValueB 61.
-- 850000  The End Is Near : Spell.dbc 707100 e0 = effet 28, MiscValue 850000, MiscValueB 61.
--   SummonProperties.dbc 61 = Category 1 (ALLY), Type 2 (GUARDIAN) : Map::SummonCreature en
--   fait un Guardian possédé par le Reaper (Minion::InitStats -> Unit::SetMinion).
--
-- Colonnes : copiées de 31216 « Mirror Image » (creature_template de ce serveur, Guardian de
-- joueur, npc_pet_mage_mirror_image), par INSERT ... SELECT, pour ne pas dépendre de l'ordre
-- des colonnes. Seules celles nommées ci-dessous changent. Niveau, faction, vie et dégâts sont
-- de toute façon imposés par le cœur (SummonGuardian) puis par le script, d'après le maître.
--
-- Modèles, tous deux présents dans creature_template_model ET CreatureDisplayInfo.dbc :
--   100481 -> 30978, le modèle de 38173 « Spectral Footman » (soldat spectral, translucide :
--             CreatureDisplayInfo 30978 alpha 184, CreatureModelData 49 HumanMale.mdx) ;
--             creature_model_info 30978 existe (BoundingRadius 0.4284, CombatReach 2.1).
--   850000 -> 11686, celui de 31216 « Mirror Image » (InvisibleStalker.mdx) : l'apparence
--             vient du clonage natif (Clone Me! 45204, SPELL_AURA_CLONE_CASTER), exactement
--             comme l'Image miroir du mage.
-- =====================================================================================

DELETE FROM `creature_template_model` WHERE `CreatureID` IN (100481, 850000);
DELETE FROM `creature_template` WHERE `entry` IN (100481, 850000);

-- Copie par table temporaire : même structure que creature_template par construction, donc
-- aucune liste de colonnes à tenir, et 31216 n'est jamais touchée.
DROP TEMPORARY TABLE IF EXISTS `tmp_reaper_creatures`;
CREATE TEMPORARY TABLE `tmp_reaper_creatures` SELECT * FROM `creature_template` WHERE `entry` = 31216;
INSERT INTO `tmp_reaper_creatures` SELECT * FROM `creature_template` WHERE `entry` = 31216;

-- Première copie -> 100481 Spectral Warden : guerrier (unit_class 1), mort-vivant (type 6),
-- frappe en mêlée. Nom = nom du sort 805716.
UPDATE `tmp_reaper_creatures` SET `entry` = 100481 WHERE `entry` = 31216 LIMIT 1;
-- Seconde copie -> 850000 : lanceur (unit_class 8 de 31216 gardé), type 7 (humanoïde) gardé.
-- Nom = nom du sort 707100 « The End Is Near Summon », sans le suffixe technique.
UPDATE `tmp_reaper_creatures` SET `entry` = 850000 WHERE `entry` = 31216 LIMIT 1;

UPDATE `tmp_reaper_creatures` SET
    `difficulty_entry_1` = 0, `difficulty_entry_2` = 0, `difficulty_entry_3` = 0,
    `KillCredit1` = 0, `KillCredit2` = 0, `subname` = '', `IconName` = '', `gossip_menu_id` = 0,
    `minlevel` = 1, `maxlevel` = 80, `faction` = 35, `npcflag` = 0,
    `lootid` = 0, `pickpocketloot` = 0, `skinloot` = 0, `mingold` = 0, `maxgold` = 0,
    `AIName` = '', `ScriptName` = 'npc_ascension_reaper_summon', `VerifiedBuild` = 0;
UPDATE `tmp_reaper_creatures` SET `name` = 'Spectral Warden', `unit_class` = 1, `type` = 6,
    `HealthModifier` = 1, `DamageModifier` = 1 WHERE `entry` = 100481;
UPDATE `tmp_reaper_creatures` SET `name` = 'The End Is Near', `HealthModifier` = 1, `DamageModifier` = 1
    WHERE `entry` = 850000;

INSERT INTO `creature_template` SELECT * FROM `tmp_reaper_creatures`;
DROP TEMPORARY TABLE `tmp_reaper_creatures`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(100481, 0, 30978, 1, 1, 0),
(850000, 0, 11686, 1, 1, 0);

-- -------------------------------------------------------------------------------------
-- Gravesite 572213 : e0 = aura 42 (PROC_TRIGGER_SPELL) vers 300979, ProcFlags DBC 0, ProcChance
-- 100 (Spell.dbc champs 34 et 35, MESURÉ). Coups DIRECTS seulement : 0x4 mêlée auto, 0x10 sort
-- mêlée, 0x1000 sort sans classe, 0x10000 sort magique = 69652 ; aucun drapeau périodique.
-- SpellTypeMask 1 (dégâts), SpellPhaseMask 2 (impact), HitMask 2 (critique),
-- AttributesMask 2 (PROC_ATTR_TRIGGERED_CAN_PROC) : les vrais coups de Dirge (801311) sont
-- déclenchés. Chance 0 = ProcChance du DBC. Le contrôle « ennemi dans une Gravesite » est
-- aura_ascension_reaper_gravesite (DoCheckProc).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` = 572213;
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
(572213, 0, 0, 0, 0, 0, 69652, 1, 2, 2, 2, 0, 0, 0, 0, 0);

DELETE FROM `spell_script_names` WHERE `spell_id` = 572213 AND `ScriptName` = 'aura_ascension_reaper_gravesite';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (572213, 'aura_ascension_reaper_gravesite');
