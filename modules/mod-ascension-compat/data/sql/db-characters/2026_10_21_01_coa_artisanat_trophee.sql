-- Artisanat, Sourcier, lot T2 DEPECAGE (21/10/2026) : compteur quotidien de la Lame de trophee (case sourcier-c du
-- Depecage, docs/CONCEPTION-artisanat-T2-sourcier-60.md § 2 bis). Une ligne par personnage : jour de jeu (indice depuis
-- 1970, remise a 06:00 Paris, CoaSourcier::JourDeJeu) et trophees poses au butin ce jour-la (TROPHEES_PAR_JOUR au plus).
-- Lue par AscensionArtisanatSourcierDepecage.cpp, qui la detecte au demarrage (absente : la Lame est INERTE).
--
-- Table A PART, et non deux colonnes de coa_artisanat_sourcier comme le proposait la conception : EcrireRegistre
-- (AscensionArtisanatSourcier.cpp) ecrit ce registre par REPLACE, qui remet toute colonne non nommee a son defaut ; chaque
-- lancement ou recompense de mini-evenement aurait remis le compteur de trophees a zero.
--
-- Rejouable (IF NOT EXISTS). Retour : DROP TABLE IF EXISTS `coa_artisanat_trophee`; (les trophees deja remis restent).
CREATE TABLE IF NOT EXISTS `coa_artisanat_trophee` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `jour` INT NOT NULL DEFAULT 0 COMMENT 'jour de jeu du compteur (remise 06:00 Paris)',
    `n` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'trophees poses ce jour',
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CoA artisanat : Lame de trophee, compteur quotidien';
