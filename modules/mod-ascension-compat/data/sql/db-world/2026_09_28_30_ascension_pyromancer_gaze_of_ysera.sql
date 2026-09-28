-- =====================================================================================
-- mod-ascension-compat — Pyromancer (classe 24, famille de sorts 30) : Gaze of Ysera,
-- 2026-09-28. Moitié « donnée » du correctif de AscensionPyromancerContracts.cpp
-- (bloc 806148 / 503229), qui retire AURA_INTERRUPT_FLAG_DIRECT_DAMAGE.
--
-- À APPLIQUER AVANT LE BINAIRE (puis « reload spell_proc ») : sans ces lignes, le binaire
-- corrigé laisse le sommeil intact sous les coups jusqu'à sa fin (40 s, plus aucune rupture).
-- L'inverse est sans danger : avec l'ancien binaire, ces lignes donnent au pire une double
-- suppression (drapeau d'interruption puis charge), que UnitAura::Remove tolère.
-- Contrôle après démarrage : 2 lignes pour SpellId IN (806148, 503229).
--
-- Texte du sort : « Direct damage dealt may interrupt this effect. »
-- Ysera's Blessing 704820, effet 1 : SPELLMOD_CHANCE_OF_SUCCESS -10 sur les deux sorts.
-- Aura::CalcProcChance (SpellAuras.cpp) applique ce modificateur du lanceur au Chance de
-- la ligne ; Chance 0 = ProcChance du DBC (806148 : 100 ; 503229 : 80), SpellMgr::LoadSpellProcs.
--   ProcFlags 139944 = 0x8|0x20|0x80|0x200|0x2000|0x20000 (coups subis : mêlée, distance,
--     sorts négatifs), sans TAKEN_PERIODIC : les dégâts périodiques ne rompent pas.
--   SpellTypeMask 1 = PROC_SPELL_TYPE_DAMAGE : un sort négatif sans dégât ne rompt pas.
--   SpellPhaseMask 0 : drapeaux TAKEN seuls (REQ_SPELL_PHASE_PROC_FLAG_MASK = DONE).
--   HitMask 0 : défaut des procs subis, NORMAL | CRITICAL.
--   AttributesMask 2 = PROC_ATTR_TRIGGERED_CAN_PROC : un coup déclenché rompt aussi, comme
--     le faisait l'ancien drapeau d'interruption.
--   Charges 1 : la rupture retire l'aura (Aura::ConsumeProcCharges).
--   DisableEffectsMask 6 : effets 1 (DUMMY, aura 4) et 2 (aura 87) exclus du proc. Seul
--     l'effet 0 (MOD_STUN) décide, et AuraEffect::CheckEffectProc exige des dégâts non nuls :
--     un sort entièrement absorbé ne rompt pas (sans ce masque, les effets 1 et 2 passaient
--     et la charge était consommée). Les trois effets sont des auras (effet 6) : aucun
--     LOG_ERROR de LoadSpellProcs.
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_proc se recharge
-- à chaud (« reload spell_proc »), mais le binaire doit porter le contrat corrigé.
-- =====================================================================================
DELETE FROM `spell_proc` WHERE `SpellId` IN (806148, 503229);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES
    (806148, 0, 0, 0, 0, 0, 139944, 1, 0, 0, 2, 6, 0, 0, 0, 1),
    (503229, 0, 0, 0, 0, 0, 139944, 1, 0, 0, 2, 6, 0, 0, 0, 1);
