-- =====================================================================================
-- mod-ascension-compat — Templar (classe 19, famille de sorts 25) : nettoyage du
-- 2026-09-28. Moitié « donnée » des correctifs de AscensionTemplarContracts.cpp,
-- AscensionTemplarEvents.cpp et AscensionTemplarAuras.cpp.
--
-- Conventions lues dans le cœur (SpellMgr.cpp:2110-2150, SpellMgr.h:114-184, 261-262) :
--   ProcFlags 0x8 TAKEN_MELEE_AUTO_ATTACK, 0x20 TAKEN_SPELL_MELEE_DMG_CLASS ;
--   HitMask 0x10 DODGE, 0x20 PARRY ; SpellPhaseMask 0 pour des drapeaux TAKEN seuls
--   (REQ_SPELL_PHASE_PROC_FLAG_MASK ne couvre que les coups donnés) ;
--   les ProcFlags de la ligne priment sur ceux du SpellInfo (mis à 0 par le contrat).
--
-- Idempotent. Updates.EnableDatabases = 0 : appliquer à la main. spell_proc se recharge à
-- chaud (« reload spell_proc ») ; spell_script_names n'est lu qu'au démarrage : redémarrer
-- le monde, avec le binaire qui porte les scripts.
-- =====================================================================================

-- 1. Keeping the Oath 92109, effet 2 (aura 42 -> 681170 Recently Parried or Dodged) :
--    le CasterAuraSpell des six rangs de Sacred Swing. Esquive ou parade subie seulement.
--    DisableEffectsMask 1 : l'absorption (effet 0, posée par le contrat) ne passe pas par le
--    proc ; seul l'effet 2 (aura 42) le porte. Pas 3 : l'effet 1 est vidé par le contrat, et
--    SpellMgr.cpp:2155 crie pour un bit posé sur un effet qui n'est pas une aura.
DELETE FROM `spell_proc` WHERE `SpellId` IN (92109, 500752);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`)
VALUES
    (92109, 0, 0, 0, 0, 0, 40, 0, 0, 48, 0, 1, 0, 100, 0, 0),
-- 2. Scarlet Hammer 500752 (affaiblissement sur l'ennemi) : les attaques automatiques du
--    Templar contre la cible ont « a chance » de lancer Scarlet Restoration 807182.
--    Chance non chiffrée par l'infobulle : 15 PPM, la valeur de Judgement of Light
--    (spell_proc 20185), le même dessin « each melee attack against the target ».
--    DisableEffectsMask 1 : seul l'effet 1 (DUMMY) porte le proc.
    (500752, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 1, 15, 0, 0, 0);

-- 3. Scripts : 500752 passe par aura_ascension_templar_event (Check/Proc), 804228 par
--    aura_ascension_templar_resolve (insensibilité au charme).
DELETE FROM `spell_script_names` WHERE `spell_id` = 500752 AND `ScriptName` = 'aura_ascension_templar_event';
DELETE FROM `spell_script_names` WHERE `spell_id` = 804228 AND `ScriptName` = 'aura_ascension_templar_resolve';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
    (500752, 'aura_ascension_templar_event'),
    (804228, 'aura_ascension_templar_resolve');
