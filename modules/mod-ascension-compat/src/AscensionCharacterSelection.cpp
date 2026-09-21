/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// THREAD HAND-OFF — READ THIS BEFORE MOVING ANY CALL IN THIS FILE.
// Every entry point here is reached from AscensionCompatServerScript::
// CanPacketReceiveEarly, which WorldSocket calls on the NETWORK thread. Nothing
// on the network thread may touch WorldSession::GetQueryProcessor() (see the
// long comment at "Network thread -> world thread hand-off" below), so this
// file registers two scripts, both created by AddSC_AscensionCharacterSelection()
// and both wired from MP_loader.cpp beside AddSC_AscensionPersonalBank():
//   * AscensionCharacterSelectionWorldScript — drains, every world tick, the
//     queue that the three extension opcodes (0x072E/0x072F/0x0772) park. Those
//     opcodes sit above NUM_OPCODE_HANDLERS, so they must be consumed on the
//     network thread and cannot travel through WorldSession::_recvQueue.
//   * AscensionCharacterSelectionServerScript — answers CMSG_CHAR_ENUM from
//     CanPacketReceive, i.e. inside WorldSession::Update on the WORLD thread and
//     immediately BEFORE WorldSession::HandleCharEnumOpcode. That keeps the
//     Ascension list query ahead of the core's own enum query in the very same
//     _queryProcessor vector, which is the order the client used to see, and it
//     costs no extra world tick.
// If either script is missing (MP_loader not wired), the matching path falls
// back to the old network-thread behaviour and logs one LOG_ERROR saying so.

