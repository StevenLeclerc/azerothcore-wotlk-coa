-- =====================================================================================
-- mod-ascension-compat — WEAPON ENGRAVINGS du Runemaster : cinq gravures sur six
-- n'ont JAMAIS pu se déclencher. Réparation par la donnée seule.
--
-- Mesuré le 2026-09-20 (lecture seule, avant toute écriture) :
--
--   La chaîne d'une gravure est : capacité du joueur (ENCHANT_ITEM_TEMPORARY sur
--   l'arme) -> enchantement SpellItemEnchantment (Effect[0] = 3 EQUIP_SPELL) ->
--   sort passif porteur de l'aura 42 PROC_TRIGGER_SPELL -> charge utile.
--   Vérifiée bout en bout dans les DBC :
--     653022 Fire  (niv. 1)  -> enchant 1000 -> 653211 -> 653210 Firebrand
--     653264 Ice   (niv. 6)  -> enchant 1004 -> 653266 -> 653217 dégâts de Givre
--     653218 Earth (niv. 14) -> enchant 1002 -> 653219 (+ 653221) -> 653272 Nature en zone
--     653222 Air   (niv. 24) -> enchant  999 -> 653223 (+ 653225) -> 653226 réplique
--     653213 Water (niv. 26) -> enchant 1001 -> 653214 (+ 653216) -> 653261 vol de mana
--     653265 Arcane(niv. 40) -> enchant 1006 -> 653267 -> 653263 dégâts + marque
--
--   La chaîne casse au maillon « proc », et pour DEUX raisons distinctes :
--
--   (1) ProcFlags = 0 dans Spell.dbc pour les six passifs porteurs, et
--       SpellMgr.cpp:2246-2247 « Skip if no proc flags in DBC » : le cœur ne
--       fabrique AUCUNE entrée de proc par défaut. Seul 653211 Fire s'en sort,
--       parce qu'il a une ligne `spell_proc` explicite (ProcFlags 65876).
--       Mesuré : `SELECT … FROM spell_proc WHERE SpellId IN (…)` ne rend QUE 653211.
--       Aucune ligne dans `spell_dbc` (119 lignes, aucune de ces ids), aucune
--       ligne dans `spell_linked_spell`, aucun C++ du module ne les touche
--       (`grep` sur src/ : seul 653211 apparaît, AscensionRunemasterSecondary.cpp:39).
--
--   (2) Pour Water, Earth et Air, le sort porteur de l'aura 42 n'est PAS celui que
--       l'enchantement applique : l'enchant applique le « Passive » (653214, 653219,
--       653223), dont l'effet 0 est un DUMMY vide, tandis que l'aura de proc vit
--       dans un « Passive2 » séparé (653216, 653221, 653225) que RIEN n'applique.
--
-- Ce fichier répare (1) pour quatre gravures et (2) pour deux d'entre elles.
-- Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud : `.reload spell_proc` et `.reload spell_linked_spell`.
--
-- -------------------------------------------------------------------------------------
-- CE QUI N'EST PAS RÉPARÉ ICI, ET POURQUOI — à lire avant de croire la classe entière
-- remise d'aplomb.
--
--   * AIR (653225) est laissé de côté DANS CE FICHIER : son effet de proc est l'aura
--     privée 354, que le cœur ne sait pas exécuter (`isTriggerAura[354]` est faux,
--     SpellMgr.cpp:1984+, et `AuraEffectHandler[354]` vaut `nullptr`). Le module s'en
--     sert ailleurs, TOUJOURS avec un AuraScript (AscensionPrimalistSecondary.cpp:92,
--     AscensionRunemasterSecondary.cpp:221). Une ligne seule ne ferait rien.
--     => Air demande du C++, livré depuis dans AscensionRunemasterEngravings.cpp et
--        2026_09_20_05_ascension_gravures_riders.sql.
--
--     CORRECTION du 2026-09-20 : ce commentaire affirmait d'abord que lier
--     653223 -> 653225 « doublerait la hâte ». C'est FAUX, et la vérification est à
--     une ligne : l'aura 349 que porte 653223 n'a AUCUN handler dans le cœur
--     (`AuraEffectHandler[349] = nullptr`, « unknown Ascension aura »). Ses +10 % ne
--     s'appliquent donc pas du tout. C'est l'aura 216 HASTE_SPELLS de 653225 qui est
--     native : la lier AJOUTE la hâte d'incantation promise, elle ne la double pas.
--
--   * Les « riders » des charges utiles restent muets, faute de script :
--       - Ice : les dégâts de 653217 partent, mais NI le ralentissement 653231
--         (traité plus bas), NI la pile d'Icebound Momentum 712490 ;
--       - Water : 653261 draine (effet 8 POWER_DRAIN, 2 points) et déclenche
--         Replenishment 1257670 ; la restitution au lanceur et les dégâts de Givre
--         proportionnels au mana volé (charge utile 712492) demandent du C++ ;
--       - Arcane : les dégâts de 653263 partent et la marque s'applique, mais le
--         paiement à l'expiration (712493 « Stored Healing ») demande du C++.
--       - Earth : réparation COMPLÈTE — 653272 est un simple SCHOOL_DAMAGE en zone
--         (ImplicitTargetA 16), sans rider.
--
-- -------------------------------------------------------------------------------------
-- 1. LES DEUX AURAS DE PROC QUE RIEN N'APPLIQUE  (type 2 = SPELL_LINK_AURA)
--
-- SpellAuras.cpp:1259-1291 : à l'application de `spell_trigger`, le cœur fait
-- `caster->AddAura(spell_effect, target)` ; à son retrait, `RemoveAura`. C'est
-- exactement la sémantique voulue : la gravure vit et meurt avec l'enchantement.
-- Air (653223 -> 653225) est délibérément ABSENT — voir ci-dessus.
-- =====================================================================================

