/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
// Chronomancer (class 22), Unstable Chronoglass 503836 (CA node 6175). Written on 2026-09-30.
//
// Read in Spell.dbc (server copy), 2026-09-30:
//   503836: effect 28, MiscValue 506015, SummonProperties 61 (ALLY / GUARDIAN), TARGET_DEST_DEST,
//           30 s. "Place an Unstable Chronoglass at target location for $d that will redirect
//           the next 3 offensive enemy spell casts onto itself."
//   506638 "Will redirect $n harmful spells to the Unstable Chronoglass.": e0 APPLY_AURA,
//           aura 96 SPELL_AURA_SPELL_MAGNET, TargetA 27 (TARGET_UNIT_MASTER), radius 30 yd,
//           ProcCharges 3, infinite duration.
//
// The creature 506015 carries 506638 through creature_template_addon
// (2026_09_30_32_ascension_creatures_chronomancer.sql). As authored, the aura would sit on the
// glass itself: Unit::GetMagicHitRedirectTarget takes the magnet from
// AuraEffect::GetBase()->GetUnitOwner(), so a plain aura on the glass only redirects spells
// aimed at the glass onto the glass. The Grounding Totem shape is an AREA aura owned by the
// magnet and applied to the protected unit; 506638 is therefore made an
// SPELL_EFFECT_APPLY_AREA_AURA_OWNER on its caster: owned by the glass, applied to its owner
// (the Chronomancer) while within the DBC radius. Everything else is native: the redirect,
// RedirectSpellEvent dropping one of the 3 charges per redirected spell, the aura's removal
// at 0 charges, and its removal from the Chronomancer when the glass leaves.
#include "ScriptMgr.h"
#include "SpellInfo.h"

namespace
{
constexpr uint32 SPELL_UNSTABLE_CHRONOGLASS_MAGNET = 506638;

class chronomancer_chronoglass_metadata : public GlobalScript
{
public:
    chronomancer_chronoglass_metadata() : GlobalScript("chronomancer_chronoglass_metadata",
        {GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR}) { }

    void OnLoadSpellCustomAttr(SpellInfo* info) override
    {
        if (!info || info->Id != SPELL_UNSTABLE_CHRONOGLASS_MAGNET)
            return;
        SpellEffectInfo& magnet = info->Effects[EFFECT_0];
        // Only the record read on 2026-09-30; a changed DBC is left alone.
        if (magnet.Effect != SPELL_EFFECT_APPLY_AURA || magnet.ApplyAuraName != SPELL_AURA_SPELL_MAGNET ||
            magnet.TargetA.GetTarget() != TARGET_UNIT_MASTER)
            return;
        magnet.Effect = SPELL_EFFECT_APPLY_AREA_AURA_OWNER;
        magnet.TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        magnet.TargetB = SpellImplicitTargetInfo(0);
    }
};
}

void AddSC_AscensionChronomancerChronoglass()
{
    new chronomancer_chronoglass_metadata();
}
