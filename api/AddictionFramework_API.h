// AddictionFramework_API.h — public C++ integration header for Addiction Framework (AF).
//
// Drop this single, self-contained header into your own SKSE plugin project to drive AF's addiction
// categories at runtime from native C++: report uses, read levels/stages, and push rate modifiers —
// WITHOUT taking AddictionFramework.esm as a master, WITHOUT linking AF's DLL, and WITHOUT a Papyrus
// round-trip. It mirrors AF's string-keyed Papyrus `Addiction.*` API 1:1; both delegate to the same
// core (AddictionManager), so the C++ and Papyrus paths are interchangeable.
//
// Only dependency: <cstdint> and (for the GetProcAddress helper) <Windows.h>. No CommonLibSSE / SKSE
// headers required, so a plain SKSE plugin or even a non-CommonLib DLL can consume it. It IS safe to
// include alongside CommonLibSSE-NG.
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// ACQUIRING THE INTERFACE
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Two equivalent, load-order-independent, master-free mechanisms are provided. Both hand you the same
// singleton. Pick one:
//
//  (A) GetProcAddress — the simplest. One call, any time from your SKSEPlugin_Load onward (SKSE has
//      already loaded every plugin DLL into the process by then, so this is order-independent). If AF
//      isn't installed you get nullptr. Recommended for most consumers:
//
//          #include "AddictionFramework_API.h"
//          static AddictionFrameworkAPI::IVAddictionFramework1* g_af = nullptr;
//          // ... at your kPostLoad or kDataLoaded (or even in SKSEPlugin_Load):
//          g_af = AddictionFrameworkAPI::GetAPI();          // nullptr if AF absent / too old
//          if (g_af) g_af->NotifyUse("sex", 1.0f);
//
//  (B) SKSE messaging (InterfaceExchange) — if you prefer the push model. In your SKSEPlugin_Load,
//      register a listener FILTERED to AF's plugin name, then catch the delivery message. AF broadcasts
//      its interface to all listeners at its own kPostLoad. This is order-independent: SKSE finishes
//      every plugin's SKSEPlugin_Load (where you register the listener below) BEFORE it dispatches any
//      kPostLoad, so you always have a listener in place when AF broadcasts — whether AF loads before or
//      after you:
//
//          // in SKSEPlugin_Load, BEFORE messaging begins:
//          SKSE::GetMessagingInterface()->RegisterListener(
//              AddictionFrameworkAPI::kPluginName, AF_MessageListener);
//
//          void AF_MessageListener(SKSE::MessagingInterface::Message* m) {
//              if (m->type == AddictionFrameworkAPI::kMessage_DeliverInterface && m->data) {
//                  auto* msg = static_cast<AddictionFrameworkAPI::InterfaceMessage*>(m->data);
//                  if (msg->abiVersion >= AddictionFrameworkAPI::kInterfaceVersion1) g_af = msg->api;
//              }
//          }
//
//      (The header stays SKSE-free: it only DEFINES the message id + payload struct that both sides agree
//      on; you do the RegisterListener with your own SKSE messaging interface. For LAZY or late
//      acquisition — long after kPostLoad — just call GetAPI() from mechanism A; it works any time.)
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THREADING CONTRACT  (READ THIS)
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// EVERY method on IVAddictionFramework1 must be called from the MAIN GAME THREAD. They touch the
// AddictionManager singleton, the player, magic effects, and (for NotifyUse/AddLevel/Cure/…) apply or
// remove spells — none of which is thread-safe. Call them from an SKSE task (SKSE::GetTaskInterface()
// ->AddTask), a Papyrus native, a game-event sink that already runs on the main thread, or your
// per-frame update. Do NOT call from a worker thread, an async continuation, or a raw std::thread.
//
// SAFE NO-OPS: an unknown / typo'd / inactive category string is always a safe no-op — exactly like
// the Papyrus API. Queries return a neutral zero (level 0.0, stage 0, false); mutators do nothing.
// So you never need to pre-check IsCategoryActive() for safety (it's only there for UI/status logic).
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstdint>

