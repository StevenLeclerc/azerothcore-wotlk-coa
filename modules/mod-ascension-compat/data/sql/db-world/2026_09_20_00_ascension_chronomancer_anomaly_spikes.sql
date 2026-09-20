-- mod-ascension-compat : Chronomancer / Infinite (spé 32), talent « Anomaly Spikes ».
--
-- Nœud 6156, rangs 503825 (ProcChance 8) et 504886 (ProcChance 15) :
-- « Periodic damage dealt now has a $h% chance to launch an Anomaly Spike at your
-- target » (projectile 503826). Le talent ne partait jamais : l'effet 0 des deux
-- rangs est bien un APPLY_AURA / aura 42 SPELL_AURA_PROC_TRIGGER_SPELL vers 503826,
-- mais le champ ProcFlags de Spell.dbc vaut 0 pour les deux. Or SpellMgr.cpp:2248-2250
-- (« Skip if no proc flags in DBC ») n'engendre alors aucune SpellProcEntry, et
-- SpellAuras.cpp:2150-2154 (Aura::GetProcEffectMask) refuse toute aura sans entrée.
-- L'aura était posée et n'était jamais candidate à un proc. Incident P-045.
--
-- Cette table est le seul endroit qui peut fournir un ProcFlags sans recompiler :
-- SpellMgr::LoadSpellProcs lit la ligne et ne retombe sur le DBC que pour les
-- colonnes laissées à 0 (SpellMgr.cpp:2109-2115).
--
-- Valeur de chaque colonne, et pourquoi :
--   ProcFlags     = 262144 = 0x00040000 PROC_FLAG_DONE_PERIODIC, le seul drapeau
--                   levé par un tick de dégâts périodiques (SpellAuraEffects.cpp,
--                   HandlePeriodicDamageAurasTick et ses trois sœurs).
--   SpellTypeMask = 1 PROC_SPELL_TYPE_DAMAGE. PROC_FLAG_DONE_PERIODIC est aussi
--                   levé par un tick de SOIN périodique ; sans cette restriction un
--                   HoT lancerait des projectiles, contre l'infobulle.
--   SpellPhaseMask= 2 PROC_SPELL_PHASE_HIT. Obligatoire : PROC_FLAG_DONE_PERIODIC
--                   appartient à REQ_SPELL_PHASE_PROC_FLAG_MASK, et laisser 0 fait
--                   journaliser « proc will not be triggered ». Les ticks appellent
--                   Unit::ProcSkillsAndAuras avec procPhase par défaut = HIT.
--   HitMask       = 0, c'est-à-dire la valeur par défaut d'un proc DONE :
--                   NORMAL | CRITICAL | ABSORB (SpellMgr.cpp:944-963). Un tick
--                   entièrement immunisé ou nul ne déclenche donc rien.
--   Chance        = 0 : le cœur retombe sur le ProcChance du DBC, 8 % au rang 1 et
--                   15 % au rang 2. C'est ce qui préserve les deux rangs avec la
--                   même ligne, et ce qui laisse « Erasion » (704489) appliquer son
--                   SPELLMOD_CHANCE_OF_SUCCESS (Aura::CalcProcChance).
--   Charges       = 0 : le DBC porte ProcCharges 0, l'aura est permanente et n'est
--                   pas consommée par le proc.
--   SpellFamilyName / masques = 0 : aucune restriction de famille, comme le ferait
--                   le générateur par défaut du cœur (EffectSpellClassMask vide).
--   Cooldown      = 0 : l'infobulle n'annonce aucun délai interne.
--
-- Deux lignes positives, et non une ligne négative : un SpellId négatif signifie
-- « toute la chaîne de rangs » et suit spellInfo->GetNextRankSpell(). Or ces deux
-- sorts ne forment aucune chaîne — `spell_ranks` est vide pour eux, `talent_dbc`
-- aussi (0 ligne), et Talent.dbc ne porte pas le nœud 6156 : les rangs CoA viennent
-- de CharacterAdvancement.dbc, que SpellMgr::LoadSpellTalentRanks ne lit pas. Une
-- ligne -503825 ne couvrirait donc que 503825.
--
-- Le nom du fichier est la clé d'update enregistrée dans `updates` (state = MODULE) ;
-- ne pas le renommer une fois appliqué.

-- Le DELETE porte sur ABS(SpellId) : une ligne negative laissee par une version
-- anterieure designerait les memes sorts et masquerait silencieusement celles-ci.
DELETE FROM `spell_proc` WHERE ABS(`SpellId`) IN (503825, 504886);
INSERT INTO `spell_proc`
    (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`, `SpellFamilyMask1`, `SpellFamilyMask2`,
     `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`, `HitMask`, `AttributesMask`, `DisableEffectsMask`,
     `ProcsPerMinute`, `Chance`, `Cooldown`, `Charges`) VALUES
(503825, 0, 0, 0, 0, 0, 262144, 1, 2, 0, 0, 0, 0, 0, 0, 0),
(504886, 0, 0, 0, 0, 0, 262144, 1, 2, 0, 0, 0, 0, 0, 0, 0);
