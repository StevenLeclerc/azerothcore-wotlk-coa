-- =====================================================================================
-- mod-ascension-compat — Runemaster : accroche des deux SpellScript de
-- src/AscensionRunemasterRunes.cpp (talents 705563 Shroudwalker et 705580 Runic Omen).
--
-- Le nom de ce fichier est la cle d'update enregistree dans `updates` (state = MODULE) :
-- NE PLUS LE RENOMMER une fois applique.
--
-- P-051 : `RegisterSpellScript` ne dit JAMAIS sur quels sorts un script s'applique.
-- Sans les lignes ci-dessous, les deux scripts sont compiles, enregistres, et ne tournent
-- jamais — et le coeur ne le signale que par une ligne « Script named '...' is not
-- assigned in the database. » dans Errors.log au demarrage.
--
-- -------------------------------------------------------------------------------------
-- COUPLAGE, A SENS UNIQUE : POSER CE SQL **APRES** LE BINAIRE
--
-- Ces deux noms de script n'existent pas dans le worldserver en service au 2026-09-20.
-- Appliquer ce fichier avant la mise en service du binaire qui porte
-- AscensionRunemasterRunes.cpp produit, au demarrage suivant, deux erreurs
-- « Spell script named '...' does not exist » et rien d'autre. Aucun risque de
-- comportement faux, seulement du bruit.
--
-- PRISE D'EFFET : REDEMARRAGE OBLIGATOIRE. `ObjectMgr::LoadSpellScriptNames()` n'a qu'un
-- seul appelant (World.cpp), il n'existe aucune commande `.reload spell_script_names`,
-- et l'accrochage lui-meme ne tourne qu'une fois au demarrage.
--
-- -------------------------------------------------------------------------------------
-- LES spell_id, LUS ET NON DEDUITS
--
-- 500287 « Warpdagger » : SPELL_WARPDAGGER dans src/AscensionRunemasterSecondary.cpp:28
--   et src/AscensionRunemasterTravel.cpp:24. Spell.dbc : famille 38, categorie 130
--   (seul sort de cette categorie), RecoveryTime = CategoryRecoveryTime = 30000.
--   Le sort porte deja `spell_ascension_runemaster_travel` ; `spell_script_names` est un
--   multimap (cle UNIQUE (spell_id, ScriptName)), deux scripts sur un sort sont normaux.
--
-- 707141 et ses dix rangs : chaine declaree dans `spell_ranks`
--   (707141, 707143..707148, 573444..573447 — relevee en base, pas supposee). Les onze
--   ids portent deja `spell_ascension_runemaster_brand_runeblade`, un rang par ligne :
--   on reprend la meme convention plutot que la forme « tous rangs » (spell_id negatif),
--   pour que la liaison reste lisible ligne a ligne et identique a celle du voisin.
--   Ce sont exactement les onze sorts que le masque de classe de 705596 « Runic Omen /
--   Proc » (0, 0, 262144) selectionne dans Spell.dbc, et aucun autre.
--
-- Ce fichier ne touche NI `spell_proc`, NI `spell_linked_spell`, NI `spell_dbc` : les deux
-- scripts sont des SpellScript accroches a AfterCast, pas des auras de proc. En
-- particulier, aucune ligne `spell_proc` n'est ajoutee pour 705580 : son effet 0 est une
-- aura 42 PROC_TRIGGER_SPELL avec ProcFlags = 0 et sans ligne `spell_proc` (verifie en
-- base), donc elle ne peut pas se declencher — et lui en donner une ferait DOUBLE emploi
-- avec le script C++.
-- =====================================================================================

-- Le DELETE porte sur ABS(spell_id) : une ligne au spell_id negatif laissee par une
-- version anterieure designerait le meme sort (forme « tous rangs » de
-- ObjectMgr::LoadSpellScriptNames) et accrocherait le script une SECONDE fois.
DELETE FROM `spell_script_names`
    WHERE `ScriptName` IN ('spell_ascension_runemaster_shroudwalker',
                           'spell_ascension_runemaster_runic_omen');

-- 705563 Shroudwalker : « Your Warpdagger now incurs no cooldown if used while your
-- Runeshroud is active. » Le script retire la recharge de Warpdagger apres le lancement.
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(500287, 'spell_ascension_runemaster_shroudwalker');

-- 705580 Runic Omen : « Every 3rd cast of Runeblade now deals $705596s1% increased
-- damage. » Le script compte les lancements sur 520285 et pose 705596 a 3 charges.
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(707141, 'spell_ascension_runemaster_runic_omen'), -- Rank 1
(707143, 'spell_ascension_runemaster_runic_omen'), -- Rank 2
(707144, 'spell_ascension_runemaster_runic_omen'), -- Rank 3
(707145, 'spell_ascension_runemaster_runic_omen'), -- Rank 4
(707146, 'spell_ascension_runemaster_runic_omen'), -- Rank 5
(707147, 'spell_ascension_runemaster_runic_omen'), -- Rank 6
(707148, 'spell_ascension_runemaster_runic_omen'), -- Rank 7
(573444, 'spell_ascension_runemaster_runic_omen'), -- Rank 8
(573445, 'spell_ascension_runemaster_runic_omen'), -- Rank 9
(573446, 'spell_ascension_runemaster_runic_omen'), -- Rank 10
(573447, 'spell_ascension_runemaster_runic_omen'); -- Rank 11

-- 705583 Runic Breakout n'a AUCUNE ligne ici, et c'est voulu : son declencheur est la
-- disparition de l'aure Runeshroud (500288), pas un lancement. Il est implemente par un
-- UnitScript (`runemaster_runes_auras`, UNITHOOK_ON_AURA_REMOVE), enregistre en C++ seul,
-- comme tous les UnitScript du module.
