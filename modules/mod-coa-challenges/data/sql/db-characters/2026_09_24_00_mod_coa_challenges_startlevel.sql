-- Ajoute `startLevel` a coa_character_challenge : le niveau qu'avait le
-- personnage au moment ou il a DECLARE le defi.
--
-- POURQUOI. CompleteAtLevelCap (CoA.Challenges.Lifecycle.cpp) valide tout defi
-- actif des qu'un personnage atteint le niveau plafond, y compris a la
-- connexion -- le commentaire d'origine le dit : « A character that logs in
-- already at the cap with an active challenge (or one activated at the cap)
-- completes it here. » C'etait sans danger uniquement parce que les conditions
-- d'entree (LOOT_INTERACTION / LEVEL_UP) rendaient l'activation impossible a un
-- personnage deja joue. Des qu'on assouplit l'entree, un personnage au plafond
-- peut declarer puis encaisser sans rien faire : P-129 a mesure l'exposition.
--
-- Le code lit desormais cette colonne et refuse de valider un defi declare
-- ALORS QUE le personnage etait deja au plafond.
--
-- VALEUR PAR DEFAUT 0, ET C'EST VOULU : les lignes ecrites avant l'existence de
-- la colonne valent 0, ce qui est sous n'importe quel plafond, donc elles
-- gardent exactement le comportement d'avant.
--
-- IDEMPOTENT : la colonne n'est ajoutee que si elle n'existe pas.

DROP PROCEDURE IF EXISTS `coa_add_startlevel`;
DELIMITER $$
CREATE PROCEDURE `coa_add_startlevel`()
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM `information_schema`.`COLUMNS`
        WHERE `TABLE_SCHEMA` = DATABASE()
          AND `TABLE_NAME`   = 'coa_character_challenge'
          AND `COLUMN_NAME`  = 'startLevel')
    THEN
        ALTER TABLE `coa_character_challenge`
            ADD COLUMN `startLevel` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `startTime`;
    END IF;
END$$
DELIMITER ;
CALL `coa_add_startlevel`();
DROP PROCEDURE `coa_add_startlevel`;
