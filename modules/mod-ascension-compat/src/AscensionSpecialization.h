#ifndef ASCENSION_SPECIALIZATION_H
#define ASCENSION_SPECIALIZATION_H

#include "Define.h"

class Player;

// Active CoA specialization of a custom-class character, 0 when none. Falls back to the
// saved setting, so a caller running before this module's login hook still sees the
// choice of a returning character.
uint32 GetAscensionActiveSpecialization(Player const* player);

// Activates a specialization the same way the client's choice does (".local spec").
bool SwitchAscensionSpecialization(Player* player, uint32 specializationId);

// Rank currently held in a paid Character Advancement entry (ability or talent essence), 0 if none.
uint32 GetAscensionTalentRank(Player const* player, uint32 entryId);

// Sets a paid Character Advancement entry to `rank` (0 removes it) with the same rules as
// ".local talent", without chat replies. Essence budgets are the caller's business: like the
// command, this does not count points.
bool SetAscensionTalentRank(Player* player, uint32 entryId, uint32 rank);

#endif
