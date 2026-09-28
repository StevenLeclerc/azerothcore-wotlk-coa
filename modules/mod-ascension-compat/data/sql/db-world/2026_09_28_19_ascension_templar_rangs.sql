-- =====================================================================================
-- mod-ascension-compat — Templar (classe 19) : moitié « donnée » des rangs manquants de la
-- progression, nettoyage du 2026-09-28.
--
--   Temple Guardian 801165 : rangs 3 à 6 (803793 niv. 40, 803794 niv. 48, 803795 niv. 54,
--                            803796 niv. 60). spell_ranks s'arrête au rang 2 (803792).
--   Gift of Zeal 706634     : rang 7 (300924, niv. 65). spell_ranks s'arrête au rang 6 (300923).
--
-- La moitié C++ (lignes { 19, 801165, 80379x, niv } et { 19, 706634, 300924, 65 } dans
-- AscensionSpellProgressionData.h, fichier commun) n'est PAS dans ce lot : elle est transmise à
-- l'écrivain des fichiers communs. Ce fichier seul n'enseigne rien : il chaîne des rangs que
-- personne ne connaît (mesuré le 2026-09-28 : 0 personnage pour 803793-803796 et 300924), pour
-- que learnSpell remplace le rang précédent au lieu de le poser à côté, et que Named() (tête de
-- chaîne) reconnaisse les rangs 3+ de Temple Guardian.
--
-- Même patron que 2026_09_28_17 : chaque ligne n'est posée QUE si le rang précédent est déjà dans
-- la même chaîne (P-046). Idempotent. Updates.EnableDatabases = 0 : à appliquer à la main, AVANT
-- de démarrer le worldserver qui porte les rangs (spell_ranks se lit au démarrage).
-- =====================================================================================

DELETE FROM `spell_ranks` WHERE `spell_id` IN (803793, 803794, 803795, 803796, 300924);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 801165, 803793, 3 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 801165 AND `spell_id` = 803792 AND `rank` = 2);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 801165, 803794, 4 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 801165 AND `spell_id` = 803793 AND `rank` = 3);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 801165, 803795, 5 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 801165 AND `spell_id` = 803794 AND `rank` = 4);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 801165, 803796, 6 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 801165 AND `spell_id` = 803795 AND `rank` = 5);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 706634, 300924, 7 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 706634 AND `spell_id` = 300923 AND `rank` = 6);

-- Contrôle attendu après application : 5 lignes.
-- SELECT * FROM spell_ranks WHERE spell_id IN (803793, 803794, 803795, 803796, 300924);
