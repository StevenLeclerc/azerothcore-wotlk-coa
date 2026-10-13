-- Artisanat, lot T0 (D-PO-34) : etat des talents d'artisanat par personnage (AscensionArtisanat.cpp).
-- coa_artisanat_personnage : metiers principaux (SkillLine) et points GAGNES par arbre ; les points depenses se
-- deduisent des talents connus (acore_world.coa_artisanat_talent_def, ligne par rang, cle = sort marqueur).
CREATE TABLE IF NOT EXISTS `coa_artisanat_personnage` (
  `guid` INT UNSIGNED NOT NULL,
  `metier_creation` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `metier_recolte` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `points_creation` INT UNSIGNED NOT NULL DEFAULT 0,
  `points_recolte` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Sorts marqueurs connus (un par rang appris).
CREATE TABLE IF NOT EXISTS `coa_artisanat_talent` (
  `guid` INT UNSIGNED NOT NULL,
  `sort` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `sort`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
