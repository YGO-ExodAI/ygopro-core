// Phase P1 Primitive 1, Chunk 6 — corpus refuse-rate measurement.
//
// Iterates over every card in /tmp/exodai_card_manifest.csv (produced by
// data_pipeline/build_corpus_refuse_manifest.py from cards.cdb intersected
// with src/ygopro-scripts/c<id>.lua presence). For each card:
//
//   1. Create a fresh scripted duel (constant.lua + utility.lua bootstrapped).
//   2. OCG_DuelNewCard with the real type bits — fires initial_effect,
//      registering whatever closures the script wants.
//   3. OCG_DuelSaveState (or, for refusal-reason recovery, the C++
//      serialize_duel entry point).
//   4. Tally the outcome.
//
// Outcomes:
//   OK                            — save succeeded
//   REFUSE_UNKNOWN_UPVALUE        — chunk-5b §13.4 refuse path (function /
//                                   table / userdata-with-no-engine-handle
//                                   upvalue captured by a closure)
//   REFUSE_OTHER                  — any other refuse status (NOT_MSG_BOUNDARY,
//                                   UNSAFE_LUA, INTERNAL stub-trip, ...)
//   NEW_CARD_FAILED               — initial_effect threw / interpreter error
//                                   / engine refused to allocate the card.
//                                   Excluded from the refuse-rate denominator
//                                   (it's measurement loss, not signal).
//
// Refuse rate (per §11.1):
//     refuse_rate = (REFUSE_UNKNOWN_UPVALUE + REFUSE_OTHER)
//                   / (OK + REFUSE_UNKNOWN_UPVALUE + REFUSE_OTHER)
//
// The denominator excludes NEW_CARD_FAILED so the rate isn't biased by
// cards we failed to even attempt.
//
// Methodology limit (read alongside the report): this is a script-corpus
// proxy. Each measurement is a single-card synthetic state, not a true
// MSG-boundary state from a real game. The full §8.6 replay-corpus
// measurement runs at end of Chunk 9 (gated on Chunk 7 Python bindings +
// replay decoder).
//
// Build: xmake build serialize_chunk6_corpus_measure
// Run:   xmake run serialize_chunk6_corpus_measure
//   - Optional first arg: max cards to process (default: all from manifest)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/stubs/common.h>

#include "ocgapi.h"
#include "ocgapi_types.h"
#include "duel.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "save_state.h"
#include "load_state.h"
#include "ocg_state.pb.h"

namespace {

constexpr const char* kManifestPath = "/tmp/exodai_card_manifest.csv";
constexpr const char* kScriptsDir =
    "/mnt/c/Users/Joe/Documents/ExodAI/src/ygopro-scripts";

struct CardEntry {
    uint32_t id;
    uint32_t type;
    uint32_t level;
    uint32_t attribute;
    uint64_t race;
    int32_t atk;
    int32_t def_;
};

std::vector<CardEntry> load_manifest() {
    std::vector<CardEntry> out;
    std::ifstream f(kManifestPath);
    if (!f.is_open()) {
        std::fprintf(stderr,
            "FATAL: cannot open %s\n"
            "  Run: python3 data_pipeline/build_corpus_refuse_manifest.py\n",
            kManifestPath);
        std::exit(2);
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        CardEntry e{};
        std::istringstream is(line);
        is >> e.id >> e.type >> e.level >> e.attribute >> e.race
           >> e.atk >> e.def_;
        if (is.fail()) {
            std::fprintf(stderr, "  manifest line skipped: %s\n", line.c_str());
            continue;
        }
        out.push_back(e);
    }
    return out;
}

// Card data for OCG_CreateDuel's cardReader callback. Look up in the
// manifest so initial_effect sees the same metadata as the live game.
const std::vector<CardEntry>* g_card_table = nullptr;

void corpus_card_reader(void* /*payload*/, uint32_t code, OCG_CardData* data) {
    if (data == nullptr) return;
    std::memset(data, 0, sizeof(*data));
    if (g_card_table == nullptr) return;
    for (const auto& v : *g_card_table) {
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

int corpus_script_reader(void* /*payload*/, OCG_Duel duel, const char* name) {
    const char* basename = std::strrchr(name, '/');
    basename = basename ? basename + 1 : name;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptsDir, basename);
    std::FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) return 0;
    std::fseek(fp, 0, SEEK_END);
    const long len = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20)) {
        std::fclose(fp);
        return 0;
    }
    std::vector<char> buf(static_cast<size_t>(len));
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        std::fclose(fp);
        return 0;
    }
    std::fclose(fp);
    return OCG_LoadScript(duel, buf.data(), static_cast<uint32_t>(buf.size()),
                          name);
}

void noop_log(void*, const char*, int) {}
void noop_card_done(void*, OCG_CardData*) {}