namespace AddictionFrameworkAPI
{
    // ABI version of the interface below. Bumped only when the interface changes. New versions are
    // APPEND-ONLY (methods added at the end, never reordered/removed), so a pointer to a newer provider
    // is safe to use through an older header — call GetVersion() if you need a newer method to exist.
    enum InterfaceVersion : std::uint32_t
    {
        kInterfaceVersion1 = 1,
    };

    // AF's SKSE plugin name — the sender to filter on (mechanism B) and the DLL to look up (mechanism A).
    inline constexpr const char* kPluginName = "AddictionFramework";

    // The exported acquisition function's name (mechanism A, resolved via GetProcAddress).
    inline constexpr const char* kRequestAPIExport = "AF_RequestPluginAPI";

    class IVAddictionFramework1;

    // SKSE message id (mechanism B). Distinctive high value to avoid clashing with AF's own internals.
    // AF broadcasts one of these to all listeners at its kPostLoad; the message's `data` is an
    // InterfaceMessage*. (There is no consumer→AF request id: AF's own listener only hears SKSE-core
    // messages, so it can't service arbitrary requesters — use GetAPI() for late/lazy acquisition.)
    enum : std::uint32_t
    {
        kMessage_DeliverInterface = 0xAF000001,  // AF → you: data = InterfaceMessage*
    };

    // Payload of a kMessage_DeliverInterface message (the message's `data` points to one of these).
    struct InterfaceMessage
    {
        std::uint32_t          abiVersion;  // == api->GetVersion()
        IVAddictionFramework1* api;         // the AF singleton (stable for the process lifetime)
    };

    // ───────────────────────────────────────────────────────────────────────────────────────────────
    // The interface. A stable, pure-virtual ABI over AF's string-keyed core. Categories are addressed
    // by their config name (a C-string: "skooma", "alcohol", "sex", "gestation", …), matching AF's
    // master-free, string-first design. All calls are MAIN-THREAD ONLY (see the contract above).
    // ───────────────────────────────────────────────────────────────────────────────────────────────
    class IVAddictionFramework1
    {
    public:
        // The ABI version this instance implements (one of InterfaceVersion). Guard against mismatch.
        [[nodiscard]] virtual std::uint32_t GetVersion() = 0;

        // Relay a USE of `a_category` with the given potency `a_amount` (the same units as an item's
        // ENIT AddictionChance, or any scripted magnitude). This is the key entry point: it drives the
        // full tolerance/gain/withdrawal model (timestamps the use, advances the level, feeds the acute
        // + blackout windows). Returns the category's new level (0–100). Unknown category → 0, no-op.
        virtual float NotifyUse(const char* a_category, float a_amount) = 0;

        // Current chronic level 0–100 (settles lazy decay on read). Unknown category → 0.
        [[nodiscard]] virtual float GetLevel(const char* a_category) = 0;

        // Current stage: 0 = clean, 1 = addicted-satisfied, 2 = withdrawal. Unknown category → 0.
        [[nodiscard]] virtual std::int32_t GetStage(const char* a_category) = 0;

        // True iff the addiction latch is engaged (stage 1 or 2). Unknown category → false.
        [[nodiscard]] virtual bool IsAddicted(const char* a_category) = 0;

        // Nudge the level directly by `a_amount` (may be negative), WITHOUT a use-timestamp — so it
        // moves tolerance/withdrawal state but not the "last use" clock or the acute/blackout windows.
        // Use NotifyUse for a real consumption; use this for scripted adjustments. Unknown → no-op.
        virtual void AddLevel(const char* a_category, float a_amount) = 0;

        // Accumulate a script-owned rate modifier for (category, kind). `a_kind` is "gain", "decay",
        // or "withdrawal" (case-insensitive). Percent is summed with any keyword-derived modifiers into
        // one aggregate: gain 100 = fully block addiction gain, decay 100 = +100% decay speed,
        // withdrawal 100 = fully mute the withdrawal effect; negative = potentiate. Persists in AF's
        // co-save. Bad kind or unknown category → no-op. (See ClearModifier to reset.)
        virtual void AddModifier(const char* a_category, const char* a_kind, float a_percent) = 0;

