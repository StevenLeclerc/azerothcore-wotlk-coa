/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */

#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <limits>

namespace
{
enum PrimalistAbilitySpells : uint32
{
    SPELL_GEODE_BARRAGE_DAMAGE = 803138,
    SPELL_GEODE_BARRAGE_RAGE = 802885
};

Player* Primalist(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->getClass() == CLASS_WILDWALKER ? player : nullptr;
}

class primalist_talent_events : public UnitScript
{
public:
    primalist_talent_events() : UnitScript("primalist_talent_events", true,
        {UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnDamage(Unit*, Unit* victim, uint32& damage) override
    {
        Player* player = Primalist(victim);
        if (!player || !player->IsAlive() || !damage || damage < player->GetHealth() ||
            !player->HasAura(560157) || player->HasSpellCooldown(560157))
            return;
        // DealDamage reaches this hook after mitigation and absorption. Mark the
        // native saved cooldown before casting the heal, including any nested events.
        player->AddSpellCooldown(560157, 0, 120000);
        damage = 0;
        player->CastSpell(player, 560179, true);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* application, AuraRemoveMode mode) override
    {
        Player* player = Primalist(unit);
        if (!player || !player->IsAlive() || !player->IsInWorld() || !application ||
            mode == AURA_REMOVE_BY_DEATH || !player->HasAura(300728))
            return;
        Aura* aura = application->GetBase();
        if (aura->GetCasterGUID() != player->GetGUID())
            return;
        // Only the visible defenses, not their separately removed SLS helpers.
        if (aura->GetId() == 680421 || aura->GetId() == 800094 || aura->GetId() == 503630)
            player->CastSpell(player, 503716, true);
    }
};

class primalist_talent_casts : public AllSpellScript
{
public:
    primalist_talent_casts() : AllSpellScript("primalist_talent_casts",
        {ALLSPELLHOOK_ON_CRIT_CHANCE, ALLSPELLHOOK_ON_HIT_RESULT}) { }

    void OnSpellHitResult(Spell* spell, Unit* target, uint8 miss, uint32, uint32, bool) override
    {
        Player* player = Primalist(spell->GetCaster());
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !target || target == player || player->IsFriendlyTo(target) || miss != SPELL_MISS_NONE ||
            info->SpellFamilyName != 37 || info->Id != SPELL_GEODE_BARRAGE_DAMAGE ||
            spell->GetScriptValue(SPELL_GEODE_BARRAGE_RAGE))
            return;
        // Each channel tick casts this damage helper. Its authored energize
        // companion rolls 30-80 internal Rage (3-8 visible Rage) per successful stone.
        spell->SetScriptValue(SPELL_GEODE_BARRAGE_RAGE, 1);
        player->CastSpell(player, SPELL_GEODE_BARRAGE_RAGE, true);
    }

    void OnSpellCritChance(Spell* spell, Unit* target, float& chance) override
    {
        Player* player = Primalist(spell->GetCaster());
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !target || !player->HasAura(805336) || info->SpellFamilyName != 37 ||
            !(info->SpellFamilyFlags[0] & 8))
            return;
        for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
            if (effect->GetCasterGUID() == player->GetGUID() && effect->GetSpellInfo()->SpellFamilyName == 37 &&
                (effect->GetSpellInfo()->SpellFamilyFlags[0] & 64))
            {
                chance = 100.0f;
                break;
            }
    }
};

SpellCastResult CheckThroatClamp(Player* player, Unit* target)
{
    Pet* pet = player ? player->GetPet() : nullptr;
    if (!pet || !pet->IsAlive() || pet->GetOwnerGUID() != player->GetGUID())
        return SPELL_FAILED_NO_PET;
    if (!target || !pet->IsInMap(target) || !pet->InSamePhase(target) || !pet->IsValidAttackTarget(target))
        return SPELL_FAILED_BAD_TARGETS;
    if (pet->HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_LOST_CONTROL))
        return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
    SpellInfo const* helper = sSpellMgr->GetSpellInfo(500811);
    if (!helper || !pet->IsWithinDistInMap(target, helper->GetMaxRange(false, pet)))
        return SPELL_FAILED_OUT_OF_RANGE;
    if (!pet->IsWithinLOSInMap(target))
        return SPELL_FAILED_LINE_OF_SIGHT;
    return SPELL_CAST_OK;
}

