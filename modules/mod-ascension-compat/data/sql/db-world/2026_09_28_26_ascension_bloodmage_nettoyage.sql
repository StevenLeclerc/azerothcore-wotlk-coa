-- =====================================================================================
-- mod-ascension-compat — BLOODMAGE (classe 20, famille de sorts 26, champ 208 de Spell.dbc)
-- Nettoyage de la classe, 2026-09-28. Accompagne les modifications du meme jour dans
-- src/AscensionBloodmageEvents.cpp, AscensionBloodmageSecondary.cpp, AscensionBloodmageTalents.cpp,
-- AscensionBloodmageVitality.cpp et AscensionBloodmageHemostasis.cpp.
--
-- FICHIER UNIQUE. Il fusionne les anciens _26, _27 (nettoyage_2) et _28 (nettoyage_3), qui posaient les
-- memes objets et dependaient de leur ordre (la ligne spell_proc 706247 etait inseree par le _26, retiree
-- par le _27, reposee par le _28). Seul l'etat final est ecrit ici : ne pas rechercher les deux autres.
--
-- A APPLIQUER A LA MAIN (Updates.EnableDatabases = 0), AVEC le binaire qui porte les scripts
-- nommes en sections 4, 7 et 10 : sans lui, le demarrage signale « Script named ... does not exist » et
-- les lignes spell_proc des auras scriptees (Dark Sigil, Blood Scent, Purify Blood, Blood Bond,
-- Blood Shards, Bite Wound) feraient partir le declenchement natif, qui ne fait rien ou pas ce
-- qu'il faut. spell_script_names, creature_template et gameobject_template ne sont lus qu'au
-- demarrage ; spell_proc, spell_linked_spell et spell_bonus_data ont leur « .reload ». Rejouable :
-- chaque INSERT est precede de son DELETE.
--
-- Chaque ligne a ete ecrite d'apres une relecture de Spell.dbc du 2026-09-28 (effets, ProcFlags,
-- ProcChance, masques de famille). Chance 0 = ProcChance du DBC (SpellMgr.cpp:2114-2115).
-- DONE_PERIODIC (0x40000) et tout drapeau DONE de sort exigent un SpellPhaseMask
-- (REQ_SPELL_PHASE_PROC_FLAG_MASK, SpellMgr.h:184, controle SpellMgr.cpp:937) ; les ticks sont
-- en phase HIT (Unit.h:1571).
--
-- Volontairement ABSENTS (traites en C++, une ligne ici compterait l'effet deux fois) :
-- 92112 Thirst, 800766/504290/800154 (hurlements), 707623 Blood Moon, 806428 Universal Donor,
-- 807566 Acquired Taste, 806274 Aortic Aegis, 806421 Essence Harvester, 804203 Apotheosis,
-- 681427 Waves of Blood, 806944 Cardiac Arrest, 560001 Eternal Presence, 707435 Cursed Blood,
-- 807357 Relentless (proc), 807488 Inhumane, 801963 Battleweaver, 523721 Bloodchaser.
-- Et 504272, 806425, 504137, 680678 : leur « prochain sort » est porte par ProcCharges = 1
-- (contrat C++) ; une ligne spell_proc les rendrait invisibles a Player::RemoveSpellMods
-- (Player.cpp:10553) et la charge ne tomberait jamais.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_proc
-- Colonnes : SpellId, SchoolMask, SpellFamilyName, SpellFamilyMask0, 1, 2, ProcFlags,
--            SpellTypeMask, SpellPhaseMask, HitMask, AttributesMask, DisableEffectsMask,
--            ProcsPerMinute, Chance, Cooldown, Charges
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` IN (
    504100, 680823, 680684, 684331, 706656, 707622, 562572, 570746, 300978, 681792,
    804811, 504292, 804599, 805035, 504116, 680745, 504098, 680695, 704633, 500012,
    802315, 806723, 572800, 680675, 806049, 806424, 805591, 704625, 704653, 704654,
    704692, 706247, 680733, 804858, 582767, 804849, 807787, 704652, 706654, 560535,
    804223, 520839, 504627, 800785, 704624);
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
-- Excess Blood : soins directs (NONE_POS | MAGIC_POS), sans les ticks (504101 est un HoT : ses ticks
-- ne passent pas de procSpell et relanceraient 504101). pc 10.
(504100, 0,  0,        0,        0,        0,  17408, 2, 2, 0, 0, 0, 0, 0,    0, 0),
-- Spice of Life : degats et soins periodiques -> Pooled Vitality. pc 10.
(680823, 0,  0,        0,        0,        0, 262144, 3, 2, 0, 0, 0, 0, 0,    0, 0),
-- Fleshbending : critiques de degats ET de soins directs (0x11154 | 0x4400).
(680684, 0,  0,        0,        0,        0,  87380, 3, 2, 2, 0, 0, 0, 0,    0, 0),
-- Blood-Cursed Weapons : au lancement de Crimson Tide (mot 0 0x4000), phase CAST.
(684331, 0, 26,    16384,        0,        0,  81920, 0, 1, 0, 0, 0, 0, 0,    0, 0),
-- Crimson Curse : critiques de degats directs.
(706656, 0,  0,        0,        0,        0,  69972, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Dark Intent : au lancement de Lunge (mot 0 0x1000000, DmgClass 0), phase CAST.
(707622, 0, 26, 16777216,        0,        0,   5120, 0, 1, 0, 0, 0, 0, 0,    0, 0),
-- Accursed Form : effet 2 seul (Marrowbite) sur les coups de melee.
(562572, 0,  0,        0,        0,        0,     20, 1, 2, 0, 0, 3, 0, 0,    0, 0),
-- Bloodguard : esquive -> Rage (effet 1 seul ; effet 0 est un 107). Gabarit 800084.
(570746, 0,  0,        0,        0,        0,     40, 0, 0,16, 0, 1, 0, 0,    0, 0),
-- Enraging Wounds : critiques de melee -> vol de vie 560589.
(300978, 0,  0,        0,        0,        0,     20, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Cursed Blood (681792) : degats de Bloodbolt (mot 1 0x20000) -> 803722 sur la cible. Effets 1-2
-- (108) exclus.
(681792, 0, 26,        0,   131072,        0,  65536, 1, 2, 0, 0, 6, 0, 0,    0, 0),
-- Monstrous Howl : les 6 prochaines Bloodfang Bite (mot 1 0x800000, avec Crimson Maw), phase CAST.
-- Charges 6 (Aura::CalcMaxCharges lit spell_proc, SpellAuras.cpp:909-913). Effets 0-1 (108)
-- exclus du declenchement : la charge tombe une fois par proc.
(804811, 0, 26,        0,  8388608,        0,     16, 0, 1, 0, 0, 3, 0, 0,    0, 6),
-- Mortal Wounds : critiques des sorts et techniques (sans attaques blanches ni ticks).
(504292, 0,  0,        0,        0,        0,  69904, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Everhungry : critiques directs -> hate.
(804599, 0,  0,        0,        0,        0,  69972, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Infected Blood : degats periodiques -> ralentissement.
(805035, 0,  0,        0,        0,        0, 262144, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Fang Over Fang : Bloodfang Bite (mot 1 0x800000) -> remise a zero de Reave, pc 20, phase CAST.
(504116, 0, 26,        0,  8388608,        0,     16, 0, 1, 0, 0, 0, 0, 0,    0, 0),
-- Agonizing Pain : degats d'Atherann's Anguish (680681, mot 0 0x4), lance en TRIGGERED_FULL_MASK
-- (AscensionBloodmageEvents.cpp) : PROC_ATTR_TRIGGERED_CAN_PROC (2).
(680745, 0, 26,        4,        0,        0,  65536, 1, 2, 0, 2, 0, 0, 0,    0, 0),
-- Saturating Sutures : degats et soins periodiques, pc 15.
(504098, 0,  0,        0,        0,        0, 262144, 3, 2, 0, 0, 0, 0, 0,    0, 0),
-- Lingering Essence : degats et soins periodiques, pc 50.
(680695, 0,  0,        0,        0,        0, 262144, 3, 2, 0, 0, 0, 0, 0,    0, 0),
-- Flesh Foundry : critiques de degats, de soins et periodiques (504412 vise un allie). Effet 1
-- (220) exclu.
(704633, 0,  0,        0,        0,        0, 349524, 3, 2, 2, 0, 2, 0, 0,    0, 0),
-- Petrified Legions : degats physiques infliges (0x14) ou subis (0x28), pc 10.
(500012, 1,  0,        0,        0,        0,     60, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Blood Parasites : coup et ticks de Taldaram's Torment (mot 2 0x200000) -> 800988 sur la cible.
-- Effet 0 (107) exclu.
(802315, 0, 26,        0,        0,  2097152, 327680, 0, 2, 0, 0, 1, 0, 0,    0, 0),
-- Eyes In The Night (aura de zone 806723) : degats directs SUBIS, et non les attaques blanches
-- infligees de son ProcFlags DBC 0x4. pc 40. Recharge 3000 : celle de l'infobulle du talent 801959
-- (celle de 806723 dit 5 s). Effet 1 (107) exclu.
(806723, 0,  0,        0,        0,        0, 139944, 1, 0, 0, 0, 2, 0, 0, 3000, 0),
-- Bloody Claws : critiques d'attaque blanche -> recharges. Effet 0 (213) exclu.
(572800, 0,  0,        0,        0,        0,      4, 0, 0, 2, 0, 1, 0, 0,    0, 0),
-- Adrenaline Boost : critiques de melee -> 680678, pc 50. Effets 0 et 2 (112) exclus.
(680675, 0,  0,        0,        0,        0,     20, 1, 2, 2, 0, 5, 0, 0,    0, 0),
-- Sovereignty : ticks de Crimson Tide (mot 0 0x4000).
(806049, 0, 26,    16384,        0,        0, 262144, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Crimson Scion : degats directs, pc 10.
(806424, 0,  0,        0,        0,        0,  69972, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Bloodsores : degats periodiques, pc 20. Patron 2026_09_20_00 (Chronomancer), ligne 57.
(805591, 0,  0,        0,        0,        0, 262144, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Coated In Darkness : lancement de Bloodbolt (MAGIC_NEG) ou de Bloodfang Bite (SPELL_MELEE).
(704625, 0, 26,        0,  8519680,        0,  65552, 0, 1, 0, 0, 0, 0, 0,    0, 0),
-- Cruel Intent : lancement de Lunge.
(704653, 0, 26, 16777216,        0,        0,   5120, 0, 1, 0, 0, 0, 0, 0,    0, 0),
-- Ultra Instinct : critiques directs de techniques.
(704654, 0,  0,        0,        0,        0,  69904, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Corrupted Blood : Taldaram's Torment au toucher (la cible est requise ; 532623 porte deja le
-- filtre Humanoide | Bete, TargetCreatureType 65).
(704692, 0, 26,        0,        0,  2097152,  65536, 0, 2, 0, 0, 0, 0, 0,    0, 0),
-- Darkcasting (706247) : « Critical strikes with abilities now spawn a Blood Orb » (critique, degats,
-- phase toucher). 707434 invoque 315303 (section 8), ramasse par npc_ascension_bloodmage_blood_orb
-- (AscensionBloodmageTalents.cpp). Aucune recharge interne : l'infobulle n'en donne pas (decision ouverte).
(706247, 0,  0,        0,        0,        0,  69904, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Bloodcraft : degats de Darkfallen Lament (mot 2 0x1000), coup et ticks. Le seul sort a degats de ce
-- masque est 630874, toujours declenche (680828 -> 630895 -> 630874) : sans PROC_ATTR_TRIGGERED_CAN_PROC
-- (AttributesMask 2), Aura::GetProcEffectMask le rejette (SpellAuras.cpp:2166-2172).
(680733, 0, 26,        0,        0,     4096, 327680, 0, 2, 0, 2, 0, 0, 0,    0, 0),
-- Blood Trails : degats et soins periodiques. Effet 0 (4) exclu.
(804858, 0,  0,        0,        0,        0, 262144, 3, 2, 0, 0, 1, 0, 0,    0, 0),
-- Taldaram's Torment Rage (582767) : chaque tick de Taldaram (mot 2 0x200000) -> 3 Rage. Effet 1
-- (286, critique des ticks) exclu. INERTE tant que 582767 n'est distribue a personne : la ligne
-- ClassSpells {20, 2, 582767} est un fichier commun (AscensionCustomClassData.h), transmise.
(582767, 0, 26,        0,        0,  2097152, 262144, 1, 2, 0, 0, 2, 0, 0,    0, 0),
-- Blood Shards (804849) : degats d'Ombre infliges, directs et periodiques ; l'AuraScript
-- aura_ascension_bloodmage_blood_shards remplace le DUMMY vide 506640 par le compteur 505366.
(804849,32,  0,        0,        0,        0, 332116, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Blood Shards (807787, passif « Level 20 ») : degats de Bloodmoon Blast (mot 1 0x2000). Meme script.
(807787, 0, 26,        0,     8192,        0,  65536, 1, 2, 0, 0, 0, 0, 0,    0, 0),
-- Relentless (704652) : critiques physiques -> 807357 ; l'AuraScript lance aussi 807358.
(704652, 1,  0,        0,        0,        0,  69972, 1, 2, 2, 0, 0, 0, 0,    0, 0),
-- Bite Wound (706654, sur l'ennemi) : attaques blanches SUBIES ; l'AuraScript soigne le lanceur.
(706654, 0,  0,        0,        0,        0,      8, 0, 0, 0, 0, 0, 0, 0,    0, 0),
-- Dark Sigil : critiques de degats directs -> soin (script). Effet 0 (aura de raid) exclu : les
-- membres du raid n'ont que lui.
(560535, 0,  0,        0,        0,        0,  69972, 1, 2, 2, 0, 1, 0, 0,    0, 0),
-- Blood Scent : degats directs sous 35 % -> vol de vie (script). Remplace l'entree par defaut
-- engendree par son ProcFlags DBC 0x4. Effet 0 (107) exclu.
(804223, 0,  0,        0,        0,        0,  69972, 1, 2, 0, 0, 1, 0, 0,    0, 0),
-- Purify Blood (aura de raid 520839) : degats directs des allies -> 573273 (script).
(520839, 0,  0,        0,        0,        0,  69972, 1, 2, 0, 0, 2, 0, 0,    0, 0),
-- Blood Bond (sur l'allie lie) : degats directs subis -> Rage au Bloodmage (script). Effet 1 (23)
-- exclu.
(504627, 0,  0,        0,        0,        0, 139944, 1, 0, 0, 0, 2, 0, 0,    0, 0),
-- Sacrificial Rite : attaquants de melee et a distance -> Cursed Blood 573250, une fois par
-- seconde (« Can only occur once per sec »). Effets 0 et 2 exclus.
(800785, 0,  0,        0,        0,        0,    680, 1, 0, 0, 0, 5, 0, 0, 1000, 0),
-- Infection : attaques blanches -> 707867, dont le TargetAuraState 18 (saignement) fait la
-- condition « a bleeding target ».
(704624, 0,  0,        0,        0,        0,      4, 0, 0, 0, 0, 0, 0, 0,    0, 0);

-- -------------------------------------------------------------------------------------
-- 2. spell_linked_spell : debuffs de zone passifs que rien ne posait. Declencheur ET effet
--    passifs (attr0 0x40) : ils survivent a la mort, hors du cas de P-162.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_linked_spell` WHERE `spell_trigger` IN (707624, 705731) AND `spell_effect` IN (707625, 707375) AND `type` = 2;
INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
(707624, 707625, 2, 'Bloodmage Blood Mark - debuff de zone +3 % degats physiques subis (2026-09-28)'),
(705731, 707375, 2, 'Bloodmage Dark Mark - debuff de zone -3 % chance de toucher (2026-09-28)');

-- -------------------------------------------------------------------------------------
-- 3. spell_bonus_data : les coefficients des infobulles. EffectBonusMultiplier vaut 0 sur tous
--    ces enregistrements et aucune ligne n'existait (5 lignes Bloodmage en tout). Une ligne par
--    id : le repli sur le rang 1 (SpellMgr.cpp:968-981) n'atteint pas 504135/504136, hors chaine.
--    Les termes d'Esprit n'ont pas de colonne : ils sont ajoutes en C++
--    (AscensionBloodmageVitality.cpp).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_bonus_data` WHERE `entry` IN (
    804685, 806928, 806929, 806930, 806931, 806932, 578304, 578305,
    802310, 504079, 504080, 504081, 504082, 504083, 504084, 504085, 504086,
    520314, 520836, 520837, 520838, 572895, 572896, 572897, 572898, 520315,
    504260, 504813, 504814, 504815, 504816, 504817, 504818, 504819, 504820,
    504275, 504276, 504277, 504278, 504279, 504280, 504281,
    504282, 504129, 504130, 504131, 504132, 504133, 504134, 504135, 504136,
    802313, 501662, 501663, 501664, 501665, 501666, 501667, 501668, 501669, 501670);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(804685, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(806928, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(806929, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(806930, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(806931, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(806932, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(578304, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(578305, 0.45, 0, 0.2, 0, 'Local Bloodmage: Bloodbolt - "$sps*0.45+$AP*0.2"'),
(802310, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504079, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504080, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504081, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504082, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504083, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504084, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504085, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(504086, 0.58, 0, 0.2, 0, 'Local Bloodmage: Sanguine Mend - "$SP*.58+$AP*.2" (Spirit in C++)'),
(520314, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(520836, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(520837, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(520838, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(572895, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(572896, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(572897, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(572898, 0.5, 0, 0, 0, 'Local Bloodmage: Heartbreak - "$sps*0.5" (Spirit in C++)'),
(520315, 0.3, 0, 0, 0, 'Local Bloodmage: Heartbreak heal - "$BH*.3" (Spirit carried by the caster value)'),
(504260, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504813, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504814, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504815, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504816, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504817, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504818, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504819, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504820, 0.85, 0, 0.25, 0, 'Local Bloodmage: Veinburst - "$AP*.25+$sps*0.85"'),
(504275, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504276, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504277, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504278, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504279, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504280, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504281, 0, 0.32, 0, 0, 'Local Bloodmage: Vampyr''s Kiss - "$SP*.32" per tick'),
(504282, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504129, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504130, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504131, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504132, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504133, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504134, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick'),
(504135, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick (hors chaine spell_ranks)'),
(504136, 0, 0.28, 0, 0, 'Local Bloodmage: Crimson Tide - "$SP*.28" per tick (hors chaine spell_ranks)'),
(802313, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice (SpellAuraEffects.cpp:622, Unit.cpp:2172)'),
(501662, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501663, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501664, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501665, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501666, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501667, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501668, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501669, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice'),
(501670, 0.0175, 0, 0, 0, 'Local Bloodmage: Bloodthorns - "$sps*0.035", halved: damage shields pass SpellDamageBonusDone twice');

-- -------------------------------------------------------------------------------------
-- 4. spell_script_names (lue au demarrage seulement). Un id negatif couvre toute la chaine
--    spell_ranks : -801074 (6 rangs de Scarlet Delirium), -800774 (8 rangs de Sanguine Rupture).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
    'aura_ascension_bloodmage_scarlet_delirium', 'spell_ascension_bloodmage_spread_taldarams',
    'aura_ascension_bloodmage_coagulation', 'aura_ascension_bloodmage_night_stalker',
    'aura_ascension_bloodmage_dark_sigil', 'aura_ascension_bloodmage_blood_scent',
    'aura_ascension_bloodmage_purify_blood', 'aura_ascension_bloodmage_blood_bond',
    'aura_ascension_bloodmage_sacrificial_rite', 'aura_ascension_bloodmage_bloodsurge',
    'aura_ascension_bloodmage_blood_shards', 'aura_ascension_bloodmage_relentless',
    'aura_ascension_bloodmage_bite_wound', 'spell_ascension_bloodmage_cursed_ground_gate',
    'spell_ascension_bloodmage_transfusion');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-801074, 'aura_ascension_bloodmage_scarlet_delirium'),
(704647,  'spell_ascension_bloodmage_spread_taldarams'),
(706258,  'aura_ascension_bloodmage_coagulation'),
(807774,  'aura_ascension_bloodmage_night_stalker'),
(560535,  'aura_ascension_bloodmage_dark_sigil'),
(804223,  'aura_ascension_bloodmage_blood_scent'),
(520839,  'aura_ascension_bloodmage_purify_blood'),
(504627,  'aura_ascension_bloodmage_blood_bond'),
(800785,  'aura_ascension_bloodmage_sacrificial_rite'),
(553267,  'aura_ascension_bloodmage_bloodsurge'),
(804849,  'aura_ascension_bloodmage_blood_shards'),
(807787,  'aura_ascension_bloodmage_blood_shards'),
(704652,  'aura_ascension_bloodmage_relentless'),
(706654,  'aura_ascension_bloodmage_bite_wound'),
(-800774, 'spell_ascension_bloodmage_cursed_ground_gate'),
(705734,  'spell_ascension_bloodmage_transfusion');

-- -------------------------------------------------------------------------------------
-- 5. spell_linked_spell : la puissance d'attaque d'Accursed Form (« $804699s2% of your Agility »). Type 2 :
--    posee et retiree avec la forme, y compris a la restauration depuis character_aura (SpellAuras.cpp:
--    1255-1292). 562572 n'est pas passif et se perd a la mort : hors du cas de P-162. Le contrat C++
--    (AscensionBloodmageTalents.cpp) desarme l'effet 0 de 804699, qui poserait sinon le -25 % de GCD de
--    Dark Frenzy sans le talent : les deux vont ensemble.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_linked_spell` WHERE `spell_trigger` = 562572 AND `spell_effect` = 804699 AND `type` = 2;
INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
(562572, 804699, 2, 'Bloodmage Accursed Form - PA = 50 % Agilite (804699 e1) (2026-09-28)');

-- -------------------------------------------------------------------------------------
-- 6. spell_bonus_data : les coefficients des infobulles. EffectBonusMultiplier vaut 0 sur ces
--    enregistrements et aucune ligne n'existait (mesure le 2026-09-28). Une ligne de tete suffit pour
--    une chaine spell_ranks complete (repli sur le rang 1, SpellMgr.cpp:968-981).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_bonus_data` WHERE `entry` IN (500123, 800156, 500169, 500124, 804197, 500126, 800781, 681036, 520312);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(500123, 0.2,    0,    0.2,   0,    'Local Bloodmage: Ravenous Strike - "$AP*0.2+$SP*.2" (10 ranks)'),
(800156, 0.265,  0,    0.265, 0,    'Local Bloodmage: Bloodfang Bite - "$AP*0.265+$SP*.265" (10 ranks)'),
(500169, 0.2,    0,    0.2,   0,    'Local Bloodmage: Claw Sweep - "$AP*.2+$SP*0.2" (8 ranks)'),
(500124, 0,      0,    0.285, 0,    'Local Bloodmage: Night Hunter''s Howl - "$AP*0.285" (5 ranks)'),
(804197, 0,      0,    0.11,  0.04, 'Local Bloodmage: Rotclaw - "$AP*0.11" direct, "$AP*0.04" per tick (12 ranks)'),
(500126, 0,      0,    0.2,   0,    'Local Bloodmage: Lunge - "$AP*0.2"'),
(800781, 0.35,   0,    0,     0,    'Local Bloodmage: Dark Liturgy - "$SP*0.35" (Spirit in C++, 6 ranks)'),
(681036, 0,      0.1,  0,     0,    'Local Bloodmage: Dark Essence heal - "$BH*0.1" per tick (replaces the 1.0 of field 229)'),
(520312, 0.7525, 0,    0,     0,    'Local Bloodmage: Life''s Essence (Blood Feast burst) - "$SP*0.7525"');

-- -------------------------------------------------------------------------------------
-- 7. spell_script_names (lue au demarrage seulement) et creature_template.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
    'aura_ascension_bloodmage_eyes_in_the_night', 'aura_ascension_bloodmage_shadow_vigil',
    'aura_ascension_bloodmage_blood_feast', 'spell_ascension_bloodmage_hemal_excision',
    'aura_ascension_bloodmage_hemal_excision_window', 'spell_ascension_bloodmage_excision',
    'aura_ascension_bloodmage_blood_trail', 'spell_ascension_bloodmage_amalgam_strike');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(806723, 'aura_ascension_bloodmage_eyes_in_the_night'),
(806182, 'aura_ascension_bloodmage_shadow_vigil'),
(706605, 'aura_ascension_bloodmage_blood_feast'),
(803681, 'spell_ascension_bloodmage_hemal_excision'),
(803734, 'aura_ascension_bloodmage_hemal_excision_window'),
(803733, 'spell_ascension_bloodmage_excision'),
(804857, 'aura_ascension_bloodmage_blood_trail'),
(589023, 'spell_ascension_bloodmage_amalgam_strike');

-- Animated Blood rang 3 : l'amalgame (315301) garde son AggressorAI et y ajoute Blood Strike 589023.
UPDATE `creature_template` SET `ScriptName` = 'npc_ascension_bloodmage_amalgam' WHERE `entry` = 315301;

-- -------------------------------------------------------------------------------------
-- 8. creature_template : les deux invocations sans ligne (mesure le 2026-09-28 : 0 ligne pour 315303 et 50069).
--    Recopie de la ligne entiere de 118329 « Blood Orb » (niveau 1, faction 35, sans butin ni script), pour
--    ne dependre d'aucune liste de colonnes. unit_flags 33554434 = NON_ATTACKABLE (0x2) | NOT_SELECTABLE
--    (0x2000000). Modeles lus dans CreatureDisplayInfo.dbc : 64748 = creature_goo_orb_bloodtroll.mdx (celui
--    de 118329), 1954 = FelBat.mdx, peau BatSkin01 (celui de 16173 « Shadowbat »).
-- -------------------------------------------------------------------------------------
DELETE FROM `creature_template_model` WHERE `CreatureID` IN (315303, 50069);
DELETE FROM `creature_template` WHERE `entry` IN (315303, 50069);

DROP TEMPORARY TABLE IF EXISTS `t_bm_orb`;
CREATE TEMPORARY TABLE `t_bm_orb` SELECT * FROM `creature_template` WHERE `entry` = 118329;
-- Blood Orb de Darkcasting (707434, SummonProperties 61) : immobile, ramasse par son seul invocateur.
UPDATE `t_bm_orb` SET `entry` = 315303, `name` = 'Blood Orb', `type` = 10, `unit_flags` = 33554434,
    `AIName` = '', `ScriptName` = 'npc_ascension_bloodmage_blood_orb';
INSERT INTO `creature_template` SELECT * FROM `t_bm_orb`;
-- Shadow Bat de Call of the Darkwing (801958, 802578) : visuel seul, la morsure et le soin sont du C++.
UPDATE `t_bm_orb` SET `entry` = 50069, `name` = 'Shadow Bat', `type` = 1, `unit_flags` = 33554434,
    `AIName` = 'NullCreatureAI', `ScriptName` = '';
INSERT INTO `creature_template` SELECT * FROM `t_bm_orb`;
DROP TEMPORARY TABLE `t_bm_orb`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(315303, 0, 64748, 1, 1, NULL),
(50069,  0, 1954,  1, 1, NULL);

-- -------------------------------------------------------------------------------------
-- 9. gameobject_template : Blood Covenant (1200012, palier 40), effet 50 TRANS_DOOR vers 194111, absent
--    (mesure le 2026-09-28 : le sort prenait 90 % de la vie et ne posait rien). Recopie de 194108
--    « Summoning Portal » (type 18, 3 participants, sort 62330), celui du Ritual of Summoning. Apparence :
--    displayId 7358 de 194108, garde faute d'une rune de sang identifiee dans GameObjectDisplayInfo.dbc.
-- -------------------------------------------------------------------------------------
DELETE FROM `gameobject_template_addon` WHERE `entry` = 194111;
DELETE FROM `gameobject_template` WHERE `entry` = 194111;

DROP TEMPORARY TABLE IF EXISTS `t_bm_covenant`;
CREATE TEMPORARY TABLE `t_bm_covenant` SELECT * FROM `gameobject_template` WHERE `entry` = 194108;
UPDATE `t_bm_covenant` SET `entry` = 194111, `name` = 'Blood Covenant';
INSERT INTO `gameobject_template` SELECT * FROM `t_bm_covenant`;
DROP TEMPORARY TABLE `t_bm_covenant`;

DROP TEMPORARY TABLE IF EXISTS `t_bm_covenant_addon`;
CREATE TEMPORARY TABLE `t_bm_covenant_addon` SELECT * FROM `gameobject_template_addon` WHERE `entry` = 194108;
UPDATE `t_bm_covenant_addon` SET `entry` = 194111;
INSERT INTO `gameobject_template_addon` SELECT * FROM `t_bm_covenant_addon`;
DROP TEMPORARY TABLE `t_bm_covenant_addon`;

-- -------------------------------------------------------------------------------------
-- 10. spell_script_names (lue au demarrage seulement).
--    spell_warl_ritual_of_summoning : CheckCast seul (pas en champ de bataille), sans test de famille, la
--    garde du Ritual of Summoning 698 dont Blood Covenant reprend le portail.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
    'spell_ascension_bloodmage_call_of_the_darkwing', 'spell_ascension_bloodmage_bat_bite',
    'spell_ascension_bloodmage_blood_rituals_heal');
DELETE FROM `spell_script_names` WHERE `spell_id` = 1200012 AND `ScriptName` = 'spell_warl_ritual_of_summoning';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(801958,  'spell_ascension_bloodmage_call_of_the_darkwing'),
(802578,  'spell_ascension_bloodmage_call_of_the_darkwing'),
(802358,  'spell_ascension_bloodmage_bat_bite'),
(704119,  'spell_ascension_bloodmage_blood_rituals_heal'),
(1200012, 'spell_warl_ritual_of_summoning');

-- -------------------------------------------------------------------------------------
-- 11. spell_bonus_data : les coefficients des infobulles. EffectBonusMultiplier vaut 0 sur ces
--    enregistrements et aucune ligne n'existait (mesure le 2026-09-28).
--    806502 : un seul enregistrement pour les 7 rangs d'Aortic Assault (aura 227 vers 806502).
--    806214 : 0.5 est le texte du talent 704115 ; le bouton d'Aortic Assault dit 0.6 (a trancher).
--    524180 : DmgClass 0 ; sans ligne, SpellHealingBonusDone rendait le soin brut (Unit.cpp).
--    704119 : le terme d'Endurance « $STA*0.085 » est en C++ (AscensionBloodmageVitality.cpp).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_bonus_data` WHERE `entry` IN (806502, 806214, 524180, 704119, 802358);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(806502, 0,    0, 0.12, 0, 'Local Bloodmage: Aortic Assault strike - "$AP*0.12" (7 ranks share 806502)'),
(806214, 0,    0, 0.5,  0, 'Local Bloodmage: Clotting cone - "$AP*0.5" (704115), added after $scalingbp'),
(524180, 0.1,  0, 0,    0, 'Local Bloodmage: Darkfallen Lament heal - "$bh*0.1" per tick'),
(704119, 0.07, 0, 0,    0, 'Local Bloodmage: Blood Rituals heal - "$BH*0.07" (706624; Stamina in C++)'),
(802358, 0.4,  0, 0.2,  0, 'Local Bloodmage: Bat Bite - "$SP*.4+$AP*.2" (801958)');
