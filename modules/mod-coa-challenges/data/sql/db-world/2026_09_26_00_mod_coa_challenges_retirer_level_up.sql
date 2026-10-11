-- Retire la condition d'entree CHALLENGE_CONDITIONS_TYPE_LEVEL_UP (« etre encore
-- niveau 1 ») des 107 definitions qui la portent, A VIES OU NON.
--
-- POURQUOI. Demande du commanditaire, 2026-09-26 : les defis se declarent quand
-- on veut, a n'importe quel niveau. Apres 2026_09_24_01 (retrait de
-- LOOT_INTERACTION des definitions sans vies), LEVEL_UP bloquait encore
-- l'entree de tout personnage de niveau > 1 : Dereir (niveau 18) ne pouvait pas
-- declarer Adventure Mode (211), refus code 8 CONDITIONS_NOT_MET.
--
-- CETTE MIGRATION NE SUFFIT PAS SEULE. Le client evalue LEVEL_UP LUI-MEME a
-- partir de son ChallengeConditions.dbc (C_Challenge.CanActivateChallenge) et
-- grise le bouton « Activate » : « You do not meet the activation requirements
-- for this challenge ». Il faut aussi le DBC client sans ses lignes LEVEL_UP,
-- livre dans patch-ZZ.MPQ -- voir outils/retirer-level-up-dbc.py.
--
-- CE QUE CA NE ROUVRE PAS. L'invariant de paiement de P-129 est dans le CODE
-- depuis 1b48a01 : CompleteAtLevelCap ne valide un palier 1 que si
-- coa_character_challenge.startLevel = 1. Un defi declare au niveau 18 ne sera
-- donc jamais valide au plafond (et GrantRewards = 0 de toute facon).
--
-- CE QUE CA CHANGE POUR LES 99 DEFIS A VIES. IsPristine lit les conditions :
-- jusqu'ici, un personnage qui avait pris un niveau n'etait plus « vierge », et
-- annuler un defi a vies devenait un ECHEC. Desormais prendre un niveau ne
-- casse plus la virginite : l'annulation reste gratuite tant qu'il n'y a ni
-- mort ni objectif atteint (FreeCancelIfPristine = 1). C'est le comportement
-- voulu (« activer ou desactiver quand on veut »).
--
-- LOOT_INTERACTION EST CONSERVE sur les 56 definitions a vies (voir
-- 2026_09_24_01) : un personnage portant LOOTED reste refuse sur celles-la.
--
-- 2026_09_16_01_mod_coa_challenges_definitions.sql est corrige dans le meme
-- geste : c'est la SOURCE des definitions (INSERT ... ON DUPLICATE KEY UPDATE),
-- toute reapplication remettrait sinon les 107 conditions.
--
-- Appliquer puis recharger a chaud : `.coa challenges reload` (console RA).
-- IDEMPOTENT. REVERSIBLE :
--   /opt/coa/backups/coa_challenge_definition-avant-retrait-level-up-20260926-114827.sql

-- 1. le jeton au milieu ou en fin, precede d'un ';'
UPDATE `coa_challenge_definition`
SET `conditions` = REPLACE(`conditions`, ';CHALLENGE_CONDITIONS_TYPE_LEVEL_UP:0/0/0', '')
WHERE `conditions` LIKE '%;CHALLENGE_CONDITIONS_TYPE_LEVEL_UP:0/0/0%';

-- 2. le jeton en tete, suivi d'un ';'
UPDATE `coa_challenge_definition`
SET `conditions` = REPLACE(`conditions`, 'CHALLENGE_CONDITIONS_TYPE_LEVEL_UP:0/0/0;', '')
WHERE `conditions` LIKE 'CHALLENGE_CONDITIONS_TYPE_LEVEL_UP:0/0/0;%';

-- 3. le jeton seul : la colonne devient vide
UPDATE `coa_challenge_definition`
SET `conditions` = ''
WHERE `conditions` = 'CHALLENGE_CONDITIONS_TYPE_LEVEL_UP:0/0/0';

-- 4. filet de securite : aucun ';' orphelin en tete, en fin, ou double
UPDATE `coa_challenge_definition` SET `conditions` = REPLACE(`conditions`, ';;', ';') WHERE `conditions` LIKE '%;;%';
UPDATE `coa_challenge_definition` SET `conditions` = TRIM(LEADING ';' FROM `conditions`) WHERE `conditions` LIKE ';%';
UPDATE `coa_challenge_definition` SET `conditions` = TRIM(TRAILING ';' FROM `conditions`) WHERE `conditions` LIKE '%;';
