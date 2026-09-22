#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <list>
#include <map>
#include <vector>
using uint8 = std::uint8_t;
using uint32 = std::uint32_t;
using int32 = std::int32_t;
enum
{
    ITEM_CLASS_CONSUMABLE = 0, ITEM_CLASS_ARMOR = 4, ITEM_CLASS_TRADE_GOODS = 7,
    ITEM_SUBCLASS_FOOD = 5, ITEM_SUBCLASS_POTION = 1, EQUIP_ERR_OK = 0, EQUIP_ERR_NONE = 59, LOOT_CORPSE = 1,
    GLOBALHOOK_ON_BEFORE_LOOT_EQUAL_CHANCED = 1,
    // Item.h:40-41 ; EQUIP_ERR_INVENTORY_FULL n'a ici qu'a se distinguer de EQUIP_ERR_OK.
    NULL_BAG = 0, NULL_SLOT = 255, EQUIP_ERR_INVENTORY_FULL = 50
};
using InventoryResult = int;
// Player.h:760 : typedef std::vector<ItemPosCount> ItemPosCountVec;
struct ItemPosCount { uint8 pos = 0; uint32 count = 0; };
using ItemPosCountVec = std::vector<ItemPosCount>;
// LootMgr.h:153 : seuls les trois champs que le module lit sont repris.
struct LootItem { uint32 itemid = 0; int32 randomPropertyId = 0; uint8 count = 1; };
struct ItemTemplate
{
    uint32 id, Class, SubClass, quality, RequiredLevel, ItemLevel;
    bool IsPotion() const { return Class == 0 && SubClass == 1; }
    uint32 GetSkill() const { return Class == 4 ? SubClass : 0; }
};
struct Manager
{
    std::map<uint32, ItemTemplate> items;
    ItemTemplate const* GetItemTemplate(uint32 id) const
    {
        auto it = items.find(id);
        return it == items.end() ? nullptr : &it->second;
    }
} manager;
auto sObjectMgr = &manager;
struct Item
{
    uint32 entry = 1397885, guid = 7;
    uint32 GetEntry() const { return entry; }
    uint32 GetGUID() const { return guid; }
};
struct Player
{
    Item item;
    uint32 level = 1, armorSkill = 1, opened = 0, acknowledgements = 0;
    bool alive = true, combat = false;
    uint32 GetLevel() const { return level; }
    bool IsAlive() const { return alive; }
    bool IsInCombat() const { return combat; }
    Item const* GetItemByGuid(uint32 guid) const { return guid == item.guid ? &item : nullptr; }
    int CanUseItem(ItemTemplate const* value) const { return value->RequiredLevel > level ? 1 : EQUIP_ERR_OK; }
    uint32 GetSkillValue(uint32 skill) const { return skill == armorSkill ? 300 : 0; }
    void SendLoot(uint32 guid, int kind) { assert(guid == item.guid && kind == LOOT_CORPSE); ++opened; }
    // Player.h:1383 : SendEquipError(InventoryResult msg, Item* pItem, Item* pItem2, uint32 itemid).
    std::vector<std::pair<InventoryResult, uint32>> equipErrors;
    void SendEquipError(InventoryResult error, Item* value, Item*, uint32 itemid = 0)
    {
        if (error == EQUIP_ERR_NONE)
        {
            assert(value == &item && !itemid);
            ++acknowledgements;
            return;
        }
        equipErrors.emplace_back(error, itemid);
    }
    // Player.h:1307. Le fixture ne modelise que la place restante : chaque reservation
    // prend une case, et `dest` porte les reservations deja faites.
    uint32 freeSlots = 10;
    InventoryResult CanStoreNewItem(uint8 bag, uint8 slot, ItemPosCountVec& dest, uint32 entry, uint32 count) const
    {
        assert(bag == NULL_BAG && slot == NULL_SLOT && entry && count);
        if (dest.size() >= freeSlots)
            return EQUIP_ERR_INVENTORY_FULL;
        dest.push_back({uint8(dest.size()), count});
        return EQUIP_ERR_OK;
    }
    // Player.h:1374 : DestroyItemCount(Item* item, uint32& count, bool update).
    std::vector<std::pair<uint32, uint32>> destroyed;
    void DestroyItemCount(Item* value, uint32& count, bool update)
    {
        assert(update && value);
        destroyed.emplace_back(value->GetEntry(), count);
    }
    // Player.h:1335. std::list : les pointeurs rendus doivent rester valides.
    std::list<Item> stored;
    Item* StoreNewItem(ItemPosCountVec const& pos, uint32 entry, bool update, int32 randomPropertyId)
    {
        assert(update && !pos.empty());
        (void)randomPropertyId;
        stored.push_back(Item{entry, 1000 + uint32(stored.size())});
        return &stored.back();
    }
    // Player.h:1396 : SendNewItem(Item*, uint32 count, bool received, bool created, bool broadcast).
    std::vector<std::pair<uint32, uint32>> given;
    void SendNewItem(Item* value, uint32 count, bool received, bool created, bool broadcast)
    {
        assert(!received && !created && broadcast);
        given.emplace_back(value->GetEntry(), count);
    }
};
struct LootStoreItem { uint32 itemid, reference = 0; };
struct LootStore {} LootTemplates_Item, otherStore;
// Ce que la table de butin rendra au prochain FillLoot.
std::vector<LootItem> fixtureLootRoll;
struct Loot
{
    uint32 containerGUID = 7;
    std::vector<uint32> awarded;
    void AddItem(LootStoreItem const& item) { awarded.push_back(item.itemid); }
    // LootMgr.h:377, 382, 383.
    std::vector<LootItem> items;
    uint32 filledFrom = 0;
    bool FillLoot(uint32 lootId, LootStore const& store, Player* lootOwner, bool personal, bool noEmptyError)
    {
        assert(&store == &LootTemplates_Item && lootOwner && personal && noEmptyError);
        filledFrom = lootId;
        items = fixtureLootRoll;
        return !items.empty();
    }
    uint32 GetMaxSlotInLootFor(Player*) const { return uint32(items.size()); }
    LootItem* LootItemInSlot(uint32 slot, Player*) { return slot < items.size() ? &items[slot] : nullptr; }
};
struct SpellCastTargets {};
struct ItemScript
{
    explicit ItemScript(char const*) { }
    virtual bool OnUse(Player*, Item*, SpellCastTargets const&) { return false; }
};
struct GlobalScript
{
    GlobalScript(char const*, std::initializer_list<int>) { }
    virtual bool OnBeforeLootEqualChanced(Player const*, std::list<LootStoreItem*>, Loot&, LootStore const&)
    { return true; }
};
uint32 firstRoll = 0, secondRoll = 0, rolls = 0;
uint32 urand(uint32 low, uint32 high)
{
    assert(low <= high);
    return low + ((rolls++ % 2 ? secondRoll : firstRoll) % (high - low + 1));
}
