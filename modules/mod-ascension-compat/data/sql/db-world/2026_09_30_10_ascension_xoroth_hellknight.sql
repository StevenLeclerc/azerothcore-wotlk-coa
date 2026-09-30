-- Knight of Xoroth (classe 17) : Hellknight 800703, charge de Champion of the Legion 800702.
-- La vitesse de deplacement promise par l'infobulle ($s1 % par charge) est calculee dans
-- Unit::UpdateSpeed a partir de l'effet 0 (GCD) ; ce script rafraichit la vitesse a chaque
-- application, changement de charges et retrait (AscensionXorothAuras.cpp).
-- spell_script_names n'est lu qu'au demarrage : appliquer AVANT de relancer le monde,
-- avec le binaire qui porte aura_ascension_xoroth_hellknight.

DELETE FROM `spell_script_names` WHERE `ScriptName` = 'aura_ascension_xoroth_hellknight';
INSERT INTO `spell_script_names` (`spell_id`,`ScriptName`) VALUES
(800703,'aura_ascension_xoroth_hellknight');
