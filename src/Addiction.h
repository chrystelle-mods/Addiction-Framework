#pragma once

#include "Display.h"  // SubEffect + EffectManager

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SKSE
{
    class SerializationInterface;
}

namespace AddictionFramework
{
    // One consequence effect (addiction or withdrawal). `spell` is a CONTAINER spell in AddictionFramework
    // .esm holding a pool of blank PVM+Recover+Detrimental effects; `effects` is the list of actor values
    // (+ per-AV max) that AF stamps onto those blanks at load (Mechanism B). Empty/invalid = no effect
    // (the category's level still tracks). Magnitude at runtime = `max * (level/100)` per sub-effect.
    struct EffectDef
    {
        RE::FormID             spell       = 0;      // container spell (0 = none)
        std::vector<SubEffect> effects;              // AVs to stamp onto the container's blanks
        bool                   detrimental = true;   // penalty (engine Detrimental flag)
        bool                   valid       = false;  // false → skip (no effects / "none" / bad AV / no container)
    };

    // Per-category tuning, built from JSON config (Config::Load), keyed by the group-spell FormID.
    struct Category
    {
        // --- curve (addictiveness) ---
        float gain               = 9.0f;   // usage points added per unit potency at level 0
        float gainFalloff        = 1.5f;   // diminishing-returns exponent (higher = flattens sooner)
        float decay              = 0.4f;   // level lost per game-hour
        float addictionThreshold = 15.0f;  // level at/above which the addicted latch engages
        float toleranceHours     = 20.0f;  // hours since last use before withdrawal onsets (while addicted)

        // --- consequence effects (magnitude scales with level; §5.1) ---
        EffectDef addiction;   // active while addicted
        EffectDef withdrawal;  // active while in withdrawal

        // --- data-out (§9): per-category globals AF drives each tick so any CK/OAR/dialogue condition can
        // read state via GetGlobalValue (uniform across every condition context; encodes stage directly).
        // 0 = no global for that channel.
        RE::FormID levelGlobal = 0;  // GlobalFloat  ← current level (0–100)
        RE::FormID stageGlobal = 0;  // GlobalShort  ← current stage (0 clean / 1 satisfied / 2 withdrawal)

        // --- acute status (§7, the acute axis): a visible marker ability AF toggles when the trailing-window
        // potency SUM crosses a threshold. Orthogonal to the level. The config SELECTS which effect to apply
        // by a stable string key (a shared named effect — Drunk/High/Stoned/Wired — or a bring-your-own
        // spell); the chosen form carries its own name + keywords (AF_Intoxicated on the inebriation set,
        // AF_AcuteStatus on all). AF is a pure applicator — it only Add/RemoveSpells the effect, never writes
        // the MGEF, so a modder's override is never clobbered.
        RE::FormID acuteSpell        = 0;      // the selected effect ability (0 = none)
        float      acuteThreshold    = 0.0f;   // cumulative potency within the window that turns it on
        float      acuteWindowHours  = 0.0f;   // trailing game-time window
        bool       acuteEnabled      = false;  // a config supplied a valid acute block + a resolved effect
        bool       acuteIsIntoxicated = false; // the chosen effect carries AF_Intoxicated (drives IsIntoxicated)

        // --- blackout (§11): a higher-threshold tier on the SAME trailing-window potency sum. Unlike the
        // acute latch, this is EDGE-triggered — crossing blackoutThreshold fires ONE blackout per bender
        // (arm/re-arm: it re-arms only after the window drains below threshold, so you sober off and chug
        // again to re-trigger), which clears the acute window (so Drunk drops — you pass out and sleep it off)
        // and hands off to the outcome selector (a registered scenario quest, or the built-in default
        // fade-to-black + time-skip). There is no blackout effect form — the outcome owns all the visuals.
        float blackoutThreshold   = 0.0f;   // cumulative window potency that triggers a blackout
        float blackoutWindowHours = 0.0f;   // trailing game-time window (usually == acuteWindowHours)
        bool  blackoutEnabled     = false;  // a config supplied a valid blackout block
    };