class spell_ascension_throat_clamp : public SpellScript
{
    PrepareSpellScript(spell_ascension_throat_clamp);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({500811}); }
    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    SpellCastResult CheckCast() { return CheckThroatClamp(Primalist(GetCaster()), GetExplTargetUnit()); }

    void Handle(SpellEffIndex)
    {
        Player* player = Primalist(GetCaster());
        Unit* target = GetHitUnit();
        if (CheckThroatClamp(player, target) == SPELL_CAST_OK)
            player->GetPet()->CastSpell(target, 500811, false); // Native dash, interrupt and school lockout.
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ascension_throat_clamp::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_ascension_throat_clamp::Handle, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};
// ---------------------------------------------------------------------------------
// Vague 5. Les lignes spell_proc sont dans 2026_09_29_31_ascension_primalist_procs.sql et
// les noms de scripts dans 2026_09_29_33_ascension_primalist_scripts.sql : un DoCheckProc
// n'est jamais appele sans entree spell_proc (Aura::GetProcEffectMask, SpellAuras.cpp:2150).
// ---------------------------------------------------------------------------------
enum PrimalistWave5TalentSpells : uint32
{
    SPELL_THREE_HAMMERS_DAMAGE = 520468,
    SPELL_TOTEMIC_SMASH_R1     = 800178,
    SPELL_GEODE                = 804002,
    SPELL_EARTHEN_AVATAR       = 680421,
    SPELL_EMERALD_WILL         = 500302,
    SPELL_EMERALD_WILL_HEAL    = 525015,
    SPELL_SON_OF_URSOC_FORM    = 806048,
    SPELL_SACRED_GROVE         = 800180,
    SPELL_SACRED_GROVE_DATA    = 800186,
    SPELL_SACRED_GROVE_TICK    = 800179,
    // revue du 23/10
    SPELL_ROCKSLIDE_DELAY      = 560155,
    SPELL_STONESHARD_R1        = 680448,
    SPELL_REXXARS_BLEED        = 806562,
    SPELL_MISHAS_RAGE          = 504227,
    SPELL_HUFFERS_SPEED        = 560973,
    SPELL_LEOKKS_FURY          = 560974,
    SPELL_PRIMAL_SHRED_R1      = 500940,
    SPELL_WILDCLAW_R1          = 800140,
    SPELL_RYLAKS_BITE_R1       = 706342,
    SPELL_LEGACY_OF_REXXAR     = 800184,
    SPELL_ROCK_BARRIER         = 503630,
    SPELL_ROCK_BARRIER_SLS     = 504853,
    SPELL_EARTHMOTHERS_PROTECTION = 560298
};

constexpr uint8 PRIMALIST_CHAIN_WALK = 32;

// Le plus haut rang de la chaine que le joueur connait ; le rang 1 hors joueur (rien a lire).
uint32 HighestKnownRank(Player const* player, uint32 firstRank)
{
    uint32 best = player ? 0 : firstRank;
    uint32 current = sSpellMgr->GetFirstSpellInChain(firstRank);
    for (uint8 step = 0; player && current && step < PRIMALIST_CHAIN_WALK; ++step)
    {
        if (player->HasSpell(current))
            best = current;
        current = sSpellMgr->GetNextSpellInChain(current);
    }
    return best;
}

// Les effets de recharge d'Ascension (165 en ms, 192 en %) nomment UN id, le rang 1, alors que la
// recharge vit sous l'id du rang lance (Spell.cpp, Player::AddSpellAndCategoryCooldowns). On marche
// la chaine spell_ranks et on traite chaque rang qui porte une recharge en cours, comme
// ReduceCooldownOverChain (AscensionStormbringerEvents.cpp). cutOf(rang) rend la reduction en ms ;
// une reduction au moins egale au reste efface l'entree (et la categorie si resetCategory).
// cutOf rend UINT64_MAX pour la sentinelle « remise a zero » (valeur 0 de l'effet 165).
template <typename CutOf>
void ReduceCooldownOverChain(Player* player, uint32 spellId, bool resetCategory, CutOf cutOf)
{
    if (!player || !spellId)
        return;
    uint32 current = sSpellMgr->GetFirstSpellInChain(spellId);
    for (uint8 step = 0; current && step < PRIMALIST_CHAIN_WALK; ++step)
    {
        if (uint32 remaining = player->GetSpellCooldownDelay(current))
        {
            uint64 cut = cutOf(current);
            if (cut >= remaining)
            {
                player->RemoveSpellCooldown(current, true);
                if (resetCategory)
                    if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(current))
                        if (uint32 category = rank->GetCategory())
                            player->RemoveCategoryCooldown(category);
            }
            else if (cut)
                player->ModifySpellCooldown(current, -int32(cut));
        }
        current = sSpellMgr->GetNextSpellInChain(current);
    }
}

// C-05 / C-06 : 680411 Thane's Guidance (Mountain Hammer, Primal Rush) et 681365 Spiritbound
// (Seismic Crash, Seismic Spike, Seismic Smash). Effet 165 : MiscValue = sort, valeur = ms, MiscValueB
// = categorie aussi. Le gestionnaire natif ne touche que l'id du rang 1 : des le rang 2 (niveaux 16 a
// 26) plus rien n'etait reduit. L'effet 1 de 680411 (remise a zero d'Earthen Avatar) est retire au
// chargement (mountain_talent_metadata), il n'arrive plus ici.
class spell_ascension_primalist_cooldown_chain : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_cooldown_chain);

    void Handle(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = Primalist(GetHitUnit());
        SpellEffectInfo const& effect = GetSpellInfo()->Effects[effIndex];
        int32 const delta = GetEffectValue();
        if (!player || effect.MiscValue <= 0 || delta > 0)
            return;
        uint64 const cut = delta ? uint64(-int64(delta)) : std::numeric_limits<uint64>::max();
        ReduceCooldownOverChain(player, uint32(effect.MiscValue), effect.MiscValueB != 0,
            [cut](uint32) { return cut; });
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_cooldown_chain::Handle, EFFECT_ALL,
            SPELL_EFFECT_ASCENSION_MODIFY_COOLDOWN);
    }
};

