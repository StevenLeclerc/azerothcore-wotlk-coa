-- =====================================================================================
-- mod-ascension-compat — Reaper : evenements de la ressource Reaped Soul,
-- et correction de trois lignes spell_proc posees le meme jour.
--
-- Compagnon de modules/mod-ascension-compat/src/AscensionReaperEvents.cpp.
-- Le nom de ce fichier est la cle d'update enregistree dans `updates` (state = MODULE) :
-- ne pas le renommer une fois applique.
--
-- Prise d'effet :
--   * les lignes `spell_proc`         : a chaud, `.reload spell_proc`.
--   * les lignes `spell_script_names` : REDEMARRAGE OBLIGATOIRE. ObjectMgr::
--     LoadSpellScriptNames() n'a qu'un appelant (World.cpp:871) et l'accrochage
--     (World.cpp:883) ne tourne qu'une fois ; il n'existe pas de
--     `.reload spell_script_names`.
--
-- DEUX PREALABLES AU REDEMARRAGE, sans lesquels les trois lignes `spell_script_names`
-- ci-dessous designeront des scripts INEXISTANTS (P-051 a l'envers : la ligne SQL existe,
-- le script non) :
--   1. relancer `cmake` sur /opt/coa/build-main AVANT de compiler. Les sources du module
--      sont ramassees par file(GLOB) au moment de la configuration
--      (src/cmake/macros/AutoCollect.cmake:28) et le module n'a pas de CMakeLists.txt
--      propre — seul mod-ascension-compat.cmake, qui ne liste aucune source. Un simple
--      `make` ne verra donc jamais AscensionReaperEvents.cpp, qui est un fichier neuf.
--   2. cabler AddSC_AscensionReaperEvents() dans
--      modules/mod-ascension-compat/src/MP_loader.cpp (declaration + appel). Aujourd'hui
--      `grep -n AscensionReaper MP_loader.cpp` ne rend que Dirge, Reflexes, Talents et
--      Secondary.
--
-- -------------------------------------------------------------------------------------
-- LE RELEVE PREALABLE, FAIT AVANT D'ECRIRE
--
-- `spell_proc`, `spell_script_names` et `spell_linked_spell` interroges en base sur les
-- douze talents Reaper casses. Resultat : TROIS lignes existaient deja, toutes trois dans
-- `spell_proc`, toutes trois posees par
-- 2026_09_20_03_ascension_talents_proc_masque_famille.sql — 705403, 706792, 804004.
-- Aucune ligne nulle part pour 92146, 301986, 524735, 705404, 705442, 706788, 707116,
-- 803999, 805181. Aucune ligne `spell_script_names` pour 500363 ni 803031.
--
-- -------------------------------------------------------------------------------------
-- (1) LES TROIS LIGNES DU 2026-09-20 REPOSENT SUR UNE ERREUR DE DmgClass — MESURE
--
-- Le fichier _03 justifie son ProcFlags 256 ainsi : « Requiem et Soulrend sont tous deux
-- DmgClass 2 (RANGED) », « Deathchaser est DmgClass 2 (RANGED) ». L'enum du coeur dit
-- l'inverse, src/server/shared/SharedDefines.h:1637-1642 :
--     SPELL_DAMAGE_CLASS_NONE = 0, MAGIC = 1, MELEE = 2, RANGED = 3
-- DmgClass 2 est donc MELEE, et src/server/game/Spells/Spell.cpp:2258-2276
-- (Spell::prepareDataForTriggerSystem, cas MELEE l. 2270) leve pour un sort MELEE
--     PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS = 0x10 = 16
-- et non PROC_FLAG_DONE_SPELL_RANGED_DMG_CLASS = 0x100 = 256, qui correspond a
-- DmgClass 3. SpellMgr::CanSpellTriggerProcOnEvent (SpellMgr.cpp:899) commence par
-- « if (!(eventInfo.GetTypeMask() & procEntry.ProcFlags)) return false; » : avec 256,
-- ces trois procs ne peuvent JAMAIS partir. Les talents sont restes inertes.
--
-- DmgClass est le champ 213 de Spell.dbc. Indice etabli par temoins, pas suppose :
--   Fireball 133 = 1 (MAGIC), Mortal Strike 12294 = 2 (MELEE), Overpower 7384 = 2,
--   Shadow Bolt 686 = 1, Auto Shot 75 = 3 (RANGED).
-- Valeurs lues pour les sorts concernes, champ 213 :
--   Requiem 805717 et 806840-806845 = 2 (MELEE)
--   Soulrend 573316-573322, 572341, 572342, 802731 = 2 (MELEE)
--   Deathchaser 805190, 560428, 560429, 561032-561034, 573052, 573053 = 2 (MELEE)
--   Soul Strike 500517-500521, 500646 = 2 (MELEE)
--   Murder 500376, 502679-502684, 504622 = 1 (MAGIC)
--
-- Consequence par ligne :
--   705403 « Soulstorm »  : 256 -> 16. Le reste de la ligne est juste et n'est pas touche
--                           (masque (256,8192,0), phase 1 CAST, ProcChance DBC 40).
--   804004 « Soulbender » : 256 -> 16. Le masque (4,0,0) de la ligne corrigeait deja le
--                           vrai bug du DBC — l'EffectSpellClassMask de l'aura vaut
--                           (0,0,64) la ou Deathchaser porte SpellFamilyFlags (4,0,0) —
--                           mais le ProcFlags annulait la correction.
--   706792 « Well of Souls » : 272 -> 65552. 272 = 0x10 | 0x100. Le 0x10 fait deja partir
--                           Soul Strike (MELEE) ; le 0x100 ne designe rien. Murder est
--                           MAGIC et negatif, donc PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_NEG
--                           = 0x10000 = 65536 (Spell.cpp:4024 et 2760). Le talent est donc
--                           aujourd'hui a moitie vivant ; 65552 = 0x10 | 0x10000 lui rend
--                           sa seconde moitie.
-- Note sur l'EffectSpellClassMask : ses indices dans Spell.dbc sont 122 + 3*e + k
-- (e = effet, k = 0..2), pas 123+e / 126+e / 129+e. Verifie par temoins — Improved
-- Fireball 11069 porte 1 en [122] (Fireball = (1,0,8)), Improved Overpower 12290 porte 4
-- en [122] (Overpower = (4,0,0)), Improved Kidney Shot 14174 dont seul l'effet 1 est un
-- modificateur porte 2097152 en [125].
--
-- -------------------------------------------------------------------------------------
-- (2) DEUX TALENTS QUE LA DONNEE SEULE REPARE
--
-- 707116 « Warden of the Lost »                                            famille 36
--   « Damage dealt by Spectral Scythes and Spectral Wardens now reduces the cooldown of
--     Spectral Warden by $/1000;707127S1 sec. »
--   La chaine est deja entierement ecrite dans le DBC, il n'y manque que le proc :
--     effet 0 = 190 SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS, aura 42
--       PROC_TRIGGER_SPELL -> 707128. SpellAuras.cpp:2882-2899 : cet effet ne vise QUE
--       les summons controles du porteur, jamais le porteur lui-meme.
--     707128 = effet 140 FORCE_CAST, cible 27 TARGET_UNIT_MASTER, declenche 707127.
--     707127 = effet 165 SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN, MiscValue 805716
--       (« Spectral Warden »), valeur -500 ms.
--   Le maitre voit donc sa recharge baisser quand son invocation frappe. Aucun degat
--   dans la chaine, aucune boucle possible.
--   ProcFlags 69652 = 0x4 DONE_MELEE_AUTO_ATTACK | 0x10 DONE_SPELL_MELEE_DMG_CLASS
--     | 0x1000 DONE_SPELL_NONE_DMG_CLASS_NEG | 0x10000 DONE_SPELL_MAGIC_DMG_CLASS_NEG.
--     Volontairement SANS 0x40000 DONE_PERIODIC : un tic de DoT toutes les secondes
--     rendrait la recharge gratuite.
--   SpellFamilyName 0 : ce sont les sorts DE L'INVOCATION qui declenchent, pas ceux du
--     joueur ; aucune famille a imposer. Une famille 36 ici desarmerait la ligne.
--   DisableEffectsMask 2 : l'effet 1 est un APPLY_AURA / aura 4 DUMMY sans TriggerSpell,
--     et c'est la SEULE part de l'aura que porte le joueur (UnitAura::FillTargetMap,
--     SpellAuras.cpp:2835-2838). Le desactiver empeche le joueur de declencher la chaine
--     avec ses propres degats, ce que l'infobulle ne promet pas.
--   ECART ASSUME ET SIGNALE : l'effet 190 pose l'aura sur TOUS les summons controles, pas
--     seulement Spectral Scythe et Spectral Warden. C'est le choix du DBC, pas le mien ;
--     restreindre exigerait du C++ dans un fichier existant.
--   ProcChance DBC = 100.
--
-- 524735 « Redshade »                                                      famille 36
--   « Using Reap now transforms your Reap into Thresh for 10 sec. »
--   Effet 1 = aura 42 PROC_TRIGGER_SPELL -> 525058 « Thresh » (rang « Dummy », aura 4,
--     DurationIndex 1). C'est ce marqueur que la description du talent accompagne de la
--     balise client @s:505170:0@, 505170 etant le vrai « Thresh ».
--   Masque (0,1,0) : Reap porte SpellFamilyFlags (0,1,0) sur ses 8 rangs
--     (500357, 504056-504058, 504557, 505151, 573302, 573303) et sur « Reap (Offhand) »
--     803001. Partage connu : 500599 « Decimate / Stun » porte (0,2049,0) et prendrait
--     donc le bit. Ecart assume, le meme que celui deja signale pour 706792 dans _03.
--   ProcFlags 16 : Reap est DmgClass 2 (MELEE), champ 213 lu sur les 8 rangs.
--   SpellPhaseMask 1 = CAST : « Using Reap », pas « damage dealt by Reap ».
--   SpellTypeMask 0 : en phase CAST le coeur passe PROC_SPELL_TYPE_MASK_ALL
--     (Unit.cpp:7120-7124), la colonne ne filtrerait rien.
--   DisableEffectsMask 1 : l'effet 0 est un APPLY_AURA / aura 4 DUMMY sans TriggerSpell.
--   RESERVE HONNETE : cette ligne fait POSER 525058. Que le serveur echange ensuite le
--     bouton Reap contre Thresh n'a PAS ete verifie — aucun mecanisme d'echange de sort
--     lie a 525058 n'existe dans le module (grep sur 525058 et 505170 : rien). Si
--     l'echange est purement client (balise @s:), la ligne suffit ; sinon il manquera un
--     SetTemporarySpellReplacement, qui est du C++.
--
-- -------------------------------------------------------------------------------------
-- (3) TROIS SCRIPTS DE AscensionReaperEvents.cpp, ET LEUR ACCROCHAGE (P-051)
--
-- aura_ascension_reaper_soul_events   -> 500363 « Reaped Soul »
--   Sert trois talents d'un seul crochet : 705442 « Fatesealer », 301986 « Spirit
--   Culling » et 805181 « Eater of Souls », dont les infobulles disent toutes
--   « generating a Reaped Soul » ou « reaching 3 Reaped Souls » — un evenement de
--   ressource qu'aucune colonne de `spell_proc` ne voit.
--   Le script ne MODIFIE jamais le nombre d'ames : il le lit. La ressource reste pilotee
--   par ResourceGainRules et AscensionCompat.cpp (regle du §5.2 de
--   docs/CONCEPTION-evenements-de-classe.md).
--   Pas de ligne `spell_proc` pour 500363 : le script s'accroche a AfterEffectApply, pas
--   a un proc.
--
-- aura_ascension_reaper_soul_infusion -> 803031 « Soul Infusion »
--   Sert 803999 « Dominion » : « Gaining Soul Infusion now increases your Armor ».
--   803031 est une aura, pas un evenement de combat : le crochet est sa pose.
--   Pas de ligne `spell_proc` sur 803031 : le script s'accroche a AfterEffectApply.
--
--   MAIS une ligne est necessaire sur 803999 LUI-MEME, et c'est un defaut trouve en me
--   relisant. DEUX talents de la liste ont un ProcFlags DBC non nul, 803999 et 804004,
--   tous deux a 4 = PROC_FLAG_DONE_MELEE_AUTO_ATTACK (champ 34 relu : 705403, 706792,
--   92146, 524735, 707116, 301986, 705442, 805181, 706788, 705404 sont tous a 0). 804004
--   a deja une ligne, dont le ProcFlags 16 ecrase le 4 du DBC ; 803999 n'en avait aucune,
--   d'ou la ligne ci-dessous. La
--   generation par defaut de SpellMgr::LoadSpellProcs (SpellMgr.cpp:2248-2270) ne le
--   saute donc pas et fabrique une SpellProcEntry ; son effet 0 etant une aura 42
--   PROC_TRIGGER_SPELL -> 804000, le talent declenche AUJOURD'HUI le gain d'armure a
--   CHAQUE coup d'arme automatique. Ce n'est pas « gaining Soul Infusion », et cela
--   maintient le buff en permanence. Ajouter mon crochet sans rien faire d'autre aurait
--   donne DEUX sources.
--   Correctif : une ligne 803999 avec DisableEffectsMask 1. Aura::GetProcEffectMask
--   (SpellAuras.cpp:2219-2233) retire l'effet 0 du masque ; le masque devient nul et
--   Unit.cpp:13460 ignore l'aura. Le proc parasite disparait, le script devient la seule
--   source. ProcFlags reste a 0 dans la ligne : SpellMgr.cpp:2110-2111 le fait retomber
--   sur le 4 du DBC, ce qui evite le LOG_ERROR « doesn't have ProcFlags value defined ».
--   SpellPhaseMask reste 0, correct : 4 n'appartient pas a REQ_SPELL_PHASE_PROC_FLAG_MASK.
--
-- aura_ascension_weakened_souls       -> 92146 « Weakened Souls »
--   « Your Soulrend now also applies Weakened Soul. »
--   Ligne `spell_proc` necessaire, ci-dessous. Masque (0,8192,0) : Soulrend porte
--   SpellFamilyFlags (0,8192,0) sur ses 9 entrees, bit exclusif dans la famille 36
--   (l'entree 573320 « Soulrend / aura » porte (0,1024,0) et reste donc dehors).
--   ProcFlags 16 (MELEE), SpellTypeMask 1 (degats), SpellPhaseMask 2 (HIT) : « applies »
--   au contact, sur chaque cible touchee.
--   DisableEffectsMask 2 : l'effet 1 est une aura 354, aura privee Ascension dont
--   l'entree de AuraEffectHandler[] vaut nullptr (SpellAuraEffects.cpp:419) — P-053. La
--   reveiller n'apporterait rien et brouillerait l'effet 0 (P-052). Le script s'accroche
--   a EFFECT_0 / SPELL_AURA_DUMMY, ce que le DBC donne bien a 92146.
--   ProcChance DBC = 100.
--   DEUX RESERVES SUR LE DEBUFF POSE, 803433, mesurees et detaillees en commentaire dans
--   AscensionReaperEvents.cpp au-dessus du cast. Elles concernent 803433, sort HORS du
--   perimetre de ce fichier : rien ici ne les repare, elles sont signalees.
--     a) duree INFINIE : DurationIndex 21, et la ligne 21 de SpellDuration.dbc vaut
--        (-1, 0, -1). Aucune source n'ecrit de duree pour ce debuff — ni l'infobulle de
--        92146, ni celle de 803433 — donc aucune n'est inventee. La poser demanderait une
--        ligne `spell_dbc` sur 803433.
--     b) PAS restreint a Ombre et Givre : l'effet 0 est une aura 271
--        SPELL_AURA_MOD_DAMAGE_FROM_CASTER, qui est cadree par SpellFamilyName +
--        EffectSpellClassMask (Unit.cpp:9365-9370 et 10869-10872 -> IsAffectedOnSpell ->
--        SpellInfo::IsAffected, SpellInfo.cpp:1422-1434) et ne lit JAMAIS son MiscValue.
--        Le MiscValue 48 (Ombre+Givre) est donc de la donnee morte pour ce type d'aura ;
--        l'EffectSpellClassMask de l'effet 0 vaut (0,0,0), et IsAffected saute le test de
--        drapeaux sur un masque nul : les 10 % valent pour TOUT sort de famille 36 du
--        lanceur, Reap physique compris. Les coups d'arme automatiques restent dehors,
--        les deux sites exigeant un spellProto. Restreindre demanderait une ligne
--        `spell_dbc` donnant un EffectSpellClassMask a l'effet 0 de 803433.
-- =====================================================================================

