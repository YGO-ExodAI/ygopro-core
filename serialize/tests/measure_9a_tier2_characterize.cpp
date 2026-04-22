// Phase P1 Primitive 1, Chunk 9a-Tier-2.0 — subunit characterization.
//
// Walks each card from the corpus that hit the chunk-9a Tier 1 stub
// for subunits/chain_lists (PROCESSOR_SUBUNIT class in the unsupported
// cards list). For each, builds a scripted_duel + new_card and
// inspects:
//   - core.units      (Tier 1 should already cover; sanity check)
//   - core.subunits   (Tier 2 target)
//   - core.tpchain / ntpchain / select_chains (also Tier 2 territory)
//
// For each non-empty list, classifies every entry by std::variant
// alternative index. Aggregates distribution across cards and per-card
// for the §14.x addendum.
//
// Output:
//   stdout: per-card classification + aggregate histograms
//   /tmp/9a_tier2_subunit_details.csv: card,list,position,variant_idx
//
// Prereq: /tmp/exodai_card_manifest.csv + /tmp/chunk6_refuse_details.csv
// (regenerate via build_corpus_refuse_manifest.py +
//  serialize_chunk6_corpus_measure).
//
// Build: xmake build serialize_9a_tier2_characterize
// Run:   xmake run serialize_9a_tier2_characterize

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <google/protobuf/stubs/common.h>

#include "ocgapi.h"
#include "ocgapi_types.h"
#include "duel.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "../processor_unit.h"

namespace {

constexpr const char* kManifestPath = "/tmp/exodai_card_manifest.csv";
constexpr const char* kRefuseCsvPath = "/tmp/chunk6_refuse_details.csv";
constexpr const char* kScriptsDir =
    "/mnt/c/Users/Joe/Documents/ExodAI/src/ygopro-scripts";
constexpr const char* kDetailsCsvPath = "/tmp/9a_tier2_subunit_details.csv";

// Variant alternative names — index matches std::variant<P::Adjust, P::Turn, ...>
// in processor_unit.h:745-761. Hand-maintained; if processor_unit.h's
// variant order changes, regenerate this list.
constexpr const char* kVariantNames[] = {
    "Adjust", "Turn", "RefreshLoc", "Startup",
    "SelectBattleCmd", "SelectIdleCmd", "SelectEffectYesNo", "SelectYesNo",
    "SelectOption", "SelectCard", "SelectCardCodes", "SelectUnselectCard",
    "SelectChain", "SelectPlace", "SelectDisField", "SelectPosition",
    "SelectTributeP", "SortChain", "SelectCounter", "SelectSum", "SortCard",
    "SelectRelease", "SelectTribute", "QuickEffect", "IdleCommand",
    "PhaseEvent", "PointEvent", "BattleCommand", "DamageStep", "ForcedBattle",
    "AddChain", "SolveChain", "SolveContinuous", "ExecuteCost",
    "ExecuteOperation", "ExecuteTarget", "Destroy", "Release", "SendTo",
    "DestroyReplace", "ReleaseReplace", "SendToReplace", "MoveToField",
    "ChangePos", "OperationReplace", "ActivateEffect", "SummonRule",
    "SpSummonRule", "SpSummon", "FlipSummon", "MonsterSet", "SpellSet",
    "SpSummonStep", "SpellSetGroup", "SpSummonRuleGroup", "Draw", "Damage",
    "Recover", "Equip", "GetControl", "SwapControl", "ControlAdjust",
    "SelfDestroyUnique", "SelfDestroy", "SelfToGrave", "TrapMonsterAdjust",
    "PayLPCost", "RemoveCounter", "AttackDisable", "AnnounceRace",
    "AnnounceAttribute", "AnnounceCard", "AnnounceNumber", "TossCoin",
    "TossDice", "RockPaperScissors", "SelectFusion", "DiscardHand",
    "DiscardDeck", "SortDeck", "RemoveOverlay", "XyzOverlay", "RefreshRelay",
};
constexpr size_t kVariantCount = sizeof(kVariantNames) / sizeof(kVariantNames[0]);

const char* variant_name(int idx) {
    return (idx >= 0 && static_cast<size_t>(idx) < kVariantCount)
        ? kVariantNames[idx] : "<unknown>";
}

struct CardEntry {
    uint32_t id, type, level, attribute;
    uint64_t race;
    int32_t atk, def_;
};
std::vector<CardEntry> g_manifest;

void noop_log(void*, const char*, int) {}
void noop_card_done(void*, OCG_CardData*) {}

void manifest_card_reader(void*, uint32_t code, OCG_CardData* data) {
    if (data == nullptr) return;
    std::memset(data, 0, sizeof(*data));
    for (const auto& v : g_manifest) {
        if (v.id == code) {
            data->code = v.id;
            data->type = v.type;
            data->level = v.level;
            data->attribute = v.attribute;
            data->race = v.race;
            data->attack = v.atk;
            data->defense = v.def_;
            return;
        }
    }
}

int script_reader(void*, OCG_Duel duel, const char* name) {
    const char* basename = std::strrchr(name, '/');
    basename = basename ? basename + 1 : name;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptsDir, basename);
    std::FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) return 0;
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20)) { std::fclose(fp); return 0; }
    std::vector<char> buf(static_cast<size_t>(len));
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        std::fclose(fp); return 0;
    }
    std::fclose(fp);
    return OCG_LoadScript(duel, buf.data(),
                          static_cast<uint32_t>(buf.size()), name);
}

