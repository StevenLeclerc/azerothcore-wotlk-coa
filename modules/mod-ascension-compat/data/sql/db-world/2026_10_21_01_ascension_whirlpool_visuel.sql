-- CoA (21/10/2026, R-20261021-1505, coa-project/docs/DIAG-bot-envol-kattailu-20261020.md) : repere visuel de la
-- projection de Whirlpool. Lie spell_ascension_whirlpool_airborne_visual (AscensionStormbringerZephyr.cpp) a 526365
-- (Whirlpool, KNOCK_BACK vz 15, declenche par l'aura periodique de 526362 ; seul sort du Spell.dbc qui le declenche).
-- Le script pose sur le personnage projete l'aura cosmetique 47335 (Cyclone Impact Visual (Blue), DUMMY seul,
-- cachee), 2 s par defaut. Option : AscensionCompat.WhirlpoolAirborneVisual / WhirlpoolAirborneVisualMs.
-- Verifie le 21/10 : aucune ligne spell_script_names, spell_proc, spell_linked_spell ni spell_dbc pour 526365 ni
-- 47335 dans acore_world et acore_world_armageddon.
-- PREREQUIS : un worldserver qui porte le script (strings worldserver | grep spell_ascension_whirlpool_airborne_visual),
-- sinon la ligne est signalee au demarrage et ne fait rien. A jouer SEULE par mysql (P-256), pas par
-- appliquer-sql-module.sh --appliquer.

DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_ascension_whirlpool_airborne_visual';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(526365, 'spell_ascension_whirlpool_airborne_visual');