// C-24 : effet prive 192 (Spell::EffectNULL), MiscValue = sort, valeur = pourcentage. 807249 Wildheart
// (Mountain Hammer, Boulder Dash, Stone Grip, toutes les 1,5 s pendant 15 s) et 560306 Hand of Neptulon,
// tick de Verdant Chase 560305 (Spirit Charge, Primal Rush, toutes les 2,5 s). SUPPOSE, comme
// spell_ascension_runemaster_cooldown_percent : le pourcentage de la recharge ENTIERE du rang, retire du
// temps restant. MiscValueB (1 sur 560306) = la categorie aussi, comme l'effet 165.
class spell_ascension_primalist_cooldown_percent : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_cooldown_percent);

    void Handle(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = Primalist(GetHitUnit());
        SpellEffectInfo const& effect = GetSpellInfo()->Effects[effIndex];
        if (!player || player != GetCaster() || effect.MiscValue <= 0)
            return;
        uint64 const pct = uint64(std::clamp(GetEffectValue(), 0, 100));
        if (!pct)
            return;
        ReduceCooldownOverChain(player, uint32(effect.MiscValue), effect.MiscValueB != 0, [pct](uint32 rank)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(rank);
            return info ? uint64(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) * pct / 100 : 0;
        });
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_cooldown_percent::Handle, EFFECT_ALL,
            SpellEffects(192));
    }
};

// C-09 520134 Fury of the Earthmother : « add 1 charge to an active Rock Barrier ». L'effet 0 est un
// effet 171 (Spell::EffectNULL), MiscValue 503630 ; l'effet 1 (177) allonge deja la duree, natif.
// Ce sont les charges de l'AURA (« Lasts for $n attacks », ProcCharges 10), plafonnees par
// Aura::ModCharges. La valeur 2 du DBC n'est pas reprise : l'infobulle dit 1.
class spell_ascension_primalist_fury_of_the_earthmother : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_fury_of_the_earthmother);

    void Handle(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Player* player = Primalist(GetHitUnit());
        int32 const barrier = GetSpellInfo()->Effects[effIndex].MiscValue;
        if (!player || barrier <= 0 || !player->IsAlive())
            return;
        if (Aura* aura = player->GetAura(uint32(barrier), player->GetGUID()))
            aura->ModCharges(1);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_fury_of_the_earthmother::Handle, EFFECT_0,
            SpellEffects(171));
    }
};

// C-10 573308 Stone Skin (pose toutes les 3 s par 806583, 5 s) : ADD_PCT_MODIFIER (degats) a 0 sur
// Geode Barrage, Geode et les sorts Seismic. 806583 : « damage now additionally scales with your parry
// rating » (573308 dit « defense » : l'infobulle du passif appris fait foi). SUPPOSE, aucune formule
// dans les donnees : +1 % de degats par point de pourcentage de parade apporte par le SCORE de parade
// (Player::GetRatingBonusValue(CR_PARRY)), borne a [0, 100]. Recalcule a chaque nouvelle pose
// (Aura::SetStackAmount -> CalculateAmount).
class aura_ascension_primalist_stone_skin : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_stone_skin);

    void Calculate(AuraEffect const*, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = true;
        Player* player = Primalist(GetCaster());
        if (!player)
            return;
        amount = std::clamp(int32(player->GetRatingBonusValue(CR_PARRY)), 0, 100);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(aura_ascension_primalist_stone_skin::Calculate, EFFECT_0,
            SPELL_AURA_ADD_PCT_MODIFIER);
    }
};

