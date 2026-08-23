#include "Config.h"

#include "Addiction.h"
#include "Blackout.h"  // BlackoutManager (blackout-safety quest exclusions)
#include "Display.h"  // EffectManager + SubEffect

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    // Under MO2's VFS this resolves to the merged virtual Data folder.
    const fs::path kConfigDir = fs::path("Data") / "SKSE" / "Plugins" / "AddictionFramework";

    // ---- baked form roster (retired _AF_roster.json, 2026-08-18) -------------------------------------
    // Every form below lives in AddictionFramework.esm — AF's own plugin, authored by AF and shipped in
    // lockstep with this DLL, so its LOCAL FormIDs are stable. They are resolved at runtime via
    // TESDataHandler::LookupForm(id, kAFPlugin); the load-order (FE/xx) byte is NEVER baked (it isn't
    // stable across launches). This is a name→forms MAP only — a category stays INERT until a config
    // supplies an archetype + effects (see SeedRoster: it never sets `activated`). Extending the shipped
    // roster means authoring the forms in the .esm AND adding a row here, then rebuilding.
    constexpr const char* kAFPlugin = "AddictionFramework.esm";

    struct RosterForms
    {
        std::string_view name;
        RE::FormID       group;
        RE::FormID       levelGlobal;
        RE::FormID       stageGlobal;
        RE::FormID       addictionSpell;
        RE::FormID       withdrawalSpell;
    };
    // Transcribed 1:1 from the old _AF_roster.json (skooma/caffeine/alcohol hand-allocated; the reserved
    // namespace 0xDA5–0xEB2 is sequential per 15-form block but written out explicitly to stay verifiable).
    constexpr RosterForms kRoster[] = {
        //  name          group   level   stage   addSpell  wdSpell
        { "skooma",      0x805,  0x813,  0x814,  0x812,    0x80C },
        { "caffeine",    0x816,  0x817,  0x818,  0x824,    0x825 },
        { "alcohol",     0x827,  0x828,  0x829,  0x835,    0x836 },
        { "cannabis",    0xDB1,  0xDB2,  0xDB3,  0xDAF,    0xDB0 },
        { "psilocybin",  0xDC0,  0xDC1,  0xDC2,  0xDBE,    0xDBF },
        { "sex",         0xDCF,  0xDD0,  0xDD1,  0xDCD,    0xDCE },
        { "gambling",    0xDDE,  0xDDF,  0xDE0,  0xDDC,    0xDDD },
        { "gestation",   0xDED,  0xDEE,  0xDEF,  0xDEB,    0xDEC },
        { "lactation",   0xDFC,  0xDFD,  0xDFE,  0xDFA,    0xDFB },
        { "adrenaline",  0xE0B,  0xE0C,  0xE0D,  0xE09,    0xE0A },
        { "custom00",    0xE1A,  0xE1B,  0xE1C,  0xE18,    0xE19 },
        { "custom01",    0xE29,  0xE2A,  0xE2B,  0xE27,    0xE28 },
        { "custom02",    0xE38,  0xE39,  0xE3A,  0xE36,    0xE37 },
        { "custom03",    0xE47,  0xE48,  0xE49,  0xE45,    0xE46 },
        { "custom04",    0xE56,  0xE57,  0xE58,  0xE54,    0xE55 },
        { "custom05",    0xE65,  0xE66,  0xE67,  0xE63,    0xE64 },
        { "custom06",    0xE74,  0xE75,  0xE76,  0xE72,    0xE73 },
        { "custom07",    0xE83,  0xE84,  0xE85,  0xE81,    0xE82 },
        { "custom08",    0xE92,  0xE93,  0xE94,  0xE90,    0xE91 },
        { "custom09",    0xEA1,  0xEA2,  0xEA3,  0xE9F,    0xEA0 },
        { "custom10",    0xEB0,  0xEB1,  0xEB2,  0xEAE,    0xEAF },
    };

    // Shared named acute-status effect library (a config's acute/blackout `effect` selects one by key).
    struct AcuteLibForm
    {
        std::string_view key;
        RE::FormID       spell;
    };
    constexpr AcuteLibForm kAcuteLibrary[] = {
        { "Drunk",  0x83F },
        { "High",   0x840 },
        { "Stoned", 0x841 },
        { "Wired",  0x842 },
    };

    constexpr RE::FormID kIntoxicatedKeyword = 0x815;  // AF_Intoxicated (marks which acute effects intoxicate)

    // ---- merged (string-keyed) config, before form/AV resolution ----
    struct RawSubEffect
    {
        std::string av;
        float       max = 0.0f;
        std::string name;  // optional display name for this effect's MGEF; derived from the AV if empty
    };

    // Friendly display name for an actor value (the MGEF's fullName → the effect row's name in the menu).
    // A config `name` overrides this. Fallback = the raw AV string.
    std::string FriendlyAvName(const std::string& a_av)
    {
        std::string key = a_av;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::map<std::string, std::string> kMap = {
            { "health", "Health" }, { "magicka", "Magicka" }, { "stamina", "Stamina" },
            { "healrate", "Health Regeneration" }, { "healratemult", "Health Regeneration" },
            { "magickarate", "Magicka Regeneration" }, { "magickaratemult", "Magicka Regeneration" },
            { "staminarate", "Stamina Regeneration" }, { "staminaratemult", "Stamina Regeneration" },
            { "speedmult", "Speed" }, { "carryweight", "Carry Weight" },
        };
        auto it = kMap.find(key);
        return it != kMap.end() ? it->second : a_av;
    }

    struct RawEffect
    {
        std::vector<RawSubEffect> subs;                // {av,max} list stamped onto the container's blanks
        std::string               name;                // optional display-name override; empty = derive
        bool                      detrimental = true;
        bool                      present     = false;  // a value was given (list / object / "none")
        bool                      tombstone   = false;  // "none" → explicit no-effect
    };

    // Display name: an explicit override, else Cap(category) + " " + suffix ("Caffeine Addiction").
    std::string EffectName(const std::string& a_category, const char* a_suffix, const std::string& a_override)
    {
        if (!a_override.empty()) {
            return a_override;
        }
        std::string s = a_category;
        if (!s.empty()) {
            s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        }
        return s + " " + a_suffix;
    }

    // A named addictiveness preset (DESIGN §7) — the "onset × fade" curve. Bundles ONLY gain/gainFalloff/
    // decay so a modder picks a feel, not numbers; addictionThreshold + toleranceHours stay explicit
    // per-category (the consequence-timing severity axis, §4.2).
    struct ArchetypeCurve
    {
        float gain        = 9.0f;
        float gainFalloff = 1.5f;
        float decay       = 0.4f;
    };

    struct RawCategory
    {
        std::string archetype;        // named curve preset; explicit params below override it
        float       gain               = 9.0f;
        float       gainFalloff        = 1.5f;
        float       decay              = 0.4f;
        float       addictionThreshold = 15.0f;
        float       toleranceHours     = 20.0f;
        std::string group;            // from the internal roster manifest (or the GetName() scan)
        std::string levelGlobal;      // from the roster manifest; GlobalFloat data-out channel (§9)
        std::string stageGlobal;      // from the roster manifest; GlobalShort data-out channel (§9)
        std::string addictionSpell;   // container spell (blank pool) — from the roster manifest
        std::string withdrawalSpell;  // container spell (blank pool) — from the roster manifest
        RawEffect   addiction;
        RawEffect   withdrawal;
        // acute status (§7): trailing-window potency trigger + which effect to apply. `acuteEffect` is a
        // stable string — a named key in the shared acuteEffects library (Drunk/High/…) OR a bring-your-own
        // "Plugin.esp|0xID" spell.
        std::string acuteEffect;
        float       acuteThreshold   = 0.0f;
        float       acuteWindowHours = 0.0f;
        bool        acuteSet         = false;
        // blackout (§11): a higher-threshold tier on the same trailing potency window. No effect form — the
        // outcome (a registered scenario quest, or the built-in default fade+skip) owns the visuals.
        float       blackoutThreshold   = 0.0f;
        float       blackoutWindowHours = 0.0f;  // 0 = inherit the acute window
        bool        blackoutSet         = false;
        // explicit-set flags: an archetype fills only the curve params the config didn't pin.
        bool        gainSet        = false;
        bool        gainFalloffSet = false;
        bool        decaySet       = false;
        // A config gave this category an archetype / curve / effects (not just roster forms). ONLY
        // activated categories register — so AF core (forms only) does nothing until a config enables one.
        bool        activated      = false;
    };

    struct RawConsumable
    {
        std::string category;   // "none" = tombstone
        float       potency = 1.0f;
    };

    // ---- form / AV resolution helpers ----
    template <class T>
    T* ResolveForm(const std::string& a_ref)
    {
        const auto bar = a_ref.find('|');
        if (bar != std::string::npos) {
            const std::string plugin = a_ref.substr(0, bar);
            const std::string idStr  = a_ref.substr(bar + 1);
            const auto        localID = static_cast<RE::FormID>(std::strtoul(idStr.c_str(), nullptr, 16));
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                return dh->LookupForm<T>(localID, plugin);
            }
            return nullptr;
        }
        return RE::TESForm::LookupByEditorID<T>(a_ref);  // best-effort (needs a runtime EditorID cache)
    }

    bool LookupActorValue(const std::string& a_name, RE::ActorValue& a_out)
    {
        auto* avl = RE::ActorValueList::GetSingleton();
        if (!avl) {
            return false;
        }
        const RE::ActorValue av = avl->LookupActorValueByName(a_name.c_str());
        // On a MISS the engine returns kTotal (the count), NOT kNone (-1). Reject both, plus anything
        // out of range, so a typo'd av string is caught rather than registering a garbage value.
        if (av == RE::ActorValue::kNone || av >= RE::ActorValue::kTotal) {
            return false;
        }
        a_out = av;
        return true;
    }

    // Does any of the spell's magic effects carry this keyword? Used to mark an acute effect as an
    // intoxication (so IsIntoxicated is true only for Drunk/High/… and not e.g. Wired).
    bool SpellHasKeyword(RE::SpellItem* a_spell, RE::BGSKeyword* a_kw)
    {
        if (!a_spell || !a_kw) {
            return false;
        }
        for (auto* eff : a_spell->effects) {
            if (eff && eff->baseEffect) {
                for (auto* kw : eff->baseEffect->GetKeywords()) {
                    if (kw == a_kw) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Parse one {av,max} sub-effect object into `a_out`. Returns false if it isn't a valid sub-effect.
    bool ParseSub(const json& a_j, RawSubEffect& a_out)
    {
        if (!a_j.is_object() || !a_j.contains("av")) {
            return false;
        }
        a_out.av   = a_j.value("av", std::string{});
        a_out.max  = a_j.value("max", 0.0f);
        a_out.name = a_j.value("name", std::string{});
        return true;
    }

    // Parse one effect slot. Accepts (DESIGN §5.5 / Mechanism B):
    //   "none"                              → explicit no-effect tombstone (level still tracks)
    //   [ {av,max}, ... ]                   → a list of sub-effects
    //   {av,max}                            → a single sub-effect (shorthand)
    //   {effects:[...], name?, detrimental?}→ the full form with a name override / buff flag
    // Absence is handled by the caller (the slot inherits under the field-level merge).
    RawEffect ParseEffectSlot(const json& a_j, const std::string& a_cat, const char* a_which)
    {
        RawEffect e;

        if (a_j.is_string()) {
            const std::string s = a_j.get<std::string>();
            if (s != "none") {
                logger::warn("Config: category '{}' {} = '{}' is not a list/object or \"none\" — treating as "
                             "no effect.",
                             a_cat, a_which, s);
            }
            e.present   = true;
            e.tombstone = true;
            return e;
        }

        auto addSubsFrom = [&](const json& a_arrOrObj) {
            if (a_arrOrObj.is_array()) {
                for (auto& s : a_arrOrObj) {
                    RawSubEffect sub;
                    if (ParseSub(s, sub)) {
                        e.subs.push_back(sub);
                    } else {
                        logger::warn("Config: category '{}' {} has a malformed effect entry — skipped.", a_cat,
                                     a_which);
                    }
                }
            } else {
                RawSubEffect sub;
                if (ParseSub(a_arrOrObj, sub)) {
                    e.subs.push_back(sub);
                }
            }
        };

        if (a_j.is_array()) {  // bare list of {av,max}
            e.present = true;
            addSubsFrom(a_j);
            return e;
        }
        if (a_j.is_object()) {
            e.present     = true;
            e.name        = a_j.value("name", std::string{});
            e.detrimental = a_j.value("detrimental", true);
            if (a_j.contains("effects")) {  // full wrapper form
                addSubsFrom(a_j["effects"]);
            } else if (a_j.contains("av")) {  // single sub-effect shorthand
                RawSubEffect sub;
                ParseSub(a_j, sub);
                e.subs.push_back(sub);
            } else {
                logger::warn("Config: category '{}' {} object has neither 'effects' nor 'av' — ignored.", a_cat,
                             a_which);
                e.present = false;
            }
            return e;
        }

        logger::warn("Config: category '{}' {} has an unexpected value type — ignoring (slot inherits).", a_cat,
                     a_which);
        return e;
    }

    // Format a baked AddictionFramework.esm local FormID as the "Plugin|0xID" ref string ResolveForm reads.
    std::string AFRef(RE::FormID a_id)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(a_id));
        return std::string(kAFPlugin) + "|" + buf;
    }

    // Seed the baked roster into the raw maps — exactly what _AF_roster.json used to supply. Called BEFORE
    // the modder config files are merged, so a pack's archetype/effects layer field-wise on top of these
    // forms. Forms alone never activate a category (activated stays false); it goes inert until a config
    // provides an archetype + effects.
    void SeedRoster(std::map<std::string, RawCategory>& a_categories,
                    std::map<std::string, std::string>& a_acuteEffects, std::string& a_intoxKeyword)
    {
        for (const auto& r : kRoster) {
            RawCategory& cat    = a_categories[std::string(r.name)];
            cat.group           = AFRef(r.group);
            cat.levelGlobal     = AFRef(r.levelGlobal);
            cat.stageGlobal     = AFRef(r.stageGlobal);
            cat.addictionSpell  = AFRef(r.addictionSpell);
            cat.withdrawalSpell = AFRef(r.withdrawalSpell);
        }
        for (const auto& a : kAcuteLibrary) {
            a_acuteEffects[std::string(a.key)] = AFRef(a.spell);
        }
        a_intoxKeyword = AFRef(kIntoxicatedKeyword);
    }

    // Parse one file into the merged maps (last call wins). Roster forms (group/level/stage/containers,
    // the acute library, AF keywords) are NOT read from files — they come from the baked roster (SeedRoster).
    void MergeFile(const fs::path& a_path, std::map<std::string, RawCategory>& a_categories,
                   std::map<std::string, RawConsumable>& a_consumables,
                   std::map<std::string, ArchetypeCurve>& a_archetypes,
                   std::vector<std::string>& a_excludeQuests)
    {
        std::ifstream in(a_path);
        if (!in) {
            logger::warn("Config: could not open {}", a_path.string());
            return;
        }

        json j;
        try {
            in >> j;
        } catch (const std::exception& e) {
            logger::error("Config: JSON parse error in {}: {}", a_path.filename().string(), e.what());
            return;
        }

        // Named curve presets (DESIGN §7). A category adopts one via "archetype"; last-wins per slug.
        if (auto it = j.find("archetypes"); it != j.end() && it->is_object()) {
            for (auto& [slug, a] : it->items()) {
                ArchetypeCurve ac;
                ac.gain        = a.value("gain", ac.gain);
                ac.gainFalloff = a.value("gainFalloff", ac.gainFalloff);
                ac.decay       = a.value("decay", ac.decay);
                a_archetypes[slug] = ac;
            }
        }

        // Blackout safety: a global list of quests that make a blackout unsafe (teleporting the player would
        // break them). Accumulated (union) across all config files — any mod/pack can contribute.
        if (auto it = j.find("blackoutExcludeQuests"); it != j.end() && it->is_array()) {
            for (auto& q : *it) {
                if (q.is_string()) {
                    a_excludeQuests.push_back(q.get<std::string>());
                }
            }
        }

        if (auto it = j.find("categories"); it != j.end() && it->is_object()) {
            for (auto& [name, c] : it->items()) {
                if (!name.empty() && name[0] == '_') {
                    continue;  // allow "_comment"-style inline keys in the map
                }
                // Field-level merge INTO the existing category (last-wins per field, NOT whole-replace): an
                // absent key keeps the prior merged value. The roster manifest supplies the FORMS; a modder
                // pack supplies archetype + curve overrides + effects. `activated` is set by the latter — a
                // forms-only (manifest) entry stays inert. Curve params record explicit-set so an archetype
                // fills only what the config didn't pin. Effect slots are tri-state (see ParseEffectSlot).
                RawCategory& cat = a_categories[name];
                if (c.contains("archetype")) {
                    cat.archetype = c.value("archetype", cat.archetype);
                    cat.activated = true;
                }
                if (c.contains("gain")) {
                    cat.gain = c.value("gain", cat.gain);
                    cat.gainSet = cat.activated = true;
                }
                if (c.contains("gainFalloff")) {
                    cat.gainFalloff = c.value("gainFalloff", cat.gainFalloff);
                    cat.gainFalloffSet = cat.activated = true;
                }
                if (c.contains("decay")) {
                    cat.decay = c.value("decay", cat.decay);
                    cat.decaySet = cat.activated = true;
                }
                if (c.contains("addictionThreshold")) {
                    cat.addictionThreshold = c.value("addictionThreshold", cat.addictionThreshold);
                    cat.activated = true;
                }
                if (c.contains("toleranceHours")) {
                    cat.toleranceHours = c.value("toleranceHours", cat.toleranceHours);
                    cat.activated = true;
                }
                // Roster forms (group/levelGlobal/stageGlobal/addiction+withdrawal container spells) are
                // baked in and seeded before this merge — configs no longer supply them.
                if (auto a = c.find("addiction"); a != c.end()) {
                    cat.addiction = ParseEffectSlot(*a, name, "addiction");
                    cat.activated = true;
                }
                if (auto w = c.find("withdrawal"); w != c.end()) {
                    cat.withdrawal = ParseEffectSlot(*w, name, "withdrawal");
                    cat.activated = true;
                }
                // Acute status trigger (§7): { threshold, windowHours, effect }. `effect` = a named library
                // key or a "Plugin.esp|0xID" bring-your-own spell.
                if (auto ax = c.find("acute"); ax != c.end() && ax->is_object()) {
                    cat.acuteThreshold   = ax->value("threshold", cat.acuteThreshold);
                    cat.acuteWindowHours = ax->value("windowHours", cat.acuteWindowHours);
                    cat.acuteEffect      = ax->value("effect", cat.acuteEffect);
                    cat.acuteSet         = true;
                    cat.activated        = true;
                }
                // Blackout tier (§11): { threshold, windowHours? }. No 'effect' key — the outcome (a
                // registered scenario quest, or the default fade+skip) owns the visuals. An unset windowHours
                // inherits the acute window at resolve time. One blackout per bender (arm/re-arm; no cooldown).
                if (auto bx = c.find("blackout"); bx != c.end() && bx->is_object()) {
                    cat.blackoutThreshold   = bx->value("threshold", cat.blackoutThreshold);
                    cat.blackoutWindowHours = bx->value("windowHours", cat.blackoutWindowHours);
                    cat.blackoutSet         = true;
                    cat.activated           = true;
                }
            }
        }

        if (auto it = j.find("consumables"); it != j.end() && it->is_object()) {
            for (auto& [form, v] : it->items()) {
                if (!form.empty() && form[0] == '_') {
                    continue;  // allow "_comment"-style inline keys in the map
                }
                RawConsumable rc;
                if (v.is_string()) {
                    rc.category = v.get<std::string>();  // shorthand: category name (or "none")
                } else if (v.is_object()) {
                    rc.category = v.value("category", std::string{});
                    rc.potency  = v.value("potency", 1.0f);
                } else {
                    logger::warn("Config: consumable '{}' has an unexpected value type; skipping.", form);
                    continue;
                }
                a_consumables[form] = rc;  // last-wins (a later "none" tombstones an earlier assignment)
            }
        }
    }
}

namespace AddictionFramework
{
    void Config::Load()
    {
        std::error_code ec;
        if (!fs::exists(kConfigDir, ec)) {
            logger::info("Config: no config dir ({}) — nothing to load.", kConfigDir.string());
            return;
        }

        // 1) gather + sort files for deterministic last-loaded-wins.
        std::vector<fs::path> files;
        for (auto& entry : fs::directory_iterator(kConfigDir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        std::map<std::string, RawCategory>    rawCategories;
        std::map<std::string, RawConsumable>  rawConsumables;
        std::map<std::string, ArchetypeCurve> rawArchetypes;
        std::map<std::string, std::string>    rawAcuteEffects;  // named acute effect → spell formref
        std::string                           rawIntoxKeyword;  // AF_Intoxicated formref
        std::vector<std::string>              rawExcludeQuests; // blackout-safety quest exclusions (formrefs)

        // Baked roster first (name→forms, acute library, AF keyword); modder configs merge on top.
        SeedRoster(rawCategories, rawAcuteEffects, rawIntoxKeyword);

        for (auto& f : files) {
            logger::info("Config: loading {}", f.filename().string());
            MergeFile(f, rawCategories, rawConsumables, rawArchetypes, rawExcludeQuests);
        }

        // Blackout safety: resolve the excluded-quest formrefs → FormIDs, hand them to the BlackoutManager.
        std::vector<RE::FormID> excludeQuests;
        for (auto& ref : rawExcludeQuests) {
            if (auto* q = ResolveForm<RE::TESQuest>(ref)) {
                excludeQuests.push_back(q->GetFormID());
            } else {
                logger::error("Config: blackoutExcludeQuests '{}' did not resolve to a Quest — ignored.", ref);
            }
        }
        BlackoutManager::GetSingleton()->SetExcludeQuests(std::move(excludeQuests));

        // Resolve the AF_Intoxicated keyword (used to mark which acute effects count as intoxication).
        RE::BGSKeyword* intoxKeyword = rawIntoxKeyword.empty() ? nullptr : ResolveForm<RE::BGSKeyword>(rawIntoxKeyword);

        // 2) resolve categories → register with the manager; build name → (groupFormID, groupSpell).
        struct ResolvedCat
        {
            std::uint32_t  groupFormID = 0;
            RE::SpellItem* groupSpell  = nullptr;
        };
        std::map<std::string, ResolvedCat> byName;
        auto*                              mgr = AddictionManager::GetSingleton();

        for (auto& [name, rc] : rawCategories) {
            // AF core does nothing on its own: a category with only roster forms (no archetype/curve/
            // effects from a config) stays inert.
            if (!rc.activated) {
                logger::info("Config: category '{}' has roster forms but no archetype/effects — inert.", name);
                continue;
            }

            // Apply the named curve preset, then let any explicitly-set param override it (§7).
            if (!rc.archetype.empty()) {
                if (auto ait = rawArchetypes.find(rc.archetype); ait != rawArchetypes.end()) {
                    if (!rc.gainSet) {
                        rc.gain = ait->second.gain;
                    }
                    if (!rc.gainFalloffSet) {
                        rc.gainFalloff = ait->second.gainFalloff;
                    }
                    if (!rc.decaySet) {
                        rc.decay = ait->second.decay;
                    }
                } else {
                    logger::error("Config: category '{}' archetype '{}' not found — using default curve.", name,
                                  rc.archetype);
                }
            }

            // Bind the category string to its internal group token (an inert Type=Addiction spell): the
            // group FormID comes from the baked roster (SeedRoster). An empty `group` means a config named a
            // category with no baked roster entry (a typo, or an unrostered name) — nothing to drive; skip.
            RE::SpellItem* group = nullptr;
            if (rc.group.empty()) {
                logger::error("Config: category '{}' has no baked roster entry (unknown category) — skipping.",
                              name);
                continue;
            }
            group = ResolveForm<RE::SpellItem>(rc.group);
            if (!group) {
                logger::error("Config: category '{}' group '{}' did not resolve to a Spell (is "
                              "AddictionFramework.esm active?) — skipping category.",
                              name, rc.group);
                continue;
            }

            Category cat;
            cat.gain               = rc.gain;
            cat.gainFalloff        = rc.gainFalloff;
            cat.decay              = rc.decay;
            cat.addictionThreshold = rc.addictionThreshold;
            cat.toleranceHours     = rc.toleranceHours;

            auto buildEffect = [&](const RawEffect& re, const std::string& a_container, const char* which,
                                   const char* suffix) -> EffectDef {
                EffectDef def;
                // No effect: absent/unset, an explicit "none" tombstone, or an empty list. Level still
                // tracks; the tombstone is intentional (no warning).
                if (!re.present || re.tombstone || re.subs.empty()) {
                    return def;
                }
                // The container spell (a blank-pool spell in AddictionFramework.esm) holds the PVM effects
                // AF retargets from the sub-effect list. It's a category-level field ("<which>Spell").
                auto* spell = ResolveForm<RE::SpellItem>(a_container);
                if (!spell) {
                    logger::error("Config: category '{}' {} needs a container spell ('{}Spell') — '{}' did not "
                                  "resolve; effect skipped.",
                                  name, which, which, a_container);
                    return def;
                }
                std::vector<SubEffect> subs;
                for (auto& rs : re.subs) {
                    RE::ActorValue av;
                    if (!LookupActorValue(rs.av, av)) {
                        logger::error("Config: category '{}' {} av '{}' is not a valid actor value — effect "
                                      "skipped.",
                                      name, which, rs.av);
                        return def;  // valid stays false
                    }
                    subs.push_back(SubEffect{ av, rs.max, rs.name.empty() ? FriendlyAvName(rs.av) : rs.name });
                }
                def.spell       = spell->GetFormID();
                def.effects     = subs;
                def.detrimental = re.detrimental;
                def.valid       = true;

                // Configure the container's blank pool from the sub-effects: retarget each blank's actor
                // value, set the Detrimental sign, stamp AF's name, and hide the unused blanks (§5.1 / B).
                const std::string display = EffectName(name, suffix, re.name);
                EffectManager::GetSingleton()->ConfigureEffect(def.spell, subs, def.detrimental, display);
                return def;
            };
            cat.addiction  = buildEffect(rc.addiction, rc.addictionSpell, "addiction", "Addiction");
            cat.withdrawal = buildEffect(rc.withdrawal, rc.withdrawalSpell, "withdrawal", "Withdrawal");

            // Optional data-out globals (§9): AF drives Level (0–100) + Stage (0/1/2) each tick, the
            // canonical CK/OAR condition surface. Resolving needs AddictionFramework.esm active.
            if (!rc.levelGlobal.empty()) {
                if (auto* g = ResolveForm<RE::TESGlobal>(rc.levelGlobal)) {
                    cat.levelGlobal = g->GetFormID();
                } else {
                    logger::error("Config: category '{}' levelGlobal '{}' did not resolve — no Level channel.",
                                  name, rc.levelGlobal);
                }
            }
            if (!rc.stageGlobal.empty()) {
                if (auto* g = ResolveForm<RE::TESGlobal>(rc.stageGlobal)) {
                    cat.stageGlobal = g->GetFormID();
                } else {
                    logger::error("Config: category '{}' stageGlobal '{}' did not resolve — no Stage channel.",
                                  name, rc.stageGlobal);
                }
            }

            // Acute status (§7): resolve the selected effect — a named library key (from acuteEffects) OR a
            // bring-your-own "Plugin.esp|0xID" spell — then apply it when the trailing-window potency sum
            // crosses the threshold. acuteIsIntoxicated is derived from the effect's AF_Intoxicated keyword.
            if (rc.acuteSet) {
                if (rc.acuteThreshold <= 0.0f || rc.acuteWindowHours <= 0.0f) {
                    logger::error("Config: category '{}' acute needs threshold>0 and windowHours>0 (got "
                                  "{:.2f}/{:.2f}) — disabled.",
                                  name, rc.acuteThreshold, rc.acuteWindowHours);
                } else if (rc.acuteEffect.empty()) {
                    logger::error("Config: category '{}' acute block has no 'effect' — disabled.", name);
                } else {
                    // A named library key wins; otherwise treat the string as a bring-your-own formref.
                    std::string ref;
                    if (auto eit = rawAcuteEffects.find(rc.acuteEffect); eit != rawAcuteEffects.end()) {
                        ref = eit->second;
                    } else if (rc.acuteEffect.find('|') != std::string::npos) {
                        ref = rc.acuteEffect;  // bring-your-own Plugin.esp|0xID
                    }
                    if (ref.empty()) {
                        logger::error("Config: category '{}' acute effect '{}' is neither a known effect name "
                                      "nor a 'Plugin.esp|0xID' — no acute status.",
                                      name, rc.acuteEffect);
                    } else if (auto* sp = ResolveForm<RE::SpellItem>(ref)) {
                        cat.acuteSpell         = sp->GetFormID();
                        cat.acuteThreshold     = rc.acuteThreshold;
                        cat.acuteWindowHours   = rc.acuteWindowHours;
                        cat.acuteEnabled       = true;
                        cat.acuteIsIntoxicated = SpellHasKeyword(sp, intoxKeyword);
                    } else {
                        logger::error("Config: category '{}' acute effect '{}' did not resolve to a Spell — no "
                                      "acute status.",
                                      name, rc.acuteEffect);
                    }
                }
            }

            // Blackout tier (§11): a higher-threshold trigger on the same trailing potency window. No effect
            // form (the outcome owns the visuals); windowHours falls back to the acute window when unset.
            if (rc.blackoutSet) {
                const float window = rc.blackoutWindowHours > 0.0f ? rc.blackoutWindowHours : rc.acuteWindowHours;
                if (rc.blackoutThreshold <= 0.0f || window <= 0.0f) {
                    logger::error("Config: category '{}' blackout needs threshold>0 and a windowHours>0 (or an "
                                  "acute windowHours to inherit) — disabled.",
                                  name);
                } else {
                    cat.blackoutThreshold   = rc.blackoutThreshold;
                    cat.blackoutWindowHours = window;
                    cat.blackoutEnabled     = true;
                    logger::info("Config: category '{}' blackout enabled (threshold {:.2f} over {:.2f}h).",
                                 name, cat.blackoutThreshold, cat.blackoutWindowHours);
                }
            }

            const std::uint32_t groupFormID = group->GetFormID();
            mgr->RegisterCategory(groupFormID, cat);
            mgr->RegisterCategoryName(name, groupFormID);  // enables the master-free by-name API
            byName[name] = ResolvedCat{ groupFormID, group };
        }

        // 3) resolve consumables → stamp ENIT (or clear it for a "none" tombstone).
        int stamped = 0, tombstoned = 0;
        for (auto& [form, rc] : rawConsumables) {
            auto* alch = ResolveForm<RE::AlchemyItem>(form);
            if (!alch) {
                logger::error("Config: consumable '{}' did not resolve to an AlchemyItem — skipping.", form);
                continue;
            }

            if (rc.category == "none") {
                alch->data.addictionItem   = nullptr;  // pull it out entirely (tombstone beats any prior)
                alch->data.addictionChance = 0.0f;
                ++tombstoned;
                continue;
            }

            auto cit = byName.find(rc.category);
            if (cit == byName.end()) {
                logger::error("Config: consumable '{}' → unknown category '{}' — skipping.", form, rc.category);
                continue;
            }

            float potency = rc.potency;
            if (potency < 0.0f || potency > 1.0f) {
                logger::warn("Config: consumable '{}' potency {:.2f} out of [0,1] — clamping.", form, potency);
                potency = std::clamp(potency, 0.0f, 1.0f);
            }

            alch->data.addictionItem   = cit->second.groupSpell;  // group spell = the category key
            alch->data.addictionChance = potency;
            ++stamped;
        }

        logger::info("Config: registered {} categor(ies); stamped {} consumable(s), {} tombstoned.",
                     byName.size(), stamped, tombstoned);
    }
}
