-- =====================================================================================
-- mod-ascension-compat — talents « promesse sans handler » réparables PAR LA DONNÉE,
-- catégorie (A2) : ceux dont l'infobulle NOMME un sort déclencheur.
--
-- Analyse du 2026-09-20, docs/ANALYSE-triage-199-talents.md § 4. Gabarits :
--   2026_09_20_00_ascension_chronomancer_anomaly_spikes.sql   (P-045)
--   2026_09_20_01_ascension_talents_proc_donnee_seule.sql     (A1, 8 lignes)
--
-- Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud : `.reload spell_proc`.
--
-- -------------------------------------------------------------------------------------
-- CE QUI DISTINGUE CE FICHIER DU PRÉCÉDENT
--
-- Les huit lignes de (A1) n'avaient AUCUNE restriction de sort : leur infobulle parlait
-- de « dégâts directs », « attaques de mêlée », « critiques ». Ici l'infobulle dit
-- « en lançant X », « les dégâts de X », « en soignant avec X ». Sans restriction, la
-- ligne ferait procer le talent sur TOUT, ce qui est pire que le silence actuel.
--
-- La restriction passe par SpellFamilyName + SpellFamilyMask0/1/2, comparés aux
-- SpellFamilyFlags du sort de l'événement :
--   SpellMgr.cpp:925-928  CanSpellTriggerProcOnEvent
--      if (eventInfo.GetTypeMask() & SPELL_PROC_FLAG_MASK)
--          if (!eventSpellInfo->IsAffected(procEntry.SpellFamilyName, procEntry.SpellFamilyMask))
--              return false;
--   SpellInfo.cpp:1422-1434  IsAffected : familyName 0 => pas de filtre ;
--      sinon famille identique ET (masque & SpellFamilyFlags) != 0.
-- Un seul bit partagé élargit donc silencieusement le proc : chaque bit retenu ci-dessous
-- a été confronté à TOUS les sorts de la même famille dans Spell.dbc (209 510 lignes).
--
-- Famille de sorts = ID de classe + 6 (references/architecture.md) :
--   18 Barbarian · 22 Stormbringer · 25 Templar · 26 Bloodmage · 27 Ranger
--   34 Tinker · 36 Reaper · 37 Primalist · 38 Runemaster
-- IsValidSpellProcFamily (SpellMgr.cpp:40-46) accepte 0 et 18..38 : les neuf passent.
--
-- -------------------------------------------------------------------------------------
-- PHASE DE PROC — le point qui décide de la fidélité à l'infobulle
--
--   Spell.cpp:4016-4047  phase CAST : une seule fois par lancement, et SEULEMENT si
--     `m_originalCaster && !IsTriggered()`. Les drapeaux viennent de DmgClass :
--     MELEE -> 0x10 (+0x400000/0x800000), RANGED -> 0x100,
--     MAGIC/NONE -> 0x4000/0x10000 (positif/négatif) ou 0x400/0x1000.
--     Unit.cpp:7119-7127 : à la phase CAST, spellTypeMask vaut MASK_ALL.
--   Spell.cpp:2729-2776  phase HIT : une fois PAR CIBLE TOUCHÉE.
--
-- D'où la règle suivie ici : « en lançant X » et tout effet qui ne doit partir qu'une
-- fois par utilisation -> SpellPhaseMask = 1 (CAST). « les dégâts de X » sur un sort
-- mono-cible ou périodique -> SpellPhaseMask = 2 (HIT).
--
-- -------------------------------------------------------------------------------------
-- RÈGLES DE VALIDATION DE LoadSpellProcs (SpellMgr.cpp:2117-2175), respectées ligne
-- à ligne pour ne pas polluer Errors.log :
--   * SpellTypeMask  seulement si ProcFlags touche (SPELL_PROC_FLAG_MASK | PERIODIC_…)
--   * SpellPhaseMask OBLIGATOIRE, et seulement si ProcFlags touche REQ_SPELL_PHASE_…
--   * HitMask seulement si ProcFlags touche TAKEN_HIT_… ou DONE_HIT_…  (0 partout ici)
--   * Chance = 0 fait retomber sur le ProcChance du DBC (2114-2115). Les quinze ont été
--     lus : 15, 40, 100, 100, 100, 100, 100, 100, 100, 100, 100, 50, 100, 100, 100.
--     AUCUN n'est nul — un DBC à 0 aurait donné un proc qui ne part jamais.
--   * Charges = 0 : ProcCharges DBC = 0 pour les quinze, aura permanente.
--   * EquippedItemClass = -1 pour les quinze : la garde « équipement » de
--     Aura::GetProcEffectMask (SpellAuras.cpp:2236-2245) ne s'applique pas.
--
--
-- DisableEffectsMask — POURQUOI SIX LIGNES LE POSENT
-- SpellAuraEffects.cpp:1393-1395 : `case SPELL_AURA_DUMMY: case SPELL_AURA_PROC_TRIGGER_SPELL:`
-- partagent HandleProcTriggerSpellAuraProc. Une aura 4 DUMMY est donc, elle aussi,
-- candidate au proc. Six de ces talents portent un effet APPLY_AURA / aura 4 dont le
-- champ EffectTriggerSpell vaut 0 : le proc n'y produirait rien (LOG_DEBUG, pas d'erreur),
-- mais il n'a aucune raison d'être appelé. On le coupe explicitement.
-- Le cas qui compte vraiment est 520621 : son effet 1 est la SEULE chose que le JOUEUR
-- porte (l'effet 0, un area-aura vers les invocations, ne lui est pas appliqué). Couper
-- l'effet 1 ramène procEffectMask à 0 côté joueur — SpellAuras.cpp:2233-2234 renvoie
-- alors 0 — et ne laisse procer que les invocations, ce que dit l'infobulle.
-- La règle de validation (SpellMgr.cpp:2154-2156) exige que chaque effet désigné SOIT
-- une aura : les six désignés sont des SPELL_EFFECT_APPLY_AURA, vérifié un à un.
--
-- AUCUNE BOUCLE DE PROC. Aura::GetProcEffectMask (SpellAuras.cpp:2148-2178) refuse
-- (a) le sort déclenché par cette aura même, (b) tout sort déclenché tant que
-- PROC_ATTR_TRIGGERED_CAN_PROC n'est pas posé. Cet attribut n'est posé que sur DEUX
-- lignes (706683, 707239), parce que leur sort nommé est lui-même lancé en `triggered`
-- — et dans les deux cas le masque de famille borne l'ouverture à ce seul sort.
-- La nature du sort déclenché a été lue une à une dans le DBC (voir commentaires).
--
-- -------------------------------------------------------------------------------------
-- CINQ DES VINGT NE SONT PAS ICI, et ne sont PAS réparables par la donnée :
--   801869 Titanstorm  — « Call Lightning » ne porte AUCUN bit que « Aeroblast » n'ait
--                        aussi ((0,16,32) contre (8388608,16,32)), même école 8, même
--                        DmgClass 1 : aucun masque ne peut les séparer.
--   524735 Redshade    — le sort déclenché 525058 « Thresh / Dummy » est une aura 4
--                        DUMMY que RIEN ne lit dans mod-ascension-compat (grep vérifié
--                        sur 525058 et 505170) : câbler le proc ne produirait rien.
--   801086 Convergence — « les 10 prochains Weapon Engravings » : l'ensemble des sorts
--                        visés n'est pas déterminable (appliqueurs « Weapon Engraving: X »
--                        contre sorts de dégâts « X Engraving / Damage »), et le bit
--                        FF0 131072 est partagé avec 653253 « Air Carving ».
--   707150 Water Runes — les effets 1 ET 2 portent une aura 42 (Water 653261 ET Ice
--                        653217) : un proc les appliquerait TOUS LES DEUX, alors que
--                        l'infobulle dit « Water OU Ice … while they are active ».
--   707116 Warden…     — exprimable sans masque (auto-attaques des invocations), mais à
--                        ProcChance 100 la réduction de recharge serait sans borne, et
--                        la donnée ne fournit aucune valeur de plafond.
-- =====================================================================================

DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN
    (520621, 560140, 560835, 705403, 705581, 705700, 705715, 706201,
     706208, 706683, 706792, 802888, 804004, 804746, 707239);

INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES

-- 560835 — Templar / Discipline, « Lightkeeper »                     famille 25
--   « Periodic damage dealt by Condemn now has a $h% chance to increase your parry
--     chance … and also … to burst, dealing Holy damage … »
--   Masque (4194304,0,0) : Condemn porte (4194304, 2147483648, 1) sur SES DIX RANGS,
--     identiques (804906 R1, 806864-806868 R2-6, 567538-567541 R7-10). Le bit FF0
--     4194304 n'est porté par AUCUN autre sort de la famille 25 ; le bit FF2 1 l'est par
--     24 autres, le bit FF1 2147483648 par aucun — on retient le seul bit FF0.
--   ProcFlags 262144 = PROC_FLAG_DONE_PERIODIC : Condemn est un DoT (effet 0 = aura 3
--     SPELL_AURA_PERIODIC_DAMAGE, amplitude 3000 ms).
--   SpellTypeMask 1 = DAMAGE, INDISPENSABLE : DONE_PERIODIC est aussi levé par un tick
--     de SOIN (SpellAuraEffects.cpp:6871) — c'est exactement le piège P-045.
--   SpellPhaseMask 2 obligatoire : DONE_PERIODIC ∈ REQ_SPELL_PHASE_PROC_FLAG_MASK.
--   Sorts déclenchés 561016 (aura 47 MOD_PARRY_PERCENT sur soi) et 561277 (effet 2
--     SCHOOL_DAMAGE, 5 cibles) : aucun des deux ne peut lever DONE_PERIODIC.
--   ProcChance DBC = 15, ce que l'infobulle écrit « $h% ».
(560835, 0, 25, 4194304, 0, 0, 262144, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 705403 — Reaper / Domination, « Soulstorm »                        famille 36
--   « Requiem and Soulrend now have a $h% chance to refund a Reaped Soul. »
--   Masque (256,8192,0) : Requiem = (256,0,0) sur ses 7 rangs, Soulrend = (0,8192,0)
--     sur ses 8 rangs. Les DEUX bits sont exclusifs — zéro autre sort de la famille 36.
--   ProcFlags 256 = DONE_SPELL_RANGED_DMG_CLASS : Requiem et Soulrend sont tous deux
--     DmgClass 2 (RANGED).
--   SpellPhaseMask 1 = CAST, et c'est le point : Requiem frappe jusqu'à 15 cibles
--     (MaxAffectedTargets = 15). En phase HIT le talent rendrait jusqu'à 15 âmes par
--     lancement ; en phase CAST il part une fois, ce que dit l'infobulle.
--   SpellTypeMask 0 : en phase CAST le cœur passe PROC_SPELL_TYPE_MASK_ALL
--     (Unit.cpp:7120-7127), la colonne ne filtrerait rien.
--   Seul l'effet 1 est une aura (effet 0 est un DUMMY qui porte un champ aura 42 mort) :
--     il déclenche 680338 « Final Requiem », effet 183 TRIGGER_SPELL_DELAYED vers
--     500363 « Reaped Soul ». Aucun dégât, aucune boucle.
--   ProcChance DBC = 40.
(705403, 0, 36, 256, 8192, 0, 256, 0, 1, 0, 0, 0, 0, 0, 0, 0),

-- 705581 — Runemaster / tronc de classe, « Prismatic Flow »          famille 38
--   « Casting Phase Out now increases your movement speed by $705582s1% … »
--   Masque (64,0,0) : « Phase Out » 500671, bit FF0 64 exclusif dans la famille 38.
--     Les trois autres « Phase Out » du DBC (524778, 560269, 563727) ont des
--     SpellFamilyFlags NULS et ne sont donc jamais désignables — mais ce sont les
--     sorts d'aura internes, pas le bouton du joueur.
--   ProcFlags 5120 = 0x400|0x1000 = DONE_SPELL_NONE_DMG_CLASS_POS | …_NEG. Phase Out
--     est DmgClass 0 : Spell.cpp:4026-4029 choisit POS ou NEG selon IsPositive(), qui
--     dépend des effets. On pose les DEUX plutôt que de parier — le masque de famille
--     borne de toute façon à ce seul sort.
--   SpellPhaseMask 1 = CAST (« Casting »).
--   Sort déclenché 705582 : auras 31 MOD_INCREASE_SPEED et 107 ADD_FLAT_MODIFIER.
--     Aucun dégât, aucune boucle. ProcChance DBC = 100.
--   DisableEffectsMask 2 : l'effet 1 est une aura 4 DUMMY sans EffectTriggerSpell.
(705581, 0, 38, 64, 0, 0, 5120, 0, 1, 0, 0, 2, 0, 0, 0, 0),

-- 804746 — Barbarian / tronc de classe, « Dauntless »                famille 18
--   « Dealing damage with Whirling Advance now dispels 1 movement slowing effect from
--     you and refunds $560916s1 Energy. »
--   Masque (0,16,0) : « Whirling Advance » = (0,16,0) sur ses 6 rangs plus 805345,
--     806610 et 807118. Un seul autre sort de la famille 18 porte ce bit : 504804
--     « Raging Advance », dont l'infobulle est « Whirl forward, dealing $806610s1%
--     Weapon Damage » — c'est la variante à charges DU MÊME bouton, elle renvoie
--     elle-même à 806610. L'élargissement est donc voulu par la donnée, pas subi.
--   ProcFlags 256 : Whirling Advance est DmgClass 2 (RANGED).
--   SpellPhaseMask 1 = CAST. Whirling Advance frappe « enemies in your way » sans
--     limite de cibles (MaxAffectedTargets = 0) : en phase HIT l'énergie serait rendue
--     une fois par ennemi traversé. Écart assumé et documenté : le proc part une fois
--     par utilisation, même si elle ne touche personne.
--   Sort déclenché 560916 : effet 30 ENERGIZE (misc 3 = POWER_ENERGY) et effet 108
--     DISPEL_MECHANIC (misc 11 = MECHANIC_SNARE). Les deux sont natifs, aucun dégât.
--   ProcChance DBC = 100.
(804746, 0, 18, 0, 16, 0, 256, 0, 1, 0, 0, 0, 0, 0, 0, 0),

-- 706683 — Bloodmage / Packleader, « Festering Maw »                 famille 26
--   « Healing with Bite Wound now reduces the cooldown of Lunge by 1 sec. »
--   Masque (0,0,8388608) : le SOIN de Bite Wound est 556234 (effet 10 HEAL), seul sort
--     « Bite Wound » à porter des SpellFamilyFlags — les trois autres (532612, 556235,
--     706654) sont à (0,0,0) et resteraient inatteignables. Un seul autre sort de la
--     famille 26 porte le bit : 804805 « Gorge », qui n'est pas un soin (aura 23
--     PERIODIC_TRIGGER_SPELL) et que SpellTypeMask = 2 écarte.
--   ProcFlags 1024 = DONE_SPELL_NONE_DMG_CLASS_POS : 556234 est DmgClass 0 et positif
--     (Spell.cpp:2764-2775 pose ce drapeau pour un soin de classe NONE).
--   SpellTypeMask 2 = PROC_SPELL_TYPE_HEAL (Unit.cpp:7128-7129).
--   SpellPhaseMask 2 = HIT : un soin n'existe qu'à l'impact.
--   AttributesMask 2 = PROC_ATTR_TRIGGERED_CAN_PROC, INDISPENSABLE ici : 556234 est le
--     `EffectTriggerSpell` de 556235, donc lancé en `triggered`, et
--     Aura::GetProcEffectMask le refuserait sans cet attribut. L'ouverture est bornée
--     par le masque de famille + SpellTypeMask : seul ce soin peut passer.
--   Sort déclenché 706718 : effet 165 ASCENSION_MODIFY_COOLDOWN, misc 500126 = « Lunge ».
--     Aucun dégât, aucun soin : ne peut pas se rappeler. ProcChance DBC = 100.
(706683, 0, 26, 0, 0, 8388608, 1024, 2, 2, 0, 2, 0, 0, 0, 0, 0),