// C-22 560154 Rockslide : « Stoneshard now has a $h% chance to cast an additional time. This effect can
// trigger itself. » 560155 (effet 183, 500 ms) relancait toujours 680448, le rang 1 (environ 60 de
// base au lieu d'environ 1 150 au niveau 60). On relance, avec le meme delai, l'id du Stoneshard qui
// vient de partir. La recursion reste celle du DBC (15 % a chaque relance, AttributesMask 2 de la
// ligne spell_proc) : une suite de 5 relances a une chance sur 13 000.
class aura_ascension_primalist_rockslide : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_rockslide);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_ROCKSLIDE_DELAY}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        return Primalist(GetTarget()) && GetTarget()->IsAlive() && eventInfo.GetActor() == GetTarget() && info &&
            spell && info->SpellFamilyName == 37 && sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_STONESHARD_R1 &&
            spell->m_targets.GetUnitTarget();
    }

    void Proc(AuraEffect const*, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* target = eventInfo.GetProcSpell()->m_targets.GetUnitTarget();
        uint32 const spellId = eventInfo.GetSpellInfo()->Id;
        int32 const delay = sSpellMgr->AssertSpellInfo(SPELL_ROCKSLIDE_DELAY)->Effects[EFFECT_0].CalcValue(caster);
        ObjectGuid const targetGuid = target->GetGUID();
        // Meme forme que Spell::EffectAscensionTriggerSpellDelayed : l'evenement vit sur le lanceur.
        caster->m_Events.AddEventAtOffset([caster, targetGuid, spellId]()
        {
            if (!caster->IsAlive())
                return;
            if (Unit* delayed = ObjectAccessor::GetUnit(*caster, targetGuid))
                if (delayed->IsAlive() && caster->IsValidAttackTarget(delayed))
                    caster->CastSpell(delayed, spellId, true);
        }, Milliseconds(std::max<int32>(1, delay)));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_rockslide::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_rockslide::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Le maitre Primalist d'un familier ou d'une invocation controlee, sinon nul.
Player* PrimalistPetOwner(Unit* unit)
{
    if (!unit || unit->IsPlayer() || !unit->IsSummon())
        return nullptr;
    Player* player = Primalist(unit->GetOwner());
    return player && unit->GetOwnerGUID() == player->GetGUID() ? player : nullptr;
}

// C-13 806561 Rexxar's Might, sur le familier (pose par 806559 a chaque critique physique du joueur,
// tA 5) : « your pet's next attack to apply a bleed, dealing ${$806562m1+$806562ppl1+$AP*0.15} ».
// La ligne spell_proc (Charges 1) consomme l'aura au premier coup du familier. $AP est lu sur
// 806559, un sort du JOUEUR : sa PA, pas celle du familier qui lance 806562 (meme lecture que
// Bring Me Their Bones, AscensionPrimalistEvents.cpp). CalcValue ajoute ensuite le niveau.
class aura_ascension_primalist_rexxars_might_pet : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_rexxars_might_pet);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_REXXARS_BLEED}); }

    bool Check(ProcEventInfo& eventInfo)
    {
        Unit* pet = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        return PrimalistPetOwner(pet) && pet->IsAlive() && eventInfo.GetActor() == pet && victim &&
            victim != pet && victim->IsAlive() && pet->IsValidAttackTarget(victim);
    }

    void Proc(AuraEffect const*, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* pet = GetTarget();
        Player* owner = PrimalistPetOwner(pet);
        Unit* victim = eventInfo.GetActionTarget();
        if (!owner || !victim)
            return;
        SpellEffectInfo const& bleed = sSpellMgr->AssertSpellInfo(SPELL_REXXARS_BLEED)->Effects[EFFECT_0];
        float const ap = std::max(0.0f, owner->GetTotalAttackPowerValue(BASE_ATTACK)) * 0.15f;
        int32 const value = bleed.BasePoints + (bleed.DieSides ? 1 : 0) +
            int32(std::min(ap, float(std::numeric_limits<int32>::max() / 2)));
        pet->CastCustomSpell(SPELL_REXXARS_BLEED, SPELLVALUE_BASE_POINT0, value, victim, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_rexxars_might_pet::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_rexxars_might_pet::Proc, EFFECT_0,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// C-14 800184 Legacy of Rexxar. Effet 0 (DUMMY) sur le joueur, effet 1 (190 -> aura 42 vers 504227)
// sur les invocations : aucun ne proc sans ligne spell_proc, et 504227 seul ne couvrait qu'un des
// trois buffs. Une ligne spell_proc (critiques ; sorts de melee et periodiques) et ce script :
//  - joueur, critique de Wildclaw (800140) -> Huffer's Speed 560973 ; de Rylak's Bite (706342) ->
//    Leokk's Fury 560974 (son effet 1 vise le familier, tA 5) ;
//  - familier, tick critique du saignement de Primal Shred (lance par le familier : 500940 est un
//    FORCE_CAST) -> Misha's Rage 504227, au joueur et au familier.
// « grant you and your pet » : chaque buff sur les deux, une seule fois chacun.
class aura_ascension_primalist_legacy_of_rexxar : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_legacy_of_rexxar);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_MISHAS_RAGE, SPELL_HUFFERS_SPEED, SPELL_LEOKKS_FURY});
    }

    // Le saignement de Primal Shred est le sort que l'effet 0 (FORCE_CAST) d'un rang de 500940 fait lancer.
    static bool IsPrimalShredBleed(uint32 spellId)
    {
        uint32 current = SPELL_PRIMAL_SHRED_R1;
        for (uint8 step = 0; current && step < PRIMALIST_CHAIN_WALK; ++step)
        {
            if (SpellInfo const* rank = sSpellMgr->GetSpellInfo(current))
                if (rank->Effects[EFFECT_0].TriggerSpell == spellId)
                    return true;
            current = sSpellMgr->GetNextSpellInChain(current);
        }
        return false;
    }

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Unit* owner = GetTarget();
        if (!info || info->SpellFamilyName != 37 || !(eventInfo.GetHitMask() & PROC_HIT_CRITICAL) ||
            eventInfo.GetActor() != owner || !owner->IsAlive())
            return false;
        if (Primalist(owner))
        {
            uint32 const first = sSpellMgr->GetFirstSpellInChain(info->Id);
            return first == SPELL_WILDCLAW_R1 || first == SPELL_RYLAKS_BITE_R1;
        }
        Player* master = PrimalistPetOwner(owner);
        return master && owner == master->GetGuardianPet() && IsPrimalShredBleed(info->Id);
    }

    // Sur le familier : AddAura (lanceur = joueur, refraichit ou empile) plutot qu'un second lancer
    // par le familier, qui compterait deux fois le joueur (tB 27 de 504227 = TARGET_UNIT_MASTER).
    static void GrantToPet(Player* player, uint32 spellId)
    {
        Guardian* pet = player->GetGuardianPet();
        if (pet && pet->IsAlive() && pet->IsInWorld() && pet->IsInMap(player))
            player->AddAura(spellId, pet);
    }

    void PlayerProc(AuraEffect const*, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = Primalist(GetTarget());
        if (!player)
            return;
        uint32 const first = sSpellMgr->GetFirstSpellInChain(eventInfo.GetSpellInfo()->Id);
        if (first == SPELL_WILDCLAW_R1)
        {
            // 560973 : tA 1 et tB 5 (le familier) sur son seul effet. Que le lancer atteigne le
            // familier n'est pas mesure : on releve sa pile et sa duree avant, et on ne complete
            // que si le lancer ne l'a pas touche (ni pile ajoutee, ni duree remise a neuf).
            Guardian* pet = player->GetGuardianPet();
            Aura const* before = pet ? pet->GetAura(SPELL_HUFFERS_SPEED, player->GetGUID()) : nullptr;
            uint8 const stacks = before ? before->GetStackAmount() : 0;
            int32 const duration = before ? before->GetDuration() : 0;
            player->CastSpell(player, SPELL_HUFFERS_SPEED, true);
            pet = player->GetGuardianPet();
            Aura const* after = pet ? pet->GetAura(SPELL_HUFFERS_SPEED, player->GetGUID()) : nullptr;
            if (!after || (before && after->GetStackAmount() == stacks && after->GetDuration() <= duration))
                GrantToPet(player, SPELL_HUFFERS_SPEED);
        }
        else if (first == SPELL_RYLAKS_BITE_R1)
            player->CastSpell(player, SPELL_LEOKKS_FURY, true);   // e0 joueur, e1 familier (tA 5)
    }

    void PetProc(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        Unit* pet = GetTarget();
        Player* player = PrimalistPetOwner(pet);
        if (!player || !player->IsAlive() || !player->IsInMap(pet))
            return;
        player->CastSpell(player, SPELL_MISHAS_RAGE, true);
        GrantToPet(player, SPELL_MISHAS_RAGE);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_legacy_of_rexxar::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_legacy_of_rexxar::PlayerProc, EFFECT_0,
            SPELL_AURA_DUMMY);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_legacy_of_rexxar::PetProc, EFFECT_1,
            SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// C-14, suite de la revue : le saignement de Primal Shred (504669 a 504676, 504684 : TriggerSpell de
