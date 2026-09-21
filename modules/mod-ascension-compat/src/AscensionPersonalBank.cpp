/* Copyright (C) 2016+ AzerothCore, GNU AGPL v3. */
/*
 * The storage behind the Personal Bank, Celestial Personal Bank and Realm Bank items.
 *
 * The client's vault frame for those items *is* the guild vault window: its patched Lua
 * switches the frame into a personal mode from SMSG_BANK_PERMISSIONS, and from then on it
 * speaks the ordinary guild-bank conversation (activate, query tab, swap items, deposit and
 * withdraw money, buy tab, rename tab). Answering that with a real guild would mean putting
 * the character in a guild they did not join, and Ascension's own bank is independent of
 * guild membership ("You can be in a guild and still have a personal bank"), so instead the
 * module answers the whole conversation itself and keeps the contents in its own tables.
 *
 * Storage layout (see this module's db-characters SQL):
 *
 *   mod_ascension_bank_tab     one row per owned tab
 *   mod_ascension_bank_item    tab/slot -> item_instance guid
 *   mod_ascension_bank_money   the drawer's gold
 *
 * Items are stored the way guild-bank items are: their own item_instance row with the owner
 * and container fields empty, saved standalone. That is what keeps enchantments, gems,
 * durability, random properties and charges intact across a relog, and it is why loading
 * here mirrors the core's own guild-bank query field for field.
 *
 * A bank belongs either to one character (the personal and celestial items) or to the account
 * (the realm item, which every character of that account shares), which is the entire difference
 * between the variants: same code, same tables, a different owner row.
 */
#include "AscensionPersonalBank.h"

#include "Bag.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildPackets.h"
#include "Item.h"
#include "ItemScript.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace
{
constexpr uint8 BANK_TABS = GUILD_BANK_MAX_TABS;
constexpr uint8 BANK_SLOTS = GUILD_BANK_MAX_SLOTS;

// Which row of the storage tables a bank lives in: one character's own bank, or the account's
// realm bank.
constexpr uint8 OWNER_CHARACTER = 0;
constexpr uint8 OWNER_REALM = 1;

/// The row a bank lives in.
///
/// The character's own bank is keyed by that character. The realm bank is keyed by the account,
/// so its characters share one drawer and a different account gets its own - a drawer keyed by
/// nothing at all made a single bank for the whole realm, which handed one account's belongings
/// to everyone who walked up to a summoned vault.
[[nodiscard]] uint64 BankOwnerId(Player const* player, uint8 kind)
{
    return kind == AscensionPersonalBank::REALM
               ? uint64(player->GetSession()->GetAccountId())
               : player->GetGUID().GetCounter();
}

// What the client asked for when it opened a tab: the frame wants the whole tab, or just
// the slots that changed.
constexpr bool SEND_ALL_SLOTS = true;
constexpr bool SEND_CHANGED_SLOTS = false;

// What a tab shows until it is named and given an icon of its own: the picture CoA draws for its
// own bank items. Item 110000/1180097 use display 64388, and that display's InventoryIcon in the
// client's DBFilesClient\ItemDisplayInfo.dbc is this - a bare icon name, no path and no extension.
//
// The format is not cosmetic. This client resolves a bare name (it is exactly how its own tab-icon
// picker writes mod_ascension_bank_tab.icon, through CMSG_SET_GUILD_BANK_TEXT), while a legacy
// "Interface\\Icons\\..." path does not resolve here - and a texture that fails to set leaves the
// previous texture on the button, which is how a tab that had once been painted as the buy tab's
// "+" kept showing that "+" for the rest of the session and looked right again after a relog.
constexpr char const* DEFAULT_TAB_ICON = "achievement_guildperk_mobilebanking";

// The extension opcode the client reads the two bank flags from; the frame's
// BANK_PERMISSIONS_PAYLOAD hook turns GetBankPermissions() into its presentation mode.
constexpr uint16 SMSG_BANK_PERMISSIONS = 0x0769;

// The bank window is only valid while the character stands next to the object that opened
// it; walking away or letting it despawn ends the session.
constexpr float BANK_REACH = 12.0f;

struct OpenBank
{
    uint8 OwnerKind = OWNER_CHARACTER;
    uint64 OwnerId = 0;

    ObjectGuid Vault;   // the summoned object the window was opened on

    uint8 Tabs = 1;
    std::array<std::string, BANK_TABS> TabName;
    std::array<std::string, BANK_TABS> TabIcon;
    std::array<std::string, BANK_TABS> TabText;

    std::array<std::array<Item*, BANK_SLOTS>, BANK_TABS> Items{};
    uint64 Money = 0;
};

std::unordered_map<ObjectGuid::LowType, OpenBank> openBanks;

/// Guards the *shape* of `openBanks`, nothing else.
///
/// The table is read from the map threads and written from the world thread. `IsOpen` is
/// called from `CanPacketReceive` for every opcode a session receives, and CMSG_USE_ITEM
/// (the tab voucher, which writes) is PROCESS_INPLACE, so a map thread can be walking the
/// buckets while another thread rehashes them - the crash of P-050.
///
/// Reads are shared, insertions and erasures exclusive. The *contents* of one OpenBank need
/// no guard: every entry is keyed by the acting character and only that character's own
/// session ever touches it, and a session is updated by one thread at a time.
std::shared_mutex openBanksLock;

/// How many banks are open, so the per-opcode `IsOpen` costs one relaxed load instead of a
/// lock on a realm where nobody has a bank window up.
std::atomic<uint32> openBankCount{0};

/// The bank `guid` has open, or nullptr.
///
/// The pointer outlives the lock on purpose: std::unordered_map only invalidates pointers and
/// references to the element that is erased (a rehash moves buckets, not nodes), and the only
/// erasures of this key - Opened and Closed - run on this same character's session. That is
/// what lets the callers do their database work and send their packets unlocked.
[[nodiscard]] OpenBank* FindOpenBank(ObjectGuid::LowType guid)
{
    if (!openBankCount.load(std::memory_order_acquire))
        return nullptr;

    std::shared_lock<std::shared_mutex> guard(openBanksLock);
    auto itr = openBanks.find(guid);
    return itr == openBanks.end() ? nullptr : &itr->second;
}

// ---------------------------------------------------------------------------------------
// Loading and unloading
// ---------------------------------------------------------------------------------------

void LoadBank(OpenBank& bank)
{
    QueryResult tabs = CharacterDatabase.Query(
        "SELECT tab_index, name, icon, text FROM mod_ascension_bank_tab "
        "WHERE owner_kind = {} AND owner_id = {} ORDER BY tab_index ASC",
        bank.OwnerKind, bank.OwnerId);
    if (tabs)
    {
        do
        {
            Field* fields = tabs->Fetch();
            uint8 const index = fields[0].Get<uint8>();
            if (index >= BANK_TABS)
            {
                LOG_ERROR("module.ascension_compat",
                          "Personal bank tab {} out of range for owner kind {} id {}",
                          index, bank.OwnerKind, bank.OwnerId);
                continue;
            }

            bank.TabName[index] = fields[1].Get<std::string>();
            bank.TabIcon[index] = fields[2].Get<std::string>();
            bank.TabText[index] = fields[3].Get<std::string>();
            bank.Tabs = std::max<uint8>(bank.Tabs, index + 1);
        } while (tabs->NextRow());
    }

    QueryResult money = CharacterDatabase.Query(
        "SELECT money FROM mod_ascension_bank_money WHERE owner_kind = {} AND owner_id = {}",
        bank.OwnerKind, bank.OwnerId);
    bank.Money = money ? money->Fetch()[0].Get<uint64>() : 0;

    // Same field order the core uses to fill guild bank tabs, so the item is reconstructed
    // exactly as the character left it.
    QueryResult items = CharacterDatabase.Query(
        "SELECT creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, "
        "randomPropertyId, durability, playedTime, text, bi.tab_index, bi.slot, bi.item_guid, itemEntry "
        "FROM mod_ascension_bank_item bi INNER JOIN item_instance ii ON bi.item_guid = ii.guid "
        "WHERE bi.owner_kind = {} AND bi.owner_id = {}",
        bank.OwnerKind, bank.OwnerId);
    if (items)
    {
        do
        {
            Field* fields = items->Fetch();
            uint8 const tab = fields[11].Get<uint8>();
            uint8 const slot = fields[12].Get<uint8>();
            ObjectGuid::LowType const itemGuid = fields[13].Get<uint32>();
            uint32 const itemEntry = fields[14].Get<uint32>();

            if (tab >= BANK_TABS || slot >= BANK_SLOTS)
            {
                LOG_ERROR("module.ascension_compat",
                          "Personal bank item {} sits in an invalid slot (tab {} slot {})",
                          itemGuid, tab, slot);
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
            if (!proto)
            {
                LOG_ERROR("module.ascension_compat",
                          "Personal bank item {} has unknown template {}", itemGuid, itemEntry);
                continue;
            }

            Item* item = NewItemOrBag(proto);
            if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, fields, itemEntry))
            {
                LOG_ERROR("module.ascension_compat",
                          "Personal bank item {} could not be loaded", itemGuid);
                delete item;
                continue;
            }

            item->AddToWorld();
            bank.Items[tab][slot] = item;
        } while (items->NextRow());
    }
}