#include "AscensionCharacterSelection.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Extensions.dll 2026-07 build: opcodes confirmed from the client senders
    // (push 0x72E in RequestActivate @0x10191540, push 0x72F in RequestDeactivate
    // @0x10191650, push 0x772 in SetCharacterSelectionSortOrder) and the client's
    // opcode name table (SMSG_CHARACTER_LIST_INFO @0x075E, ..._SORT_ORDER @0x076F,
    // ..._MAIL @0x0770, ..._GAME_MODE @0x0771, CMSG_..._SET_SORT_ORDER @0x0772).
    // The character-selection session has no Player object, so requests are
    // validated against the account id.
    constexpr uint16 CMSG_ASCENSION_CHARACTER_ACTIVATE = 0x072E;
    constexpr uint16 CMSG_ASCENSION_CHARACTER_DEACTIVATE = 0x072F;
    constexpr uint16 CMSG_ASCENSION_CHARACTER_SORT_ORDER = 0x0772;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_LIST_INFO = 0x075E;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT = 0x075F;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT = 0x0760;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_SORT_ORDER = 0x076F;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_SELECTION_MAIL = 0x0770;
    constexpr uint16 SMSG_ASCENSION_CHARACTER_SELECTION_GAME_MODE = 0x0771;

    // Enum.CharacterSelect (SharedXML\Enum.lua, mirrored by the client): team ids
    // are the classic faction-group ids and the ruleset decides which banner the
    // character list draws next to the name.
    constexpr uint32 CHARACTER_SELECTION_FACTION_ALLIANCE = 469;
    constexpr uint32 CHARACTER_SELECTION_FACTION_HORDE = 67;
    constexpr uint8 CHARACTER_SELECTION_RULESET_NONE = 0;
    constexpr uint8 CHARACTER_SELECTION_RULESET_HIGH_RISK = 1;

    // Rulesets are persisted as auras on the character (AscensionRulesets.cpp).
    constexpr uint32 SPELL_ASCENSION_HIGH_RISK = 1004019;

    // Result codes are matched verbatim by the client string tables
    // (Extensions.dll VA 0x10B38B38 for activate, 0x10B38A1C for deactivate).
    constexpr char ACTIVATE_CHARACTER_OK[] = "ACTIVATE_CHARACTER_OK";
    constexpr char ACTIVATE_CHARACTER_NOT_FOUND[] = "ACTIVATE_CHARACTER_NOT_FOUND";
    constexpr char ACTIVATE_CHARACTER_NOT_OWNED[] = "ACTIVATE_CHARACTER_NOT_OWNED";
    constexpr char ACTIVATE_CHARACTER_ALREADY_ACTIVE[] = "ACTIVATE_CHARACTER_ALREADY_ACTIVE";
    constexpr char ACTIVATE_CHARACTER_MAX_ACTIVE[] = "ACTIVATE_CHARACTER_MAX_ACTIVE";
    constexpr char ACTIVATE_CHARACTER_ONLINE[] = "ACTIVATE_CHARACTER_ONLINE";
    constexpr char ACTIVATE_CHARACTER_FAILED[] = "ACTIVATE_CHARACTER_FAILED";

    constexpr char DEACTIVATE_CHARACTER_OK[] = "DEACTIVATE_CHARACTER_OK";
    constexpr char DEACTIVATE_CHARACTER_NOT_FOUND[] = "DEACTIVATE_CHARACTER_NOT_FOUND";
    constexpr char DEACTIVATE_CHARACTER_NOT_OWNED[] = "DEACTIVATE_CHARACTER_NOT_OWNED";
    constexpr char DEACTIVATE_CHARACTER_ALREADY_INACTIVE[] = "DEACTIVATE_CHARACTER_ALREADY_INACTIVE";
    constexpr char DEACTIVATE_CHARACTER_ONLINE[] = "DEACTIVATE_CHARACTER_ONLINE";
    constexpr char DEACTIVATE_CHARACTER_FAILED[] = "DEACTIVATE_CHARACTER_FAILED";

    // The client clamps the reported character-list maximum to 0x80 entries.
    constexpr uint32 CHARACTER_LIST_MAXIMUM = 128;
    constexpr std::size_t SORT_ORDER_PAYLOAD_MAXIMUM = 1023;

    // "AscensionCompat.Enable" is the module's own master switch
    // (AscensionCompat.cpp:343 binds AscensionCompatConfig::ENABLED to it, same
    // default). It has to be tested here too: the network-thread entry points
    // are behind AscensionCompatServerScript::CanPacketReceiveEarly, which
    // returns early when the module is off (AscensionCompat.cpp:5100), but
    // AscensionCharacterSelectionServerScript::CanPacketReceive is reached
    // directly by the core and would otherwise answer CMSG_CHAR_ENUM on a
    // server where the module is disabled.
    bool CharacterSelectionEnabled()
    {
        return sConfigMgr->GetOption<bool>("AscensionCompat.Enable", true) &&
               sConfigMgr->GetOption<bool>("AscensionCompat.CharacterSelectionEnable", true);
    }

    uint32 CharacterSelectionMaxActive()
    {
        uint32 const maximum = sConfigMgr->GetOption<uint32>(
            "AscensionCompat.CharacterSelectionMaxActive", 10);
        return std::clamp(maximum, uint32(1), CHARACTER_LIST_MAXIMUM);
    }

    // Challenge modes as the client renders them (Enum.GameMode in
    // SharedXML\Enum.lua): Random 0x01, Ironman 0x02, Survivalist 0x04,
    // Draft 0x08, Resolute 0x20, WildCard 0x40, Felforged 0x80, Nightmare
    // 0x100, FreepickRarities 0x400, BuildDraft 0x800, Crusader 0x1000.
    // This fork does not persist per-character challenge modes yet, so the
    // client is told "no modes" and simply leaves the mode line empty.
    uint32 CharacterSelectionActiveGameModes()
    {
        return 0;
    }

    uint32 CharacterSelectionEnabledGameModes()
    {
        return 0;
    }

    // The client compares this against Enum.CharacterSelect.Faction
    // (Alliance = 469, Horde = 67, Other = 0) to pick the banner icon.
    uint32 CharacterSelectionTeamId(uint8 race)
    {
        switch (Player::TeamIdForRace(race))
        {
            case TEAM_ALLIANCE:
                return CHARACTER_SELECTION_FACTION_ALLIANCE;
            case TEAM_HORDE:
                return CHARACTER_SELECTION_FACTION_HORDE;
            default:
                return 0;
        }
    }

    // Per-character extras, mirroring the live server: one SMSG 0x0771 (game
    // mode) and one SMSG 0x0770 (mail) per character, sent before SMSG 0x075E.
    //
    // The leading u32 is not the guid but the character's 1-based position in the
    // character list: CharacterSelect.lua resolves each row through
    // GetCharIDFromIndex() (translationTable, built from 1..GetNumCharacters())
    // and passes that value to GetCharacterSelectionGameModeData(),
    // Extensions.dll hashes it straight into its per-character table. Inactive
    // characters are absent from that list (CHAR_SEL_ENUM filters them), so only
    // active characters get an entry and they are numbered consecutively.
    //
    // isMercenary: the client's own name for the byte after teamId
    // (CharacterSelect.lua reads it from GetCharacterSelectionGameModeData, and
    // Extensions.dll keeps it at struct +0x18 and hands it back as a boolean).
    // Its UI never uses it: that read is the only mercenary reference in the
    // shipped interface, neither client binary nor the server data has a
    // mercenary system, and the live capture has 0 for every character. Whether
    // it means own-faction PvP is unconfirmed, so 0 is sent.
    void SendCharacterSelectionPerCharacter(WorldSession* session, uint32 listIndex, uint8 race,
        bool highRisk, bool hasMail)
    {
        WorldPacket gameMode(SMSG_ASCENSION_CHARACTER_SELECTION_GAME_MODE, 18);
        gameMode << listIndex
                 << CharacterSelectionActiveGameModes()      // activeGameModes
                 << CharacterSelectionEnabledGameModes()     // enabledGameModes
                 << CharacterSelectionTeamId(race)           // teamId (banner)
                 << uint8(0)                                 // isMercenary
                 << uint8(highRisk ? CHARACTER_SELECTION_RULESET_HIGH_RISK
                                   : CHARACTER_SELECTION_RULESET_NONE);
        session->SendPacket(&gameMode);

        WorldPacket mail(SMSG_ASCENSION_CHARACTER_SELECTION_MAIL, 6);
        mail << listIndex
             << uint8(hasMail ? 1 : 0)                       // hasMail
             << uint8(0);                                    // hasStoreMail (not tracked by this fork)
        session->SendPacket(&mail);
    }

    void SendResult(WorldSession* session, uint16 opcode, char const* result)
    {
        WorldPacket packet(opcode, 64);
        packet << result;
        session->SendPacket(&packet);
    }

    // Character state shared by activate and deactivate: owning account, the
    // persisted online flag, the effective active flag (missing row = active)
    // and how many characters of that account are currently active.
    std::string BuildCharacterStateQuery(uint32 charGuid)
    {
        return Acore::StringFormat(
            "SELECT `c`.`account`, `c`.`online`, COALESCE(`s`.`active` <> 0, 1), "
            "(SELECT COUNT(*) FROM `characters` AS `c2` "
            "LEFT JOIN `character_ascension_state` AS `s2` ON `s2`.`guid` = `c2`.`guid` "
            "WHERE `c2`.`account` = `c`.`account` AND (`s2`.`active` IS NULL OR `s2`.`active` <> 0)) "
            "FROM `characters` AS `c` "
            "LEFT JOIN `character_ascension_state` AS `s` ON `s`.`guid` = `c`.`guid` "
            "WHERE `c`.`guid` = {}", charGuid);
    }

    void HandleActivateRequest(WorldSession* session, WorldPacket const& packet)
    {
        if (packet.size() < sizeof(uint32))
        {
            SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_FAILED);
            return;
        }

        uint32 const charGuid = packet.read<uint32>(0);
        uint32 const accountId = session->GetAccountId();

        session->GetQueryProcessor().AddCallback(
            CharacterDatabase.AsyncQuery(BuildCharacterStateQuery(charGuid)).WithCallback(
                [session, accountId, charGuid](QueryResult result)
                {
                    if (!result || result->GetRowCount() == 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_NOT_FOUND);
                        return;
                    }

                    Field* fields = result->Fetch();

                    if (fields[0].Get<uint32>() != accountId)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_NOT_OWNED);
                        return;
                    }

                    if (fields[1].Get<uint8>() != 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_ONLINE);
                        return;
                    }

                    if (fields[2].Get<uint32>() != 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_ALREADY_ACTIVE);
                        return;
                    }

                    uint64 const activeCount = fields[3].Get<uint64>();
                    if (activeCount >= CharacterSelectionMaxActive())
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_MAX_ACTIVE);
                        return;
                    }

                    CharacterDatabase.Execute(
                        "INSERT INTO `character_ascension_state` (`guid`, `active`) VALUES ({}, 1) "
                        "ON DUPLICATE KEY UPDATE `active` = 1", charGuid);

                    SendResult(session, SMSG_ASCENSION_CHARACTER_ACTIVATE_RESULT, ACTIVATE_CHARACTER_OK);

                    // The auth-side realm count follows active characters (the
                    // realm list shows it), so it has to learn about the change.
                    sWorld->UpdateRealmCharCount(accountId);

                    LOG_INFO("module.ascension_compat",
                        "Activated character {} for account {} ({}/{} active characters)",
                        charGuid, accountId, activeCount + 1, CharacterSelectionMaxActive());
                }));
    }

    void HandleDeactivateRequest(WorldSession* session, WorldPacket const& packet)
    {
        if (packet.size() < sizeof(uint32))
        {
            SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_FAILED);
            return;
        }

        uint32 const charGuid = packet.read<uint32>(0);
        uint32 const accountId = session->GetAccountId();

        session->GetQueryProcessor().AddCallback(
            CharacterDatabase.AsyncQuery(BuildCharacterStateQuery(charGuid)).WithCallback(
                [session, accountId, charGuid](QueryResult result)
                {
                    if (!result || result->GetRowCount() == 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_NOT_FOUND);
                        return;
                    }

                    Field* fields = result->Fetch();

                    if (fields[0].Get<uint32>() != accountId)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_NOT_OWNED);
                        return;
                    }

                    if (fields[1].Get<uint8>() != 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_ONLINE);
                        return;
                    }

                    if (fields[2].Get<uint32>() == 0)
                    {
                        SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_ALREADY_INACTIVE);
                        return;
                    }

                    CharacterDatabase.Execute(
                        "INSERT INTO `character_ascension_state` (`guid`, `active`) VALUES ({}, 0) "
                        "ON DUPLICATE KEY UPDATE `active` = 0", charGuid);

                    SendResult(session, SMSG_ASCENSION_CHARACTER_DEACTIVATE_RESULT, DEACTIVATE_CHARACTER_OK);

                    // Parking a character frees a realm slot, and the realm list
                    // count has to follow.
                    sWorld->UpdateRealmCharCount(accountId);

                    LOG_INFO("module.ascension_compat",
                        "Deactivated character {} for account {}", charGuid, accountId);
                }));
    }

    // CMSG 0x0772 carries an opaque, space-separated sort table ("1 2 3 ...").
    // The client composes it from its own list positions, so the server only
    // stores and later echoes the payload through SMSG 0x076F.
    void HandleSortOrderRequest(WorldSession* session, WorldPacket const& packet)
    {
        // operator>> advances the read cursor, so read from a copy of the const packet.
        //
        // ReadCString(false), NOT operator>>: ByteBuffer.h:293 makes operator>>
        // call ReadCString(true), which throws ByteBufferInvalidValueException
        // (ByteBuffer.cpp:88-89) on any payload that is not valid UTF-8. That
        // exception had no handler anywhere on the path it travels
        // (CanPacketReceiveEarly -> WorldSocket::ReadDataHandler ->
        // Socket<T>::ReadHandlerInternal -> NetworkThread::Run's _ioContext.run()),
        // so three bytes from any authenticated client terminated the whole
        // worldserver. ReadCString(false) is bounded by size() and cannot throw;
        // the payload is validated below instead.
        WorldPacket readable = packet;
        std::string const payload = readable.ReadCString(false);

        // The client always sends a full table (the reset button re-sends
        // "1 2 3 ..."), so an empty payload is malformed and must not clobber
        // a stored order.
        if (payload.empty())
        {
            LOG_DEBUG("module.ascension_compat",
                "Ignored empty character-selection sort order from account {}",
                session->GetAccountId());
            return;
        }

        if (payload.size() > SORT_ORDER_PAYLOAD_MAXIMUM)
        {
            LOG_ERROR("module.ascension_compat",
                "Account {} sent an oversized character-selection sort order ({} bytes); ignored",
                session->GetAccountId(), payload.size());
            return;
        }

        // The stored value is echoed straight back through SMSG 0x076F as a
        // cstring, so nothing outside printable ASCII has any business being
        // there (the table the client composes, and the identity table this
        // file generates below, are digits and spaces). Refusing the rest keeps
        // control bytes and broken encodings out of the column and out of the
        // packet that goes back to the client.
        if (std::any_of(payload.begin(), payload.end(),
                [](char c) { return uint8(c) < 0x20 || uint8(c) > 0x7E; }))
        {
            LOG_ERROR("module.ascension_compat",
                "Account {} sent a character-selection sort order with non-printable bytes ({} bytes); ignored",
                session->GetAccountId(), payload.size());
            return;
        }

        std::string escaped = payload;
        CharacterDatabase.EscapeString(escaped);

        CharacterDatabase.Execute(
            "INSERT INTO `account_ascension_settings` (`account_id`, `sort_order`) VALUES ({}, '{}') "
            "ON DUPLICATE KEY UPDATE `sort_order` = VALUES(`sort_order`)",
            session->GetAccountId(), escaped);

        LOG_DEBUG("module.ascension_compat",
            "Stored character-selection sort order for account {} ({} bytes)",
            session->GetAccountId(), payload.size());
    }

    // Wire layout of one SMSG 0x075E entry (Extensions.dll parser RVA 190EE0):
    // u32 guid, u8 active, u8 online, u8 level, u8 race, u8 class, u8 gender,
    // u32 zoneId, cstring name. The header holds four counts, read into the
    // list object at +4, +8, +0xC and +0x10 in packet order and returned by
    // C_CharacterList.GetCounts() as max, total, active and inactive; the
    // inactive one is what HasInactiveCharacters() tests. The client fires
    // CHARACTER_LIST_UPDATED when the packet arrives.
    void HandleCharacterListQuery(WorldSession* session, uint32 maxActive, QueryResult result)
    {
        // Per-character data for SMSG 0x0770 / 0x0771, sent after the sort order
        // and before the list itself (the order the live server uses).
        struct CharacterSelectionExtra
        {
            uint32 index;
            uint8 race;
            bool highRisk;
            bool hasMail;
        };

        std::vector<CharacterSelectionExtra> extras;
        extras.reserve(CHARACTER_LIST_MAXIMUM);

        uint32 total = 0;
        uint32 activeCount = 0;
        uint32 listIndex = 0;
        std::string sortOrder;

        // Four counts, in the order CharacterSelect.lua receives them from
        // C_CharacterList.GetCounts(): max, total, active, inactive. The max is
        // clamped to 128 by Extensions.dll, and the client's create gate is
        // "active < max" (CharacterSelect_CanCreateCharacter), so this header is
        // what decides whether a character can be created at all: with the max at
        // the active limit, creating needs a free active slot. DEFAULT_MAX_
        // CHARACTERS_PER_REALM = 10 in the client Lua is only the fallback it uses
        // when the header reports a max of 0.
        WorldPacket info(SMSG_ASCENSION_CHARACTER_LIST_INFO, 256);
        info << maxActive << uint32(0) << uint32(0) << uint32(0);

        // Query layout: 0 guid, 1 name, 2 online, 3 level, 4 race, 5 class,
        // 6 gender, 7 zone, 8 active, 9 stored sort order, 10 high-risk aura,
        // 11 unread mail.
        if (result && result->GetRowCount() != 0)
        {
            do
            {
                Field* fields = result->Fetch();

                uint8 const active = fields[8].Get<uint32>() != 0 ? 1 : 0;
                ++total;
                activeCount += active;

                if (total == 1)
                    sortOrder = fields[9].Get<std::string>();

                uint32 const guid = fields[0].Get<uint32>();
                uint8 const race = fields[4].Get<uint8>();

                // Extras are keyed by list position, and CHAR_SEL_ENUM drops
                // inactive characters from that list, so they must not consume
                // one (their rows never ask for game-mode or mail data).
                if (active != 0)
                {
                    extras.push_back({ ++listIndex, race,
                        fields[10].Get<uint64>() != 0, fields[11].Get<uint64>() != 0 });
                }

                info << guid                                                       // guid
                     << active                                                     // active
                     << uint8(fields[2].Get<uint8>() != 0 ? 1 : 0)                 // online
                     << fields[3].Get<uint8>()                                    // level
                     << race                                                      // race
                     << fields[5].Get<uint8>()                                    // class
                     << fields[6].Get<uint8>()                                    // gender
                     << fields[7].Get<uint32>()                                   // zone
                     << fields[1].Get<std::string>();                             // name (cstring)
            } while (result->NextRow());
        }

        // Second count is the whole list, third the active count, fourth the
        // parked count. Lua reads the third one for the create gate and the
        // fourth one for HasInactiveCharacters(), so both have to stay accurate.
        info.put<uint32>(4, total);
        info.put<uint32>(8, activeCount);
        info.put<uint32>(12, total - activeCount);

        // CharacterSelect.lua only enables reordering when its sort order is
        // set (canSort = true after GetCharacterSelectionSortOrder() returns a
        // table), and it can only send one once reordering is possible. So the
        // packet is always answered: the stored order, or the identity table
        // the client itself produces when the order is reset ("1 2 3 ...",
        // padded to the display maximum it reports in the list header).
        if (sortOrder.empty())
        {
            for (uint32 i = 1; i <= maxActive; ++i)
            {
                if (i > 1)
                    sortOrder += ' ';
                sortOrder += std::to_string(i);
            }
        }

        WorldPacket order(SMSG_ASCENSION_CHARACTER_SORT_ORDER, sortOrder.size() + 8);
        order << sortOrder;
        session->SendPacket(&order);

        for (CharacterSelectionExtra const& extra : extras)
        {
            SendCharacterSelectionPerCharacter(session, extra.index, extra.race,
                extra.highRisk, extra.hasMail);
        }

        session->SendPacket(&info);

        // NOTE: no list-flags packet anymore. The live capture contains no
        // SMSG 0x0767, the client's opcode table assigns 0x0768 to something
        // else entirely, and C_CharacterList.HasInactiveCharacters() reads the
        // inactive count out of the list header above rather than from a flag.

        LOG_DEBUG("module.ascension_compat",
            "Sent Ascension character list for account {}: max={}, total={}, active={}, inactive={}, extras={}, sort order={} bytes",
            session->GetAccountId(), maxActive, total, activeCount, total - activeCount, extras.size(), sortOrder.size());
    }

    // Runs on the world thread only: it feeds session->GetQueryProcessor().
    void SendCharacterListInfoNow(WorldSession* session)
    {
        uint32 const accountId = session->GetAccountId();
        uint32 const maxActive = CharacterSelectionMaxActive();

        // Mirrors the core enum filter: inactive characters are excluded from
        // SMSG_CHAR_ENUM but still listed here with active = 0. Active characters
        // come first, each group in enum order (COALESCE(c.order, c.guid)): the
        // client takes the entries after the active count to be the inactive ones,
        // and it resolves a row by its position among the active characters.
        std::string const query = Acore::StringFormat(
            "SELECT `c`.`guid`, `c`.`name`, `c`.`online`, `c`.`level`, `c`.`race`, `c`.`class`, `c`.`gender`, `c`.`zone`, "
            "COALESCE(`s`.`active` <> 0, 1), "
            "COALESCE((SELECT `a`.`sort_order` FROM `account_ascension_settings` AS `a` "
            "WHERE `a`.`account_id` = {}), ''), "
            "EXISTS(SELECT 1 FROM `character_aura` AS `ha` WHERE `ha`.`guid` = `c`.`guid` AND `ha`.`spell` = {}), "
            "EXISTS(SELECT 1 FROM `mail` AS `m` WHERE `m`.`receiver` = `c`.`guid` "
            "AND (`m`.`checked` & 1) = 0 AND `m`.`deliver_time` <= UNIX_TIMESTAMP()) "
            "FROM `characters` AS `c` "
            "LEFT JOIN `character_ascension_state` AS `s` ON `s`.`guid` = `c`.`guid` "
            "WHERE `c`.`account` = {} AND `c`.`deleteInfos_Name` IS NULL "
            "ORDER BY COALESCE(`s`.`active` <> 0, 1) DESC, COALESCE(`c`.`order`, `c`.`guid`)",
            accountId, SPELL_ASCENSION_HIGH_RISK, accountId);

        session->GetQueryProcessor().AddCallback(
            CharacterDatabase.AsyncQuery(query).WithCallback(
                [session, maxActive](QueryResult result)
                {
                    HandleCharacterListQuery(session, maxActive, result);
                }));
    }

    // -----------------------------------------------------------------------
    // Network thread -> world thread hand-off
    //
    // Every entry point of this file is reached from
    // AscensionCompatServerScript::CanPacketReceiveEarly, which WorldSocket
    // calls on the NETWORK thread (the comment at AscensionCompat.cpp:4943 says
    // so itself, and routes CMSG_CHARACTER_ADVANCEMENT_KNOWN_ENTRIES through a
    // queue for exactly that reason). WorldSession::GetQueryProcessor() is an
    // AsyncCallbackProcessor whose whole state is a bare std::vector
    // (AsyncCallbackProcessor.h:59) with no lock: AddCallback() does
    // emplace_back while the WORLD thread, in WorldSession::Update ->
    // ProcessQueryCallbacks (WorldSession.cpp:614, :1435), does
    // `std::vector<T> updateCallbacks{ std::move(_callbacks) }`. Calling it from
    // the network thread is a plain data race on that vector.
    //
    // So nothing is executed here any more: the request is copied into a
    // mutex-protected queue and replayed from the world thread. The session is
    // looked up again on replay (never kept as a pointer across threads), which
    // also removes the dangling-session window the old code had.
    // -----------------------------------------------------------------------
    enum class SelectionRequestKind : uint8
    {
        Activate,
        Deactivate,
        SortOrder,
    };

    struct PendingSelectionRequest
    {
        uint32 accountId = 0;
        SelectionRequestKind kind = SelectionRequestKind::Activate;
        uint16 opcode = 0;
        std::vector<uint8> body;
    };

    // A session that spams the character screen must not grow this without
    // bound; the world thread drains it every tick, so the caps are only a
    // guard. The per-account one matters as much as the global one: these
    // opcodes are consumed before WorldSession::AntiDOS ever sees them, so one
    // account could otherwise fill the whole queue and starve every other.
    constexpr std::size_t MAX_PENDING_SELECTION_REQUESTS = 512;
    constexpr std::size_t MAX_PENDING_SELECTION_REQUESTS_PER_ACCOUNT = 8;
    // The largest useful body is one sort-order table (SORT_ORDER_PAYLOAD_
    // MAXIMUM plus its terminator); WorldSocket lets a client send about 10 KB.
    constexpr std::size_t MAX_SELECTION_REQUEST_BODY = 2048;

    std::mutex gPendingSelectionLock;
    std::deque<PendingSelectionRequest> gPendingSelection;

    // Set by each script's constructor, i.e. only if MP_loader.cpp really called
    // AddSC_AscensionCharacterSelection(). Without them nothing would ever drain
    // the queue, nor answer CMSG_CHAR_ENUM from the world thread, so the old
    // direct paths stay available as fallbacks and say once, loudly, what is
    // missing. They are only ever stored during script registration (single
    // threaded, before the network starts) and loaded afterwards.
    std::atomic<bool> gSelectionPumpRegistered{ false };
    std::atomic<bool> gSelectionEnumHookRegistered{ false };
    std::atomic<bool> gSelectionPumpWarned{ false };
    std::atomic<bool> gSelectionEnumHookWarned{ false };

    void DispatchSelectionRequest(WorldSession* session, SelectionRequestKind kind, WorldPacket const& packet)
    {
        switch (kind)
        {
            case SelectionRequestKind::Activate:
                HandleActivateRequest(session, packet);
                break;
            case SelectionRequestKind::Deactivate:
                HandleDeactivateRequest(session, packet);
                break;
            case SelectionRequestKind::SortOrder:
                HandleSortOrderRequest(session, packet);
                break;
        }
    }

    void WarnSelectionPumpMissing()
    {
        if (gSelectionPumpWarned.exchange(true, std::memory_order_acq_rel))
            return;

        LOG_ERROR("module.ascension_compat",
            "AscensionCharacterSelectionWorldScript is not registered: character-selection "
            "requests keep running on the network thread, which races WorldSession's query processor. "
            "Check that MP_loader.cpp calls AddSC_AscensionCharacterSelection().");
    }

    void WarnSelectionEnumHookMissing()
    {
        if (gSelectionEnumHookWarned.exchange(true, std::memory_order_acq_rel))
            return;

        LOG_ERROR("module.ascension_compat",
            "AscensionCharacterSelectionServerScript is not registered: the Ascension character list "
            "keeps being queried from the network thread, which races WorldSession's query processor. "
            "Check that MP_loader.cpp calls AddSC_AscensionCharacterSelection().");
    }

    void DeferOrRunSelectionRequest(WorldSession* session, SelectionRequestKind kind, WorldPacket const& packet)
    {
        if (!gSelectionPumpRegistered.load(std::memory_order_acquire))
        {
            WarnSelectionPumpMissing();
            DispatchSelectionRequest(session, kind, packet);
            return;
        }

        uint32 const accountId = session->GetAccountId();

        if (packet.size() > MAX_SELECTION_REQUEST_BODY)
        {
            LOG_ERROR("module.ascension_compat",
                "Dropping an oversized character-selection request from account {} (opcode {}, {} bytes)",
                accountId, uint32(packet.GetOpcode()), packet.size());
            return;
        }

        PendingSelectionRequest request;
        request.accountId = accountId;
        request.kind = kind;
        request.opcode = static_cast<uint16>(packet.GetOpcode());
        if (packet.size())
            request.body.assign(packet.contents(), packet.contents() + packet.size());

        std::lock_guard<std::mutex> lock(gPendingSelectionLock);
        if (gPendingSelection.size() >= MAX_PENDING_SELECTION_REQUESTS)
        {
            LOG_WARN("module.ascension_compat",
                "Dropping a character-selection request from account {}: the queue is full ({} entries)",
                accountId, gPendingSelection.size());
            return;
        }

        std::size_t const fromThisAccount = static_cast<std::size_t>(
            std::count_if(gPendingSelection.begin(), gPendingSelection.end(),
                [accountId](PendingSelectionRequest const& pending) { return pending.accountId == accountId; }));
        if (fromThisAccount >= MAX_PENDING_SELECTION_REQUESTS_PER_ACCOUNT)
        {
            LOG_WARN("module.ascension_compat",
                "Dropping a character-selection request from account {}: {} of its requests are already waiting",
                accountId, fromThisAccount);
            return;
        }

        gPendingSelection.push_back(std::move(request));
    }
}