-- 706208 — Primalist / Geomancy, « Rupturer »                        famille 37
--   « Damage dealt by Seismic Spike or Seismic Crash now generates a stack of Geomolding. »
--   RÉPARATION PARTIELLE assumée : les deux autres phrases de l'infobulle (pile
--     supplémentaire au critique, recharge de Terrasurge via Lithic Lance) relèvent des
--     effets 1 et 2, qui restent sans code.
--   Masque (16,4194304,0) : Seismic Spike = (16,0,0) sur ses 7 rangs, Seismic Crash =
--     (0,4194304,0) sur ses 10 rangs. Les seuls autres porteurs dans la famille 37 sont
--     807093 « Seismic Spike / Damage NPC » et 804125 « Seismic Crash » — les mêmes
--     capacités sous une autre entrée.
--   ProcFlags 262160 = 0x10 | 0x40000 : Seismic Spike est un dégât direct DmgClass 1
--     (DONE_SPELL_MELEE_DMG_CLASS) ; Seismic Crash est un canalisé dont les dégâts
--     passent par une aura 3 PERIODIC_DAMAGE de 500 ms (DONE_PERIODIC).
--   SpellTypeMask 1 : sans lui, DONE_PERIODIC serait aussi levé par un tick de soin.
--   SpellPhaseMask 2 = HIT (« Damage dealt by »).
--   Sort déclenché 560170 « Geomolding » : auras 107/108 sur soi, plafonnées par
--     StackAmount. Aucun dégât. ProcChance DBC = 100.
--   DisableEffectsMask 4 : l'effet 2 est une aura 4 DUMMY sans EffectTriggerSpell.
(706208, 0, 37, 16, 4194304, 0, 262160, 1, 2, 0, 0, 4, 0, 0, 0, 0),