void UnloadBank(OpenBank& bank)
{
    for (auto& tab : bank.Items)
    {
        for (Item*& item : tab)
        {
            if (!item)
                continue;

            item->RemoveFromWorld();
            delete item;
            item = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Persisting
// ---------------------------------------------------------------------------------------

/// Points a bank slot at an item (or at nothing) inside `trans`.
void StoreSlot(CharacterDatabaseTransaction trans, OpenBank const& bank, uint8 tab, uint8 slot, Item* item)
{
    trans->Append("DELETE FROM mod_ascension_bank_item WHERE owner_kind = {} AND owner_id = {} "
                  "AND tab_index = {} AND slot = {}",
                  bank.OwnerKind, bank.OwnerId, tab, slot);

    if (!item)
        return;

    trans->Append("INSERT INTO mod_ascension_bank_item (owner_kind, owner_id, tab_index, slot, item_guid) "
                  "VALUES ({}, {}, {}, {}, {})",
                  bank.OwnerKind, bank.OwnerId, tab, slot, item->GetGUID().GetCounter());

    // Standalone save, exactly like a guild bank item: no owner, no container.
    item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
    item->SetGuidValue(ITEM_FIELD_OWNER, ObjectGuid::Empty);
    item->FSetState(ITEM_NEW);
    item->SaveToDB(trans);
}

/// Writes the drawer's gold inside `trans`.
///
/// It takes a transaction because the character's own gold has to be written by the same
/// commit: `ModifyMoney` only moves PLAYER_FIELD_COINAGE in memory, and characters.money is
/// not touched until the periodic save. Writing the drawer on its own left a window - fifteen
/// minutes wide by default - in which a crash duplicated the deposited gold (or destroyed the
/// withdrawn gold), which is what the six item paths already avoid by saving both ends in one
/// transaction.
void StoreMoney(CharacterDatabaseTransaction trans, OpenBank const& bank)
{
    trans->Append("REPLACE INTO mod_ascension_bank_money (owner_kind, owner_id, money) "
                  "VALUES ({}, {}, {})",
                  bank.OwnerKind, bank.OwnerId, bank.Money);
}

void StoreTab(CharacterDatabaseTransaction trans, OpenBank const& bank, uint8 tab)
{
    std::string name = bank.TabName[tab];
    std::string icon = bank.TabIcon[tab];
    std::string text = bank.TabText[tab];
    CharacterDatabase.EscapeString(name);
    CharacterDatabase.EscapeString(icon);
    CharacterDatabase.EscapeString(text);

    trans->Append("REPLACE INTO mod_ascension_bank_tab (owner_kind, owner_id, tab_index, name, icon, text) "
                  "VALUES ({}, {}, {}, '{}', '{}', '{}')",
                  bank.OwnerKind, bank.OwnerId, tab, name, icon, text);
}

/// The same write on its own, for the paths where no gold moves with it (renaming a tab, and
/// the voucher, which costs no gold).
void StoreTab(OpenBank const& bank, uint8 tab)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    StoreTab(trans, bank, tab);
    CharacterDatabase.CommitTransaction(trans);
}

/// Logs one bank event the way the core logs guild-bank events, so the frame's log tab
/// reads the same. Item events are filed under their tab, money under the money tab, and
/// a move inside one tab is not logged at all (again as the core does).
void LogBankEvent(OpenBank const& bank, uint8 eventType, uint8 tab, Player* player, uint32 itemOrMoney,
                  uint16 count, uint8 destTab = 0)
{
    if (tab >= BANK_TABS)
        return;

    if (eventType == GUILD_BANK_LOG_MOVE_ITEM && tab == destTab)
        return;

    bool const moneyEvent = eventType == GUILD_BANK_LOG_DEPOSIT_MONEY ||
                            eventType == GUILD_BANK_LOG_WITHDRAW_MONEY;

    CharacterDatabase.Execute(
        "INSERT INTO mod_ascension_bank_log (owner_kind, owner_id, tab_index, event_type, player_guid, "
        "item_or_money, stack_count, dest_tab, timestamp) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {})",
        bank.OwnerKind, bank.OwnerId, moneyEvent ? uint32(GUILD_BANK_MONEY_LOGS_TAB) : uint32(tab), eventType,
        player->GetGUID().GetCounter(), itemOrMoney, count, destTab,
        uint32(GameTime::GetGameTime().count()));

    // The frame shows the last 25 entries per tab, so nothing older needs keeping.
    CharacterDatabase.Execute(
        "DELETE FROM mod_ascension_bank_log WHERE owner_kind = {} AND owner_id = {} AND tab_index = {} AND log_id NOT IN "
        "(SELECT log_id FROM (SELECT log_id FROM mod_ascension_bank_log WHERE owner_kind = {} AND owner_id = {} "
        "AND tab_index = {} ORDER BY log_id DESC LIMIT 25) keep)",
        bank.OwnerKind, bank.OwnerId, moneyEvent ? uint32(GUILD_BANK_MONEY_LOGS_TAB) : uint32(tab),
        bank.OwnerKind, bank.OwnerId, moneyEvent ? uint32(GUILD_BANK_MONEY_LOGS_TAB) : uint32(tab));
}

// ---------------------------------------------------------------------------------------
// Talking to the client
// ---------------------------------------------------------------------------------------

/// The stock guild-bank permissions packet, which is where the frame takes the number of
/// tabs it believes the character owns from. Without it the frame answers "your guild has
/// not purchased any guild bank space" - which is also the guild wording we must never show.
void SendRights(Player* player, uint8 tabs)
{
    WorldPackets::Guild::GuildPermissionsQueryResults rights;
    rights.RankID = 0;
    rights.Flags = GR_RIGHT_ALL;
    rights.WithdrawGoldLimit = int32(BANK_SLOTS * BANK_TABS);
    rights.NumTabs = int8(std::min<uint8>(tabs, BANK_TABS));
    for (uint8 tab = 0; tab < BANK_TABS; ++tab)
    {
        rights.Tab[tab].Flags = GUILD_BANK_RIGHT_FULL;
        rights.Tab[tab].WithdrawItemLimit = BANK_SLOTS;
    }

    player->GetSession()->SendPacket(rights.Write());
}

void SendTabList(Player* player, OpenBank const& bank, uint8 tab, bool sendAllSlots)
{
    WorldPackets::Guild::GuildBankQueryResults packet;
    packet.Money = bank.Money;
    packet.Tab = tab;
    packet.FullUpdate = sendAllSlots;
    packet.WithdrawalsRemaining = BANK_SLOTS;   // a personal bank withdraws freely

    if (sendAllSlots && tab == 0)
    {
        packet.TabInfo.reserve(bank.Tabs);
        for (uint8 i = 0; i < bank.Tabs; ++i)
        {
            WorldPackets::Guild::GuildBankTabInfo info;
            info.Name = bank.TabName[i];
            info.Icon = bank.TabIcon[i].empty() ? DEFAULT_TAB_ICON : bank.TabIcon[i];
            packet.TabInfo.push_back(info);
        }
    }

    if (tab >= bank.Tabs)
    {
        player->GetSession()->SendPacket(packet.Write());
        return;
    }

    for (uint8 slot = 0; slot < BANK_SLOTS; ++slot)
    {
        Item* item = bank.Items[tab][slot];
        if (!item && sendAllSlots)
            continue;   // a full tab listing carries the occupied slots only

        WorldPackets::Guild::GuildBankItemInfo info;
        info.Slot = slot;

        if (item)
        {
            info.ItemID = item->GetEntry();
            info.Count = int32(item->GetCount());
            info.Charges = int32(std::abs(item->GetSpellCharges()));
            info.EnchantmentID = int32(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT));
            info.Flags = item->GetInt32Value(ITEM_FIELD_FLAGS);
            info.RandomPropertiesID = item->GetItemRandomPropertyId();
            info.RandomPropertiesSeed = int32(item->GetItemSuffixFactor());

            for (uint32 socketSlot = 0; socketSlot < MAX_GEM_SOCKETS; ++socketSlot)
            {
                uint32 const enchantId = item->GetEnchantmentId(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socketSlot));
                if (!enchantId)
                    continue;

                WorldPackets::Guild::GuildBankSocketEnchant gem;
                gem.SocketIndex = socketSlot;
                gem.SocketEnchantID = int32(enchantId);
                info.SocketEnchant.push_back(gem);
            }
        }

        packet.ItemInfo.push_back(info);
    }

    player->GetSession()->SendPacket(packet.Write());
}

