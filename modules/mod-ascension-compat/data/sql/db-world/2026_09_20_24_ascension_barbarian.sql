-- =====================================================================================
-- mod-ascension-compat — Barbarian (classe 12, famille de sorts 18)
--
-- Deux câblages, chacun appuyé sur une infobulle officielle citée par le tracker amont
-- jealous-sound/azerothcore-wotlk-coa, et sur l'enregistrement DBC lu champ par champ.
--
--   560532 « Skull Smash »        — issue amont #1144, infobulle citée :
--       « 30 Energy | 25 yd range | Instant | 2 min cooldown | Pick up a rock and hurl
--         it at an enemy's skull, disorienting them for 40 sec (8 sec vs players).
--         Damage taken will end this effect. »
--     La description du DBC porte EN PLUS un bloc conditionnel que l'infobulle du
--     tracker ne reprend pas, parce qu'il ne s'affiche qu'avec le talent :
--       « $?s561314[ Removes all bleed effects on the target.][] »
--     Or l'effet 2 du sort est un FORCE_CAST (140) de 560554 « Blood Loss »
--     (DISPEL_MECHANIC, MiscValue 15 = MECHANIC_BLEED) **sans aucune condition** :
--     tout Barbarian nettoie les saignements de sa cible, talent ou pas.
--     Réparation : le script C++ `spell_ascension_barbarian_skull_smash` supprime cet
--     effet quand le lanceur est un Barbarian qui n'a pas l'aura 561314.
--     Un lanceur qui n'est pas un joueur Barbarian garde le comportement natif.
--
--     AVERTISSEMENT — 561314 N'EST POSÉE PAR AUCUN MÉCANISME CONNU DE CE SERVEUR.
--     Mesuré en lecture seule, toutes recherches négatives pour 561314, 806229 et
--     560532 : `ascension_custom_class_spell` (class=12 ne porte que 804765, 500915,
--     500919, 800193, 800943, 801576, 804136, 15590, 804280), `talent_dbc` (table vide,
--     0 ligne), `/opt/coa/server/data/dbc/Talent.dbc` (2 384 lignes balayées, aucune ne
--     porte l'un de ces trois ids), `spell_linked_spell`,
--     `spell_scripts`, `conditions`, ainsi que les sources du cœur et du module.
--     Décisif : sur 140 personnages de classe 12, `character_spell` ne contient aucune
--     ligne pour ces trois sorts. L'attribut PASSIVE du DBC ne prouve RIEN quant à
--     l'apprentissage (P-070).
--     Donc : tant que 560532 n'est pas apprenable, ce garde-fou est inerte. Mais dès que
--     560532 le deviendra sans que 561314 le soit, il SUPPRIMERA le nettoyage de
--     saignements pour TOUS les Barbarians. Établir par quel mécanisme un Barbarian
--     apprend 561314 avant d'appliquer ce fichier.
--
--     À savoir aussi : un SECOND sort de la famille 18 porte le même FORCE_CAST de
--     560554, sans aucune condition — 560552 « Blood Loss » (E0, effet 140,
--     TriggerSpell 560554). Balayage complet de Spell.dbc sur les champs 116+e : seuls
--     560532 et 560552 déclenchent 560554. 560552 est inerte aujourd'hui (référencé
--     nulle part dans le module ni dans le cœur, absent de `spell_script_names`,
--     `spell_linked_spell`, `ascension_custom_class_spell`, connu d'aucun personnage) et
--     ce travail ne le conditionne PAS.
--
--   806229 « Ancestral Evolution » — issue amont #3617 (titrée « Tinker » à tort : le
--     SpellFamilyName du DBC est 18, Barbarian), infobulle citée :
--       « Instant | 3 min cooldown | Honor your ancestor's and gain their blessing,
--         allowing all party and raid members to reflect 20% of all damage taken
--         for 20 sec. »
--     Le DBC concorde exactement, et c'est de lui que viennent les chiffres du code :
--       Effect[0] = 65 APPLY_AREA_AURA_RAID, rayon 12 = 100 yd, aura 4 DUMMY,
--       BasePoints 19 + DieSides 1 = 20 (les 20 %), DurationIndex 18 = 20 000 ms,
--       RecoveryTime 180 000 ms = 3 min.
--     L'aura DUMMY n'a AUCUN effet côté cœur : la capacité s'applique à tout le raid
--     et ne fait rien. Réparation : cette ligne `spell_proc` donne à l'aura l'événement
--     qui lui manque (P-045 : ProcFlags DBC = 0 et pas de ligne => le cœur ne fabrique
--     rien), et le script `aura_ascension_barbarian_event` renvoie la part.
--
-- Vérifié, pas supposé :
--   * `spell_proc` ne contient AUCUNE ligne pour 560532 ni 806229 (SELECT fait avant) ;
--   * `spell_script_names` n'en contient aucune non plus ;
--   * les 16 colonnes ci-dessous sont celles de `SHOW COLUMNS FROM spell_proc`, dans
--     l'ordre, et chaque ligne en porte 16 ;
--   * les valeurs de drapeaux sont lues dans `src/server/game/Spells/SpellMgr.h`
--     (`enum ProcFlags`, `ProcFlagsSpellType`, `ProcFlagsHit`, `ProcAttributes`) ;
--   * aucune boucle de proc possible — mais PAS pour la raison qu'on croit.
--     `Unit::DealDamage` (Unit.cpp:987 à ~1356) ne produit aucun proc de DÉGÂTS, mais
--     elle appelle `Unit::Kill` quand le coup est létal (Unit.cpp:1243), et `Unit::Kill`
--     lève PROC_FLAG_KILL et PROC_FLAG_KILLED (Unit.cpp:14696 et 14702) puis
--     PROC_FLAG_DEATH (Unit.cpp:14706). Ce qui empêche la récursion, c'est que ces trois
--     procs sont émis sans `DamageInfo` : le helper `Damage()` du script est donc faux
--     pour eux. Le `Check` écarte en plus un événement dont le sort serait 806229.
--     Conséquence à connaître : un reflet qui achève sa cible CRÉDITE le kill au porteur
--     de l'aura et déclenche ses propres talents « on kill » — cases 800131 et 712468 du
--     même script — et non au joueur qui combattait réellement la victime.
--   * effet de bord assumé du paiement par `Unit::DealDamage` : Unit.cpp:1037 exécute
--     `RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TAKE_DAMAGE)` sur la cible, et
--     806229 n'a pas SPELL_ATTR4_DAMAGE_DOESNT_BREAK_AURAS (AttributesEx4 = 0 au DBC).
--     Le reflet casse donc sape, métamorphose et le désorientement de Skull Smash
--     lui-même, pour tout le raid et pendant 20 s.
--
-- CE FICHIER N'A PAS ÉTÉ APPLIQUÉ. Lecture seule côté base pendant sa rédaction.
-- =====================================================================================

