-- =====================================================================================
-- mod-ascension-compat — Templar (classe 19) : Silverhand Incantation 801465, nettoyage du
-- 2026-09-28.
--
-- L'infobulle promet ${$m1+$ppl1+$AP*2} dégâts sacrés en zone et autant de soin aux alliés
-- proches. 801465 e0 (TRIGGER_MISSILE_SPELL_WITH_VALUE, bp 300) passe 300 à 801501
-- (e0 SCHOOL_DAMAGE, e1 HEAL, BonusMultiplier 0, aucune ligne ici) : 300 à tout niveau.
-- ap_bonus est lu dans les deux branches non-DOT (Unit::SpellDamageBonusDone et
-- Unit::SpellHealingBonusDone) ; direct_bonus 0 garde la puissance des sorts hors du calcul,
-- comme l'infobulle. Ne PAS ajouter aussi une ligne 801465/801501 à TemplarCoefficients :
-- l'AP serait compté deux fois.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main, puis
-- « .reload spell_bonus_data » (aucun binaire requis).
-- =====================================================================================

DELETE FROM `spell_bonus_data` WHERE `entry` = 801501;
INSERT INTO `spell_bonus_data` (`entry`, `direct_bonus`, `dot_bonus`, `ap_bonus`, `ap_dot_bonus`, `comments`) VALUES
    (801501, 0, 0, 2, 0, 'Templar Silverhand Incantation: $AP*2, damage and heal');