/// Refreshes a tab after a change: every slot, so moved-away slots are cleared on screen too.
void SendTabChanged(Player* player, OpenBank const& bank, uint8 tab)
{
    SendTabList(player, bank, tab, SEND_CHANGED_SLOTS);
}

/// Everything the frame needs for the bank it is showing, in the order it needs it.
///
/// The order is the whole point, and the client's own guild-bank frame decides it. Its tab strip
/// is only redrawn inside `GuildBankFrame_UpdateTabs()`, and the one event this side of the wire
/// can raise it with is the bank-kind payload: the frame's hook on that payload re-selects its
/// first panel tab, and `GuildBankFrameTab_OnClick` ends in `GuildBankFrame_UpdateTabs()`. That
/// function paints each button from the data the client holds *at that instant* - the buy tab is
/// the button at `GetNumGuildBankTabs()+1` and gets the UI-GuildBankFrame-NewTab "+" texture, a
/// real tab gets the icon from its own tab info - and `SetTexture` with nothing leaves the
/// previous texture in place. So a repaint that runs before the tab data lands paints the buy tab
/// where a real tab belongs, and nothing afterwards puts it right: a tab list and a permissions
/// packet do not repaint by themselves.
///
/// Hence the shape below: the kind payload first (so the frame already presents itself as the
/// personal or realm bank while the data loads), then the count, then the data - sent twice - and
/// finally the kind payload again, which by then is a repaint that sees the current count and the
/// current tabs, so no button keeps a "+" it was given while the other bank was on screen.
///
/// The data is sent twice on purpose, and the second copy is the one that matters. A tab list does
/// two things at once: it is what raises the number of tabs the client believes it owns, and it is
/// the only carrier of the tab names and icons - and the client keeps those only for the tabs it
/// already owned when the list was parsed. So a single list sent to a client that is still holding
/// the *previous* bank's count (2 tabs, from the realm bank, say) raises the count to 3 while
/// keeping just the first two entries; the third tab stays nameless and iconless, and the strip
/// paints it exactly like the buy tab's "+" it was showing. The second copy is parsed with the
/// count already correct, so all three entries land. Reopening the window does nothing for this
/// (the count is already right and the list is still the same list) - only a relog rebuilt the
/// frame, which is why that was the one thing that appeared to fix it.
void SendBankData(Player* player, OpenBank const& bank, uint8 kind, bool fullSlots)
{
    bool const allSlots = fullSlots ? SEND_ALL_SLOTS : SEND_CHANGED_SLOTS;

    AscensionPersonalBank::SendKindHint(player, kind);
    SendRights(player, bank.Tabs);
    SendTabList(player, bank, 0, allSlots);          // raises the count
    SendTabList(player, bank, 0, allSlots);          // stores the names and icons under it
    SendTabChanged(player, bank, 0);
    AscensionPersonalBank::SendKindHint(player, kind);   // repaint with everything current
}

// ---------------------------------------------------------------------------------------
// Item movement
//
// The two ends of a move are a character's bags and one bank slot. Every case the client can
// ask for (put in, take out, split a stack, shift-click into the first fitting slot, drag
// bank slot onto bank slot) is handled here, mirroring the core's own guild-bank rules - minus
// the rank checks a personal bank does not have, and minus its soulbound restriction, which
// is exactly what the Personal Bank tooltip promises: "You can put soulbound items into bank".
// ---------------------------------------------------------------------------------------

/// The stack is gone for good: its item_instance row goes with it.
void DestroyBankItem(CharacterDatabaseTransaction trans, OpenBank& bank, uint8 tab, uint8 slot, Item* item)
{
    bank.Items[tab][slot] = nullptr;
    StoreSlot(trans, bank, tab, slot, nullptr);
    item->FSetState(ITEM_REMOVED);
    item->SaveToDB(trans);
    item->RemoveFromWorld();
    delete item;
}

[[nodiscard]] uint32 ItemCount(OpenBank const& bank)
{
    uint32 count = 0;
    for (auto const& tab : bank.Items)
        for (Item* item : tab)
            if (item)
                ++count;

    return count;
}

[[nodiscard]] Item* BankItem(OpenBank const& bank, uint8 tab, uint8 slot)
{
    if (tab >= bank.Tabs || slot >= BANK_SLOTS)
        return nullptr;

    return bank.Items[tab][slot];
}

/// The first bank slot that can take `item` as-is: a stack with room, else an empty slot.
///
/// `amount` is how much is really going to move. Zero means "whatever fits" - the caller will
/// clip the merge and keep the rest - while a split says how much has to fit *whole*, because
/// a split has already decided how much leaves the source and cannot be clipped.
[[nodiscard]] bool FindBankSlotFor(OpenBank const& bank, uint8 tab, Item* item, uint32 amount, uint8& outSlot)
{
    if (tab >= bank.Tabs)
        return false;

    uint32 const maxStack = item->GetMaxStackCount();
    for (uint8 slot = 0; slot < BANK_SLOTS; ++slot)
    {
        Item* existing = bank.Items[tab][slot];
        if (existing && existing->GetEntry() == item->GetEntry() && existing->GetCount() < maxStack &&
            (!amount || amount <= maxStack - existing->GetCount()))
        {
            outSlot = slot;
            return true;
        }
    }

    for (uint8 slot = 0; slot < BANK_SLOTS; ++slot)
    {
        if (!bank.Items[tab][slot])
        {
            outSlot = slot;
            return true;
        }
    }

    return false;
}

