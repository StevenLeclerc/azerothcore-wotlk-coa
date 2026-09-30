-- Templar : retrait de Tithe 804781 des trois rotations de bots (base acore_playerbots).
-- La source, mod-playerbots/data/sql/playerbots/custom/playerbots_coa_rotations.sql, est corrigee
-- elle aussi (trois regles retirees) : si le fichier est rejoue, Tithe ne revient pas.
-- Ce fichier-ci retire les regles deja en base sans rejouer tout le fichier.
-- Les strategies sont relues au chargement des bots (CustomStrategy::LoadActionLines).
-- Nom de base explicite : a executer tel quel, depuis n'importe quelle base.

SELECT COUNT(*) AS `tithe_avant` FROM `acore_playerbots`.`playerbots_custom_strategy`
 WHERE `owner` = 0 AND `name` IN ('templar-crusader','templar-oathkeeper','templar-zealot')
   AND `action_line` LIKE 'buff missing::Tithe>cast buff::Tithe!%';
DELETE FROM `acore_playerbots`.`playerbots_custom_strategy`
 WHERE `owner` = 0 AND `name` IN ('templar-crusader','templar-oathkeeper','templar-zealot')
   AND `action_line` LIKE 'buff missing::Tithe>cast buff::Tithe!%';
