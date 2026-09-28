-- =====================================================================================
-- mod-ascension-compat — Necromancer (classe 23, famille de sorts 29) : nettoyage du
-- 2026-09-28. Moitié « donnée » du correctif de Transfer Life.
--
-- 801530 « Transfer Life » (ClassSpells@18, canalisé) pose sur TOUS les alliés à 20 yd
-- (TargetB 30 = TARGET_UNIT_SRC_AREA_ALLY) une aure 23 qui déclenche 801545 chaque seconde.
-- 801545 exige Raise Marker 807177 sur sa cible (TargetAuraSpell) : tout allié non marqué
-- (le Nécromancien lui-même compris) fait échouer le tic, et Spell::prepare coupe alors la
-- canalisation du joueur (SendChannelUpdate(0), Spell.cpp:3565-3569).
-- Le filtre « serviteurs relevés seulement » (spell_ascension_necromancer_ability::Filter,
-- AscensionNecromancerAbilities.cpp) ne s'accroche qu'aux sorts qui portent une ligne ici.
-- Le marqueur 807177 est posé sur les serviteurs relevés par AscensionNecromancerSummons.cpp.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_script_names est lu
-- au démarrage : redémarrer le monde.
-- =====================================================================================

DELETE FROM `spell_script_names` WHERE `spell_id` = 801530 AND `ScriptName` = 'spell_ascension_necromancer_ability';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(801530, 'spell_ascension_necromancer_ability');
