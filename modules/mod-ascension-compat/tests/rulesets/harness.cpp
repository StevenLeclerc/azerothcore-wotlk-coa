#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
using uint32 = std::uint32_t;
using uint8 = std::uint8_t;
using int32 = std::int32_t;
using uint64 = std::uint64_t;
// ObjectGuid.h:216 pour ToString ; le module s'en sert comme clef de table et
// teste sa vacuite avec IsEmpty() et avec `!guid`.
struct ObjectGuid
{
    uint32 value = 0;
    ObjectGuid() = default;
    explicit ObjectGuid(uint32 raw) : value(raw) { }
    bool IsEmpty() const { return !value; }
    explicit operator bool() const { return value != 0; }
    uint32 GetCounter() const { return value; }
    std::string ToString() const { return "Player-0-" + std::to_string(value); }
    bool operator==(ObjectGuid const& other) const { return value == other.value; }
    bool operator!=(ObjectGuid const& other) const { return value != other.value; }
};
template<> struct std::hash<ObjectGuid>
{
    std::size_t operator()(ObjectGuid const& guid) const noexcept { return guid.value; }
};
// Journal : on retient le texte de format, pas le rendu.
inline std::vector<std::string> loggedInfo, loggedErrors;
template<class... Args> void LogInfoStub(char const*, char const* format, Args const&...)
{ loggedInfo.push_back(format); }
template<class... Args> void LogErrorStub(char const*, char const* format, Args const&...)
{ loggedErrors.push_back(format); }
#define LOG_INFO(category, ...) LogInfoStub(category, __VA_ARGS__)
#define LOG_ERROR(category, ...) LogErrorStub(category, __VA_ARGS__)
// Tirage rendu deterministe : le harnais choisit la piece perdue.
inline uint32 fixtureRoll = 0;
inline uint32 urand(uint32 low, uint32 high) { assert(low <= high); return low + fixtureRoll % (high - low + 1); }
// ENUMS
using SpellEffIndex = uint32;
constexpr uint32 EFFECT_0 = 0, SPELL_EFFECT_DUMMY = 3;
constexpr int GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR = 1, AURA_REMOVE_BY_DEATH = 1;
constexpr int PLAYERHOOK_ON_LOGIN = 1, PLAYERHOOK_ON_UPDATE = 2, PLAYERHOOK_ON_LOGOUT = 3,
    PLAYERHOOK_ON_PVP_KILL = 4, PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE = 5,
    PLAYERHOOK_ON_PLAYER_RELEASED_GHOST = 6;
// Player.h:659 et :663-683 ; Item.h:40-41 ; EQUIP_ERR_OK est le seul code lu ici.
constexpr uint8 INVENTORY_SLOT_BAG_0 = 255, EQUIPMENT_SLOT_START = 0, EQUIPMENT_SLOT_END = 19,
    NULL_BAG = 0, NULL_SLOT = 255;
