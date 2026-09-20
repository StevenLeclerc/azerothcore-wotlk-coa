-- =====================================================================================
-- mod-ascension-compat — RUNEMASTER / Genesis (500501), arbre Riftblade.
--
-- « Apply a runic brand to an enemy for $d, accumulating 50% of your damage dealt, and
--   unleashing after $d as Elemental Damage. When this deals damage, it triggers your
--   Weapon Engraving. »
--
-- Accompagne `src/AscensionRunemasterGenesis.cpp`.
--
-- ⚠ CE FICHIER EXIGE LE BINAIRE QUI PORTE CES DEUX SCRIPTS. Appliqué avant, le serveur
--   signale au démarrage « ScriptName ... does not exist » et les deux lignes ne servent
--   à rien. L'ordre est : construire, installer, PUIS appliquer ce fichier.
--
-- Rappel P-051 : un script enregistré en C++ mais absent de `spell_script_names` ne
-- tourne JAMAIS, et ne coûte qu'une ligne de log. Ces deux lignes sont la moitié
-- manquante du correctif, pas un accessoire.
--
-- Le nom de ce fichier est la clé d'update : NE PLUS LE RENOMMER.
-- Prise d'effet à chaud : `.reload spell_script_names` (les scripts eux-mêmes viennent
-- du binaire ; la passe de métadonnées, elle, ne se recharge qu'au redémarrage).
--
-- -------------------------------------------------------------------------------------
-- Relevé du 2026-09-20 (lecture seule, avant toute écriture) :
--
--   Spell.dbc :
--     500501  famille 38, DurationIndex 31 = 8000 ms, ProcFlags 0, StackAmount 0.
--             Effet 0  APPLY_AURA / aura 4 DUMMY, EffectTriggerSpell 500502
--             Effet 1  APPLY_AURA / aura 69,      MiscValue 127, MiscValueB 50, montant 1
--             Effet 2  APPLY_AURA / aura 4 DUMMY
--     500502  famille 38, « Genesis / Damage », SchoolMask 28 (Feu|Nature|Givre, soit
--             l'« Elemental » de l'infobulle), un seul SPELL_EFFECT_SCHOOL_DAMAGE de
--             valeur de base 1 : un porteur de montant calculé.
--
--   Base : AUCUNE ligne pour 500501 ni 500502 dans `spell_script_names`, `spell_proc`,
--          `spell_linked_spell`, `spell_bonus_data` ni `spell_custom_attr`. Ce fichier
--          n'écrase donc rien.
--
--   Aucune ligne `spell_proc` n'est nécessaire : rien ici ne procède par proc. La marque
--   accumule depuis UNITHOOK_ON_DAMAGE et se paie à son expiration naturelle.
--
-- -------------------------------------------------------------------------------------
-- LE PIÈGE DE L'EFFET 1, corrigé en C++ et pas ici — à savoir avant de croire la donnée
-- innocente.
--
--   L'aura 69 n'est PAS une aura privée Ascension : c'est la SPELL_AURA_SCHOOL_ABSORB du
--   cœur (SpellAuraDefines.h:132), avec un vrai handler (SpellAuraEffects.cpp:134). Telle
--   qu'écrite, la marque poserait sur l'ennemi un bouclier de UN point, toutes écoles
--   (MiscValue 127), et Unit.cpp:2513 retire l'aura ENTIÈRE dès qu'un montant d'absorption
--   tombe à zéro :
--       if (absorbAurEff->GetAmount() <= 0) absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
--   Le premier point de dégâts subi par la cible supprimerait donc Genesis, avec un mode
--   de retrait qui n'est pas AURA_REMOVE_BY_EXPIRE : jamais 8 s, jamais de paiement.
--   `runemaster_genesis_metadata` bascule cet effet en DUMMY au chargement ; son
--   MiscValueB (= les 50 % de l'infobulle) reste lisible.
-- =====================================================================================

DELETE FROM `spell_script_names` WHERE `spell_id` IN (500501, 500502)
    AND `ScriptName` IN ('aura_ascension_runemaster_genesis',
                         'spell_ascension_runemaster_genesis_damage');

INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
-- La marque : accumule les dégâts infligés à la cible marquée, et libère 50 % du total
-- à l'expiration NATURELLE (ni dissipation, ni mort de la cible).
(500501, 'aura_ascension_runemaster_genesis'),
-- La charge utile : porte le montant calculé, et déclenche la ou les gravures d'arme du
-- joueur sur la cible touchée (« When this deals damage, it triggers your Weapon
-- Engraving »). C'est OnHit qui sait ce que le coup a réellement infligé, ce dont la
-- réplique Air (653226) a besoin pour calculer sa part.
(500502, 'spell_ascension_runemaster_genesis_damage');