    enum class Stage : std::int32_t
    {
        kClean      = 0,  // below the addicted threshold
        kSatisfied  = 1,  // addicted, used recently (no withdrawal)
        kWithdrawal = 2,  // addicted, past tolerance since last use
    };

    // The three ongoing rate modifiers (§7). Both the suffix-matched keyword reads and the direct script
    // API feed one summed-percent aggregate per (category, kind): gain 100 = fully blocked, decay 100 =
    // +100% decay speed, withdrawal 100 = fully muted; negative = a potentiator.
    enum class ModKind : std::int32_t
    {
        kGain       = 0,  // reduce/boost addiction gain per use   (keyword suffix AF_ModGain)
        kDecay      = 1,  // hasten/slow level decay                (keyword suffix AF_HastenDecay)
        kWithdrawal = 2,  // suppress/amplify the withdrawal effect (keyword suffix AF_SuppressWithdrawal)
    };

    // Player-only addiction state, keyed by category (the group-spell FormID at runtime). Decay is
    // LAZY (settled on access from elapsed game-hours). Consequences are applied by the EffectManager as
    // one visible PVM+Detrimental ability per effect whose magnitude IS the penalty (DESIGN §5.1),
    // refreshed on a throttled tick and immediately on use. Persisted via SKSE co-save.
    class AddictionManager
    {
    public:
        static AddictionManager* GetSingleton();

        // Register a category (from JSON config) under its group-spell FormID. Last call wins.
        void RegisterCategory(std::uint32_t a_groupFormID, const Category& a_category);
        // Register the category's config NAME <-> group FormID. The string is the sole external identifier
        // (Papyrus API + mod-event strArg); the group Form stays internal (co-save key, ENIT, sender).
        void RegisterCategoryName(const std::string& a_name, std::uint32_t a_groupFormID);
        std::uint32_t KeyForName(const std::string& a_name) const;   // 0 if unknown
        std::string   NameForKey(std::uint32_t a_groupFormID) const;  // "" if unknown

        // Register a use of `potency` (the item's AddictionChance) toward a category. Returns new level.
        float RegisterUse(std::uint32_t a_category, float a_potency);

        float GetLevel(std::uint32_t a_category);
        Stage GetStage(std::uint32_t a_category);
        bool  IsAddicted(std::uint32_t a_category);
        bool  IsInAcuteStatus(std::uint32_t a_category);    // any acute status active (potency sum ≥ threshold)
        bool  IsIntoxicated(std::uint32_t a_category);      // acute active AND the effect carries AF_Intoxicated
        float GetAcuteLevel(std::uint32_t a_category);       // the current trailing-window potency sum

        // --- public API surface (§9), driven by the Papyrus `Addiction` natives ---
        float NotifyUse(std::uint32_t a_category, float a_amount);  // trigger-agnostic relay = RegisterUse
        void  AddLevel(std::uint32_t a_category, float a_amount);   // raw level nudge (no use-timestamp)
        void  Cure(std::uint32_t a_category);                       // level→0, effects off, fire AF_OnCured
        void  CureAll();                                            // cure EVERY category + strip all AF spells
        void  ResetWithdrawalTimer(std::uint32_t a_category);       // "Cure Hangover": clear clock, keep level
        std::vector<std::uint32_t> GetActiveAddictions();           // group FormIDs currently addicted

        // --- direct rate-modifier API (§7): blunt, script-owned (Actor.ModActorValue-style, no TTL/keys).
        // Accumulate a % offset for a category+kind; ClearModifier hard-resets it; GetModifier reads the
        // effective aggregate (keyword sources + script offset). Persisted via co-save. Key 0 = no-op.
        void  AddModifier(std::uint32_t a_category, ModKind a_kind, float a_percent);
        void  ClearModifier(std::uint32_t a_category, ModKind a_kind);
        float GetModifier(std::uint32_t a_category, ModKind a_kind) const;

        // Frame tick (real seconds). Throttled internally; refreshes each category's applied effect.
        void  Tick(float a_deltaSeconds);