-- 560140 — Primalist / Geomancy, « Nature's Will »                   famille 37
--   « Casting Seismic spells now extends the duration of Earthshaping by … sec. »
--   Masque (80, 4194560, 1024) = l'union des cinq capacités « Seismic » du joueur :
--     FF0 16   Seismic Spike   (7 rangs)      FF0 64   Seismic Tremor (5 rangs)
--     FF1 256  Seismic Wave    (7 rangs)      FF1 4194304 Seismic Crash (10 rangs)
--     FF2 1024 Seismic Smash   (5 rangs)
--     Chaque rang de chaque chaîne porte bien le bit de sa chaîne — vérifié un à un.
--   Faux positif connu et mesuré : 926597 « Lesser Primal Wave » porte FF1 256 sans
--     être un sort Seismic. C'est un sort « Lesser », du même genre que 572830 « Lesser
--     Spirit Charge » que lancent les invocations : l'acteur du proc serait alors
--     l'invocation, pas le joueur porteur de l'aura. Le retirer coûterait Seismic Wave.
--   ProcFlags 272 = 0x10 | 0x100 : quatre des cinq sont DmgClass 1 (MELEE), Seismic
--     Smash est DmgClass 2 (RANGED).
--   SpellPhaseMask 1 = CAST (« Casting »), une fois par lancement : Seismic Spike touche
--     5 cibles et Seismic Wave 8, la phase HIT multiplierait l'extension de durée.
--   Sort déclenché 536256 : effet 177 ASCENSION_MODIFY_AURA_DURATION, misc 680441
--     (« Earthshaping »). Aucun dégât. ProcChance DBC = 100.
(560140, 0, 37, 80, 4194560, 1024, 272, 0, 1, 0, 0, 0, 0, 0, 0, 0),

