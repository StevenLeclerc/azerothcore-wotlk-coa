#include "AscensionCreaturePreset.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"
#include <utility>

AscensionCreaturePresetMgr* AscensionCreaturePresetMgr::Instance()
{
    static AscensionCreaturePresetMgr instance;
    return &instance;
}

void AscensionCreaturePresetMgr::LoadFromDB()
{
    // La requete court HORS du verrou : on ne tient jamais un verrou du module en appelant du
    // code qui peut bloquer. Le catalogue est bati a cote, puis echange sous unique_lock, de
    // sorte qu'un lecteur voit soit l'ancien catalogue entier, soit le nouveau, jamais un
    // catalogue vide au milieu du remplissage.
    std::unordered_map<uint64, CreatureDisplayPreset> presets;
    std::unordered_map<uint32, std::vector<uint32>> entryToDisplays;

    QueryResult result = WorldDatabase.Query(
        "SELECT entry, display_id, race, gender, class, skin, face, hair, haircolor, facialhair, guild_id, "
        "item_head, item_shoulders, item_body, item_chest, item_waist, item_legs, item_feet, item_wrists, "
        "item_hands, item_back, item_tabard FROM creature_display_preset");

    if (!result)
    {
        LOG_WARN("module.ascension_compat", ">> Table creature_display_preset is empty or missing.");

        std::unique_lock<std::shared_mutex> lock(_lock);
        _presets.clear();
        _entryToDisplays.clear();
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        CreatureDisplayPreset preset;
        preset.entry = fields[0].Get<uint32>();
        preset.display_id = fields[1].Get<uint32>();
        preset.race = fields[2].Get<uint8>();
        preset.gender = fields[3].Get<uint8>();
        preset.class_id = fields[4].Get<uint8>();
        preset.skin = fields[5].Get<uint8>();
        preset.face = fields[6].Get<uint8>();
        preset.hair = fields[7].Get<uint8>();
        preset.haircolor = fields[8].Get<uint8>();
        preset.facialhair = fields[9].Get<uint8>();
        preset.guild_id = fields[10].Get<uint32>();

        for (std::size_t i = 0; i < 11; ++i)
        {
            preset.items[i] = fields[11 + i].Get<uint32>();
        }

        uint64 key = MakeKey(preset.entry, preset.display_id);
        presets[key] = preset;
        entryToDisplays[preset.entry].push_back(preset.display_id);
    } while (result->NextRow());

    std::size_t const presetCount = presets.size();
    std::size_t const entryCount = entryToDisplays.size();

    {
        std::unique_lock<std::shared_mutex> lock(_lock);
        _presets = std::move(presets);
        _entryToDisplays = std::move(entryToDisplays);
    }

    LOG_INFO("module.ascension_compat", ">> Loaded {} creature display presets into cache across {} unique creature entries.",
        presetCount, entryCount);
}

CreatureDisplayPreset const* AscensionCreaturePresetMgr::FindPresetUnlocked(uint32 entry, uint32 displayId) const
{
    if (displayId != 0)
    {
        auto itr = _presets.find(MakeKey(entry, displayId));
        if (itr != _presets.end())
            return &itr->second;
    }

    auto listItr = _entryToDisplays.find(entry);
    if (listItr != _entryToDisplays.end() && !listItr->second.empty())
    {
        auto itr = _presets.find(MakeKey(entry, listItr->second.front()));
        if (itr != _presets.end())
            return &itr->second;
    }

    return nullptr;
}

CreatureDisplayPreset const* AscensionCreaturePresetMgr::FindPresetByGenderUnlocked(uint32 entry, uint8 gender) const
{
    auto listItr = _entryToDisplays.find(entry);
    if (listItr != _entryToDisplays.end())
    {
        for (uint32 disp : listItr->second)
        {
            auto itr = _presets.find(MakeKey(entry, disp));
            if (itr != _presets.end() && itr->second.gender == gender)
                return &itr->second;
        }

        if (!listItr->second.empty())
        {
            auto itr = _presets.find(MakeKey(entry, listItr->second.front()));
            if (itr != _presets.end())
                return &itr->second;
        }
    }
    return nullptr;
}

std::optional<CreatureDisplayPreset> AscensionCreaturePresetMgr::GetPreset(uint32 entry, uint32 displayId) const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    if (CreatureDisplayPreset const* preset = FindPresetUnlocked(entry, displayId))
        return *preset;
    return std::nullopt;
}

std::optional<CreatureDisplayPreset> AscensionCreaturePresetMgr::GetPresetByGender(uint32 entry, uint8 gender) const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    if (CreatureDisplayPreset const* preset = FindPresetByGenderUnlocked(entry, gender))
        return *preset;
    return std::nullopt;
}

bool AscensionCreaturePresetMgr::HasPreset(uint32 entry, uint32 displayId) const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    if (displayId != 0)
        return _presets.find(MakeKey(entry, displayId)) != _presets.end();
    return _entryToDisplays.find(entry) != _entryToDisplays.end();
}

std::size_t AscensionCreaturePresetMgr::GetPresetCount() const
{
    std::shared_lock<std::shared_mutex> lock(_lock);
    return _presets.size();
}

void AscensionCreaturePresetMgr::SetActivePresetOverride(ObjectGuid guid, uint32 entry, uint32 displayId)
{
    std::unique_lock<std::shared_mutex> lock(_lock);
    CreatureDisplayPreset const* preset = FindPresetUnlocked(entry, displayId);
    if (preset)
    {
        _activePresetOverrides[guid] = MakeKey(preset->entry, preset->display_id);
        _activeOverrideCount.store(uint32(_activePresetOverrides.size()), std::memory_order_release);
    }
}

void AscensionCreaturePresetMgr::ClearActivePresetOverride(ObjectGuid guid)
{
    // Appele pour chaque deconnexion et pour chaque creature qui quitte le monde : ne pas
    // prendre le verrou exclusif quand il n'y a rien a effacer.
    if (_activeOverrideCount.load(std::memory_order_acquire) == 0)
        return;

    std::unique_lock<std::shared_mutex> lock(_lock);
    _activePresetOverrides.erase(guid);
    _activeOverrideCount.store(uint32(_activePresetOverrides.size()), std::memory_order_release);
}

std::optional<CreatureDisplayPreset> AscensionCreaturePresetMgr::GetActivePresetOverride(ObjectGuid guid) const
{
    if (_activeOverrideCount.load(std::memory_order_acquire) == 0)
        return std::nullopt;

    std::shared_lock<std::shared_mutex> lock(_lock);
    auto itr = _activePresetOverrides.find(guid);
    if (itr != _activePresetOverrides.end())
    {
        auto presetItr = _presets.find(itr->second);
        if (presetItr != _presets.end())
            return presetItr->second;
    }
    return std::nullopt;
}

bool AscensionCreaturePresetMgr::HasActivePresetOverride(ObjectGuid guid) const
{
    // Ce test tourne pour chaque joueur et chaque bot a chaque tick : le portillon atomique
    // lui epargne la prise du verrou partage tant qu'aucun morphe n'est pose.
    if (_activeOverrideCount.load(std::memory_order_acquire) == 0)
        return false;

    std::shared_lock<std::shared_mutex> lock(_lock);
    auto itr = _activePresetOverrides.find(guid);
    return itr != _activePresetOverrides.end() && _presets.find(itr->second) != _presets.end();
}