bool IsAscensionCharacterSelectionOpcode(uint16 opcode)
{
    return opcode == CMSG_ASCENSION_CHARACTER_ACTIVATE ||
           opcode == CMSG_ASCENSION_CHARACTER_DEACTIVATE ||
           opcode == CMSG_ASCENSION_CHARACTER_SORT_ORDER;
}

bool HandleAscensionCharacterSelectionPacket(WorldSession* session, WorldPacket const& packet)
{
    if (!session || !CharacterSelectionEnabled())
        return false;

    SelectionRequestKind kind;
    switch (uint16(packet.GetOpcode()))
    {
        case CMSG_ASCENSION_CHARACTER_ACTIVATE:
            kind = SelectionRequestKind::Activate;
            break;
        case CMSG_ASCENSION_CHARACTER_DEACTIVATE:
            kind = SelectionRequestKind::Deactivate;
            break;
        case CMSG_ASCENSION_CHARACTER_SORT_ORDER:
            kind = SelectionRequestKind::SortOrder;
            break;
        default:
            return false;
    }

    // Nothing may escape from here: this runs on the network thread, where
    // WorldSocket::ReadDataHandler wraps only CMSG_PING and CMSG_AUTH_SESSION in
    // a try, and an exception that reaches NetworkThread::Run's _ioContext.run()
    // takes the whole realm down.
    try
    {
        DeferOrRunSelectionRequest(session, kind, packet);
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module.ascension_compat",
            "Ignored a malformed character-selection packet (opcode {}, {} bytes) from account {}: {}",
            uint32(packet.GetOpcode()), packet.size(), session->GetAccountId(), e.what());
    }
    catch (...)
    {
        LOG_ERROR("module.ascension_compat",
            "Ignored a malformed character-selection packet (opcode {}, {} bytes) from account {}",
            uint32(packet.GetOpcode()), packet.size(), session->GetAccountId());
    }

    // Consumed either way. These opcodes are above NUM_OPCODE_HANDLERS, so
    // letting them through would make WorldSocket close the connection.
    return true;
}

