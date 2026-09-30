-- Reaper : Tormented Souls 500481, « heals you for ... removing 1 stack » ne partait jamais.
-- Écart 2026-09-29-reaper-500481 (docs/ECARTS-classes-20260929.md).
--
-- LU Spell.dbc 500481 : e0 aura 87 (-10 % dégâts subis), e1 aura 42 PROC_TRIGGER_SPELL -> 500482
-- (Tormented Soul | Heal, effet 10 HEAL), ProcFlags 0, ProcCharges 0, StackAmount 0.
-- MESURÉ le 2026-09-30 : aucune ligne spell_proc ni spell_script_names pour 500481 / 500482.
-- ProcFlags 0 sans ligne spell_proc : le cœur n'engendre aucun événement (P-045, P-053).
--
-- La ligne ouvre le proc sur tout coup DIRECT subi, pas sur les dégâts périodiques :
--   0x8 mêlée auto + 0x20 sort de mêlée + 0x80 distance auto + 0x200 sort à distance
--   + 0x2000 sort négatif sans classe + 0x20000 sort magique négatif = 139944.
-- SpellTypeMask 1 (dégâts), phase 2 (impact), HitMask 0 = défaut du cœur pour un coup subi
-- (normal + critique : une esquive, une parade ou un coup entièrement absorbé ne consomment rien).
-- Charges 1 : « removing 1 stack » ; le dernier (et seul) consommé retire l'aura, donc aussi la
-- réduction de 10 % et Hard Bargain 572300 qui vit avec elle.
-- Le soin garde la valeur de 500482 (m1 + par niveau) ; la part $AP*.058 + $STA*.24 de
-- l'infobulle est ajoutée par le binaire (reaper_talent_events, AscensionReaperTalents.cpp).
-- Sans ce binaire, cette ligne marche déjà, avec le soin de base seulement.
--
-- Idempotent. Recharge à chaud : .reload spell_proc (les auras déjà posées gardent 0 charge
-- jusqu'à leur prochaine application).

DELETE FROM `spell_proc` WHERE `SpellId` = 500481;
INSERT INTO `spell_proc` (`SpellId`,`SchoolMask`,`SpellFamilyName`,`SpellFamilyMask0`,`SpellFamilyMask1`,`SpellFamilyMask2`,`ProcFlags`,`SpellTypeMask`,`SpellPhaseMask`,`HitMask`,`AttributesMask`,`DisableEffectsMask`,`ProcsPerMinute`,`Chance`,`Cooldown`,`Charges`) VALUES
(500481, 0, 0, 0, 0, 0, 139944, 1, 2, 0, 0, 0, 0, 0, 0, 1);
