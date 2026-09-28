-- mod-ascension-compat : Chronomancer (classe 22, famille de sorts 28), nettoyage du 2026-09-28.
--
-- Tout ce fichier est de la donnee que lit le C++ des fichiers AscensionChronomancer*.cpp
-- du meme lot. spell_proc et spell_target_position se rechargent a chaud ; spell_script_names,
-- spell_group et spell_group_stack_rules ne sont lus qu'au demarrage.
--
-- Rappels (SpellMgr.cpp:2109-2115) : une ligne spell_proc remplace le ProcFlags du DBC ;
-- Chance 0 garde le ProcChance du DBC ; Charges 0 garde le ProcCharges du SpellInfo (contrats
-- compris, charges avant LoadSpellProcs). Un id negatif couvre toute la chaine spell_ranks.
-- ProcFlags : 0x8 TAKEN_MELEE_AUTO, 0x20 TAKEN_SPELL_MELEE, 0x40 DONE_RANGED_AUTO,
-- 0x80 TAKEN_RANGED_AUTO, 0x100 DONE_SPELL_RANGED, 0x200 TAKEN_SPELL_RANGED, 0x400 DONE_NONE_POS,
-- 0x1000 DONE_NONE_NEG, 0x2000 TAKEN_NONE_NEG, 0x4000 DONE_MAGIC_POS, 0x10000 DONE_MAGIC_NEG,
-- 0x20000 TAKEN_MAGIC_NEG, 0x40000 DONE_PERIODIC, 0x100000 TAKEN_DAMAGE.
-- 0x15400 = DONE NONE_POS | NONE_NEG | MAGIC_POS | MAGIC_NEG, leves en phase CAST (1) par un
-- sort lance (Spell.cpp:4017-4029) ; les sorts de classe « ranged » (Wand of Time, Shatter Echo)
-- levent 0x100 a la place (Spell.cpp:2278-2289).
-- DisableEffectsMask ecarte les effets du talent qui ne sont pas l'aura de proc.

