-- =====================================================================================
-- mod-ascension-compat — Primalist (classe 31, famille 37) : Spirit of Life 840002, qui
-- n'existait pas, et le proc de Primordial Earth 806536. 2026-09-30. NON APPLIQUÉ.
--
-- Idempotent (DELETE puis INSERT sur 840002 et 806536 seulement). À appliquer AVEC le
-- worldserver qui porte AscensionPrimalistSpiritOfLife.cpp (npc_ascension_primalist_spirit_of_life,
-- fichier NEUF : `construire.py --configurer --build`). Sans lui, les esprits apparaîtraient
-- sans rien lancer.
--
-- MESURÉ le 2026-09-30 : creature_template n'a pas 840002 ; spell_proc n'a rien sur 806536 ;
-- 802888 Eternally Chosen a déjà sa ligne (2026_09_20_03, masque (16384,131072,0), phase CAST).
--
-- Spell.dbc serveur, lu le 2026-09-30 :
--   806536 Primordial Earth : e0 aura 42 -> 572853, ProcFlags DBC 0, ProcChance 100.
--          « Critical strikes with melee attacks now summon 1 Spirit of Life that immediately
--          casts a lesser Spirit Charge on a nearby ally ... »
--   572853 e0 effet 28, bp 1, MiscValue 840002, MiscValueB 61 ; 572826 (Eternally Chosen) idem
--          avec bp 2 (deux esprits, SummonProperties 61 compte bp) et e1 effet 165 -> 500696 -5 s.
--          TARGET_DEST_CASTER_RADIUS 10 yd, durée 5 s.
--   572830 Lesser Spirit Charge (lancé par l'esprit) : charge + soin sur le membre du groupe le
--          plus proche + dégâts Nature (5 yd) autour du Primalist. Le script ajoute les termes du maître
--          ($spn*.2 + $AP*.2). 572827 (aura périodique 1,5 s) n'est PAS posée : voir le .cpp.
--
-- Source tierce (miroir db.exil.es, hertigservices/ascension-data, npcs.jsonl, lu le 2026-09-30) :
--   840002 « Spirit of Life » : display 8824, Type Undead, « casts » 572830 (recharge 16 s).
-- Modèle 8824 (Creature\Ent\Ent.mdx, EntSkinDead, alpha 128) présent dans CreatureDisplayInfo.dbc
-- serveur ET creature_model_info (MESURÉ).
--
-- Colonnes copiées de 31216 « Mirror Image » par table temporaire ; seules les nommées changent.
-- Non attaquable, non sélectionnable : unit_flags 2 (NON_ATTACKABLE) | 33554432 (NOT_SELECTABLE).
-- Vie sans objet (inattaquable) ; exp 0 pour ne pas tomber à 1 PV sous le niveau 60.
-- =====================================================================================

DELETE FROM `creature_template_model` WHERE `CreatureID` = 840002;
DELETE FROM `creature_template` WHERE `entry` = 840002;

DROP TEMPORARY TABLE IF EXISTS `tmp_coa_primal_creature`;
CREATE TEMPORARY TABLE `tmp_coa_primal_creature` SELECT * FROM `creature_template` WHERE `entry` = 31216;
UPDATE `tmp_coa_primal_creature` SET
    `entry` = 840002, `difficulty_entry_1` = 0, `difficulty_entry_2` = 0, `difficulty_entry_3` = 0,
    `KillCredit1` = 0, `KillCredit2` = 0, `name` = 'Spirit of Life', `subname` = '',
    `IconName` = '', `gossip_menu_id` = 0, `minlevel` = 1, `maxlevel` = 80, `exp` = 0, `faction` = 35,
    `npcflag` = 0, `rank` = 0, `dmgschool` = 3, `unit_class` = 2, `unit_flags` = 33554434, `type` = 6,
    `type_flags` = 0, `lootid` = 0, `pickpocketloot` = 0, `skinloot` = 0, `mingold` = 0, `maxgold` = 0,
    `AIName` = '', `HealthModifier` = 1, `ManaModifier` = 1, `ArmorModifier` = 1, `DamageModifier` = 1,
    `flags_extra` = 0, `ScriptName` = 'npc_ascension_primalist_spirit_of_life', `VerifiedBuild` = 0;
INSERT INTO `creature_template` SELECT * FROM `tmp_coa_primal_creature`;
DROP TEMPORARY TABLE `tmp_coa_primal_creature`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(840002, 0, 8824, 1, 1, 0);

-- -------------------------------------------------------------------------------------
-- 806536 : la ligne proposée par le partage du 30/09, vérifiée. ProcFlags 20 = 0x4 mêlée auto
-- | 0x10 sort de classe mêlée ; SpellTypeMask 1 (dégâts), SpellPhaseMask 2 (impact), HitMask 2
-- (critique), AttributesMask 0 (pas les sorts déclenchés). Famille 0 : 0x4 n'est pas dans
-- SPELL_PROC_FLAG_MASK, le type et la phase ne filtrent que les sorts. Chance 0 = DBC 100.
-- Action par défaut : 572853 lancé par le Primalist ; l'esprit apparaît près de lui.
-- Pas de temps de recharge (le DBC n'en donne pas) : un esprit par critique de mêlée.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` = 806536;
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
(806536, 0, 0, 0, 0, 0, 20, 1, 2, 2, 0, 0, 0, 0, 0, 0);
