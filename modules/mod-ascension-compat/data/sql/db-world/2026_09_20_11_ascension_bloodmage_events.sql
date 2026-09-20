-- =====================================================================================
-- mod-ascension-compat — BLOODMAGE (CLASS_SON_OF_ARUGAL = 20, SpellFamilyName 26 = id de
-- classe + 6, lu au champ 208 de Spell.dbc ; le champ 149 vaut 0 pour TOUS les sorts du
-- fichier et n'est pas la famille).
--
-- Accompagne `src/AscensionBloodmageEvents.cpp`, qui câble neuf talents. Les quinze lignes
-- ci-dessous enregistrent les sept scripts de sort/aura de ce fichier. Les trois autres
-- crochets (UnitScript `bloodmage_damage_events`, UnitScript `bloodmage_thick_pelt_scaling`,
-- AllSpellScript `bloodmage_dark_essence_casts`, GlobalScript `bloodmage_event_contracts`)
-- ne se déclarent PAS ici : ces types de scripts s'enregistrent au démarrage sans passer
-- par `spell_script_names`.
--
-- ⚠ CE FICHIER EXIGE LE BINAIRE QUI PORTE CES SCRIPTS. Appliqué avant, le serveur signale
--   au démarrage « ScriptName ... does not exist » et les lignes ne servent à rien.
--   L'ordre est : construire, installer, PUIS appliquer ce fichier.
--
-- Rappel P-051 : un script enregistré en C++ mais absent de `spell_script_names` ne tourne
-- JAMAIS, et ne coûte qu'une ligne de log. Ces lignes sont la moitié manquante du
-- correctif, pas un accessoire.
--
-- Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud : `.reload spell_script_names`. La passe de métadonnées
-- (`bloodmage_event_contracts`), elle, ne se recharge qu'au redémarrage du worldserver.
--
-- -------------------------------------------------------------------------------------
-- UN INDEX DE CHAMP QUI MÉRITE D'ÊTRE ÉCRIT NOIR SUR BLANC
--
-- EffectRadiusIndex de Spell.dbc est le champ 92+e, PAS 104+e (104+e = EffectChainTarget).
-- Témoins relus le 2026-09-20 : Arcane Explosion 1449 a champ92 = 13, et l'entrée 13 de
-- SpellRadius.dbc vaut 10 yards ; Chain Lightning 421 a champ92 = 0 et champ104 = 3, ses
-- trois rebonds. De même EffectBonusMultiplier est le champ 229+e et EffectDamageMultiplier
-- le champ 216+e : Chain Lightning lit 0.571 en 229 (son coefficient) et 0.7 en 216 (la
-- décote par rebond). Une première passe de ce travail a lu le rayon en 104+e, en a conclu
-- que Clotting et Dark Essence n'avaient aucune zone, et allait les écarter tous les deux.
-- Ils sont câblés : ils en ont une.
--
-- -------------------------------------------------------------------------------------
-- RELEVÉ DU 2026-09-20, en lecture seule, AVANT toute écriture.
--
--   680680 « Atherann's Anguish », famille 26, école 32 (Ombre), DurationIndex 1 = 10000 ms,
--          ProcFlags 0, StackAmount 0. Les trois effets visent TARGET_UNIT_DEST_AREA_ENEMY
--          (16) ; rayon 8 yards.
--            Effet 0  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 680681
--            Effet 1  APPLY_AURA / aura 69,      MiscValue 127, MiscValueB 30, montant 1
--            Effet 2  APPLY_AURA / aura 4 DUMMY
--   680681 « Atherann's Anguish / Damage », un SPELL_EFFECT_SCHOOL_DAMAGE de valeur de base
--          1 (un porteur de montant calculé), cible 6, EffectBonusMultiplier 0.25.
--   681403 « Infuse », famille 26, école 32, DurationIndex 1 = 10000 ms, ProcFlags 0,
--          cible 6 (mono-cible).
--            Effet 0  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 681404
--            Effet 1  APPLY_AURA / aura 69,      MiscValue 127, MiscValueB 0
--          MiscValueB VAUT ZÉRO : contrairement à Atherann's Anguish (30) et à Genesis (50),
--          ce sort n'écrit AUCUN pourcentage, et sa description n'en cite aucun. Le script
--          libère donc « the stored amount », soit 100 %. L'infobulle parle de « diminishing
--          rates » sans le chiffrer : aucun taux dégressif n'est appliqué, faute de source.
--   681404 « Infuse / Damage », un SPELL_EFFECT_SCHOOL_DAMAGE, cible 16 avec
--          EffectRadiusIndex 13 = 10 yards : la libération éclabousse autour de la cible
--          marquée. EffectBonusMultiplier 0.15.
--   570023 « Thirst for Blood », famille 26, passif, ProcFlags 0.
--            Effet 0  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 570025 (« Ravenous »)
--            Effet 1  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 570024 (« Sated »)
--          570024 = SPELL_AURA_HASTE_SPELLS 10 ; 570025 = SPELL_AURA_MOD_CRIT_DAMAGE_BONUS
--          100 sur MiscValue 126, et son effet 1 est un SPELL_EFFECT_REMOVE_AURA sur 570024.
--          Les deux existent et fonctionnent seuls ; RIEN ne les accordait jamais.
--   706613 « Thirst », famille 26, StackAmount 10 : la ressource de classe. Ligne
--          `ResourceDisplays` {20, 706613, 10} de src/AscensionCustomResourceData.h.
--   560259 « Vampyr Lord », famille 26, passif.
--            Effet 0  Effect 190 SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS,
--                     aura 79 MOD_DAMAGE_PERCENT_DONE, montant 35, MiscValue 127
--            Effet 1  APPLY_AURA / aura 4 DUMMY
--          L'effet 190 EST implémenté par ce cœur (SpellEffects.cpp -> EffectApplyAreaAura,
--          et UnitAura::FillTargetMap lui donne sa propre branche). Le talent n'est donc pas
--          muet : il est trop large, il touche TOUS les serviteurs. Le correctif est un
--          filtre de cible, pas un bonus — en ajouter un doublerait le talent.
--   706683 « Festering Maw », famille 26, passif.
--            Effet 0  APPLY_AURA / aura 42 PROC_TRIGGER_SPELL, EffectTriggerSpell 706718
--          706718 porte un SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN (165), MiscValue 500126
--          (« Lunge »), valeur de base -1000 : la seconde de l'infobulle. Cet effet a un vrai
--          handler (Spell::EffectAscensionModifyCooldown -> ModifyAscensionCooldown).
--   704115 « Clotting », famille 26, passif.
--            Effet 0  APPLY_AURA / aura 4 DUMMY
--            Effet 1  APPLY_AURA / aura 112 OVERRIDE_CLASS_SCRIPTS, montant 25,
--                     MiscValue 20007 (un id de class script inconnu du cœur)
--          806212 « Aortic Assault / Rank 1 » et ses six montées en rang 807780..807785
--          (AscensionSpellProgressionData.h, niveaux 24/32/40/48/56/60) portent TOUS le même
--          effet 2 : APPLY_AURA / aura 23 PERIODIC_TRIGGER_SPELL, amplitude 3000, vers
--          806214, sur une aura de 3000 ms — le cône se déclenche donc déjà, mais SANS
--          vérifier que le joueur a le talent, alors que la description l'écrit
--          « $?s704115[ ... ][] ».
--          806214 « Clotting / Damage » : TARGET_UNIT_CONE_ENEMY_54, EffectRadiusIndex 13
--          = 10 yards, base 600, déjà normalisée par $scalingbp ({806214, 1} dans
--          src/AscensionScalingBaseData.h).
--   680732 « Dark Essence », famille 26, passif, un seul effet DUMMY inerte.
--          681036 « Dark Essence / Heal » : aura 8 PERIODIC_HEAL, amplitude 1500, durée
--          3000 ms, TARGET_SRC_CASTER (22) / TARGET_UNIT_SRC_AREA_ALLY (30),
--          EffectRadiusIndex 23 = 40 yards, EffectBonusMultiplier 1.0. Complet — mais rien
--          ne le lançait, et rien ne le restreignait aux alliés marqués par Blood Rituals
--          (706623).
--          « Cursed Form abilities » n'est pas une devinette : chaque capacité de forme
--          conditionne son propre lancer à un CasterAuraSpell, 525031 ou 524861. Relu au
--          champ 24 de Spell.dbc : Lunge 500126 = 525031, Rotclaw 804197 = 524861, Aortic
--          Assault 806212 = 524861. C'est la paire que src/AscensionBloodmageTalents.cpp
--          documente et synchronise déjà.
--
-- Base acore_world, état AVANT ce fichier :
--   `spell_script_names` : AUCUNE ligne pour 680680, 681403, 570023, 560259, 706683, 704115,
--                          680732, 806212, 807780..807785, 806214, 681036, 704626, 556233.
--                          Une seule ligne pour 706613, `aura_ascension_resource_talent_refresh`,
--                          qui reste en place : la clé unique est (spell_id, ScriptName), deux
--                          scripts coexistent sur un même sort et le cœur les appelle tous les
--                          deux.
--   `spell_proc`         : une ligne 704626 (SchoolMask 1, SpellFamilyName 0, SpellFamilyMask0 0,
--                          ProcFlags 40) et une ligne 706683 (SchoolMask 0, SpellFamilyName 26,
--                          SpellFamilyMask0 0, ProcFlags 1024, AttributesMask 2), toutes deux
--                          posées le 2026-09-20 — VÉRIFIÉES, et NON retouchées ici.
--                          Aucune ligne pour 680680, 681403, 570023, 560259, 704115, 680732.
--   `spell_linked_spell` : aucune ligne, ni en déclencheur ni en effet, pour ces sorts.
--
-- Pourquoi AUCUNE ligne `spell_proc` n'est ajoutée :
--   - 680680 / 681403 ne procèdent par aucun proc : la marque accumule depuis
--     UNITHOOK_ON_DAMAGE et se paie à son expiration NATURELLE.
--   - 570023 / 560259 / 704115 / 680732 sont des passifs sans proc.
--   - 704626 et 706683 en ont déjà une (P-045 ne s'applique donc plus à eux). Celle de
--     706683 a SpellFamilyMask 0 : elle accepte AUJOURD'HUI tout sort de famille 26 positif
--     de classe de dégâts « none », donc n'importe quel soin. Le `DoCheckProc` du script
--     `aura_ascension_bloodmage_festering_maw` la resserre sur le seul sort que la
--     description nomme, 556234 « Bite Wound / Heal ». La ligne existante n'est pas touchée :
--     ce fichier n'écrase rien.
--   - P-052 ne mord pas ici : 706683 n'a qu'un seul effet d'aura, il n'y a donc rien à
--     isoler avec `DisableEffectsMask`.
--
-- Ce qui reste NON câblé, et signalé plutôt que deviné :
--   - « $AP*0.5 » de la description de Clotting : rien dans la donnée ne l'exprime
--     (EffectBonusMultiplier de 806214 vaut 0), et l'ajouter dans CalcValue passerait sous le
--     normalisateur $scalingbp qui multiplie déjà les points de base de cet effet.
--   - la seconde ligne de l'infobulle de Dark Essence (« increases target's critical strike
--     chance by $680693s3% ») : 680693 n'a qu'un seul effet, son « s3 » nomme un effet qui
--     n'existe pas, et aucun autre enregistrement ne chiffre cette valeur.
--   - le coefficient d'attaque de Thick Pelt : les deux descriptions se contredisent
--     ($AP*0.25 sur 556233, $AP*0.2 sur 704626). Le script applique celui écrit sur
--     l'enregistrement dont c'est le montant, 556233.
-- =====================================================================================

