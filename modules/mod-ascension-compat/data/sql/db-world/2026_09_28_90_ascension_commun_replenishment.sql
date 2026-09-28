-- =====================================================================================
-- mod-ascension-compat — commun, nettoyage des classes du 2026-09-28.
--
-- Replenishment 1257670 : l'aura (24 PERIODIC_ENERGIZE, 5, MiscValueB 1, toutes les 5 s pendant
-- 15 s) promet « 5% of your maximum mana every $t1 sec », mais le cœur versait 5 points de mana
-- par tick. L'AuraScript aura_ascension_replenishment_pct (AscensionResourceTalents.cpp) convertit
-- le montant en pourcentage du mana maximal du porteur.
--
-- Donné par : Chronomancer (Reverse Engineering 520875), Sun Cleric, Witch Doctor, Witch Hunter,
-- Venomancer, Pyromancer, et les talents Pious, Foresight, Sun's Gift, Mojo Wave, Rot Lich,
-- Resources of the Earth, Ashen Priest, Brushin' Off The Dusk ; et la gravure d'eau du Runemaster
-- 653261 (2026_09_20_04_ascension_gravures_arme.sql).
--
-- CIBLES (revue du 2026-09-28) : 1257670 vise TOUT le raid (effet 0 = 6 APPLY_AURA, ImplicitA 56
-- TARGET_UNIT_CASTER_AREA_RAID, aura 24 ; MESURÉ dans Spell.dbc, identique à 57669 WotLK). Seul, le
-- pourcentage donnerait 15 % de mana par déclenchement à 40 joueurs, et une menace de 0,5 x le mana
-- rendu pour chacun. On lie donc AUSSI spell_gen_replenishment (spell_generic.cpp:4357, cœur, paire
-- SpellScript + AuraScript) : au plus 10 cibles, celles qui ont le moins de mana en %, les unités
-- sans mana écartées, le lanceur seul en arène. Son Validate exige 57669 et 61782 : présents dans
-- notre Spell.dbc (MESURÉ). Son AuraScript ne change le montant que pour 57669 et 61782 (default:
-- break) : il ne touche pas au pourcentage de aura_ascension_replenishment_pct.
--
-- ÉQUILIBRAGE À TRANCHER PAR LE COMMANDITAIRE AVANT APPLICATION : 5 % par tick (infobulle), soit
-- 1 %/s pendant 15 s, 5 fois le taux de WotLK (0,2 %/s). Ne rien appliquer = 5 points par tick.
--
-- spell_script_names n'est lu qu'au démarrage. Updates.EnableDatabases = 0 : à appliquer à la
-- main, AVANT de démarrer le worldserver qui porte le script. Sans cette ligne, le script est
-- compilé mais inerte (5 points par tick, comme avant). Idempotent.
-- =====================================================================================

DELETE FROM `spell_script_names` WHERE `spell_id` = 1257670
  AND `ScriptName` IN ('aura_ascension_replenishment_pct', 'spell_gen_replenishment');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(1257670, 'aura_ascension_replenishment_pct'),
(1257670, 'spell_gen_replenishment');

-- Contrôle attendu après application : 2 lignes.
-- SELECT * FROM spell_script_names WHERE spell_id = 1257670;
