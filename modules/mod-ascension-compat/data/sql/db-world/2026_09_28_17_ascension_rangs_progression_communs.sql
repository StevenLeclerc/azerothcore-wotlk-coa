-- =====================================================================================
-- mod-ascension-compat — fichiers communs, nettoyage du 2026-09-28 : moitié « donnée » des
-- trois rangs ajoutés à la main dans AscensionSpellProgressionData.h (std::array 2709 -> 2712).
--
--   300887 Brutal Shout rang 7      (Barbarian, racine 300857, niveau 65)
--   707678 Inquisitor's Edict rang 7 (Witch Hunter, racine 706741, niveau 65)
--   707906 Storm Alert rang 3        (Stormbringer, racine 707044, niveau 56)
--
-- Chaque ligne n'est posée QUE si le rang précédent est déjà dans la même chaîne : une chaîne
-- absente ou différente de ce que l'audit a mesuré laisse la table intacte (P-046 : ne pas créer
-- de chaîne là où il n'y en a pas). Idempotent : DELETE préalable sur l'id du rang ajouté.
-- Updates.EnableDatabases = 0 : à appliquer à la main.
--
-- ORDRE D'INSTALLATION : appliquer ce fichier AVANT (ou avec) le worldserver qui porte les trois
-- rangs dans AscensionSpellProgressionData.h, puis redémarrer (spell_ranks se lit au démarrage).
-- Sans la chaîne, learnSpell(rang, false) pose le nouveau rang À CÔTÉ de l'ancien (deux boutons).
-- Mesuré le 2026-09-28 : 2 Stormbringers ont déjà >= 56 niveaux et apprendront Storm Alert rang 3
-- dès leur connexion. Chaînes 300857 (1-6), 706741 (1-6), 707044 (1-2) présentes en base.
-- =====================================================================================

DELETE FROM `spell_ranks` WHERE `spell_id` IN (300887, 707678, 707906);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 300857, 300887, 7 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 300857 AND `spell_id` = 300886 AND `rank` = 6);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 706741, 707678, 7 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 706741 AND `spell_id` = 707355 AND `rank` = 6);

INSERT INTO `spell_ranks` (`first_spell_id`, `spell_id`, `rank`)
SELECT 707044, 707906, 3 FROM DUAL
WHERE EXISTS (SELECT 1 FROM `spell_ranks` WHERE `first_spell_id` = 707044 AND `spell_id` = 707905 AND `rank` = 2);

-- Contrôle attendu après application : 3 lignes si les trois chaînes existent comme mesuré.
-- SELECT * FROM spell_ranks WHERE spell_id IN (300887, 707678, 707906);
