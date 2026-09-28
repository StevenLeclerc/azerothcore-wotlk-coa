-- =====================================================================================
-- mod-ascension-compat — Stormbringer (classe 16, famille de sorts 22) : nettoyage du
-- 2026-09-28. Moitié « donnée » des correctifs de AscensionStormbringerTalents.cpp,
-- AscensionStormbringerEvents.cpp et AscensionStormbringerPet.cpp.
--
-- Rappels payés : P-045/P-053 (aura de proc à ProcFlags DBC nul et sans ligne spell_proc :
-- SpellMgr::LoadSpellProcs la saute), P-051 (script C++ sans ligne spell_script_names : ne
-- tourne jamais).
--
-- Conventions vérifiées dans le cœur (SpellMgr.cpp:2107-2115, 2127-2150) :
--   ProcFlags 0 -> repris du DBC ; Chance 0 -> ProcChance du DBC ; Charges 0 -> ProcCharges ;
--   SpellPhaseMask exigé seulement pour les drapeaux DONE (REQ_SPELL_PHASE_PROC_FLAG_MASK =
--   SPELL_PROC_FLAG_MASK & DONE_HIT_PROC_FLAG_MASK, SpellMgr.h:184) : les lignes à drapeaux
--   TAKEN seuls ont donc SpellPhaseMask 0, sinon LoadSpellProcs journalise une erreur.
--   Phase CAST (1) : une fois par lancement, cible nulle (le sort déclenché doit viser le
--   lanceur) ; phase HIT (2) : une fois par cible touchée.
-- Drapeaux (SpellMgr.h) : 0x4 DONE_MELEE_AUTO, 0x8 TAKEN_MELEE_AUTO, 0x10 DONE_SPELL_MELEE,
--   0x20 TAKEN_SPELL_MELEE, 0x40 DONE_RANGED_AUTO, 0x80 TAKEN_RANGED_AUTO, 0x100 DONE_SPELL_RANGED,
--   0x200 TAKEN_SPELL_RANGED, 0x400 DONE_NONE_POS, 0x1000 DONE_NONE_NEG, 0x2000 TAKEN_NONE_NEG,
--   0x4000 DONE_MAGIC_POS, 0x10000 DONE_MAGIC_NEG, 0x20000 TAKEN_MAGIC_NEG, 0x40000 DONE_PERIODIC,
--   0x80000 TAKEN_PERIODIC.
--   65876 = 0x4|0x10|0x40|0x100|0x10000 (tous les dégâts directs faits) ;
--   87040 = 0x400|0x1000|0x4000|0x10000 (tout sort lancé) ;
--   139944 = 0x8|0x20|0x80|0x200|0x2000|0x20000 (tous les coups subis).
-- Masques de famille : relus par balayage de la famille 22 de Spell.dbc le 2026-09-28.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_proc se recharge à
-- chaud (« reload spell_proc ») ; spell_script_names, spell_bonus_data, spell_group et
-- spell_linked_spell sont lus au démarrage : redémarrer le monde.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_proc — talents dont l'aura 42 (ou 231, 354) n'avait aucun moteur
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` IN (92096, 300593, 300612, 301219, 301298, 500041, 500580,
    500923, 505214, 520084, 560030, 561228, 681110, 704209, 705692, 705708, 705717, 705723, 706297,
    706629, 707053, 803565, 804591, 804826, 806374, 806409, 806412, 807481);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
-- 92096 Electrocutioner : dégâts de sort -> 804592 (remise à zéro d'Electrocute, ProcChance 5).
(92096,  0,  0,        0,      0, 0,  65536, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 300593 Dark Skies : décision par LANCEMENT (phase CAST, qui connaît déjà le critique) ;
-- HitMask 3 = normal + critique, le script retire le buff sur critique.
(300593, 0,  0,        0,      0, 0,  65536, 0, 1, 3, 0, 0, 0, 100, 0, 0),
-- 300612 Storm Chaser : dégâts de sort directs -> 300915 (ProcChance 33).
(300612, 0,  0,        0,      0, 0,  65536, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 301219 Thunder Grip : dégâts directs (coups blancs compris) -> 301220 (ProcChance 30).
(301219, 0,  0,        0,      0, 0,  65876, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 301298 Sparks : Electrocute et Arm of Thorim (masque (33554432,2,0)) -> 706729 ;
-- l'effet 0 (aura 79) n'est pas une aura de proc.
(301298, 0, 22, 33554432,      2, 0,  65536, 1, 2, 0, 0, 1, 0,   0, 0, 0),
-- 500041 Body of Lightning (aura sur l'allié) : ses dégâts directs -> 800406.
(500041, 0,  0,        0,      0, 0,  65876, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 500580 Storm Bond (aura sur le familier, effet 190) : ses dégâts -> 807463 (+2 Static au maître).
(500580, 0,  0,        0,      0, 0,  65556, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 500923 Gale Guard : tout coup subi -> 504872 (repoussement). « Lasts for $d or 3 attacks ».
(500923, 0,  0,        0,      0, 0, 139944, 1, 0, 0, 0, 0, 0,   0, 0, 3),
-- 505214 Mystic Thunder (marque sur l'ennemi) : l'ennemi relance un sort -> 505347 (silence
-- posé sur lui-même, tA1), une seule fois. SUPPOSÉ : à la fin de l'incantation relancée.
(505214, 0,  0,        0,      0, 0,  87040, 0, 1, 0, 0, 0, 0,   0, 0, 1),
-- 520084 Surge of Might (aura 231 sur les alliés) : dégâts directs -> 573254, montant posé par
-- aura_ascension_stormbringer_surge_of_might.
(520084, 0,  0,        0,      0, 0,  65876, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 560030 Lightning Cage : remplace l'entrée engendrée depuis ProcFlags DBC 0xA22A8 (coups
-- SUBIS). Sorts de dégâts du masque (6416,131090,0) -> 804086 (+20 Static), un gain par
-- lancement (aura_ascension_stormbringer_once_per_cast). Effets 0 et 2 hors proc.
(560030, 0, 22,     6416, 131090, 0,  65536, 1, 2, 0, 0, 5, 0, 100, 0, 0),
-- 561228 Blessing of Lei Shen : ticks de Stormflow (masque (0,65536,0)), soin par script.
(561228, 0, 22,        0,  65536, 0, 262144, 1, 2, 0, 0, 0, 0, 100, 0, 0),
-- 681110 Storm Ascendance : sorts de dégâts -> 804084 (+10 Static), une fois par lancement ;
-- effets 1 et 2 (auras 79 et 107) hors proc. « Can only occur once every sec. » : Cooldown 1000.
(681110, 0,  0,        0,      0, 0,  65536, 1, 2, 0, 0, 6, 0, 100, 1000, 0),
-- 704209 Comforting Winds : dégâts du joueur -> 584235 (hâte du familier, tA5).
(704209, 0,  0,        0,      0, 0,  65540, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 705692 Thunder Wave : dégâts de Forked Lightning (seul porteur du bit FF0 0x100) -> 712353
-- (ProcChance 5) ; spell_ascension_stormbringer_forked_lightning fait la transformation.
(705692, 0, 22,      256,      0, 0,  65536, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 705708 Aeromancy : lancement d'Updraft (masque (0,256,0), les 10 Updraft) -> 707544 ;
-- effet 1 (aura 108) hors proc.
(705708, 0, 22,        0,    256, 0,  87040, 0, 1, 0, 0, 2, 0, 100, 0, 0),
-- 705717 Evergale : lancement de Call Lightning / Aeroblast (masque (0,16,0)) -> 705716
-- (rend une charge de Gale, SpellCharges 189) ; effet 1 (aura 107) hors proc.
(705717, 0, 22,        0,     16, 0,  65536, 0, 1, 0, 0, 2, 0, 100, 0, 0),
-- 705723 Unstable : critiques directs -> 707222 (Stormflow plus rapide).
(705723, 0,  0,        0,      0, 0,  65808, 1, 2, 2, 0, 0, 0, 100, 0, 0),
-- 706297 Eye of the Tornado : critiques de dégâts -> 804044 (+5 Static).
(706297, 0,  0,        0,      0, 0,  65876, 1, 2, 2, 0, 0, 0, 100, 0, 0),
-- 706629 Focal Point : lancement de Call Lightning / Aeroblast -> 680876 (critique du groupe).
(706629, 0, 22,        0,     16, 0,  65536, 0, 1, 0, 0, 0, 0, 100, 0, 0),
-- 707053 Altered Course : lancement de Discharge (masque (0,4096,0), attrape aussi Ride the
-- Lightning 800099, « Scales with modifiers to Discharge ») -> 707219 (vitesse).
(707053, 0, 22,        0,   4096, 0,  87040, 0, 1, 0, 0, 0, 0, 100, 0, 0),
-- 803565 Atmospheric Pressure : tout coup subi, périodique compris -> 803568 ; effets 0 et 2
-- (auras de zone ennemie) hors proc, ce qui empêche aussi les ennemis sous l'aura de proc.
(803565, 0,  0,        0,      0, 0, 664232, 1, 0, 0, 0, 5, 0, 100, 0, 0),
-- 804591 Thunder King : critiques de sort -> 800020 (Thunder Fist) ; effets 1 et 2 hors proc.
(804591, 0,  0,        0,      0, 0,  65536, 1, 2, 2, 0, 6, 0, 100, 0, 0),
-- 804826 Charge : la hâte vaut pour « le prochain sort » : une charge, consommée au lancement
-- (phase CAST) du premier sort non déclenché. SUPPOSÉ : le lancement de Charge lui-même ne la
-- consomme pas (l'aura n'est posée qu'après sa phase CAST, Spell.cpp:4016 puis 4079).
(804826, 0,  0,        0,      0, 0,  87040, 0, 1, 0, 0, 0, 0, 100, 0, 1),
-- 806374 Conduit : dégâts de Givre (école 16), directs ou périodiques -> 806393 (ProcChance 60).
(806374, 16, 0,        0,      0, 0, 327680, 1, 2, 0, 0, 0, 0,   0, 0, 0),
-- 806409 Kinetic Energy : lancement de Torrential Wrath / Deluge (masque (0,32769,0)) -> 806410.
(806409, 0, 22,        0,  32769, 0,  65536, 0, 1, 0, 0, 0, 0, 100, 0, 0),
-- 806412 Waterfall : critiques de dégâts, recharges réduites par script (une fois par lancement).
(806412, 0,  0,        0,      0, 0,  65876, 1, 2, 2, 0, 0, 0, 100, 0, 0),
-- 807481 Predictable Weather : « your NEXT Storm Alert or Torrential Wrath » : une charge,
-- consommée par le sort que le modificateur touche (PROC_ATTR_REQ_SPELLMOD = 8), masque
-- (67108864,1,0). Remplace l'entrée engendrée depuis ProcFlags DBC 4 (coups blancs).
(807481, 0, 22, 67108864,      1, 0,  87040, 0, 1, 0, 8, 0, 0, 100, 0, 1);

-- -------------------------------------------------------------------------------------
-- 2. spell_script_names — les scripts C++ de ce nettoyage
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
    'aura_ascension_stormbringer_static_hidden', 'aura_ascension_stormbringer_dark_skies',
    'aura_ascension_stormbringer_waterfall', 'aura_ascension_stormbringer_once_per_cast',
    'aura_ascension_stormbringer_lei_shen', 'aura_ascension_stormbringer_surge_of_might',
    'aura_ascension_stormbringer_stormcloak', 'aura_ascension_stormbringer_kiss_shield',
    'spell_ascension_stormbringer_eye_burst', 'spell_ascension_stormbringer_forked_lightning',
    'spell_ascension_stormbringer_stormbreaker', 'spell_ascension_stormbringer_ride_the_lightning');
DELETE FROM `spell_script_names` WHERE `spell_id` = 803564 AND `ScriptName` = 'aura_ascension_barometric_pressure';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(707038, 'aura_ascension_stormbringer_static_hidden'),
(300593, 'aura_ascension_stormbringer_dark_skies'),
(806412, 'aura_ascension_stormbringer_waterfall'),
(560030, 'aura_ascension_stormbringer_once_per_cast'),
(681110, 'aura_ascension_stormbringer_once_per_cast'),
(561228, 'aura_ascension_stormbringer_lei_shen'),
(520084, 'aura_ascension_stormbringer_surge_of_might'),
(804833, 'aura_ascension_stormbringer_stormcloak'),
(500292, 'aura_ascension_stormbringer_kiss_shield'),
(806455, 'spell_ascension_stormbringer_eye_burst'),
-- Forked Lightning, les sept rangs de spell_ranks (801851).
(801851, 'spell_ascension_stormbringer_forked_lightning'),
(501438, 'spell_ascension_stormbringer_forked_lightning'),
(501439, 'spell_ascension_stormbringer_forked_lightning'),
(501440, 'spell_ascension_stormbringer_forked_lightning'),
(501441, 'spell_ascension_stormbringer_forked_lightning'),
(582303, 'spell_ascension_stormbringer_forked_lightning'),
(582304, 'spell_ascension_stormbringer_forked_lightning'),
(705669, 'spell_ascension_stormbringer_stormbreaker'),
(800099, 'spell_ascension_stormbringer_ride_the_lightning'),
-- Crushing Pressure partage le script de Barometric Pressure (compagnon 803567).
(803564, 'aura_ascension_barometric_pressure');

-- -------------------------------------------------------------------------------------
-- 3. spell_linked_spell — 681110 Storm Ascendance pose 681187 (-50 % de dégâts magiques
--    subis). Type 2 : lié à l'aura, retiré avec elle. P-162 ne s'applique pas : 681110 n'est
--    pas passif (attr0 0). La transformation de 681187 (créature 161579 absente, rendu
--    « cochon rose ») est neutralisée par contrat dans AscensionStormbringerTalents.cpp.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_linked_spell` WHERE `spell_trigger` = 681110 AND `spell_effect` = 681187;
INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
(681110, 681187, 2, 'CoA Stormbringer Storm Ascendance: magic damage taken reduction');

