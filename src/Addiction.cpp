#include "Addiction.h"

#include "Blackout.h"
#include "Display.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
    // Category names are matched case-INSENSITIVELY (Skyrim treats these strings case-insensitively, and
    // BSFixedString interns them that way — so a modder's GetLevel("Cannabis") must resolve our "cannabis"
    // key). Lowercase is the canonical internal form.
    std::string ToLower(std::string a_s)
    {
        std::transform(a_s.begin(), a_s.end(), a_s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a_s;
    }

    // Case-sensitive suffix test (callers lowercase first). Used for the OStim `nostrip`-style keyword
    // match: a modifier keyword's editorID need only END WITH the AF token, so a modder's own
    // `MyMod_AF_ModGain` (their plugin, no AF master) is recognized (§7).
    bool EndsWith(const std::string& a_s, const std::string& a_suffix)
    {
        return a_s.size() >= a_suffix.size() &&
               a_s.compare(a_s.size() - a_suffix.size(), a_suffix.size(), a_suffix) == 0;
    }
}

namespace AddictionFramework
{
    // How often (real seconds) the tick refreshes applied effects.
    static constexpr float kTickIntervalSec = 2.0f;

    AddictionManager* AddictionManager::GetSingleton()
    {
        static AddictionManager singleton;
        return &singleton;
    }

    void AddictionManager::RegisterCategory(std::uint32_t a_groupFormID, const Category& a_category)
    {
        _categories[a_groupFormID] = a_category;
        logger::info("RegisterCategory {:08X}: gain={:.1f} decay={:.2f} thr={:.0f} | addiction spell={:08X} "
                     "effects={} valid={} | withdrawal spell={:08X} effects={} valid={}",
                     a_groupFormID, a_category.gain, a_category.decay, a_category.addictionThreshold,
                     a_category.addiction.spell, a_category.addiction.effects.size(), a_category.addiction.valid,
                     a_category.withdrawal.spell, a_category.withdrawal.effects.size(),
                     a_category.withdrawal.valid);
    }

    const Category* AddictionManager::FindCategory(std::uint32_t a_category) const
    {
        auto it = _categories.find(a_category);
        return it == _categories.end() ? nullptr : &it->second;
    }

    void AddictionManager::RegisterCategoryName(const std::string& a_name, std::uint32_t a_groupFormID)
    {
        const std::string key    = ToLower(a_name);  // canonical lowercase (case-insensitive matching)
        _categoryNames[key]      = a_groupFormID;
        _keyNames[a_groupFormID] = key;  // reverse, for the event strArg + string-returning API
    }

    std::uint32_t AddictionManager::KeyForName(const std::string& a_name) const
    {
        auto it = _categoryNames.find(ToLower(a_name));
        return it == _categoryNames.end() ? 0u : it->second;
    }

    std::string AddictionManager::NameForKey(std::uint32_t a_groupFormID) const
    {
        auto it = _keyNames.find(a_groupFormID);
        return it == _keyNames.end() ? std::string{} : it->second;
    }

    float AddictionManager::Now() const
    {
        auto* cal = RE::Calendar::GetSingleton();
        return cal ? cal->GetHoursPassed() : 0.0f;
    }

    void AddictionManager::SettleDecay(std::uint32_t a_key, State& a_st, const Category& a_cat, float a_now)
    {
        const float elapsed = a_now - a_st.lastUpdated;
        if (elapsed > 0.0f && a_st.level > 0.0f) {
            // HastenDecay modifier (§7): 100% = double decay speed, negative = slow it. Sampled at settle;
            // settles are frequent (tick + every read) so the chunk error is negligible.
            const float mult = std::max(0.0f, 1.0f + ModifierPercent(a_key, ModKind::kDecay) / 100.0f);
            a_st.level = std::max(0.0f, a_st.level - a_cat.decay * mult * elapsed);
        }
        a_st.lastUpdated = a_now;

        // Hysteresis latch: onset once level crosses the addicted threshold, cleared only when fully
        // decayed to 0. Keeps addiction+withdrawal effects active through a cold-turkey taper (§4.3).
        if (a_st.level >= a_cat.addictionThreshold) {
            a_st.addicted = true;
        } else if (a_st.level <= 0.0f) {
            a_st.addicted = false;
        }
    }

