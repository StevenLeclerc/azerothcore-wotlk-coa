-- Artisanat, Sourcier (revue du 16/10/2026, constat 12, D-PO-51) : la relance immediate apres une mort causee par un humain
-- d'une autre equipe n'est accordee qu'une fois par jour de jeu. relance_jcj_jour : jour de jeu (indice depuis 1970, remise a
-- 06:00 Paris) ou elle a ete accordee ; 0 : jamais. Lue par AscensionArtisanatSourcier.cpp, qui la detecte au demarrage
-- (absente : aucune relance immediate, echec ordinaire). Rejouable : la colonne n'est ajoutee que si elle manque.
SET @colonne_existe := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'coa_artisanat_sourcier' AND COLUMN_NAME = 'relance_jcj_jour');
SET @requete := IF(@colonne_existe = 0,
    'ALTER TABLE `coa_artisanat_sourcier` ADD COLUMN `relance_jcj_jour` INT NOT NULL DEFAULT 0 AFTER `recomp_n`',
    'DO 0');
PREPARE ajout FROM @requete;
EXECUTE ajout;
DEALLOCATE PREPARE ajout;
