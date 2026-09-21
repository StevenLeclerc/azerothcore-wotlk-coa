-- =====================================================================================
-- mod-ascension-compat — Knight of Xoroth : treize procs de la classe ont vu leur chance
-- faussée le 2026-09-19 par `rev_20260919_20`. Douze la tirent DEUX FOIS ; le treizième,
-- 560630, la tire alors qu'aucun tirage n'est dû. Correctif en donnée seule.
-- **NON APPLIQUÉ.**
--
-- Cette session n'écrit rien en base. Appliquer avec le mécanisme d'update du module,
-- puis `.reload spell_proc`. Retour arrière : remettre `Chance` = 0 sur les mêmes ids.
--
-- -------------------------------------------------------------------------------------
-- LE DÉFAUT, mesuré et non supposé.
--
-- Le cœur teste un proc dans cet ordre, `SpellAuras.cpp` (arbre de service) :
--     2216   if (!CallScriptCheckProcHandlers(aurApp, eventInfo)) return 0;   <-- le script
--     2276   float procChance = CalcProcChance(*procEntry, eventInfo);
--     2278   if (roll_chance_f(procChance)) return procEffectMask;            <-- la donnée
-- Le script d'abord, le tirage de `spell_proc` ENSUITE. Les deux se multiplient.
--
-- Or `AscensionXorothEvents.cpp`, fonction `Check`, tire DÉJÀ lui-même :
--   * `Chance(player, id)` (AscensionXoroth.cpp:148) lit `SpellInfo::ProcChance` du sort
--     lui-même, pose la recharge interne et ajoute le bonus éventuel ;
--   * pour les trois pestilences de familier, `roll_chance_i(GetSpellInfo()->ProcChance)`.
--
-- Tant que `spell_proc.Chance` valait 100, le tirage du cœur était neutre et celui du
-- module faisait foi. `data/sql/updates/pending_db_world/rev_20260919_20_coa_proc_chance_parity.sql`
-- (généré, 153 sorts, toutes classes confondues) a mis `Chance` = 0 sur une partie d'entre
-- eux pour « déférer au client ». `SpellMgr.cpp:2114-2115` retombe alors sur le
-- `ProcChance` du DBC — c'est-à-dire exactement la valeur que le module tire déjà.
-- Résultat : la chance annoncée est ÉLEVÉE AU CARRÉ.
--
--   id      sort                          infobulle   réel aujourd'hui
--   302548  A Curse from Hell               15 %        2,25 %
--   573034  Demonfire Retaliation           30 %        9 %
--   704971  Pestilent Retaliation            5 %        0,25 %
--   706502  Dread                           15 %        2,25 %
--   706590  Omen of Rage                    10 %        1 %
--   801065  Forgefiend's Bulwark            30 %        9 %
--   804345  Demon's Blood                   25 %        6,25 %
--   802603  Pestilence of Famine (familier) 20 %        4 %
--   802604  Pestilence of War (familier)    20 %        4 %
--   802605  Pestilence of Conquest (fam.)   20 %        4 %
--   800443  Hellfire Imp                    20 %        4 %   (le module tire 706565, 20 %)
--   562029  Defiance Pet Aura               20 %        4 %   (idem)
--
-- LA RÉPARATION : rendre au cœur un portillon neutre (`Chance` = 100) et laisser le
-- module — qui porte en plus la recharge interne et le bonus par Demonfire — être la
-- seule autorité. C'est l'état d'avant le 2026-09-19 pour ces douze lignes.
--
-- -------------------------------------------------------------------------------------
-- LE TREIZIÈME CAS — 560630 « Demon King ». Même régression du 2026-09-19, dans l'AUTRE
-- sens : pas un doublage, un tirage indu sur une certitude. Il est VIVANT aujourd'hui.
--
-- Ici le module ne tire pas : `AscensionXorothEvents.cpp:56-57` fait
-- `case 560630: return parry;` — aucun appel à `Chance(...)`. Le 35 % du DBC n'est donc
-- pas élevé au carré ; il s'applique à un événement qui ne comporte aucun tirage.
--
-- La chaîne, lue et non déduite :
--   * `AscensionXorothEvents.cpp:137-138` `case 560546: Cast(player, player, 560630);`
--     — 560546 a `spell_proc.Chance` = 100 : bloquer POSE l'aura à coup sûr ;
--   * `AscensionXorothEvents.cpp:140-141` `case 560630: GetAura()->Remove();` — et c'est
--     le SEUL chemin de retrait (`/usr/bin/grep -n 560630 AscensionXoroth*.cpp` ne rend
--     que Contracts.cpp:74, Events.cpp:138, Events.cpp:140) ;
--   * `Spell.dbc` 560630 : `ProcChance` 35, `DurationIndex` 21 → `SpellDuration.dbc`
--     ligne 21 = **-1**, donc PERMANENTE ; effet 0 = APPLY_AURA, aura 47
--     MOD_PARRY_PERCENT, valeur 10 ;
--   * `spell_proc` : 560630 `Chance` = 0 et `ProcsPerMinute` = 0 → `SpellMgr.cpp:2114-2115`
--     retombe sur les 35 du DBC ; `SpellAuras.cpp:2276-2278` tire APRÈS le script.
-- Conséquence, sur la classe que le joueur joue : un +10 % de parade PERMANENT qui ne se
-- consomme qu'une parade sur trois.
--
-- La promesse est écrite — dans l'infobulle du talent que le joueur ACHÈTE, 560546, rang
-- « Specialization » : « Level 40 Passive / Blocking an attack now increases your chance
-- to parry by $560630s1% until you parry an attack ». Pas de clause de chance, et son
-- `ProcChance` vaut 100. L'infobulle de 560630, rang « Aura », dit encore « has a $h%
-- chance ... for 30 sec » : texte périmé, contredit par sa propre `DurationIndex` = -1.
-- Le tracker amont est MUET sur les deux ids (0 entrée pour 560546, 0 pour 560630) :
-- rien ne lui est emprunté ici.
--
-- Et la réparation ne dépend même pas de l'arbitrage entre ces deux textes, parce que les
-- 35 % portent le RETRAIT et jamais la pose. Sous le texte de 560546 (pose certaine,
-- retrait certain) comme sous celui de 560630 (pose à 35 %, retrait « until you parry »),
-- 560630 doit valoir 100. C'est le seul id du fichier dont la correction est vraie sous
-- les deux lectures.
--
-- -------------------------------------------------------------------------------------
-- CE QUI N'EST PAS TOUCHÉ, et pourquoi :
--   * 560546 : `Chance` vaut déjà 100, ce qui correspond au texte de son propre rang.
--     Départager les deux infobulles (pose certaine contre pose à 35 %) demanderait une
--     mesure en jeu que cette session n'a pas faite ; on ne touche donc pas à cet id.
--   * 92104, 681449, 520662, 802602, 300376, 520372, 524920, 704972, 704979, 704991,
--     800997, 800702 : `Chance` vaut déjà 100 (ils ne sont pas dans les 153).
--   * les 140 autres sorts de `rev_20260919_20` appartiennent aux vingt autres classes.
--     **Le même doublon les guette dès que leur module tire lui-même** ; ce fichier ne
--     parle que de la classe 17. À vérifier classe par classe.
--
-- -------------------------------------------------------------------------------------
-- RECTIFICATION d'une mise en garde du fichier 2026_09_20_18 de la veille au soir.
--
-- Ce fichier-là annonce, en gros caractères, un « SECOND DÉFAUT » : l'effet 1 du talent
-- 706755 (Impcaller) serait un SPELLMOD_COST +100 à `EffectSpellClassMask` VIDE, donc
-- appliqué à toute la famille 23 (P-071). **C'EST FAUX, et c'est une erreur d'index.**
--
-- L'index correct est celui de P-064 : `EffectSpellClassMask` occupe les champs bruts
-- 122 à 130, trois mots par effet — effet 0 = 122,123,124 ; effet 1 = 125,126,127.
-- Revalidé ici sur un témoin hors CoA : 11069 « Improved Fireball », aura 107, a le
-- masque (1,0,0) à 122+3e — le drapeau de famille 0x1 de Fireball (133). Lu à
-- « 123+e / 126+e / 129+e », le même sort donne (0,0,0), c'est-à-dire rien.
--
-- Avec le bon index, 706755 se lit ainsi :
--   effet 0 : APPLY_AURA / aura 42 PROC_TRIGGER_SPELL vers 807699, masque (0,0,0)
--             — normal, l'aura 42 n'utilise pas de masque de classe ;
--   effet 1 : APPLY_AURA / aura 107 ADD_FLAT_MODIFIER, MiscValue 14 SPELLMOD_COST,
--             +100, masque (0,768,0) — NON VIDE. P-071 ne s'applique pas à ce talent,
--             et il n'y a rien à désarmer.
--
-- Ce que ce masque vise EXACTEMENT, résolu contre les `SpellFamilyFlags` (champs 209-211)
-- de la famille 23 : **dix-neuf sorts**, et non « la seule capacité transformée » —
--   * Infernal Strike rangs 1 à 11 : 801016, 501511-501520      (drapeau 0x200)
--   * Shieldgore rangs 1 à 7       : 804353, 806869-806874      (drapeau 0x100)
--   * **Annihilation 504581**, drapeaux (0x00020000, 0x00000200, 0) : 0x200 est dans
--     768 = 0x300, elle est donc touchée elle aussi.
-- Annihilation est la transformation de Meatsaw promise par Dread 706502, pas celle
-- d'Infernal Strike — mais ce n'est PAS un débordement accidentel : sa propre infobulle
-- se termine par « Scales with modifiers to |cffffffffInfernal Strike|r », et le DBC
-- l'écrit en lui donnant le drapeau 0x200 d'Infernal Strike EN PLUS du sien (0x20000).
-- Le surcoût de 100 (10 de rage) la touche donc par construction. Fait du DBC, consigné
-- et non corrigé : rien n'écrit un masque « juste » qui l'exclurait.
--
-- Balayage complet de la famille 23 avec l'index 122+3e : **0 effet modificateur
-- (aura 107 ou 108) à masque vide**. P-071 n'a aucun cas chez le Knight of Xoroth.
-- Sonde : /opt/coa/coa-project/sonde-xoroth-masques.py
-- -------------------------------------------------------------------------------------

