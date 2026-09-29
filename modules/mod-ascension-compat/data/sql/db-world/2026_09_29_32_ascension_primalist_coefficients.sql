-- Primalist (classe 31, famille 37) : vague 5, coefficients spell_bonus_data.
-- MESURE (Spell.dbc) : champ BonusMultiplier a 0 sur ces sorts, ou remis a 0 au chargement par
-- AscensionCompatData::StockCoefficientSpells (804002, 800178), et aucune ligne spell_bonus_data.
-- Sans ligne, Unit::SpellDamageBonusDone / SpellHealingBonusDone ne lisent ni la puissance des
-- sorts ni la puissance d'attaque (Unit.cpp:9276-9328, 10143-10181). Les valeurs sont celles des
-- infobulles. Une ligne sur le rang 1 couvre toute la chaine spell_ranks (GetFirstSpellInChain).
-- Colonnes : direct_bonus, dot_bonus, ap_bonus, ap_dot_bonus.
-- Lu au demarrage ou par `.reload spell_bonus_data`. A appliquer a la main.

DELETE FROM `spell_bonus_data` WHERE `entry` IN
 (803138,680448,803981,800135,681119,681130,680442,804002,806186,805462,800178,803974,805107,504824,505208);

INSERT INTO `spell_bonus_data` (`entry`,`direct_bonus`,`dot_bonus`,`ap_bonus`,`ap_dot_bonus`,`comments`) VALUES
-- Geode Barrage : les 10 rangs (500402..502777) n'infligent rien, ils declenchent 803138 (aura 227)
(803138,0.36,0,0.09,0,'Local Primalist: Geode Barrage stone (10 ranks via aura 227), "$spn*.36+$AP*.09"'),
(680448,1.0,0,0.1,0,'Local Primalist: Stoneshard (9 ranks), "$spn*1+$AP*.1"'),
(803981,0,0.20,0,0.06,'Local Primalist: Seismic Crash per tick (8 ranks), "$SP*.20+$AP*0.06"'),
(800135,0.3,0,0.2,0,'Local Primalist: Hand of the Earthmother (8 ranks), "$AP*0.2+$BH*0.3"'),
(681119,1.3,0,0,0,'Local Primalist: Terrasurge (9 ranks), "$spn*1.3"'),
(681130,0.355,0,0.155,0,'Local Primalist: Mountain Hammer (6 ranks), "$spn*0.355+$AP*0.155"'),
-- dot_bonus reste a 0 : l'effet periodique ne promet aucun coefficient
(680442,0.6,0,0,0,'Local Primalist: Seismic Tremor impact (5 ranks), "$spn*0.6"'),
(804002,0.349,0,0.085,0,'Local Primalist: Geode "$SP*0.349+$AP*.085"'),
-- Le terme $STA*1.25 de l'infobulle ne s'exprime pas ici : signale, non porte
(806186,0.65,0,0.17,0,'Local Primalist: Mountain Fury pull damage, $STA*1.25 NOT carried'),
-- Une ligne ne distingue pas les effets : le soin promet $BH*.3 deux fois, un seul est porte
(805462,0.3,0,0.28,0,'Local Primalist: Seismic Wave heal+damage (7 ranks), "$AP*.28+$BH*.3"'),
-- Effet arme (e0) : MeleeDamageBonusDone ne lit pas spell_bonus_data. Seul l'eclat (e1) est porte
(800178,0,0,0.35,0,'Local Primalist: Totemic Smash splash (7 ranks), "$AP*0.35"'),
(803974,0.2,0,0.1,0,'Local Primalist: Quake nature part (7 ranks), "$spn*.2+$AP*.1"'),
(805107,1.0,0,1.0,0,'Local Primalist: Earthmother''s Binding "$BH*1+$AP*1"'),
(504824,0,0,0,0.45,'Local Primalist: Grove Guardian enemy tick "$AP*.45"'),
(505208,0,0,0,0.25,'Local Primalist: Grove Guardian heal tick "$AP*.25"');