OCG_Duel make_corpus_duel(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xC6;
    opts.seed[2] = 0xCAFE;
    opts.seed[3] = 0xBABE;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &corpus_card_reader;
    opts.scriptReader = &corpus_script_reader;
    opts.logHandler = &noop_log;
    opts.cardReaderDone = &noop_card_done;
    OCG_Duel duel = nullptr;
    if (OCG_CreateDuel(&duel, &opts) != OCG_DUEL_CREATION_SUCCESS) {
        return nullptr;
    }
    corpus_script_reader(nullptr, duel, "constant.lua");
    corpus_script_reader(nullptr, duel, "utility.lua");
    return duel;
}

// Pick a placement location based on type bits. Field/Continuous/Spell/
// Trap go in SZONE; Monster goes in MZONE; Field Spell goes in FZONE
// (slot 5 of SZONE per ocgcore).
void choose_placement(uint32_t type, uint32_t& loc, uint32_t& seq, uint32_t& pos) {
    constexpr uint32_t kTypeMonster = 0x1;
    constexpr uint32_t kTypeSpell   = 0x2;
    constexpr uint32_t kTypeTrap    = 0x4;
    constexpr uint32_t kTypeField   = 0x80000;
    pos = 0x1;  // POS_FACEUP_ATTACK
    if (type & (kTypeSpell | kTypeTrap)) {
        loc = 0x8;  // LOCATION_SZONE
        seq = (type & kTypeField) ? 5 : 0;
    } else if (type & kTypeMonster) {
        loc = 0x4;  // LOCATION_MZONE
        seq = 0;
    } else {
        // Unknown — try MZONE as a default. If new_card rejects, we tally
        // NEW_CARD_FAILED and move on.
        loc = 0x4;
        seq = 0;
    }
}

enum class Outcome {
    OK,
    REFUSE_UNKNOWN_UPVALUE,
    REFUSE_OTHER,
    NEW_CARD_FAILED,
};

struct PerCardResult {
    uint32_t code;
    Outcome outcome;
    std::string detail;  // refuse_reason for REFUSE_*
};

PerCardResult measure_one(const CardEntry& e) {
    PerCardResult r{e.id, Outcome::NEW_CARD_FAILED, {}};
    OCG_Duel handle = make_corpus_duel(0xC600 + (e.id & 0xff));
    if (handle == nullptr) {
        r.detail = "OCG_CreateDuel failed";
        return r;
    }

    uint32_t loc = 0, seq = 0, pos = 0;
    choose_placement(e.type, loc, seq, pos);

    OCG_NewCardInfo info{};
    info.team = 0;
    info.duelist = 0;
    info.code = e.id;
    info.con = 0;
    info.loc = loc;
    info.seq = seq;
    info.pos = pos;
    OCG_DuelNewCard(handle, &info);

    auto* d = static_cast<duel*>(handle);
    if (d->cards.size() < 2) {
        // The scripted_duel pattern: temp_card is allocated by field
        // ctor (cardid=1). new_card on success appends a 2nd card. If
        // we don't see one, new_card silently dropped the request — most
        // likely an interpreter error during initial_effect.
        r.detail = "no card materialized post-new_card";
        OCG_DestroyDuel(handle);
        return r;
    }

    // Save via the C++ entry point so we can recover refuse_reason.
    std::string out, reason;
    auto status = ocg::serialize::serialize_duel(*d, &out, &reason);
    OCG_DestroyDuel(handle);

    if (status == OCG_SAVE_OK) {
        r.outcome = Outcome::OK;
    } else if (status == OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE) {
        r.outcome = Outcome::REFUSE_UNKNOWN_UPVALUE;
        r.detail = reason;
    } else {
        r.outcome = Outcome::REFUSE_OTHER;
        r.detail = "status=" + std::to_string(status) + " reason=" + reason;
    }
    return r;
}

