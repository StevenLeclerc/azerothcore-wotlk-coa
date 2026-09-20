-- =====================================================================================
-- mod-ascension-compat — Stormbringer (classe 16, famille de sorts 22)
--   CLASS_STORMBRINGER = 16 (SharedDefines.h:141) ; la classe 14 est CLASS_DEMON_HUNTER
--   (Felsworn, SharedDefines.h:139). Famille = classe + 6 = 22
--   (SpellMgr.cpp:42-45, IsValidSpellProcFamily).
-- Moitié « donnée » de src/AscensionStormbringerEvents.cpp.
--
-- Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud : `.reload spell_proc` puis `.reload spell_script_names`.
--
-- Rappels payés : P-051 (un script enregistré en C++ sans ligne spell_script_names ne
-- tourne jamais) et P-045 (une aura de proc à ProcFlags DBC nul et sans ligne spell_proc
-- ne se déclenche jamais — SpellMgr::LoadSpellProcs, « Skip if no proc flags in DBC »).
--
-- Tous les index Spell.dbc cités ci-dessous ont été relus le 2026-09-20 contre des sorts
-- témoins : 208 SpellFamilyName, 209/210/211 SpellFamilyFlags, 213 DmgClass, 225
-- SchoolMask, 34 ProcFlags, 35 ProcChance, 40 DurationIndex.
--   Témoins de l'index 213 (DmgClass) : Fireball 133 = 1, Shadow Bolt 686 = 1,
--   Mortal Strike 12294 = 2, Ambush 2098 = 2. L'énumération est
--   SPELL_DAMAGE_CLASS_NONE 0, MAGIC 1, MELEE 2, RANGED 3 (SharedDefines.h:1638-1641).
-- =====================================================================================


