-- =====================================================================================
-- mod-ascension-compat — Guardian : Turn The Blade, Net Throw, Hold the Line.
--
-- 1. Turn The Blade (707170). HitMask 12287 (PROC_HIT_MASK_ALL) avec Charges = 1 : la
--    charge partait au premier coup recu, rate ou esquive compris, alors que le Proc de
--    aura_ascension_guardian_event ne lance le desarmement 707722 que sur une parade.
--    HitMask 32 = PROC_HIT_PARRY (SpellMgr.h) : la charge ne part plus que sur une parade.
--    Rectifie la valeur posee par rev_20260908_08_class_completion_runtime.sql.
--    SpellTypeMask 1 (DAMAGE) -> 7 (PROC_SPELL_TYPE_MASK_ALL) : une attaque speciale paree
--    ne fait aucun degat et porte PROC_SPELL_TYPE_NO_DMG_HEAL (Unit.cpp:7157-7161) ; le
--    filtre de SpellMgr.cpp:931-934 (ProcFlags 0x20 dans SPELL_PROC_FLAG_MASK) la rejetait,
--    donc une Frappe heroique paree ne desarmait jamais.
--
-- 2. Net Throw (802304 et sa copie de test 802309) : aura_ascension_guardian_net_throw
--    (AscensionGuardianEvents.cpp) garde l'aura 308 de l'effet 1 a 0 ; le critique garanti
--    de Centurion Strike (Clinch Brawler 707310) passe par guardian_echo::OnSpellCalculatedTarget.
--
-- 3. Hold the Line (803830) : aura_ascension_guardian_hold_the_line
--    (AscensionGuardianAbilities.cpp) ajoute les immunites incapacite (14) et assomme (30).
--
-- Idempotent. A chaud (Updates.EnableDatabases = 0) : appliquer a la main, puis
-- « reload spell_proc » par RA. spell_script_names se relit au demarrage seulement :
-- l'appliquer AVANT le redemarrage qui installe le binaire portant ces deux scripts.
-- =====================================================================================
UPDATE `spell_proc` SET `HitMask` = 32, `SpellTypeMask` = 7 WHERE `SpellId` = 707170;

DELETE FROM `spell_script_names` WHERE `spell_id` IN (802304, 802309) AND `ScriptName` = 'aura_ascension_guardian_net_throw';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(802304, 'aura_ascension_guardian_net_throw'),
(802309, 'aura_ascension_guardian_net_throw');

DELETE FROM `spell_script_names` WHERE `spell_id` = 803830 AND `ScriptName` = 'aura_ascension_guardian_hold_the_line';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(803830, 'aura_ascension_guardian_hold_the_line');