void print_summary(const std::vector<PerCardResult>& results) {
    size_t ok = 0, refuse_unk = 0, refuse_other = 0, new_card_failed = 0;
    for (const auto& r : results) {
        switch (r.outcome) {
            case Outcome::OK: ++ok; break;
            case Outcome::REFUSE_UNKNOWN_UPVALUE: ++refuse_unk; break;
            case Outcome::REFUSE_OTHER: ++refuse_other; break;
            case Outcome::NEW_CARD_FAILED: ++new_card_failed; break;
        }
    }

    const size_t denom = ok + refuse_unk + refuse_other;
    const double refuse_rate = denom > 0
        ? 100.0 * (refuse_unk + refuse_other) / denom : 0.0;
    const double refuse_unk_rate = denom > 0
        ? 100.0 * refuse_unk / denom : 0.0;

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  Chunk 6 corpus refuse-rate measurement\n");
    std::printf("================================================================\n");
    std::printf("  Cards in manifest:           %zu\n", results.size());
    std::printf("  ----------------------------------------------------------------\n");
    std::printf("  OK (saved successfully):     %zu  (%.2f%% of attempted)\n",
                ok, denom > 0 ? 100.0 * ok / denom : 0.0);
    std::printf("  REFUSE_UNKNOWN_UPVALUE:      %zu  (%.2f%% of attempted)\n",
                refuse_unk, refuse_unk_rate);
    std::printf("  REFUSE_OTHER:                %zu  (%.2f%% of attempted)\n",
                refuse_other,
                denom > 0 ? 100.0 * refuse_other / denom : 0.0);
    std::printf("  NEW_CARD_FAILED (excluded):  %zu  (%.2f%% of total)\n",
                new_card_failed,
                100.0 * new_card_failed / results.size());
    std::printf("  ----------------------------------------------------------------\n");
    std::printf("  Refuse rate (denominator: attempted saves): %.2f%%\n",
                refuse_rate);
    std::printf("    of which UNKNOWN_UPVALUE: %.2f%%\n", refuse_unk_rate);
    std::printf("================================================================\n");

    // §11.1 ladder classification
    const char* band;
    if (refuse_rate < 5.0)       band = "<5% — proceed as planned";
    else if (refuse_rate < 20.0) band = "5-20% — audit + widen allow-list";
    else if (refuse_rate < 50.0) band = "20-50% — STOP, full audit, promote escape hatch";
    else                         band = ">50% — Strategy A-refined not viable; promote Strategy C";
    std::printf("  §11.1 band: %s\n", band);
    std::printf("================================================================\n");

    // Top-N refuse reasons by frequency for diagnosis.
    std::map<std::string, size_t> reason_counts;
    for (const auto& r : results) {
        if (r.outcome == Outcome::REFUSE_UNKNOWN_UPVALUE ||
            r.outcome == Outcome::REFUSE_OTHER) {
            // Strip the function source location to coalesce variants —
            // refuse_reason includes "function source=[string \"cN.lua\"] line=K"
            // which is per-card; we want the common-prefix counted once.
            std::string key = r.detail;
            auto pos = key.find("function source=");
            if (pos != std::string::npos) key.resize(pos);
            // Trim trailing semicolon + spaces
            while (!key.empty() &&
                   (key.back() == ' ' || key.back() == ';')) {
                key.pop_back();
            }
            reason_counts[key]++;
        }
    }

    if (!reason_counts.empty()) {
        std::printf("\n  Top refuse-reason classes (source location stripped):\n");
        std::vector<std::pair<std::string, size_t>> sorted(
            reason_counts.begin(), reason_counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        const size_t top_n = std::min<size_t>(15, sorted.size());
        for (size_t i = 0; i < top_n; ++i) {
            std::printf("    %4zu × %s\n",
                        sorted[i].second, sorted[i].first.c_str());
        }
    }

    // First 20 NEW_CARD_FAILED cards for diagnosis (measurement-loss audit)
    size_t shown = 0;
    for (const auto& r : results) {
        if (r.outcome == Outcome::NEW_CARD_FAILED) {
            if (shown == 0) {
                std::printf("\n  Sample NEW_CARD_FAILED cards (first 20):\n");
            }
            std::printf("    c%u — %s\n", r.code, r.detail.c_str());
            if (++shown >= 20) break;
        }
    }

    // Dump per-card refuse details to CSV for offline analysis. One line
    // per refused card: code, outcome, refuse_reason. Lets us slice by
    // upvalue type / slot / class without re-running the 7-minute corpus
    // sweep.
    constexpr const char* kDetailsCsv = "/tmp/chunk6_refuse_details.csv";
    std::FILE* out = std::fopen(kDetailsCsv, "w");
    if (out) {
        std::fprintf(out, "code,outcome,reason\n");
        for (const auto& r : results) {
            const char* outcome_str = "?";
            switch (r.outcome) {
                case Outcome::OK: continue;  // skip OKs to keep file small
                case Outcome::REFUSE_UNKNOWN_UPVALUE: outcome_str = "UNK_UPVAL"; break;
                case Outcome::REFUSE_OTHER:           outcome_str = "OTHER"; break;
                case Outcome::NEW_CARD_FAILED:        outcome_str = "NEW_CARD_FAILED"; break;
            }
            // Quote the reason to handle commas inside the message.
            std::string escaped = r.detail;
            for (auto& c : escaped) if (c == '"') c = '\'';
            std::fprintf(out, "%u,%s,\"%s\"\n",
                         r.code, outcome_str, escaped.c_str());
        }
        std::fclose(out);
        std::printf("\n  Per-card refuse details written to %s\n", kDetailsCsv);
    }
}

}  // namespace

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    auto manifest = load_manifest();
    std::printf("loaded %zu cards from manifest\n", manifest.size());

    size_t max_cards = manifest.size();
    if (argc >= 2) {
        max_cards = std::min(max_cards,
                             static_cast<size_t>(std::atoll(argv[1])));
        std::printf("limiting to first %zu (CLI override)\n", max_cards);
    }

    g_card_table = &manifest;

    std::vector<PerCardResult> results;
    results.reserve(max_cards);

    const size_t progress_every = std::max<size_t>(100, max_cards / 50);
    for (size_t i = 0; i < max_cards; ++i) {
        results.push_back(measure_one(manifest[i]));
        if ((i + 1) % progress_every == 0) {
            std::printf("  progress: %zu / %zu\n", i + 1, max_cards);
            std::fflush(stdout);
        }
    }

    print_summary(results);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
