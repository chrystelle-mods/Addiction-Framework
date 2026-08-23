#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

namespace AddictionFramework
{
    // One registered blackout OUTCOME (DESIGN §11), loaded from a JSON file a scenario mod drops in
    // Data/SKSE/Plugins/AddictionFramework/BlackoutOutcomes/. Declarative, Acheron-style: it names a Quest
    // to Start() on a blackout, gated by which addiction categories it applies to, with a player-adjustable
    // chance (the Menu slider, §Phase D). No hard AF dependency — the scenario ships its own ESP + quest.
    struct BlackoutOutcome
    {
        std::string              name;         // unique key (the "name" field; falls back to the filename)
        std::string              description;  // shown in the Menu
        std::string              questRef;     // raw "Plugin.esp|0xID" (kept for logging)
        RE::FormID               quest = 0;    // resolved TESQuest to Start()
        std::vector<std::string> categories;   // lowercased category names it applies to; empty = any
        float                    defaultChance = 0.0f;  // author-suggested chance 0–100 (player overrides)
        bool                     valid = false;

        // --- optional eligibility GATES (evaluated at the instant of the blackout, in Select()) ---
        // A gate that's present must pass for this outcome to be offered; an absent gate is unrestricted;
        // different gate TYPES are AND'd (a "dungeon + slaver nearby" outcome needs both). A gated-out
        // outcome is dropped from the weighted pool BEFORE the draw — its chance mass falls to the other
        // eligible outcomes, then to the built-in default (so a blackout still happens, just via whatever
        // is eligible here).

        // Location gate: the player's current location — or any of its PARENT locations — must carry one of
        // these LocType keyword editorIDs (OR within the list). Populated from friendly aliases ("inn",
        // "city", …) expanded to their `LocType*` editorIDs, raw keyword editorIDs, or "Plugin.esp|0xID"
        // keyword refs resolved to their editorID. Empty (with matchWilderness=false) = any location.
        std::vector<std::string> locationKeywords;
        bool                     matchWilderness = false;  // "wilderness"/"outdoors": outdoors & not a habitation

        // Nearby-faction gate: at least `factionCount` LIVING actors in one of these factions must be within
        // `factionRadius` game-units of the player. Empty = no gate. FormIDs resolved at fire time.
        std::vector<RE::FormID>  factions;
        float                    factionRadius = 4096.0f;  // ~one cell
        std::int32_t             factionCount  = 1;
    };

    // Owns the blackout OUTCOME registry + the fade/skip finite-state machine. On a blackout the
    // AddictionManager calls OnBlackout(category); this weighted-selects a registered scenario (by the
    // player-set chances) or the built-in default, then drives the screen fade. Per-frame Update() steps
    // the fade. The chance table persists via co-save (routed through AddictionManager's callback).
    class BlackoutManager
    {
    public:
        static BlackoutManager* GetSingleton();

        // Resolve the base-game fade imods + scan BlackoutOutcomes/*.json. Call once at kDataLoaded, AFTER
        // Config::Load (so category names resolve for the Menu, though matching is by name at fire time).
        void Init();

        // A blackout fired for `a_category` (its group-spell FormID). Weighted-select an outcome; for a
        // SCENARIO, start its quest IMMEDIATELY and hand off (the quest owns the entire visual — its own
        // fade/teleport/wake-up). Only the built-in DEFAULT (no scenario) runs AF's own fade + time-skip.
        void OnBlackout(std::uint32_t a_category);

        // Is it safe to yank the player right now? A blackout can be reached with arbitrary third-party
        // consumables, so gate the trigger: false while in dialogue, mounted, in a kill-move, with movement
        // controls disabled (covers OStim/SexLab scenes + scripted sequences), or while a config-listed
        // transport-unsafe quest is running. DriveBlackout DEFERS (stays armed) when this is false.
        bool IsSafeToBlackout() const;

        // Config-supplied quests that make a blackout unsafe (teleport would break them) — resolved FormIDs.
        void SetExcludeQuests(std::vector<RE::FormID> a_quests);

        // Step the DEFAULT fade FSM (called every frame from AddictionManager::Tick with the real delta).
        void Update(float a_deltaSeconds);

        // --- Menu surface (§Phase D) ---
        const std::vector<BlackoutOutcome>& Outcomes() const { return _outcomes; }
        float GetChance(const std::string& a_name) const;         // player-set chance (default if unset)
        void  SetChance(const std::string& a_name, float a_pct);  // clamped 0–100; persisted
        // Stable pointer to a registered outcome's chance cell, for the Menu's ImGui slider to bind to
        // (all outcome names are seeded at Init, so this never inserts/rehashes → the pointer stays valid).
        float* ChancePtr(const std::string& a_name) { return &_chances[a_name]; }

        // --- co-save (chances), invoked from AddictionManager's serialization callbacks ---
        void WriteChances(SKSE::SerializationInterface* a_intfc) const;  // opens the 'BLKO' record
        void ReadChances(SKSE::SerializationInterface* a_intfc);          // payload after the tag is read
        void RevertChances();                                             // reset to authored defaults

        // Stop any in-progress fade + reset the FSM (used by AddictionManager::Revert on load).
        void ResetFade();

        static constexpr std::uint32_t kChancesRecord = 'BLKO';

    private:
        BlackoutManager() = default;

        // The DEFAULT outcome's fade FSM only. A scenario self-owns its whole visual (its own fade/teleport/
        // wake-up), so it never touches this — AF just Start()s its quest and is done.
        enum class FadePhase
        {
            kIdle,
            kFadingOut,  // fading to black; skip time + reveal once fully black
            kReveal,     // AF's fade-in animating; → Idle
        };

        void  FadeOut();                    // Trigger the fade-to-black-hold imod (~3s fade, then holds)
        void  Reveal();                     // stop the hold + trigger AF's fade-back-in
        void  AdvanceGameHours(float a_h);  // bump Calendar::gameDaysPassed (the default's time-skip)
        const BlackoutOutcome* Select(std::uint32_t a_category) const;  // weighted draw; nullptr = default

        // Eligibility gates checked in Select() alongside the category filter (see BlackoutOutcome). Both
        // return true when the outcome declares no gate of that kind, so an ungated outcome always passes.
        bool LocationEligible(const BlackoutOutcome& a_outcome) const;  // player's location vs. locationKeywords
        bool FactionNearby(const BlackoutOutcome& a_outcome) const;     // living faction member within radius

        std::vector<BlackoutOutcome>             _outcomes;
        std::unordered_map<std::string, float>   _chances;         // name → player-set chance (overrides default)
        std::vector<RE::FormID>                  _excludeQuests;   // transport-unsafe quests (safety gate)

        RE::TESImageSpaceModifier* _holdImod = nullptr;  // FadeToBlackHoldImod (Skyrim.esm 0x0F756E)
        RE::TESImageSpaceModifier* _backImod = nullptr;  // FadeToBlackBackImod (Skyrim.esm 0x0F756F)

        FadePhase _phase       = FadePhase::kIdle;
        float     _phaseTimer  = 0.0f;   // counts down (real seconds)
        float     _pendingSkip = 0.0f;   // game-hours to skip on the default outcome
    };
}