-- 706201 — Primalist / tronc de classe, « Stonefaced »               famille 37
--   « Casting Seismic abilities now also grants 2 stacks of Earth's Rage. »
--   Même ensemble de sorts nommés que 560140, donc même masque et mêmes ProcFlags ;
--   voir la justification bit à bit ci-dessus.
--   Sort déclenché 573215 : effet 175 ASCENSION_MODIFY_AURA_STACKS (trigger 806068,
--     misc 2 = deux piles) et effet 183 TRIGGER_SPELL_DELAYED vers 680472. Natifs,
--     aucun dégât. ProcChance DBC = 100.
--   DisableEffectsMask 6 : les effets 1 et 2 sont des auras 4 DUMMY sans trigger.
(706201, 0, 37, 80, 4194560, 1024, 272, 0, 1, 0, 0, 6, 0, 0, 0, 0),

-- 802888 — Primalist / Life, « Eternally Chosen »                    famille 37
--   « Spirit Charge and Primal Rush now summons 2 Spirits of Life … »
--   Masque (16384,131072,0) : Primal Rush = (16384,0,0) sur ses 7 rangs, bit exclusif ;
--     Spirit Charge = (0,131072,0) sur ses 5 rangs. Un seul autre porteur du bit FF1
--     131072 : 572830 « Lesser Spirit Charge / Spirits of Life », que lancent
--     justement les esprits invoqués — l'acteur du proc serait l'esprit, pas le joueur,
--     donc l'aura du joueur n'est pas interrogée.
--   ProcFlags 16 = DONE_SPELL_MELEE_DMG_CLASS : les deux sont DmgClass 1.
--   SpellPhaseMask 1 = CAST, une invocation par lancement (Spirit Charge et Primal Rush
--     touchent tous deux jusqu'à 5 cibles : la phase HIT invoquerait 5 fois).
--   Le DBC portait ProcFlags 4 (DONE_MELEE_AUTO_ATTACK) : le talent se déclenchait sur
--     les attaques blanches, et — 0x4 n'appartenant pas à SPELL_PROC_FLAG_MASK — SANS
--     AUCUN contrôle de famille (SpellMgr.cpp:925). La ligne corrige les deux.
--   Sort déclenché 572826 : effet 28 SUMMON et effet 165 MODIFY_COOLDOWN. Aucun dégât
--     direct ; il est de surcroît déclenché par cette aura même, donc refusé en retour
--     par Aura::GetProcEffectMask. ProcChance DBC = 100.
(802888, 0, 37, 16384, 131072, 0, 16, 0, 1, 0, 0, 0, 0, 0, 0, 0),

-- 706792 — Reaper / Domination, « Well of Souls »                    famille 36
--   « Damage dealt by Soul Strike and Murder now reduces the cooldown of Spectral
--     Scythe by … sec. »
--   Masque (0,2112,0) = FF1 64 (Murder, 8 rangs) | FF1 2048 (Soul Strike, 6 rangs).
--   Deux autres porteurs, tous deux examinés :
--     573030 « Murder / The End is Near » — la même capacité sous une autre entrée ;
--     801322 « Shudder Scythe / Damage » — déclenché par 572382, donc lancé en
--       `triggered` : Aura::GetProcEffectMask le refuse, AttributesMask valant 0 ici ;
--     500599 « Decimate / Stun » porte FF1 2048 — et son infobulle dit elle-même
--       « Scales with modifiers to Soul Strike and Reap » : le partage de bits est un
--       choix de conception, pas une collision accidentelle. Écart assumé et signalé.
--   ProcFlags 272 = 0x10 | 0x100 : Murder est DmgClass 1, Soul Strike DmgClass 2.
--   SpellTypeMask 1 + SpellPhaseMask 2 (HIT) : « Damage dealt by », et les deux sorts
--     sont mono-cible — aucune multiplication par le nombre de cibles.
--   Seul l'effet 1 est une aura (l'effet 0 est un DUMMY portant un champ aura 108 mort).
--   Sort déclenché 706794 : effet 165 ASCENSION_MODIFY_COOLDOWN, misc 500484
--     (« Spectral Scythe »). Aucun dégât. ProcChance DBC = 100.
(706792, 0, 36, 0, 2112, 0, 272, 1, 2, 0, 0, 0, 0, 0, 0, 0),

