-- =====================================================================================
-- mod-ascension-compat — Guardian : la Ballade du Pourfendeur n'avait aucun script.
--
-- CE QUE FAIT « Inspiring Leader » (505344), d'après son infobulle Spell.dbc :
--   « You pledge to inspire your allies in the midst of battle, transforming your
--     Pulverize and your Broad Sweep into Ballads. »
--   AscensionGuardianTalents.cpp:52-60 l'applique exactement ainsi, par
--   SetTemporarySpellReplacement : Pulverize -> Ballad of the Conqueror,
--   chaîne 805150 (Broad Sweep) -> Ballad of the Dragonslayer.
--   Le sort que le joueur LANCE est donc la ballade, pas Broad Sweep.
--
-- LA RUPTURE, mesurée et non déduite : les neuf rangs de la Ballade du Conquérant
-- (801776, 501068-501074, 574340) ONT une ligne `spell_script_names` vers
-- `spell_ascension_guardian_ability` ; les neuf rangs de la Ballade du Pourfendeur
-- n'en ont AUCUNE. Vérifié en base le 2026-09-20 :
--     SELECT spell_id FROM spell_script_names
--      WHERE spell_id IN (801772,501066,501067,572717,572718,572719,574341,574363,574364);
--   -> 0 ligne.
-- Sans ligne, le SpellScript n'est jamais attaché (P-051) et deux promesses tombent :
--   * « Earthsplitter » (805155), infobulle Spell.dbc recopiée : « Casting Broad Sweep
--     now increases the amount of enemies struck by your next Broad Sweep by $806081s3
--     and reduces its Energy cost by $806081s2% for $806081d, stacking $806081u times. » —
--     la charge 806081 est posée par AscensionGuardianAbilities.cpp::After(), qui ne
--     voyait que la chaîne 805150. Le talent était donc simplement MUET pour tout
--     Guardian portant Inspiring Leader, et rien de plus : 505344 est un passif
--     permanent (Spell.dbc rang « Passive », Attributes 0x140 dont le bit passif 0x40),
--     donc dès qu'il est appris Broad Sweep est remplacé en permanence et le joueur ne
--     lance plus jamais Broad Sweep. 806081 n'était posée par RIEN sur ce chemin : il
--     n'y avait aucun buff à appliquer, et rien ne le consommait non plus (Spell.dbc
--     806081 champ 36 ProcCharges = 0 — il expire à la durée, il n'est pas dépensé) ;
--   * « Calculated Strike » (705341) : « Your Reprisal, Broad Sweep, and Spear Throw
--     now deal an additional 2% Weapon Damage for each Energy point » — même cause,
--     même Hit() qui testait GetFirstSpellInChain(id) == 805150.
-- Les ballades n'ont pas de ligne `spell_ranks` : GetFirstSpellInChain rend l'id lui-même,
-- le test était donc toujours faux. (Vérifié : SELECT ... FROM spell_ranks -> 0 ligne.)
--
-- LE CORRECTIF EST EN DEUX MOITIÉS, et les deux sont nécessaires :
--   1. ici, les neuf lignes `spell_script_names` qui manquaient, exactement calquées
--      sur celles de la Ballade du Conquérant ;
--   2. dans le code, AscensionGuardianAbilities.cpp reconnaît désormais la ballade
--      comme Broad Sweep quand 505344 est porté (helper IsBroadSweep). C'est le même
--      idiome que AscensionClassMechanics.cpp:1452-1455, qui reconnaît déjà la Ballade
--      du Conquérant comme Pulverize pour Guardbreaker.
--
-- CE QUE CES LIGNES N'APPORTENT PAS : aucun effet nouveau. Le script est le même que
-- celui déjà attaché à Broad Sweep et aux ballades du Conquérant ; sur une ballade du
-- Pourfendeur il n'exécute que les deux branches ci-dessus. La consommation de Peril
-- (524610) reste réservée à Pulverize et aux ballades du Conquérant : la liste d'ids
-- de Hit() n'a pas été élargie.
--
-- CE QUI EST RÉELLEMENT RESTAURÉ SUR LA BALLADE, ET CE QUI NE L'EST PAS.
-- Earthsplitter 806081 porte deux moitiés, et une seule arrive sur la ballade :
--   * E1 = aura 108 ADD_PCT_MODIFIER, misc 14 SPELLMOD_COST, base -11 -> -10 % de coût,
--     masque (16,0,0). Les neuf ballades portent bien le drapeau de famille A=16 de
--     Broad Sweep (Spell.dbc champs 209-211 : (16,0,8)). Cette moitié-là ARRIVE ;
--   * E2 = aura 107 ADD_FLAT_MODIFIER, misc 34, base 0 DieSides 1 -> +1 cible, converti
--     en SPELL_AURA_MOD_MAX_AFFECTED_TARGETS par AscensionClassMechanics.cpp:698
--     ({806081, 24, EFFECT_2, 0, {{16,0,0}}}). Le cœur ne somme cette aura que DANS
--     `if (uint32 maxTargets = m_spellValue->MaxAffectedTargets)` — Spell.cpp:1283
--     (cône) et 1377 (aire), les deux SEULS usages de cette aura dans tout le cœur.
--     Or les NEUF rangs de la Ballade du Pourfendeur ont Spell.dbc champ 212
--     MaxAffectedTargets = 0 (Broad Sweep : 8). Cette moitié-là reste MORTE sur la
--     ballade tant que le champ 212 y vaut 0.
-- Rien n'est inventé ici : aucune infobulle, aucun champ du DBC, aucune entrée du
-- tracker amont ne donne un nombre de cibles à la ballade (801772 et 806081 : 0 entrée
-- dans audit-coa-issues.jsonl). AUCUN MaxAffectedTargets n'est posé.
-- Hors du chemin Inspiring Leader, sur Broad Sweep lui-même, les deux moitiés
-- fonctionnent : le champ 212 y vaut 8.
--
-- CHARGE AJOUTÉE, à connaître avant mise en service. Sur une ballade, Hit() tourne une
-- fois par cible touchée, et Calculated Strike 705342 — un vrai lancer de sort, jusqu'à
-- 200 % de dégâts d'arme — part donc une fois par ennemi vivant dans les 8 yd. Broad
-- Sweep bornait cela à 8 (Spell.dbc champ 212 = 8) ; la ballade n'a AUCUNE borne
-- (champ 212 = 0). La sémantique n'est donc PAS identique à celle de Broad Sweep.
-- Aucun plafond n'est posé ici : aucune source n'en donne un, et le choix (borner les
-- déclenchements, ou réserver Calculated Strike à la chaîne 805150) appartient à
-- l'exploitant.
--
-- PRISE D'EFFET : pas de rechargement à chaud. `spell_script_names` n'est lu qu'une fois,
-- par ObjectMgr::LoadSpellScriptNames (src/server/game/Globals/ObjectMgr.cpp:6347), et
-- aucune commande `.reload` ne le couvre (vérifié : aucune occurrence dans
-- src/server/scripts/Commands/). Il faut redémarrer le worldserver — ce qui est de toute
-- façon nécessaire, la moitié C++ demandant une reconstruction du binaire.
-- Retour arrière : le DELETE seul, puis redémarrage.
-- =====================================================================================

DELETE FROM `spell_script_names`
 WHERE `ScriptName` = 'spell_ascension_guardian_ability'
   AND `spell_id` IN (801772, 501066, 501067, 572717, 572718, 572719, 574341, 574363, 574364);

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(801772, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 1
(501066, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 2
(501067, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 3
(572717, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 4
(572718, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 5
(572719, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 6
(574341, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 7
(574363, 'spell_ascension_guardian_ability'),  -- Ballad of the Dragonslayer, Rank 8
(574364, 'spell_ascension_guardian_ability');  -- Ballad of the Dragonslayer, Rank 9