-- 1. spell_proc ------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN (680389, 504602, 555729, 572357, 706096, 807309,
    806337, 807578, 520173, 707656, 704488, 560310, 806216, 520171, 560129, 680946, 704487, 804451,
    804444, 520045, 560541, 706046, 520168, 807210, 583245, 802228, 520457, 680373, 706059, 707657,
    807569, 706099);
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
-- Temporal Resilience, 7 rangs (chaine 680389) : un ennemi qui frappe recoit Temporally Warped 536265.
(-680389, 0, 0, 0, 0, 0, 680, 1, 0, 0, 0, 1, 0, 0, 0, 0),
-- Warping Reality : Waves of Time (mask1 0x8) accorde Warped Reality 504603.
(504602, 0, 28, 0, 8, 0, 87040, 7, 1, 0, 0, 2, 0, 0, 0, 0),
-- Constant Recovery : soins de Reverse Wound (mask1 0x20000, directs et periodiques) -> Recovering.
(555729, 0, 28, 0, 131072, 0, 278528, 2, 2, 0, 0, 1, 0, 0, 0, 0),
-- Time Means Nothing : degats de Chromatic Shard (mask2 0x2000000) -> 572405.
(572357, 0, 28, 0, 0, 33554432, 69632, 1, 2, 0, 0, 5, 0, 0, 0, 0),
-- A Ripple In Time : tout critique, degats ou soins, y compris les sorts « ranged » -> 706783.
(706096, 0, 0, 0, 0, 0, 349440, 3, 2, 2, 0, 2, 0, 0, 0, 0),
-- Bronze Timeways : Correct the Mistake (mask1 0x800) -> 2 piles d'Endless Sands (deux effets).
(807309, 0, 28, 0, 2048, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Hourglass of Eternity : degats periodiques, 5 % (DBC) -> 504726.
(806337, 0, 0, 0, 0, 0, 262144, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Chaos Fusion : Incarnation of Chaos (mask2 0x2000) -> 807755 (duree alignee en C++).
(807578, 0, 28, 0, 0, 8192, 87040, 7, 1, 0, 0, 2, 0, 0, 0, 0),
-- Tripping the Rift : Clasp of Infinity (mask0 0x800000) -> 521214.
(520173, 0, 28, 8388608, 0, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Maker of Realities : Hasten (mask0 0x10000000) -> 712454.
(707656, 0, 28, 268435456, 0, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Mark of Order : soins de Fortify Timeline (mask1 0x400000) -> 806269.
(704488, 0, 28, 0, 4194304, 0, 16384, 2, 2, 0, 0, 0, 0, 0, 0, 0),
-- Through The Aeons : activer un Aeon (mask1 0x40, les 4 Aeons seuls) -> 560311. Remplace la
-- mise a mort (ProcFlags DBC 0x2) et l'attribut REQ_EXP_OR_HONOR de l'entree par defaut.
(560310, 0, 28, 0, 64, 0, 87040, 7, 1, 0, 0, 2, 0, 0, 0, 0),
-- Wand Mastery : Wand of Time (mask1 0x100000) et Shatter Echo (mask2 0x14) -> Crystal Cannon.
(806216, 0, 28, 0, 1048576, 20, 256, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Magic Sensitivity : critiques de Shatter Echo -> 520177.
(520171, 0, 28, 0, 0, 20, 256, 1, 2, 2, 0, 0, 0, 0, 0, 0),
-- Elder Wand : « Wand attacks » = tir de baguette (0x40) et sorts de baguette (0x100), 30 %.
(560129, 0, 0, 0, 0, 0, 320, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Paradox : Melt Reality (mask1 0x200) -> 680947 (1 charge par contrat).
(680946, 0, 28, 0, 512, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Gift of the Timeways : Melt Reality -> 503903.
(704487, 0, 28, 0, 512, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Time Skip : Roll Back (mask0 0x1) sur un allie -> 804470 (relance differee de Roll Back).
(804451, 0, 28, 1, 0, 0, 16384, 7, 2, 0, 0, 0, 0, 0, 0, 0),
-- Shield of the Ages : coup critique subi -> 804445.
(804444, 0, 0, 0, 0, 0, 139944, 1, 0, 2, 0, 0, 0, 0, 0, 0),
-- Converge the Infinite : Fortify Timeline (mask1 0x400000) -> 520840 (1 charge par contrat).
(520045, 0, 28, 0, 4194304, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Wonders of Time : tout soin, 10 % (DBC) -> Endless Sands.
(560541, 0, 0, 0, 0, 0, 278528, 2, 2, 0, 0, 3, 0, 0, 0, 0),
-- Magic Erosion : degats de Decomposition (mask1 0x80000) -> 570060.
(706046, 0, 28, 0, 524288, 0, 331776, 1, 2, 0, 0, 1, 0, 0, 0, 0),
-- Clocked In : le prochain Artificer's Wand (mask2 0x400) donne un Echo Fragment, une charge.
(520168, 0, 28, 0, 0, 1024, 65536, 7, 1, 0, 0, 5, 0, 0, 0, 1),
-- Ideal Time | Proc : la prochaine technique seulement (une charge), pas les periodiques ni les
-- attaques automatiques.
(807210, 0, 0, 0, 0, 0, 87312, 3, 2, 0, 0, 0, 0, 0, 0, 1),
-- Chaotic Time : degats periodiques -> 583258 (recharge d'Incarnation), effets 0-1 exclus.
(583245, 0, 0, 0, 0, 0, 262144, 1, 2, 0, 0, 3, 0, 0, 0, 0),
-- Time Out! (stase 802228) : degats directs subis ; le tirage est fait dans le script
-- aura_ascension_time_out_stasis sur le ProcChance du rang, d'ou Chance 100.
(802228, 0, 0, 0, 0, 0, 139944, 1, 0, 0, 0, 0, 0, 100, 0, 0),
-- Infinite Shield : tout degat subi, une fois par seconde ; 10 charges (contrat), soin en script.
(520457, 0, 0, 0, 0, 0, 1048576, 0, 0, 0, 0, 2, 0, 0, 1000, 0),
-- Borrowed Time : degats physiques subis, une fois par seconde ; soin en script.
(680373, 1, 0, 0, 0, 0, 680, 1, 0, 0, 0, 0, 0, 0, 1000, 0),
-- Shifting Chaos : Anomaly Spike (mask0 0x8000000, toujours declenche, d'ou AttributesMask
-- 2 = PROC_ATTR_TRIGGERED_CAN_PROC, SpellMgr.h:277) et Chromatic Shard.
(706059, 0, 28, 134217728, 0, 33554432, 65536, 1, 2, 0, 2, 0, 0, 0, 0, 0),
-- Overcorrection : soins de Correct the Mistake (mask1 0x800).
(707657, 0, 28, 0, 2048, 0, 16384, 2, 2, 0, 0, 0, 0, 0, 0, 0),
-- Pure Chaos : tout degat, 10 % (DBC), seulement pendant Incarnation (script).
(807569, 0, 0, 0, 0, 0, 332032, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- The Bieko Effect : Hasten (mask0 0x10000000) -> 712371 (reduction de recharge en script).
(706099, 0, 28, 268435456, 0, 0, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0);

-- 2. spell_script_names (lus au demarrage) -----------------------------------------------------
DELETE FROM `spell_script_names` WHERE `spell_id` IN (802228, 802229, 803896, 803897, 804490, 801277, 801291,
    501831, 501832, 501833, 501834, 501835, 572578, 712371, 680971, 500116, 706059, 806315, 680373,
    707657, 520457, 807569);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(802228, 'aura_ascension_time_out_stasis'),
(802229, 'aura_ascension_time_out'),
(803896, 'aura_ascension_time_out'),
(803897, 'aura_ascension_time_out'),
(804490, 'spell_ascension_roll_back'),
(801277, 'spell_ascension_waves_of_time'),
-- Timerend : 7 lignes sans chaine spell_ranks.
(801291, 'aura_ascension_timerend'),
(501831, 'aura_ascension_timerend'),
(501832, 'aura_ascension_timerend'),
(501833, 'aura_ascension_timerend'),
(501834, 'aura_ascension_timerend'),
(501835, 'aura_ascension_timerend'),
(572578, 'aura_ascension_timerend'),
(712371, 'aura_ascension_bieko_effect'),
(680971, 'aura_ascension_destabilize_time'),
(500116, 'aura_ascension_chronomancer_discovery'),
(706059, 'aura_ascension_shifting_chaos'),
(806315, 'aura_ascension_temporal_anomaly'),
(680373, 'aura_ascension_borrowed_time'),
(707657, 'aura_ascension_overcorrection'),
(520457, 'aura_ascension_infinite_shield'),
(807569, 'aura_ascension_pure_chaos');

-- 3. Teleport: Caverns of Time (effet 0 TELEPORT_UNITS, cible B TARGET_DEST_DB) : aucune
-- coordonnee. Position de game_tele 196 CavernsOfTime. Rechargeable : .reload spell_target_position
DELETE FROM `spell_target_position` WHERE `ID` = 804753;
INSERT INTO `spell_target_position` (`ID`, `EffectIndex`, `MapID`, `PositionX`, `PositionY`, `PositionZ`,
    `Orientation`, `VerifiedBuild`) VALUES
(804753, 0, 1, -8204.88, -4495.25, 9.0091, 4.72574, 0);

-- 4. « Can only have 1 Continuum spell active at a time » : Flux Emitter, Aether Compression,
-- Singularity Core, Paradox Cannon. Regle 1 EXCLUSIVE, comme les groupes 2116001-2116003.
DELETE FROM `spell_group` WHERE `id` = 2122001;
INSERT INTO `spell_group` (`id`, `spell_id`) VALUES
(2122001, 804435),
(2122001, 804436),
(2122001, 804438),
(2122001, 806203);
DELETE FROM `spell_group_stack_rules` WHERE `group_id` = 2122001;
INSERT INTO `spell_group_stack_rules` (`group_id`, `stack_rule`, `description`) VALUES
(2122001, 1, 'CoA Chronomancer Continuum');
