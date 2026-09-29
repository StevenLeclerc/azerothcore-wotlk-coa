-- =====================================================================================
-- mod-ascension-compat — Reaper (classe 30, famille 36), nettoyage de la vague 5, 2026-09-29.
--
-- UN SEUL fichier SQL pour toute la classe : aucun 2026_09_28_*_reaper_* n'existe (MESURÉ le
-- 2026-09-29). Idempotent (DELETE puis INSERT). Updates.EnableDatabases = 0 : à appliquer à la
-- main. spell_script_names n'est lu qu'au DÉMARRAGE : l'appliquer AVANT de démarrer le
-- worldserver qui porte les scripts (AscensionReaper{Events,Secondary,Talents}.cpp). spell_proc
-- se recharge à chaud (.reload spell_proc), mais une ligne dont le script n'est pas lié donne un
-- proc sans son contrôle : appliquer les deux parties ENSEMBLE, avec le binaire.
--
-- Pour chaque ligne spell_proc : ProcFlags DBC = 0 et aucune ligne (MESURÉ, Spell.dbc champ 34 et
-- acore_world.spell_proc), donc aucune SpellProcEntry (SpellMgr.cpp « Skip if no proc flags ») et
-- l'aura ne se déclenchait jamais. Chance 0 = ProcChance du DBC.
--
-- VOLONTAIREMENT SANS LIGNE (le moteur est en C++, une ligne ferait un second déclenchement) :
-- 804311 Soul Harvester (reaper_kill_events), 504012 Soul Harvest et 680995 Painbringer
-- (aura_ascension_reaper_soul_events), 92147 Tormentor, 504046 Purgatory, 706786 Damned
-- (aura_ascension_reaper_soul_infusion), 705428, 504309, 705424, 705397, 805186, 707899, 704357,
-- 705414, 712484, 300569 (reaper_secondary_hits / reaper_tormented_souls_events).
-- =====================================================================================