-- 804004 — Reaper / Soul, « Soulbender »                             famille 36
--   « Significantly increases the Runic Power generated by Deathchaser. »
--   Masque (4,0,0) : « Deathchaser » = (4,0,0) sur ses 8 rangs (805190 R1, 560428-…,
--     573052-573053), bit exclusif dans la famille 36.
--   Le triage signalait ici un bug net : l'EffectSpellClassMask de l'aura vaut (0,0,64),
--     que le générateur par défaut du cœur aurait recopié — il ne peut JAMAIS rencontrer
--     (4,0,0). La ligne explicite remplace ce masque engendré.
--   ProcFlags 256 : Deathchaser est DmgClass 2 (RANGED).
--   SpellPhaseMask 1 = CAST. C'est le choix décisif et il est explicite : les dégâts de
--     Deathchaser sont périodiques (aura 3, amplitude 250 ms) ; brancher le proc sur
--     DONE_PERIODIC rendrait 10 points de puissance runique TOUS LES QUART DE SECONDE.
--     En phase CAST, le talent ajoute 10 points par lancement, ce qui est la lecture
--     littérale de « increases the Runic Power generated by Deathchaser ».
--   Sort déclenché 355463 « Energize Runic Power +10 » : effet 30 ENERGIZE, misc 6
--     (POWER_RUNIC_POWER). Aucun dégât. ProcChance DBC = 100.
(804004, 0, 36, 4, 0, 0, 256, 0, 1, 0, 0, 0, 0, 0, 0, 0),

