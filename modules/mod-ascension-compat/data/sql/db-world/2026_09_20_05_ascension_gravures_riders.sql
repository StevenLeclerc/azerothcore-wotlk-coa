-- =====================================================================================
-- mod-ascension-compat — WEAPON ENGRAVINGS, seconde passe : les riders.
--
-- Suite de 2026_09_20_04_ascension_gravures_arme.sql, qui a rendu leur proc à Ice,
-- Earth, Water et Arcane. Ce fichier accompagne `AscensionRunemasterEngravings.cpp`
-- et couvre ce qu'aucune table ne sait dire.
--
-- ⚠ CE FICHIER EXIGE LE BINAIRE QUI PORTE CES QUATRE SCRIPTS. Appliqué avant,
--   `spell_script_names` désigne des scripts inexistants et le serveur le signale au
--   démarrage (« ScriptName ... does not exist »). L'ordre est : construire,
--   installer, PUIS appliquer ce fichier.
--
-- Rappel P-051 : un script enregistré en C++ mais absent de `spell_script_names` ne
-- tourne jamais, et ne coûte qu'une ligne de log. Les quatre lignes ci-dessous sont
-- la moitié manquante du correctif, pas un accessoire.
--
-- Prise d'effet à chaud : `.reload spell_script_names`, `.reload spell_proc`,
-- `.reload spell_linked_spell` — mais les scripts eux-mêmes viennent du binaire.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. AIR — la seule gravure qui ne pouvait pas être réparée en donnée
--
-- 653223 (appliqué par l'enchantement 999) ne porte qu'un DUMMY vide et une aura 349
-- que le cœur NE SAIT PAS EXÉCUTER : `AuraEffectHandler[349]` est `nullptr`
-- (« unknown Ascension aura »). Les +10 % de vitesse que promet l'infobulle ne sont
-- donc PAS appliqués aujourd'hui, et ce fichier ne les répare pas non plus : rien ne
-- permet d'affirmer à quoi 349 correspond. Sur les quatre sorts du DBC qui la portent,
-- deux parlent de hâte et un de réduction de dégâts subis — c'est trop peu pour
-- réécrire l'aura en 192 MOD_MELEE_RANGED_HASTE, qui serait pourtant le candidat.
--
-- Ce que le lien ci-dessous apporte : 653225, qui porte (a) l'aura de proc 354, que
-- `aura_ascension_runemaster_air_engraving` sait enfin exécuter, et (b) une aura 216
-- HASTE_SPELLS de +10 %, elle parfaitement native. La part « vitesse d'incantation »
-- de l'infobulle devient donc vraie ; la part mêlée/distance reste morte, et c'est dit.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_linked_spell` WHERE `spell_trigger` = 653223 AND `spell_effect` = 653225;
INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
(653223, 653225, 2, 'Runemaster Air Engraving - applique le Passive2 porteur du proc');

-- DisableEffectsMask = 4 : coupe l'effet 2 (aura 216 HASTE_SPELLS). C'est le remède de
-- P-052 — une ligne `spell_proc` réveille TOUS les effets d'aura du sort, et seul
-- l'effet 0 doit procer. L'effet 2 reste appliqué comme aura passive, il est seulement
-- exclu du proc. Validation SpellMgr.cpp : l'effet désigné doit être une aura — il l'est.
-- Chance 0 -> ProcChance du DBC = 15 %, le chiffre de l'infobulle.
DELETE FROM `spell_proc` WHERE `SpellId` = 653225;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`,
    `SpellFamilyMask1`, `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`,
    `HitMask`, `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`,
    `Cooldown`, `Charges`) VALUES
(653225, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 4, 0, 0, 0, 0);

-- -------------------------------------------------------------------------------------
-- 2. LES QUATRE SCRIPTS
--
-- aura_ascension_runemaster_air_engraving   653225  réplique 30 % des dégâts (aura 354)
-- aura_ascension_runemaster_ice_engraving   653266  applique Icebound Momentum, 1 pile,
--                                                   fenêtre de 5 s (helper 712495).
--                                                   N'empêche PAS l'action par défaut :
--                                                   les dégâts de Givre restent natifs.
-- spell_ascension_runemaster_water_engraving 653261 la valeur du POWER_DRAIN est le
--                                                   POURCENTAGE de l'infobulle, pas un
--                                                   montant : sans script il draine 2
--                                                   points de mana au lieu de 2 % du
--                                                   maximum du lanceur.
-- aura_ascension_runemaster_arcane_mark      653263 paie 30 % du soin stocké quand la
--                                                   marque expire NATURELLEMENT.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_script_names` WHERE `spell_id` IN (653225, 653266, 653261, 653263)
    AND `ScriptName` IN ('aura_ascension_runemaster_air_engraving',
                         'aura_ascension_runemaster_ice_engraving',
                         'spell_ascension_runemaster_water_engraving',
                         'aura_ascension_runemaster_arcane_mark');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(653225, 'aura_ascension_runemaster_air_engraving'),
(653266, 'aura_ascension_runemaster_ice_engraving'),
(653261, 'spell_ascension_runemaster_water_engraving'),
(653263, 'aura_ascension_runemaster_arcane_mark');
