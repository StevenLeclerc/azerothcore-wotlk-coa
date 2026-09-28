-- mod-ascension-compat : Chronomancer (classe 22, famille 28), complements du nettoyage du 2026-09-28.
-- A appliquer APRES 2026_09_28_40_ascension_chronomancer_nettoyage.sql (meme lot de C++).
-- spell_proc et spell_bonus_data se rechargent a chaud ; spell_script_names n'est lu qu'au demarrage.
--
-- Note sur 2026_09_28_40 : le commentaire « 10 charges (contrat) » de la ligne spell_proc 520457
-- (Infinite Shield) est juste. AscensionClassMechanics19To25.cpp (INFINITE_SHIELD_CHARGES = 10) porte
-- le ProcCharges du DBC de 6 a 10 ; Charges 0 dans spell_proc garde cette valeur.

-- 1. spell_proc ------------------------------------------------------------------------------
-- Singularity Core : « Empower your Wand attacks ». Sans ligne, l'entree par defaut prenait la
-- famille 28 et le masque (0,0,0x10) des auras 108 : seul Shatter Echo declenchait 804437.
-- 0x40 tir de baguette | 0x100 sorts a distance, famille 0 : le tri (Wand of Time, Shatter Echo
-- sous Pulse Driver) est fait par aura_ascension_singularity_core. Effets 1-2 (spellmods) exclus.
DELETE FROM `spell_proc` WHERE `SpellId` = 804438;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(804438, 0, 0, 0, 0, 0, 320, 1, 2, 0, 0, 6, 0, 0, 0, 0);

-- 2. spell_script_names (lus au demarrage) -----------------------------------------------------
DELETE FROM `spell_script_names` WHERE `spell_id` IN (804438, 806204, -806335, 707555, 707566);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(804438, 'aura_ascension_singularity_core'),
-- Crystal Cannon (Specialization) : un canon par Discordance, sur la cible visee (la ligne
-- spell_proc 806204, en phase HIT, part une fois par cible de la chaine).
(806204, 'aura_ascension_crystal_cannon_spec'),
-- Melt Reality, 7 rangs par la chaine spell_ranks 806335 : la hate ajoute des ticks.
(-806335, 'aura_ascension_timerend'),
-- Slipstream : seconde activation (replacer l'ennemi a 20 m au plus).
(707555, 'aura_ascension_slipstream'),
(707566, 'spell_ascension_slipstream_place');

-- 3. spell_bonus_data -------------------------------------------------------------------------
-- Epoch : « $m1 + $SP*0.5 ». EffectBonusMultiplier 0 dans le DBC. Une ligne sur le rang 1 couvre
-- les rangs 2-8 par repli sur la chaine (SpellMgr::GetSpellBonusData).
-- Unmake : rang 1 « $SP*0.1 » par tick, rangs 2-11 « $SP*0.12 » (DBC 0 puis 0.0624) : une ligne
-- par rang, le rang 1 differant.
-- Infinite Shield, soin 520458 : « $SP*.25 » du talent 520457.
DELETE FROM `spell_bonus_data` WHERE `entry` IN (801270, 804418, 503784, 503785, 503786, 503787, 503788,
    503789, 503790, 504574, 574344, 574345, 520458);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(801270, 0.5, 0, 0, 0, 'Chronomancer - Epoch, $SP*0.5 (rangs 2-8 par repli spell_ranks)'),
(804418, 0, 0.1, 0, 0, 'Chronomancer - Unmake rang 1, $SP*0.1 par tick'),
(503784, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 2, $SP*0.12 par tick'),
(503785, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 3'),
(503786, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 4'),
(503787, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 5'),
(503788, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 6'),
(503789, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 7'),
(503790, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 8'),
(504574, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 9'),
(574344, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 10'),
(574345, 0, 0.12, 0, 0, 'Chronomancer - Unmake rang 11'),
(520458, 0.25, 0, 0, 0, 'Chronomancer - Infinite Shield soin, $SP*.25 du talent 520457');
