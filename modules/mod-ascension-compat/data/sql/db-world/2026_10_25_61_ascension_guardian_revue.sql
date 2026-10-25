-- Guardian (CLASS_GUARDIAN 18, famille 24) : revue du 24/10/2026 (docs/REVUE-classe-guardian-20261024.md),
-- correctifs du 25/10/2026, sans l'equilibrage (G-04 et G-36 non touches : le fichier
-- 2026_09_28_23_ascension_guardian_coefficients.sql reste inerte).
-- Idempotent. A appliquer a la main sur acore_world (Updates.EnableDatabases = 0), AVANT ou AVEC le
-- binaire qui porte spell_ascension_guardian_cooldown_chain (AscensionGuardianAbilities.cpp) :
-- sans le binaire, la ligne spell_script_names est signalee au demarrage et reste sans effet ;
-- sans cette migration, le binaire garde l'ancien comportement de ces sorts.
-- Prise d'effet : demarrage, ou `.reload spell_script_names`, `.reload spell_proc`, `.reload spell_bonus_data`.

-- ---------------------------------------------------------------------------------------------
-- G-01 / G-02 : effets 165 / 195 sur toute la chaine de rangs (spell_ranks), et non le seul rang 1.
--   806544 No Escape            : -10 s a Battle Rush (802197, 501546-501548), depuis Ram sur cible enracinee
--   803127 Wreck Formation       : remise a zero de Hammer of the Law (704418, 707710-707715), effet 0
--   meme defaut, releve a la relecture (lignes PREVENTIVES : au 25/10 aucun de ces 4 sorts n'est lance
--   par quoi que ce soit, ni TriggerSpell, ni code, ni spell_proc ; 802893 n'est cite que par l'effet DUMMY de 804089) :
--   520638 Battlemaster: Axes    : -2,5 s a Press the Attack 801219 (hors chaine), Battle Rush, Advance (500170)
--   802893 Death Ram Reset       : -10 s a Ram (802284, 573204-573211)
--   705363 Iron Guardian (Proc)  : -3 s a Ram
--   712369 Colossal Reprise (CDR): -1 s a Broad Sweep (805150, 504177-504182)
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_ascension_guardian_cooldown_chain';
INSERT INTO `spell_script_names` (`spell_id`,`ScriptName`) VALUES
(806544,'spell_ascension_guardian_cooldown_chain'),
(803127,'spell_ascension_guardian_cooldown_chain'),
(520638,'spell_ascension_guardian_cooldown_chain'),
(802893,'spell_ascension_guardian_cooldown_chain'),
(705363,'spell_ascension_guardian_cooldown_chain'),
(712369,'spell_ascension_guardian_cooldown_chain');

-- ---------------------------------------------------------------------------------------------
-- G-08 Vanguard's Shield 804384 : « Whenever you are hit by a spell, you lose 1 stack of this effect. »
-- procFlags 0 dans Spell.dbc et aucune ligne : la pile ne baissait jamais (-20 % de degats magiques
-- permanents a 10 piles). « Sort » lu comme sort negatif de classe de degats magique
-- (PROC_FLAG_TAKEN_SPELL_MAGIC_DMG_CLASS_NEG 0x20000) ; les ticks periodiques n'en sont pas
-- (PROC_FLAG_TAKEN_PERIODIC). SpellTypeMask 7 : degats, soins, autres (un sort de controle magique compte).
-- SpellPhaseMask 2 (impact). AttributesMask 0x12 = PROC_ATTR_TRIGGERED_CAN_PROC (un sort declenche est
-- aussi un sort subi) | PROC_ATTR_USE_STACKS_FOR_CHARGES (Aura::ConsumeProcCharges -> ModStackAmount(-1)).
-- 805740 « Vanguard's Shield Decrement » (effet 175 sur 804384, MiscValue -1) est le sort prevu par
-- Ascension pour retirer la pile, mais rien ne le lance : la ligne spell_proc donne le meme resultat sans lui.
DELETE FROM `spell_proc` WHERE `SpellId` = 804384;
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,
 `SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,
 `DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
(804384,0,0,0,0,0,131072,7,2,0,18,0,0,100,0,0);

-- ---------------------------------------------------------------------------------------------
-- G-26 Heavy Blow rang 1 (803129) : l'infobulle dit « ${$m1+0+$AP*0.37} », sans puissance des sorts.
-- direct_bonus 0.75 (valeur native gardee par rev_20260908_01_guardian_coefficients.sql) n'existe
-- sur aucun des rangs 2 a 9 (0). Aligne le rang 1. 573212 (Vanguard, 0.33) n'a pas d'infobulle : inchange.
-- ATTENTION : /opt/coa/core-main/data/sql/updates/pending_db_world/rev_20260908_01_guardian_coefficients.sql
-- (ligne 9) reinsere (803129, 0.75, ...) : s'il est rejoue (reconstruction de base), rejouer CE fichier apres.
UPDATE `spell_bonus_data` SET `direct_bonus` = 0 WHERE `entry` = 803129;