    Stage AddictionManager::ComputeStage(const State& a_st, const Category& a_cat, float a_now) const
    {
        if (!a_st.addicted) {
            return Stage::kClean;
        }
        // The withdrawal-onset window tightens as addiction deepens: lerp hoursToWithdrawal (at the
        // addictionThreshold) → hoursToWithdrawalAtPeak (at level 100), evaluated against the CURRENT
        // (already-settled, decaying) level. Clamp the fraction to [0,1]: the addicted latch holds through
        // the cold-turkey taper BELOW the threshold, which saturates to the base window (never longer), and
        // level can't exceed 100. Equal endpoints (the default) → constant window. Sobering up mid-
        // abstinence relaxes the window, but it's bounded by the base, so onset always still fires.
        float window = a_cat.hoursToWithdrawal;
        if (a_cat.hoursToWithdrawalAtPeak != a_cat.hoursToWithdrawal) {
            const float span = 100.0f - a_cat.addictionThreshold;
            const float t    = span > 0.0f
                                   ? std::clamp((a_st.level - a_cat.addictionThreshold) / span, 0.0f, 1.0f)
                                   : 0.0f;
            window = std::lerp(a_cat.hoursToWithdrawal, a_cat.hoursToWithdrawalAtPeak, t);
        }
        if ((a_now - a_st.lastUseHour) > window) {
            return Stage::kWithdrawal;
        }
        return Stage::kSatisfied;
    }

