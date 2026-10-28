-- Witch Hunter, Caltrops (503679) : le champ invoque la creature 506010, dont le modele
-- (CreatureDisplayInfo 401941 = World\Outland\PassiveDoodads\Caltrop01.m2) est la barricade
-- a pointes de la Citadelle des Flammes infernales : boite englobante 24 x 22.6 x 19 yd a l'echelle 1.
-- Le DBC (client et serveur) porte l'echelle 1.0 ; on la ramene a 0.1 (~2.4 yd de large, 1.9 de haut),
-- pour une zone d'effet de 5 yd (SpellRadius 8). Purement visuel. Pris en compte au demarrage
-- (`.reload creature_template` ne recharge pas creature_template_model).
UPDATE `creature_template_model` SET `DisplayScale` = 0.1 WHERE `CreatureID` = 506010 AND `CreatureDisplayID` = 401941;
