#include "Blackout.h"

#include "Addiction.h"  // AddictionManager (category-name lookup) + kSerializationVersion

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    // Scenario registration files live beside AF's config packs, in their own subfolder so third-party
    // scenario mods don't mingle with AF's own JSON. Under MO2's VFS this is the merged virtual Data.
    const fs::path kOutcomeDir =
        fs::path("Data") / "SKSE" / "Plugins" / "AddictionFramework" / "BlackoutOutcomes";

    // Base-game fade imods (resolved by FormID — EditorID lookup needs a runtime cache we can't assume).
    constexpr RE::FormID    kFadeHoldID = 0x0F756E;  // FadeToBlackHoldImod (fade out, hold black)
    constexpr RE::FormID    kFadeBackID = 0x0F756F;  // FadeToBlackBackImod (fade back in)
    constexpr std::string_view kSkyrimEsm = "Skyrim.esm"sv;

    // Fade tuning (real seconds) + the default time-skip (game-hours). FadeToBlackHoldImod fades over ~3s;
    // kFadeOutHold gives it that long (+ a beat) so the OUTCOME fires under FULL black, not mid-fade — that
    // "teleport before the screen is black" was the whole reason the blackout felt instant.
    constexpr float kFadeOutHoldSeconds    = 3.5f;   // fade-to-black before the default's time-skip
    constexpr float kFadeInSeconds         = 1.5f;   // AF's own fade-in window before Idle (default path)
    constexpr float kDefaultSkipHours      = 6.0f;   // "you were out for ~6 hours"

    std::string ToLower(std::string a_s)
    {
        std::transform(a_s.begin(), a_s.end(), a_s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a_s;
    }

    // Resolve a "Plugin.esp|0xFormID" ref to a form of type T (blackout outcomes only ever use this form).
    template <class T>
    T* ResolveForm(const std::string& a_ref)
    {
        const auto bar = a_ref.find('|');
        if (bar == std::string::npos) {
            return nullptr;
        }
        const std::string plugin  = a_ref.substr(0, bar);
        const std::string idStr   = a_ref.substr(bar + 1);
        const auto        localID = static_cast<RE::FormID>(std::strtoul(idStr.c_str(), nullptr, 16));
        auto*             dh      = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<T>(localID, plugin) : nullptr;
    }

    float RandomUnit()  // [0, 1)
    {
        static std::mt19937                          rng{ std::random_device{}() };
        static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(rng);
    }

    // Friendly location aliases → vanilla LocType keyword editorID(s). A modder can also pass a raw
    // editorID ("LocTypeInn") or a "Plugin.esp|0xID" keyword ref, so this only needs the common cases.
    // Matching is by editorID string (BGSKeywordForm::HasKeywordString), so an alias that maps to a
    // keyword the load order lacks simply never matches — harmless.
    const std::unordered_map<std::string, std::vector<std::string>> kLocationAliases = {
        { "inn",        { "LocTypeInn" } },
        { "city",       { "LocTypeCity" } },
        { "town",       { "LocTypeTown" } },
        { "dwelling",   { "LocTypeDwelling" } },
        { "habitation", { "LocTypeHabitation" } },
        { "settlement", { "LocTypeHabitation" } },
        { "dungeon",    { "LocTypeDungeon" } },
        { "store",      { "LocTypeStore" } },
        { "shop",       { "LocTypeStore" } },
        { "temple",     { "LocTypeTemple" } },
        { "fort",       { "LocTypeMilitaryFort" } },
        { "camp",       { "LocTypeMilitaryCamp" } },
        { "farm",       { "LocTypeFarm" } },
        { "castle",     { "LocTypeCastle" } },
    };

    constexpr std::string_view kHabitationKeyword = "LocTypeHabitation"sv;
}

namespace AddictionFramework
{
    BlackoutManager* BlackoutManager::GetSingleton()
    {
        static BlackoutManager singleton;
        return &singleton;
    }

