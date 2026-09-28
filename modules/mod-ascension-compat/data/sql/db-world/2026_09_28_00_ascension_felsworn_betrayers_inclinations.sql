-- Felsworn - Betrayer's Inclinations (804758, ClassSpells level 12).
-- Effect 2 is SPELL_AURA_PROC_TRIGGER_SPELL (42) towards Betrayer's Blow (804766), ProcChance 10,
-- but Spell.dbc ProcFlags = 0 and there was no spell_proc row: SpellMgr::LoadSpellProcs skips a
-- spell without DBC proc flags (SpellMgr.cpp:2248-2250), so the blow never fired.
-- 804758 is in neither FelswornEvents nor FelswornCastDrivers: ApplyContracts leaves it alone.
-- ProcFlags 20 = DONE_MELEE_AUTO_ATTACK (0x4) | DONE_SPELL_MELEE_DMG_CLASS (0x10);
-- SpellTypeMask 1 = DAMAGE; SpellPhaseMask 2 = HIT; HitMask 0 = default (normal and critical).
-- Apply by hand, then `.reload spell_proc` and `.reload conditions`.
DELETE FROM `spell_proc` WHERE `SpellId` = 804758;
INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`,
    `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`,
    `DisableEffectsMask`, `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES (804758, 0, 0, 0, 0, 0, 20, 1, 2, 0, 0, 0, 0, 10, 0, 0);

-- "against Demons": 804766 carries TargetCreatureType 4 (Demon) and would fail on anything else;
-- the condition keeps the 10 % roll for demon targets. CONDITION_SOURCE_TYPE_SPELL_PROC (24),
-- CONDITION_CREATURE_TYPE (24), ConditionTarget 1 = action target (SpellAuras.cpp:2210-2211),
-- ConditionValue1 3 = CREATURE_TYPE_DEMON.
DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 24 AND `SourceEntry` = 804758;
INSERT INTO `conditions` (`SourceTypeOrReferenceId`, `SourceGroup`, `SourceEntry`, `SourceId`, `ElseGroup`,
    `ConditionTypeOrReference`, `ConditionTarget`, `ConditionValue1`, `ConditionValue2`, `ConditionValue3`,
    `NegativeCondition`, `ErrorType`, `ErrorTextId`, `ScriptName`, `Comment`)
VALUES (24, 0, 804758, 0, 0, 24, 1, 3, 0, 0, 0, 0, 0, '', 'Betrayer''s Inclinations: proc only against Demons');