/// True when `dest` and `item` are the same thing and `dest` still has stack room.
///
/// With `amount`, that much has to fit whole. The callers that pass zero go on to clip the
/// merge to the room they find and keep the remainder where it was; the split callers cannot
/// do that - they have already decided how much leaves the source - so for them an overflowing
/// merge has to be refused, not clipped. Without that test a split of 15 onto a stack of 19
/// out of 20 wrote a count of 34 straight into item_instance.count, a state the core never
/// produces and which then blocks every later merge into that slot.
[[nodiscard]] bool CanMergeInto(Item* dest, Item* item, uint32 amount = 0)
{
    if (!dest || !item || dest->GetEntry() != item->GetEntry())
        return false;

    uint32 const maxStack = dest->GetMaxStackCount();
    if (dest->GetCount() >= maxStack)
        return false;

    return !amount || amount <= maxStack - dest->GetCount();
}

/// Takes `amount` off a bank stack and persists the new count.
void ReduceBankItem(CharacterDatabaseTransaction trans, OpenBank& bank, uint8 tab, uint8 slot, Item* item, uint32 amount)
{
    if (amount >= item->GetCount())
    {
        DestroyBankItem(trans, bank, tab, slot, item);
        return;
    }

    item->SetCount(item->GetCount() - amount);
    item->FSetState(ITEM_CHANGED);
    item->SaveToDB(trans);
}

/// Puts an item into a bank slot. Items arriving from a character's bag have just been
/// taken out of the world by MoveItemFromInventory, so they need putting back in.
void StoreInBank(CharacterDatabaseTransaction trans, OpenBank& bank, uint8 tab, uint8 slot, Item* item)
{
    bank.Items[tab][slot] = item;
    if (!item->IsInWorld())
        item->AddToWorld();

    StoreSlot(trans, bank, tab, slot, item);
}

/// Removes an item from the character's bags: the inventory row goes with it.
void RemoveFromInventory(CharacterDatabaseTransaction trans, Player* player, uint8 bag, uint8 slot, Item* item)
{
    player->MoveItemFromInventory(bag, slot, true);
    item->DeleteFromInventoryDB(trans);
    player->SaveInventoryAndGoldToDB(trans);
}

void MoveIntoInventory(CharacterDatabaseTransaction trans, Player* player, ItemPosCountVec const& dest, Item* item)
{
    player->MoveItemToInventory(dest, item, true);
    player->SaveInventoryAndGoldToDB(trans);
}

/// Character -> bank, either into a named slot or (shift-click) into the first slot that fits.
void DepositToBank(Player* player, OpenBank& bank, uint8 bag, uint8 slot, uint8 tab, uint8 bankSlot,
                   uint32 split, bool autoStore)
{
    Item* source = player->GetItemByPos(bag, slot);
    if (!source)
        return;

    uint32 const sourceEntry = source->GetEntry();
    uint32 const sourceCount = source->GetCount();

    if (source->IsNotEmptyBag())
    {
        player->SendEquipError(EQUIP_ERR_CAN_ONLY_DO_WITH_EMPTY_BAGS, source, nullptr);
        return;
    }

    if (tab >= bank.Tabs)
        return;

    // A split that would not actually split the stack is a plain move. This is settled before
    // a slot is picked, because the slot has to have room for what really moves.
    if (split == 0 || split >= source->GetCount())
        split = 0;

    if (autoStore && !FindBankSlotFor(bank, tab, source, split, bankSlot))
    {
        player->SendEquipError(EQUIP_ERR_BANK_FULL, source, nullptr);

        // Only for a split, and only because a split is what this diff made refusable here:
        // the frame has drawn the fraction landing in the tab, and the core redraws it in the
        // same case (Guild.cpp:2777). A whole-stack refusal is left exactly as it was.
        if (split)
            SendTabChanged(player, bank, tab);
        return;
    }

    if (bankSlot >= BANK_SLOTS)
        return;

    Item* dest = bank.Items[tab][bankSlot];
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    if (split)
    {
        // Deposit part of a stack: the bank gets a fresh item, the bag keeps the rest.
        Item* moved = source->CloneItem(split, player);
        if (!moved)
        {
            CharacterDatabase.CommitTransaction(trans);
            player->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, source, nullptr);
            return;
        }

        if (dest)
        {
            if (CanMergeInto(dest, moved, moved->GetCount()))
            {
                dest->SetCount(dest->GetCount() + moved->GetCount());
                dest->FSetState(ITEM_CHANGED);
                dest->SaveToDB(trans);
                delete moved;   // the clone only existed to carry the count
            }
            else
            {
                CharacterDatabase.CommitTransaction(trans);
                delete moved;
                player->SendEquipError(EQUIP_ERR_BANK_FULL, source, nullptr);

                // Same reason as the refused move inside the bank: the frame has drawn the
                // fraction arriving in this slot and only a tab update takes it back. The core
                // does it the same way - Guild::_MoveItems sends its step 7
                // _SendBankContentUpdate (Guild.cpp:2777) even when the split move at 5.2 was
                // refused by CanStore.
                SendTabChanged(player, bank, tab);
                return;
            }
        }
        else
        {
            StoreInBank(trans, bank, tab, bankSlot, moved);
        }

        source->SetCount(source->GetCount() - split);
        source->SetState(ITEM_CHANGED, player);
        player->SaveInventoryAndGoldToDB(trans);
    }
    else if (dest)
    {
        if (CanMergeInto(dest, source))
        {
            uint32 const room = dest->GetMaxStackCount() - dest->GetCount();
            uint32 const amount = std::min<uint32>(room, source->GetCount());

            dest->SetCount(dest->GetCount() + amount);
            dest->FSetState(ITEM_CHANGED);
            dest->SaveToDB(trans);

            if (amount >= source->GetCount())
                RemoveFromInventory(trans, player, bag, slot, source);
            else
            {
                source->SetCount(source->GetCount() - amount);
                source->SetState(ITEM_CHANGED, player);
                player->SaveInventoryAndGoldToDB(trans);
            }
        }
        else
        {
            // A swap. The bank has to be able to give its item up and the character has to
            // be able to hold it.
            ItemPosCountVec destPos;
            InventoryResult msg = player->CanStoreItem(bag, slot, destPos, dest, true);
            if (msg != EQUIP_ERR_OK)
            {
                CharacterDatabase.CommitTransaction(trans);
                player->SendEquipError(msg, dest, nullptr);
                return;
            }

            // The bank hands its item over and takes the character's in exchange.
            bank.Items[tab][bankSlot] = nullptr;
            StoreSlot(trans, bank, tab, bankSlot, nullptr);
            dest->FSetState(ITEM_CHANGED);
            dest->SaveToDB(trans);      // keeps the travelling item's own row alive

            // The item the bank gave up is withdrawn, the character's is deposited.
            LogBankEvent(bank, GUILD_BANK_LOG_WITHDRAW_ITEM, tab, player, dest->GetEntry(),
                         uint16(dest->GetCount()));

            RemoveFromInventory(trans, player, bag, slot, source);
            StoreInBank(trans, bank, tab, bankSlot, source);
            MoveIntoInventory(trans, player, destPos, dest);
        }
    }
    else
    {
        RemoveFromInventory(trans, player, bag, slot, source);
        StoreInBank(trans, bank, tab, bankSlot, source);
    }

    CharacterDatabase.CommitTransaction(trans);
    SendTabChanged(player, bank, tab);
    LogBankEvent(bank, GUILD_BANK_LOG_DEPOSIT_ITEM, tab, player, sourceEntry,
                 uint16(split ? split : sourceCount));
}

