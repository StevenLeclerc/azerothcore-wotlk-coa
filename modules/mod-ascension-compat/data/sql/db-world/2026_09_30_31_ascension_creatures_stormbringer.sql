-- =====================================================================================
-- mod-ascension-compat — Stormbringer (classe 16, famille 22) : les deux créatures invoquées
-- qui n'existaient pas, et les trois talents qui les appellent. 2026-09-30. NON APPLIQUÉ.
--
-- Idempotent (DELETE puis INSERT sur les seules entrées créées ici). Updates.EnableDatabases = 0 :
-- à appliquer à la main, AVEC le worldserver qui porte AscensionStormbringerSummons.cpp
-- (npc_ascension_stormbringer_water_elemental, aura_ascension_stormbringer_electrified_waters,
-- stormbringer_static_electricity). Sans ce binaire, la ligne spell_proc 573436 ferait partir
-- 573437 sur TOUT coup critique direct, sans le contrôle « ennemi sous Drown ». Fichier C++
-- NEUF : construire avec `amont/construire.py --configurer --build` (le glob cmake doit le voir).
--
-- MESURÉ le 2026-09-30 : creature_template n'a ni 310603 ni 50074 ; creature_template_model,
-- _addon, _movement non plus ; spell_proc et spell_script_names n'ont rien sur 524954, 573436,
-- 707542.
--
-- Spell.dbc serveur, lu le 2026-09-30 :
--   573438 e0 effet 28, MiscValue 310603, MiscValueB 61, durée 20 s.
--   801802 e0 effet 28, MiscValue 50074, MiscValueB 61, TARGET_DEST_DEST, durée 16 s ;
--          e1 effet 64 -> 804084 (+10 Static, sur soi).
--   SummonProperties.dbc 61 = (61, Category 1 ALLY, 0, Type 2 GUARDIAN, 0, Flags 2) :
--     Guardian du Stormbringer ; niveau = celui du maître (Spell::SummonGuardian) ; faction =
--     celle du maître (Minion::InitStats). minlevel/maxlevel/faction ci-dessous sont écrasés.
--
-- Source tierce (miroir db.exil.es, hertigservices/ascension-data, npcs.jsonl, lu le 2026-09-30) :
--   50074 « Thunder Orb » : display 29352, Type Non-Combat Pet.
--   310603 : « casts » 573441 et Skull Smash 560532 ; pas de display publié.
--
-- Colonnes : copiées de 31216 « Mirror Image » (Guardian de joueur) par table temporaire,
-- comme 2026_09_30_00_ascension_reaper_creatures.sql ; seules les colonnes nommées changent.
-- `exp` = 0 : creature_classlevelstats.basehp1/basehp2 valent 1 sous le niveau 60/80 (MESURÉ),
-- et Guardian::InitStatsForLevel lit BaseHealth[exp].
-- Modèles, tous présents dans CreatureDisplayInfo.dbc serveur ET creature_model_info (MESURÉ) :
--   310603 -> 525   (Creature\WATERELEMENTAL\WaterElemental.mdx, celui de 510) — SUPPOSÉ ;
--   50074  -> 29352 (SPELLS\SnowFlakeCreature_Var1.mdx, celui de 503201 Power Sphere) — du miroir.
-- =====================================================================================

DELETE FROM `creature_template_movement` WHERE `CreatureId` IN (310603, 50074);
DELETE FROM `creature_template_addon` WHERE `entry` IN (310603, 50074);
DELETE FROM `creature_template_model` WHERE `CreatureID` IN (310603, 50074);
DELETE FROM `creature_template` WHERE `entry` IN (310603, 50074);

