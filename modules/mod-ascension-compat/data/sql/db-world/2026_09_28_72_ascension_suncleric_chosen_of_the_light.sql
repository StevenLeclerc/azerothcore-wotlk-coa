-- mod-ascension-compat — Sun Cleric (classe 27, famille 33), nettoyage du 2026-09-28.
-- Chosen of the Light (800622) : son effet 1 (aura 42 -> 706442, soin de 10 % des PV max)
-- ne partait jamais : ProcFlags = 0 au DBC et aucune ligne spell_proc (SpellMgr.cpp:2249).
-- Le module le prend en charge (SunClericEvents, AscensionSunClericData.h) : le contrat
-- neutralise l'aura 42 native, le script d'evenement lance 706442 une seule fois par
-- lancer, au premier coup encaisse sous 50 % de vie (AscensionSunClericEvents.cpp).
-- Ligne spell_proc calquee sur 680646 (Scroll of Hope, meme mecanique).
-- spell_script_names n'est lu qu'au demarrage.
DELETE FROM `spell_script_names` WHERE `spell_id` = 800622 AND `ScriptName` = 'aura_ascension_sun_cleric_event';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (800622, 'aura_ascension_sun_cleric_event');

DELETE FROM `spell_proc` WHERE `SpellId` = 800622;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES (800622, 0, 0, 0, 0, 0, 1048575, 7, 2, 32767, 2, 0, 0, 100, 0, 0);