-- -------------------------------------------------------------------------------------
-- 1. CORRECTION — 705700 « Invigoration », ProcFlags 16 -> 65536
--
-- La ligne posée par 2026_09_20_03_ascension_talents_proc_masque_famille.sql est juste
-- sur tout SAUF ProcFlags. Son commentaire dit « ProcFlags 16 : Aeroblast est DmgClass 1
-- (MELEE) ». DmgClass 1 est MAGIC, pas MELEE (SharedDefines.h:1638-1641) ; 16 est
-- PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS. Le talent est donc resté MORT après cette passe.
--
-- Mesuré dans le coeur : pour un sort DmgClass MAGIC, Spell.cpp:2266-2300 laisse
-- m_procAttacker à 0 (branche `default`, Aeroblast n'est pas une baguette), et la phase
-- CAST recalcule alors Spell.cpp:4018-4030 :
--     procAttacker = IsPositive ? ..._MAGIC_DMG_CLASS_POS : ..._MAGIC_DMG_CLASS_NEG
-- Aeroblast est un sort de dégâts, donc PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG
-- = 0x00010000 = 65536 (SpellMgr.h:134).
--
-- Le reste de la ligne est confirmé et n'est pas touché :
--   masque (8388608,0,0) : le bit FF0 8388608 n'est porté QUE par les dix rangs
--     d'Aeroblast dans la famille 22 (801839 R1, 501450-501458) — balayage complet du
--     Spell.dbc refait aujourd'hui ;
--   SpellPhaseMask 1 (CAST) : une pile par lancement, et le contrôle HitMask de
--     SpellMgr.cpp est sauté en phase CAST tant que HitMask vaut 0 ;
--   DisableEffectsMask 2 : l'effet 1 est une aura 4 DUMMY sans EffectTriggerSpell.
--
-- La chaîne en aval a été relue et n'a besoin de rien : 705700 effet 0 = aura 42
-- PROC_TRIGGER_SPELL -> 681273, dont l'effet 140 SPELL_EFFECT_FORCE_CAST vise la cible 5
-- TARGET_UNIT_PET et fait lancer 680918 par le familier (Spell::EffectForceCast,
-- SpellEffects.cpp:1311-1363). 680918 a déjà son script de durée
-- (spell_ascension_air_invigoration_duration).
-- -------------------------------------------------------------------------------------

UPDATE `spell_proc` SET `ProcFlags` = 65536 WHERE `SpellId` = 705700 AND `ProcFlags` = 16;


-- -------------------------------------------------------------------------------------
-- 2. 804035 « Tailwind » — support du proc de 705715 « Gift of Air », 2e phrase
--
-- « In addition, your critical strikes now extend the duration of your Tailwind by
--   1.5 sec. » (infobulle de 705715).
--
-- Pourquoi la ligne est posée sur Tailwind et non sur le talent : 705715 a déjà une ligne
-- spell_proc, consacrée à sa PREMIÈRE phrase (Kiss of the Clouds -> 804033 sur le
-- familier), et un sort n'a droit qu'à une ligne. Le sort qui porte réellement
-- l'allongement, 583254 (effet 177 SPELL_EFFECT_ASCENSION_MODIFY_AURA_DURATION,
-- MiscValue 804035, BasePoints -1+1500 = +1500 ms), est référencé par DEUX sorts et deux
-- seulement — 706537 « Gift of Air / Passive SLS » et 706123 « Cursed ID - Procs wont work
-- on it », chacun en EffectTriggerSpell[0] (champ 116) d'une aura 42. Balayage complet
-- refait : les 234 champs des 209510 enregistrements de Spell.dbc, la valeur 583254
-- n'apparaît nulle part ailleurs.
-- Aucun des deux ne peut le déclencher : leur ProcFlags DBC (champ 34) vaut 0 et ni l'un
-- ni l'autre n'a de ligne `spell_proc` (vérifié en base), donc SpellMgr::LoadSpellProcs
-- les saute — « Skip if no proc flags in DBC », SpellMgr.cpp:2248, c'est P-045.
-- 706537 est de surcroît absent de Talent.dbc ET de SkillLineAbility.dbc, donc jamais
-- appris par personne ; aucune ligne de Spell.dbc ne cite 706537 dans ses 234 champs.
--
-- ProcFlags 69972 = 0x4 | 0x10 | 0x40 | 0x100 | 0x1000 | 0x10000, soit les six drapeaux
--   DONE de dégâts (SpellMgr.h:113-135) :
--     0x00004 DONE_MELEE_AUTO_ATTACK          0x00010 DONE_SPELL_MELEE_DMG_CLASS
--     0x00040 DONE_RANGED_AUTO_ATTACK         0x00100 DONE_SPELL_RANGED_DMG_CLASS
--     0x01000 DONE_SPELL_NONE_DMG_CLASS_NEG   0x10000 DONE_SPELL_MAGIC_DMG_CLASS_NEG
--   Les drapeaux POS (soins) et PERIODIC sont écartés : l'infobulle dit « critical
--   strikes », pas « critical heals » ni « periodic ».
-- HitMask 2 = PROC_HIT_CRITICAL (SpellMgr.h:258).
-- SpellPhaseMask 2 = PROC_SPELL_PHASE_HIT : le critique n'est connu qu'au contact.
--   CORRIGÉ : contrairement à ce qui était écrit ici, le contrôle de phase ne concerne pas
--   que les sorts. REQ_SPELL_PHASE_PROC_FLAG_MASK = SPELL_PROC_FLAG_MASK &
--   DONE_HIT_PROC_FLAG_MASK (SpellMgr.h:184) ; l'auto-attaque de MÊLÉE (0x4) est bien hors
--   de cette intersection (absente de SPELL_PROC_FLAG_MASK, SpellMgr.h:159-172), mais
--   l'auto-attaque à DISTANCE (0x40) est dans les deux masques, donc dans l'intersection.
--   La ligne fonctionne quand même, pour une autre raison : les procs d'auto-attaque sont
--   émis avec le procPhase par défaut de ProcSkillsAndAuras, PROC_SPELL_PHASE_HIT
--   (Unit.h:1571, valeur 2 ; Unit.cpp:2890 ne l'écrase pas), ce que le SpellPhaseMask 2
--   couvre exactement.
--   La validation de LoadSpellProcs passe, elle, parce que 0x10000 EST dans ce masque.
-- SpellFamilyName 0 : aucune restriction de sort, c'est le C++ qui borne (classe,
--   propriétaire de l'aura, acteur de l'événement).
-- DisableEffectsMask 0, et c'est un choix REVU. Première écriture : 3, pour désactiver
--   les effets 0 et 1 (SPELL_EFFECT_APPLY_AREA_AURA_RAID, auras 192 MOD_MELEE_RANGED_HASTE
--   et 216 HASTE_SPELLS), seuls reçus par les alliés du raid, et couper court à leurs procs.
--   Relecture : ça n'économise RIEN. Dans Aura::GetProcEffectMask (SpellAuras.cpp:2216-2234)
--   CallScriptCheckProcHandlers est appelé AVANT la boucle sur les effets ; le CheckProc du
--   script tourne donc pour les alliés dans les deux cas. Le masque 3 n'apportait qu'une
--   dépendance : si l'effet 2 (APPLY_AURA sur le lanceur, aura 79 MOD_DAMAGE_PERCENT_DONE)
--   n'était pas appliqué, le masque tombait à 0 et le talent mourait EN SILENCE — la famille
--   de pannes que P-045/P-051/P-053 documentent déjà. D'où 0.
--   Aucun risque P-052 : aucune des trois auras (192, 216, 79) ne tombe dans un `case` de
--   AuraEffect::HandleProc (SpellAuraEffects.cpp:1384-1411), et le script appelle de toute
--   façon PreventDefaultAction, ce qui saute toute la boucle d'effets
--   (Aura::TriggerProcOnEvent, SpellAuras.cpp:2323-2339).
-- Charges 0, et ProcCharges DBC de 804035 vaut 0 : Aura::ConsumeProcCharges ne peut pas
--   consommer Tailwind à chaque critique (vérifié champ 36).
-- Chance 100 : ProcChance DBC de 804035 vaut 101, on l'écrit explicitement.
--
-- LIMITE ASSUMÉE, mesurée : ni Spell::EffectAscensionModifyAuraDuration
-- (SpellEffects.cpp:464-482) ni Aura::SetDuration (SpellAuras.cpp:816-826) ne plafonnent
-- la durée. Tailwind dure 15 000 ms (SpellDuration.dbc index 8) et gagne 1 500 ms par
-- critique. Aucune donnée n'écrit de plafond : on n'en invente pas.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_proc` WHERE `SpellId` = 804035;
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(804035, 0, 0, 0, 0, 0, 69972, 0, 2, 2, 0, 0, 0, 100, 0, 0);


-- -------------------------------------------------------------------------------------
-- 3. 801869 « Titanstorm » — ligne explicite, en remplacement de l'entrée générée
--
-- « Casting Call Lightning or Electrocute now reduces the cooldown of Arm of Thorim and
--   Lightning Cage by 1.5 sec. »
--
-- 801869 a un ProcFlags DBC non nul (327680 = DONE_SPELL_MAGIC_DMG_CLASS_NEG |
-- DONE_PERIODIC), donc SpellMgr::LoadSpellProcs lui FABRIQUE déjà une entrée par défaut
-- (SpellMgr.cpp:2248-2318) : famille 0, phase HIT, chance 100. Le talent n'est donc pas
-- muet — il est TROP BAVARD : il se déclenche sur tout dégât magique négatif, sur chaque
-- tic périodique, et une fois PAR CIBLE touchée. Cette ligne remplace cette entrée.
--
-- ProcFlags 65536 : DONE_SPELL_MAGIC_DMG_CLASS_NEG seul. Call Lightning (500040) et
--   Electrocute (801844) sont tous deux DmgClass 1 MAGIC, école 8 (nature), et négatifs.
--   DONE_PERIODIC est retiré : l'infobulle dit « Casting ».
-- SpellPhaseMask 1 = CAST : Spell::_cast n'appelle ProcSkillsAndAuras qu'UNE fois par
--   lancement (Spell.cpp:4016-4047), ce qui supprime le comptage par cible.
--   CONSÉQUENCE ASSUMÉE, à ne pas redécouvrir : la phase CAST est émise AVANT toute
--   résolution de coup — le bloc Spell.cpp:4011-4046 précède l'appel de handle_immediate()
--   ligne 4079, donc avant les SPELL_MISS_*. La réduction s'applique donc au lancement, y
--   compris si Call Lightning / Electrocute rate, est résisté, ou frappe une cible immunisée,
--   ce que l'entrée générée (phase HIT) ne faisait pas. C'est cohérent avec l'infobulle,
--   qui dit « Casting », mais ce n'est PAS un simple changement de comptage.
-- SpellFamilyName 0 et masques nuls : c'est un choix mesuré, pas un renoncement. Les 96
--   bits de SpellFamilyFlags de la famille 22 ne peuvent pas séparer Call Lightning
--   (0,16,32) d'Aeroblast (8388608,16,32) : aucun bit de l'un n'échappe à l'autre. Le tri
--   se fait donc en C++ (aura_ascension_stormbringer_titanstorm::CheckProc), par chaîne de
--   rang : sSpellMgr->GetFirstSpellInChain(id) contre 500040 et 801844.
-- DisableEffectsMask 0 : 801869 n'a qu'un effet, l'aura 42 que le script prend en charge.
-- Chance 0 : ProcChance DBC = 100, repris par LoadSpellProcs (SpellMgr.cpp:2114-2115).
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_proc` WHERE `SpellId` = 801869;
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(801869, 0, 0, 0, 0, 0, 65536, 0, 1, 0, 0, 0, 0, 0, 0, 0);


-- -------------------------------------------------------------------------------------
-- 4. spell_script_names — sans ces deux lignes les AuraScript ne tournent JAMAIS (P-051)
--
-- Le troisième script du fichier, stormbringer_event_casts, est un AllSpellScript : il
-- s'enregistre par `new` et n'a PAS de ligne ici (une ligne spell_script_names pour un
-- AllSpellScript serait rejetée au chargement).
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_script_names` WHERE `spell_id` IN (804035, 801869)
    AND `ScriptName` IN ('aura_ascension_stormbringer_tailwind', 'aura_ascension_stormbringer_titanstorm');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(804035, 'aura_ascension_stormbringer_tailwind'),
(801869, 'aura_ascension_stormbringer_titanstorm');