void SendAscensionCharacterListInfo(WorldSession* session)
{
    if (!session || !CharacterSelectionEnabled())
        return;

    // Registered path: AscensionCharacterSelectionServerScript::CanPacketReceive
    // answers this very CMSG_CHAR_ENUM a moment later, on the world thread and
    // just before WorldSession::HandleCharEnumOpcode. Doing anything here would
    // send the list twice, so the network thread has nothing left to do.
    if (gSelectionEnumHookRegistered.load(std::memory_order_acquire))
        return;

    // Fallback only (script not registered): the old behaviour, which races the
    // world thread on WorldSession::GetQueryProcessor(). Kept so the character
    // screen still fills, and loudly reported once per process.
    WarnSelectionEnumHookMissing();

    try
    {
        SendCharacterListInfoNow(session);
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module.ascension_compat",
            "Could not send the Ascension character list for account {}: {}",
            session->GetAccountId(), e.what());
    }
    catch (...)
    {
        LOG_ERROR("module.ascension_compat",
            "Could not send the Ascension character list for account {}", session->GetAccountId());
    }
}

void ProcessAscensionCharacterSelectionQueue()
{
    std::deque<PendingSelectionRequest> batch;
    {
        std::lock_guard<std::mutex> lock(gPendingSelectionLock);
        if (gPendingSelection.empty())
            return;
        batch.swap(gPendingSelection);
    }

    for (PendingSelectionRequest const& request : batch)
    {
        // The session is resolved again here, on the world thread that owns the
        // session map: the account may have disconnected since the packet was
        // read, and keeping a WorldSession* across threads is exactly what this
        // hand-off exists to avoid.
        WorldSession* session = sWorldSessionMgr->FindSession(request.accountId);
        if (!session)
            continue;

        try
        {
            WorldPacket packet(request.opcode, request.body.size());
            if (!request.body.empty())
                packet.append(request.body.data(), request.body.size());

            DispatchSelectionRequest(session, request.kind, packet);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module.ascension_compat",
                "Ignored a malformed character-selection request (opcode {}) from account {}: {}",
                request.opcode, request.accountId, e.what());
        }
        catch (...)
        {
            LOG_ERROR("module.ascension_compat",
                "Ignored a malformed character-selection request (opcode {}) from account {}",
                request.opcode, request.accountId);
        }
    }
}

