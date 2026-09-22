"""Execute Stormbringer cost rules and native stack spending with Charged Conduit."""
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

import sys as _sys
_sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from coa_test_env import compile_cxx  # noqa: E402

ROOT = Path(__file__).resolve().parents[4]
MODULE = ROOT / "modules/mod-ascension-compat/src"


def main():
    extract = runpy.run_path(str(ROOT / "modules/mod-ascension-compat/tests/client_compat/run.py"))["method"]
    service = extract((MODULE / "AscensionCompat.cpp").read_text(), "class AscensionResourceService")
    harness = (Path(__file__).parent.parent / "resource_generation/harness.cpp").read_text()
    harness = harness.replace("uint32 Id = 0;", "uint32 Id = 0, SpellFamilyName = 22, CasterAuraSpell = 0;")
    harness = harness.replace("    bool HasAura(uint32 id) const", """
    Aura const* GetAura(uint32 id) const
    {
        auto it = auras.find(id);
        return it != auras.end() && it->second.m_stackAmount ? &it->second : nullptr;
    }
    bool HasAura(uint32 id) const""")
    harness = harness.replace("    Player* ToPlayer() { return this; }", """    Player* ToPlayer() { return this; }
    uint32 guid = 1;
    uint32 GetGUID() const { return guid; }
    // Unit.h:1501 : HasAura(spellId, casterGUID, ...) — la surcharge a deux arguments
    // ne compte que les auras posees par CE lanceur.
    std::set<std::pair<uint32, uint32>> castAuras;
    bool HasAura(uint32 id, uint32 caster) const { return castAuras.count({id, caster}) != 0; }
    void AddCasterAura(uint32 id, uint32 caster) { castAuras.insert({id, caster}); }""")
    harness = harness.replace("#include <map>", "#include <map>\n#include <set>\n#include <utility>")
    # Spell.h:553 `SpellCastTargets m_targets;` et Spell.h:136 `Unit* GetUnitTarget() const;`
    harness = harness.replace("    Player* GetCaster() const { return owner; }", """    struct Targets
    {
        Player* target = nullptr;
        Player* GetUnitTarget() const { return target; }
    } m_targets;
    Player* GetCaster() const { return owner; }""")
    harness = harness.replace("    int32 Count(uint32 id)",
                              "    void RemoveAurasDueToSpell(uint32 id) { auras.erase(id); }\n    int32 Count(uint32 id)")
    harness = harness.replace("// NATIVE_STACK", extract(
        (ROOT / "src/server/game/Spells/Auras/SpellAuras.cpp").read_text(), "bool Aura::ModStackAmount("))
    core = (ROOT / "src/server/game/Spells/SpellEffects.cpp").read_text()
    constants = "\n".join(re.findall(r"constexpr uint32 ASCENSION_\w+ = \d+;", core))
    code = (MODULE / "AscensionCustomResourceData.h").read_text() + harness + constants + "\n"
    code += """
constexpr uint32 CLASS_STORMBRINGER=16, CLASS_RANGER=23;
constexpr uint32 SPELL_STORMBRINGER_STATIC=803102, SPELL_STORMBRINGER_CHARGED_CONDUIT=803790;
using SpellCastResult=int;
// Jetons locaux : seule leur distinction compte, le module ne fait que les comparer.
constexpr int SPELL_CAST_OK=0, SPELL_FAILED_CASTER_AURASTATE=1, SPELL_FAILED_NO_POWER=2,
    SPELL_FAILED_TARGET_AURASTATE=3;
"""
    # Valeurs lues, jamais recopiees : la classe vient de SharedDefines.h, les deux sorts
    # de la source qui les declare.
    shared = (ROOT / "src/server/shared/SharedDefines.h").read_text()
    code += "constexpr uint32 CLASS_REAPER=%s;\n" % re.search(r"CLASS_REAPER\s*=\s*(\d+)", shared).group(1)
    code += "\n".join(re.findall(r"constexpr uint32 SPELL_REAPER_SCYTHE_RUSH(?:_MARKER)? = \d+;",
                                 (MODULE / "AscensionCompat.cpp").read_text())) + "\n"
    code += extract(core, "void ModifyAscensionAuraStacks(") + "\n"
    cast = extract(service, "void OnSpellCast(")
    # Retain production entry gates and the entire cost dispatch; gain dispatch is tested separately.
    cast = cast[:cast.index("        for (AscensionCompatData::ResourceGainRule")] + cast[
        cast.index("        for (AscensionCompatData::ResourceCostRule"):cast.index("        ConsumeReaperSouls")] + "}"
    code += "struct ResourceService {\n" + "\n".join(extract(service, signature) for signature in (
        "static bool Matches(", "static uint8 GetAuraStacks(", "static void ModifyAuraStacks(",
        "void CheckCast(")) + "\n" + cast + "\n};\n"
    code += Path(__file__).with_name("cases.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="coa-storm-resources-") as directory:
        out = Path(directory)
        cpp, exe = out / "resources.cpp", out / "resources.exe"
        cpp.write_text(code, encoding="utf-8")
        compile_cxx(cpp, exe, cwd=out, timeout=60)
        subprocess.run([str(exe)], cwd=out, check=True, timeout=15)
    print("PASS: every Static cost rule, threshold checks, conduit preservation and native positive/negative gates")


if __name__ == "__main__":
    main()
