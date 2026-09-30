-- Primalist : Earthmother's Roar 301307 et Sacred Grove (tick) 800179 rendent une part des PV,
-- du mana et de la Rage MANQUANTS, par cible, et non du maximum.
-- Écarts 2026-09-29-primalist-301307 et 2026-09-29-primalist-800179 (docs/ECARTS-classes-20260929.md).
--
-- LU Spell.dbc : 301307 e0 HEAL_PCT (136) 20 ; 800179 e0 HEAL_PCT 3, e1 ENERGIZE_PCT (137) mana 3,
-- e2 ENERGIZE_PCT Rage 3. Infobulles : 301306 « $301307s1% of their missing health », 800180
-- « $800179s1% of their missing health, Rage, and mana ».
-- MESURÉ le 2026-09-30 : aucune ligne spell_script_names pour 301307 ni 800179.
-- Le script spell_ascension_primalist_missing_pct est dans AscensionPrimalistTalents.cpp :
-- appliquer AVEC le binaire qui le porte (sinon « Script named ... does not have a script »
-- au démarrage, et rien ne change). spell_script_names n'est lu qu'au démarrage.
-- Idempotent.

DELETE FROM `spell_script_names` WHERE `spell_id` IN (301307, 800179) AND `ScriptName` = 'spell_ascension_primalist_missing_pct';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(301307, 'spell_ascension_primalist_missing_pct'),
(800179, 'spell_ascension_primalist_missing_pct');
