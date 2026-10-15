-- Artisanat, lot T1 « Le Sourcier » (T1-4) : registre par personnage du Filon-mere (AscensionArtisanatSourcier.cpp).
-- Jours et semaines de jeu : indices depuis 1970 avec remise a 06:00 (Paris) ; semaine du mercredi 06:00 au mercredi.
-- relance_ts : instant UTC (secondes) a partir duquel une relance est accordee le jour meme (echec), 0 : aucune.
CREATE TABLE IF NOT EXISTS `coa_artisanat_sourcier` (
  `guid` INT UNSIGNED NOT NULL,
  `jour_lance` INT NOT NULL DEFAULT 0,
  `relance_ts` INT UNSIGNED NOT NULL DEFAULT 0,
  `essai2_jour` INT NOT NULL DEFAULT 0,
  `semaine_garanti` INT NOT NULL DEFAULT -1,
  `recomp_jour` INT NOT NULL DEFAULT 0,
  `recomp_n` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
