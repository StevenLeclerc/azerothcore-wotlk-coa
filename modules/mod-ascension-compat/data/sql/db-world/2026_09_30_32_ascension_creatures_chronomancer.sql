-- =====================================================================================
-- mod-ascension-compat — Chronomancer (classe 22) : Unstable Chronoglass 503836 (nœud CA 6175),
-- la créature 506015 qui n'existait pas. 2026-09-30. NON APPLIQUÉ.
--
-- Idempotent (DELETE puis INSERT sur 506015 seulement). À appliquer AVEC le worldserver qui
-- porte AscensionChronomancerChronoglass.cpp (fichier NEUF : `construire.py --configurer
-- --build`). Sans lui, la créature apparaît mais ne détourne rien (506638 reste une aura simple
-- posée sur le verre lui-même) : sans danger, sans effet.
--
-- MESURÉ le 2026-09-30 : creature_template n'a pas 506015 ; _model, _addon, _movement non plus.
--
-- Spell.dbc serveur, lu le 2026-09-30 :
--   503836 e0 effet 28, MiscValue 506015, MiscValueB 61 (ALLY / GUARDIAN), TARGET_DEST_DEST,
--          portée 20 yd, durée 30 s. « Place an Unstable Chronoglass at target location for $d
--          that will redirect the next 3 offensive enemy spell casts onto itself. »
--   506638 e0 aura 96 SPELL_AURA_SPELL_MAGNET, TargetA 27 (MASTER), rayon 30 yd, 3 charges,
--          durée infinie. Le C++ la change, au chargement, en APPLY_AREA_AURA_OWNER sur soi :
--          possédée par le verre, appliquée au Chronomancer à 30 yd ou moins. Les sorts ennemis
--          de classe MAGIQUE à cible unique visant le Chronomancer partent sur le verre
--          (Unit::GetMagicHitRedirectTarget) ; chaque détournement retire une charge
--          (RedirectSpellEvent) ; à 0 l'aura tombe. Tout cela est du cœur natif.
--   503870 « [Aura] » (étourdissement, 20 charges, 15 s) : NON utilisé, Rooted suffit.
--
-- Source tierce (miroir db.exil.es, hertigservices/ascension-data, npcs.jsonl, lu le 2026-09-30) :
--   506015 « Unstable Chronoglass » : display 24868, Type Non-Combat Pet.
-- Modèle 24868 (COT_HourGlass_redo.mdx, celui de 27840 « Hourglass of Eternity ») présent dans
-- CreatureDisplayInfo.dbc serveur ET creature_model_info (MESURÉ).
--
-- Colonnes copiées de 31216 « Mirror Image » par table temporaire ; seules les nommées changent.
-- Niveau et faction imposés par le cœur d'après le maître (Guardian). Vie : base de classe 1 au
-- niveau du maître × 1 (exp 0) — SUPPOSÉ. Il doit rester attaquable : un aimant que
-- l'attaquant ne peut pas viser n'est pas retenu (_IsValidAttackTarget). Type 12 (du miroir) :
-- les PNJ ne l'attaquent pas d'eux-mêmes, les sorts détournés le frappent.
-- =====================================================================================

DELETE FROM `creature_template_movement` WHERE `CreatureId` = 506015;
DELETE FROM `creature_template_addon` WHERE `entry` = 506015;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 506015;
DELETE FROM `creature_template` WHERE `entry` = 506015;

DROP TEMPORARY TABLE IF EXISTS `tmp_coa_chrono_creature`;
CREATE TEMPORARY TABLE `tmp_coa_chrono_creature` SELECT * FROM `creature_template` WHERE `entry` = 31216;
UPDATE `tmp_coa_chrono_creature` SET
    `entry` = 506015, `difficulty_entry_1` = 0, `difficulty_entry_2` = 0, `difficulty_entry_3` = 0,
    `KillCredit1` = 0, `KillCredit2` = 0, `name` = 'Unstable Chronoglass', `subname` = '',
    `IconName` = '', `gossip_menu_id` = 0, `minlevel` = 1, `maxlevel` = 80, `exp` = 0, `faction` = 35,
    `npcflag` = 0, `rank` = 0, `dmgschool` = 6, `unit_class` = 1, `unit_flags` = 0, `type` = 12,
    `type_flags` = 0, `lootid` = 0, `pickpocketloot` = 0, `skinloot` = 0, `mingold` = 0, `maxgold` = 0,
    `AIName` = 'NullCreatureAI', `HealthModifier` = 1, `ManaModifier` = 1, `ArmorModifier` = 1,
    `DamageModifier` = 1, `flags_extra` = 0, `ScriptName` = '', `VerifiedBuild` = 0;
INSERT INTO `creature_template` SELECT * FROM `tmp_coa_chrono_creature`;
DROP TEMPORARY TABLE `tmp_coa_chrono_creature`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(506015, 0, 24868, 1, 1, 0);

-- L'aimant, posé à la création (Creature::LoadCreaturesAddon -> AddAura, lanceur = le verre).
INSERT INTO `creature_template_addon` (`entry`, `path_id`, `mount`, `bytes1`, `bytes2`, `emote`, `visibilityDistanceType`, `auras`) VALUES
(506015, 0, 0, 0, 0, 0, 0, '506638');

-- Immobile : sans cela le Guardian suit son maître (Spell::SummonGuardian -> MoveFollow).
INSERT INTO `creature_template_movement` (`CreatureId`, `Ground`, `Swim`, `Flight`, `Rooted`, `Chase`, `Random`) VALUES
(506015, 0, 0, 0, 1, 0, 0);
