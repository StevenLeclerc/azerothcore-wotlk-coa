-- Runemaster (classe 32, famille 38) : procs morts, sceaux, et scripts de la vague 5 (29/09/2026).
--
-- A APPLIQUER APRES LE BINAIRE qui porte AscensionRunemasterSecondary.cpp de la meme vague :
--   * spell_script_names n'est lu qu'au demarrage ;
--   * -805380 passe a Charges 1 : avec l'ancien binaire (qui fait encore GetAura()->Remove()),
--     rien ne change ; avec le nouveau binaire SANS cette ligne, le sceau Arcane ne serait plus
--     jamais consomme et durerait ses 6 s. Binaire et SQL partent ensemble.
--
-- Colonnes spell_proc : SpellId, SchoolMask, SpellFamilyName, SpellFamilyMask0/1/2, ProcFlags,
-- SpellTypeMask, SpellPhaseMask, HitMask, AttributesMask, DisableEffectsMask, ProcsPerMinute,
-- Chance, Cooldown, Charges. Chance 0 = le ProcChance du DBC (SpellMgr.cpp, « take defaults
-- from dbcs »), ce qui laisse Aura::CalcProcChance appliquer SPELLMOD_CHANCE_OF_SUCCESS.
-- ProcFlags : 0x4 DONE_MELEE_AUTO_ATTACK, 0x10 DONE_SPELL_MELEE, 0x100 DONE_SPELL_RANGED,
-- 0x10000 DONE_SPELL_MAGIC_NEG ; 69972 = 332116 (ligne -805380) moins DONE_PERIODIC 0x40000.
--
-- Talents pilotes en C++ (hook de lancement, PAS de ligne ici, pour ne rien poser deux fois) :
-- 520237, 520755, 560037, 705568, 705618, 706523, 707876, 500476, 500250, 520138, 705560,
-- 705579, 705634, 706531, 706823, 707423, 707460, 705586, 806737, 806698.

DELETE FROM `spell_proc` WHERE `SpellId` IN (300581, 300582, 300944, 705550, 705572, 712308, -807834,
    805381, 805382, 807013, 500639, 804061, 805743, 805796, 707653, 520917, 560053, 653228, 705569,
    707730, 800756);

INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`,
    `SpellFamilyMask1`, `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`,
    `HitMask`, `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`,
    `Cooldown`, `Charges`) VALUES
-- Leystone Springs : attaques auto -> 520866 (mana), 15 % DBC. e1 (107) n'est pas un proc.
(300581, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 2, 0, 0, 0, 0),
-- Magic Etchings : degats de Primordial Blast (mot0 0x400000) -> 520555 armure -4 %.
(300582, 0, 38, 4194304, 0, 0, 65536, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Runic Power : e1 seul (e0 = aura de zone de groupe), Primordial Blast -> 561056.
(300944, 0, 38, 4194304, 0, 0, 65536, 1, 2, 0, 0, 1, 0, 0, 0, 0),
-- Runeslinger : degats directs (hors DoT) -> 520760 hate, 5 % DBC.
(705550, 0, 0, 0, 0, 0, 65808, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Primordial Echoes : critique de Primordial Blast, 40 % DBC ; le script filtre la chaine 800732
-- (le masque couvre aussi Stratus/Ignis/Lithos/Hydros/Runic Obliteration) et relance le rang.
(705572, 0, 38, 4194304, 0, 0, 65536, 1, 2, 2, 0, 0, 0, 0, 0, 0),
-- Leyfrost : critique de Hoarfrost (mot0 0x800000, classe melee) -> 712464. Les autres porteurs
-- du bit (Wild Steam, racines) sont declenches : refuses sans AttributesMask 2.
(712308, 0, 38, 8388608, 0, 0, 16, 1, 2, 2, 0, 0, 0, 0, 0, 0),
-- Runic Tattoos: Frost, 5 rangs : attaques subies (melee, distance, auto et sort) -> 707731.
-- e0 (aura 22, resistance) neutralise.
(-807834, 0, 0, 0, 0, 0, 680, 1, 2, 0, 0, 1, 0, 0, 0, 0),
-- Palm Sigil: Frost : comme -805380, e1 (aura 42 -> 808022) neutralise : 807825 pose deja
-- 808022 par son effet 1. Script aura_ascension_frost_palm_sigil.
(805381, 126, 0, 0, 0, 0, 332116, 1, 2, 3, 2, 2, 0, 100, 0, 1),
-- Palm Sigil: Earth / Wind : aura 42 native, degats magiques DIRECTS (sans 0x40000).
(805382, 126, 0, 0, 0, 0, 69972, 1, 2, 3, 2, 0, 0, 100, 0, 1),
(807013, 126, 0, 0, 0, 0, 69972, 1, 2, 3, 2, 0, 0, 100, 0, 1),
-- Palm Sigil: Water : comme -805380, un seul effet. Script aura_ascension_runemaster_water_palm_sigil.
(500639, 126, 0, 0, 0, 0, 332116, 1, 2, 3, 2, 0, 0, 100, 0, 1),
-- Magebreaker : coup de Hurricane 645437 (mot2 4, seul porteur ; declenche -> AttributesMask 2).
(804061, 0, 38, 0, 0, 4, 16, 1, 2, 0, 2, 0, 0, 0, 0, 0),
-- Steam Conjurer : degats de 653272 (mot1 0x20000, seul porteur ; declenche), 5 % DBC, 3 s.
(805743, 0, 38, 0, 131072, 0, 65552, 1, 2, 0, 2, 0, 0, 0, 3000, 0),
-- Fists of Power 805796 : attaques auto -> 806982 Earthen Fists.
(805796, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0),
-- Nomad's Scroll : REMPLACE l'entree fabriquee depuis le DBC (ProcFlags 4 : chaque attaque auto
-- dissipait un ralentissement). Lancement de Runic Tattoos: Air 802630 (mot2 0x20, seul porteur).
(707653, 0, 38, 0, 0, 32, 87040, 7, 1, 0, 0, 0, 0, 0, 0, 0),
-- Stone Savant : critique de 653272 -> 520934 (attaque supplementaire), sous Earthen Fists
-- (condition ci-dessous).
(520917, 0, 38, 0, 131072, 0, 65552, 1, 2, 2, 2, 0, 0, 0, 0, 0),
-- Sky and Stone : degats des charges utiles Air (mot1 0x40000) et Earth (mot1 0x20000), 10 % DBC.
(560053, 0, 38, 0, 393216, 0, 65552, 1, 2, 0, 2, 0, 0, 0, 0, 0),
-- 653228 (Sky and Stone, hate) : « your next 10 attacks » -> 10 charges sur les attaques auto.
(653228, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 10),
-- Windsage 705569 : 3 prochaines Runeblade (mot2 0x40000), e1 (spellmod) neutralise.
-- Script aura_ascension_runemaster_windsage.
(705569, 0, 38, 0, 0, 262144, 16, 1, 2, 0, 0, 2, 0, 0, 0, 3),
-- Earthen Codex 707730 : « next instance of direct Magic Damage » -> une charge.
(707730, 126, 0, 0, 0, 0, 69972, 1, 2, 0, 2, 0, 0, 0, 0, 1),
-- Protective Warding : critique subi -> 800753 (script cooldown_percent : -10 % sur 500464).
(800756, 0, 0, 0, 0, 0, 139944, 1, 2, 2, 0, 0, 0, 0, 0, 0);

-- Palm Sigil: Arcane : consommation par charge (Elements of Fury 707413 donne une seconde charge).
UPDATE `spell_proc` SET `Charges` = 1 WHERE `SpellId` = -805380;

-- Stone Savant : « While Earthen Fists is active ». 24 = CONDITION_SOURCE_TYPE_SPELL_PROC,
-- 1 = CONDITION_AURA (valeur 1 = sort, valeur 2 = effet), cible 0 = l'acteur du proc.
DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 24 AND `SourceEntry` = 520917;
INSERT INTO `conditions` (`SourceTypeOrReferenceId`, `SourceGroup`, `SourceEntry`, `SourceId`,
    `ElseGroup`, `ConditionTypeOrReference`, `ConditionTarget`, `ConditionValue1`,
    `ConditionValue2`, `ConditionValue3`, `NegativeCondition`, `ErrorType`, `ErrorTextId`,
    `ScriptName`, `Comment`) VALUES
(24, 0, 520917, 0, 0, 1, 0, 806982, 0, 0, 0, 0, 0, '', 'Runemaster Stone Savant - seulement sous Earthen Fists');

DELETE FROM `spell_script_names` WHERE `ScriptName` IN ('aura_ascension_frost_palm_sigil',
    'aura_ascension_runemaster_water_palm_sigil', 'aura_ascension_runemaster_windsage',
    'aura_ascension_runemaster_primordial_echoes', 'spell_ascension_runemaster_blade_rift',
    'spell_ascension_runemaster_runic_explosion', 'spell_ascension_runemaster_cooldown_percent');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(805381, 'aura_ascension_frost_palm_sigil'),
(500639, 'aura_ascension_runemaster_water_palm_sigil'),
(705569, 'aura_ascension_runemaster_windsage'),
(705572, 'aura_ascension_runemaster_primordial_echoes'),
(500287, 'spell_ascension_runemaster_blade_rift'),
(712324, 'spell_ascension_runemaster_runic_explosion'),
(807980, 'spell_ascension_runemaster_cooldown_percent'),
(807629, 'spell_ascension_runemaster_cooldown_percent'),
(807979, 'spell_ascension_runemaster_cooldown_percent'),
(800753, 'spell_ascension_runemaster_cooldown_percent');