    void BlackoutManager::Init()
    {
        // Resolve the fade imods once.
        _holdImod = ResolveForm<RE::TESImageSpaceModifier>(std::string(kSkyrimEsm) + "|0xF756E");
        _backImod = ResolveForm<RE::TESImageSpaceModifier>(std::string(kSkyrimEsm) + "|0xF756F");
        if (!_holdImod || !_backImod) {
            logger::warn("Blackout: fade imods did not resolve (hold={}, back={}) — the default blackout will "
                         "skip time without a visible fade.",
                         _holdImod != nullptr, _backImod != nullptr);
        }

        _outcomes.clear();

        std::error_code ec;
        if (!fs::exists(kOutcomeDir, ec)) {
            logger::info("Blackout: no outcomes dir ({}) — only the built-in default is available.",
                         kOutcomeDir.string());
            return;
        }

        std::vector<fs::path> files;
        for (auto& entry : fs::directory_iterator(kOutcomeDir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (auto& f : files) {
            std::ifstream in(f);
            if (!in) {
                logger::warn("Blackout: could not open {}", f.string());
                continue;
            }
            json j;
            try {
                in >> j;
            } catch (const std::exception& e) {
                logger::error("Blackout: JSON parse error in {}: {}", f.filename().string(), e.what());
                continue;
            }

            BlackoutOutcome o;
            o.name        = j.value("name", f.stem().string());  // filename is the fallback key
            o.description = j.value("description", std::string{});
            o.questRef    = j.value("quest", std::string{});
            o.defaultChance = std::clamp(j.value("chance", 0.0f), 0.0f, 100.0f);

            // categories: a string ("any"/"alcohol"), a list, or absent (= any). "any" anywhere = any.
            if (auto cit = j.find("categories"); cit != j.end()) {
                auto addOne = [&](const std::string& s) {
                    const std::string low = ToLower(s);
                    if (low == "any" || low == "*") {
                        o.categories.clear();
                        return false;  // signal "any" — stop collecting
                    }
                    o.categories.push_back(low);
                    return true;
                };
                if (cit->is_string()) {
                    addOne(cit->get<std::string>());
                } else if (cit->is_array()) {
                    for (auto& e : *cit) {
                        if (e.is_string() && !addOne(e.get<std::string>())) {
                            o.categories.clear();
                            break;
                        }
                    }
                }
            }

            // locations: friendly alias / raw keyword editorID / "Plugin.esp|0xID" / "wilderness"; string or list.
            if (auto lit = j.find("locations"); lit != j.end()) {
                auto addLoc = [&](const std::string& raw) {
                    const std::string low = ToLower(raw);
                    if (low == "any" || low == "*") {
                        o.locationKeywords.clear();  // explicit "any" = no location gate
                        o.matchWilderness = false;
                        return;
                    }
                    if (low == "wilderness" || low == "outdoors") {
                        o.matchWilderness = true;
                        return;
                    }
                    if (auto ait = kLocationAliases.find(low); ait != kLocationAliases.end()) {
                        for (auto& ed : ait->second) {
                            o.locationKeywords.push_back(ed);
                        }
                        return;
                    }
                    if (raw.find('|') != std::string::npos) {  // "Plugin.esp|0xID" keyword ref
                        if (auto* kw = ResolveForm<RE::BGSKeyword>(raw)) {
                            if (const char* ed = kw->GetFormEditorID(); ed && *ed) {
                                o.locationKeywords.emplace_back(ed);
                            } else {
                                logger::warn("Blackout: outcome '{}' location keyword '{}' has no editorID at "
                                             "runtime — ignoring that entry.", o.name, raw);
                            }
                        } else {
                            logger::warn("Blackout: outcome '{}' location keyword '{}' did not resolve — "
                                         "ignoring that entry.", o.name, raw);
                        }
                        return;
                    }
                    o.locationKeywords.push_back(raw);  // passthrough: a literal keyword editorID
                };
                if (lit->is_string()) {
                    addLoc(lit->get<std::string>());
                } else if (lit->is_array()) {
                    for (auto& e : *lit) {
                        if (e.is_string()) {
                            addLoc(e.get<std::string>());
                        }
                    }
                }
            }

            // nearbyFaction: a faction ref ("Plugin.esp|0xID") or list, OR an object
            // { faction: ref|[refs], radius?: units, count?: n }. If specified but nothing resolves, the
            // gate can never pass, so the outcome is dropped (fail-closed, like a missing quest).
            bool factionGateRequested = false;
            if (auto fit = j.find("nearbyFaction"); fit != j.end()) {
                factionGateRequested = true;
                auto addFac = [&](const std::string& ref) {
                    if (auto* fac = ResolveForm<RE::TESFaction>(ref)) {
                        o.factions.push_back(fac->GetFormID());
                    } else {
                        logger::warn("Blackout: outcome '{}' nearbyFaction '{}' did not resolve (is its "
                                     "plugin installed?).", o.name, ref);
                    }
                };
                auto addFacNode = [&](const json& node) {
                    if (node.is_string()) {
                        addFac(node.get<std::string>());
                    } else if (node.is_array()) {
                        for (auto& e : node) {
                            if (e.is_string()) {
                                addFac(e.get<std::string>());
                            }
                        }
                    }
                };
                if (fit->is_object()) {
                    if (auto fac = fit->find("faction"); fac != fit->end()) {
                        addFacNode(*fac);
                    }
                    o.factionRadius = std::max(0.0f, fit->value("radius", o.factionRadius));
                    o.factionCount  = std::max(1, fit->value("count", o.factionCount));
                } else {
                    addFacNode(*fit);
                }
            }

            if (o.questRef.empty()) {
                logger::error("Blackout: outcome '{}' ({}) has no 'quest' — skipping.", o.name,
                              f.filename().string());
                continue;
            }
            if (factionGateRequested && o.factions.empty()) {
                logger::error("Blackout: outcome '{}' declares a nearbyFaction gate but none of its factions "
                              "resolved — skipping (the gate could never be satisfied).", o.name);
                continue;
            }
            if (auto* q = ResolveForm<RE::TESQuest>(o.questRef)) {
                o.quest = q->GetFormID();
                o.valid = true;
            } else {
                logger::error("Blackout: outcome '{}' quest '{}' did not resolve (is its plugin installed?) — "
                              "skipping.",
                              o.name, o.questRef);
                continue;
            }

            _chances.emplace(o.name, o.defaultChance);  // seed the player-chance table (co-save overrides)
            logger::info("Blackout: registered outcome '{}' quest={:08X} chance={:.0f} categories={} "
                         "locations={}{} nearbyFaction={}", o.name, o.quest, o.defaultChance,
                         o.categories.empty() ? 1 : (int)o.categories.size(),
                         (int)o.locationKeywords.size(), o.matchWilderness ? "+wild" : "",
                         o.factions.empty() ? 0 : (int)o.factions.size());
            _outcomes.push_back(std::move(o));
        }
        logger::info("Blackout: {} outcome(s) registered.", _outcomes.size());
    }

    // --- selection ------------------------------------------------------------------------------------

    const BlackoutOutcome* BlackoutManager::Select(std::uint32_t a_category) const
    {
        const std::string catName = AddictionManager::GetSingleton()->NameForKey(a_category);  // lowercase

        // Gather participating outcomes with a positive current chance, and total them.
        std::vector<const BlackoutOutcome*> pool;
        float                               sum = 0.0f;
        for (auto& o : _outcomes) {
            if (!o.valid) {
                continue;
            }
            if (!o.categories.empty() &&
                std::find(o.categories.begin(), o.categories.end(), catName) == o.categories.end()) {
                continue;  // category-filtered out
            }
            if (!LocationEligible(o) || !FactionNearby(o)) {
                continue;  // location / nearby-faction gate not satisfied at the current spot
            }
            const float chance = GetChance(o.name);
            if (chance <= 0.0f) {
                continue;
            }
            pool.push_back(&o);
            sum += chance;
        }
        if (pool.empty()) {
            return nullptr;  // nothing registered/enabled → the default
        }

        // sum <= 100: the leftover (100 - sum) is the DEFAULT slice. sum > 100: normalize (roll over [0,sum),
        // so a scenario is always chosen — no default slice). This yields "A=100,B=100 → 50/50".
        const float denom = (sum <= 100.0f) ? 100.0f : sum;
        const float roll  = RandomUnit() * denom;
        float       acc   = 0.0f;
        for (auto* o : pool) {
            acc += GetChance(o->name);
            if (roll < acc) {
                return o;
            }
        }
        return nullptr;  // roll landed in the [sum, 100) default remainder
    }

    bool BlackoutManager::LocationEligible(const BlackoutOutcome& a_outcome) const
    {
        if (a_outcome.locationKeywords.empty() && !a_outcome.matchWilderness) {
            return true;  // no location gate
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* loc    = player ? player->GetCurrentLocation() : nullptr;

        // Walk the current location + its parent-location chain (guarded against a malformed cycle), so a
        // "Bannered Mare → Whiterun" nest satisfies both LocTypeInn and LocTypeCity. Track habitation for
        // the wilderness test.
        bool inHabitation = false;
        int  guard        = 0;
        for (RE::BGSLocation* cur = loc; cur && guard < 16; cur = cur->parentLoc, ++guard) {
            for (auto& ed : a_outcome.locationKeywords) {
                if (cur->HasKeywordString(ed)) {
                    return true;
                }
            }
            if (cur->HasKeywordString(kHabitationKeyword)) {
                inHabitation = true;
            }
        }

        if (a_outcome.matchWilderness && !inHabitation) {
            // "Outdoors and not a settlement." Require an exterior cell so a (non-habitation) dungeon
            // interior doesn't read as wilderness; a null location in the open world still counts.
            bool exterior = true;
            if (auto* cell = player ? player->GetParentCell() : nullptr) {
                exterior = !cell->IsInteriorCell();
            }
            if (exterior) {
                return true;
            }
        }
        return false;
    }

    bool BlackoutManager::FactionNearby(const BlackoutOutcome& a_outcome) const
    {
        if (a_outcome.factions.empty()) {
            return true;  // no faction gate
        }
        auto* player    = RE::PlayerCharacter::GetSingleton();
        auto* procLists = RE::ProcessLists::GetSingleton();
        if (!player || !procLists) {
            return false;
        }

        std::vector<RE::TESFaction*> facs;
        facs.reserve(a_outcome.factions.size());
        for (auto id : a_outcome.factions) {
            if (auto* f = RE::TESForm::LookupByID<RE::TESFaction>(id)) {
                facs.push_back(f);
            }
        }
        if (facs.empty()) {
            return false;
        }

        // Scan the fully-processed (nearby, simulated) actors — exactly the "nearby loaded NPCs" set — and
        // count living faction members inside the radius. Cheap enough to run once at a (rare) blackout.
        std::int32_t found = 0;
        for (auto& handle : procLists->highActorHandles) {
            auto       actorPtr = handle.get();
            RE::Actor* actor    = actorPtr.get();
            if (!actor || actor == player || actor->IsDead()) {
                continue;
            }
            if (player->GetDistance(actor) > a_outcome.factionRadius) {
                continue;
            }
            for (auto* f : facs) {
                if (actor->IsInFaction(f)) {
                    if (++found >= a_outcome.factionCount) {
                        return true;
                    }
                    break;
                }
            }
        }
        return false;
    }

    void BlackoutManager::OnBlackout(std::uint32_t a_category)
    {
        if (const BlackoutOutcome* chosen = Select(a_category)) {
            // SCENARIO: hand off IMMEDIATELY at the moment of the blackout. AF just Start()s the quest; its
            // start-up-stage fragment owns the entire visual (its own fade-to-black, teleport, and wake-up).
            // No AF fade, no reveal contract — maximum flexibility for the scenario author.
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(chosen->quest);
            if (quest && quest->Start()) {
                logger::info("Blackout: handed off to scenario '{}' (quest {:08X}).", chosen->name, chosen->quest);
                return;
            }
            logger::info("Blackout: scenario '{}' quest {:08X} did not start (missing / used-up Run-Once) — "
                         "using the default.",
                         chosen->name, chosen->quest);
        }

        // DEFAULT (no scenario, or the scenario couldn't start): AF's own best-effort imod fade + time-skip.
        // Guard against overlapping with an in-progress default fade.
        if (_phase != FadePhase::kIdle) {
            return;
        }
        logger::info("Blackout: built-in default (fade + {:.0f}h skip).", kDefaultSkipHours);
        FadeOut();
        _pendingSkip = kDefaultSkipHours;
        _phase       = FadePhase::kFadingOut;
        _phaseTimer  = kFadeOutHoldSeconds;
    }

    // --- fade FSM -------------------------------------------------------------------------------------

    void BlackoutManager::FadeOut()
    {
        if (_holdImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(_holdImod, 1.0f, nullptr);
        }
    }

    void BlackoutManager::Reveal()
    {
        if (_holdImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(_holdImod);
        }
        if (_backImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(_backImod, 1.0f, nullptr);  // AF's own fade-in
        }
    }

    void BlackoutManager::AdvanceGameHours(float a_h)
    {
        auto* cal = RE::Calendar::GetSingleton();
        if (cal && cal->gameDaysPassed) {
            cal->gameDaysPassed->value += a_h / 24.0f;  // bumps the master clock (gameHour derives from it)
        }
    }

    void BlackoutManager::SetExcludeQuests(std::vector<RE::FormID> a_quests)
    {
        _excludeQuests = std::move(a_quests);
    }

    bool BlackoutManager::IsSafeToBlackout() const
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead()) {
            return false;
        }
        // (4) movement controls disabled → a scripted scene/animation owns the player (OStim, SexLab, vanilla
        // sequences, carriage rides, etc.). The broadest guard.
        if (auto* cmap = RE::ControlMap::GetSingleton(); cmap && !cmap->IsMovementControlsEnabled()) {
            return false;
        }
        // (2) mounted (horse/creature) — teleporting off a mount is buggy.
        if (player->IsOnMount()) {
            return false;
        }
        // cinematic kill-move — the camera/animation is engine-driven.
        if (player->IsInKillMove()) {
            return false;
        }
        // (1) in a conversation.
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
            return false;
        }
        // (3) a config-listed transport-unsafe quest is running.
        for (auto q : _excludeQuests) {
            if (auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(q); quest && quest->IsRunning()) {
                return false;
            }
        }
        return true;
    }