/// Bank -> character, either into a named slot or (shift-click) wherever it fits.
void WithdrawToPlayer(Player* player, OpenBank& bank, uint8 tab, uint8 bankSlot, uint8 bag, uint8 slot,
                      uint32 split, bool autoStore)
{
    Item* source = BankItem(bank, tab, bankSlot);
    if (!source)
        return;

    uint32 const sourceEntry = source->GetEntry();
    uint32 const sourceCount = source->GetCount();

    if (split == 0 || split >= source->GetCount())
        split = 0;

    // The character side of the move: a named destination, or the first bag with room.
    //
    // A split is planned further down instead, against the clone that really leaves the bank:
    // planning the whole stack here both refused withdrawals that would have fitted (room for
    // one out of a stack of twenty) and, worse, handed that plan to a clone of one.
    ItemPosCountVec destPos;
    if (!split)
    {
        InventoryResult msg = player->CanStoreItem(bag, slot, destPos, source, false);
        if (autoStore || msg != EQUIP_ERR_OK)
        {
            destPos.clear();
            msg = player->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, source, false);
        }

        if (msg != EQUIP_ERR_OK)
        {
            player->SendEquipError(msg, source, nullptr);
            return;
        }
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Every successful path ends the same way: commit, refresh the tab, file the log entry.
    uint32 const movedAmount = split ? split : sourceCount;
    auto finish = [&]()
    {
        CharacterDatabase.CommitTransaction(trans);
        SendTabChanged(player, bank, tab);
        LogBankEvent(bank, GUILD_BANK_LOG_WITHDRAW_ITEM, tab, player, sourceEntry, uint16(movedAmount));
    };

    if (split)
    {
        Item* moved = source->CloneItem(split, player);
        if (!moved)
        {
            CharacterDatabase.CommitTransaction(trans);
            player->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, source, nullptr);
            return;
        }

        // The storage plan above was drawn for the WHOLE stack, and the plan is what decides
        // the counts: Player::MoveItemToInventory hands each entry's count to _StoreItem,
        // which does `pItem->SetCount(count)` on a free slot and `pItem2->SetCount(+count)` on
        // a merge. Reusing it for a clone of `split` therefore handed the character the whole
        // stack's count while the bank was debited `split` alone - taking 1 out of 20 returned
        // 20 and left 19, repeatable at will. The split needs its own plan, drawn for the clone.
        destPos.clear();
        InventoryResult splitMsg = player->CanStoreItem(bag, slot, destPos, moved, false);
        if (autoStore || splitMsg != EQUIP_ERR_OK)
        {
            destPos.clear();
            splitMsg = player->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, moved, false);
        }

        if (splitMsg != EQUIP_ERR_OK)
        {
            CharacterDatabase.CommitTransaction(trans);
            delete moved;   // nothing was taken from the bank yet: the clone just goes away
            player->SendEquipError(splitMsg, source, nullptr);

            // The frame has drawn the fraction leaving this slot; nothing left it, so the tab
            // is resent (the core's step 7, Guild.cpp:2777, for the same refused split).
            SendTabChanged(player, bank, tab);
            return;
        }

        ReduceBankItem(trans, bank, tab, bankSlot, source, split);
        MoveIntoInventory(trans, player, destPos, moved);
        finish();
        return;
    }

    // A named slot that already holds something of the same kind takes the stack in.
    if (!autoStore && bag != NULL_BAG && slot != NULL_SLOT)
    {
        if (Item* destItem = player->GetItemByPos(bag, slot))
        {
            if (CanMergeInto(destItem, source))
            {
                uint32 const room = destItem->GetMaxStackCount() - destItem->GetCount();
                uint32 const amount = std::min<uint32>(room, source->GetCount());

                destItem->SetCount(destItem->GetCount() + amount);
                destItem->SetState(ITEM_CHANGED, player);
                ReduceBankItem(trans, bank, tab, bankSlot, source, amount);
                player->SaveInventoryAndGoldToDB(trans);
                finish();
                return;
            }

            // Otherwise it is a trade: the character's item takes the bank slot.
            if (destItem->IsNotEmptyBag())
            {
                CharacterDatabase.CommitTransaction(trans);
                player->SendEquipError(EQUIP_ERR_CAN_ONLY_DO_WITH_EMPTY_BAGS, destItem, nullptr);
                return;
            }

            ItemPosCountVec swapPos;
            InventoryResult const swapMsg = player->CanStoreItem(bag, slot, swapPos, destItem, true);
            if (swapMsg != EQUIP_ERR_OK)
            {
                CharacterDatabase.CommitTransaction(trans);
                player->SendEquipError(swapMsg, destItem, nullptr);
                return;
            }

            LogBankEvent(bank, GUILD_BANK_LOG_DEPOSIT_ITEM, tab, player, destItem->GetEntry(),
                         uint16(destItem->GetCount()));

            RemoveFromInventory(trans, player, bag, slot, destItem);
            StoreInBank(trans, bank, tab, bankSlot, destItem);   // takes the slot over
            MoveIntoInventory(trans, player, destPos, source);
            finish();
            return;
        }
    }

    // A plain withdrawal: the slot is emptied and the item moves to the character.
    bank.Items[tab][bankSlot] = nullptr;
    StoreSlot(trans, bank, tab, bankSlot, nullptr);
    MoveIntoInventory(trans, player, destPos, source);
    finish();
}

/// Bank -> bank, within a tab or across tabs.
void MoveWithinBank(Player* player, OpenBank& bank, uint8 tab, uint8 slot, uint8 destTab, uint8 destSlot,
                    uint32 split)
{
    Item* source = BankItem(bank, tab, slot);
    if (!source || destTab >= bank.Tabs || destSlot >= BANK_SLOTS || (tab == destTab && slot == destSlot))
        return;

    uint32 const sourceEntry = source->GetEntry();
    uint32 const sourceCount = source->GetCount();

    if (split == 0 || split >= source->GetCount())
        split = 0;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    Item* dest = bank.Items[destTab][destSlot];

    if (split)
    {
        if (dest && !CanMergeInto(dest, source, split))
        {
            CharacterDatabase.CommitTransaction(trans);

            // The client has already drawn the move it asked for; nothing moved here, so the
            // tabs are resent to put its view back where the server actually stands.
            SendTabChanged(player, bank, tab);
            if (destTab != tab)
                SendTabChanged(player, bank, destTab);
            return;
        }

        if (dest)
        {
            dest->SetCount(dest->GetCount() + split);
            dest->FSetState(ITEM_CHANGED);
            dest->SaveToDB(trans);
        }
        else
        {
            Item* moved = source->CloneItem(split, player);
            if (!moved)
            {
                CharacterDatabase.CommitTransaction(trans);
                return;
            }

            StoreInBank(trans, bank, destTab, destSlot, moved);
        }

        ReduceBankItem(trans, bank, tab, slot, source, split);
    }
    else if (dest && CanMergeInto(dest, source))
    {
        uint32 const room = dest->GetMaxStackCount() - dest->GetCount();
        uint32 const amount = std::min<uint32>(room, source->GetCount());

        dest->SetCount(dest->GetCount() + amount);
        dest->FSetState(ITEM_CHANGED);
        dest->SaveToDB(trans);
        ReduceBankItem(trans, bank, tab, slot, source, amount);
    }
    else
    {
        // Plain move, or a swap with whatever sits in the destination slot.
        bank.Items[tab][slot] = dest;
        bank.Items[destTab][destSlot] = source;
        StoreSlot(trans, bank, tab, slot, dest);
        StoreSlot(trans, bank, destTab, destSlot, source);
    }

    CharacterDatabase.CommitTransaction(trans);
    SendTabChanged(player, bank, tab);
    if (destTab != tab)
        SendTabChanged(player, bank, destTab);

    LogBankEvent(bank, GUILD_BANK_LOG_MOVE_ITEM, tab, player, sourceEntry,
                 uint16(split ? split : sourceCount), destTab);
}

// ---------------------------------------------------------------------------------------
// Opcode handling
// ---------------------------------------------------------------------------------------

