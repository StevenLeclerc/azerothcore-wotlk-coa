/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
#include "AscensionRunemasterTalents.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"

namespace
{
bool IsEarthTattoo(uint32 id)
{
    return id == 801094 || (id >= 803754 && id <= 803758);
}

bool StonePetroglyphActive(Player* player)
{
    if (!player->IsAlive() || !player->HasAura(707157))
        return false;
    if (player->HasAura(801094, player->GetGUID()))
        return true;
    for (uint32 id = 803754; id <= 803758; ++id)
        if (player->HasAura(id, player->GetGUID()))
            return true;
    return false;
}

void SyncStonePetroglyph(Player* player)
{
    if (!StonePetroglyphActive(player))
        player->RemoveAurasDueToSpell(712310, player->GetGUID());
    else if (!player->HasAura(712310, player->GetGUID()))
        player->CastSpell(player, 712310, true);
}

// Palm Sigil (805380/805381/805382) gates its cast on CasterAuraSpell 808089, a marker spell
// literally named "Runeshroud or Waveforged" that nothing else ever grants, making it permanently
// uncastable. Mirror the real Runeshroud/Waveforged state onto it instead.
void SyncRuneshroudOrWaveforged(Player* player)
{
    bool active = player->HasAura(500288, player->GetGUID()) || player->HasAura(705565, player->GetGUID());
    if (!active)
        player->RemoveAurasDueToSpell(808089, player->GetGUID());
    else if (!player->HasAura(808089, player->GetGUID()))
        player->CastSpell(player, 808089, true);
}

class runemaster_talent_events : public UnitScript
{
public:
    runemaster_talent_events() : UnitScript("runemaster_talent_events", true,
        {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !aura)
            return;
        uint32 id = aura->GetId();
        if (id == 707157 || id == 712310 || IsEarthTattoo(id))
            SyncStonePetroglyph(player);
        if (id == 500288 || id == 705565)
            SyncRuneshroudOrWaveforged(player);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player || player->getClass() != CLASS_SPIRIT_MAGE || !application)
            return;
        Aura* aura = application->GetBase();
        uint32 id = aura->GetId();
        if (id == 707157 || IsEarthTattoo(id))
            SyncStonePetroglyph(player);
        if (id == 500288 && aura->GetCasterGUID() == player->GetGUID() && mode != AURA_REMOVE_BY_DEATH &&
            player->IsAlive() && player->IsInWorld() && player->HasAura(520054))
            player->CastSpell(player, 520768, true);
        if (id == 500288 || id == 705565)
            SyncRuneshroudOrWaveforged(player);
    }
};
}

