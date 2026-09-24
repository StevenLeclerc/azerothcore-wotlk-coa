-- Retire la condition d'entree CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION des
-- 115 definitions SANS VIES qui la portent.
--
-- LES 56 DEFINITIONS A VIES LA GARDENT, et c'est deliberé. IsPristine
-- (CoA.Challenges.Lifecycle.cpp) s'en sert pour decider si annuler un defi est
-- une annulation gratuite ou un ECHEC : la retirer de ces 56 leur rendrait en
-- silence une annulation gratuite qu'elles n'ont jamais eue. Porter cette
-- regle dans le code a ete essaye puis abandonne -- un test LOOTED code en dur
-- frappait aussi les 46 defis a vies qui n'ont JAMAIS porte la condition, donc
-- changeait le comportement de definitions que ce lot ne touche pas. La garder
-- dans la donnee, la ou elle a toujours ete, ne casse rien et n'ajoute aucun
-- code. Les defis a vies sont de toute facon la famille Hardcore / Ironman :
-- « depuis un personnage neuf » y est le dessin, pas un accident.
--
-- NE PAS APPLIQUER SEUL. Ce fichier fait partie d'un lot, et applique seul il
-- rouvre la faille de monnaie de P-129. Il exige, DEJA EN SERVICE :
--   1. db-characters/2026_09_24_00_mod_coa_challenges_startlevel.sql
--      (colonne startLevel sur coa_character_challenge)
--   2. le worldserver RECOMPILE avec les deux correctifs de
--      CoA.Challenges.Lifecycle.cpp : ecriture de startLevel a l'activation,
--      et garde-fou de CompleteAtLevelCap.
--   3. 2026_09_16_01_mod_coa_challenges_definitions.sql corrige lui aussi.
--      ATTENTION, la raison N'EST PAS l'updater : sur ce royaume
--      Updates.EnableDatabases = 0 (worldserver.conf), l'updater est eteint et
--      ces fichiers s'appliquent A LA MAIN. La raison est que ce fichier
--      engendre est la SOURCE des definitions : il porte les memes colonnes en
--      INSERT ... ON DUPLICATE KEY UPDATE conditions=VALUES(conditions), donc
--      toute reapplication -- a la main, sur un royaume neuf, ou le jour ou
--      l'updater sera rallume -- remet les 171 conditions en place.
--
-- POURQUOI. Les 221 definitions exigeaient toutes un personnage vierge : 171
-- reclamaient de n'avoir JAMAIS ouvert une fenetre de butin, 107 d'etre encore
-- niveau 1, et aucune n'echappait aux deux. Le drapeau LOOTED est pose a la
-- premiere fenetre de butin de la vie du personnage et il est definitif. 1472
-- des 2944 personnages le portaient : l'interface laissait cliquer, le serveur
-- refusait en code 8, et le refus n'arrivait au joueur que par un message de
-- chat. Voir P-128.
--
-- CE QUE LA CONDITION TENAIT SANS QUE CE SOIT ECRIT. Elle ne gardait pas que
-- l'entree : elle gardait aussi la monnaie, parce que CompleteAtLevelCap valide
-- tout defi actif a la connexion d'un personnage au plafond. C'est pour cela
-- que ce lot deplace l'invariant dans le CODE avant de toucher la donnee.
--
-- LEVEL_UP EST CONSERVE sur 107 definitions : il reste le garde-fou des defis
-- reellement concus pour etre menes depuis zero.
--
-- SUR LA VALEUR VIDE. Les definitions qui n'avaient que cette condition et se retrouvent
-- avec conditions = ''. EvaluateConditions sort sur `if (conds.empty())` et rend
-- un vecteur vide, donc aucune condition cassee ; CoAParse::Split ecarte par
-- ailleurs les jetons vides.
--
-- IDEMPOTENT : rejouable sans effet supplementaire.
-- REVERSIBLE : /opt/coa/backups/coa_challenge_definition-avant-assouplissement-20260924.sql

-- 1. le jeton au milieu ou en fin, precede d'un ';'
UPDATE `coa_challenge_definition`
SET `conditions` = REPLACE(`conditions`, ';CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION:0/0/0', '')
WHERE `lives` = 0 AND `conditions` LIKE '%;CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION:0/0/0%';

-- 2. le jeton en tete, suivi d'un ';'
UPDATE `coa_challenge_definition`
SET `conditions` = REPLACE(`conditions`, 'CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION:0/0/0;', '')
WHERE `lives` = 0 AND `conditions` LIKE 'CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION:0/0/0;%';

-- 3. le jeton seul : la colonne devient vide
UPDATE `coa_challenge_definition`
SET `conditions` = ''
WHERE `lives` = 0 AND `conditions` = 'CHALLENGE_CONDITIONS_TYPE_LOOT_INTERACTION:0/0/0';

-- 4. filet de securite : aucun ';' orphelin en tete, en fin, ou double
UPDATE `coa_challenge_definition` SET `conditions` = REPLACE(`conditions`, ';;', ';') WHERE `conditions` LIKE '%;;%';
UPDATE `coa_challenge_definition` SET `conditions` = TRIM(LEADING ';' FROM `conditions`) WHERE `conditions` LIKE ';%';
UPDATE `coa_challenge_definition` SET `conditions` = TRIM(TRAILING ';' FROM `conditions`) WHERE `conditions` LIKE '%;';