// l'effet 0 des rangs 1 a 9 de 500940) ne pouvait jamais etre critique. Un tick periodique ne critique
// que par une aura 286 (ABILITY_PERIODIC_CRIT) qui le couvre (AuraEffect::CalcPeriodicCritChance) ;
// aucune des trois de la famille 37 (300698, 560156, 704245) ne couvre son masque, et Aura::RefreshTimers
// remet la chance a 0 a chaque relance (CalculatePeriodicData). « Critical strikes with Primal Shred »
// (800184) : seulement si le maitre a Legacy of Rexxar, la chance est posee avant chaque tick, au calcul
// du coeur (DmgClass 2 melee, ecole physique : SpellDoneCritChance du familier, puis SpellTakenCritChance).
class aura_ascension_primalist_primal_shred_crit : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_primal_shred_crit);

    void Tick(AuraEffect const* aurEff)
    {
        AuraEffect* effect = GetEffect(aurEff->GetEffIndex());
        Unit* pet = GetCaster();
        Unit* target = GetUnitOwner();
        Player* master = PrimalistPetOwner(pet);
        if (!effect || !target || !master || !master->HasAura(SPELL_LEGACY_OF_REXXAR))
            return;
        SpellInfo const* info = GetSpellInfo();
        float chance = pet->SpellDoneCritChance(nullptr, info, info->GetSchoolMask(), BASE_ATTACK, true);
        if (chance > 0.0f)
            chance = target->SpellTakenCritChance(pet, info, info->GetSchoolMask(), chance, BASE_ATTACK, true);
        effect->SetCritChance(std::max(0.0f, chance));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_primalist_primal_shred_crit::Tick, EFFECT_0,
            SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// C-08, suite de la revue : 504853 (SLS de Rock Barrier, effet 2 = cout de Hand of the Earthmother, que
// 560298 Earthmother's Protection ramene a -50 %) n'est pose qu'avec le talent : un lien
// spell_linked_spell sans condition l'affichait chez tout Primalist (Attributes 0, icone visible).
// « While your Rock Barrier is active » : pose a l'application ET a la relance (le lien ne suivait pas la
// relance), a la duree de Rock Barrier (504853 dure 8 s par lui-meme, Rock Barrier 10 s), retire avec lui.
class aura_ascension_primalist_rock_barrier : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_rock_barrier);

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Player* player = Primalist(GetUnitOwner());
        if (!player || GetCasterGUID() != player->GetGUID())
            return;
        if (!player->HasAura(SPELL_EARTHMOTHERS_PROTECTION))
        {
            player->RemoveAurasDueToSpell(SPELL_ROCK_BARRIER_SLS, player->GetGUID());
            return;
        }
        // AddAura refait l'aura existante sans recalculer ses montants : on la retire pour relire le mod
        // de 560298 (qui a pu changer de rang), puis on la repose.
        player->RemoveAurasDueToSpell(SPELL_ROCK_BARRIER_SLS, player->GetGUID());
        if (Aura* sls = player->AddAura(SPELL_ROCK_BARRIER_SLS, player))
        {
            int32 const duration = GetAura()->GetDuration();
            if (duration > 0)
            {
                sls->SetMaxDuration(duration);
                sls->SetDuration(duration);
            }
        }
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        if (Unit* owner = GetUnitOwner())
            owner->RemoveAurasDueToSpell(SPELL_ROCK_BARRIER_SLS, GetCasterGUID());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_primalist_rock_barrier::Apply, EFFECT_0,
            SPELL_AURA_MOD_RESISTANCE, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_primalist_rock_barrier::Remove, EFFECT_0,
            SPELL_AURA_MOD_RESISTANCE, AURA_EFFECT_HANDLE_REAL);
    }
};