-- -------------------------------------------------------------------------------------
-- 310603 Electrified Water Elemental. Élémentaire (type 4), guerrier (unit_class 1), mêlée
-- Givre (dmgschool 4, SUPPOSÉ). Vie et dégâts SUPPOSÉS, imposés toutes les 2 s par le script
-- d'après le maître : 40 % de sa vie max ; par coup (2,0 s) 2 à 3 × niveau + 20 à 30 % de sa
-- puissance des sorts Givre. Au plus 3 par Stormbringer (SUPPOSÉ). À sa disparition, le maître
-- vivant regagne 2 s sur Drown (573442, sur toute la chaîne 806407).
-- Auras posées à la création (creature_template_addon) :
--   573441 « Aura on Summons » : e1 effet 143 APPLY_AREA_AURA_OWNER, dummy « You have an
--          Electrified Water Elemental active! » sur le maître ; durée 20 s ;
--   807135 « [Visual] » : aura 4 dummy, infinie.
-- -------------------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS `tmp_coa_storm_creature`;
CREATE TEMPORARY TABLE `tmp_coa_storm_creature` SELECT * FROM `creature_template` WHERE `entry` = 31216;
UPDATE `tmp_coa_storm_creature` SET
    `entry` = 310603, `difficulty_entry_1` = 0, `difficulty_entry_2` = 0, `difficulty_entry_3` = 0,
    `KillCredit1` = 0, `KillCredit2` = 0, `name` = 'Electrified Water Elemental', `subname` = '',
    `IconName` = '', `gossip_menu_id` = 0, `minlevel` = 1, `maxlevel` = 80, `exp` = 0, `faction` = 35,
    `npcflag` = 0, `rank` = 0, `dmgschool` = 4, `BaseAttackTime` = 2000, `unit_class` = 1,
    `unit_flags` = 0, `type` = 4, `type_flags` = 0, `lootid` = 0, `pickpocketloot` = 0, `skinloot` = 0,
    `mingold` = 0, `maxgold` = 0, `AIName` = '', `HealthModifier` = 1, `ManaModifier` = 1,
    `ArmorModifier` = 1, `DamageModifier` = 1, `flags_extra` = 0,
    `ScriptName` = 'npc_ascension_stormbringer_water_elemental', `VerifiedBuild` = 0;
INSERT INTO `creature_template` SELECT * FROM `tmp_coa_storm_creature`;
DROP TEMPORARY TABLE `tmp_coa_storm_creature`;

