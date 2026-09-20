-- =====================================================================================
-- mod-ascension-compat — Ranger : les trois talents « promesse sans handler » que le
-- fichier src/AscensionRangerEvents.cpp repare, et rien d'autre.
--
-- Moitie DONNEE du correctif. P-051 : un script enregistre en C++ mais absent de
-- `spell_script_names` ne tourne JAMAIS et ne coute qu'une ligne de log au demarrage
-- (« Script named '...' is not assigned in the database. »).
--
-- Le nom de ce fichier est la cle d'update enregistree dans `updates` (state = MODULE) ;
-- ne pas le renommer une fois applique.
--
-- PRISE D'EFFET : REDEMARRAGE OBLIGATOIRE. `ObjectMgr::LoadSpellScriptNames()` n'a qu'un
-- seul appelant (World.cpp:871) et l'accrochage lui-meme ne tourne qu'une fois ; il
-- n'existe pas de `.reload spell_script_names`. (`.reload spell_scripts` porte sur la
-- table `spell_scripts`, qui n'a aucun rapport.)
--
-- AUCUNE ligne `spell_proc` ici, et c'est voulu : les quatre scripts sont deux AuraScript
-- non-proc (calcul de montant, apply/remove) et deux SpellScript (AfterCast, OnHit).
-- Aucun n'a de crochet DoCheckProc / OnEffectProc, donc aucun n'a besoin d'entree de proc.
--
-- -------------------------------------------------------------------------------------
-- CE QUE CHAQUE LIGNE ACCROCHE, ET SUR QUELLE LECTURE
--
-- 705098 « Wingman »  ->  aura_ascension_ranger_wingman
--   « Reduces all damage taken by $680278s3% for each War Falcon or Dragonhawk you have
--     active. »
--   Effet 2 du DBC : APPLY_AURA / aura 87 SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN,
--     EffectMiscValue 127 (toutes les ecoles), EffectBasePoints -1 donc montant 0.
--     Le contenant existe, il est vide : le talent ne reduit rien aujourd'hui.
--   Le chiffre par familier n'est pas invente : 680278 et 681394 (« Shows how many
--     War Falcons / Dragonhawks you have active ») portent tous deux en effet 2
--     EffectBasePoints -3, donc une valeur de -2. C'est ce que citent les deux
--     infobulles : « $680278s3% » cote Wingman, « ${$w3}% » cote compteur.
--   Le COMPTE se lit dans les applications d'aura. ATTENTION, c'est la moitie qui
--     manquait : AVANT ce fichier, RIEN ne posait jamais 680278 ni 681394. Verifie,
--     pas suppose — aucun champ d'aucun des 209510 enregistrements de Spell.dbc ne les
--     reference, aucune ligne d'acore_world (spell_linked_spell, creature_addon,
--     creature_template_addon, smart_scripts, npc_spellclick_spells, spell_proc) ne les
--     nommait, et aucun fichier de src/server/ ni de modules/ hors celui-ci. Wingman
--     serait donc reste aussi mort qu'avant, avec trois lignes `spell_script_names`
--     inertes en prime. D'ou la ligne `creature_template_addon` ajoutee plus bas.
--     Une fois l'aura posee sur le familier, la chaine tient :
--     - l'effet 0 et l'effet 1 de 680278 sont des SPELL_EFFECT_APPLY_AREA_AURA_OWNER
--       (143), rayon 100 yards (SpellRadius.dbc indice 12 = 100.0, indice de champ 92+e
--       verifie contre Arcane Explosion 1449 = 10 y et Consecration 26573 = 8 y) ;
--       UnitAura::FillTargetMap (SpellAuras.cpp:2875-2881) les envoie vers
--       GetCharmerOrOwner(), donc vers le maitre ;
--     - deux familiers font DEUX applications distinctes : Aura::CanStackWith prend la
--       branche « caster different » et rend true parce que l'effet 1 est un
--       SPELL_AURA_PERIODIC_ENERGIZE dont aucune cible implicite n'est une zone
--       (SpellEffectInfo::IsTargetingArea = false), et aucune ligne `spell_group` ne
--       porte sur ces ids (SELECT verifie, 0 ligne) ;
--     - Unit::GetAuraCount (Unit.cpp:6279) compte 1 par application, StackAmount valant
--       0 dans le DBC.
--   L'effet 2 de 680278 / 681394 n'est PAS « le chiffre par familier range la » : c'est
--     un SPELL_AURA_ADD_FLAT_MODIFIER (107), EffectMiscValue 23 = SPELLMOD_EFFECT3
--     (SpellDefines.h:99), EffectSpellClassMask (0x00080000,0,0) — exactement les
--     SpellFamilyFlags de 705098. Le DBC cable donc lui-meme « -2 sur l'effet d'indice 2
--     de Wingman par compteur ». Cet effet n'est pas un effet de zone : il RESTE sur le
--     familier, ou il est inerte, seuls les Player portant des spellmods. C'est donc
--     bien l'AuraScript qui fournit le montant, sans doublon. (Indice des champs
--     EffectSpellClassMask relu dans DBCStructure.h : std::array<flag96,3>, champs
--     122-130, soit 128-130 pour l'effet d'indice 2 ; temoins Improved Fireball 11069
--     champ 122 = 1 = SpellFamilyFlags de Fireball 133, et Improved Frostbolt 11070
--     champ 122 = 32 = SpellFamilyFlags de Frostbolt 116.)
--   L'infobulle de 680278 confirme les deux moities : « You have a War Falcon active!
--     Generating 1 Focus per second.$?s705098[ Reduces damage taken by ${$w3}%.][] ».
--     Le Focus est l'effet 1 (aura 24 PERIODIC_ENERGIZE, MiscValue 2, valeur 1, periode
--     1000 ms), le $w3 est le -2 de l'effet 2.
--
--   BRANCHE DRAGONHAWK : MORTE AUJOURD'HUI, et le code la garde expres. Le seul
--     SPELL_EFFECT_SUMMON Ranger visant un « Dragonhawk » est 573058 « Dragonhawk
--     Tamer », EffectMiscValue 52393 — et `creature_template` n'a PAS 52393 (SELECT
--     verifie : seuls 50264 et 50393 existent, tous deux nommes « War Falcon »). Aucune
--     creature ne peut donc porter 681394. Ce n'est pas un « fait » a compter : c'est
--     une branche gratuite qui deviendra juste le jour ou la creature existera.
--   Le handler nullptr de P-053 ne concerne pas l'aura 87 : SpellAuraEffects.cpp:152
--     lui donne HandleNoImmediateEffect, et la valeur est relue a chaque coup par
--     Unit.cpp:9351 (sorts) et Unit.cpp:10856 (melee). Le talent couvre donc bien
--     « all damage taken ».
--
-- 680278 / 681394  ->  aura_ascension_ranger_companion_count
--   (681394 : ligne posee d'avance, sans effet tant que 52393 n'existe pas.)
--   Ces deux lignes ne changent RIEN au comportement des compteurs : le script ne fait
--   que rappeler AuraEffect::RecalculateAmount sur l'effet 2 de Wingman quand un
--   familier apparait ou disparait. Sans elles, Wingman garderait la valeur calculee au
--   moment ou son aura passive a ete posee, c'est-a-dire zero.
--   Unit::_UnapplyAura (Unit.cpp:5115) retire l'application de `m_appliedAuras`
--   (m_appliedAuras.erase, Unit.cpp:5133) AVANT de desappliquer ses effets : au retrait,
--   GetAuraCount a deja oublie le partant.
--
-- -802036 « Skullpiercer » et -806368 « Woodland Arrow »  ->  spell_ascension_ranger_phoenix_plumes
--   705074 « Phoenix Plumes » : « Your Skullpiercers and Woodland Arrows used with 5
--     stacks of Advantage now restore $520784s3% Focus and summon a War Falcon for
--     $520558d. » Ses deux effets DBC sont des aura 4 DUMMY a 0 : rien du tout.
--   Le sort declenche est 520558 « Phoenix Plumes / Proc », qui porte les DEUX moities :
--     effet 0 = SPELL_EFFECT_SUMMON (28) de la creature 50393 « War Falcon »
--     (creature_template verifie) avec SummonProperties 61, pour sa propre duree de
--     6000 ms (SpellDuration.dbc indice 32) — c'est le « $520558d » de l'infobulle ;
--     effet 1 = SPELL_EFFECT_ENERGIZE_PCT (137), MiscValue 2 (Focus), valeur 3, soit
--     exactement le « $520784s3% » cite. 520784 n'est pas lance : c'est la variante sans
--     invocation (son effet 0 est un DUMMY la ou 520558 a un SUMMON).
--   Le signe negatif accroche toute la chaine : 802036 est bien premier rang de ses dix
--     rangs et 806368 premier rang de ses sept (table `spell_ranks`, verifie).
--     ObjectMgr::LoadSpellScriptNames (ObjectMgr.cpp:6366) refuserait un rang
--     intermediaire.
--   Le crochet est AfterCast (Spell.cpp:4102), volontairement en amont de la
--     consommation d'Advantage faite en OnSpellCast (Spell.cpp:4129, par
--     HandleAscensionClassMechanicsCast) : la pile de 5 est encore entiere. Les dix rangs
--     de Skullpiercer et les sept de Woodland Arrow portent CasterAuraSpell = 804329
--     (champ 24, indice etabli : les 68 sorts du DBC qui y valent 804329 sont tous de la
--     famille 27), ce sont donc bien des consommateurs.
--
-- -500075 « Precision Shot »  ->  spell_ascension_ranger_swiftshot
--   705028 « Swiftshot » : « Damage dealt by Precision Shot now increases enemy Physical
--     damage taken by $800578s1% for $800578d. » Son unique effet DBC est une aura 4
--     DUMMY : aucun code, aucune ligne de donnee ne lancait 800578.
--   800578 « Precision Shot » (le debuff) : APPLY_AURA / aura 87, MiscValue 1
--     (SPELL_SCHOOL_MASK_NORMAL), valeur 4, duree 12000 ms (indice 29). Il fonctionne
--     nativement une fois lance.
--   Sept rangs dans la chaine de 500075 (verifie). Ces sept sorts portent DEJA
--     `spell_ascension_ranger_light_arrows` : le coeur stocke les liaisons dans un
--     multimap, deux scripts differents sur le meme sort coexistent sans se gener.
--
-- -------------------------------------------------------------------------------------
-- CE QUE CE FICHIER NE FAIT PAS, ET POURQUOI
--
-- 520621 « Coordination » : rien a ajouter. Sa ligne `spell_proc` (ProcFlags 69908,
--   SpellTypeMask 1, SpellPhaseMask 2, DisableEffectsMask 2) est deja en base depuis
--   2026_09_20_03_ascension_talents_proc_masque_famille.sql — verifie par SELECT.
--   Reserve signalee, non corrigee ici : SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS
--   (SpellAuras.cpp:2882-2900) pose l'aura sur TOUTES les invocations controlees du
--   joueur, pas seulement sur les War Falcons et Dragonhawks que nomme l'infobulle.
--   Restreindre demanderait de toucher au coeur.
--
-- 800084 « Briar Veil » : rien a ajouter non plus. Sa ligne `spell_proc` (ProcFlags 40,
--   HitMask 16 = PROC_HIT_DODGE) est en base depuis
--   2026_09_20_01_ascension_talents_proc_donnee_seule.sql — verifie par SELECT. Les
--   trois effets sont nativement servis : effet 0 aura 49 MOD_DODGE_PERCENT
--   (HandleAuraModDodgePercent, SpellAuraEffects.cpp:114), effet 1 SPELL_EFFECT_DUMMY
--   sans aura donc inerte, effet 2 aura 42 PROC_TRIGGER_SPELL vers 803288, dont les
--   effets 136 HEAL_PCT (2% de vie) et 30 ENERGIZE (5 Focus) sont natifs. ProcChance
--   DBC = 100, ProcCharges = 0.
--   Le piege P-052 ne mord pas ici : l'aura 49 n'a pas de case dans
--   AuraEffect::HandleProc, la reveiller ne produit rien, et DisableEffectsMask 0 reste
--   correct.
-- =====================================================================================