constexpr uint32 SON_OF_URSOC_CREATURE = 421493;   // absente de creature_template (MESURE)
constexpr uint32 SON_OF_URSOC_DISPLAY  = 328642;   // present dans CreatureDisplayInfo.dbc (MESURE)
constexpr int32 MOUNTAIN_AVATAR_DURATION = 5000;   // « for 5 sec » ; 712446 bp 4999

// 520464 Mineralization : « While at or below 35% health, your spells and abilities ».
// Le seuil n'existe que dans le CasterAuraState de 520465, que le lancement declenche ignore.
class aura_ascension_primalist_mineralization : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_mineralization);

    bool Check(ProcEventInfo& eventInfo)
    {
        Unit* owner = GetTarget();
        return Primalist(owner) && owner->IsAlive() && eventInfo.GetActor() == owner && !owner->HealthAbovePct(35);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_mineralization::Check);
    }
};

// 520469 Shattering : seuls les degats des marteaux 520468 comptent (sort sans drapeau de famille).
class aura_ascension_primalist_shattering : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_shattering);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info &&
            info->Id == SPELL_THREE_HAMMERS_DAMAGE;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_shattering::Check);
    }
};

// 503718 Infused With Power : « Effectively healing allies » - soin effectif non nul.
class aura_ascension_primalist_infused_with_power : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_infused_with_power);

    bool Check(ProcEventInfo& eventInfo)
    {
        HealInfo const* heal = eventInfo.GetHealInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && heal && heal->GetEffectiveHeal();
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_infused_with_power::Check);
    }
};

// 500937 Totemic Echoes : le bit 16 de FF2 porte aussi Brutalize 560314, Totemic Crash 803534 et
// l'eclat 500945. Seuls les 7 rangs de Totemic Smash, et seulement leur cible principale : la phase
// HIT passe une fois par ennemi touche par l'eclat.
class aura_ascension_primalist_totemic_echoes : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_totemic_echoes);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info && spell &&
            info->SpellFamilyName == 37 && sSpellMgr->GetFirstSpellInChain(info->Id) == SPELL_TOTEMIC_SMASH_R1 &&
            eventInfo.GetActionTarget() && spell->m_targets.GetUnitTarget() == eventInfo.GetActionTarget();
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_totemic_echoes::Check);
    }
};

