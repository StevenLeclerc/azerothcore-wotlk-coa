-- Artisanat, Sourcier, lot T2 (21/10/2026, integration) : les sessions des mini-evenements du Sourcier, persistees.
-- Une ligne par session ouverte (Filon-mere, Faille tellurique, Jardin d'orage, Floraison des tombes, Traque legendaire,
-- Appel du Linceul), etat « en_cours » puis la raison de sa fin (succes, rendu, echec : directeur_absent, temps, ...).
--
-- Deux usages (CoaSourcier::OuvrirSession / FermerSession / RendreSessionsInterrompues, AscensionArtisanatSourcier.cpp) :
--   1. au demarrage du monde, toute ligne encore « en_cours » est une session tuee par un redemarrage : le lancement du
--      specialiste est RENDU (jour_lance et relance_ts du registre coa_artisanat_sourcier remis a jour_avant et
--      relance_avant, si le registre porte encore le jour de cette session) et la ligne passe « interrompu » ;
--   2. le Directeur (exploitation/directeur/sourcier_fin.py) la lit a chaque passe : session du module finie (rendue,
--      echouee, interrompue) -> il retire ses vagues au lieu de jouer jusqu'a son echeance.
-- Detectee au demarrage (absente : rien n'est ecrit, comportement d'avant). Menage : 30 jours d'historique.
--
-- Rejouable (IF NOT EXISTS). Retour : DROP TABLE IF EXISTS `coa_artisanat_sourcier_session`;
CREATE TABLE IF NOT EXISTS `coa_artisanat_sourcier_session` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `pivot` BIGINT UNSIGNED NOT NULL COMMENT 'ObjectGuid brut de l''objet pivot (objet ou creature)',
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid du specialiste',
    `evenement` VARCHAR(32) NOT NULL COMMENT 'filon_mere, faille_tellurique, jardin_orage, floraison_tombes, traque_legendaire, appel_linceul',
    `debut` INT UNSIGNED NOT NULL COMMENT 'instant du sort (UTC, epoch)',
    `etat` VARCHAR(24) NOT NULL DEFAULT 'en_cours',
    `fin` INT UNSIGNED NOT NULL DEFAULT 0,
    `jour_avant` INT NOT NULL DEFAULT 0 COMMENT 'registre AVANT ce lancement : jour_lance',
    `relance_avant` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'registre AVANT ce lancement : relance_ts',
    PRIMARY KEY (`id`),
    KEY `idx_guid_evenement` (`guid`, `evenement`),
    KEY `idx_etat` (`etat`),
    KEY `idx_pivot` (`pivot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='CoA artisanat : sessions des mini-evenements du Sourcier';