-- DELETE sur ABS(`spell_id`) : une ligne de signe oppose laissee par une version
-- anterieure designerait le meme sort et accrocherait le script une SECONDE fois.
DELETE FROM `spell_script_names` WHERE ABS(`spell_id`) IN (705098, 680278, 681394)
    AND `ScriptName` IN ('aura_ascension_ranger_wingman', 'aura_ascension_ranger_companion_count');
DELETE FROM `spell_script_names` WHERE ABS(`spell_id`) IN
    (802036, 501715, 501716, 501717, 501718, 501719, 501720, 501721, 501722, 501723,
     806368, 806444, 806445, 806446, 806447, 806448, 572579)
    AND `ScriptName` = 'spell_ascension_ranger_phoenix_plumes';
DELETE FROM `spell_script_names` WHERE ABS(`spell_id`) IN
    (500075, 572108, 572109, 572110, 572111, 572112, 572113)
    AND `ScriptName` = 'spell_ascension_ranger_swiftshot';

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(705098, 'aura_ascension_ranger_wingman'),
(680278, 'aura_ascension_ranger_companion_count'),
(681394, 'aura_ascension_ranger_companion_count'),
(-802036, 'spell_ascension_ranger_phoenix_plumes'),
(-806368, 'spell_ascension_ranger_phoenix_plumes'),
(-500075, 'spell_ascension_ranger_swiftshot');

