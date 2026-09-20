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

# Carriers whose proc aura the core cannot execute on its own, and the script the module
# must therefore name in `spell_script_names`. Air's aura 354 has AuraEffectHandler[354]
# == nullptr: the proc row alone fires into the void.
NEEDS_SCRIPT = {653225: 'aura_ascension_runemaster_air_engraving'}

# Riders no table can express: the payload works without them, the tooltip does not.
RIDER_SCRIPTS = {
    653266: 'aura_ascension_runemaster_ice_engraving',    # Icebound Momentum stacks
    653261: 'spell_ascension_runemaster_water_engraving',  # drain is a PERCENT, not an amount
    653263: 'aura_ascension_runemaster_arcane_mark',       # pays out the stored healing
    653272: 'spell_ascension_runemaster_earth_engraving',  # Expansive Engraver's Earth quarter
}

# Fire's roll was moved out of the proc entry and into the script, because no spell modifier
# can reach a family-0 aura and Expansive Engraver has to raise its chance. The row is pinned
# at 100 as a result: if the roll ever leaves the C++, Fire procs on EVERY direct hit.
FIRE_ENGRAVING = 653211
FIRE_SOURCE = 'src/AscensionRunemasterSecondary.cpp'

# Spells that carry SpellFamilyName 0 and are handled by a module script. A
# `SpellFamilyName != 38` guard — the one the rest of the Runemaster metadata uses — would
# skip them in silence: no binding, no metadata pass, no log line. See P-055.
FAMILY_ZERO_SOURCES = {
    'src/AscensionRunemasterGenesis.cpp': (500501, 500502),
    'src/AscensionRunemasterEngravings.cpp': (653225, 653266, 653261, 653263, 653272),
}

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
    """SpellId -> {column: value}, from the INSERTs of `spell_proc` in the module's SQL."""
    rows = {}
    for block in re.findall(r'INSERT\s+INTO\s+`?spell_proc`?\s*\((.*?)\)\s*VALUES(.*?);',
                            sql, re.S | re.I):
        columns = [c.strip(' `\n') for c in block[0].split(',')]
        for values in re.findall(r'\(([^()]*)\)', block[1]):
            cells = [c.strip() for c in values.split(',')]
            if len(cells) != len(columns):
                continue
            entry = dict(zip(columns, cells))
            rows[int(entry['SpellId'])] = entry
    return rows


def script_rows(sql):
    """spell_id -> {script names}, from the INSERTs of `spell_script_names`."""
    rows = {}
    for block in re.findall(r'INSERT\s+INTO\s+`?spell_script_names`?\s*\(.*?\)\s*VALUES(.*?);',
                            sql, re.S | re.I):
        for spell, name in re.findall(r"\((-?\d+),\s*'([^']+)'\)", block):
            rows.setdefault(abs(int(spell)), set()).add(name)
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
        cls.procs_full = proc_rows(sql)
        cls.procs = {k: int(v['ProcFlags']) for k, v in cls.procs_full.items()}
        cls.links = linked_rows(sql)
        cls.scripts = script_rows(sql)

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
                self.assertEqual(self.links.get((equip_spell, carrier)), 2,
                                 f'{element}: no type 2 (SPELL_LINK_AURA) row applying {carrier}; '
                                 f'{equip_spell} is applied by the enchant but carries no proc aura')

    def test_proc_chance_is_never_zero(self):
        """Chance 0 in `spell_proc` falls back to the DBC ProcChance; a zero there is a dead proc."""
        for ability, (element, _enchant, carrier, _direct) in ENGRAVINGS.items():
            with self.subTest(element=element):
                self.assertGreater(self.spell[carrier][35], 0,
                                   f'{element}: {carrier} has ProcChance 0 in Spell.dbc')

    def test_scripts_are_named_in_sql(self):
        """P-051: a script registered in C++ but absent from `spell_script_names` never runs."""
        for spell_id, name in {**NEEDS_SCRIPT, **RIDER_SCRIPTS}.items():
            with self.subTest(spell=spell_id):
                self.assertIn(name, self.scripts.get(spell_id, set()),
                              f'{name} is not attached to {spell_id} in the module SQL: '
                              'the C++ would be compiled in and never called')

    def test_fire_chance_is_rolled_in_cpp(self):
        """The pinned Chance=100 row and the C++ roll are one mechanism: neither works alone."""
        pinned = self.procs_full.get(FIRE_ENGRAVING, {}).get('Chance')
        source = (MODULE / FIRE_SOURCE).read_text()
        rolls = 'roll_chance_i' in source.split('class aura_ascension_runemaster_fire_engraving')[-1] \
            .split('class ')[0]
        if pinned == '100':
            self.assertTrue(rolls, f'`spell_proc` pins {FIRE_ENGRAVING} at Chance 100 but '
                                   f'{FIRE_SOURCE} no longer rolls: Fire would proc on every hit')
        elif rolls:
            self.fail(f'{FIRE_SOURCE} rolls the Fire chance itself, but `spell_proc` does not pin '
                      f'{FIRE_ENGRAVING} at 100: the chance would be applied twice')

    def test_family_zero_handlers_have_no_family_guard(self):
        """P-055: these records carry family 0, so a family-38 guard disables them silently."""
        for source, ids in FAMILY_ZERO_SOURCES.items():
            # Comments are stripped first: these files quote the offending guard on purpose,
            # to warn the next reader. Only real code counts.
            text = '\n'.join(l for l in (MODULE / source).read_text().splitlines()
                             if not l.lstrip().startswith('//'))
            for spell_id in ids:
                with self.subTest(spell=spell_id):
                    self.assertEqual(self.spell[spell_id][149], 0,
                                     f'{spell_id} is no longer family 0: re-read {source}, the '
                                     'reasoning in its header no longer holds')
            with self.subTest(source=source):
                self.assertNotIn('SpellFamilyName != 38', text,
                                 f'{source} handles family-0 spells but gates on family 38: '
                                 'the scripts would never bind and nothing would be logged')

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
