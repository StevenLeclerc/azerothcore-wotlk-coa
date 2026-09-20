-- =====================================================================================
-- mod-ascension-compat — talents « promesse sans handler » réparables PAR LA DONNÉE SEULE
--
-- Analyse du 2026-09-20, catégorie (A1) du triage des 199 sorts recensés dans
-- docs/ANNEXE-recensement-talents-non-cables.md. Gabarit : le correctif P-045
-- (Chronomancer « Anomaly Spikes »),
-- modules/mod-ascension-compat/data/sql/db-world/2026_09_20_00_ascension_chronomancer_anomaly_spikes.sql
--
-- EN SERVICE depuis le 2026-09-20. Appliqué à acore_world et enregistré dans `updates`
-- (state = MODULE). Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud, sans redémarrage : `.reload spell_proc`.
--
-- -------------------------------------------------------------------------------------
-- Ce que chaque talent partage avec P-045 : son effet est un APPLY_AURA / aura 42
-- SPELL_AURA_PROC_TRIGGER_SPELL vers un sort qui EXISTE dans Spell.dbc, mais le champ
-- ProcFlags (champ 34) du DBC vaut 0, ou décrit un autre événement que celui de
-- l'infobulle. SpellMgr.cpp:2248-2250 (« Skip if no proc flags in DBC ») n'engendre
-- alors aucune SpellProcEntry, et SpellAuras.cpp:2150-2154 refuse toute aura sans
-- entrée : l'aura est posée, elle n'est jamais candidate à un proc.
--
-- Les huit lignes ci-dessous sont les SEULS cas des 199 dont la condition annoncée par
-- l'infobulle s'exprime entièrement avec ProcFlags / SpellTypeMask / SpellPhaseMask /
-- HitMask / SchoolMask, SANS restriction à un sort nommé. Les vingt autres talents
-- réparables par la donnée disent « en lançant X », « les dégâts de X » : ils
-- exigent en plus un SpellFamilyName + SpellFamilyMask établi bit à bit, et ne sont
-- donc PAS dans ce fichier.
--
-- Noms et valeurs des drapeaux lus dans :
--   src/server/game/Spells/SpellMgr.h        — enum ProcFlags, ProcFlagsSpellType,
--                                              ProcFlagsSpellPhase, ProcFlagsHit
--                                              (ce n'est PAS SpellDefines.h)
--   src/server/game/Spells/SpellMgr.cpp:2085 — SpellMgr::CanSpellTriggerProcOnEvent
--   src/server/game/Spells/Auras/SpellAuras.cpp:2148 — Aura::GetProcEffectMask
--
-- Règles du cœur respectées ligne à ligne, pour ne pas polluer Errors.log :
--   * SpellTypeMask  ne doit être posé QUE si ProcFlags touche
--     (SPELL_PROC_FLAG_MASK | PERIODIC_PROC_FLAG_MASK)   — SpellMgr.cpp:2141
--   * SpellPhaseMask ne doit être posé QUE si ProcFlags touche
--     REQ_SPELL_PHASE_PROC_FLAG_MASK, et il est OBLIGATOIRE dans ce cas — 2143,2147
--   * Chance = 0 fait retomber le cœur sur le ProcChance du DBC (2114-2115).
--     Le ProcChance DBC des huit sorts a été lu : 10, 100, 100, 30, 100, 100, 25, 100.
--     Aucun n'est nul — une valeur nulle aurait donné un proc qui ne part jamais.
--   * Charges = 0 : ProcCharges DBC = 0 pour les huit, aura permanente.
--
-- AUCUNE BOUCLE DE PROC POSSIBLE, établi dans le code et non supposé :
-- Aura::GetProcEffectMask refuse (a) tout sort déclenché par cette aura même
-- (`spell->GetTriggeredByAuraSpellInfo() == m_spellInfo`), et (b) tout sort déclenché
-- en général tant que PROC_ATTR_TRIGGERED_CAN_PROC n'est pas posé — il ne l'est ici
-- pour aucune ligne (AttributesMask = 0). Vérification faite en plus, sort par sort,
-- sur la nature du sort déclenché (voir les commentaires).
-- =====================================================================================

DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN
    (520145, 560286, 704626, 706220, 800084, 801096, 803163, 803347);

INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES

-- 520145 — Runemaster, tronc de classe, « Glyphs of Power »
--   « Your direct damage dealt has a $h% chance to restore $706528s1% of your maximum mana. »
--   ProcFlags 69972 = 0x11154 = DONE_MELEE_AUTO_ATTACK | DONE_SPELL_MELEE_DMG_CLASS
--     | DONE_RANGED_AUTO_ATTACK | DONE_SPELL_RANGED_DMG_CLASS
--     | DONE_SPELL_NONE_DMG_CLASS_NEG | DONE_SPELL_MAGIC_DMG_CLASS_NEG.
--     DONE_PERIODIC est volontairement EXCLU : l'infobulle dit « direct damage ».
--   SpellTypeMask 1 = PROC_SPELL_TYPE_DAMAGE, pour que les soins ne comptent pas.
--   Sort déclenché 706528 : effet 137 ENERGIZE_PCT, DmgClass 0 — ne fait aucun dégât,
--   ne peut lever aucun des drapeaux ci-dessus. ProcChance DBC = 10.
(520145, 0, 0, 0, 0, 0, 69972, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 560286 — Primalist / Wildwalker, « Lacerations »
--   « Melee critical strikes now increase the effectiveness of bleed effects… »
--   Le DBC porte déjà ProcFlags 20, mais AUCUN HitMask : sans HitMask, le défaut d'un
--   proc DONE est NORMAL|CRITICAL|ABSORB (SpellMgr.cpp:944-963), donc le talent
--   partirait sur TOUT coup de mêlée, à 100 %, au lieu des seuls coups critiques.
--   HitMask 2 = PROC_HIT_CRITICAL rétablit l'infobulle.
--   Sort déclenché 560288 : aura 255, DmgClass 0, aucun dégât. ProcChance DBC = 100.
(560286, 0, 0, 0, 0, 0, 20, 1, 2, 2, 0, 0, 0, 0, 0, 0),

-- 704626 — Bloodmage / Son of Arugal, « Thick Pelt »
--   « Physical damage taken now reduces the Physical damage taken from the next attack… »
--   Le DBC porte ProcFlags 4 = DONE_MELEE_AUTO_ATTACK : le talent se déclenche
--   aujourd'hui sur VOS PROPRES attaques, exactement l'inverse de l'infobulle.
--   40 = 0x28 = TAKEN_MELEE_AUTO_ATTACK | TAKEN_SPELL_MELEE_DMG_CLASS.
--   SchoolMask 1 = SPELL_SCHOOL_MASK_NORMAL, c'est-à-dire « physique » : il est comparé
--   à eventInfo.GetSchoolMask(), qui existe aussi pour une attaque blanche.
--   SpellPhaseMask 0 : les drapeaux TAKEN n'appartiennent pas à
--   REQ_SPELL_PHASE_PROC_FLAG_MASK ; le poser ferait journaliser une erreur.
--   Sort déclenché 556233 : aura 14 MOD_DAMAGE_TAKEN sur soi. ProcChance DBC = 100.
(704626, 1, 0, 0, 0, 0, 40, 1, 0, 0, 0, 0, 0, 0, 0, 0),

-- 706220 — Primalist / Wildwalker, « Terrasmash »
--   « Damage dealt by your off hand weapon now has a $h% chance to launch a Geode… »
--   8388608 = 0x800000 = PROC_FLAG_DONE_OFFHAND_ATTACK.
--   SpellTypeMask ET SpellPhaseMask restent à 0 : 0x800000 n'appartient ni à
--   SPELL_PROC_FLAG_MASK ni à REQ_SPELL_PHASE_PROC_FLAG_MASK, les poser ferait
--   journaliser deux erreurs et ne servirait à rien.
--   Boucle écartée par lecture du DBC : le Geode 804002 est DmgClass 1 (MELEE) et
--   pourrait donc lever un drapeau de mêlée — mais son AttributesEx3 vaut 0x40000000,
--   sans le bit 0x01000000 SPELL_ATTR3_REQUIRES_OFF_HAND_WEAPON, donc Spell.cpp:596-600
--   lui donne BASE_ATTACK et il lève DONE_MAINHAND_ATTACK, pas DONE_OFFHAND_ATTACK.
--   ProcChance DBC = 30.
(706220, 0, 0, 0, 0, 0, 8388608, 0, 0, 0, 0, 0, 0, 0, 0, 0),

-- 800084 — Ranger, tronc de classe, « Briar Veil »
--   « …causing dodging attacks to restore $803288s1% maximum health and $803288s2 Focus. »
--   40 = 0x28 = TAKEN_MELEE_AUTO_ATTACK | TAKEN_SPELL_MELEE_DMG_CLASS.
--   HitMask 16 = PROC_HIT_DODGE, INDISPENSABLE : le défaut d'un proc TAKEN est
--   NORMAL|CRITICAL, qui exclut l'esquive (SpellMgr.cpp:944-963). Une esquive est bien
--   remontée comme événement de proc : Unit.cpp:2890 appelle ProcSkillsAndAuras après
--   CHAQUE échange de mêlée, et Unit.cpp:159 pose PROC_HIT_DODGE sur MELEE_HIT_DODGE.
--   SpellTypeMask 0 volontairement : une attaque esquivée ne fait aucun dégât, son
--   SpellTypeMask serait NO_DMG_HEAL ; poser 1 tuerait le talent.
--   Sort déclenché 803288 : effets 136 HEAL_PCT et 30 ENERGIZE. ProcChance DBC = 100.
(800084, 0, 0, 0, 0, 0, 40, 0, 0, 16, 0, 0, 0, 0, 0, 0),

-- 801096 — Runemaster / Spiritmage, « Ley Power »
--   « While active, melee attacks and abilities grant you Harnessed Leylines. »
--   20 = 0x14 = DONE_MELEE_AUTO_ATTACK | DONE_SPELL_MELEE_DMG_CLASS.
--   SpellTypeMask 1 n'est pas décoratif ici : le sort déclenché 804316 est lui-même
--   DmgClass 1 (MELEE) et lèverait DONE_SPELL_MELEE_DMG_CLASS ; comme il ne fait ni
--   dégât ni soin, son SpellTypeMask est NO_DMG_HEAL et la restriction à DAMAGE le
--   recale. (La garde de Aura::GetProcEffectMask suffirait déjà ; c'est une ceinture
--   en plus de la bretelle.)
--   Réparation PARTIELLE assumée : les effets 0 (SCRIPT_EFFECT vers 802647) et 1
--   (DUMMY) de Ley Power restent sans code. ProcChance DBC = 100.
(801096, 0, 0, 0, 0, 0, 20, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 803163 — Templar / Monk, « Might of Aggramar »
--   « Physical damage dealt now has a $h% chance to increase the tick rate of… »
--   Le DBC porte ProcFlags 4 : seules les attaques blanches déclenchent, alors que
--   l'infobulle dit « physical damage dealt ». 20 = 0x14 ajoute les sorts de classe
--   de dégâts mêlée. SchoolMask 1 restreint au physique.
--   Sort déclenché 803164 : aura 108 ADD_PCT_MODIFIER, DmgClass 0. ProcChance DBC = 25.
(803163, 1, 0, 0, 0, 0, 20, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 803347 — Primalist / Wildwalker, « Bestial Wrath »
--   « When you critically strike, your pet restores $803348s1 Focus. »
--   69972 = 0x11154, même jeu de drapeaux « dégâts directs faits » que 520145,
--   plus HitMask 2 = PROC_HIT_CRITICAL.
--   Réparation PARTIELLE assumée : la seconde phrase de l'infobulle (« when your pet
--   critically strikes ») porte sur le familier, alors que l'effet 0 pose l'aura sur
--   le JOUEUR seul (SPELL_EFFECT_APPLY_AURA) — elle reste sans effet.
--   Sort déclenché 803348 : effet 30 ENERGIZE, aucun dégât. ProcChance DBC = 100.
(803347, 0, 0, 0, 0, 0, 69972, 1, 2, 2, 0, 0, 0, 0, 0, 0);