-- -------------------------------------------------------------------------------------
-- (1) correction de ProcFlags sur les trois lignes du 2026-09-20. Seule cette colonne
--     change : masque de famille, phase et type restent ceux de _03, qui sont justes.
-- -------------------------------------------------------------------------------------
UPDATE `spell_proc` SET `ProcFlags` = 16    WHERE `SpellId` = 705403;
UPDATE `spell_proc` SET `ProcFlags` = 16    WHERE `SpellId` = 804004;
UPDATE `spell_proc` SET `ProcFlags` = 65552 WHERE `SpellId` = 706792;

-- -------------------------------------------------------------------------------------
-- (2) et (3) lignes neuves. Le DELETE couvre une reapplication du fichier.
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_proc` WHERE `SpellId` IN (92146, 524735, 707116, 803999);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
     `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`,
     `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES
-- 92146  « Weakened Souls »     — Soulrend, degats, phase HIT
( 92146, 0, 36, 0, 8192, 0,    16, 1, 2, 0, 0, 2, 0, 0, 0, 0),
-- 524735 « Redshade »           — Reap, phase CAST
(524735, 0, 36, 0,    1, 0,    16, 0, 1, 0, 0, 1, 0, 0, 0, 0),
-- 707116 « Warden of the Lost » — degats des invocations, phase HIT
(707116, 0,  0, 0,    0, 0, 69652, 1, 2, 0, 0, 2, 0, 0, 0, 0),
-- 803999 « Dominion » — muselle le proc parasite engendre par le ProcFlags 4 du DBC.
--   ProcFlags 0 dans la ligne = « garde celui du DBC » ; seul DisableEffectsMask agit.
(803999, 0,  0, 0,    0, 0,     0, 0, 0, 0, 0, 1, 0, 0, 0, 0);

-- -------------------------------------------------------------------------------------
-- (3) accrochage des trois scripts. ABS() sur spell_id : une ligne negative laissee par
--     une version anterieure designerait le meme sort et accrocherait le script une
--     SECONDE fois (les liaisons sont stockees dans un multimap).
-- -------------------------------------------------------------------------------------
DELETE FROM `spell_script_names`
    WHERE ABS(`spell_id`) IN (500363, 803031, 92146)
      AND `ScriptName` IN ('aura_ascension_reaper_soul_events',
                           'aura_ascension_reaper_soul_infusion',
                           'aura_ascension_weakened_souls');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(500363, 'aura_ascension_reaper_soul_events'),
(803031, 'aura_ascension_reaper_soul_infusion'),
( 92146, 'aura_ascension_weakened_souls');
