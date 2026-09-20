-- =====================================================================================
-- mod-ascension-compat — Chronomancer : moitié DONNÉE du câblage des talents cassés
-- (src/AscensionChronomancerEvents.cpp est l'autre moitié).
--
-- Le nom de ce fichier est la clé d'update enregistrée dans `updates` (state = MODULE) ;
-- NE PAS LE RENOMMER une fois appliqué.
--
-- Gabarits : 2026_09_20_00_ascension_chronomancer_anomaly_spikes.sql (P-045) et
--            2026_09_20_02_ascension_chronomancer_displacement_script.sql (P-051).
--
-- PRISE D'EFFET
--   `spell_script_names` : REDÉMARRAGE OBLIGATOIRE. ObjectMgr::LoadSpellScriptNames()
--       n'a qu'un appelant (World.cpp) et l'accrochage ne tourne qu'une fois.
--   `spell_proc`         : à chaud avec `.reload spell_proc`.
--   `spell_bonus_data`   : à chaud avec `.reload spell_bonus_data`.
--
-- VÉRIFIÉ EN BASE AVANT ÉCRITURE (2026-09-20) : aucun des sorts touchés ici n'a de ligne
-- `spell_script_names`, `spell_proc`, `spell_linked_spell` ni `spell_bonus_data`. Les
-- deux correctifs Chronomancer du jour portent sur 503825/504886 et 806727, disjoints.
--
-- Chaque DELETE porte sur ABS() : un identifiant négatif laissé par une version
-- antérieure désigne le même sort (ou toute sa chaîne de rangs) et doublerait la
-- liaison au lieu de la remplacer.
-- =====================================================================================


-- -------------------------------------------------------------------------------------
-- 1. spell_script_names — les quatre scripts de AscensionChronomancerEvents.cpp
--
-- Rappel P-051 : `RegisterSpellScript` ne dit jamais SUR QUOI un script s'applique.
-- Sans ces lignes, les quatre scripts se chargent, ne s'accrochent à rien, et le cœur
-- se contente d'une ligne « is not assigned in the database » au démarrage.
--
-- 706083 « The Vast Infinite » : effet 0 = aura 69 SCHOOL_ABSORB (25, MiscValue 127,
--        MiscValueB 100), effet 1 = aura 4 DUMMY. Les deux index attendus par le script
--        correspondent au DBC, donc aucun handler ne sera écarté au chargement.
-- 804441 « Timeguard » : effet 0 = aura 69, effets 1 et 2 = aura 4 DUMMY (35 et 50),
--        ProcCharges 3. Le script s'accroche à l'effet 0 uniquement.
-- 800857 + 501772..501778 « Accelerated Recovery », les 8 rangs (`spell_ranks` les
--        chaîne bien, vérifié le 2026-09-20 : 800857 -> 501772..501778, 8 lignes) :
--        effet 0 = aura 8 PERIODIC_HEAL sur les huit. Une ligne par rang pour la
--        lisibilité ; un -800857 aurait marché aussi. ObjectMgr::LoadSpellScriptNames
--        boucle sur GetNextRankSpell() quand l'identifiant est négatif et accroche TOUS
--        les rangs, pas seulement le premier (ObjectMgr.cpp, branche `if (allRanks)`).
-- 801304 et 803382 « Hasten » : le script ne pose que DoCheckProc/OnProc, sans index
--        d'effet, donc il s'accroche aux deux lignes quelle que soit leur forme. Le
--        script lui-même refuse de procer sur 803382 quand 801304 du même lanceur est
--        présent, pour qu'un allié qui porterait les deux ne proque pas deux fois.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_script_names` WHERE ABS(`spell_id`) IN
    (706083, 804441, 800857, 501772, 501773, 501774, 501775, 501776, 501777, 501778, 801304, 803382)
    AND `ScriptName` IN ('aura_ascension_vast_infinite', 'aura_ascension_timeguard',
                         'aura_ascension_accelerated_recovery', 'aura_ascension_hasten_strikes');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(706083, 'aura_ascension_vast_infinite'),
(804441, 'aura_ascension_timeguard'),
(800857, 'aura_ascension_accelerated_recovery'),
(501772, 'aura_ascension_accelerated_recovery'),
(501773, 'aura_ascension_accelerated_recovery'),
(501774, 'aura_ascension_accelerated_recovery'),
(501775, 'aura_ascension_accelerated_recovery'),
(501776, 'aura_ascension_accelerated_recovery'),
(501777, 'aura_ascension_accelerated_recovery'),
(501778, 'aura_ascension_accelerated_recovery'),
(801304, 'aura_ascension_hasten_strikes'),
(803382, 'aura_ascension_hasten_strikes');


-- -------------------------------------------------------------------------------------
-- 2. spell_proc — « Unmaker of Realities » (706107)
--
-- Le talent dit : « While Hasten is active the ally now has a $803382h% chance when they
-- deal damage to strike the target an additional time equal to $803382s1% of the damage
-- dealt. » Les deux nombres sont LUS dans Spell.dbc de 803382 : ProcChance (champ 35) =
-- 25, effet 0 BasePoints 29 + DieSides 1 = 30.
--
-- Pourquoi une ligne est indispensable — P-045 : le champ ProcFlags (34) vaut 0 pour
-- 801304 comme pour 803382. SpellMgr::LoadSpellProcs (« Skip if no proc flags in DBC »)
-- n'engendre alors aucune SpellProcEntry, et Aura::GetProcEffectMask refuse toute aura
-- sans entrée. Aucun crochet OnProc ne peut être appelé.
--
-- S'y ajoute P-053 : l'effet 0 de 803382 porte l'aura privée 354, dont l'entrée dans
-- AuraEffectHandler[] (SpellAuraEffects.cpp) est `nullptr`. Même posée, elle ne fait
-- rien du tout, et son TriggerSpell 803706 n'est jamais tiré.
--
-- Valeur de chaque colonne :
--   ProcFlags = 69972 = 0x11154, « l'allié inflige des dégâts directs » :
--        0x00000004 DONE_MELEE_AUTO_ATTACK        0x00000010 DONE_SPELL_MELEE_DMG_CLASS
--        0x00000040 DONE_RANGED_AUTO_ATTACK       0x00000100 DONE_SPELL_RANGED_DMG_CLASS
--        0x00001000 DONE_SPELL_NONE_DMG_CLASS_NEG 0x00010000 DONE_SPELL_MAGIC_DMG_CLASS_NEG
--        DONE_PERIODIC est volontairement absent : « strike the target an additional
--        time » décrit un coup, pas un tick. DONE_MAINHAND/OFFHAND sont absents aussi :
--        ils se lèvent EN PLUS de l'auto-attaque et feraient procer deux fois le même coup.
--   SpellTypeMask  = 1 PROC_SPELL_TYPE_DAMAGE. SpellMgr.cpp:931 ne consulte cette colonne
--        que pour les événements de SPELL_PROC_FLAG_MASK : l'auto-attaque de mêlée
--        (0x4) n'en fait pas partie et n'est donc pas filtrée ici.
--   SpellPhaseMask = 2 PROC_SPELL_PHASE_HIT. OBLIGATOIRE : quatre des drapeaux retenus
--        appartiennent à REQ_SPELL_PHASE_PROC_FLAG_MASK (SpellMgr.h:184) et
--        SpellMgr.cpp:2143 journalise « proc will not be triggered » si la colonne est 0.
--   HitMask   = 0, soit la valeur par défaut d'un proc DONE : NORMAL | CRITICAL | ABSORB.
--   Chance    = 25, écrite et non déduite. Elle ne peut PAS être laissée à 0 : le cœur
--        retomberait sur le ProcChance du DBC, qui vaut 101 pour 801304 (le marqueur
--        « pas de chance définie ») et non 25. C'est 803382 qui porte le 25, et c'est
--        lui que l'infobulle du talent nomme ($803382h).
--   Cooldown  = 0 : aucune infobulle n'annonce de délai interne.
--   Charges   = 0 : ProcCharges vaut 0 au DBC pour les deux, l'aura n'est pas consommée.
--   SpellFamilyName / masques = 0 : aucune restriction sur le sort déclencheur, le
--        talent parle de « dégâts » sans nommer de sort.
--   DisableEffectsMask = 0, et c'est DÉLIBÉRÉ malgré P-052 : le script utilise les
--        crochets d'aura entiers (DoCheckProc/OnProc) et appelle PreventDefaultAction(),
--        ce qui coupe toute la passe par effet (Aura::TriggerProcOnEvent). Désactiver un
--        effet ici l'empêcherait au contraire d'atteindre le script.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN (801304, 803382);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(801304, 0, 0, 0, 0, 0, 69972, 1, 2, 0, 0, 0, 0, 25, 0, 0),
(803382, 0, 0, 0, 0, 0, 69972, 1, 2, 0, 0, 0, 0, 25, 0, 0);


-- -------------------------------------------------------------------------------------
-- 3. spell_bonus_data — « Gravity Bomb » (801281), l'explosion ne montait avec rien
--
-- Description de 801281, mot pour mot :
--   « After $d they explode for ${($801282m1+$801282ppl1+$SP*1.2+$RAP*0.3)} Chromatic
--     Damage in a $801254a1 yds radius. »
--
-- MESURÉ : 801282 porte EffectBonusMultiplier = 0.0 (champs 229-231 de Spell.dbc, mise
-- en page lue dans src/server/shared/DataStores/DBCStructure.h) et n'avait aucune ligne
-- `spell_bonus_data`. Unit::SpellDamageBonusDone (Unit.cpp:9243) part de
-- `coeff = Effects[i].BonusMultiplier`, et n'ajoute la puissance des sorts que
-- `if (coeff && DoneAdvertisedBenefit)`. Avec coeff nul, l'explosion ne rendait QUE sa
-- valeur plate (BasePoints 966, RealPointsPerLevel 6.0) : zéro puissance des sorts,
-- zéro puissance d'attaque, à tous les niveaux. C'est la panne du talent.
--
-- direct_bonus = 1.2 et ap_bonus = 0.3 sont les deux coefficients ÉCRITS ci-dessus.
--
-- ÉCART ASSUMÉ ET MESURÉ, pas une approximation silencieuse : l'infobulle dit $RAP
-- (puissance d'attaque à distance). Unit.cpp:9262 ne choisit RANGED_ATTACK que si
-- `UseRangedAttackPowerForDamage` ou `IsRangedWeaponSpell() && DmgClass != MELEE`. Or
-- 801282 a DmgClass = 1 (MAGIC) et aucun de ces drapeaux : le cœur prendra donc la
-- puissance d'attaque de mêlée. Pour un Chronomancer les deux sont proches de zéro, le
-- terme dominant reste 1.2 × puissance des sorts.
--
-- RISQUE MESURÉ, ET INERTE À CE JOUR — À RELIRE AVANT TOUTE DISTRIBUTION DES RANGS 2-6
--
-- 801282 est aussi le sort déclenché par 501820..501824 (« Gravity Bomb » rangs 2 à 6,
-- famille 28) : mesuré au DBC, les six identifiants 801281 et 501820..501824 portent tous
-- EffectTriggerSpell = 801282 sur leur effet 0 (aura 23). La description des rangs 2 à 6
-- ne promet que « $s3 Shadow damage » (effet 3, aura 3 PERIODIC_DAMAGE) : pour eux
-- l'explosion 801282 est un dégât de zone EN PLUS, qui gagnera donc aussi le coefficient.
--
-- Cette ligne est néanmoins conservée, parce que le risque ne porte aujourd'hui sur
-- personne. VÉRIFIÉ EN BASE LE 2026-09-20, les rangs 2 à 6 ne sont distribués nulle part :
--   ascension_custom_class_spell : 0 ligne pour 801281 et 501820..501824
--        (la seule ligne Chronomancer du lot est class 22, spell_id 800857, niveau 6) ;
--   npc_trainer                  : 0 ligne pour ces six identifiants ;
--   spell_ranks                  : 0 ligne, ni comme `spell_id` ni comme `first_spell_id`.
--
-- CONDITION DE REPRISE, explicite : le jour où 501820..501824 sont distribués (ligne dans
-- `ascension_custom_class_spell`, `npc_trainer` ou `playercreateinfo_spell_custom`), cette
-- ligne `spell_bonus_data` doit être RETIRÉE et remplacée par un contrat C++ ciblé. Ce
-- fichier ne peut pas faire la distinction par la donnée : `spell_bonus_data` est indexée
-- par le sort déclenché (801282), qui est commun aux six rangs, et `spell_ranks` ne les
-- chaîne pas. Le contrat C++ ne peut pas non plus passer par
-- SpellInfo::Effects[0].BonusMultiplier de 801281 : le porteur du dégât est 801282. Il
-- faudrait un SpellScript sur 801282 lisant le sort déclencheur.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_bonus_data` WHERE `entry` = 801282;
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(801282, 1.2, 0, 0.3, 0, 'Chronomancer - Gravity Bomb explosion, coefficients de la description de 801281');


-- -------------------------------------------------------------------------------------
-- 4. spell_bonus_data — « Accelerated Recovery » (800857 + 7 rangs)
--
-- BLOC SÉPARABLE. Il ne répare pas un talent de la liste : il rend utile la PREMIÈRE
-- moitié de « Rapid Acceleration » (570149). Si l'arbitrage d'équilibrage ne convient
-- pas, supprimer ce seul bloc ne casse rien d'autre.
--
-- Chaîne du raisonnement, entièrement mesurée :
--   a) 570149 effet 0 = aura 108 ADD_PCT_MODIFIER, +15, MiscValue 40, masque de classe
--      [0, 0x10000000, 0]. 800857 porte SpellFamilyFlags [0, 0x10000000, 0] : le
--      modificateur vise bien Accelerated Recovery, et rien d'autre de la famille 28.
--   b) MiscValue 40 est hors de portée (MAX_SPELLMOD = 32) :
--      AuraEffect::CalculateSpellMod le refuse avec une ligne de debug. Le C++ de ce lot
--      le ramène à SPELLMOD_BONUS_MULTIPLIER, qui est le coefficient de puissance de
--      soin dans Unit::SpellHealingBonusDone.
--   c) Mais ce coefficient vaut 0 pour les huit rangs : EffectBonusMultiplier = 0.0 au
--      DBC et aucune ligne ici. +15% de zéro reste zéro : sans (c), le remappage (b) ne
--      produit toujours rien.
--
-- dot_bonus = 0.23 est ÉCRIT dans la description des huit rangs, à l'identique :
--   « healing the target for ${(($d*1000/$T1)*($m1+0+$bh*0.23))*(...)} over $d »
-- soit, par tick, base + 0.23 × soin bonus. C'est exactement la définition de
-- `dot_bonus` : Unit::SpellHealingBonusDone branche `coeff = bonus->dot_damage` quand
-- damagetype vaut DOT, et AuraEffect::CalculateAmount appelle SpellHealingBonusDone en
-- DOT une fois pour le montant d'UN tick.
-- (Correspondance colonne -> champ lue dans SpellMgr::LoadSpellBonuses : la colonne
--  s'appelle `dot_bonus`, le champ C++ `dot_damage`.)
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_bonus_data` WHERE `entry` IN
    (800857, 501772, 501773, 501774, 501775, 501776, 501777, 501778);
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
(800857, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 1, coefficient de sa propre description'),
(501772, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 2'),
(501773, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 3'),
(501774, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 4'),
(501775, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 5'),
(501776, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 6'),
(501777, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 7'),
(501778, 0, 0.23, 0, 0, 'Chronomancer - Accelerated Recovery rang 8');


-- -------------------------------------------------------------------------------------
-- CE QUE CE LOT NE RÉPARE PAS, ET POURQUOI — à lire avant de croire le talent réparé
--
-- 707555 « Slipstream » : ÉCARTÉ. Le bannissement lui-même fonctionne nativement
--   (effet 0 aura 12 MOD_STUN, effet 1 aura 39 SCHOOL_IMMUNITY MiscValue 127). Ce qui
--   manque est la seconde activation : « Activate this ability again before the duration
--   expires to choose a new location to place that enemy within 20 yds. » Le DBC nomme
--   toute la machinerie prévue — 707565 « Slipstream / Debuff », 707567 « Slipstream /
--   Allower » (infobulle : « You may now pick the location to place your target! ») et
--   707566 « Slipstream / Summoner », qui est un sort à destination au sol (cible 87
--   TARGET_DEST_DEST) invoquant la créature 52119 comme marqueur. La réparation exige
--   donc de donner 707566 à la barre d'action du joueur au bon moment, ce qu'aucune de
--   ces deux moitiés ne peut faire. Rien n'a été inventé à la place.
--
-- 801281 « Gravity Bomb », effet 1 : « The initial target takes $s2% increased damage
--   from you during the delay. » BasePoints vaut -1 et DieSides 1, donc CalcValue = 0 :
--   le pourcentage n'est écrit NULLE PART. Les rangs 501820..501824 portent au même
--   endroit l'aura 271 (MOD_DAMAGE_PERCENT_FROM_CASTER) là où 801281 n'a qu'un DUMMY,
--   mais eux aussi avec BasePoints -1. Aucun chiffre n'a été inventé : cette moitié du
--   talent reste morte et c'est volontaire.
-- =====================================================================================