OCG_Duel make_diag_duel(uint64_t seed) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed;
    opts.seed[1] = 0xC9A;
    opts.seed[2] = 0xCAFE;
    opts.seed[3] = 0xBABE;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &manifest_card_reader;
    opts.scriptReader = &script_reader;
    opts.logHandler = &noop_log;
    opts.cardReaderDone = &noop_card_done;
    OCG_Duel d = nullptr;
    if (OCG_CreateDuel(&d, &opts) != OCG_DUEL_CREATION_SUCCESS) return nullptr;
    script_reader(nullptr, d, "constant.lua");
    script_reader(nullptr, d, "utility.lua");
    return d;
}

void choose_placement(uint32_t type, uint32_t& loc, uint32_t& seq, uint32_t& pos) {
    constexpr uint32_t kSpell = 0x2;
    constexpr uint32_t kTrap = 0x4;
    constexpr uint32_t kField = 0x80000;
    constexpr uint32_t kMonster = 0x1;
    pos = 0x1;
    if (type & (kSpell | kTrap)) {
        loc = 0x8;
        seq = (type & kField) ? 5 : 0;
    } else if (type & kMonster) {
        loc = 0x4; seq = 0;
    } else { loc = 0x4; seq = 0; }
}

void load_manifest() {
    std::ifstream f(kManifestPath);
    if (!f.is_open()) {
        std::fprintf(stderr, "FATAL: cannot open %s\n", kManifestPath);
        std::exit(2);
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        CardEntry e{};
        std::istringstream is(line);
        is >> e.id >> e.type >> e.level >> e.attribute >> e.race
           >> e.atk >> e.def_;
        if (!is.fail()) g_manifest.push_back(e);
    }
}

std::vector<uint32_t> load_subunit_cards() {
    // Read /tmp/chunk6_refuse_details.csv; pick rows whose reason
    // mentions "chunk-9a Tier 1 stub: subunits" (PROCESSOR_SUBUNIT class).
    std::vector<uint32_t> codes;
    std::ifstream f(kRefuseCsvPath);
    if (!f.is_open()) {
        std::fprintf(stderr, "FATAL: cannot open %s\n", kRefuseCsvPath);
        std::exit(2);
    }
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) { header = false; continue; }
        auto c1 = line.find(',');
        auto c2 = line.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        uint32_t code = std::stoul(line.substr(0, c1));
        std::string reason = line.substr(c2 + 1);
        if (reason.find("chunk-9a Tier 1 stub: subunits") != std::string::npos ||
            reason.find("chunk-5a stub: ProcessorState") != std::string::npos) {
            codes.push_back(code);
        }
    }
    return codes;
}

const CardEntry* find_card(uint32_t code) {
    for (const auto& v : g_manifest) {
        if (v.id == code) return &v;
    }
    return nullptr;
}