[[nodiscard]] uint32 TabPrice(uint8 tab)
{
    switch (tab)
    {
        case 0: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_0);
        case 1: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_1);
        case 2: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_2);
        case 3: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_3);
        case 4: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_4);
        case 5: return sWorld->getIntConfig(CONFIG_GUILD_BANK_TAB_COST_5);
        default: return 0;
    }
}

/// Reads one of the bank packets. The read happens on a copy and inside a try/catch: a
/// truncated or mismatched packet must never throw out of the network thread, which is
/// exactly how an empty buffer once took the whole world down.
template <typename T, typename Use>
void WithBankPacket(WorldPacket const& packet, Use&& use)
{
    try
    {
        WorldPacket copy(packet);
        T parsed(std::move(copy));
        parsed.Read();
        use(parsed);
    }
    catch (...)
    {
        LOG_DEBUG("module.ascension_compat", "Ignored a malformed bank packet (opcode {})",
                  packet.GetOpcode());
    }
}

void HandleQueryTab(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankQueryTab>(packet,
        [&](WorldPackets::Guild::GuildBankQueryTab& query)
        {
            if (query.Banker != bank.Vault)
                return;

            SendTabList(player, bank, uint8(query.Tab), query.FullUpdate);
        });
}

void HandleSwapItems(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankSwapItems>(packet,
        [&](WorldPackets::Guild::GuildBankSwapItems& swap)
        {
            if (swap.Banker != bank.Vault)
                return;

            if (swap.BankOnly)
            {
                MoveWithinBank(player, bank, uint8(swap.BankTab1), uint8(swap.BankSlot1),
                               uint8(swap.BankTab), uint8(swap.BankSlot),
                               uint32(std::max<int32>(0, swap.BankItemCount)));
                return;
            }

            // ToSlot decides the direction: 1 moves the bank item into the character, 0 the
            // other way (this is the core's own reading of the field).
            if (swap.ToSlot)
                WithdrawToPlayer(player, bank, uint8(swap.BankTab), uint8(swap.BankSlot),
                                 swap.ContainerSlot, swap.ContainerItemSlot,
                                 uint32(std::max<int32>(0, swap.StackCount)), swap.AutoStore);
            else
                DepositToBank(player, bank, swap.ContainerSlot, swap.ContainerItemSlot,
                              uint8(swap.BankTab), uint8(swap.BankSlot),
                              swap.AutoStore ? 0 : uint32(std::max<int32>(0, swap.StackCount)),
                              swap.AutoStore);
        });
}

void HandleDepositMoney(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankDepositMoney>(packet,
        [&](WorldPackets::Guild::GuildBankDepositMoney& deposit)
        {
            // Capped before anything else, like the withdrawal below: Money is a uint32 off
            // the wire, and `-int32(x)` of a value above 2^31 comes out POSITIVE, which would
            // have paid the character for depositing. HasEnoughMoney alone does not rule that
            // out - characters.money is an unsigned column, so a value above the gold cap can
            // reach memory without ModifyMoney ever having allowed it.
            uint32 const amount = uint32(std::min<uint64>(deposit.Money, uint64(MAX_MONEY_AMOUNT)));
            if (deposit.Banker != bank.Vault || !amount || !player->HasEnoughMoney(amount))
                return;

            if (!player->ModifyMoney(-int32(amount)))
                return;

            bank.Money += amount;

            // One commit for both ends. The character's gold is only in memory until this
            // SaveGoldToDB, so writing the drawer alone duplicated the deposit at any crash.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            StoreMoney(trans, bank);
            player->SaveGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            SendTabChanged(player, bank, 0);
            LogBankEvent(bank, GUILD_BANK_LOG_DEPOSIT_MONEY, 0, player, amount, 0);
        });
}

void HandleWithdrawMoney(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankWithdrawMoney>(packet,
        [&](WorldPackets::Guild::GuildBankWithdrawMoney& withdraw)
        {
            // Money is a uint32 off the wire: without the cap, a value above MAX_MONEY_AMOUNT
            // makes int32() negative and ModifyMoney *takes* gold instead of giving it. The
            // core's guild bank caps the same way (Guild::HandleMemberWithdrawMoney).
            uint32 const amount = uint32(std::min<uint64>(withdraw.Money, uint64(MAX_MONEY_AMOUNT)));
            if (withdraw.Banker != bank.Vault || !amount || bank.Money < amount)
                return;

            // ModifyMoney refuses - and credits nothing - when the character would go over the
            // gold cap or a trial account's cap. Debiting the drawer first destroyed the gold
            // silently, so the credit has to succeed before anything leaves the bank.
            if (!player->ModifyMoney(int32(amount)))
            {
                LOG_ERROR("module.ascension_compat",
                          "Personal bank: {} could not be credited {} copper on withdrawal "
                          "(kind {} id {}); the drawer keeps it",
                          player->GetName(), amount, bank.OwnerKind, bank.OwnerId);
                return;
            }

            bank.Money -= amount;

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            StoreMoney(trans, bank);
            player->SaveGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            SendTabChanged(player, bank, 0);
            LogBankEvent(bank, GUILD_BANK_LOG_WITHDRAW_MONEY, 0, player, amount, 0);
        });
}

void HandleBuyTab(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankBuyTab>(packet,
        [&](WorldPackets::Guild::GuildBankBuyTab& buy)
        {
            if (buy.Banker != bank.Vault || bank.Tabs >= BANK_TABS || uint8(buy.BankTab) != bank.Tabs)
                return;

            // Same cap as the two money handlers, for the same reason: the price comes from
            // configuration (CONFIG_GUILD_BANK_TAB_COST_*) as a uint32, and a value above 2^31
            // would make `-int32(price)` positive and pay for the tab instead of charging it.
            uint32 const price = uint32(std::min<uint64>(TabPrice(bank.Tabs), uint64(MAX_MONEY_AMOUNT)));
            if (!price || !player->HasEnoughMoney(price))
                return;

            if (!player->ModifyMoney(-int32(price)))
                return;

            // The tab row and the gold that paid for it, in one commit: persisting the tab on
            // its own gave a free tab to anyone whose character was not saved before a crash.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            StoreTab(trans, bank, bank.Tabs);   // owning an empty tab row is what "purchased" means
            player->SaveGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            ++bank.Tabs;

            SendBankData(player, bank, bank.OwnerKind == OWNER_REALM ? AscensionPersonalBank::REALM
                                                                     : AscensionPersonalBank::PERSONAL,
                         true);

            // Buying a tab is gold leaving the character, so it belongs in the money log.
            LogBankEvent(bank, GUILD_BANK_LOG_WITHDRAW_MONEY, 0, player, price, 0);

            LOG_INFO("module.ascension_compat",
                     "Personal bank: {} bought tab {} ({} copper) as kind {} id {}",
                     player->GetName(), bank.Tabs - 1, price, bank.OwnerKind, bank.OwnerId);
        });
}

void HandleUpdateTab(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankUpdateTab>(packet,
        [&](WorldPackets::Guild::GuildBankUpdateTab& update)
        {
            if (update.Banker != bank.Vault || uint8(update.BankTab) >= bank.Tabs ||
                update.Name.empty() || update.Icon.empty())
                return;

            bank.TabName[uint8(update.BankTab)] = std::string(update.Name);
            bank.TabIcon[uint8(update.BankTab)] = std::string(update.Icon);
            StoreTab(bank, uint8(update.BankTab));
            SendTabList(player, bank, 0, SEND_ALL_SLOTS);
        });
}

void SendTabText(Player* player, OpenBank const& bank, uint8 tab)
{
    WorldPackets::Guild::GuildBankTextQueryResult result;
    result.Tab = tab;
    result.Text = bank.TabText[tab];
    player->GetSession()->SendPacket(result.Write());
}