START TRANSACTION;

UPDATE `spell_proc` SET `Chance` = 100 WHERE `SpellId` IN (
    302548,  -- A Curse from Hell        : Check tire Chance(player, 302548) + 2 %/Demonfire
    560630,  -- Demon King (aura)        : Check renvoie `parry` SANS tirage ; les 35 % du DBC
             -- portent le RETRAIT de l'aura, que 560546 promet certain (cf. TREIZIÈME CAS)
    562029,  -- Defiance Pet Aura        : Check tire Chance(player, 706565)
    573034,  -- Demonfire Retaliation    : Check tire Chance(player, 573034)
    704971,  -- Pestilent Retaliation    : Check tire Chance(player, 704971), recharge 5 s
    706502,  -- Dread                    : Check tire Chance(player, 706502)
    706590,  -- Omen of Rage             : Check tire Chance(player, 706590)
    800443,  -- Hellfire Imp             : Check tire Chance(player, 706565)
    801065,  -- Forgefiend's Bulwark     : Check tire Chance(player, 801065)
    802603,  -- Pestilence of Famine     : Check tire roll_chance_i(ProcChance)
    802604,  -- Pestilence of War        : idem
    802605,  -- Pestilence of Conquest   : idem
    804345   -- Demon's Blood            : Check tire Chance(player, 804345)
);

COMMIT;
