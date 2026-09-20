"""Check that every Runemaster Weapon Engraving has a working proc path.

The chain of an engraving is: player ability (ENCHANT_ITEM_TEMPORARY) -> enchant
(SpellItemEnchantment Effect 3 = EQUIP_SPELL) -> passive spell carrying a proc aura
-> payload. AzerothCore only builds a default proc entry when the DBC ProcFlags are
non-zero (SpellMgr.cpp, "Skip if no proc flags in DBC"); all six passives have
ProcFlags = 0, so each one needs an explicit `spell_proc` row, and the three whose
proc aura lives in a separate "Passive2" spell also need a `spell_linked_spell`
row to apply it at all.

Five of the six were silently dead until 2026-09-20 (see INCIDENTS P-053).

No server build, database or game client is needed.
Run: python -B modules/mod-ascension-compat/tests/test_weapon_engravings.py \
        --dbc-dir /opt/coa/server/data/dbc
"""

import argparse
from pathlib import Path
import re
import struct
import unittest

MODULE = Path(__file__).resolve().parents[1]
DBC_DIR = None

# ability -> (element, enchant id, proc carrier spell, applied by the enchant itself?)
ENGRAVINGS = {
    653022: ('Fire', 1000, 653211, True),
    653264: ('Ice', 1004, 653266, True),
    653218: ('Earth', 1002, 653221, False),
    653222: ('Air', 999, 653225, False),
    653213: ('Water', 1001, 653216, False),
    653265: ('Arcane', 1006, 653267, True),
}

# Fire's proc row predates this module's SQL: it is part of the base world data.
PROC_ROW_FROM_BASE_DATA = {653211}

# Air's proc effect is the Ascension-private aura 354, which the core cannot execute
# on its own (isTriggerAura[354] is false and there is no native handler). A row alone
# would do nothing, and applying 653225 would also double the haste 653223 grants.
NEEDS_CPP = {653225}

PROC_AURAS = (42, 231, 354)


class DBC:
    def __init__(self, path):
        self.data = path.read_bytes()
        magic, self.rc, self.fc, self.rs, self.ss = struct.unpack_from('<4sIIII', self.data, 0)
        assert magic == b'WDBC', path
        self.body = 20
        self.strings = self.body + self.rc * self.rs

    def rows(self):
        count = self.rs // 4
        for i in range(self.rc):
            yield struct.unpack_from('<%di' % count, self.data, self.body + i * self.rs)

    def by_id(self):
        return {row[0]: row for row in self.rows()}


def module_sql():
    return '\n'.join(p.read_text() for p in sorted(
        (MODULE / 'data/sql/db-world').glob('*.sql')))


def proc_rows(sql):
    """SpellId -> ProcFlags, from the INSERTs of `spell_proc` in the module's SQL."""
    rows = {}
    for block in re.findall(r'INSERT\s+INTO\s+`?spell_proc`?\s*\((.*?)\)\s*VALUES(.*?);',
                            sql, re.S | re.I):
        columns = [c.strip(' `\n') for c in block[0].split(',')]
        for values in re.findall(r'\(([^()]*)\)', block[1]):
            cells = [c.strip() for c in values.split(',')]
            if len(cells) != len(columns):
                continue
            entry = dict(zip(columns, cells))
            rows[int(entry['SpellId'])] = int(entry['ProcFlags'])
    return rows


def linked_rows(sql):
    """(trigger, effect) -> type, from the INSERTs of `spell_linked_spell`."""
    rows = {}
    for block in re.findall(r'INSERT\s+INTO\s+`?spell_linked_spell`?\s*\(.*?\)\s*VALUES(.*?);',
                            sql, re.S | re.I):
        for values in re.findall(r'\((-?\d+),\s*(-?\d+),\s*(\d+)\s*,', block):
            rows[(int(values[0]), int(values[1]))] = int(values[2])
    return rows