/// The per-tab note the frame can show; the core stores the same thing in guild_bank_tab.
void HandleTabTextQuery(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankTextQuery>(packet,
        [&](WorldPackets::Guild::GuildBankTextQuery& query)
        {
            if (uint8(query.Tab) >= bank.Tabs)
                return;

            SendTabText(player, bank, uint8(query.Tab));
        });
}

void HandleSetTabText(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankSetTabText>(packet,
        [&](WorldPackets::Guild::GuildBankSetTabText& set)
        {
            if (uint8(set.Tab) >= bank.Tabs)
                return;

            bank.TabText[uint8(set.Tab)] = std::string(set.TabText);
            StoreTab(bank, uint8(set.Tab));

            // The core broadcasts the new note to everyone holding the tab open; only this
            // character can see their own bank, so echoing it back is the whole audience.
            SendTabText(player, bank, uint8(set.Tab));
        });
}

/// The frame's log tab: the 25 most recent entries of one tab, or of the money drawer.
void HandleLogQuery(Player* player, OpenBank& bank, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankLogQuery>(packet,
        [&](WorldPackets::Guild::GuildBankLogQuery& query)
        {
            // The client asks for the money log as tab index GUILD_BANK_MAX_TABS, which is
            // why the core stores money events under its own GUILD_BANK_MONEY_LOGS_TAB.
            bool const moneyLog = uint8(query.Tab) >= BANK_TABS;
            if (!moneyLog && uint8(query.Tab) >= bank.Tabs)
                return;

            uint32 const storedTab = moneyLog ? uint32(GUILD_BANK_MONEY_LOGS_TAB) : uint32(uint8(query.Tab));

            // Newest 25, then oldest first: the order the core hands its own log over in.
            QueryResult rows = CharacterDatabase.Query(
                "SELECT event_type, player_guid, item_or_money, stack_count, dest_tab, timestamp FROM "
                "(SELECT * FROM mod_ascension_bank_log WHERE owner_kind = {} AND owner_id = {} "
                "AND tab_index = {} ORDER BY log_id DESC LIMIT 25) recent ORDER BY log_id ASC",
                bank.OwnerKind, bank.OwnerId, storedTab);

            WorldPackets::Guild::GuildBankLogQueryResults result;
            result.Tab = uint8(query.Tab);

            if (rows)
            {
                do
                {
                    Field* fields = rows->Fetch();
                    uint8 const eventType = fields[0].Get<uint8>();

                    WorldPackets::Guild::GuildBankLogEntry entry;
                    entry.PlayerGUID = ObjectGuid::Create<HighGuid::Player>(fields[1].Get<uint32>());
                    entry.TimeOffset = int32(GameTime::GetGameTime().count()) - int32(fields[5].Get<uint32>());
                    entry.EntryType = int8(eventType);

                    if (eventType == GUILD_BANK_LOG_DEPOSIT_ITEM || eventType == GUILD_BANK_LOG_WITHDRAW_ITEM)
                    {
                        entry.ItemID = int32(fields[2].Get<uint32>());
                        entry.Count = int32(fields[3].Get<uint16>());
                    }
                    else if (eventType == GUILD_BANK_LOG_MOVE_ITEM || eventType == GUILD_BANK_LOG_MOVE_ITEM2)
                    {
                        entry.ItemID = int32(fields[2].Get<uint32>());
                        entry.Count = int32(fields[3].Get<uint16>());
                        entry.OtherTab = int8(fields[4].Get<uint8>());
                    }
                    else
                        entry.Money = fields[2].Get<uint32>();

                    result.Entry.push_back(entry);
                } while (rows->NextRow());
            }

            player->GetSession()->SendPacket(result.Write());
        });
}

/// A personal bank has no weekly gold allowance to track - what the character has is the
/// only limit - so the frame is told there is no cap rather than zero (which reads as
/// "nothing may be withdrawn").
void HandleWithdrawAllowanceQuery(Player* player, OpenBank& /*bank*/, WorldPacket const& packet)
{
    WithBankPacket<WorldPackets::Guild::GuildBankRemainingWithdrawMoneyQuery>(packet,
        [&](WorldPackets::Guild::GuildBankRemainingWithdrawMoneyQuery&)
        {
            WorldPackets::Guild::GuildBankRemainingWithdrawMoney result;
            result.RemainingWithdrawMoney = std::numeric_limits<int32>::max();
            player->GetSession()->SendPacket(result.Write());
        });
}
} // namespace

