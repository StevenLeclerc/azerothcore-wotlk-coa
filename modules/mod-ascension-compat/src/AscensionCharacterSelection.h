/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */
#ifndef ASCENSION_CHARACTER_SELECTION_H
#define ASCENSION_CHARACTER_SELECTION_H

#include "Define.h"

class WorldSession;
class WorldPacket;

// Ascension character-selection protocol (Extensions.dll, 2026-07 build).
// The custom client layers its character screen on top of the standard enum
// (names taken from the client's opcode table):
//   CMSG 0x072E CharacterSelection ActivateCharacter(guid)
//   CMSG 0x072F CharacterSelection DeactivateCharacter(guid)
//   CMSG 0x0772 CHARACTER_SELECTION_SET_SORT_ORDER(payload)
//   SMSG 0x075E CHARACTER_LIST_INFO (four counts, then one entry per character)
//   SMSG 0x075F/0x0760 activate/deactivate results
//   SMSG 0x076F CHARACTER_SELECTION_SORT_ORDER
//   SMSG 0x0770 CHARACTER_SELECTION_MAIL      (per character: list index, hasMail, hasStoreMail)
//   SMSG 0x0771 CHARACTER_SELECTION_GAME_MODE (per character: list index, active/enabled modes,
//                                              team, mercenary, ruleset -> banner icon)
// The four counts of SMSG 0x075E are max, total, active and inactive, in that
// order: the client derives its create gate from them (it offers a new
// character while active < max).
// The per-character packets are keyed by the character's 1-based position in the
// character list (the value the client passes to GetCharacterSelectionGameModeData),
// not by guid; inactive characters are not part of that list.
// These requests arrive while the session is STATUS_AUTHED (there is no Player),
// so they are handled from the early extension sink and stay account-scoped.
bool IsAscensionCharacterSelectionOpcode(uint16 opcode);

// Returns true when the packet was consumed by this feature.
bool HandleAscensionCharacterSelectionPacket(WorldSession* session, WorldPacket const& packet);

// Legacy entry point for CMSG_CHAR_ENUM, still called from
// AscensionCompatServerScript::CanPacketReceiveEarly on the NETWORK thread.
// It is a no-op as soon as AddSC_AscensionCharacterSelection() has run, because
// AscensionCharacterSelectionServerScript::CanPacketReceive then answers the
// same CMSG_CHAR_ENUM on the world thread, just before the core's own handler.
// It only does the work itself — racily, and after one LOG_ERROR — when that
// script is missing. Does nothing when the feature is disabled.
void SendAscensionCharacterListInfo(WorldSession* session);

// Drains the activate/deactivate/sort-order requests parked from the network
// thread by HandleAscensionCharacterSelectionPacket.
// WORLD THREAD ONLY: it feeds WorldSession::GetQueryProcessor(), whose vector
// has no lock, and it resolves sessions through WorldSessionMgr::FindSession().
// AscensionCharacterSelectionWorldScript calls it every world tick; it is
// cheap when the queue is empty and safe to call more than once per tick.
void ProcessAscensionCharacterSelectionQueue();

// Registers AscensionCharacterSelectionWorldScript (the queue pump) and
// AscensionCharacterSelectionServerScript (the world-thread CMSG_CHAR_ENUM
// hook). Called from MP_loader.cpp: without it both paths fall back to the
// network thread and each logs one LOG_ERROR.
void AddSC_AscensionCharacterSelection();

#endif // ASCENSION_CHARACTER_SELECTION_H