-- -------------------------------------------------------------------------------------
-- 4. spell_bonus_data — coefficient de puissance des sorts de la famille 22
--
-- Aucune ligne n'existait pour la famille 22 (mesuré : 0 sur 955 sorts) et
-- AscensionStockCoefficients.cpp remet EffectBonusMultiplier à 0 : ces sorts ne montaient pas
-- avec la puissance des sorts. Valeurs LUES dans l'infobulle (champ 170) de chaque rang, par
-- sonde : toutes identiques d'un rang à l'autre. SpellMgr::GetSpellBonusData cherche l'id puis
-- le rang 1 de sa chaîne : une ligne par rang 1 pour les chaînes de spell_ranks, une ligne par
-- id pour les sorts non chaînés (Aeroblast, Brine, Updraft, Torrential Wrath, Ride the
-- Lightning, Eye of the Storm burst).
-- $cond(a>b, …) : spell_bonus_data n'a qu'un coefficient ; on garde la branche que
-- l'infobulle affiche à puissances égales (la branche « sinon ») : Brine 0,871, Torrential
-- Wrath 1,5. La branche puissance d'attaque de Shock n'est pas exprimable (signalé).
-- EXCLUS, parce qu'ils reprennent un montant déjà résolu : 560336, 802750, 570174, 705644,
-- 561308 (contrat AscensionInheritsResolvedAmount), 807555, 807133 (instantané du maître),
-- 706433 (Power Sphere, puissance ajoutée par le code).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_bonus_data` WHERE `entry` IN (804020, 500040, 800227, 801844, 801847, 567555, 806400,
    804036, 500038, 800099, 806455, 801839, 501450, 501451, 501452, 501453, 501454, 501455, 501456, 501457,
    501458, 807105, 807106, 807107, 807108, 807109, 807110, 807111, 802354, 570161, 570162, 570163, 570164,
    570165, 570166, 804017, 503352, 503353, 503354, 503355, 503356, 503357, 503358, 503359, 503360,
    567560, 707220);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(804020, 0.55,   0,      0, 0, 'Stormbringer Shock (chain): $spn*0.55'),
(500040, 1.0,    0,      0, 0, 'Stormbringer Call Lightning (chain): $spn*1'),
(800227, 0.2,    0.15,   0, 0, 'Stormbringer Conjure Storm (chain): $spn*.2, $spn*.15 per tick'),
(801844, 0.45,   0,      0, 0, 'Stormbringer Electrocute (chain): $spn*0.45'),
(801847, 1.2,    0,      0, 0, 'Stormbringer Arm of Thorim (chain): $spn*1.2'),
(567555, 0,      0.2,    0, 0, 'Stormbringer Stormflow (chain): $SP*.2 per tick'),
(806400, 0.3,    0,      0, 0, 'Stormbringer Deluge (chain): $spn*0.3'),
(804036, 0.42,   0,      0, 0, 'Stormbringer Gale (chain): $spn*0.42'),
(500038, 0.245,  0,      0, 0, 'Stormbringer Invigorating Surge (chain, heal): $SP*.245'),
(800099, 0.5,    0,      0, 0, 'Stormbringer Ride the Lightning: $SP*.5'),
(806455, 0.64,   0,      0, 0, 'Stormbringer Eye of the Storm burst: $spn*0.64 (806307 tooltip)'),
(801839, 0.635,  0,      0, 0, 'Stormbringer Aeroblast R1: $spn*0.635'),
(501450, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501451, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501452, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501453, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501454, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501455, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501456, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501457, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(501458, 0.635,  0,      0, 0, 'Stormbringer Aeroblast: $spn*0.635'),
(807105, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807106, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807107, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807108, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807109, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807110, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(807111, 0.871,  0,      0, 0, 'Stormbringer Brine: $spn*0.871'),
(802354, 0,      0.0683, 0, 0, 'Stormbringer Updraft R1: $spn*0.0683 per tick'),
(570161, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(570162, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(570163, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(570164, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(570165, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(570166, 0,      0.0683, 0, 0, 'Stormbringer Updraft: $spn*0.0683 per tick'),
(804017, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath R1: $spn*1.5'),
(503352, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503353, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503354, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503355, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503356, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503357, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503358, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503359, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
(503360, 1.5,    0,      0, 0, 'Stormbringer Torrential Wrath: $spn*1.5'),
-- Sorts désormais déclenchés par le code de classe, lancés sans montant imposé (CastSpell) :
-- ils ont donc besoin de leur coefficient. Conduction : branche « sinon » du $cond, comme Brine.
(567560, 0.35,   0,      0, 0, 'Stormbringer Conduction: $spn*.35 (Torrential Wrath, Deluge)'),
(707220, 0.65,   0,      0, 0, 'Stormbringer Thunder Wave: $spa*.65 (Forked Lightning proc 705692)');

-- -------------------------------------------------------------------------------------
-- 5. spell_group — « Can only have 1 Aegis up at a time », « Only 1 Pressure spell »,
--    « Does not stack with Call of the Wind / Storm ». Règle 1 = EXCLUSIVE (SpellMgr.h).
--    Ids 2116001-2116003 (16 = la classe) : hors de la plage déjà prise (max 2100001 mesuré)
--    et de celle que d'autres écrivains pourraient prendre en suivant.
--    Appartenance par premier rang (SpellMgr::GetSpellGroupSpellMapBounds, GetFirstRankSpell) :
--    680282 couvre 680590-680594, 578311 couvre 578312-578315, 804018 couvre 503319-503323.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_group` WHERE `id` IN (2116001, 2116002, 2116003);
INSERT INTO `spell_group` (`id`, `spell_id`) VALUES
(2116001, 680282), (2116001, 680316), (2116001, 680334),
(2116002, 803563), (2116002, 803564), (2116002, 572399),
(2116003, 578311), (2116003, 578316), (2116003, 804018), (2116003, 680291);
DELETE FROM `spell_group_stack_rules` WHERE `group_id` IN (2116001, 2116002, 2116003);
INSERT INTO `spell_group_stack_rules` (`group_id`, `stack_rule`, `description`) VALUES
(2116001, 1, 'CoA Stormbringer Aegis'),
(2116002, 1, 'CoA Stormbringer Pressure'),
(2116003, 1, 'CoA Stormbringer Call');
