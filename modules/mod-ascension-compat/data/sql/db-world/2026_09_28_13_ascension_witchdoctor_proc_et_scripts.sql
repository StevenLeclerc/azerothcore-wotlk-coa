-- =====================================================================================
-- mod-ascension-compat — Witch Doctor : chance d'Umbral Glaive Specialization et
-- script de Lesser Shadow Avatar.
--
-- 1. Umbral Glaive Specialization (681242). Le Check de aura_ascension_witch_doctor_event
--    (AscensionWitchDoctorEvents.cpp, case UmbralTalent) tire lui-même ProcChance (10)
--    + critique à distance. Avec spell_proc.Chance = 0, SpellMgr remplace ce 0 par la
--    ProcChance du DBC et le cœur retire 10 % APRÈS le Check : la chance réelle tombait
--    à (10 + crit) % x 10 %. rev_20260909_00_witch_doctor_completion.sql posait 100 ;
--    rev_20260919_20_coa_proc_chance_parity.sql (liste engendrée par
--    Tools/Generate-CoAProcChances.py, absent de la machine au 2026-09-28) l'a remis à 0.
--    LA LIGNE DOIT RESTER À 100 : si ce générateur est retrouvé et relancé, en retirer 681242.
--    Aucun double compte : le cœur applique SPELLMOD_CHANCE_OF_SUCCESS à 100 et un tirage
--    à 100 ou plus passe toujours.
--
-- 2. Lesser Shadow Avatar (706544, via Residual Energy 706543). Son effet 1 déclenche
--    706803 chaque seconde, un effet 192 que le cœur traite par EffectNULL : la réduction
--    de recharge de Spirit Glaive promise n'avait jamais lieu. Le Tick de
--    aura_ascension_witch_doctor_lifecycle la fait désormais, à condition que le sort
--    porte ce script.
--
-- Idempotent. À chaud (Updates.EnableDatabases = 0) : appliquer à la main, puis
-- « reload spell_proc » par RA. spell_script_names se relit au démarrage seulement.
-- =====================================================================================
UPDATE `spell_proc` SET `Chance` = 100 WHERE `SpellId` = 681242;

DELETE FROM `spell_script_names` WHERE `spell_id` = 706544 AND `ScriptName` = 'aura_ascension_witch_doctor_lifecycle';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(706544, 'aura_ascension_witch_doctor_lifecycle');
