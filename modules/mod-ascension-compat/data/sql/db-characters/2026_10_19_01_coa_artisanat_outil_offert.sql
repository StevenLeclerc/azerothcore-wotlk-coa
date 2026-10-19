-- Artisanat : outil offert a l'apprentissage d'un metier (AscensionArtisanat.cpp, ArtisanatOutilOffert), 19/10/2026.
-- Une ligne par (personnage, metier) deja servi : desapprendre puis reapprendre ne redonne rien. objets : ids donnes
-- (vide si le joueur avait deja ses outils). Absente : le module ne donne rien (journal « table ABSENTE »).
-- Retour arriere : DROP TABLE IF EXISTS `coa_artisanat_outil_offert`; (puis redemarrer : le module relit la table au demarrage).
CREATE TABLE IF NOT EXISTS `coa_artisanat_outil_offert` (
  `guid` INT UNSIGNED NOT NULL,
  `metier` SMALLINT UNSIGNED NOT NULL,
  `objets` VARCHAR(64) NOT NULL DEFAULT '',
  `quand` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`guid`, `metier`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
