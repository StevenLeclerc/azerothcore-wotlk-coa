-- Witch Hunter - Trap Launcher (802272, ClassSpells level 40).
-- 802272 is a lone SPELL_AURA_DUMMY that nothing read: the traps were always laid at the caster's
-- feet. The DBC carries a launched twin for each trap (range index 4 = 30 yd, TARGET_DEST_DEST,
-- same summoned creature 506250-506253) that no script handled: 680376, 681562, 681563, 681564.
-- The lifecycle aura script now swaps each known ground trap for its twin while 802272 is up
-- (Player::SetTemporarySpellReplacement), and the summon script lays the twin at the target point.
-- Apply by hand (Updates.EnableDatabases = 0), before the world server start that loads this build.
DELETE FROM `spell_script_names` WHERE `spell_id` IN (680376, 681562, 681563, 681564)
    AND `ScriptName` = 'spell_ascension_witch_hunter_summon';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(680376, 'spell_ascension_witch_hunter_summon'),
(681562, 'spell_ascension_witch_hunter_summon'),
(681563, 'spell_ascension_witch_hunter_summon'),
(681564, 'spell_ascension_witch_hunter_summon');

DELETE FROM `spell_script_names` WHERE `spell_id` = 802272 AND `ScriptName` = 'aura_ascension_witch_hunter_lifecycle';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(802272, 'aura_ascension_witch_hunter_lifecycle');
