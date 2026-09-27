-- =====================================================================================
-- mod-ascension-compat — Knight of Xoroth : Shieldgore « High Threat Fire Damage ».
--
-- Le tooltip de Shieldgore (804353, rangs 806869-806874, remplaçant d'Infernal Strike par
-- le talent DEFIANCE 301302) promet « High Threat », et rien ne le portait :
--   * acore_world.spell_threat n'avait aucune ligne pour ces sept ids (lu le 2026-09-27) ;
--   * e0 est un DUMMY (effet 3) de valeur 0, sans script ; aucun code du module ne majore
--     la menace de ce sort.
--
-- Multiplicateur choisi : pctMod 2.0, la valeur que ce même serveur donne déjà aux sorts
-- Blizzard dont le tooltip dit « causes a high amount of threat » : Searing Pain 5676 et
-- Frost Shock 8056 (spell_threat : flatMod 0, pctMod 2, apPctMod 0, lu le 2026-09-27).
-- Aucun chiffre Ascension n'existe ; c'est une décision d'équilibrage, révisable ici seul.
--
-- Idempotent. Donnée seule : le cœur relit spell_threat au démarrage (SpellMgr::LoadSpellThreats).
-- =====================================================================================
DELETE FROM `spell_threat` WHERE `entry` IN (804353, 806869, 806870, 806871, 806872, 806873, 806874);
INSERT INTO `spell_threat` (`entry`, `flatMod`, `pctMod`, `apPctMod`) VALUES
(804353, 0, 2, 0),
(806869, 0, 2, 0),
(806870, 0, 2, 0),
(806871, 0, 2, 0),
(806872, 0, 2, 0),
(806873, 0, 2, 0),
(806874, 0, 2, 0);
