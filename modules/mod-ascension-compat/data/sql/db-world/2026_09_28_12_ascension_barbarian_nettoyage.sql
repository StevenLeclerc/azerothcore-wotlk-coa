-- =====================================================================================
-- mod-ascension-compat — Barbarian (classe 12, famille de sorts 18), nettoyage du 28/09.
-- Donnée seule, compagne du C++ de AscensionBarbarian*.cpp du même jour.
-- À appliquer à la main (Updates.EnableDatabases = 0), AVANT le redémarrage qui charge
-- le binaire portant les scripts : spell_script_names se lit au démarrage.
-- Idempotent : chaque ligne est supprimée avant d'être réinsérée.
-- =====================================================================================

-- 804141 « Berserker » : le hasard était tiré deux fois (cœur à 20 % puis code à 20 %).
-- Le cœur laisse passer chaque technique à distance ; le seul tirage reste celui du code,
-- avec SPELLMOD_CHANCE_OF_SUCCESS (572531), et 572531 se cumule à chaque technique.
UPDATE `spell_proc` SET `Chance` = 100 WHERE `SpellId` = 804141;

-- 803814 « Carnage » : ProcFlags 0 au DBC, aucun proc n'existait. KILL (2), exige
-- l'expérience ou l'honneur (AttributesMask 1), effet 0 (aura 72) écarté. Le script
-- aura_ascension_barbarian_event (Check/Proc, cas 803814) lance Born in Blood 560521.
DELETE FROM `spell_proc` WHERE `SpellId` = 803814;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES (803814, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1, 1, 0, 100, 0, 0);
DELETE FROM `spell_script_names` WHERE `spell_id` = 803814 AND `ScriptName` = 'aura_ascension_barbarian_event';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (803814, 'aura_ascension_barbarian_event');

-- 560530 « Clanlord's Totem » (aura posée par 804737 sur le raid) : aura 231 vers 560529
-- sans ProcFlags ni ligne spell_proc. Dégâts directs faits (69972), sorts déclenchés
-- EXCLUS (AttributesMask 0) pour que 560529 ne relance pas le proc ; effet 1 écarté.
-- Le montant (PA du Barbare x 0,35) est posé par aura_ascension_barbarian_clanlord ;
-- 560529 passe par spell_ascension_barbarian_conversion (valeur transmise telle quelle).
DELETE FROM `spell_proc` WHERE `SpellId` = 560530;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES (560530, 0, 0, 0, 0, 0, 69972, 1, 2, 0, 0, 2, 0, 100, 0, 0);
DELETE FROM `spell_script_names` WHERE `spell_id` IN (560530, 560529);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
    (560530, 'aura_ascension_barbarian_clanlord'),
    (560529, 'spell_ascension_barbarian_conversion');

-- 560521 « Born in Blood » : la description promet 4 x (5 + PA x 0,05) ; sans ligne,
-- un soin de DmgClass 1 n'ajoute aucune puissance d'attaque (Unit::SpellHealingBonusDone).
DELETE FROM `spell_bonus_data` WHERE `entry` = 560521;
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`)
VALUES (560521, 0, 0, 0, 0.05, 'Local Barbarian: Born in Blood');

-- 804748 « Ancestral Keg » : son effet 50 invoque l'objet 9000118, absent de la base ;
-- EffectTransmitted s'arrêtait en silence. Objet SPELLCASTER (22) sur le modèle du
-- Brewfest Pony Keg 186738 : Data0 = 570762 « Ancestral Keg » (seul candidat du DBC :
-- aura 23 toutes les 2 s vers 572066, 5 % de vie et de mana, rompue au mouvement),
-- Data1 = -1 (utilisations illimitées), Data2 = 1 (groupe du propriétaire seulement).
DELETE FROM `gameobject_template` WHERE `entry` = 9000118;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`,
    `size`, `Data0`, `Data1`, `Data2`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES (9000118, 22, 7549, 'Ancestral Keg', '', '', '', 0.5, 570762, -1, 1, '', '', 12340);