DELETE FROM `spell_proc` WHERE `SpellId` IN (561170, 573039, 705437, 707707, 300565, 706795, 560487,
    500286, 704356, 805196, 805198, 704558, 301193, 560919, 704193, 803997, 504304, 572768, 807775,
    561127, 561340, 705964, 707455);
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
-- Spectre : Withering Touch (573071, masque 0/131072/0, DmgClass 1) pose toujours Ghost Claw 803985. e0 (mod) hors proc.
(561170, 0, 36, 0, 131072, 0, 65536, 0, 2, 0, 0, 1, 0, 0, 0, 0),
-- Extinction : dégâts physiques directs, 5 % DBC -> 520501 (effet 175, crée ou empile 560414).
(573039, 1, 0, 0, 0, 0, 20, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Mortal's End : Wraithblade physique (805258, 2/0/0, DmgClass 2 ; 0x10 seul exclut 807166 magique). e0 (MAX_STACKS) hors proc.
(705437, 0, 36, 2, 0, 0, 16, 1, 2, 0, 0, 1, 0, 0, 0, 0),
-- Red Wake : coup critique direct -> Soul Fragment 805077.
(707707, 0, 0, 0, 0, 0, 65556, 1, 2, 2, 0, 0, 0, 0, 0, 0),
-- Soulsight : dégâts directs, 15 % DBC -> 805077. e0 (aura de raid 290) hors proc.
(300565, 0, 0, 0, 0, 0, 65556, 1, 2, 0, 0, 1, 0, 0, 0, 0),
-- Empyrean Fortitude : parade -> 359019 (puissance runique). e0 (aura 47) hors proc.
(706795, 0, 0, 0, 0, 0, 40, 0, 0, 32, 0, 1, 0, 0, 0, 0),
-- Soul Knight : parade ou esquive -> 560488.
(560487, 0, 0, 0, 0, 0, 40, 0, 0, 48, 0, 0, 0, 0, 0, 0),
-- Soul Slip : raté, esquive, parade, 50 % DBC -> 805077. e2 (mod) hors proc.
(500286, 0, 0, 0, 0, 0, 40, 0, 0, 52, 0, 4, 0, 0, 0, 0),
-- Soultaker : dégâts de Dreadwake (503286, 0/1073741824/0, DmgClass 2) -> 582751 (effet 165, recharge de Tormented Souls).
(704356, 0, 36, 0, 1073741824, 0, 16, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Soulrot : dégâts de Deathchaser (4/0/0). Ses dégâts sont PÉRIODIQUES (805190 e0 = aura 3) :
-- 0x40000 est indispensable, 0x10 seul avec SpellTypeMask 1 ne partirait jamais (le coup initial ne fait pas de dégâts).
-- Le masque de famille s'applique aussi au périodique (SPELL_PROC_FLAG_MASK contient PROC_FLAG_DONE_PERIODIC, SpellMgr.h).
(805196, 0, 36, 4, 0, 0, 262160, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Ruin : dégâts de Murder (0/64/0, DmgClass 1) -> 805199. La variante Shudder Scythe (805708) n'est pas couverte.
(805198, 0, 36, 0, 64, 0, 65536, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Dark Passage : critique direct d'ombre -> 504035 (effet 165, recharge de Limbo).
(704558, 32, 0, 0, 0, 0, 65552, 1, 2, 2, 0, 0, 0, 0, 0, 0),
-- Beyond Death : dégâts de Reliquary of the Lost (500630, 0/4/0, DmgClass 1), 8 % DBC -> Soul Bolt 500618. e1 (mod) hors proc.
(301193, 0, 36, 0, 4, 0, 65536, 1, 2, 0, 0, 2, 0, 0, 0, 0),
-- Essence Binder : Soul Strike (0/2048/0, DmgClass 2) -> 800436 et 800437.
(560919, 0, 36, 0, 2048, 0, 16, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Decimation : un cumul de 573289 par Reap lancé (0/1/0, phase CAST ; modèle de Redshade 524735 en service).
-- Le passage à Decimate est aura_ascension_reaper_decimation_count ; Decimate (4194304/0/0) ne recompte pas.
(704193, 0, 36, 0, 1, 0, 16, 0, 1, 0, 0, 0, 0, 0, 0, 0),
-- Ghostly Weapon : attaques auto et techniques de mêlée (0x14 : SUPPOSÉ d'après « melee attacks »). e1, e2 (mods) hors proc.
(803997, 0, 0, 0, 0, 0, 20, 1, 2, 0, 0, 6, 0, 100, 0, 0),
-- Souls for the Slaughter (aura posée par le C++ sur critique de Slaughter) : attaques auto seules.
(504304, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 100, 0, 0),
-- Siphon Anima (aura posée par le C++ à l'invocation de Spectral Scythe) : dégâts directs faits.
(572768, 0, 0, 0, 0, 0, 65556, 1, 2, 0, 0, 0, 0, 100, 0, 0),
-- Deathstalker, vitesse 807775 : « Dealing damage will break the effect ». Tout dégât fait (auto, mêlée,
-- distance, magie, périodique), une charge : le proc la consomme et retire l'aura (Aura::ConsumeProcCharges).
(807775, 0, 0, 0, 0, 0, 327956, 1, 2, 0, 0, 0, 0, 0, 0, 1),
-- Soulforged Weaponry r1 et r2 : attaques auto (8 % / 15 % DBC) -> 705964 ; la double arme est le DoCheckProc. e0 (aura 122) hors proc.
(561127, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0),
(561340, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0),
-- Soulforged Weaponry, aura 705964 (-100 % de coût sur Murder) : une charge consommée par Murder
-- (PROC_ATTR_REQ_SPELLMOD, modèle 57761). Sans cette ligne l'aura n'a aucune charge : Murder gratuit 8 s durant.
(705964, 0, 36, 0, 64, 0, 65536, 0, 1, 0, 8, 0, 0, 0, 0, 1),
-- Chasing Death : coup de Deathchaser (4/0/0) -> 807418 sur la cible ; le seuil de 20 % est aura_ascension_reaper_chasing_death.
(707455, 0, 36, 4, 0, 0, 16, 0, 2, 0, 0, 0, 0, 0, 0, 0);

DELETE FROM `spell_script_names` WHERE `spell_id` IN (803997, 504304, 572768, 525058, 704215, 573289,
    804054, 807775, 561127, 561340, 707455, 807418);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(803997, 'aura_ascension_reaper_ghostly_weapon'),
(504304, 'aura_ascension_reaper_souls_for_the_slaughter'),
(572768, 'aura_ascension_reaper_siphon_anima'),
(525058, 'aura_ascension_reaper_reap_transform'),
(704215, 'aura_ascension_reaper_reap_transform'),
(573289, 'aura_ascension_reaper_decimation_count'),
(804054, 'spell_ascension_reaper_deathstalker'),
(807775, 'aura_ascension_reaper_deathstalker_speed'),
(561127, 'aura_ascension_reaper_soulforged_weaponry'),
(561340, 'aura_ascension_reaper_soulforged_weaponry'),
(707455, 'aura_ascension_reaper_chasing_death'),
(807418, 'aura_ascension_reaper_chasing_death_extend');