        // --- SKSE co-save serialization (registered in main.cpp) ---
        void Save(SKSE::SerializationInterface* a_intfc);
        void Load(SKSE::SerializationInterface* a_intfc);
        void Revert();  // clear runtime state (new game / pre-load)

        static constexpr std::uint32_t kSerializationID      = 'AFCT';  // unique record tag
        static constexpr std::uint32_t kSerializationType    = 'STAT';  // states record
        static constexpr std::uint32_t kModifierType         = 'SMOD';  // script rate-modifier offsets
        static constexpr std::uint32_t kSerializationVersion = 4;  // v4: dropped lastBlackoutHour (no cooldown)

    private:
        struct State
        {
            float level       = 0.0f;
            float lastUpdated = 0.0f;  // game-hours at last decay settle
            float lastUseHour = 0.0f;  // game-hours at last use
            Stage lastStage   = Stage::kClean;  // last stage we fired an event for
            // Hysteresis latch (DESIGN §4.3): set when level crosses addictionThreshold upward, cleared
            // only when level decays to 0 — so cold turkey keeps both effects on the whole way down.
            bool  addicted    = false;

            // Acute axis: recent (game-hour, potency) uses for the trailing-window sum, and the current
            // status latch (edge-detected for AF_OnAcuteStart/AF_OnAcuteEnd). Only tracked for acute-enabled
            // categories; pruned to the window.
            std::vector<std::pair<float, float>> recentUses;
            bool                                 inAcuteStatus = false;

            // Blackout tier (§11): edge-trigger latch. `blackoutArmed` clears on a fire and re-arms when the
            // window sum drops back below blackoutThreshold — one blackout per over-threshold bender.
            bool  blackoutArmed = true;
        };

        // Script-owned rate-modifier offsets (§7), summed-percent, per category. Fed by AddModifier;
        // added to the live keyword-derived percents to form the effective aggregate.
        struct Modifiers
        {
            float gain       = 0.0f;
            float decay      = 0.0f;
            float withdrawal = 0.0f;
        };

        float           Now() const;                                          // game-hours + debug offset
        void            SettleDecay(std::uint32_t a_key, State& a_st, const Category& a_cat, float a_now);
        Stage           ComputeStage(const State& a_st, const Category& a_cat, float a_now) const;  // no settle
        // Settle → compute stage → drive both consequence effects → drive data-out globals → fire stage-
        // change event on transition.
        void            UpdateState(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now);
        void            DriveGlobals(const Category& a_cat, const State& a_st, Stage a_stage) const;
        // Acute axis: sum recent potencies in the trailing window; toggle the (possibly shared) marker
        // ability + fire the acute edge event.
        float           RecentPotency(const State& a_st, float a_windowHours, float a_now) const;
        void            DriveAcute(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now);
        // Blackout tier (§11): edge-detect the higher threshold; on a fire clear the acute window and hand
        // off to the BlackoutManager outcome selector. Runs just BEFORE DriveAcute so a fire's window-clear
        // drops the Drunk status in the same pass.
        void            DriveBlackout(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now);
        void            FireEvent(const char* a_eventName, std::uint32_t a_category, float a_numArg) const;
        const Category* FindCategory(std::uint32_t a_category) const;         // nullptr if unknown

        // Modifier aggregation (§7): effective = live keyword percent + script offset. Key 0 → 0.
        float           ModifierPercent(std::uint32_t a_key, ModKind a_kind) const;
        float           KeywordModifierPercent(std::uint32_t a_key, ModKind a_kind) const;  // scans active effects
        float           ScriptModifierPercent(std::uint32_t a_key, ModKind a_kind) const;

        std::unordered_map<std::uint32_t, Category>    _categories;    // group-spell FormID → tuning
        std::unordered_map<std::string, std::uint32_t> _categoryNames; // config name → group FormID
        std::unordered_map<std::uint32_t, std::string> _keyNames;      // group FormID → config name (reverse)
        std::unordered_map<std::uint32_t, State>       _states;        // group-spell FormID → player state
        std::unordered_map<std::uint32_t, Modifiers>   _scriptModifiers;  // group-spell FormID → script offsets
        float                                           _tickAccum = 0.0f;
    };
}