    void BlackoutManager::Update(float a_deltaSeconds)
    {
        if (_phase == FadePhase::kIdle) {
            return;
        }
        _phaseTimer -= a_deltaSeconds;
        if (_phaseTimer > 0.0f) {
            return;
        }

        switch (_phase) {
        case FadePhase::kFadingOut:  // fully black now — skip the time under cover, then fade back in
            AdvanceGameHours(_pendingSkip);
            Reveal();
            _phase      = FadePhase::kReveal;
            _phaseTimer = kFadeInSeconds;
            break;
        case FadePhase::kReveal:
            _phase = FadePhase::kIdle;
            break;
        default:
            _phase = FadePhase::kIdle;
            break;
        }
    }

    void BlackoutManager::ResetFade()
    {
        if (_holdImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(_holdImod);
        }
        _phase       = FadePhase::kIdle;
        _phaseTimer  = 0.0f;
        _pendingSkip = 0.0f;
    }

    // --- Menu surface ---------------------------------------------------------------------------------

    float BlackoutManager::GetChance(const std::string& a_name) const
    {
        auto it = _chances.find(a_name);
        return it == _chances.end() ? 0.0f : it->second;
    }

    void BlackoutManager::SetChance(const std::string& a_name, float a_pct)
    {
        _chances[a_name] = std::clamp(a_pct, 0.0f, 100.0f);
    }

