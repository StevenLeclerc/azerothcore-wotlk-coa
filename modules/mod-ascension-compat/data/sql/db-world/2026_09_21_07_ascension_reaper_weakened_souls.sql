-- =====================================================================================
-- mod-ascension-compat — Reaper (classe 30, famille de sorts 36)
-- « Weakened Souls » 92146 : câblage du talent, retenu depuis le 2026-09-20.
--
-- CE QUI DÉBLOQUE LA LIVRAISON : la durée de « Weakened Soul » 803433 était INFINIE
-- (DurationIndex 21, et la ligne 21 de SpellDuration.dbc vaut (-1, 0, -1)). Aucune source
-- n'en écrit une — ni l'infobulle de 92146, ni celle de 803433, ni le DBC client, ni la
-- base monde amont `coa-world-20260912.zip`, ni `baseline.json`, ni les 4 436 entrées du
-- tracker amont ; les archives Wayback de db.ascension.gg étaient hors ligne au contrôle
-- du 2026-09-21. Le propriétaire du royaume a donc TRANCHÉ : **15 secondes**.
-- C'est une décision de conception, assumée comme telle, et elle est posée en C++
-- (`AscensionReaperEvents.cpp`, WEAKENED_SOUL_DURATION) plutôt que par une ligne
-- `spell_dbc`, qui exigerait une ligne complète de 234 colonnes lue au seul démarrage.
--
-- CE QUI RESTE UN ÉCART, ACCEPTÉ ET NON CORRIGÉ : le debuff n'est PAS restreint à Ombre
-- et Givre, et cette restriction n'est PAS EXPRIMABLE pour l'aura 271
-- SPELL_AURA_MOD_DAMAGE_FROM_CASTER. Ses deux seuls consommateurs (Unit.cpp:9365-9370 et
-- 10869-10872) filtrent sur le GUID du lanceur et sur `IsAffectedOnSpell`, et ne lisent
-- JAMAIS le MiscValue — le 48 (Ombre+Givre) de 803433 est de la donnée morte pour ce type
-- d'aura. Mesure sur Spell.dbc : la famille 36 compte 1 018 sorts, dont **529 ne touchent
-- ni l'Ombre ni le Givre** (487 d'entre eux physiques). Les +10 % les atteignent aussi.
-- Restreindre demanderait d'inventer un masque que l'infobulle n'écrit pas.
--
-- -------------------------------------------------------------------------------------
-- LA LIGNE spell_proc, vérifiée champ par champ le 2026-09-21
-- -------------------------------------------------------------------------------------
--   famille 36, masque (0, 8192, 0) : balayage de Spell.dbc, ce bit sélectionne
--     EXACTEMENT les 9 entrées nommées « Soulrend » (572341, 572342, 573316-573319,
--     573321, 573322, 802731) et rien d'autre. L'entrée 573320 « Soulrend / aura », qui
--     porte (0, 1024, 0), reste dehors — c'est voulu, ce n'est pas le coup porté.
--   ProcFlags 16 = PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS, SpellTypeMask 1 (dégâts),
--     SpellPhaseMask 2 (HIT) : « applies » au contact, sur chaque cible touchée.
--   DisableEffectsMask 2 : l'effet 1 de 92146 est une aura 354, aura privée Ascension
--     dont l'entrée d'AuraEffectHandler[] vaut nullptr (P-053). La réveiller n'apporterait
--     rien et brouillerait l'effet 0 (P-052). Le script s'accroche à EFFECT_0 /
--     SPELL_AURA_DUMMY, ce que le DBC donne bien à 92146 (effet 0 = APPLY_AURA, aura 4).
--   Chance 0 : repli sur le ProcChance du DBC, qui vaut 100. La ligne est nécessaire
--     malgré tout, le DBC portant ProcFlags 0 — sans elle, le cœur ne fabrique aucune
--     SpellProcEntry et l'aura ne proc jamais (P-045).
--
-- Prise d'effet : `spell_proc` se recharge à chaud (`.reload spell_proc`), mais
-- `spell_script_names` ne se lie qu'au DÉMARRAGE. Un redémarrage est nécessaire.
-- RETOUR ARRIÈRE : les deux DELETE seuls, puis redémarrage.
-- =====================================================================================

DELETE FROM `spell_proc` WHERE `SpellId` = 92146;

INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
     `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`,
     `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES
(92146, 0, 36, 0, 8192, 0, 16, 1, 2, 0, 0, 2, 0, 0, 0, 0);

DELETE FROM `spell_script_names`
 WHERE `spell_id` = 92146 AND `ScriptName` = 'aura_ascension_weakened_souls';

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(92146, 'aura_ascension_weakened_souls');
