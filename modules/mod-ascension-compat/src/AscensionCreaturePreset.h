#ifndef AZEROTHCORE_ASCENSION_CREATURE_PRESET_H
#define AZEROTHCORE_ASCENSION_CREATURE_PRESET_H

#include "Common.h"
#include "ObjectGuid.h"
#include <array>
#include <atomic>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

struct CreatureDisplayPreset
{
    uint32 entry = 0;
    uint32 display_id = 0;
    uint8 race = 0;
    uint8 gender = 0;
    uint8 class_id = 0;
    uint8 skin = 0;
    uint8 face = 0;
    uint8 hair = 0;
    uint8 haircolor = 0;
    uint8 facialhair = 0;
    uint32 guild_id = 0;
    std::array<uint32, 11> items = {0};
};

/// Le catalogue des apparences de creature, et les morphes en cours.
///
/// Les trois tables sont lues depuis les fils de carte - `OnAllCreatureUpdate`, `OnCreatureAddWorld`,
/// `OnPlayerUpdate` - et ecrites par `.localreloadpresets`, `.morphpreset` et `.demorphpreset`, qui
/// arrivent par CMSG_MESSAGECHAT. Cet opcode est PROCESS_THREADUNSAFE, donc traite par le fil monde
/// apres que `MapMgr::Update` a joint tous les fils de carte : les ecritures ne pouvaient pas croiser
/// les lectures, et la surete reposait entierement sur cet invariant, qui n'etait ecrit nulle part.
/// Le jour ou une de ces ecritures devient atteignable depuis un chemin de fil de carte - un script de
/// sort, un crochet de creature, un opcode PROCESS_INPLACE - on retombe sur P-050 (rehash concurrent,
/// segfault) sur un chemin dix fois plus chaud. Le verrou ci-dessous rend l'invariant inutile.
///
/// Les accesseurs rendent une COPIE (`std::optional`) et non un pointeur dans la table : la structure
/// fait moins de cent octets, et les appelants - HandleMorphPresetCommand, CanPacketReceive - gardent
/// la valeur sur plusieurs lignes, ce qu'un pointeur nu ne permet plus des que le verrou est rendu.
class AscensionCreaturePresetMgr
{
public:
    static AscensionCreaturePresetMgr* Instance();

    void LoadFromDB();

    [[nodiscard]] std::optional<CreatureDisplayPreset> GetPreset(uint32 entry, uint32 displayId = 0) const;
    [[nodiscard]] std::optional<CreatureDisplayPreset> GetPresetByGender(uint32 entry, uint8 gender) const;
    [[nodiscard]] bool HasPreset(uint32 entry, uint32 displayId = 0) const;
    [[nodiscard]] std::size_t GetPresetCount() const;

    void SetActivePresetOverride(ObjectGuid guid, uint32 entry, uint32 displayId = 0);
    void ClearActivePresetOverride(ObjectGuid guid);
    [[nodiscard]] std::optional<CreatureDisplayPreset> GetActivePresetOverride(ObjectGuid guid) const;
    /// Le meme predicat que GetActivePresetOverride, sans la copie : ce test tourne a chaque tick.
    [[nodiscard]] bool HasActivePresetOverride(ObjectGuid guid) const;

private:
    static uint64 MakeKey(uint32 entry, uint32 displayId)
    {
        return (uint64(entry) << 32) | uint64(displayId);
    }

    /// Appelees le verrou deja tenu : std::shared_mutex n'est pas recursif.
    [[nodiscard]] CreatureDisplayPreset const* FindPresetUnlocked(uint32 entry, uint32 displayId) const;
    [[nodiscard]] CreatureDisplayPreset const* FindPresetByGenderUnlocked(uint32 entry, uint8 gender) const;

    mutable std::shared_mutex _lock;
    /// Le nombre d'entrees de _activePresetOverrides, tenu sous unique_lock et lu hors verrou.
    ///
    /// La table est vide chez presque tout le monde : elle ne se remplit que par `.morphpreset`.
    /// Or ses lecteurs sont les chemins les plus chauds du module - OnPlayerUpdate pour chaque
    /// joueur et chaque bot a chaque tick, OnCreatureRemoveWorld pour chaque creature qui quitte
    /// le monde, ce qui veut dire toute une grille d'un coup au dechargement. Sans ce portillon,
    /// poser le verrou rendrait ces chemins plus chers qu'ils ne l'etaient sans verrou du tout.
    /// Un lecteur qui lit zero peut avoir un tick de retard sur un morphe tout juste pose ; les
    /// deux ecritures viennent de commandes de tchat et ne se croisent pas entre elles.
    std::atomic<uint32> _activeOverrideCount{0};
    std::unordered_map<uint64, CreatureDisplayPreset> _presets;
    std::unordered_map<uint32, std::vector<uint32>> _entryToDisplays;
    std::unordered_map<ObjectGuid, uint64> _activePresetOverrides;
};

#define sAscensionPresets AscensionCreaturePresetMgr::Instance()

#endif // AZEROTHCORE_ASCENSION_CREATURE_PRESET_H