class WeaponEngravings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spell = DBC(DBC_DIR / 'Spell.dbc').by_id()
        cls.enchant = DBC(DBC_DIR / 'SpellItemEnchantment.dbc').by_id()
        sql = module_sql()
        cls.procs = proc_rows(sql)
        cls.links = linked_rows(sql)

    def carrier_aura_effect(self, spell_id):
        row = self.spell[spell_id]
        return [e for e in range(3) if row[71 + e] and row[95 + e] in PROC_AURAS]

    def test_ability_enchant_and_equip_spell_exist(self):
        """Each ability enchants the weapon, and the enchant applies a passive that exists."""
        for ability, (element, enchant_id, _carrier, _direct) in ENGRAVINGS.items():
            row = self.spell[ability]
            self.assertEqual(row[71], 54, f'{element}: effect 0 is not ENCHANT_ITEM_TEMPORARY')
            self.assertEqual(row[110], enchant_id, f'{element}: wrong enchant id')
            enchant = self.enchant[enchant_id]
            self.assertEqual(enchant[2], 3, f'{element}: enchant effect is not EQUIP_SPELL')
            self.assertIn(enchant[11], self.spell, f'{element}: equip spell missing from Spell.dbc')

    def test_every_carrier_has_a_proc_path(self):
        """A carrier with ProcFlags 0 and no `spell_proc` row can never fire."""
        for ability, (element, _enchant, carrier, _direct) in ENGRAVINGS.items():
            with self.subTest(element=element):
                self.assertTrue(self.carrier_aura_effect(carrier),
                                f'{element}: {carrier} carries no proc aura')
                if carrier in NEEDS_CPP:
                    self.assertNotIn(carrier, self.procs,
                                     f'{element}: {carrier} now has a row — aura 354 still has no '
                                     'native handler, so update NEEDS_CPP and this test together')
                    continue
                dbc_flags = self.spell[carrier][34]
                flags = self.procs.get(carrier, 0) or dbc_flags
                if carrier in PROC_ROW_FROM_BASE_DATA:
                    continue
                self.assertTrue(flags, f'{element}: {carrier} has neither DBC ProcFlags nor a '
                                       '`spell_proc` row in the module SQL — it will never proc')

    def test_detached_carriers_are_applied(self):
        """Water, Earth and Air keep their proc aura in a spell the enchant does not apply."""
        for ability, (element, enchant_id, carrier, direct) in ENGRAVINGS.items():
            if direct:
                continue
            equip_spell = self.enchant[enchant_id][11]
            with self.subTest(element=element):
                self.assertNotEqual(equip_spell, carrier,
                                    f'{element}: marked detached but the enchant applies it')
                if carrier in NEEDS_CPP:
                    continue
                self.assertEqual(self.links.get((equip_spell, carrier)), 2,
                                 f'{element}: no type 2 (SPELL_LINK_AURA) row applying {carrier}; '
                                 f'{equip_spell} is applied by the enchant but carries no proc aura')

    def test_proc_chance_is_never_zero(self):
        """Chance 0 in `spell_proc` falls back to the DBC ProcChance; a zero there is a dead proc."""
        for ability, (element, _enchant, carrier, _direct) in ENGRAVINGS.items():
            if carrier in NEEDS_CPP:
                continue
            with self.subTest(element=element):
                self.assertGreater(self.spell[carrier][35], 0,
                                   f'{element}: {carrier} has ProcChance 0 in Spell.dbc')

    def test_payloads_exist(self):
        """Every payload named by a carrier's proc aura is a real spell."""
        for ability, (element, _enchant, carrier, _direct) in ENGRAVINGS.items():
            row = self.spell[carrier]
            for effect in self.carrier_aura_effect(carrier):
                with self.subTest(element=element, effect=effect):
                    self.assertIn(row[116 + effect], self.spell,
                                  f'{element}: payload {row[116 + effect]} missing from Spell.dbc')


def main():
    global DBC_DIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dbc-dir', required=True, type=Path,
                        help='client DBC set the server loads')
    args, rest = parser.parse_known_args()
    DBC_DIR = args.dbc_dir
    unittest.main(argv=[__file__] + rest, verbosity=2)


if __name__ == '__main__':
    main()