-- 707239 — Tinker / Firearms, « War Crimes »                         famille 34
--   « Damage dealt by Explosive Augmentation and Tracer Augmentation now has a $h%
--     chance to apply an additional stack of Napalm. »
--   Le DBC porte ProcFlags 87312 SANS masque de famille : le talent proce aujourd'hui
--     sur TOUT sort de dégâts du Tinker. La ligne le resserre.
--   Masque (0,16777216,33554432) :
--     FF1 16777216 = 653238 « Explosive Augmentation / Damage », bit exclusif ;
--     FF2 33554432 = 653247 « Tracer Augmentation / Stack Damage » (et 653252
--       « Stealth Reveal », qui ne fait aucun dégât et que SpellTypeMask = 1 écarte).
--     Le bit FF2 8388608 des entrées « Augmentation » génériques est VOLONTAIREMENT
--       écarté : il est partagé par Aether, Piercing, Stim et Magic Augmentation.
--   ProcFlags 262160 = 0x10 | 0x40000 : 653238 est un dégât direct DmgClass 1 ;
--     les dégâts de Tracer sont portés par l'aura 3 périodique de 653247 (DONE_PERIODIC).
--   SpellTypeMask 1 ; SpellPhaseMask 2 (HIT).
--   AttributesMask 2 = PROC_ATTR_TRIGGERED_CAN_PROC : 653238 est le
--     `EffectTriggerSpell` de 653237, donc lancé en `triggered`. L'ouverture reste
--     bornée aux deux sorts du masque.
--   Sort déclenché 805315 « Napalm / Damage » : il est déclenché par cette aura même,
--     donc refusé en retour ; et ses SpellFamilyFlags (0,131072,0) ne rencontrent aucun
--     bit du masque. Double garantie. ProcChance DBC = 50, ce qu'écrit « $h% ».
(707239, 0, 34, 0, 16777216, 33554432, 262160, 1, 2, 0, 2, 0, 0, 0, 0, 0),

-- 705715 — Stormbringer / Wind, « Gift of Air »                      famille 22
--   « Casting Kiss of the Clouds now empowers your Air Elemental with Gift of Air … »
--   RÉPARATION PARTIELLE assumée : la seconde phrase (« your critical strikes now extend
--     the duration of your Tailwind ») est portée par un AUTRE sort, 706537
--     « Gift of Air / Passive SLS », hors de cette ligne.
--   Masque (0,0,1048576) : « Kiss of the Clouds » = (0,0,1048576) sur ses 9 rangs
--     (500039 R1, 501442-501449) et sur 355241. Bit exclusif dans la famille 22.
--   ProcFlags 5120 = 0x400|0x1000 : Kiss of the Clouds est DmgClass 0 (bouclier de
--     groupe) ; on pose POS et NEG plutôt que de parier sur IsPositive(), le masque de
--     famille bornant de toute façon à ce seul sort.
--   SpellPhaseMask 1 = CAST (« Casting »), ce qui évite aussi de procer une fois par
--     membre du raid bouclié (cible 56 TARGET_UNIT_CASTER_AREA_RAID).
--   Sort déclenché 804033 : cible 5 TARGET_UNIT_PET, auras 216 (hâte), 79 (dégâts) et
--     8 (soin périodique) — il atterrit bien sur l'élémentaire, comme l'infobulle le dit.
--     Le soin périodique qu'il pose est porté par le FAMILIER, pas par le joueur.
--   ProcChance DBC = 100.
--   DisableEffectsMask 2 : l'effet 1 est une aura 4 DUMMY sans EffectTriggerSpell.
(705715, 0, 22, 0, 0, 1048576, 5120, 0, 1, 0, 0, 2, 0, 0, 0, 0),