namespace
{
void Refused(SpellInfo const* info, char const* what)
{
    LOG_ERROR("server.loading", "Ascension contract Runemaster: spell {} refused ({}), left as the DBC has it.",
        info->Id, what);
}

// Frost Spectre 560039/561350: "critical strike chance and critical damage of Smolder and
// Primordial Blast against Frozen targets". Both effects are aura 112 with the RAW private
// selectors 20000/20001, which the core does not enable (SpellAuraDefines.h: "Raw private
// 20000-series records are not implicitly enabled"): the reviewed selectors are 21000 and 21007,
// the conversion Barbarian 705234 and Necromancer 707627 already make. MiscValueB (4 = Frozen)
// and the mask (Smolder 0x200, Primordial Blast word 2 0x40) stay as read.
void FrostSpectre(SpellInfo* info)
{
    SpellEffectInfo& chance = info->Effects[EFFECT_0];
    SpellEffectInfo& damage = info->Effects[EFFECT_1];
    if (chance.ApplyAuraName != SPELL_AURA_OVERRIDE_CLASS_SCRIPTS || chance.MiscValue != 20000 ||
        damage.ApplyAuraName != SPELL_AURA_OVERRIDE_CLASS_SCRIPTS || damage.MiscValue != 20001 ||
        chance.MiscValueB != AURA_STATE_FROZEN || damage.MiscValueB != AURA_STATE_FROZEN ||
        chance.SpellClassMask != flag96(512, 0, 64) || damage.SpellClassMask != flag96(512, 0, 64))
        return Refused(info, "aura 112 selectors 20000/20001, Frozen, mask (512,0,64)");
    chance.MiscValue = ASCENSION_STATE_MASKED_CRIT;
    damage.MiscValue = ASCENSION_STATE_MASKED_CRIT_DAMAGE;
}

// Advanced Magi 804557: "Increases the spell power scaling of Elemental Burst by $s2%". SpellModOp
// 41 is Ascension's private "spell power coefficient"; AuraEffect::CalculateSpellMod drops every
// op >= MAX_SPELLMOD. Same conversion as Stormbringer's Invoking Storms: SPELLMOD_BONUS_MULTIPLIER.
void AdvancedMagi(SpellInfo* info)
{
    SpellEffectInfo& effect = info->Effects[EFFECT_1];
    if (effect.ApplyAuraName != SPELL_AURA_ADD_PCT_MODIFIER || effect.MiscValue != 41 ||
        effect.SpellClassMask != flag96(0, 0, 131072))
        return Refused(info, "effect 1 pct modifier op 41 on (0,0,131072)");
    effect.MiscValue = SPELLMOD_BONUS_MULTIPLIER;
}

// Air Engraving 653223 and Fire Engraving 653211, effect 1: ADD_PCT_MODIFIER SPELLMOD_DAMAGE worth
// 0 with an EMPTY class mask -- the slot that Windbreaker 804094 (+30, SPELLMOD_EFFECT2 on
// 653223) and Symbols of Power 705613/705614 (+15/+30 on 653211) fill. SpellInfo::IsAffected
// accepts an empty mask in the same family, so the talents' +30% went to every family-38 spell.
// The mask becomes the spell the talent names: Hurricane's strike 645437 (0,0,4, the only spell
// with that bit) and Fist of the Ancients (0,0,32768; the bit also covers 712403, an aura, and
// 500502 Genesis Damage, a leak left named in the report).
void EngravingDamageSlot(SpellInfo* info, flag96 const& mask)
{
    SpellEffectInfo& effect = info->Effects[EFFECT_1];
    if (effect.ApplyAuraName != SPELL_AURA_ADD_PCT_MODIFIER || effect.MiscValue != SPELLMOD_DAMAGE ||
        effect.BasePoints != -1 || effect.DieSides != 1 || effect.SpellClassMask)
        return Refused(info, "effect 1 pct SPELLMOD_DAMAGE worth 0 with an empty mask");
    effect.SpellClassMask = mask;
}
}

void ApplyAscensionRunemasterTalentContracts(SpellInfo* info)
{
    if (!info || info->SpellFamilyName != 38)
        return;
    switch (info->Id)
    {
        case 560039:
        case 561350:
            FrostSpectre(info);
            return;
        case 804557:
            AdvancedMagi(info);
            return;
        case 653223:
            EngravingDamageSlot(info, flag96(0, 0, 4));
            return;
        case 653211:
            EngravingDamageSlot(info, flag96(0, 0, 32768));
            return;
        case 712310:
            break;
        default:
            return;
    }
    // The native periodic heal and effect-98 immunity already exist. Complete
    // knockback immunity for the separate destination-based effect as well.
    auto& effect = info->Effects[EFFECT_1];
    effect.Effect = SPELL_EFFECT_APPLY_AURA;
    effect.ApplyAuraName = SPELL_AURA_EFFECT_IMMUNITY;
    effect.MiscValue = SPELL_EFFECT_KNOCK_BACK_DEST;
    effect.BasePoints = 0;
    effect.DieSides = 0;
}

void AddSC_AscensionRunemasterTalents()
{
    new runemaster_talent_events();
}