-- -------------------------------------------------------------------------------------
-- 50074 Thunder Orb. Immobile, sans IA de combat : tout ce qu'il fait vient de ses auras.
--   801863 « Orb Shock » : e0 aura 23 toutes les 2 s -> 801868 (dégâts Nature autour de l'orbe,
--          jusqu'à $i ennemis, + aura 271 « dégâts subis des orbes » 8 s) ; e2 aura 133 -90 %
--          de vie max. Texte d'aura : « Shocking nearby enemies ... every $t1 sec. » ;
--   803005 « Thunder Orb » : aura 12 (étourdi) + aura 25 (pacifié), « Cannot move. »
--          Un sort déclenché par une aura périodique est lancé TRIGGERED_FULL_MASK : l'étourdissement
--          ne l'empêche pas.
-- AIName NullCreatureAI (aucune mêlée), type 12 Non-Combat Pet (du miroir : les PNJ ne
-- l'attaquent pas d'eux-mêmes, Creature::CanStartAttack ; les sorts l'atteignent), attaquable
-- (« until destroyed »). Rooted en plus de l'étourdissement : il ne suit pas le maître.
-- Vie : base de classe 1 au niveau du maître × 1, puis -90 % par 801863 (SUPPOSÉ : c'est le DBC).
-- Dégâts : ceux de 801868 lancé par l'orbe, qui n'a aucune puissance des sorts (bp 16 du DBC).
-- -------------------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS `tmp_coa_storm_creature`;
CREATE TEMPORARY TABLE `tmp_coa_storm_creature` SELECT * FROM `creature_template` WHERE `entry` = 31216;
UPDATE `tmp_coa_storm_creature` SET
    `entry` = 50074, `difficulty_entry_1` = 0, `difficulty_entry_2` = 0, `difficulty_entry_3` = 0,
    `KillCredit1` = 0, `KillCredit2` = 0, `name` = 'Thunder Orb', `subname` = '',
    `IconName` = '', `gossip_menu_id` = 0, `minlevel` = 1, `maxlevel` = 80, `exp` = 0, `faction` = 35,
    `npcflag` = 0, `rank` = 0, `dmgschool` = 3, `unit_class` = 1, `unit_flags` = 0, `type` = 12,
    `type_flags` = 0, `lootid` = 0, `pickpocketloot` = 0, `skinloot` = 0, `mingold` = 0, `maxgold` = 0,
    `AIName` = 'NullCreatureAI', `HealthModifier` = 1, `ManaModifier` = 1, `ArmorModifier` = 1,
    `DamageModifier` = 1, `flags_extra` = 0, `ScriptName` = '', `VerifiedBuild` = 0;
INSERT INTO `creature_template` SELECT * FROM `tmp_coa_storm_creature`;
DROP TEMPORARY TABLE `tmp_coa_storm_creature`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(310603, 0, 525, 1, 1, 0),
(50074, 0, 29352, 1, 1, 0);

INSERT INTO `creature_template_addon` (`entry`, `path_id`, `mount`, `bytes1`, `bytes2`, `emote`, `visibilityDistanceType`, `auras`) VALUES
(310603, 0, 0, 0, 1, 0, 0, '573441 807135'),
(50074, 0, 0, 0, 0, 0, 0, '801863 803005');

INSERT INTO `creature_template_movement` (`CreatureId`, `Ground`, `Swim`, `Flight`, `Rooted`, `Chase`, `Random`) VALUES
(50074, 0, 0, 0, 1, 0, 0);

-- -------------------------------------------------------------------------------------
-- 573436 Electrified Waters : e0 aura 42 -> 573437, ProcFlags DBC 0, ProcChance 100 (MESURÉ).
-- Coups DIRECTS : 0x4 mêlée auto, 0x10 sort mêlée, 0x1000 sort sans classe, 0x10000 sort
-- magique = 69652 ; aucun drapeau périodique. SpellTypeMask 1 (dégâts), SpellPhaseMask 2
-- (impact), HitMask 2 (critique), AttributesMask 0 (les sorts déclenchés — Conduction, fin de
-- Drown 806408 — ne comptent pas). Chance 0 = ProcChance du DBC. Le contrôle « ennemi sous
-- Drown de ce Stormbringer » et le remplacement de 573437 par 573438 sont
-- aura_ascension_stormbringer_electrified_waters.
--
-- 707542 Binder of Storms : e0 aura 42 -> 801802, ProcChance 60 (MESURÉ). « Damage dealt by
-- Conduction now has a $h% chance to summon a Thunder Orb. » Famille 22, masque (32768,0,0) :
-- porté par le seul 567560 Conduction dans toute la famille 22 (MESURÉ). ProcFlags 0x10000 :
-- Conduction est DmgClass 1 MAGIC (Spell.dbc champ 213, témoins 133/78/75 vérifiés).
-- AttributesMask 2 (PROC_ATTR_TRIGGERED_CAN_PROC) : Conduction est toujours lancée déclenchée
-- (AscensionStormbringerTalents.cpp). Action par défaut du cœur : 801802 lancé sur l'ennemi
-- touché -> orbe à ses pieds (TARGET_DEST_DEST pris de la cible, Spell::InitExplicitTargets).
-- Cooldown 15000 (relecture du 2026-09-30) : 801802 porte RecoveryTime 15000 (Spell.dbc champ 29,
-- MESURÉ), mais un sort déclenché par proc (TRIGGERED_FULL_MASK) n'arme aucune recharge
-- (Spell::SendSpellCooldown, TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD, LU) : sans ce plafond, un orbe
-- de 16 s à dégâts de zone toutes les 2 s partirait sur 60 % des Conduction, sans limite de nombre.
--
-- 524954 Static Electricity : pas de ligne. Son sort déclenché 573451 est l'effet 178,
-- EffectNULL dans ce cœur ; le critique de Torrential Wrath est lu par
-- stormbringer_static_electricity (AllSpellScript), qui lance 573438.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` IN (573436, 707542);
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
(573436, 0, 0, 0, 0, 0, 69652, 1, 2, 2, 0, 0, 0, 0, 0, 0),
(707542, 0, 22, 32768, 0, 0, 65536, 1, 2, 0, 2, 0, 0, 0, 15000, 0);

DELETE FROM `spell_script_names` WHERE `spell_id` = 573436 AND `ScriptName` = 'aura_ascension_stormbringer_electrified_waters';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (573436, 'aura_ascension_stormbringer_electrified_waters');
