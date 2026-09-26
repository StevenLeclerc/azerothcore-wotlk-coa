-- P-146 (26/09/2026) : retire de l'entraineur 200001 (« Book of Artisans ») les 11
-- recettes que le serveur juge invalides (SpellMgr::ComputeIsSpellValid).
--
--   968902-968906, 971024-971028 : « Sanguine Conqueror's ... » (forge, skill 164,
--       rang 270), effet CREATE_ITEM avec ItemType = 0 : aucun objet cree.
--   94043 : « Iron Bar Upgrade Kit » (alchimie, skill 171, rang 200), cree l'objet
--       121793, absent d'item_template.
--
-- Chaque bot qui tentait de les apprendre declenchait « Broken spell ... learning not
-- allowed » et DeleteSpellFromAllPlayers (deux DELETE de masse) : 37 refus pour
-- chaque recette de forge et 433 pour 94043 en 9 h.
-- Sauvegarde des lignes : /opt/coa/backups/trainer_spell-200001-11-recettes-20260926.sql
-- Idempotent. Prise en compte a chaud : `.reload trainer`.

DELETE FROM `trainer_spell`
WHERE `TrainerId` = 200001
  AND `SpellId` IN (94043,
                    968902, 968903, 968904, 968905, 968906,
                    971024, 971025, 971026, 971027, 971028);
