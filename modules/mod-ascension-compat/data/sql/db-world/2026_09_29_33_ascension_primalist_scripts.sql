-- Primalist (classe 31, famille 37) : vague 5, scripts de sort et d'aura.
-- spell_script_names n'est lu qu'au demarrage : appliquer AVANT de relancer le monde,
-- avec le binaire qui porte ces classes (AscensionPrimalistTalents.cpp, AscensionPrimalistEvents.cpp).
-- Les talents traites par AllSpellScript (primalist_wave5_spells, AscensionPrimalistMountain.cpp)
-- ou par UnitScript n'ont pas besoin de ligne.

DELETE FROM `spell_script_names` WHERE `ScriptName` IN (
 'aura_ascension_primalist_mineralization','aura_ascension_primalist_shattering',
 'aura_ascension_primalist_infused_with_power','aura_ascension_primalist_totemic_echoes',
 'aura_ascension_primalist_totemic_crash','aura_ascension_primalist_mountain_avatar',
 'aura_ascension_primalist_earthen_avatar','aura_ascension_primalist_son_of_ursoc',
 'spell_ascension_primalist_soothing_touch','aura_ascension_primalist_sacred_grove',
 'aura_ascension_primalist_spiritual_frenzy','aura_ascension_primalist_spiritual_frenzy_pet');

INSERT INTO `spell_script_names` (`spell_id`,`ScriptName`) VALUES
(520464,'aura_ascension_primalist_mineralization'),
(520469,'aura_ascension_primalist_shattering'),
(503718,'aura_ascension_primalist_infused_with_power'),
(500937,'aura_ascension_primalist_totemic_echoes'),
(547213,'aura_ascension_primalist_totemic_crash'),
(707616,'aura_ascension_primalist_mountain_avatar'),
(680421,'aura_ascension_primalist_earthen_avatar'),
(806048,'aura_ascension_primalist_son_of_ursoc'),
(520841,'spell_ascension_primalist_soothing_touch'),
(800180,'aura_ascension_primalist_sacred_grove'),
(707367,'aura_ascension_primalist_spiritual_frenzy'),
(707369,'aura_ascension_primalist_spiritual_frenzy_pet');