constexpr int EQUIP_ERR_OK = 0, EQUIP_ERR_INVENTORY_FULL = 50;
// Item.h:209-210.
constexpr int ITEM_UNCHANGED = 0, ITEM_CHANGED = 1;
// Mail.h:37, :49, :58.
constexpr int MAIL_NORMAL = 0, MAIL_CHECK_MASK_COPIED = 0x04, MAIL_STATIONERY_DEFAULT = 41;
// Unit.h:202-208.
enum class DeathState : uint8 { Alive = 0, JustDied = 1, Corpse = 2, Dead = 3, JustRespawned = 4 };
struct ItemPosCount { uint8 pos = 0; uint32 count = 0; };
using ItemPosCountVec = std::vector<ItemPosCount>;
struct Player;
struct Aura;
struct AuraApplication;
// ItemTemplate.h : seuls ItemLevel et Name1 sont lus par le module.
struct ItemTemplate { uint32 ItemLevel = 0; std::string Name1 = "Test Item"; };
struct Item
{
    uint32 entry = 5000;
    ObjectGuid guid{500};
    uint8 bag = INVENTORY_SLOT_BAG_0, slot = 0;
    ItemTemplate proto;
    int state = ITEM_UNCHANGED;
    ObjectGuid owner;
    bool deletedFromDB = false, savedToDB = false;
    uint32 GetEntry() const { return entry; }
    ObjectGuid GetGUID() const { return guid; }
    uint8 GetBagSlot() const { return bag; }
    uint8 GetSlot() const { return slot; }
    ItemTemplate const* GetTemplate() const { return &proto; }
    int GetState() const { return state; }
    void FSetState(int value) { state = value; }
    void SetOwnerGUID(ObjectGuid value) { owner = value; }
    void DeleteFromInventoryDB(int) { deletedFromDB = true; }
    void SaveToDB(int) { savedToDB = true; }
};
struct Map
{
    bool dungeon = false, battlegroundOrArena = false;
    bool IsDungeon() const { return dungeon; }
    bool IsBattlegroundOrArena() const { return battlegroundOrArena; }
};
struct WorldSession
{
    bool loggingOut = false;
    std::vector<std::string> messages;
    bool PlayerLogout() const { return loggingOut; }
};
// Chat.h : PSendSysMessage passe par Acore::StringFormat, d'ou les {} du module.
// Le fixture ne retient que le format : il prouve QUEL message est parti.
struct ChatHandler
{
    WorldSession* session;
    explicit ChatHandler(WorldSession* value) : session(value) { }
    template<class... Args> void PSendSysMessage(char const* format, Args const&...)
    { session->messages.emplace_back(format); }
};
struct PvPInfo { bool IsInNoPvPArea = false, IsInFFAPvPArea = false; };
struct CharacterCacheEntry { std::string Name; };
struct CharacterCacheStub
{
    std::map<uint32, CharacterCacheEntry> rows;
    CharacterCacheEntry const* GetCharacterCacheByGuid(ObjectGuid guid) const
    {
        auto it = rows.find(guid.GetCounter());
        return it == rows.end() ? nullptr : &it->second;
    }
} characterCacheStub;
#define sCharacterCache (&characterCacheStub)
using CharacterDatabaseTransaction = int;
struct CharacterDatabaseStub
{
    uint32 transactions = 0, commits = 0;
    CharacterDatabaseTransaction BeginTransaction() { return int(++transactions); }
    void CommitTransaction(CharacterDatabaseTransaction) { ++commits; }
} CharacterDatabase;
struct SpellInfo
{
    uint32 Id = 0, Attributes = 0, AttributesEx3 = 0, Stances = 0;
    bool HasAttribute(SpellAttr0 attr) const { return (Attributes & attr) != 0; }
    bool HasAttribute(SpellCustomAttributes) const { return false; }
    bool HasAttribute(SpellAttr7) const { return false; }
    bool IsChanneled() const { return false; }
    bool IsSingleTarget() const { return false; }
    bool IsDeathPersistent() const;
};
struct Unit
{
    virtual ~Unit() = default;
    virtual Player* ToPlayer() { return nullptr; }
    ObjectGuid guid{1};
    uint8 level = 30;
    bool isPlayer = false;
    ObjectGuid GetGUID() const { return guid; }
    uint8 GetLevel() const { return level; }
    bool IsPlayer() const { return isPlayer; }
    using AuraApplicationMap = std::map<uint32, AuraApplication*>;
    using AuraMap = std::map<uint32, Aura*>;
    AuraApplicationMap m_appliedAuras;
    AuraMap m_ownedAuras;
    void _UnapplyAura(AuraApplicationMap::iterator& it, int) { it = m_appliedAuras.erase(it); }
    void RemoveOwnedAura(AuraMap::iterator& it, int) { it = m_ownedAuras.erase(it); }
    void RemoveAllAurasOnDeath();
};
struct Creature : Unit { };
struct Player : Unit
{
    Player() { isPlayer = true; }
    bool resting = false;
    bool inWorld = true;
    bool IsInWorld() const { return inWorld; }
    // --- Ce que la sanction High Risk lit et ecrit sur le joueur. ---
    std::string name = "Victim";
    WorldSession* session = nullptr;
    Map* map = nullptr;
    PvPInfo pvpInfo;
    bool gameMaster = false, battleground = false, arena = false, teleporting = false;
    uint32 money = 0, health = 0, ffaUpdates = 0;
    DeathState deathState = DeathState::Alive;
    uint32 flags = 0;
    std::map<uint8, Item*> equipment;      // emplacement -> piece portee
    std::vector<Item*> bags;               // pieces recues par MoveItemToInventory
    std::vector<uint8> destroyed, detached;
    bool bagsFull = false;
    std::string const& GetName() const { return name; }
    WorldSession* GetSession() const { return session; }
    Map* GetMap() const { return map; }
    Map* FindMap() const { return map; }
    bool IsGameMaster() const { return gameMaster; }
    bool InBattleground() const { return battleground; }
    bool InArena() const { return arena; }
    bool IsBeingTeleported() const { return teleporting; }
    DeathState getDeathState() { return deathState; } // Unit.h:1806, non const dans le moteur
    uint32 GetHealth() const { return health; }
    void SetHealth(uint32 value) { health = value; }
    uint32 GetMoney() const { return money; }
    // Player.h:1636 : bool ModifyMoney(int32 amount, bool sendError = true);
    bool ModifyMoney(int32 amount, bool sendError)
    {
        assert(!sendError);
        money = uint32(int32(money) + amount);
        return true;
    }
    void UpdateFFAPvPState(bool reset) { assert(!reset); ++ffaUpdates; } // Player.h:1895
    Item* GetItemByPos(uint8 bag, uint8 slot) const
    {
        assert(bag == INVENTORY_SLOT_BAG_0);
        auto it = equipment.find(slot);
        return it == equipment.end() ? nullptr : it->second;
    }
    void DestroyItem(uint8 bag, uint8 slot, bool update)
    {
        assert(bag == INVENTORY_SLOT_BAG_0 && update);
        destroyed.push_back(slot);
        equipment.erase(slot);
    }
    void MoveItemFromInventory(uint8 bag, uint8 slot, bool update)
    {
        assert(bag == INVENTORY_SLOT_BAG_0 && update);
        detached.push_back(slot);
        equipment.erase(slot);
    }
    // Player.h:1312 : CanStoreItem(bag, slot, dest, Item* pItem, bool swap) const.
    int CanStoreItem(uint8 bag, uint8 slot, ItemPosCountVec& dest, Item* item, bool swap) const
    {
        assert(bag == NULL_BAG && slot == NULL_SLOT && item && !swap);
        if (bagsFull)
            return EQUIP_ERR_INVENTORY_FULL;
        dest.push_back({0, 1});
        return EQUIP_ERR_OK;
    }
    // Player.h:1369 : MoveItemToInventory(dest, pItem, update, in_characterInventoryDB).
    void MoveItemToInventory(ItemPosCountVec const& dest, Item* item, bool update, bool inCharacterInventoryDB)
    {
        assert(!dest.empty() && update && inCharacterInventoryDB);
        bags.push_back(item);
    }
    std::set<uint32> auras;
    std::set<uint32> known;
    uint32 learns = 0;
    bool HasSpell(uint32 id) const { return known.contains(id); }
    void learnSpell(uint32 id, bool dependent) { assert(!dependent); known.insert(id); ++learns; }
    Player* ToPlayer() override { return this; }
    bool HasPlayerFlag(PlayerFlags flag) const
    {
        if (flag == PLAYER_FLAGS_RESTING)
            return resting;
        return (flags & uint32(flag)) != 0;
    }
    bool HasAura(uint32 id) const { return auras.contains(id); }
    void RemoveAurasDueToSpell(uint32 id) { auras.erase(id); }
    void CastSpell(Player* target, uint32 id, bool triggered)
    {
        assert(target == this && triggered);
        auras.insert(id);
    }
};
struct Aura
{
    SpellInfo* info;
    Unit* owner;
    SpellInfo const* GetSpellInfo() const { return info; }
    ObjectGuid GetCasterGUID() const { return owner->GetGUID(); } // SpellAuras.h
    Unit* GetOwner() const { return owner; }
    bool IsPassive() const { return info->HasAttribute(SPELL_ATTR0_PASSIVE); }
    bool IsDeathPersistent() const { return info->IsDeathPersistent(); }
    bool IsSingleTarget() const { return false; }
    bool IsUsingCharges() const { return false; }
    uint32 GetCharges() const { return 0; }
    bool CanBeSaved() const;
};
struct AuraApplication { Aura* aura; Aura const* GetBase() const { return aura; } };
struct Hook { template<class T> void operator+=(T) { } };
struct SpellScript
{
    Unit* caster = nullptr;
    SpellInfo* info = nullptr;
    Hook OnCheckCast, OnEffectHit;
    virtual bool Validate(SpellInfo const*) { return true; }
    virtual bool Load() { return true; }
    virtual void Register() { }
    Unit* GetCaster() const { return caster; }
    SpellInfo const* GetSpellInfo() const { return info; }
    bool ValidateSpellInfo(std::initializer_list<uint32> ids)
    {
        assert((std::set<uint32>(ids) == std::set<uint32>{1004019, 1004119, 9931032}));
        return true;
    }
};
struct GlobalScript
{
    GlobalScript(char const*, std::initializer_list<int>) { }
    virtual void OnLoadSpellCustomAttr(SpellInfo*) { }
};
struct PlayerScript
{
    PlayerScript(char const*, std::initializer_list<int>) { }
    virtual ~PlayerScript() = default;
    virtual void OnPlayerLogin(Player*) { }
    virtual void OnPlayerPVPKill(Player*, Player*) { }
    virtual void OnPlayerKilledByCreature(Creature*, Player*) { }
    virtual void OnPlayerReleasedGhost(Player*) { }
    virtual void OnPlayerUpdate(Player*, uint32) { }
    virtual void OnPlayerLogout(Player*) { }
};
// ObjectAccessor.h : FindConnectedPlayer rend le joueur en ligne, ou nullptr.
namespace ObjectAccessor
{
inline std::map<uint32, Player*> connected;
inline Player* FindConnectedPlayer(ObjectGuid guid)
{
    auto it = connected.find(guid.GetCounter());
    return it == connected.end() ? nullptr : it->second;
}
}
// Mail.h : seul l'enchainement AddItem/SendMailTo est reproduit ; on retient
// le destinataire et la piece pour pouvoir l'affirmer.
inline std::vector<std::pair<uint32, uint32>> sentMail;  // destinataire -> entree d'objet
struct MailReceiver
{
    Player* player;
    uint32 guidLow;
    MailReceiver(Player* value, uint32 low) : player(value), guidLow(low) { }
};
struct MailSender
{
    MailSender(int type, uint32 sender, int stationery)
    {
        assert(type == MAIL_NORMAL && stationery == MAIL_STATIONERY_DEFAULT);
        (void)sender;
    }
};
struct MailDraft
{
    Item* attachment = nullptr;
    MailDraft(std::string const&, std::string const&) { }
    MailDraft& AddItem(Item* item) { attachment = item; return *this; }
    void SendMailTo(CharacterDatabaseTransaction, MailReceiver const& receiver, MailSender const&, int mask)
    {
        assert(mask == MAIL_CHECK_MASK_COPIED);
        sentMail.emplace_back(receiver.guidLow, attachment ? attachment->GetEntry() : 0u);
    }
};
struct ConfigMgrStub
{
    bool rulesetLoginDefault = true;
    // Le defaut demande par l'appelant est rendu tel quel : une valeur de
    // configuration inventee ici ferait passer un test pour la mauvaise raison.
    std::map<std::string, uint32> overrides;
    template <class T> T GetOption(char const* name, T fallback) const
    {
        if (std::string(name) == "AscensionCompat.RulesetLoginDefault")
            return T(rulesetLoginDefault);
        auto it = overrides.find(name);
        return it == overrides.end() ? fallback : T(it->second);
    }
};
ConfigMgrStub configMgrStub;
#define sConfigMgr (&configMgrStub)
#define PrepareSpellScript(name)
#define RegisterSpellScript(name)
#define SpellCheckCastFn(...) 0
#define SpellEffectFn(...) 0
// NATIVE
// SOURCE
void CheckAura(SpellInfo info)
{
    Unit owner;
    Aura aura{&info, &owner};
    AuraApplication app{&aura};
    assert(aura.CanBeSaved() && aura.IsDeathPersistent()); // Current DBC can persist natively.
    info.AttributesEx3 &= ~SPELL_ATTR3_ALLOW_AURA_WHILE_DEAD;
    owner.m_appliedAuras.emplace(1, &app);
    owner.m_ownedAuras.emplace(1, &aura);
    owner.RemoveAllAurasOnDeath();
    assert(owner.m_appliedAuras.empty() && owner.m_ownedAuras.empty());
    ruleset_aura_metadata metadata;
    metadata.OnLoadSpellCustomAttr(&info);
    owner.m_appliedAuras.emplace(1, &app);
    owner.m_ownedAuras.emplace(1, &aura);
    owner.RemoveAllAurasOnDeath();
    assert(owner.m_appliedAuras.size() == 1 && owner.m_ownedAuras.size() == 1 && aura.CanBeSaved());
}
int main()
{
    Player player;
    ruleset_player_spells login;
    player.known.insert(84421);
    player.known.insert(123);
    player.auras.insert(123); // The login default leaves unrelated auras alone.
    login.OnPlayerLogin(&player);
    assert((player.known == std::set<uint32>{123, 84420, 84421, 84422}) && player.learns == 2);
    // A character created without any ruleset aura falls back to PvE instead of Ruleset.None.
    assert((player.auras == std::set<uint32>{123, 1004119, 9931032}));
    login.OnPlayerLogin(&player);
    assert(player.learns == 2 && (player.auras == std::set<uint32>{123, 1004119, 9931032}));
    // Any ruleset already on the character is kept, including High-Risk and War Mode.
    for (auto const& kept : {std::set<uint32>{1004019}, std::set<uint32>{1004119},
                             std::set<uint32>{1004119, 9931032}})
    {
        player.auras = kept;
        login.OnPlayerLogin(&player);
        assert(player.auras == kept);
    }
    // The config gate suppresses only the default; the selection spells are still learned.
    configMgrStub.rulesetLoginDefault = false;
    Player gated;
    login.OnPlayerLogin(&gated);
    assert((gated.known == std::set<uint32>{84420, 84421, 84422}) && gated.learns == 3 && gated.auras.empty());
    configMgrStub.rulesetLoginDefault = true;
    // A character evicted from an instance is out of the world at login; the default waits for the next one.
    Player evicted;
    evicted.inWorld = false;
    login.OnPlayerLogin(&evicted);
    assert((evicted.known == std::set<uint32>{84420, 84421, 84422}) && evicted.auras.empty());
    Unit npc;
    SpellInfo info;
    spell_ascension_ruleset_select script;
    script.caster = &npc;
    script.info = &info;
    assert(!script.Load() && script.CheckCast() == SPELL_FAILED_NOT_HERE);
    script.caster = &player;
    assert(script.Load() && script.Validate(&info) && script.CheckCast() == SPELL_FAILED_NOT_HERE);
    player.resting = true;
    assert(script.CheckCast() == SPELL_CAST_OK);
    const std::map<uint32, std::set<uint32>> modes = {
        {84420, {1004119}}, {84421, {1004019}}, {84422, {1004119, 9931032}}
    };
    for (auto const& [previous, auras] : modes)
        for (auto const& [selected, expected] : modes)
        {
            (void)previous;
            player.auras = auras;
            player.auras.insert(123); // Switching preserves unrelated auras.
            info.Id = selected;
            script.Select(0);
            auto wanted = expected;
            wanted.insert(123);
            assert(player.auras == wanted);
            script.Select(0);
            assert(player.auras == wanted);
        }
    auto before = player.auras;
    info.Id = 123;
    script.Select(0);
    assert(player.auras == before);
    ruleset_aura_metadata metadata;
    metadata.OnLoadSpellCustomAttr(&info);
    assert(info.AttributesEx3 == 0);
    // DBC_CASES

    // ------------------------------------------------------------------
    // La sanction High Risk : le seul filet automatisable de cette zone.
    // ------------------------------------------------------------------
    Map open;
    Map dungeon; dungeon.dungeon = true;
    WorldSession victimSession;
    Player victim;
    victim.guid = ObjectGuid(10);
    victim.name = "Victim";
    victim.session = &victimSession;
    victim.map = &open;
    victim.level = 30;
    victim.auras.insert(1004019); // SPELL_HIGH_RISK

    // Chaque garde de PenaltyApplies compte, une par une.
    assert(HighRisk::PenaltyApplies(&victim));
    assert(!HighRisk::PenaltyApplies(nullptr));
    victim.gameMaster = true; assert(!HighRisk::PenaltyApplies(&victim)); victim.gameMaster = false;
    victim.inWorld = false; assert(!HighRisk::PenaltyApplies(&victim)); victim.inWorld = true;
    victim.battleground = true; assert(!HighRisk::PenaltyApplies(&victim)); victim.battleground = false;
    victim.arena = true; assert(!HighRisk::PenaltyApplies(&victim)); victim.arena = false;
    victim.pvpInfo.IsInNoPvPArea = true; assert(!HighRisk::PenaltyApplies(&victim));
    victim.pvpInfo.IsInNoPvPArea = false;
    victim.map = &dungeon; assert(!HighRisk::PenaltyApplies(&victim)); victim.map = &open;
    victim.auras.erase(1004019); assert(!HighRisk::PenaltyApplies(&victim)); victim.auras.insert(1004019);

    // Les unites nulles ne s'affichent pas.
    assert(HighRisk::MoneyText(0) == "0 copper");
    assert(HighRisk::MoneyText(6) == "6 copper");
    assert(HighRisk::MoneyText(600) == "6 silver");
    assert(HighRisk::MoneyText(10006) == "1 gold 6 copper");
    assert(HighRisk::MoneyText(10606) == "1 gold 6 silver 6 copper");

    // La prime suit le niveau d'objet porte, au tarif par defaut de 100 cuivre.
    Item chest; chest.slot = 4; chest.entry = 5000; chest.guid = ObjectGuid(500);
    chest.proto.ItemLevel = 70; chest.proto.Name1 = "Chest";
    Item weapon; weapon.slot = 15; weapon.entry = 5001; weapon.guid = ObjectGuid(501);
    weapon.proto.ItemLevel = 30; weapon.proto.Name1 = "Sword";
    assert(HighRisk::Premium(&victim) == 0); // nu : rien a assurer
    victim.equipment[4] = &chest;
    victim.equipment[15] = &weapon;
    assert(HighRisk::Premium(&victim) == (70 + 30) * 100);

    WorldSession killerSession;
    Player killer;
    killer.guid = ObjectGuid(20);
    killer.name = "Killer";
    killer.level = 31;
    killer.map = &open;
    killer.session = &killerSession;
    characterCacheStub.rows[20] = {"Killer"};
    ObjectAccessor::connected[20] = &killer;

    // 1. Solvable : la prime est prelevee, l'equipement reste.
    victim.money = 20000;
    HighRisk::Remember(&victim, &killer);
    assert(HighRisk::ArmedDebts.load() == 0);  // notee, pas encore armee
    HighRisk::Tick(&victim, 100000);
    assert(victim.money == 20000);             // une dette non armee ne se solde pas
    HighRisk::Arm(&victim);
    assert(HighRisk::ArmedDebts.load() == 1);
    HighRisk::Tick(&victim, 1000);             // delai de grace : 2000 ms par defaut
    assert(victim.money == 20000 && HighRisk::ArmedDebts.load() == 1);
    HighRisk::Tick(&victim, 1000);
    assert(victim.money == 10000 && HighRisk::ArmedDebts.load() == 0);
    assert(victim.equipment.size() == 2 && victim.detached.empty() && victim.destroyed.empty());

    // 2. Insolvable : la piece tiree part dans les sacs du tueur.
    victim.money = 0;
    fixtureRoll = 0;                           // premiere piece portee : l'emplacement 4
    HighRisk::Remember(&victim, &killer);
    HighRisk::Arm(&victim);
    HighRisk::Tick(&victim, 5000);
    assert(victim.equipment.count(4) == 0 && victim.detached.size() == 1 && victim.detached.back() == 4);
    assert(killer.bags.size() == 1 && killer.bags.back() == &chest);
    assert(victim.destroyed.empty() && sentMail.empty());

    // 3. Sacs pleins : la piece part par courrier, au nom du tueur.
    killer.bagsFull = true;
    victim.money = 0;
    fixtureRoll = 0;                           // il ne reste que l'emplacement 15
    HighRisk::Remember(&victim, &killer);
    HighRisk::Arm(&victim);
    HighRisk::Tick(&victim, 5000);
    assert(sentMail.size() == 1 && sentMail.back().first == 20 && sentMail.back().second == 5001);
    assert(weapon.owner == ObjectGuid(20) && weapon.savedToDB && weapon.deletedFromDB);
    killer.bagsFull = false;

    // 4. Ecart de niveau : rien ne se perd.
    killer.level = 40;
    victim.equipment[4] = &chest;
    victim.money = 0;
    HighRisk::Remember(&victim, &killer);
    HighRisk::Arm(&victim);
    HighRisk::Tick(&victim, 5000);
    assert(victim.equipment.count(4) == 1 && victim.destroyed.empty() && sentMail.size() == 1);
    killer.level = 31;

    // 5. Une dette oubliee ne laisse pas le portillon ouvert.
    HighRisk::Remember(&victim, &killer);
    HighRisk::Arm(&victim);
    assert(HighRisk::ArmedDebts.load() == 1);
    HighRisk::Forget(&victim);
    assert(HighRisk::ArmedDebts.load() == 0);
    HighRisk::Tick(&victim, 5000);
    assert(victim.equipment.count(4) == 1);

    // 6. Mort en PvE : personne a qui remettre la piece, elle est detruite.
    Creature beast;
    beast.guid = ObjectGuid(99);
    beast.level = 31;
    victim.money = 0;
    fixtureRoll = 0;
    HighRisk::Remember(&victim, &beast);
    HighRisk::Arm(&victim);
    HighRisk::Tick(&victim, 5000);
    assert(victim.destroyed.size() == 1 && victim.destroyed.back() == 4);
    assert(sentMail.size() == 1 && killer.bags.size() == 1);

    // Le drapeau FFA suit l'aura, et seulement aux transitions.
    ruleset_high_risk_ffa ffa;
    Player flagged;
    flagged.map = &open;
    flagged.auras.insert(1004019);
    ffa.OnPlayerUpdate(&flagged, 100);
    assert(flagged.pvpInfo.IsInFFAPvPArea && flagged.ffaUpdates == 1);
    ffa.OnPlayerUpdate(&flagged, 100);
    assert(flagged.ffaUpdates == 1);
    flagged.auras.erase(1004019);
    ffa.OnPlayerUpdate(&flagged, 100);
    assert(!flagged.pvpInfo.IsInFFAPvPArea && flagged.ffaUpdates == 2);
    flagged.auras.insert(1004019);
    flagged.gameMaster = true;
    ffa.OnPlayerUpdate(&flagged, 100);
    assert(!flagged.pvpInfo.IsInFFAPvPArea && flagged.ffaUpdates == 2);

    // Le filet du cadavre a vie non nulle : il corrige, et il ne parle qu'une fois.
    ruleset_dead_player_health_guard guard;
    Player corpse;
    corpse.map = &open;
    corpse.guid = ObjectGuid(30);
    corpse.deathState = DeathState::Corpse;
    corpse.health = 500;
    std::size_t const errorsBefore = loggedErrors.size();
    guard.OnPlayerUpdate(&corpse, 100);
    assert(!corpse.health && loggedErrors.size() == errorsBefore + 1);
    corpse.health = 500;
    guard.OnPlayerUpdate(&corpse, 100);
    assert(!corpse.health && loggedErrors.size() == errorsBefore + 1);
    corpse.deathState = DeathState::Alive;
    guard.OnPlayerUpdate(&corpse, 100);       // sorti de l'etat de cadavre : signalement rearme
    corpse.deathState = DeathState::Corpse;
    corpse.health = 500;
    guard.OnPlayerUpdate(&corpse, 100);
    assert(!corpse.health && loggedErrors.size() == errorsBefore + 2);
    // Un fantome legitime porte 1 PV et doit etre epargne.
    corpse.flags = PLAYER_FLAGS_GHOST;
    corpse.health = 1;
    guard.OnPlayerUpdate(&corpse, 100);
    assert(corpse.health == 1 && loggedErrors.size() == errorsBefore + 2);
}