// 547213 Totemic Crash : « At the end of the duration, if you are within 5 yds of the target ».
// Periode 1999 ms pour 2000 ms de duree : le seul tick est la fin.
class aura_ascension_primalist_totemic_crash : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_totemic_crash);

    // C-23 : 547216 relance toujours 800178, le rang 1 (zone environ 8 fois plus faible au rang 7).
    // Le declencheur natif est toujours empeche ; on lance le plus haut rang connu de la chaine.
    void Tick(AuraEffect const*)
    {
        PreventDefaultAction();
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !caster->IsAlive() || !target->IsAlive() || !caster->IsInMap(target) ||
            !caster->IsWithinDist(target, 5.0f) || !caster->IsValidAttackTarget(target))
            return;
        if (uint32 rank = HighestKnownRank(caster->ToPlayer(), SPELL_TOTEMIC_SMASH_R1))
            caster->CastSpell(target, rank, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_primalist_totemic_crash::Tick,
            EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 707616 Mountain Avatar : e0 -> 712446 (effet 178, Spell::EffectNULL : ne donne rien),
// e1 -> 712465 (effet 177, prolonge 680421 de 5 s, natif). On donne OU on prolonge.
class aura_ascension_primalist_mountain_avatar : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_mountain_avatar);

    bool Check(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        DamageInfo const* damage = eventInfo.GetDamageInfo();
        return Primalist(GetTarget()) && eventInfo.GetActor() == GetTarget() && info && damage &&
            damage->GetDamage() && (info->Id == SPELL_GEODE_BARRAGE_DAMAGE || info->Id == SPELL_GEODE);
    }

    void Grant(AuraEffect const*, ProcEventInfo&)
    {
        PreventDefaultAction();
        _granted = false;
        Unit* owner = GetTarget();
        if (owner->HasAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
            return;
        owner->CastSpell(owner, SPELL_EARTHEN_AVATAR, true);
        if (Aura* avatar = owner->GetAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
        {
            avatar->SetMaxDuration(MOUNTAIN_AVATAR_DURATION);
            avatar->SetDuration(MOUNTAIN_AVATAR_DURATION);
            _granted = true;
        }
    }

    void Extend(AuraEffect const*, ProcEventInfo&)
    {
        Unit* owner = GetTarget();
        if (_granted || !owner->HasAura(SPELL_EARTHEN_AVATAR, owner->GetGUID()))
            PreventDefaultAction();
        _granted = false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(aura_ascension_primalist_mountain_avatar::Check);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_mountain_avatar::Grant,
            EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        OnEffectProc += AuraEffectProcFn(aura_ascension_primalist_mountain_avatar::Extend,
            EFFECT_1, SPELL_AURA_PROC_TRIGGER_SPELL);
    }

    bool _granted = false;
};

// 680421 Earthen Avatar : « immune to stun, root and snare » - aucun des trois effets DBC
// (auras 79, 138, 87) ne l'accorde. Un seul id pour le talent et la version de 5 s.
class aura_ascension_primalist_earthen_avatar : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_earthen_avatar);

    void Immune(Unit* target, bool apply)
    {
        for (uint32 mechanic : {MECHANIC_STUN, MECHANIC_ROOT, MECHANIC_SNARE})
            target->ApplySpellImmune(GetId(), IMMUNITY_MECHANIC, mechanic, apply);
    }

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* target = GetTarget();
        Immune(target, true);
        target->RemoveAurasWithMechanic((1 << MECHANIC_STUN) | (1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE),
            AURA_REMOVE_BY_DEFAULT, GetId());
    }

    void Remove(AuraEffect const*, AuraEffectHandleModes)
    {
        Immune(GetTarget(), false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_primalist_earthen_avatar::Apply,
            EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(aura_ascension_primalist_earthen_avatar::Remove,
            EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
    }
};

// 806048 Son of Ursoc : aura 56 vers la creature 421493, absente de creature_template ;
// HandleAuraTransform pose alors l'affichage 16358 (cochon rose). On pose l'affichage de
// l'ours, et la liberation des ralentissements que l'infobulle promet et qu'aucun effet ne porte.
// Au retrait, HandleAuraTransform restaure l'affichage d'origine.
class aura_ascension_primalist_son_of_ursoc : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_son_of_ursoc);

    void Apply(AuraEffect const*, AuraEffectHandleModes)
    {
        Unit* target = GetTarget();
        if (!sObjectMgr->GetCreatureTemplate(SON_OF_URSOC_CREATURE))
            target->SetDisplayId(SON_OF_URSOC_DISPLAY);
        target->RemoveMovementImpairingAuras(true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(aura_ascension_primalist_son_of_ursoc::Apply,
            EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
    }
};

// 500302 Emerald Will : « Successfully dispelling a poison or disease effect with Soothing
// Touch ». Le coeur n'a pas de masque de coup « dissipation reussie » : on compte avant et apres.
class spell_ascension_primalist_soothing_touch : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_soothing_touch);

    bool Validate(SpellInfo const*) override { return ValidateSpellInfo({SPELL_EMERALD_WILL_HEAL}); }
    bool Load() override { return Primalist(GetCaster()) != nullptr; }

    uint32 Count(Unit* target)
    {
        DispelChargesList list;
        target->GetDispellableAuraList(GetCaster(), (1 << DISPEL_DISEASE) | (1 << DISPEL_POISON), list,
            GetSpellInfo());
        uint32 charges = 0;
        for (auto const& entry : list)
            charges += entry.second;
        return charges;
    }

    void Before(SpellMissInfo miss)
    {
        Unit* target = GetHitUnit();
        _before = miss == SPELL_MISS_NONE && target && GetCaster()->HasAura(SPELL_EMERALD_WILL) ? Count(target) : 0;
    }

    void After()
    {
        Unit* target = GetHitUnit();
        if (_before && target && Count(target) < _before)
            GetCaster()->CastSpell(GetCaster(), SPELL_EMERALD_WILL_HEAL, true);
        _before = 0;
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_ascension_primalist_soothing_touch::Before);
        AfterHit += SpellHitFn(spell_ascension_primalist_soothing_touch::After);
    }

    uint32 _before = 0;
};

// 800180 Sacred Grove : l'aura de zone (e1, DUMMY, passee en MOD_HEALING_PCT par
// primalist_secondary_metadata) ne portait rien. 800186 (tick toutes les
// 2000 ms vers 800179, +25 % de soins recus) existe mais n'est pose par rien ; le poser lui-meme
// propagerait ses trois APPLY_AREA_AURA_RAID en cascade. On le lit, on ne le pose pas.
// 800179 : e0 HEAL_PCT et e1 ENERGIZE_PCT mana sur la cible, e2 ENERGIZE_PCT Rage avec tA 0 -
// Spell.cpp:2033-2062 lui donne la cible explicite (SpellInfo.cpp:822) : rien a verser a la main.
class aura_ascension_primalist_sacred_grove : public AuraScript
{
    PrepareAuraScript(aura_ascension_primalist_sacred_grove);

    bool Validate(SpellInfo const*) override
    {
        return ValidateSpellInfo({SPELL_SACRED_GROVE_DATA, SPELL_SACRED_GROVE_TICK});
    }

    void CalcPeriodic(AuraEffect const*, bool& isPeriodic, int32& amplitude)
    {
        isPeriodic = true;
        amplitude = sSpellMgr->AssertSpellInfo(SPELL_SACRED_GROVE_DATA)->Effects[EFFECT_0].Amplitude;
        if (amplitude <= 0)
            amplitude = 2000;
    }

    void Tick(AuraEffect const*)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (caster && Primalist(caster) && target->IsAlive() && caster->IsInMap(target))
            caster->CastSpell(target, SPELL_SACRED_GROVE_TICK, true);
    }

    void Register() override
    {
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(aura_ascension_primalist_sacred_grove::CalcPeriodic,
            EFFECT_1, SPELL_AURA_MOD_HEALING_PCT);
        OnEffectPeriodic += AuraEffectPeriodicFn(aura_ascension_primalist_sacred_grove::Tick,
            EFFECT_1, SPELL_AURA_MOD_HEALING_PCT);
    }
};