// Returns the variant alternative indexes (in order) for every entry in
// a processor_list.
std::vector<int> classify_list(const std::list<processor_unit>& lst) {
    std::vector<int> out;
    for (const auto& u : lst) {
        out.push_back(static_cast<int>(u.index()));
    }
    return out;
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    load_manifest();
    auto target_codes = load_subunit_cards();
    std::printf("Tier 2.0 characterization: %zu candidate cards "
                "(PROCESSOR_SUBUNIT + legacy chunk-5a stub)\n",
                target_codes.size());

    std::FILE* csv = std::fopen(kDetailsCsvPath, "w");
    std::fprintf(csv, "card,list,position,variant_idx,variant_name\n");

    // Aggregate state
    std::map<int, size_t> subunit_variant_hist;
    std::map<int, size_t> tpchain_size_hist;
    std::map<int, size_t> ntpchain_size_hist;
    std::map<int, size_t> selchain_size_hist;
    std::map<size_t, size_t> subunit_size_hist;
    std::set<uint32_t> cards_with_subunits;
    std::set<uint32_t> cards_with_tpchain;
    std::set<uint32_t> cards_with_ntpchain;
    std::set<uint32_t> cards_with_selchain;
    size_t cards_processed = 0;
    size_t cards_no_state = 0;

    for (uint32_t code : target_codes) {
        const CardEntry* ce = find_card(code);
        if (ce == nullptr) continue;
        OCG_Duel handle = make_diag_duel(0xC9A0 + (code & 0xff));
        if (handle == nullptr) continue;
        uint32_t loc, seq, pos;
        choose_placement(ce->type, loc, seq, pos);
        OCG_NewCardInfo info{};
        info.code = code; info.loc = loc; info.seq = seq;
        info.pos = pos; info.team = 0; info.duelist = 0; info.con = 0;
        OCG_DuelNewCard(handle, &info);

        auto* d = static_cast<duel*>(handle);
        if (d->game_field == nullptr) {
            OCG_DestroyDuel(handle);
            continue;
        }
        const auto& core = d->game_field->core;

        bool any_state = false;
        if (!core.subunits.empty()) {
            any_state = true;
            cards_with_subunits.insert(code);
            subunit_size_hist[core.subunits.size()]++;
            auto idx_list = classify_list(core.subunits);
            for (size_t pos = 0; pos < idx_list.size(); ++pos) {
                int idx = idx_list[pos];
                subunit_variant_hist[idx]++;
                std::fprintf(csv, "%u,subunits,%zu,%d,%s\n",
                             code, pos, idx, variant_name(idx));
            }
        }
        if (!core.tpchain.empty()) {
            any_state = true;
            cards_with_tpchain.insert(code);
            tpchain_size_hist[core.tpchain.size()]++;
            std::fprintf(csv, "%u,tpchain,0,-1,<chain>\n", code);
        }
        if (!core.ntpchain.empty()) {
            any_state = true;
            cards_with_ntpchain.insert(code);
            ntpchain_size_hist[core.ntpchain.size()]++;
            std::fprintf(csv, "%u,ntpchain,0,-1,<chain>\n", code);
        }
        if (!core.select_chains.empty()) {
            any_state = true;
            cards_with_selchain.insert(code);
            selchain_size_hist[core.select_chains.size()]++;
            std::fprintf(csv, "%u,select_chains,0,-1,<chain>\n", code);
        }
        if (!any_state) ++cards_no_state;
        ++cards_processed;

        OCG_DestroyDuel(handle);
    }
    std::fclose(csv);

    std::printf("\n=== Aggregate ===\n");
    std::printf("  Cards processed:                %zu\n", cards_processed);
    std::printf("  Cards no state populated:       %zu\n", cards_no_state);
    std::printf("  Cards with subunits:            %zu\n", cards_with_subunits.size());
    std::printf("  Cards with tpchain populated:   %zu\n", cards_with_tpchain.size());
    std::printf("  Cards with ntpchain populated:  %zu\n", cards_with_ntpchain.size());
    std::printf("  Cards with select_chains pop:   %zu\n", cards_with_selchain.size());

    std::printf("\n=== Subunit variant distribution ===\n");
    {
        std::vector<std::pair<int, size_t>> sorted(
            subunit_variant_hist.begin(), subunit_variant_hist.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [idx, cnt] : sorted) {
            std::printf("  %5zu × %s (variant_idx=%d)\n", cnt,
                        variant_name(idx), idx);
        }
    }
    std::printf("\n=== Subunits length histogram ===\n");
    for (const auto& [len, cnt] : subunit_size_hist) {
        std::printf("  size=%zu : %zu cards\n", len, cnt);
    }
    if (!tpchain_size_hist.empty()) {
        std::printf("\n=== tpchain length histogram ===\n");
        for (const auto& [len, cnt] : tpchain_size_hist) {
            std::printf("  size=%d : %zu cards\n", len, cnt);
        }
    }
    if (!ntpchain_size_hist.empty()) {
        std::printf("\n=== ntpchain length histogram ===\n");
        for (const auto& [len, cnt] : ntpchain_size_hist) {
            std::printf("  size=%d : %zu cards\n", len, cnt);
        }
    }
    if (!selchain_size_hist.empty()) {
        std::printf("\n=== select_chains length histogram ===\n");
        for (const auto& [len, cnt] : selchain_size_hist) {
            std::printf("  size=%d : %zu cards\n", len, cnt);
        }
    }

    std::printf("\nPer-card details: %s\n", kDetailsCsvPath);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
