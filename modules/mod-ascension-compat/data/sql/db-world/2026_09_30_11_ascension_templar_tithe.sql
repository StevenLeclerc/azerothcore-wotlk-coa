-- Templar (classe 19) : retrait de Tithe 804781, sort sans effet (effet 0 APPLY_AURA, aura 0 NONE ;
-- aucun script, aucune ligne spell_area ni conditions). Decision du commanditaire du 2026-09-30.
-- Le binaire ne l'enseigne plus (AscensionCustomClassData.h : ClassSpells et LegacyGeneratedClassSpells).
-- Cette table n'est lue par aucun code (seulement citee en commentaire par mod-playerbots) :
-- ligne retiree pour que la donnee dise la meme chose que le binaire.
-- Le fichier engendre data/sql/updates/pending_db_world/rev_1787754600000000000.sql du coeur
-- la recree s'il est rejoue : sans consequence, rien ne la lit.

SELECT COUNT(*) AS `tithe_avant` FROM `ascension_custom_class_spell` WHERE `class` = 19 AND `spell_id` = 804781;
DELETE FROM `ascension_custom_class_spell` WHERE `class` = 19 AND `spell_id` = 804781;