    void AddictionManager::FireEvent(const char* a_eventName, std::uint32_t a_category, float a_numArg) const
    {
        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            return;
        }
        const std::string name = NameForKey(a_category);
        SKSE::ModCallbackEvent ev{};
        ev.eventName = a_eventName;
        ev.strArg    = name.c_str();  // category STRING — event handlers stay master-free (Form is hidden)
        ev.numArg    = a_numArg;
        ev.sender    = RE::TESForm::LookupByID(a_category);  // group spell (internal detail; use strArg)
        source->SendEvent(&ev);
    }

    void AddictionManager::UpdateState(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now)
    {
        SettleDecay(a_category, a_st, a_cat, a_now);
        const Stage stage = ComputeStage(a_st, a_cat, a_now);

        // Consequence effects (§5.1 + Mechanism B): each is a container spell whose blank PVM effects were
        // retargeted from config at load; the EffectManager scales every blank to max*(level/100) here.
        auto* effects = EffectManager::GetSingleton();
        if (a_cat.addiction.valid) {
            effects->ApplyEffect(a_cat.addiction.spell, a_st.addicted, a_st.level);
        }
        if (a_cat.withdrawal.valid) {
            // SuppressWithdrawal modifier (§7): scale the withdrawal magnitude by (1 - suppress%). 100% =
            // fully muted (drop the effect); negative = amplify. Folded into an effective level so the
            // EffectManager's drift check catches a suppression change even at a constant level.
            const float suppress = std::max(0.0f, 1.0f - ModifierPercent(a_category, ModKind::kWithdrawal) / 100.0f);
            const bool  active   = (stage == Stage::kWithdrawal) && (suppress > 0.0f);
            effects->ApplyEffect(a_cat.withdrawal.spell, active, a_st.level * suppress);
        }

        // Data-out (§9): drive the category's Level/Stage globals for CK/OAR/dialogue conditions.
        DriveGlobals(a_cat, a_st, stage, a_now);

        // Blackout tier: edge-detect the higher threshold FIRST, so a fire's window-clear is seen by the
        // acute pass below (dropping the Drunk status + firing AF_OnAcuteEnd in the same update).
        DriveBlackout(a_category, a_st, a_cat, a_now);

        // Acute axis: toggle the (possibly shared) marker ability from the trailing-window potency sum. Runs
        // on the tick too, so it clears itself as recent uses age out of the window.
        DriveAcute(a_category, a_st, a_cat, a_now);

        if (stage != a_st.lastStage) {
            logger::info("Stage change cat={:08X}: {} -> {}", a_category, static_cast<std::int32_t>(a_st.lastStage),
                         static_cast<std::int32_t>(stage));
            FireEvent("AF_OnStageChanged", a_category, static_cast<float>(static_cast<std::int32_t>(stage)));
            a_st.lastStage = stage;
        }
    }

    void AddictionManager::DriveGlobals(const Category& a_cat, const State& a_st, Stage a_stage, float a_now) const
    {
        // Level global (0–100). Only write on a meaningful drift (globals are polled by conditions; a plain
        // float write is cheap, but skip the no-op churn).
        if (a_cat.levelGlobal) {
            if (auto* g = RE::TESForm::LookupByID<RE::TESGlobal>(a_cat.levelGlobal)) {
                if (std::fabs(g->value - a_st.level) > 0.01f) {
                    g->value = a_st.level;
                }
            }
        }
        // Stage global (0 clean / 1 satisfied / 2 withdrawal) — the thing a faction rank couldn't encode.
        if (a_cat.stageGlobal) {
            const float sv = static_cast<float>(static_cast<std::int32_t>(a_stage));
            if (auto* g = RE::TESForm::LookupByID<RE::TESGlobal>(a_cat.stageGlobal)) {
                if (g->value != sv) {
                    g->value = sv;
                }
            }
        }
        // Acute-percent global — standardized "how far OVER the acute threshold" so mods can condition on
        // intensity without knowing a category's potency scale: 0 = off / at threshold, 100 = 2× threshold,
        // 200 = 3×, unbounded above. `pct = max(0, (potencySum/threshold − 1) × 100)`.
        if (a_cat.acutePercentGlobal) {
            float pct = 0.0f;
            if (a_cat.acuteEnabled && a_cat.acuteThreshold > 0.0f) {
                pct = (RecentPotency(a_st, a_cat.acuteWindowHours, a_now) / a_cat.acuteThreshold - 1.0f) * 100.0f;
                if (pct < 0.0f) {
                    pct = 0.0f;
                }
            }
            if (auto* g = RE::TESForm::LookupByID<RE::TESGlobal>(a_cat.acutePercentGlobal)) {
                if (std::fabs(g->value - pct) > 0.1f) {
                    g->value = pct;
                }
            }
        }
    }

    // --- acute status (§7) ---------------------------------------------------------------------------

    float AddictionManager::RecentPotency(const State& a_st, float a_windowHours, float a_now) const
    {
        const float cutoff = a_now - a_windowHours;
        float       sum    = 0.0f;
        for (const auto& [t, p] : a_st.recentUses) {
            if (t >= cutoff) {
                sum += p;
            }
        }
        return sum;
    }

    void AddictionManager::DriveAcute(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now)
    {
        if (!a_cat.acuteEnabled || !a_cat.acuteSpell) {
            return;
        }
        const bool want = RecentPotency(a_st, a_cat.acuteWindowHours, a_now) >= a_cat.acuteThreshold;

        // Per-category edge event (fires for THIS category regardless of the shared-spell state below).
        if (want != a_st.inAcuteStatus) {
            a_st.inAcuteStatus = want;  // a_st aliases the _states entry, so the reconcile below sees it
            FireEvent(want ? "AF_OnAcuteStart" : "AF_OnAcuteEnd", a_category, want ? 1.0f : 0.0f);
            logger::info("Acute {}: cat={:08X} (sum {:.2f} vs threshold {:.2f})", want ? "ON" : "off", a_category,
                         RecentPotency(a_st, a_cat.acuteWindowHours, a_now), a_cat.acuteThreshold);
        }

        // Reconcile the (possibly SHARED) effect ability: on iff ANY acute-enabled category selecting this
        // same spell is currently active. AF is a pure applicator — only Add/RemoveSpell, never the MGEF.
        bool anyActive = false;
        for (const auto& [key, other] : _states) {
            const Category* oc = FindCategory(key);
            if (oc && oc->acuteEnabled && oc->acuteSpell == a_cat.acuteSpell && other.inAcuteStatus) {
                anyActive = true;
                break;
            }
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell  = RE::TESForm::LookupByID<RE::SpellItem>(a_cat.acuteSpell);
        if (player && spell) {
            const bool has = player->HasSpell(spell);
            if (anyActive && !has) {
                player->AddSpell(spell);
            } else if (!anyActive && has) {
                player->RemoveSpell(spell);
            }
        }
    }

    // --- blackout tier (§11) -------------------------------------------------------------------------

    void AddictionManager::DriveBlackout(std::uint32_t a_category, State& a_st, const Category& a_cat, float a_now)
    {
        if (!a_cat.blackoutEnabled) {
            return;
        }
        const float sum  = RecentPotency(a_st, a_cat.blackoutWindowHours, a_now);
        const bool  over = sum >= a_cat.blackoutThreshold;

        if (!over) {
            a_st.blackoutArmed = true;  // re-arm once the window falls back below the threshold
            return;
        }
        if (!a_st.blackoutArmed) {
            return;  // already fired this bender — wait for the window to drain below threshold and re-arm
        }
        // Safety: a blackout yanks the player (teleport/fade), which is unsafe mid-dialogue, on a mount, in a
        // scene/animation, etc. DEFER without disarming — so it fires the moment it's safe again, as long as
        // the trailing window is still over threshold (a third-party consumable could trigger this anywhere).
        if (!BlackoutManager::GetSingleton()->IsSafeToBlackout()) {
            return;
        }

        // Fire ONE blackout. Disarm (re-arms only after the window drains below threshold — sober off and
        // chug again to re-trigger), then CLEAR the acute window so the player "sleeps it off": DriveAcute
        // (called right after, in UpdateState) then sees the empty window and drops Drunk + fires AF_OnAcuteEnd.
        a_st.blackoutArmed = false;
        a_st.recentUses.clear();

        logger::info("BLACKOUT: cat={:08X} (window sum {:.2f} >= threshold {:.2f})", a_category, sum,
                     a_cat.blackoutThreshold);
        FireEvent("AF_OnBlackout", a_category, sum);

        // Hand off to the outcome selector: a registered scenario quest, or the built-in default fade+skip.
        BlackoutManager::GetSingleton()->OnBlackout(a_category);
    }

    // --- modifier aggregation (§7) -------------------------------------------------------------------

    float AddictionManager::ScriptModifierPercent(std::uint32_t a_key, ModKind a_kind) const
    {
        auto it = _scriptModifiers.find(a_key);
        if (it == _scriptModifiers.end()) {
            return 0.0f;
        }
        switch (a_kind) {
        case ModKind::kGain:       return it->second.gain;
        case ModKind::kDecay:      return it->second.decay;
        case ModKind::kWithdrawal: return it->second.withdrawal;
        default:                   return 0.0f;
        }
    }

    // Scan the player's active magic effects; sum the magnitude of every effect carrying a keyword whose
    // editorID ends with this kind's AF token (global) or `<token>_<category>` (category-specific). Flexible
    // suffix match (OStim `nostrip` model) → a modder's own keyword works with no AF master. Self-healing:
    // an inactive/dispelled effect is skipped, so a removed modifier drops out on its own.
    float AddictionManager::KeywordModifierPercent(std::uint32_t a_key, ModKind a_kind) const
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0.0f;
        }
        auto* target = player->AsMagicTarget();
        auto* list   = target ? target->GetActiveEffectList() : nullptr;
        if (!list) {
            return 0.0f;
        }

        const char* token = nullptr;
        switch (a_kind) {
        case ModKind::kGain:       token = "af_modgain"; break;
        case ModKind::kDecay:      token = "af_hastendecay"; break;
        case ModKind::kWithdrawal: token = "af_suppresswithdrawal"; break;
        default:                   return 0.0f;
        }
        const std::string globalTok  = token;
        const std::string name       = NameForKey(a_key);  // canonical lowercase, "" if unknown
        const std::string categoryTok = name.empty() ? std::string{} : (globalTok + "_" + name);

        float sum = 0.0f;
        for (auto* ae : *list) {
            if (!ae || ae->flags.any(RE::ActiveEffect::Flag::kInactive) ||
                ae->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
                continue;
            }
            auto* mgef = ae->GetBaseObject();
            if (!mgef) {
                continue;
            }
            for (auto* kw : mgef->GetKeywords()) {
                if (!kw) {
                    continue;
                }
                const char* eid = kw->GetFormEditorID();
                if (!eid || !eid[0]) {
                    continue;
                }
                const std::string low = ToLower(eid);
                // Category-specific (`..._skooma`) OR global (`...af_modgain`). The two are exclusive per
                // keyword — a `_skooma`-suffixed id ends with the category, not the bare token.
                if ((!categoryTok.empty() && EndsWith(low, categoryTok)) || EndsWith(low, globalTok)) {
                    sum += ae->magnitude;
                    break;  // one match per effect (avoid double-counting a doubly-keyworded effect)
                }
            }
        }
        return sum;
    }

    float AddictionManager::ModifierPercent(std::uint32_t a_key, ModKind a_kind) const
    {
        if (!a_key) {
            return 0.0f;  // unknown category → no modifier (matches the safe-no-op API contract)
        }
        return KeywordModifierPercent(a_key, a_kind) + ScriptModifierPercent(a_key, a_kind);
    }

    float AddictionManager::RegisterUse(std::uint32_t a_category, float a_potency)
    {
        const Category* cat = FindCategory(a_category);
        if (!cat) {
            logger::warn("RegisterUse: no category registered for {:08X} — ignoring use.", a_category);
            return 0.0f;
        }
        const float now = Now();
        auto&       st  = _states[a_category];

        SettleDecay(a_category, st, *cat, now);

        const float headroom = std::max(0.0f, 1.0f - st.level / 100.0f);
        float       gain     = cat->gain * a_potency * std::pow(headroom, cat->gainFalloff);
        // ModGain modifier (§7): 100% = fully blocked (a nicotine patch), negative = a potentiator. Floor
        // the multiplier at 0 so gain never goes negative from a modifier.
        const float gainMult = std::max(0.0f, 1.0f - ModifierPercent(a_category, ModKind::kGain) / 100.0f);
        gain *= gainMult;
        st.level       = std::min(100.0f, st.level + gain);
        st.lastUseHour = now;

        // Acute axis: record this use's potency for the trailing-window sum, then prune entries older than
        // the window so the vector stays bounded. Only for acute-enabled categories.
        if (cat->acuteEnabled) {
            st.recentUses.emplace_back(now, a_potency);
            const float cutoff = now - cat->acuteWindowHours;
            auto&       ru     = st.recentUses;
            ru.erase(std::remove_if(ru.begin(), ru.end(), [cutoff](const auto& p) { return p.first < cutoff; }),
                     ru.end());
        }

        UpdateState(a_category, st, *cat, now);
        FireEvent("AF_OnUse", a_category, gain);  // fired for every use (any trigger source, §12)

        logger::info("RegisterUse: cat={:08X} potency={:.2f} gain={:.2f} -> level={:.2f}", a_category, a_potency, gain,
                     st.level);
        return st.level;
    }

    float AddictionManager::GetLevel(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return 0.0f;
        }
        if (const Category* cat = FindCategory(a_category)) {
            SettleDecay(a_category, it->second, *cat, Now());
        }
        return it->second.level;
    }

    Stage AddictionManager::GetStage(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return Stage::kClean;
        }
        const Category* cat = FindCategory(a_category);
        if (!cat) {
            return Stage::kClean;
        }
        const float now = Now();
        SettleDecay(a_category, it->second, *cat, now);
        return ComputeStage(it->second, *cat, now);
    }

    bool AddictionManager::IsAddicted(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return false;
        }
        if (const Category* cat = FindCategory(a_category)) {
            SettleDecay(a_category, it->second, *cat, Now());  // settling maintains the addicted latch
        }
        return it->second.addicted;
    }

    bool AddictionManager::IsInAcuteStatus(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return false;
        }
        const Category* cat = FindCategory(a_category);
        if (!cat || !cat->acuteEnabled) {
            return false;
        }
        return RecentPotency(it->second, cat->acuteWindowHours, Now()) >= cat->acuteThreshold;
    }

    bool AddictionManager::IsIntoxicated(std::uint32_t a_category)
    {
        const Category* cat = FindCategory(a_category);
        if (!cat || !cat->acuteIsIntoxicated) {
            return false;  // the selected effect isn't an inebriation one (e.g. "Wired") → never "intoxicated"
        }
        return IsInAcuteStatus(a_category);
    }

    float AddictionManager::GetAcuteLevel(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        const Category* cat = FindCategory(a_category);
        if (it == _states.end() || !cat || !cat->acuteEnabled) {
            return 0.0f;
        }
        return RecentPotency(it->second, cat->acuteWindowHours, Now());
    }

    float AddictionManager::GetAcutePercent(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        const Category* cat = FindCategory(a_category);
        if (it == _states.end() || !cat || !cat->acuteEnabled || cat->acuteThreshold <= 0.0f) {
            return 0.0f;
        }
        const float pct = (RecentPotency(it->second, cat->acuteWindowHours, Now()) / cat->acuteThreshold - 1.0f) * 100.0f;
        return pct > 0.0f ? pct : 0.0f;
    }

    void AddictionManager::RegisterAcuteEffect(const std::string& a_name, std::uint32_t a_spellFormID)
    {
        if (a_name.empty() || !a_spellFormID) {
            return;
        }
        _acuteEffects[ToLower(a_name)] = a_spellFormID;
    }

    bool AddictionManager::IsAcuteEffectActive(const std::string& a_name) const
    {
        const auto it = _acuteEffects.find(ToLower(a_name));
        if (it == _acuteEffects.end()) {
            return false;  // unknown status name
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell  = RE::TESForm::LookupByID<RE::SpellItem>(it->second);
        // AF ref-count-applies the shared acute spell in DriveAcute, so HasSpell IS the live "is this status
        // active across any category" answer.
        return player && spell && player->HasSpell(spell);
    }

    float AddictionManager::NotifyUse(std::uint32_t a_category, float a_amount)
    {
        return RegisterUse(a_category, a_amount);  // trigger-agnostic relay (§12)
    }

    void AddictionManager::AddLevel(std::uint32_t a_category, float a_amount)
    {
        const Category* cat = FindCategory(a_category);
        if (!cat) {
            logger::warn("AddLevel: no category registered for {:08X}.", a_category);
            return;
        }
        const float now = Now();
        auto&       st  = _states[a_category];
        SettleDecay(a_category, st, *cat, now);
        st.level = std::clamp(st.level + a_amount, 0.0f, 100.0f);
        UpdateState(a_category, st, *cat, now);  // no lastUseHour bump — a raw nudge, not a "use"
        logger::info("AddLevel: cat={:08X} amount={:.2f} -> level={:.2f}", a_category, a_amount, st.level);
    }

    void AddictionManager::Cure(std::uint32_t a_category)
    {
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return;  // nothing to cure
        }
        const Category* cat = FindCategory(a_category);
        if (!cat) {
            return;
        }
        const float now = Now();
        it->second.level    = 0.0f;
        it->second.addicted = false;
        UpdateState(a_category, it->second, *cat, now);  // strips effects, fires stage→clean
        FireEvent("AF_OnCured", a_category, 0.0f);
        logger::info("Cure: cat={:08X}.", a_category);
    }

    void AddictionManager::CureAll()
    {
        // Player-facing cleanup ("remove all Addiction Framework effects"): cure every category the player has
        // state for (level→0, latch off, acute window cleared) — UpdateState then strips the consequence +
        // acute spells and cleans the data-out globals — then belt-and-suspenders strip every category's
        // applied effect + acute spell straight off the player.
        const float now = Now();
        int         cured = 0;
        for (auto& [key, st] : _states) {
            const Category* cat = FindCategory(key);
            if (!cat) {
                continue;
            }
            st.level    = 0.0f;
            st.addicted = false;
            st.recentUses.clear();
            UpdateState(key, st, *cat, now);
            FireEvent("AF_OnCured", key, 0.0f);
            ++cured;
        }
        EffectManager::GetSingleton()->ClearAll();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& [key, cat] : _categories) {
                if (cat.acuteSpell) {
                    if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(cat.acuteSpell);
                        sp && player->HasSpell(sp)) {
                        player->RemoveSpell(sp);
                    }
                }
            }
        }
        logger::info("CureAll: cured {} categor(ies) + stripped all AF effects from the player.", cured);
    }

    void AddictionManager::ResetWithdrawalTimer(std::uint32_t a_category)
    {
        const Category* cat = FindCategory(a_category);
        if (!cat) {
            logger::warn("ResetWithdrawalTimer: no category registered for {:08X}.", a_category);
            return;
        }
        auto it = _states.find(a_category);
        if (it == _states.end()) {
            return;  // no state → not addicted → nothing to stave off
        }
        const float now = Now();
        SettleDecay(a_category, it->second, *cat, now);
        it->second.lastUseHour = now;  // reset the withdrawal clock ONLY — addiction level untouched
        // Re-derive stage/effects: if in withdrawal, this drops it back to satisfied and strips the effect.
        UpdateState(a_category, it->second, *cat, now);
        logger::info("ResetWithdrawalTimer: cat={:08X} — withdrawal clock reset (level {:.2f}).", a_category,
                     it->second.level);
    }

    std::vector<std::uint32_t> AddictionManager::GetActiveAddictions()
    {
        std::vector<std::uint32_t> out;
        const float               now = Now();
        for (auto& [key, st] : _states) {
            if (const Category* cat = FindCategory(key)) {
                SettleDecay(key, st, *cat, now);
                if (st.addicted) {
                    out.push_back(key);
                }
            }
        }
        return out;
    }

    void AddictionManager::AddModifier(std::uint32_t a_category, ModKind a_kind, float a_percent)
    {
        if (!a_category) {
            return;  // unknown category name → safe no-op (matches the string-API contract)
        }
        auto& m = _scriptModifiers[a_category];
        switch (a_kind) {
        case ModKind::kGain:       m.gain += a_percent; break;
        case ModKind::kDecay:      m.decay += a_percent; break;
        case ModKind::kWithdrawal: m.withdrawal += a_percent; break;
        default:                   return;
        }
        // Refresh now so a withdrawal-suppress (or any) change takes effect immediately, not next tick.
        if (auto it = _states.find(a_category); it != _states.end()) {
            if (const Category* cat = FindCategory(a_category)) {
                UpdateState(a_category, it->second, *cat, Now());
            }
        }
        logger::info("AddModifier: cat={:08X} kind={} += {:.1f}", a_category,
                     static_cast<std::int32_t>(a_kind), a_percent);
    }

    void AddictionManager::ClearModifier(std::uint32_t a_category, ModKind a_kind)
    {
        auto it = _scriptModifiers.find(a_category);
        if (it == _scriptModifiers.end()) {
            return;
        }
        switch (a_kind) {
        case ModKind::kGain:       it->second.gain = 0.0f; break;
        case ModKind::kDecay:      it->second.decay = 0.0f; break;
        case ModKind::kWithdrawal: it->second.withdrawal = 0.0f; break;
        default:                   return;
        }
        if (auto st = _states.find(a_category); st != _states.end()) {
            if (const Category* cat = FindCategory(a_category)) {
                UpdateState(a_category, st->second, *cat, Now());
            }
        }
        logger::info("ClearModifier: cat={:08X} kind={}", a_category, static_cast<std::int32_t>(a_kind));
    }

    float AddictionManager::GetModifier(std::uint32_t a_category, ModKind a_kind) const
    {
        return ModifierPercent(a_category, a_kind);  // effective aggregate (keyword + script); key 0 → 0
    }

    void AddictionManager::Tick(float a_deltaSeconds)
    {
        // The blackout fade FSM steps every frame with the real delta, independent of the throttled state
        // loop below (a fade must animate smoothly, not at the ~2s state cadence).
        BlackoutManager::GetSingleton()->Update(a_deltaSeconds);

        _tickAccum += a_deltaSeconds;
        if (_tickAccum < kTickIntervalSec) {
            return;
        }
        _tickAccum = 0.0f;

        if (_states.empty()) {
            return;
        }

        const float now = Now();
        for (auto& [key, st] : _states) {
            if (const Category* cat = FindCategory(key)) {
                UpdateState(key, st, *cat, now);
            }
        }
    }

    // --- SKSE co-save serialization -------------------------------------------------------------

    void AddictionManager::Save(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kSerializationType, kSerializationVersion)) {
            logger::error("AF: failed to open save record.");
            return;
        }

        const std::uint32_t count = static_cast<std::uint32_t>(_states.size());
        a_intfc->WriteRecordData(&count, sizeof(count));

        for (auto& [key, st] : _states) {
            a_intfc->WriteRecordData(&key, sizeof(key));
            a_intfc->WriteRecordData(&st.level, sizeof(st.level));
            a_intfc->WriteRecordData(&st.lastUpdated, sizeof(st.lastUpdated));
            a_intfc->WriteRecordData(&st.lastUseHour, sizeof(st.lastUseHour));
            const std::int32_t stage = static_cast<std::int32_t>(st.lastStage);
            a_intfc->WriteRecordData(&stage, sizeof(stage));
            const std::uint8_t addicted = st.addicted ? 1 : 0;
            a_intfc->WriteRecordData(&addicted, sizeof(addicted));
            // Acute axis (v2): recent (game-hour, potency) uses + the status latch.
            const std::uint32_t ruCount = static_cast<std::uint32_t>(st.recentUses.size());
            a_intfc->WriteRecordData(&ruCount, sizeof(ruCount));
            for (auto& [t, p] : st.recentUses) {
                a_intfc->WriteRecordData(&t, sizeof(t));
                a_intfc->WriteRecordData(&p, sizeof(p));
            }
            const std::uint8_t acute = st.inAcuteStatus ? 1 : 0;
            a_intfc->WriteRecordData(&acute, sizeof(acute));
            // Blackout tier (v4): re-arm latch.
            const std::uint8_t barmed = st.blackoutArmed ? 1 : 0;
            a_intfc->WriteRecordData(&barmed, sizeof(barmed));
        }
        logger::info("AF: saved {} state(s).", count);

        // Script rate-modifier offsets (§7) — a separate record, keyed by the same group FormID.
        if (a_intfc->OpenRecord(kModifierType, kSerializationVersion)) {
            const std::uint32_t mcount = static_cast<std::uint32_t>(_scriptModifiers.size());
            a_intfc->WriteRecordData(&mcount, sizeof(mcount));
            for (auto& [key, m] : _scriptModifiers) {
                a_intfc->WriteRecordData(&key, sizeof(key));
                a_intfc->WriteRecordData(&m.gain, sizeof(m.gain));
                a_intfc->WriteRecordData(&m.decay, sizeof(m.decay));
                a_intfc->WriteRecordData(&m.withdrawal, sizeof(m.withdrawal));
            }
            logger::info("AF: saved {} script-modifier set(s).", mcount);
        } else {
            logger::error("AF: failed to open script-modifier save record.");
        }

        // Blackout scenario chances (§11 / Menu) — a separate 'BLKO' record on the same co-save stream.
        BlackoutManager::GetSingleton()->WriteChances(a_intfc);
    }

    void AddictionManager::Load(SKSE::SerializationInterface* a_intfc)
    {
        _states.clear();
        _scriptModifiers.clear();

        std::uint32_t type    = 0;
        std::uint32_t version = 0;
        std::uint32_t length  = 0;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (version != kSerializationVersion) {
                logger::warn("AF: record {:08X} version {} != {}, skipping.", type, version, kSerializationVersion);
                continue;
            }

            if (type == kSerializationType) {  // player state
                std::uint32_t count = 0;
                a_intfc->ReadRecordData(&count, sizeof(count));
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t key = 0;
                    State         st{};
                    a_intfc->ReadRecordData(&key, sizeof(key));
                    a_intfc->ReadRecordData(&st.level, sizeof(st.level));
                    a_intfc->ReadRecordData(&st.lastUpdated, sizeof(st.lastUpdated));
                    a_intfc->ReadRecordData(&st.lastUseHour, sizeof(st.lastUseHour));
                    std::int32_t stage = 0;
                    a_intfc->ReadRecordData(&stage, sizeof(stage));
                    st.lastStage = static_cast<Stage>(stage);
                    std::uint8_t addicted = 0;
                    a_intfc->ReadRecordData(&addicted, sizeof(addicted));
                    st.addicted = addicted != 0;
                    // Acute axis (v2): recent uses + status latch.
                    std::uint32_t ruCount = 0;
                    a_intfc->ReadRecordData(&ruCount, sizeof(ruCount));
                    for (std::uint32_t k = 0; k < ruCount; ++k) {
                        float t = 0.0f, p = 0.0f;
                        a_intfc->ReadRecordData(&t, sizeof(t));
                        a_intfc->ReadRecordData(&p, sizeof(p));
                        st.recentUses.emplace_back(t, p);
                    }
                    std::uint8_t acute = 0;
                    a_intfc->ReadRecordData(&acute, sizeof(acute));
                    st.inAcuteStatus = acute != 0;
                    // Blackout tier (v4): re-arm latch.
                    std::uint8_t barmed = 1;
                    a_intfc->ReadRecordData(&barmed, sizeof(barmed));
                    st.blackoutArmed = barmed != 0;
                    // Consequence effects are re-derived by the tick from level/stage after load.
                    std::uint32_t newKey = key;
                    if (!a_intfc->ResolveFormID(key, newKey)) {
                        logger::warn("AF: could not resolve category {:08X}; dropping state.", key);
                        continue;
                    }
                    _states[newKey] = st;
                }
            } else if (type == kModifierType) {  // script rate-modifier offsets (§7)
                std::uint32_t count = 0;
                a_intfc->ReadRecordData(&count, sizeof(count));
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t key = 0;
                    Modifiers     m{};
                    a_intfc->ReadRecordData(&key, sizeof(key));
                    a_intfc->ReadRecordData(&m.gain, sizeof(m.gain));
                    a_intfc->ReadRecordData(&m.decay, sizeof(m.decay));
                    a_intfc->ReadRecordData(&m.withdrawal, sizeof(m.withdrawal));
                    std::uint32_t newKey = key;
                    if (!a_intfc->ResolveFormID(key, newKey)) {
                        logger::warn("AF: could not resolve modifier category {:08X}; dropping.", key);
                        continue;
                    }
                    _scriptModifiers[newKey] = m;
                }
            } else if (type == BlackoutManager::kChancesRecord) {  // blackout scenario chances (§11 / Menu)
                BlackoutManager::GetSingleton()->ReadChances(a_intfc);
            } else {
                logger::warn("AF: unexpected record {:08X}, skipping.", type);
            }
        }
        logger::info("AF: loaded {} state(s), {} script-modifier set(s).", _states.size(), _scriptModifiers.size());
    }

    void AddictionManager::Revert()
    {
        _states.clear();
        _scriptModifiers.clear();
        _tickAccum = 0.0f;
        BlackoutManager::GetSingleton()->RevertChances();  // scenario chances → authored defaults
        BlackoutManager::GetSingleton()->ResetFade();      // stop any in-progress fade
        logger::info("AF: reverted state.");
    }
}
