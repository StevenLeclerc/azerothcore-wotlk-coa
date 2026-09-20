-- =====================================================================================
-- mod-ascension-compat — Primalist (classe 31, famille de sorts 37) : liaisons de la
-- passe « talents non fonctionnels » du 2026-09-20.
--
-- Moitié donnée de modules/mod-ascension-compat/src/AscensionPrimalistEvents.cpp.
-- Gabarits : 2026_09_20_01_ascension_talents_proc_donnee_seule.sql (P-045),
--            2026_09_20_02_ascension_chronomancer_displacement_script.sql (P-051),
--            2026_09_20_03_ascension_talents_proc_masque_famille.sql.
--
-- Le nom de ce fichier est la clé d'update enregistrée dans `updates` (state = MODULE) :
-- ne pas le renommer une fois appliqué.
--
-- PRISE D'EFFET
--   * les lignes `spell_script_names` exigent un REDÉMARRAGE : ObjectMgr::LoadSpellScriptNames
--     n'a qu'un appelant (World.cpp:871) et l'accrochage lui-même (World.cpp:883) ne tourne
--     qu'une fois. Il n'existe aucune commande `.reload spell_script_names`.
--   * les lignes `spell_proc` prennent à chaud avec `.reload spell_proc`, mais elles ne
--     servent à rien sans le binaire qui porte les scripts : redémarrer de toute façon.
--
-- CE FICHIER NE TOUCHE PAS aux lignes `spell_proc` déjà en service pour 560140, 706201,
-- 706208, 706220, 802888 et 803347 (passes 01 et 03 du même jour, relues en base avant
-- écriture). Les DELETE ci-dessous ne portent que sur les deux sorts que ce fichier ajoute.
--
-- -------------------------------------------------------------------------------------
-- ORDRE DE CHARGEMENT, LU DANS World.cpp ET NON SUPPOSÉ
--   424  LoadSpellInfoCustomAttributes()  -> GlobalScript `primalist_event_metadata`
--   871  LoadSpellScriptNames()           -> lit ce fichier
--   883  sScriptMgr->LoadDatabase()
--   886  ValidateSpellScripts()           -> valide contre les SpellInfo déjà corrigés
-- C'est ce qui permet à `aura_ascension_primalist_bestial_wrath::Validate` d'exiger que
-- l'effet 1 de 803347 soit déjà devenu un SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- 1. spell_script_names
--
-- `RegisterSpellScript` ne dit jamais sur quels sorts un script s'applique (P-051) : sans
-- ces lignes, les cinq scripts du fichier C++ ne tournent JAMAIS et le cœur se contente
-- d'un « Script named '…' is not assigned in the database. » au démarrage.
--
-- Le DELETE porte sur `ScriptName`, PAS sur le spell_id : c'est ce qui emporte aussi une
-- éventuelle ligne à spell_id négatif laissée par une version antérieure, qui désignerait
-- le même sort et accrocherait le script une SECONDE fois (les liaisons sont stockées dans
-- un multimap ; le handler tournerait deux fois).
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_script_names` WHERE `ScriptName` IN
    ('aura_ascension_primalist_rupturer',
     'spell_ascension_primalist_fury_of_the_wild',
     'aura_ascension_primalist_bestial_wrath',
     'spell_ascension_primalist_bring_me_their_bones',
     'aura_ascension_primalist_bones_stacker');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES

-- 706208 « Rupturer » — branche « coup critique ».
--   La ligne spell_proc du 2026-09-20 (passe 03) accorde déjà la pile de base de
--   Geomolding sur les dégâts de Seismic Spike / Seismic Crash. L'AuraScript s'accroche au
--   MÊME effet 0 (aura 42, EffectTriggerSpell 560170, lu dans Spell.dbc) et n'appelle PAS
--   PreventDefaultAction : le déclenchement natif continue, le script n'ajoute que la pile
--   supplémentaire et la pile d'Earthshaping promises au critique.
(706208, 'aura_ascension_primalist_rupturer'),

-- 801234 « Fury of the Wild » — « When you cast a Boon, you cast the same Boon on your pet
--   at 50% effectiveness. » Le talent lui-même n'a qu'un APPLY_AURA / SPELL_AURA_DUMMY
--   inerte ; il n'est donc pas scripté. Ce sont les cinq Boons du joueur qui portent le
--   script, et le script lance la copie « (Pet) » correspondante. Aucun chiffre n'est
--   inventé, mais les copies ne sont PAS une réduction uniforme de moitié — voici ce que
--   Spell.dbc porte réellement, effet par effet (relu ce jour) :
--     500935 Turtle  aura 15 = 4, aura 87 = -10
--       -> 500938    aura 15 = 4, aura 87 = -10   IDENTIQUE, pas réduit du tout
--     500939 Bear    auras 166 / 213 / 138 à 10
--       -> 504611    auras 166 = 5 et 138 = 5 ; l'aura 213 a DISPARU (dummy inerte)
--     500943 Hawk    aura 72 = -10, aura 216 = 12, aura 227 = 1 (déclenche 997800)
--       -> 504612    aura 72 = -5, aura 227 = 1 ; l'aura 216 a DISPARU. Le « 1 » est la
--                    valeur propre du déclencheur périodique, pas la moitié de 12
--     504856 Lion    TROIS auras 117 = 40 (mécaniques 1, 5, 10)
--       -> 505218    DEUX auras 117 = 20 (mécaniques 1, 5) ; la troisième a DISPARU
--     800137 Wolf    aura 31 = 25, aura 49 = 8
--       -> 802726    aura 31 = 13, aura 49 = 5 ; AUCUN effet APPLY_AURA, donc le joueur
--                    ne porte aucune application de celle-ci, seuls les invoqués l'ont
--   Trois copies sur cinq perdent donc un effet au passage, et celle de Turtle n'est pas
--   affaiblie. C'est la donnée telle qu'elle est écrite ; rien ici ne la compense.
--
--   EXCLUSION MUTUELLE — les cinq Boons du JOUEUR sont dans `spell_group` 1132 avec
--   `stack_rule` = 2 (SPELL_GROUP_STACK_RULE_EXCLUSIVE_FROM_SAME_CASTER) ; les cinq copies
--   « (Pet) » ne sont dans AUCUN groupe et sont permanentes (DurationIndex 21 ->
--   SpellDuration.dbc 21 = -1). Ce fichier n'ajoute VOLONTAIREMENT pas les copies au
--   groupe 1132 : Aura::CanStackWith (SpellAuras.cpp:2001) applique la règle entre l'aura
--   entrante et l'aura existante sans distinguer laquelle est la copie, si bien que la
--   copie retirerait le Boon du joueur qui vient de la lancer. C'est le SpellScript qui
--   retire les quatre autres copies avant de poser la nouvelle.
--   Chaque copie est bâtie d'effets SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS, plus au
--   plus UN SPELL_AURA_DUMMY inerte qui est la seule chose que le joueur garde (802726
--   n'en a même pas : le joueur ne porte alors aucune application). UnitAura::FillTargetMap,
--   SpellAuras.cpp:2827-2905 : l'effet 190 ne pousse que owner->m_Controlled, jamais le
--   porteur — pas de double buff possible.
--   Boon of the Eagle (504773), Boon of the Tiger, Boon of the Elements et les Empowered
--   Boons n'ont AUCUNE copie « (Pet) » dans Spell.dbc : ils restent hors du talent.
(500935, 'spell_ascension_primalist_fury_of_the_wild'),
(500939, 'spell_ascension_primalist_fury_of_the_wild'),
(500943, 'spell_ascension_primalist_fury_of_the_wild'),
(504856, 'spell_ascension_primalist_fury_of_the_wild'),
(800137, 'spell_ascension_primalist_fury_of_the_wild'),

-- 803347 « Bestial Wrath » — seconde phrase de l'infobulle.
--   La ligne spell_proc de la passe 01 couvre la première phrase (vos critiques rendent du
--   Focus au familier, effet 0 -> 803348, ENERGIZE ciblant TARGET_UNIT_PET) et la passe 01
--   signalait explicitement la seconde comme non réparable par la donnée : le familier ne
--   proque pas les auras de son maître (Unit::ProcSkillsAndAuras, Unit.cpp:7105-7137, ne
--   parcourt que les applications de l'acteur, sans renvoi vers le propriétaire).
--   Le GlobalScript `primalist_event_metadata` transforme l'effet 1 — un SPELL_AURA_DUMMY
--   inerte sur le joueur — en SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS, exactement le
--   montage des cinq « Boon of the X (Pet) ». Le familier porte alors l'effet 1 ; ses
--   critiques rencontrent la ligne spell_proc EXISTANTE (ProcFlags 69972, HitMask 2,
--   SpellTypeMask 1, SpellPhaseMask 2) sur SA propre application, et l'AuraScript lance
--   803350 (ENERGIZE_PCT, TARGET_UNIT_MASTER) et 572047 sur la victime.
--   AUCUN doublon : l'application du joueur ne garde que l'effet 0, celle du familier que
--   l'effet 1 — les deux masques de proc sont disjoints, donc pas de piège P-052.
(803347, 'aura_ascension_primalist_bestial_wrath'),

-- 806552 / 806378 « Bring Me Their Bones ».
--   806552 effet 1 déclenche 806554 « Mark » (5 piles) sur l'ennemi, effet 2 déclenche
--   806378 « Stacker Passive on Pet » (aura 42 -> 806589) sur le familier.
--   Le SpellScript sur 806552 est un FILET : il ne pose 806378 que si le familier ne le
--   porte pas déjà du même lanceur, donc il ne peut pas doubler l'effet 2 natif.
--   L'AuraScript sur 806378 remplace toute la chaîne 806589, cassée deux fois dans la
--   donnée : son effet 0 (SPELL_EFFECT_ASCENSION_MODIFY_AURA_STACKS) a MiscValue = 0 et
--   ModifyAscensionAuraStacks (SpellEffects.cpp:330-333) sort immédiatement sur un delta
--   nul ; son effet 1 lance 806553 à CHAQUE coup au lieu du cinquième. Le script appelle
--   donc PreventDefaultAction.
(806552, 'spell_ascension_primalist_bring_me_their_bones'),
(806378, 'aura_ascension_primalist_bones_stacker');

-- -------------------------------------------------------------------------------------
-- 2. spell_proc
--
-- Règles de LoadSpellProcs (SpellMgr.cpp:2117-2175) respectées ligne à ligne :
--   * SpellTypeMask  seulement si ProcFlags touche (SPELL_PROC_FLAG_MASK | PERIODIC_…)
--   * SpellPhaseMask OBLIGATOIRE, et seulement si ProcFlags touche REQ_SPELL_PHASE_…
--   * HitMask seulement si ProcFlags touche TAKEN_HIT_… ou DONE_HIT_…
--   * Chance = 0 retombe sur le ProcChance du DBC (2114-2115) : lu pour les deux sorts,
--     100 pour 806378 et 100 pour 706167. Aucun n'est nul.
--   * Charges = 0 : ProcCharges DBC = 0 pour les deux.
--   * EquippedItemClass = -1 pour les deux (champ 68 du DBC, lu) : la garde « équipement »
--     de Aura::GetProcEffectMask ne s'applique pas.
--   * Chaque effet désigné par DisableEffectsMask DOIT être une aura (SpellMgr.cpp:2155) :
--     l'effet 0 de 706167 est bien un SPELL_EFFECT_APPLY_AURA.
-- -------------------------------------------------------------------------------------

DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN (706167, 806378);

INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES

-- 806378 — Primalist / familier, « Bring Me Their Bones / Stacker Passive on Pet »
--   « causing damage dealt by your pet's abilities to the target to add a stack »
--   Le DBC porte ProcFlags 0 : SpellMgr::LoadSpellProcs n'engendre aucune SpellProcEntry
--   (« Skip if no proc flags in DBC ») et l'aura n'est jamais candidate au proc — P-045.
--   ProcFlags 69904 = 0x10 | 0x100 | 0x1000 | 0x10000
--     = DONE_SPELL_MELEE_DMG_CLASS | DONE_SPELL_RANGED_DMG_CLASS
--     | DONE_SPELL_NONE_DMG_CLASS_NEG | DONE_SPELL_MAGIC_DMG_CLASS_NEG.
--     0x4 DONE_MELEE_AUTO_ATTACK est VOLONTAIREMENT exclu : l'infobulle dit
--     « your pet's abilities », pas ses attaques blanches.
--   SpellTypeMask 1 = DAMAGE. SpellPhaseMask 2 = HIT, obligatoire pour ces quatre drapeaux.
--   AUCUN masque de famille, et c'est voulu : les capacités d'un familier appartiennent à
--     toutes les familles ; le tri est fait par le script, qui n'agit que si la victime
--     porte 806554 posé par CE joueur.
--   AUCUNE BOUCLE, mais l'argument doit être écrit en entier, car le garde « un sort lancé
--     en triggered ne proque pas » a TROIS échappatoires (Aura::GetProcEffectMask,
--     SpellAuras.cpp:2166-2173) : il est sauté si le sort PORTEUR a
--     SPELL_ATTR3_CAN_PROC_FROM_PROCS (0x04000000) dans le DBC, si AttributesMask porte
--     PROC_ATTR_TRIGGERED_CAN_PROC, ou si l'événement est une attaque blanche
--     (AUTO_ATTACK_PROC_FLAG_MASK) ou un kill. Ici les trois sont fermées : 806378 a
--     AttributesEx3 = 0x40000000 (champ 7, DO_NOT_DISPLAY_RANGE seul, donc PAS
--     CAN_PROC_FROM_PROCS), AttributesMask vaut 0 ci-dessous, et ProcFlags 69904 ne contient
--     aucun drapeau d'attaque blanche. 806553 n'a pas non plus SPELL_ATTR3_NOT_A_PROC. Et
--     de toute façon la marque est retirée AVANT le lancement, donc le script n'aurait rien
--     à consommer.
--     (La même erreur de raisonnement a été relevée sur 706208, qui lui PORTE
--     SPELL_ATTR3_CAN_PROC_FROM_PROCS : ce qui protège Rupturer est le masque de ProcFlags
--     de sa ligne, pas AttributesMask.)
--   ProcChance DBC = 100.
(806378, 0, 0, 0, 0, 0, 69904, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 706167 — Primalist, « Natural Efficiency » : ligne de MISE AU SILENCE, pas de réparation.
--   « Whenever you are struck by a root, stun, or incapacitate effect you are now instantly
--     healed for $707806s1% of your maximum health. »
--   Le DBC porte ProcFlags 174760 = 0x2AAA8, c'est-à-dire les HUIT drapeaux TAKEN de dégâts
--     (0x8 | 0x20 | 0x80 | 0x200 | 0x800 | 0x2000 | 0x8000 | 0x20000), 0x8
--     PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK compris (SpellMgr.h:114). Une SpellProcEntry est
--     donc bien engendrée par défaut (SpellMgr.cpp:2247 « Skip if no proc flags in DBC »
--     n'est pas atteint, et SpellMgr.cpp:1984 pose isTriggerAura[SPELL_AURA_DUMMY] = true).
--   CE PROC N'EST PAS INERTE, et c'est le vrai motif de cette ligne : l'EffectTriggerSpell
--     de l'effet 0 de 706167 vaut 707806 — le soin lui-même. AuraEffect::HandleProc
--     (SpellAuraEffects.cpp:1393-1395) route SPELL_AURA_DUMMY vers
--     HandleProcTriggerSpellAuraProc, qui lance bel et bien 707806
--     (SpellAuraEffects.cpp:7085-7089 ; le LOG_DEBUG sans effet est la branche `else`,
--     réservée au cas où le sort déclenché n'existe PAS). Autrement dit, tel qu'il est
--     livré, Natural Efficiency soigne 4 % des PV max à CHAQUE coup reçu, attaques blanches
--     comprises. La ligne ci-dessous supprime ce sur-soin ; elle n'est pas cosmétique.
--   Aucun drapeau de proc ne sait dire « un effet de contrôle vient de m'atteindre » :
--     l'événement est pris ailleurs, sur UNITHOOK_ON_AURA_APPLY (UnitScript
--     `primalist_event_auras`), qui lit le masque de mécaniques et le type d'aura.
--   DisableEffectsMask 1 coupe donc le seul effet d'aura du sort : Aura::GetProcEffectMask
--     renvoie 0, le sur-soin disparaît et l'événement revient entièrement au UnitScript.
--     Même technique que la ligne 520621 de la passe 03.
--   SpellTypeMask 0 et SpellPhaseMask 0 : aucun drapeau TAKEN n'appartient à
--     REQ_SPELL_PHASE_PROC_FLAG_MASK, poser une phase ferait journaliser une erreur.
(706167, 0, 0, 0, 0, 0, 174760, 0, 0, 0, 0, 1, 0, 0, 0, 0);