DELETE FROM `spell_linked_spell` WHERE `spell_trigger` IN (653214, 653219, 653217)
    AND `spell_effect` IN (653216, 653221, 653231);

INSERT INTO `spell_linked_spell` (`spell_trigger`, `spell_effect`, `type`, `comment`) VALUES
-- Water Engraving : le passif de l'enchant 1001 porte enfin son aura de proc.
(653214, 653216, 2, 'Runemaster Water Engraving - applique le Passive2 porteur du proc'),
-- Earth Engraving : idem pour l'enchant 1002.
(653219, 653221, 2, 'Runemaster Earth Engraving - applique le Passive2 porteur du proc'),
-- Ice Engraving : l'infobulle de 653266 promet « reduce the target movement speed by
-- ${0-$653231m1}% for $653231d » = 50 % pendant 8 s. 653231 existe (aura 33
-- MOD_DECREASE_SPEED, -50, durée 8 s) et N'EST LANCÉ PAR RIEN dans tout le DBC :
-- aucun sort ne le porte en TriggerSpell, aucun C++ ne le nomme. 653217 ne le
-- déclenche pas non plus. type 1 = SPELL_LINK_HIT (Spell.cpp:3330-3341) : la cible
-- touchée se l'applique, au crédit du lanceur — donc seulement en cas de coup porté.
-- REVERSIBLE SEUL : DELETE FROM spell_linked_spell WHERE spell_trigger=653217;
(653217, 653231, 1, 'Runemaster Ice Engraving - ralentissement promis par l infobulle');

-- =====================================================================================
-- 2. LES QUATRE LIGNES DE PROC
--
-- Gabarit : la ligne de 653211 Fire, en service et sans erreur au journal.
--   ProcFlags 65876 = 0x10154
--     0x00004 DONE_MELEE_AUTO_ATTACK      0x00010 DONE_SPELL_MELEE_DMG_CLASS
--     0x00040 DONE_RANGED_AUTO_ATTACK     0x00100 DONE_SPELL_RANGED_DMG_CLASS
--     0x10000 DONE_SPELL_MAGIC_DMG_CLASS_NEG
--   soit « tous les dégâts directs infligés ». DONE_PERIODIC (0x40000) est ABSENT :
--   c'est ce qui exclut les DoT, comme le fait le DoCheckProc de Fire en C++
--   (`damage->GetDamageType() != DOT`). Les infobulles disent bien « direct damage ».
--   SpellTypeMask 1 = PROC_SPELL_TYPE_DAMAGE — le garde-fou de P-045 : sans lui, un
--   tick de soin lève le même drapeau.
--   SpellPhaseMask 2 = HIT. HitMask 0 = normal + critique + absorbé.
--   Chance 0 : SpellMgr.cpp:2113-2114 retombe alors sur le ProcChance du DBC, qui est
--   NON NUL et correspond exactement au chiffre annoncé par chaque infobulle :
--     653216 Water 30 %   653221 Earth 15 %   653266 Ice 40 %   653267 Arcane 40 %
--   AttributesMask 0 : pas de PROC_ATTR_TRIGGERED_CAN_PROC, donc aucune boucle
--   possible (SpellAuras.cpp:2166-2173 refuse tout sort déclenché).
--   DisableEffectsMask 0, comme la ligne de Fire, dont l'effet 1 est aussi un
--   modificateur de sort (aura 108).
--
-- Aucune de ces quatre lignes n'écrase quoi que ce soit : les quatre ids sont absents
-- de `spell_proc` (vérifié), et aucun n'a de ligne dans `spell_script_names`.
-- =====================================================================================

DELETE FROM `spell_proc` WHERE `SpellId` IN (653216, 653221, 653266, 653267);

INSERT INTO `spell_proc` (`SpellId`, `SchoolMask`, `SpellFamilyName`, `SpellFamilyMask0`,
    `SpellFamilyMask1`, `SpellFamilyMask2`, `ProcFlags`, `SpellTypeMask`, `SpellPhaseMask`,
    `HitMask`, `AttributesMask`, `DisableEffectsMask`, `ProcsPerMinute`, `Chance`,
    `Cooldown`, `Charges`) VALUES
-- Water Engraving (Passive2) : 30 % des dégâts directs -> 653261, vol de mana + Replenishment.
(653216, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Earth Engraving (Passive2) : 15 % -> 653272, dégâts de Nature autour de la cible.
(653221, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Ice Engraving : 40 % -> 653217, dégâts de Givre (+ ralentissement, section 1).
(653266, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 0, 0, 0, 0, 0),
-- Arcane Engraving : 40 % -> 653263, dégâts d'Arcane + marque de 5 s.
-- Cooldown 5000 : l'infobulle dit « This can not occur more than once every 5 sec. »
-- C'est la seule des quatre à porter une limite de cadence, et elle est écrite.
(653267, 0, 0, 0, 0, 0, 65876, 1, 2, 0, 0, 0, 0, 0, 5000, 0);