namespace AscensionPersonalBank
{
void SendKindHint(Player* player, uint8 kind)
{
    // Two flags, read in this order by the client's GetBankPermissions().
    WorldPacket packet(SMSG_BANK_PERMISSIONS, 2);
    packet << uint8(kind == PERSONAL ? 1 : 0);
    packet << uint8(kind == REALM ? 1 : 0);
    player->GetSession()->SendPacket(&packet);
}

bool IsOpen(Player* player)
{
    return player && FindOpenBank(player->GetGUID().GetCounter()) != nullptr;
}

void Opened(Player* player, uint8 kind, ObjectGuid vault)
{
    if (!player)
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

    // Replace any window this character already had open. The old contents are taken out of the
    // table under the lock and released after it: UnloadBank removes objects from the world and
    // deletes them, which must never run while another thread waits to read the table.
    OpenBank previous;
    bool hadPrevious = false;
    {
        std::unique_lock<std::shared_mutex> guard(openBanksLock);
        auto existing = openBanks.find(guid);
        if (existing != openBanks.end())
        {
            previous = std::move(existing->second);
            hadPrevious = true;
            openBanks.erase(existing);
            openBankCount.store(uint32(openBanks.size()), std::memory_order_release);
        }
    }

    if (hadPrevious)
        UnloadBank(previous);

    OpenBank bank;
    bank.OwnerKind = kind == REALM ? OWNER_REALM : OWNER_CHARACTER;
    bank.OwnerId = BankOwnerId(player, kind);
    bank.Vault = vault;
    LoadBank(bank);   // three synchronous queries: deliberately outside the lock

    OpenBank const* stored = nullptr;
    {
        std::unique_lock<std::shared_mutex> guard(openBanksLock);
        stored = &openBanks.emplace(guid, std::move(bank)).first->second;
        openBankCount.store(uint32(openBanks.size()), std::memory_order_release);
    }

    SendBankData(player, *stored, kind, true);

    LOG_INFO("module.ascension_compat",
             "{} bank opened for {} (kind {}, owner {} id {}, {} tabs, {} items, {} copper)",
             kind == REALM ? "Realm" : "Personal", player->GetName(), uint32(kind),
             uint32(stored->OwnerKind), stored->OwnerId, stored->Tabs, ItemCount(*stored), stored->Money);
}

bool HandlePacket(Player* player, WorldPacket const& packet)
{
    if (!player)
        return false;

    // Only the lookup is guarded. Everything below sends packets and talks to the database,
    // and none of that may hold the table's lock; see FindOpenBank on why the pointer stays
    // good once the lock is gone.
    OpenBank* found = FindOpenBank(player->GetGUID().GetCounter());
    if (!found)
        return false;

    OpenBank& bank = *found;

    // The window lives on the summoned object: once it has despawned, or the character has
    // walked away from it, the bank is closed and the opcode goes back to the core.
    //
    // That hand-back is NOT free for a character who is in a real guild: the five vault
    // objects are gameobject_template.type 34 (measured: entries 475001, 475002, 80782,
    // 80159, 80160 are all type 34 = GAMEOBJECT_TYPE_GUILD_BANK), so the banker check at
    // WorldSession::HandleGuildBankSwapItems (GuildHandler.cpp:332) accepts them, and a
    // swap that arrives in the same breath as the despawn is then applied to that
    // character's GUILD bank, at the same tab and slot. Only an in-flight packet can be in
    // that window, and closing it means swallowing these opcodes instead of handing them
    // back - a policy change, deliberately not made here.
    GameObject* vault = ObjectAccessor::GetGameObject(*player, bank.Vault);
    if (!vault || !vault->IsInWorld() || player->GetDistance(vault) > BANK_REACH)
    {
        LOG_INFO("module.ascension_compat", "Personal bank closed for {} (bank object gone or out of reach)",
                 player->GetName());
        Closed(player);
        return false;
    }

    switch (packet.GetOpcode())
    {
        case CMSG_GUILD_BANK_QUERY_TAB:
            HandleQueryTab(player, bank, packet);
            return true;
        case CMSG_GUILD_BANK_SWAP_ITEMS:
            HandleSwapItems(player, bank, packet);
            return true;
        case CMSG_GUILD_BANK_DEPOSIT_MONEY:
            HandleDepositMoney(player, bank, packet);
            return true;
        case CMSG_GUILD_BANK_WITHDRAW_MONEY:
            HandleWithdrawMoney(player, bank, packet);
            return true;
        case CMSG_GUILD_BANK_BUY_TAB:
            HandleBuyTab(player, bank, packet);
            return true;
        case CMSG_GUILD_BANK_UPDATE_TAB:
            HandleUpdateTab(player, bank, packet);
            return true;
        case MSG_QUERY_GUILD_BANK_TEXT:
            HandleTabTextQuery(player, bank, packet);
            return true;
        case CMSG_SET_GUILD_BANK_TEXT:
            HandleSetTabText(player, bank, packet);
            return true;
        case MSG_GUILD_BANK_LOG_QUERY:
            HandleLogQuery(player, bank, packet);
            return true;
        case MSG_GUILD_BANK_MONEY_WITHDRAWN:
            HandleWithdrawAllowanceQuery(player, bank, packet);
            return true;
        default:
            return false;
    }
}

void Closed(Player* player)
{
    if (!player)
        return;

    OpenBank closing;
    {
        std::unique_lock<std::shared_mutex> guard(openBanksLock);
        auto itr = openBanks.find(player->GetGUID().GetCounter());
        if (itr == openBanks.end())
            return;

        closing = std::move(itr->second);
        openBanks.erase(itr);
        openBankCount.store(uint32(openBanks.size()), std::memory_order_release);
    }

    UnloadBank(closing);   // deletes world objects: never under the lock
}

/// The work of AddTab, with the tab row appended to `trans` instead of committed on its own.
///
/// The voucher has to destroy itself in the same commit that writes the tab: DestroyItemCount
/// only takes the item out of memory, and characters/item_instance are not touched until the
/// periodic save, so a crash in between left either a free tab with the voucher still in the
/// bags or a spent voucher with no tab. That is the same class of hole the two gold paths just
/// had, and it closes the same way - one commit for both ends.
bool AddTab(Player* player, uint8 kind, CharacterDatabaseTransaction trans)
{
    if (!player || !trans)
        return false;

    // The bank the voucher names: the character's own for the personal and celestial items, the
    // account's shared one for the realm voucher.
    uint8 const ownerKind = kind == REALM ? OWNER_REALM : OWNER_CHARACTER;
    uint64 const ownerId = BankOwnerId(player, kind);

    // CMSG_USE_ITEM is PROCESS_INPLACE, so this runs on a map thread: the lookup goes through
    // the guarded helper like every other one.
    OpenBank* open = FindOpenBank(player->GetGUID().GetCounter());
    bool const isOpen = open && open->OwnerKind == ownerKind && open->OwnerId == ownerId;

    uint8 tabs = 1;
    if (isOpen)
    {
        tabs = open->Tabs;
    }
    else
    {
        QueryResult rows = CharacterDatabase.Query(
            "SELECT MAX(tab_index) FROM mod_ascension_bank_tab WHERE owner_kind = {} AND owner_id = {}",
            ownerKind, ownerId);
        if (rows)
        {
            Field* field = rows->Fetch();
            if (!field[0].IsNull())
                tabs = std::max<uint8>(tabs, field[0].Get<uint8>() + 1);
        }
    }

    if (tabs >= BANK_TABS)
        return false;

    // The storage is the source of truth, so the tab row is written the same way whether the
    // window happens to be open or not - and it can be a different bank than the open one:
    // using a realm voucher while the personal window is up still buys a realm tab.
    OpenBank owner;
    owner.OwnerKind = ownerKind;
    owner.OwnerId = ownerId;
    StoreTab(trans, owner, tabs);

    if (isOpen)
    {
        // A write to one entry's value, not to the table's shape: nothing rehashes, and only
        // this character's own session ever reads or writes this entry.
        open->Tabs = tabs + 1;
        SendBankData(player, *open, kind, true);
    }

    LOG_INFO("module.ascension_compat", "{} bank: {} unlocked tab {} with a voucher (owner {} id {})",
             kind == REALM ? "Realm" : "Personal", player->GetName(), tabs, uint32(ownerKind), ownerId);
    return true;
}

/// The header's form, for a caller that has nothing of its own to write with the tab. A refusal
/// leaves the transaction unused, which costs nothing: an uncommitted transaction is only a
/// list of queries and its destructor drops them (TransactionBase::Cleanup).
bool AddTab(Player* player, uint8 kind)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (!AddTab(player, kind, trans))
        return false;

    CharacterDatabase.CommitTransaction(trans);
    return true;
}
} // namespace AscensionPersonalBank

namespace
{
/// Ends the loaded bank when the character leaves the world, so a despawned or stale
/// session cannot keep items in memory.
class AscensionPersonalBankPlayerScript : public PlayerScript
{
public:
    AscensionPersonalBankPlayerScript()
        : PlayerScript("AscensionPersonalBankPlayerScript", {PLAYERHOOK_ON_LOGOUT})
    {
    }

    void OnPlayerLogout(Player* player) override
    {
        AscensionPersonalBank::Closed(player);
    }
};

/// "Personal Bank Tab Voucher" and "Realm Bank Tab Voucher": using one buys the next tab of
/// the bank it names, the same way gold does at the frame's own buy button.
class item_ascension_bank_tab_voucher : public ItemScript
{
public:
    item_ascension_bank_tab_voucher() : ItemScript("item_ascension_bank_tab_voucher") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const&) override
    {
        // Which bank the voucher buys a tab in. Both vouchers exist in two catalogue entries
        // each, and they are otherwise indistinguishable on use - the only difference between
        // the two items is this choice.
        uint8 kind = 0;
        char const* which = nullptr;
        switch (item->GetEntry())
        {
            case 110002:    // Personal Bank Tab Voucher
            case 134986:    // Personal Bank Tab Voucher (second catalogue entry)
                kind = AscensionPersonalBank::PERSONAL;
                which = "personal bank";
                break;
            case 1180485:   // Realm Bank Tab Voucher
                kind = AscensionPersonalBank::REALM;
                which = "realm bank";
                break;
            default:
                return false;
        }

        // Answer the client's item request before the item is consumed.
        player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);

        // The tab row and the voucher that paid for it go in one commit, like the gold that
        // pays for a tab bought at the frame: AddTab writes into this transaction, the
        // destroyed voucher is written into it by SaveInventoryAndGoldToDB, and nothing is
        // sent to the database until both are in.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!AscensionPersonalBank::AddTab(player, kind, trans))
        {
            // Nothing was appended: the transaction is dropped without being committed.
            ChatHandler(player->GetSession())
                .PSendSysMessage("Your {} already owns every tab.", which);
            return true;
        }

        ChatHandler(player->GetSession())
            .PSendSysMessage("A new tab has been unlocked in your {}.", which);

        uint32 count = 1;
        player->DestroyItemCount(item, count, true);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }
};
} // namespace

void AddSC_AscensionPersonalBank()
{
    new AscensionPersonalBankPlayerScript();
    new item_ascension_bank_tab_voucher();
}
