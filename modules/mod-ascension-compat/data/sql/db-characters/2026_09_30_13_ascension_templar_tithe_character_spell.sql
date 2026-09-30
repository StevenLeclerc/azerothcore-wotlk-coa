-- Templar : retrait de Tithe 804781 des grimoires (base acore_characters).
-- MESURE le 2026-09-30 : 64 lignes, 64 personnages ; 0 ligne dans character_action,
-- character_aura et character_spell_cooldown.
-- ORDRE : a appliquer MONDE ARRETE, et seulement quand le binaire installe ne porte plus
-- 804781 dans ClassSpells (sinon la connexion le reapprend a tout Templar de niveau >= 20 ;
-- et un personnage en ligne le reecrirait a sa prochaine sauvegarde).

-- Controle avant : doit rendre le nombre de lignes qui vont partir.
SELECT COUNT(*) AS `tithe_avant`, COUNT(DISTINCT `guid`) AS `personnages` FROM `character_spell` WHERE `spell` = 804781;

DELETE FROM `character_spell` WHERE `spell` = 804781;
DELETE FROM `character_action` WHERE `type` = 0 AND `action` = 804781;

-- Controle apres : doit rendre 0.
SELECT COUNT(*) AS `tithe_apres` FROM `character_spell` WHERE `spell` = 804781;
