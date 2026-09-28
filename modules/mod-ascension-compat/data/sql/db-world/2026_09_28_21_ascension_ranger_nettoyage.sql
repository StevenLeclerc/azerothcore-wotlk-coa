-- =====================================================================================
-- mod-ascension-compat — Ranger (classe 21, famille de sorts 27) : nettoyage du 2026-09-28.
-- Moitié « donnée » des correctifs de AscensionRangerTalents.cpp, AscensionRangerEvents.cpp,
-- AscensionRangerAssault.cpp, AscensionRangerSecondary.cpp et AscensionRangerHookshot.cpp.
--
-- Rappels payés : P-045/P-053 (aura de proc à ProcFlags DBC nul et sans ligne spell_proc :
-- SpellMgr::LoadSpellProcs la saute), P-051 (script C++ sans ligne spell_script_names : ne
-- tourne jamais).
--
-- Conventions vérifiées dans le cœur (SpellMgr.cpp:2107-2115, 2127-2150, 896-958) :
--   ProcFlags 0 -> repris du DBC, Chance 0 -> ProcChance du DBC, Charges 0 -> ProcCharges,
--   SpellPhaseMask exigé pour tout drapeau DONE de REQ_SPELL_PHASE_PROC_FLAG_MASK (SpellMgr.h:184),
--   dont 0x40 (tir automatique), les lignes à drapeaux TAKEN seuls ont SpellPhaseMask 0.
--   Phase CAST (1) : une fois par lancement, cible nulle (le sort déclenché vise le lanceur),
--   phase HIT (2) : une fois par cible touchée.
--   Le masque de famille n'est lu que si SpellFamilyName est non nul (SpellInfo::IsAffected).
--   Auto Shot (75) est de famille 9 : toute ligne à 0x40 garde SpellFamilyName 0.
-- Drapeaux (SpellMgr.h:106-135) : 0x2 KILL, 0x4 DONE_MELEE_AUTO, 0x8 TAKEN_MELEE_AUTO,
--   0x10 DONE_SPELL_MELEE, 0x20 TAKEN_SPELL_MELEE, 0x40 DONE_RANGED_AUTO, 0x80 TAKEN_RANGED_AUTO,
--   0x100 DONE_SPELL_RANGED, 0x200 TAKEN_SPELL_RANGED, 0x400 DONE_NONE_POS, 0x1000 DONE_NONE_NEG,
--   0x4000 DONE_MAGIC_POS, 0x10000 DONE_MAGIC_NEG, 0x40000 DONE_PERIODIC.
--   272 = 0x110 (techniques de mêlée et à distance), 340 = 0x154 (coups directs faits, auto et
--   techniques), 65876 = 0x10154 (tous les dégâts directs faits, magie comprise),
--   87312 = 0x15510 (tout sort ou technique lancé, sans les attaques automatiques).
-- AttributesMask : 1 REQ_EXP_OR_HONOR, 2 TRIGGERED_CAN_PROC, 8 REQ_SPELLMOD (la charge ne tombe
--   que sur le sort que le modificateur a réellement touché, patron des lignes 1719 et 12043).
-- Masques de famille : relus par balayage de la famille 27 de Spell.dbc le 2026-09-28.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_proc se recharge à chaud
-- (« reload spell_proc ») et spell_bonus_data aussi, spell_script_names, spell_linked_spell et
-- creature_template_addon sont lus au démarrage : redémarrer le monde.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_proc — talents dont l'aura 42 / 231 / 354 n'avait aucun moteur
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` IN (560809, 801468, 504329, 705047, 801437, 573059, 524831,
    503647, 560810, 560802, 707744, 705075, 707383, 706282, 537248, 520353, 572372, 705071, 705054,
    560698, 680931, 573202, 301249, 520586, 520628, 520783, 524873, 570737, 704311, 706748, 804178,
    806957, 807459, 807023, 806973, 680276, 560341, 705033, 524869, 680471, 800684);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
-- 560809 Endurance of the Ranger (aura de groupe du cor 806359) : « a 30% chance when they cast a
-- spell or ability to heal themselves [...] Can only occur once every 2 sec ». Une fois par lancement.
(560809, 0,  0,         0,         0,         0,  87312, 0, 1, 0, 0, 0, 0, 30, 2000, 0),
-- 801468 Pinpoint Accuracy : critique d'une technique directe de la classe -> 801471 (+4 Focus).
(801468, 0, 27,         0,         0,         0,    272, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 504329 Survival Kit : Survival Potion 802839 (DmgClass 1, seul porteur de FF2 0x20000000) -> 504328.
(504329, 0, 27,         0,         0, 536870912,  81920, 0, 1, 0, 0, 0, 0,  0,    0, 0),
-- 705047 Beatdown : attaques automatiques (mêlée et tir) -> 801437, chance 5 du DBC, +15 à l'arme
-- d'hast par 706586 (CHANCE_OF_SUCCESS). Effets 1 et 2 hors proc.
(705047, 0,  0,         0,         0,         0,     68, 1, 2, 0, 0, 6, 0,  0,    0, 0),
-- 801437 Beatdown (IGNORE_ARMOR -100 %, 60 s, sans charge) : « your next instance of Physical ability
-- damage ». Une charge, consommée par la technique que le modificateur a touchée (son propre masque).
(801437, 0, 27,         0, 134316289,    131072,    272, 1, 2, 0, 8, 0, 0,  0,    0, 1),
-- 573059 Gutpiercer (effet 190, porté par les War Falcons) : leurs coups -> 803505 (chance 15).
(573059, 0,  0,         0,         0,         0,     20, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 524831 Thread The Needle : critique de Quick Shot (FF1 0x1) ou de Falconstrike (FF1 0x400000) -> 503647.
(524831, 0, 27,         0,   4194305,         0,    272, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 503647 Thread The Needle (-36 % coût et incantation, 3 piles, 15 s, sans charge) : « your next
-- ability that consumes Advantage ». Consommé au lancement du consommateur qu'il a touché.
(503647, 0, 27,         0, 201326592,  16785408,    272, 0, 1, 0, 8, 0, 0,  0,    0, 1),
-- 560810 A Quick Demise : tir automatique -> 561109 (chance 20), la prolongation suit la chaîne
-- de rangs de Serrated Shot (spell_ascension_ranger_quick_demise).
(560810, 0,  0,         0,         0,         0,     64, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 560802 Quel'dorei Poison : Emerald Arrow (FF1 0x4000000) et Woodland Arrow (FF2 0x2000) -> 586212.
-- Neurotoxin Arrow porte aussi FF2 0x2000 : elle empoisonne aussi (aucun masque ne l'en sépare).
(560802, 0, 27,         0,  67108864,      8192,    256, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 707744 Highwayman : critique direct -> 705063, « dans le dos » : aura_ascension_ranger_highwayman.
(707744, 0,  0,         0,         0,         0,    340, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 705075 Falcon's Aid : dégâts périodiques -> 520588 (War Falcon, chance 10).
(705075, 0,  0,         0,         0,         0, 262144, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 707383 Falcon's Aid (SLS) : Woodland Arrow (FF2 0x2000) et ticks de Toxic Dart (FF0 0x8000) -> 520588.
(707383, 0, 27,     32768,         0,      8192, 262400, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 706282 Forest Fighter : critique de Falconstrike -> un War Falcon de plus.
(706282, 0, 27,         0,   4194304,         0,    256, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 537248 Command Aura (aura 231 sur les alliés) : leurs dégâts directs -> 537249, montant posé par
-- aura_ascension_ranger_command_aura (RAP du Ranger x 0,35). L'effet 1 n'est pas une aura.
(537248, 0,  0,         0,         0,         0,  65876, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 520353 Guile of the Ranger : lancement d'Instinct (FF0 0x10, DmgClass 2 -> 0x10) -> 520545.
(520353, 0, 27,        16,         0,         0,     16, 0, 1, 0, 0, 0, 0,  0,    0, 0),
-- 572372 Corrosive Shot : tir automatique -> 572419 (chance 5), l'effet 0 (aura 112) hors proc.
(572372, 0,  0,         0,         0,         0,     64, 1, 2, 0, 0, 1, 0,  0,    0, 0),
-- 705071 Plumes of War : critique de Falconstrike -> 705078, effets 0 et 1 (modificateurs) hors proc.
(705071, 0, 27,         0,   4194304,         0,    256, 1, 2, 2, 0, 3, 0,  0,    0, 0),
-- 705054 Elven Enchantments : dégâts des carquois (Fire Arrows FF1 0x80000, Poison Quiver FF1 0x400,
-- Crippling Shot et Envenomed Focus Drain partagent ces bits) -> 560698. Ce sont des sorts déclenchés :
-- TRIGGERED_CAN_PROC. Effets 1 et 2 hors proc.
(705054, 0, 27,         0,    525312,         0, 262400, 1, 2, 0, 2, 6, 0,  0,    0, 0),
-- 560698 Elven Enchantments (pile) : « Cast time of the next Incendiary Shot » -> consommé au lancement.
(560698, 0, 27,         0,         0,   4194304,    256, 0, 1, 0, 8, 0, 0,  0,    0, 1),
-- 680931 Headshots Only : ticks de Serrated Shot (FF2 0x80) et de Toxic Dart (FF0 0x8000) -> 573202.
(680931, 0, 27,     32768,         0,       128, 262144, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 573202 Headshots Only (pile) : « your next Precision Shot » -> consommé par le Precision Shot touché.
(573202, 0, 27,         0,   8388608,         0,    256, 1, 2, 0, 8, 0, 0,  0,    0, 1),
-- 301249 Superb Shot : dégâts d'Auto Shot -> 561183 (+3 Focus).
(301249, 0,  0,         0,         0,         0,     64, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 520586 Falcon Guide : lancement de Falconstrike (FF1 0x400000) ou de Battle Screech (FF2 0x8) -> 520597.
(520586, 0, 27,         0,   4194304,         8,   5376, 0, 1, 0, 0, 0, 0,  0,    0, 0),
-- 520628 Freedom (aura de groupe) : critiques des alliés -> 520629, effets 0 et 1 (immunités) hors proc.
(520628, 0,  0,         0,         0,         0,  65876, 1, 2, 2, 0, 3, 0,  0,    0, 0),
-- 520783 Falcon Diving : « Killing an enemy that yields experience or honor [...] once every 4 sec ».
(520783, 0,  0,         0,         0,         0,      2, 0, 0, 0, 1, 0, 0,  0, 4000, 0),
-- 524873 Silent, But Deadly : dégâts physiques directs -> 524872 (chance 8).
(524873, 1,  0,         0,         0,         0,    340, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 570737 Pecking (effet 190, porté par les War Falcons) : leurs coups -> 570749 (chance 25).
(570737, 0,  0,         0,         0,         0,     20, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 704311 Quick-Witted : attaque évitée (raté, esquive, parade) -> 704314 sur l'attaquant.
(704311, 0,  0,         0,         0,         0,    680, 0, 0, 52, 0, 0, 0,  0,    0, 0),
-- 706748 'Tactical' Advantage : Knockout (FF0 0x20) et Bushwhack (FF1 0x40000000) -> 706749.
(706748, 0, 27,        32, 1073741824,        0,    272, 0, 2, 0, 0, 0, 0,  0,    0, 0),
-- 804178 Strike Where It Hurts : critique d'une technique de la classe -> 567588, effet 0 hors proc.
(804178, 0, 27,         0,         0,         0,  65808, 1, 2, 2, 0, 1, 0,  0,    0, 0),
-- 806957 Knuckleduster : dégâts périodiques -> 525081 (+5 Focus, chance 100). Pas de recharge dans
-- l'infobulle : un gain par tick de chaque DoT.
(806957, 0,  0,         0,         0,         0, 262144, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 807459 Venom-Coated Seeds : dégâts de Snapseed (FF1 0x10, DmgClass 3) -> 807553.
(807459, 0, 27,         0,        16,         0,    256, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 807023 Tip of the Spear : Wild Strike (FF1 0x100) -> 807024 x 3, arme d'hast : aura_..._tip_of_the_spear.
(807023, 0, 27,         0,       256,         0,     16, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 806973 Marked for Death : critique direct -> 806974, dague et cible sous 35 % : aura_..._marked_for_death.
(806973, 0,  0,         0,         0,         0,     20, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 680276 Dirty Blades : attaques automatiques de mêlée -> 520571 par aura_ascension_ranger_dirty_blades.
-- 0x4 n'exige pas de phase. L'effet 1 (modificateur) hors proc.
(680276, 0,  0,         0,         0,         0,      4, 0, 0, 0, 0, 2, 0,  0,    0, 0),
-- 560341 Rapid Strikes : attaques automatiques -> 560342 (chance 25), « once per sec ».
(560341, 0,  0,         0,         0,         0,     68, 1, 2, 0, 0, 0, 0,  0, 1000, 0),
-- 705033 Pierced : critique de Skullpiercer (FF1 0x8000000) ou de Precision Shot (FF1 0x800000) -> 782754.
(705033, 0, 27,         0, 142606336,         0,    256, 1, 2, 2, 0, 0, 0,  0,    0, 0),
-- 524869 Incendiary Arrows : dépenses d'Advantage à dégâts directs, comptées par le script.
(524869, 0,  0,         0,         0,         0,    272, 1, 2, 0, 0, 0, 0,  0,    0, 0),
-- 680471 Masterful Archery (pile) : « Your next Precision Shot » -> consommé par le Precision Shot touché.
(680471, 0, 27,         0,   8388608,         0,    256, 1, 2, 0, 8, 0, 0,  0,    0, 1),
-- 800684 Backstep (bonus) : « Your next Skullpiercer [...] is instant » -> consommé au lancement.
(800684, 0, 27,         0, 142606336,         0,    256, 0, 1, 0, 8, 0, 0,  0,    0, 1);

-- -------------------------------------------------------------------------------------
-- 2. spell_bonus_data — coefficients promis par l'infobulle et appliqués nulle part
-- -------------------------------------------------------------------------------------
-- 560808 Endurance of the Ranger (soin) : ${$560808m1+$560808ppl1+$AP*0.1295}. Le soin est lancé
-- par l'allié avec le Ranger pour lanceur d'origine : l'AP lue est celle du Ranger.
-- 804712 Emerald Arrow (chaîne de 6 rangs, lue au rang 1) : ${$m1+$RAP*0.2}, direct_bonus 0
-- garde l'absence de puissance des sorts d'aujourd'hui.
DELETE FROM `spell_bonus_data` WHERE `entry` IN (560808, 804712);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(560808, 0, 0, 0.1295, 0, 'Ranger - Endurance of the Ranger heal'),
(804712, 0, 0, 0.2,    0, 'Ranger - Emerald Arrow explicit RAP');

-- -------------------------------------------------------------------------------------
-- 3. spell_linked_spell — passif de palier que rien ne posait
-- -------------------------------------------------------------------------------------
-- 92115 Skirmisher (niveau 10) promet Toxic Dart +30 % de dégâts et de cadence : c'est 500022,
-- que rien ne pose. 500022 est passif (attr0 0x40) : il survit à la mort (P-162 ne s'applique pas).
DELETE FROM `spell_linked_spell` WHERE `spell_trigger` = 92115 AND `spell_effect` = 500022;
INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
(92115, 500022, 2, 'Ranger Skirmisher level 10 passive');

-- -------------------------------------------------------------------------------------
-- 4. creature_template_addon — les faucons de Falcon's Call comptent pour Wingman et Guidance
-- -------------------------------------------------------------------------------------
-- Décision prise sur le texte : Wingman dit « for each War Falcon », et 50264 (invoqué par 800251
-- Falcon's Call) s'appelle « War Falcon », comme 50393. Conséquence voulue : chaque faucon rend aussi
-- 1 Focus/s (680278 e1). Aucun spawn de 50264 dans `creature`.
DELETE FROM `creature_template_addon` WHERE `entry` = 50264;
INSERT INTO `creature_template_addon` (`entry`, `auras`) VALUES (50264, '680278');

-- -------------------------------------------------------------------------------------
-- 5. spell_script_names — scripts de cette vague (lus au démarrage)
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
    'spell_ascension_ranger_quick_demise', 'aura_ascension_ranger_highwayman',
    'aura_ascension_ranger_command_aura', 'aura_ascension_ranger_dirty_blades',
    'spell_ascension_ranger_dirty_blades_leech', 'spell_ascension_ranger_woodland_arrow',
    'aura_ascension_ranger_incendiary_arrows', 'aura_ascension_ranger_tip_of_the_spear',
    'aura_ascension_ranger_marked_for_death', 'spell_ascension_ranger_skewer',
    'spell_ascension_ranger_skewer_pull', 'aura_ascension_ranger_backstep',
    'aura_ascension_ranger_backstep_bonus', 'aura_ascension_ranger_toxic_dart_haste',
    'spell_ascension_ranger_flank', 'spell_ascension_ranger_wild_strike_offhand',
    'aura_ascension_ranger_guidance', 'aura_ascension_ranger_rapid_strikes',
    'spell_ascension_ranger_sly', 'aura_ascension_ranger_sly', 'aura_ascension_ranger_pierced',
    'aura_ascension_ranger_masterful_archery', 'aura_ascension_ranger_hawkeye');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(561109,  'spell_ascension_ranger_quick_demise'),
(707744,  'aura_ascension_ranger_highwayman'),
(537248,  'aura_ascension_ranger_command_aura'),
(680276,  'aura_ascension_ranger_dirty_blades'),
(520571,  'spell_ascension_ranger_dirty_blades_leech'),
-- Coexiste avec (-806368, spell_ascension_ranger_phoenix_plumes).
(-806368, 'spell_ascension_ranger_woodland_arrow'),
(524869,  'aura_ascension_ranger_incendiary_arrows'),
(807023,  'aura_ascension_ranger_tip_of_the_spear'),
(806973,  'aura_ascension_ranger_marked_for_death'),
(800093,  'spell_ascension_ranger_skewer'),
(800153,  'spell_ascension_ranger_skewer_pull'),
(800685,  'aura_ascension_ranger_backstep'),
(800684,  'aura_ascension_ranger_backstep_bonus'),
(-807237, 'aura_ascension_ranger_toxic_dart_haste'),
(-804940, 'spell_ascension_ranger_flank'),
(-800083, 'spell_ascension_ranger_wild_strike_offhand'),
(532261,  'aura_ascension_ranger_guidance'),
(560341,  'aura_ascension_ranger_rapid_strikes'),
(801345,  'spell_ascension_ranger_sly'),
(570015,  'spell_ascension_ranger_sly'),
(801345,  'aura_ascension_ranger_sly'),
(705033,  'aura_ascension_ranger_pierced'),
(680469,  'aura_ascension_ranger_masterful_archery'),
(800358,  'aura_ascension_ranger_hawkeye');