// 301307 Earthmother's Roar (« heals allies equal to $301307s1% of their missing health ») and
// 800179 Sacred Grove tick (800180 : « regain $800179s1% of their missing health, Rage, and
// mana ») : HEAL_PCT (136) and ENERGIZE_PCT (137) give a share of the MAXIMUM. Decision of the
// commissioner (2026-09-30) : the share is taken on what is MISSING, per target.
//  - HEAL_PCT is resolved at launch (EffectHealPct, SpellEffects.cpp) with its done/taken
//    bonuses ; OnHit runs before the heal is dealt (Spell::DoAllEffectOnTarget) and scales it by
//    missing / maximum, read at that moment. A full-health target gets 0 (no heal is dealt).
//  - ENERGIZE_PCT is resolved on hit : the default handler is prevented and the same checks
//    as EffectEnergizePct are made, on max - current.
// 800186's own aura text says « maximum » : it is never applied (see 800180 above), the
// player reads 800180 and 301306.
class spell_ascension_primalist_missing_pct : public SpellScript
{
    PrepareSpellScript(spell_ascension_primalist_missing_pct);

    void ScaleHeal()
    {
        Unit* target = GetHitUnit();
        int32 heal = GetHitHeal();
        if (!target || heal <= 0)
            return;
        uint32 const maximum = target->GetMaxHealth();
        uint32 const current = target->GetHealth();
        if (!maximum || !target->IsAlive() || current >= maximum)
        {
            SetHitHeal(0);
            return;
        }
        SetHitHeal(int32(uint64(heal) * (maximum - current) / maximum));
    }

    void Energize(SpellEffIndex effIndex)
    {
        PreventHitDefaultEffect(effIndex);
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !target->IsAlive())
            return;
        if (target->HasUnitState(UNIT_STATE_ISOLATED))
        {
            caster->SendSpellDamageImmune(target, GetSpellInfo()->Id);
            return;
        }
        int32 const misc = GetSpellInfo()->Effects[effIndex].MiscValue;
        if (misc < 0 || misc >= int8(MAX_POWERS))
            return;
        Powers const power = Powers(misc);
        if (target->IsPlayer() && !target->CanReceivePowerFromSpell(power) &&
            !GetSpellInfo()->HasAttribute(SPELL_ATTR7_ONLY_IN_SPELLBOOK_UNTIL_LEARNED))
            return;
        uint32 const maximum = target->GetMaxPower(power);
        uint32 const current = target->GetPower(power);
        int32 const pct = GetEffectValue();
        if (!maximum || current >= maximum || pct <= 0)
            return;
        if (uint32 gain = CalculatePct(maximum - current, pct))
            caster->EnergizeBySpell(target, GetSpellInfo()->Id, gain, power);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_ascension_primalist_missing_pct::ScaleHeal);
        // 301307 has no ENERGIZE_PCT : an unmatched effect hook only writes an error at start.
        if (SpellInfo const* script = sSpellMgr->GetSpellInfo(m_scriptSpellId))
            if (script->HasEffect(SPELL_EFFECT_ENERGIZE_PCT))
                OnEffectHitTarget += SpellEffectFn(spell_ascension_primalist_missing_pct::Energize, EFFECT_ALL,
                    SPELL_EFFECT_ENERGIZE_PCT);
    }
};
}

void AddSC_AscensionPrimalistTalents()
{
    new primalist_talent_events();
    new primalist_talent_casts();
    RegisterSpellScript(spell_ascension_throat_clamp);
    RegisterSpellScript(aura_ascension_primalist_mineralization);
    RegisterSpellScript(aura_ascension_primalist_shattering);
    RegisterSpellScript(aura_ascension_primalist_infused_with_power);
    RegisterSpellScript(aura_ascension_primalist_totemic_echoes);
    RegisterSpellScript(aura_ascension_primalist_totemic_crash);
    RegisterSpellScript(aura_ascension_primalist_mountain_avatar);
    RegisterSpellScript(aura_ascension_primalist_earthen_avatar);
    RegisterSpellScript(aura_ascension_primalist_son_of_ursoc);
    RegisterSpellScript(spell_ascension_primalist_soothing_touch);
    RegisterSpellScript(aura_ascension_primalist_sacred_grove);
    RegisterSpellScript(spell_ascension_primalist_missing_pct);
    RegisterSpellScript(spell_ascension_primalist_cooldown_chain);
    RegisterSpellScript(spell_ascension_primalist_cooldown_percent);
    RegisterSpellScript(spell_ascension_primalist_fury_of_the_earthmother);
    RegisterSpellScript(aura_ascension_primalist_stone_skin);
    RegisterSpellScript(aura_ascension_primalist_rockslide);
    RegisterSpellScript(aura_ascension_primalist_rexxars_might_pet);
    RegisterSpellScript(aura_ascension_primalist_legacy_of_rexxar);
    RegisterSpellScript(aura_ascension_primalist_primal_shred_crit);
    RegisterSpellScript(aura_ascension_primalist_rock_barrier);
}