-- -------------------------------------------------------------------------------------
-- LE PORTEUR DU COMPTEUR — sans cette ligne, tout ce qui precede est inerte.
--
-- 680278 est permanent (DurationIndex 21 = -1) : ObjectMgr::LoadCreatureTemplateAddons
-- (ObjectMgr.cpp) ne journalise donc meme pas son avertissement « temporary aura », et
-- Creature::LoadCreaturesAddon pose l'aura sur le familier, qui en est le caster.
-- `creature_template_addon` n'avait AUCUNE ligne pour 50393 (SELECT verifie, 0 ligne) ;
-- le DELETE n'est la que pour rendre le fichier rejouable.
--
-- 50393 « War Falcon » est la creature invoquee par 520558 « Phoenix Plumes », 520588
-- « Falconstrike Summon », 806341 « Falcon Dive » et 807119 « Falcon Diving » (tous
-- famille 27) — c'est le familier que l'infobulle de Wingman appelle « War Falcon ».
--
-- LAISSE DE COTE, FAUTE D'ECRIT : 50264 est AUSSI nomme « War Falcon » (invoque par
-- 800251 « Falcon's Call », dont la description dit seulement « Summons a falcon »).
-- Rien d'ecrit ne dit s'il compte pour Wingman. Il n'est donc PAS ajoute ici : l'ajouter
-- serait une ligne de plus, a decider sur une source, pas sur une deduction.
DELETE FROM `creature_template_addon` WHERE `entry` = 50393;
INSERT INTO `creature_template_addon` (`entry`, `auras`) VALUES (50393, '680278');
