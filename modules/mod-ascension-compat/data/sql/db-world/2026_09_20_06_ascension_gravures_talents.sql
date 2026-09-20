-- =====================================================================================
-- mod-ascension-compat — les talents qui MODIFIENT les gravures d'arme.
--
-- Troisième passe du chantier des gravures, après 2026_09_20_04 (les procs) et
-- 2026_09_20_05 (les riders). Ces talents n'avaient jusqu'ici rien à modifier :
-- la mécanique qu'ils empoignent ne se déclenchait pas.
--
-- Accompagne `AscensionRunemasterEngravings.cpp`, `AscensionRunemasterHurricane.cpp`
-- et `AscensionRunemasterSecondary.cpp`.
--
-- ⚠ COMME LE FICHIER 05, CELUI-CI EXIGE LE BINAIRE QUI PORTE CE CODE, et pour une
--   raison plus mordante : la ligne `spell_proc` de Fire Engraving y passe à
--   Chance = 100. Appliquée SANS le binaire, Fire Engraving procerait à CHAQUE coup
--   direct au lieu de 30 % — un talent de classe cassé dans le sens fort.
--   L'ordre est : construire, installer, PUIS appliquer.
--   Retour arrière de cette seule ligne :
--     UPDATE spell_proc SET Chance = 0 WHERE SpellId = 653211; puis .reload spell_proc
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. EXPANSIVE ENGRAVER (807496), quart « Fire » : le tirage passe dans le script
--
-- « While Air Engraving is active, Fire Engraving's trigger chance is increased by $s1
-- percentage points. » ($s1 = effet 0 du talent = 30.)
--
-- Un modificateur de sort ne sait pas exprimer cette condition : il s'applique dès que
-- son aura est là, sans second test, alors que l'infobulle exige qu'une AUTRE aura
-- (Air Engraving) soit active. Et 807496 n'écrit aucun modificateur : ses trois effets
-- sont des DUMMY. Le seul endroit où la chance peut dépendre d'une autre aura est donc
-- le `DoCheckProc` du script. La ligne est mise à 100 pour que le cœur ne tire plus, et
-- `aura_ascension_runemaster_fire_engraving::Check` fait le tirage réel, en lisant la
-- chance de base dans le `ProcChance` du DBC (30) — de sorte que l'infobulle et le
-- code ne dépendent que d'un seul nombre.
--
-- Les quinze autres colonnes sont celles de la ligne d'origine, à l'identique :
-- ProcFlags 65876, SpellTypeMask 1, SpellPhaseMask 2, le reste à 0.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_proc` WHERE `SpellId` = 653211;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`,
    `SpellFamilyMask1`, `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`,
    `HitMask`, `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`,
    `Cooldown`, `Charges`) VALUES
(653211, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 0, 0, 100, 0, 0);

-- -------------------------------------------------------------------------------------
-- 2. EXPANSIVE ENGRAVER, quart « Earth »
--
-- « While Air Engraving is active, Earth Engraving deals 10% increased damage to the
-- first target hit, plus an additional 10% for each subsequent target. » Les deux 10 %
-- sont écrits en toutes lettres dans l'infobulle, ce ne sont pas des variables $s.
-- Le script s'attache à la charge utile en zone, 653272.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_script_names` WHERE `spell_id` = 653272
    AND `ScriptName` = 'spell_ascension_runemaster_earth_engraving';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(653272, 'spell_ascension_runemaster_earth_engraving');

-- -------------------------------------------------------------------------------------
-- 3. CE QUI N'A BESOIN D'AUCUNE LIGNE ICI, ET POURQUOI
--
-- * WATER RUNES (707150) est câblé dans `spell_ascension_hurricane_damage`, déjà
--   attaché à 645437 : pas de nouvelle ligne. Les deux auras 42 du talent lui-même
--   (vers 653261 et 653217, sur `ProcFlags = 4`, attaques blanches de mêlée) restent
--   sans ligne `spell_proc` — DÉLIBÉRÉMENT : l'infobulle parle des coups de *Hurricane*,
--   qui ne sont pas des attaques blanches, et une ligne de proc appliquerait Water ET
--   Ice là où elle dit « ou ». C'est la raison pour laquelle ce talent avait été écarté
--   de la passe SQL du 20/09 (fichier 03).
--
-- * CONVERGENCE (801086) est traité par le crochet `ALLSPELLHOOK_ON_CAST` de
--   `runemaster_engraving_momentum`, qui voit passer les six charges utiles de gravure.
--   Un `spell_proc` sur 801086 ne pourrait pas les reconnaître : « la prochaine gravure
--   déclenchée » n'est pas un événement de proc, et les six charges utiles ne partagent
--   aucun bit de `SpellFamilyFlags` qui les isolerait du reste de la famille 38.
--
-- * Les deux quarts restants d'EXPANSIVE ENGRAVER ne sont PAS implémentés, et c'est
--   assumé : « Ice » demande d'ajouter 30 points de chance de critique à deux sorts
--   contre les cibles portant Firebrand — le cœur n'expose aucun crochet de chance de
--   critique pour un sort déclenché ; « Air » dit « increased to 60% against targets
--   affected by your Icebound Momentum », or Icebound Momentum est un buff porté par
--   le Runemaster lui-même, pas par la cible : la phrase ne se laisse pas lire sans
--   deviner. Deviner ici, c'est inventer une règle de jeu.
