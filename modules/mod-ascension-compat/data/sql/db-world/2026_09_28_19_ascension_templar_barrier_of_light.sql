-- =====================================================================================
-- mod-ascension-compat — Templar (classe 19, famille 25) : Barrier of Light 805417.
-- Chaque tick (805417 e1, aura 23 -> 807698 FORCE_CAST -> 807694 REDIRECT_THREAT 100 %)
-- inscrit une redirection de menace dans le ThreatManager de l'allié, que le cœur ne
-- retire jamais. aura_templar_barrier_of_light (AscensionTemplarAuras.cpp) la retire
-- quand l'aura de zone quitte l'allié (fin de durée, sortie de portée, mort).
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_script_names n'est
-- lu qu'au démarrage : redémarrer le monde, avec le binaire qui porte le script.
-- =====================================================================================
DELETE FROM `spell_script_names` WHERE `spell_id` = 805417 AND `ScriptName` = 'aura_templar_barrier_of_light';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (805417, 'aura_templar_barrier_of_light');