namespace
{
    // Drains, on the world thread, what the three extension opcodes parked from
    // the network thread. WorldScript::OnUpdate is called from World::Update
    // (World.cpp:1346), after WorldSessionMgr::UpdateSessions (World.cpp:1218):
    // these are self-contained request/response exchanges, so the tick of delay
    // costs nothing and no packet order depends on it.
    class AscensionCharacterSelectionWorldScript : public WorldScript
    {
    public:
        AscensionCharacterSelectionWorldScript()
            : WorldScript("AscensionCharacterSelectionWorldScript", { WORLDHOOK_ON_UPDATE })
        {
            gSelectionPumpRegistered.store(true, std::memory_order_release);
        }

        void OnUpdate(uint32 /*diff*/) override
        {
            ProcessAscensionCharacterSelectionQueue();
        }
    };

    // CMSG_CHAR_ENUM is a real core opcode (Opcodes.cpp:186, STATUS_AUTHED), so
    // it is queued into WorldSession::_recvQueue and handled on the world thread.
    // WorldSession::Update calls sScriptMgr->CanPacketReceive (WorldSession.cpp:
    // 524, STATUS_AUTHED branch) immediately before opHandle->Call, i.e. before
    // HandleCharEnumOpcode adds the core's own enum query to _queryProcessor
    // (CharacterHandler.cpp:264). Sending the Ascension list from here therefore
    // does two things at once: it leaves the network thread out of the query
    // processor, and it keeps the Ascension query ahead of the core enum query
    // in that same vector, which is the order the client saw before.
    //
    // This returns true unconditionally: the packet is the core's, we only ride
    // along. Never consume it, or the client never gets SMSG_CHAR_ENUM.
    class AscensionCharacterSelectionServerScript : public ServerScript
    {
    public:
        AscensionCharacterSelectionServerScript()
            : ServerScript("AscensionCharacterSelectionServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE })
        {
            gSelectionEnumHookRegistered.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (!session || packet.GetOpcode() != CMSG_CHAR_ENUM || !CharacterSelectionEnabled())
                return true;

            // Same discipline as every other entry point of this file: nothing
            // escapes. This one runs on the world thread, where an escaping
            // exception would be caught by WorldSession::Update's own try
            // (WorldSession.cpp:451) and would skip the core's enum handler.
            try
            {
                SendCharacterListInfoNow(session);
            }
            catch (std::exception const& e)
            {
                LOG_ERROR("module.ascension_compat",
                    "Could not send the Ascension character list for account {}: {}",
                    session->GetAccountId(), e.what());
            }
            catch (...)
            {
                LOG_ERROR("module.ascension_compat",
                    "Could not send the Ascension character list for account {}", session->GetAccountId());
            }

            return true;
        }
    };
}

void AddSC_AscensionCharacterSelection()
{
    new AscensionCharacterSelectionWorldScript();
    new AscensionCharacterSelectionServerScript();
}
