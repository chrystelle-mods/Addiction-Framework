#pragma once

namespace AddictionFramework
{
    // JSON config loader (DESIGN §3/§7). Scans Data/SKSE/Plugins/AddictionFramework/*.json, merges
    // last-loaded-wins, resolves forms + validates actor-value strings, registers each category with the
    // AddictionManager (keyed by its group-spell FormID), and stamps the ENIT addictionItem/addictionChance
    // onto each assigned consumable's AlchemyItem in memory (AF as an internal mini-SkyPatcher). Call once
    // at kDataLoaded (after all plugins are loaded).
    class Config
    {
    public:
        static void Load();
    };
}