DELETE FROM `spell_script_names` WHERE `spell_id` IN (560532, 806229);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
-- Le script est enregistré dans AddAscensionBarbarianAbilityScripts() ; sans cette
-- ligne il ne tournerait jamais (P-051).
(560532, 'spell_ascension_barbarian_skull_smash'),
-- Script déjà enregistré et déjà utilisé par 45 autres sorts de la classe
-- (SELECT ScriptName, COUNT(*) FROM spell_script_names GROUP BY ScriptName :
--  aura_ascension_barbarian_event = 45).
(806229, 'aura_ascension_barbarian_event');

DELETE FROM `spell_proc` WHERE `SpellId` = 806229;
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES
-- 806229 — Barbarian / Ancestry, « Ancestral Evolution »              famille 18
--   ProcFlags 664232 = 8 | 32 | 128 | 512 | 8192 | 131072 | 524288
--     = TAKEN_MELEE_AUTO_ATTACK | TAKEN_SPELL_MELEE_DMG_CLASS
--     | TAKEN_RANGED_AUTO_ATTACK | TAKEN_SPELL_RANGED_DMG_CLASS
--     | TAKEN_SPELL_NONE_DMG_CLASS_NEG | TAKEN_SPELL_MAGIC_DMG_CLASS_NEG
--     | TAKEN_PERIODIC.
--     C'est « all damage taken » : mêlée, distance, sorts et périodiques. Valeur
--     reprise telle quelle de 805821, le seul autre talent « quand tu subis des
--     dégâts » de cette classe, pour ne pas inventer un jeu de drapeaux.
--   SchoolMask 0 : l'infobulle dit « all damage », aucune école n'est exclue.
--   SpellFamilyName 0 : la source des dégâts n'appartient à aucune famille donnée.
--   SpellTypeMask 1 = PROC_SPELL_TYPE_DAMAGE — garde-fou de P-045 : un soin reçu
--     lève les mêmes drapeaux « TAKEN ». Les auto-attaques de mêlée ne passent pas
--     par ce test (PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK n'est pas dans
--     SPELL_PROC_FLAG_MASK), elles restent donc couvertes.
--   SpellPhaseMask 0 : REQ_SPELL_PHASE_PROC_FLAG_MASK vaut
--     SPELL_PROC_FLAG_MASK & DONE_HIT_PROC_FLAG_MASK, qui ne contient aucun drapeau
--     « TAKEN » — le poser ferait journaliser une erreur sans rien filtrer.
--   HitMask 9283 = NORMAL | CRITICAL | BLOCK | ABSORB | FULL_BLOCK. Un coup
--     entièrement bloqué ou absorbé arrive avec 0 dégât : le `Check` du script exige
--     GetDamageInfo()->GetDamage(), il n'en sort donc rien.
--   AttributesMask 2 = PROC_ATTR_TRIGGERED_CAN_PROC : « all damage taken » inclut les
--     dégâts d'un sort lui-même déclenché.
--   DisableEffectsMask 0 : le sort n'a qu'un effet, et le script appelle
--     PreventDefaultAction() (P-052 : DUMMY et PROC_TRIGGER_SPELL partagent le même
--     case ; sans cela le cœur tenterait de déclencher le sort 0).
--   Chance 100, Cooldown 0, Charges 0 — ProcChance du DBC = 100, aucune limite
--     n'est écrite ni dans l'infobulle ni dans le DBC.
(806229, 0, 0, 0, 0, 0, 664232, 1, 0, 9283, 2, 0, 0, 100, 0, 0);