DELETE FROM `spell_script_names` WHERE (`spell_id`, `ScriptName`) IN (
    (680680, 'aura_ascension_bloodmage_anguish'),
    (681403, 'aura_ascension_bloodmage_infuse'),
    (706613, 'aura_ascension_bloodmage_thirst_tiers'),
    (570023, 'aura_ascension_bloodmage_thirst_for_blood'),
    (560259, 'aura_ascension_bloodmage_vampyr_lord'),
    (706683, 'aura_ascension_bloodmage_festering_maw'),
    (806212, 'aura_ascension_bloodmage_aortic_assault'),
    (807780, 'aura_ascension_bloodmage_aortic_assault'),
    (807781, 'aura_ascension_bloodmage_aortic_assault'),
    (807782, 'aura_ascension_bloodmage_aortic_assault'),
    (807783, 'aura_ascension_bloodmage_aortic_assault'),
    (807784, 'aura_ascension_bloodmage_aortic_assault'),
    (807785, 'aura_ascension_bloodmage_aortic_assault'),
    (806214, 'spell_ascension_bloodmage_clotting'),
    (681036, 'spell_ascension_bloodmage_dark_essence'));

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
-- Atherann's Anguish : la marque accumule les dégâts que le Bloodmage inflige à l'ennemi
-- marqué et libère 30 % du total (MiscValueB de l'effet 1) à l'expiration NATURELLE.
(680680, 'aura_ascension_bloodmage_anguish'),
-- Infuse : même patron, mais la marque accepte aussi les dégâts du groupe/raid du lanceur
-- et de leurs serviteurs, et libère 100 % du total (le record n'écrit aucun pourcentage).
(681403, 'aura_ascension_bloodmage_infuse'),
-- Thirst for Blood, moitié « ressource » : posé sur Thirst (706613), l'AfterEffectApply en
-- AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK est ré-appelé à CHAQUE changement de pile
-- (Aura::SetStackAmount -> AuraEffect::ChangeAmount avec onStackOrReapply = true). Il
-- accorde Sated (1-5) ou Ravenous (6-10) et retire l'autre. Même idiome que
-- `aura_ascension_resource_talent_refresh`, déjà en service sur ce même sort.
(706613, 'aura_ascension_bloodmage_thirst_tiers'),
-- Thirst for Blood, moitié « talent » : le palier doit être recalculé quand le talent est
-- appris, et retiré quand il est perdu (changement de spécialisation) alors que Thirst est
-- déjà debout.
(570023, 'aura_ascension_bloodmage_thirst_for_blood'),
-- Vampyr Lord : DoCheckAreaTarget restreint l'aura de serviteurs aux seules créatures
-- d'Animated Blood (325301, 335301, 315301), le porteur de l'aura restant toujours accepté.
(560259, 'aura_ascension_bloodmage_vampyr_lord'),
-- Festering Maw : DoCheckProc resserre le proc sur 556234 « Bite Wound / Heal », le seul
-- soin que la description nomme.
(706683, 'aura_ascension_bloodmage_festering_maw'),
-- Clotting, moitié « porte » : les sept rangs d'Aortic Assault. Le script empêche le tic
-- périodique de l'effet 2 de déclencher le cône si le joueur n'a pas 704115.
(806212, 'aura_ascension_bloodmage_aortic_assault'),
(807780, 'aura_ascension_bloodmage_aortic_assault'),
(807781, 'aura_ascension_bloodmage_aortic_assault'),
(807782, 'aura_ascension_bloodmage_aortic_assault'),
(807783, 'aura_ascension_bloodmage_aortic_assault'),
(807784, 'aura_ascension_bloodmage_aortic_assault'),
(807785, 'aura_ascension_bloodmage_aortic_assault'),
-- Clotting, moitié « bonus » : +25 % (montant de l'effet 1 de 704115) contre une cible qui
-- saigne, relu sur le talent et jamais écrit en dur.
(806214, 'spell_ascension_bloodmage_clotting'),
-- Dark Essence : restreint la sélection de la zone alliée de 40 yards aux seuls alliés
-- porteurs de la marque Blood Rituals (706623). Le lancement, lui, vient de l'AllSpellScript
-- `bloodmage_dark_essence_casts`, qui ne s'enregistre pas en base.
(681036, 'spell_ascension_bloodmage_dark_essence');