-- 705700 — Stormbringer / Wind, « Invigoration »                     famille 22
--   « Your Aeroblast now grants your Air Elemental a stack of Invigoration. »
--   Masque (8388608,0,0) : « Aeroblast » = (8388608,16,32) sur ses 10 rangs (801839 R1,
--     501450-501458). Le bit FF0 8388608 est exclusif ; les bits FF1 16 et FF2 32 sont
--     partagés avec Call Lightning et 60 autres sorts, et sont écartés pour cela.
--     802750 « Aeroblast / Proc » (le second coup différé) est à (0,0,0) : il ne peut
--     pas rencontrer le masque, donc pas de double pile.
--   ProcFlags 16 : Aeroblast est DmgClass 1 (MELEE).
--   SpellPhaseMask 1 = CAST : une pile par lancement.
--   Sort déclenché 681273 « Invigoration / Pet Trigger » : effet 140 FORCE_CAST de
--     680918 sur la cible 5 TARGET_UNIT_PET. C'est l'élémentaire qui lance 680918
--     (aura 79), donc aucun événement de proc ne revient au joueur.
--   ProcChance DBC = 100.
--   DisableEffectsMask 2 : l'effet 1 est une aura 4 DUMMY sans EffectTriggerSpell.
(705700, 0, 22, 8388608, 0, 0, 16, 0, 1, 0, 0, 2, 0, 0, 0, 0),

-- 520621 — Ranger / Survival, « Coordination »                       AUCUNE famille
--   « Your War Falcons and Dragonhawks now grant you a stack of Coordination when they
--     deal damage. »
--   CE TALENT N'A PAS BESOIN DE MASQUE DE FAMILLE, et c'est un résultat mesuré, pas un
--     renoncement : l'essentiel des dégâts d'un familier passe par des attaques blanches,
--     qui n'ont AUCUN SpellInfo. Pour PROC_FLAG_DONE_MELEE_AUTO_ATTACK (0x4), qui
--     n'appartient pas à SPELL_PROC_FLAG_MASK, SpellMgr.cpp:925 ne consulte même pas
--     IsAffected. Les entrées « War Falcon » / « Dragonhawk » porteuses de
--     SpellFamilyFlags (680278, 800247, 681393, 681394) sont des auras de compte et de
--     mise à l'échelle, pas les sorts de dégâts : un masque bâti sur elles n'aurait
--     jamais rien rencontré.
--   La réserve du triage (« l'aura est sur le familier, or l'infobulle dit grant YOU »)
--     est levée par la donnée : le sort déclenché 520622 cible 27 TARGET_UNIT_MASTER,
--     donc la pile revient bien au joueur. Corrigé ici.
--   ProcFlags 69908 = 0x4 | 0x10 | 0x100 | 0x1000 | 0x10000 : attaque blanche de mêlée,
--     plus les sorts de dégâts de classe mêlée / distance / aucune / magique NÉGATIFS.
--     Le DBC portait 87040, qui comprenait les variantes POSITIVES (0x400, 0x4000) —
--     un soin du familier aurait donné une pile — et oubliait la mêlée, c'est-à-dire
--     l'essentiel. SpellTypeMask 1 achève d'écarter tout ce qui n'est pas un dégât.
--   SpellPhaseMask 2 (HIT) obligatoire pour les drapeaux 0x10/0x100/0x1000/0x10000 ;
--     il n'est pas consulté pour l'attaque blanche, dont le typeMask ne touche pas
--     REQ_SPELL_PHASE_PROC_FLAG_MASK.
--   L'effet 0 est un SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS (190) : SpellAuras.cpp
--     :2882-2900 le pose sur les invocations CONTRÔLÉES du joueur, pas sur le joueur.
--     L'application que garde le joueur ne porte que l'effet 1 (aura 4 DUMMY) : c'est
--     exactement lui que DisableEffectsMask coupe ci-dessous, de sorte que les coups du
--     JOUEUR ne déclenchent rien du tout.
--   Sort déclenché 520622 : auras 290 et 163 sur le maître, plafonnées par StackAmount.
--     Aucun dégât, et il est déclenché par cette aura même. ProcChance DBC = 100.
--   DisableEffectsMask 2 : coupe l'effet 1 (aura 4 DUMMY), la seule chose que le JOUEUR
--     porte de cette aura — c'est ce qui garantit que seules les invocations procent.
(520621, 0, 0, 0, 0, 0, 69908, 1, 2, 0, 0, 2, 0, 0, 0, 0);