    // --- co-save (chances) — routed through AddictionManager's serialization callbacks -----------------

    void BlackoutManager::WriteChances(SKSE::SerializationInterface* a_intfc) const
    {
        if (!a_intfc->OpenRecord(kChancesRecord, AddictionManager::kSerializationVersion)) {
            logger::error("Blackout: failed to open chances save record.");
            return;
        }
        const std::uint32_t count = static_cast<std::uint32_t>(_chances.size());
        a_intfc->WriteRecordData(&count, sizeof(count));
        for (auto& [name, chance] : _chances) {
            const std::uint32_t len = static_cast<std::uint32_t>(name.size());
            a_intfc->WriteRecordData(&len, sizeof(len));
            a_intfc->WriteRecordData(name.data(), len);
            a_intfc->WriteRecordData(&chance, sizeof(chance));
        }
        logger::info("Blackout: saved {} scenario chance(s).", count);
    }

    void BlackoutManager::ReadChances(SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t count = 0;
        a_intfc->ReadRecordData(&count, sizeof(count));
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t len = 0;
            a_intfc->ReadRecordData(&len, sizeof(len));
            std::string name(len, '\0');
            if (len) {
                a_intfc->ReadRecordData(name.data(), len);
            }
            float chance = 0.0f;
            a_intfc->ReadRecordData(&chance, sizeof(chance));
            // Only apply to a still-registered outcome (a since-removed scenario's chance is dropped).
            if (_chances.find(name) != _chances.end()) {
                _chances[name] = std::clamp(chance, 0.0f, 100.0f);
            }
        }
        logger::info("Blackout: loaded {} scenario chance(s).", count);
    }

    void BlackoutManager::RevertChances()
    {
        for (auto& o : _outcomes) {
            _chances[o.name] = o.defaultChance;  // back to authored defaults (pre-load / new game)
        }
    }
}