        // Hard-reset the script-owned modifier for (category, kind) back to 0. Bad kind / unknown → no-op.
        virtual void ClearModifier(const char* a_category, const char* a_kind) = 0;

        // Read the EFFECTIVE modifier aggregate for (category, kind) — keyword sources + script offset.
        // Bad kind or unknown category → 0.
        [[nodiscard]] virtual float GetModifier(const char* a_category, const char* a_kind) = 0;

        // Cure one category: level → 0, effects off, fires AF_OnCured. Unknown → no-op.
        virtual void Cure(const char* a_category) = 0;

        // "Cure hangover": clear the withdrawal clock (reset time-since-last-use) but KEEP the level, so
        // withdrawal restarts from satisfied without losing tolerance. Unknown → no-op.
        virtual void ResetWithdrawalTimer(const char* a_category) = 0;

        // Is this category enabled by a loaded config (activated), as opposed to an inert roster slot or
        // an unknown name? For UI/status logic only — you never need this for call safety. Unknown → false.
        [[nodiscard]] virtual bool IsCategoryActive(const char* a_category) = 0;

        // Acute axis (short-term "how affected right now"): true if any acute status is active for the
        // category (trailing-window potency ≥ threshold). Unknown → false.
        [[nodiscard]] virtual bool IsInAcuteStatus(const char* a_category) = 0;

        // As above, but true only when the active acute effect carries AF's inebriation keyword
        // (Drunk/High/Stoned — NOT Wired). Unknown → false.
        [[nodiscard]] virtual bool IsIntoxicated(const char* a_category) = 0;

        // The current trailing-window potency sum for the acute axis. Unknown → 0.
        [[nodiscard]] virtual float GetAcuteLevel(const char* a_category) = 0;

        // Cure EVERY category and strip all AF-applied spells off the player (the panic/cleanup button).
        virtual void CureAll() = 0;

        // (v1 intentionally omits an "enumerate active categories" call: returning a container across the
        // DLL boundary isn't ABI-safe. Track the category names you care about on your side, or query
        // IsCategoryActive/IsAddicted per name.)
    };
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Mechanism (A) helper: acquire the interface via GetProcAddress. Header-only, needs only <Windows.h>.
// Returns nullptr if AF isn't loaded or doesn't support the requested ABI version. Caches the result.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
#ifndef ADDICTIONFRAMEWORK_API_NO_GETPROC

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
        #define ADDICTIONFRAMEWORK_API_DEFINED_WLM
    #endif
    #include <Windows.h>
    #ifdef ADDICTIONFRAMEWORK_API_DEFINED_WLM
        #undef WIN32_LEAN_AND_MEAN
        #undef ADDICTIONFRAMEWORK_API_DEFINED_WLM
    #endif

namespace AddictionFrameworkAPI
{
    // The exported signature: extern "C" void* AF_RequestPluginAPI(std::uint32_t abiVersion).
    using RequestAPIFn = void* (*)(std::uint32_t);

    // Acquire AF's interface. Safe to call repeatedly (the lookup is cached). nullptr = AF not present
    // or the requested version is newer than AF provides.
    [[nodiscard]] inline IVAddictionFramework1* GetAPI(std::uint32_t a_abiVersion = kInterfaceVersion1)
    {
        static IVAddictionFramework1* cached = [a_abiVersion]() -> IVAddictionFramework1* {
            const HMODULE handle = GetModuleHandleA("AddictionFramework.dll");
            if (!handle) {
                return nullptr;  // AF not installed / not loaded
            }
            const auto request =
                reinterpret_cast<RequestAPIFn>(GetProcAddress(handle, kRequestAPIExport));
            if (!request) {
                return nullptr;  // AF too old to export the C++ API
            }
            return static_cast<IVAddictionFramework1*>(request(a_abiVersion));
        }();
        return cached;
    }
}

#endif  // ADDICTIONFRAMEWORK_API_NO_GETPROC
