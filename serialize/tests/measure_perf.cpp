// Phase P1 Primitive 1, Chunk 10 — save/load perf benchmarks.
//
// Measures save + load timing across a range of fixtures:
//   - Vanilla (chunk 5a): 2 cards, no effects — size baseline
//   - Type-C single card (chunk 5b/5c): Dueltaining, Branded, Hydor,
//     Vendread — medium-size blobs with Lua bytecode
//   - Tier 2 subunit (chunk 9a): Earthbound Immortal Aslla piscu,
//     Advanced Crystal Beast Amber Mammoth — small + processor state
//   - Envpool boundary (chunk 9a Tier 1): post-StartDuel vanilla
//     duel — units stack populated, full 2-player-5-card state
//
// For each fixture: N=100 save iterations + N=100 load iterations.
// Reports median / p95 / p99 / max for wall_ms plus blob size bytes.
//
// Warm/cold cache: deliberately NOT measured here — bytecode_cache
// scaffolding is scoped per-duel (each OCG_CreateDuel gets a fresh
// cache). Cross-duel warming would require bytecode-cache persistence
// across duels, which is chunk-10 scope but not yet wired. Documented
// as a §10 followup.
//
// Budgets per plan §9.2:
//   save p95 < 5ms (p99 < 20ms)
//   load p95 < 50ms (p99 < 200ms)
//
// Build: xmake build serialize_perf_measure
// Run:   xmake run serialize_perf_measure

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/stubs/common.h>

#include "ocgapi.h"
#include "ocgapi_types.h"

namespace {

constexpr const char* kManifestPath = "/tmp/exodai_card_manifest.csv";
constexpr const char* kScriptsDir =
    "/mnt/c/Users/Joe/Documents/ExodAI/src/ygopro-scripts";

struct CardEntry {
    uint32_t id, type, level, attribute;
    uint64_t race;
    int32_t atk, def_;
};
std::vector<CardEntry> g_manifest;

// Stats
struct Stats {
    double median_ms, p95_ms, p99_ms, max_ms, min_ms;
    double mean_ms;
    size_t n;
};

Stats compute_stats(std::vector<double>& xs) {
    Stats s{};
    s.n = xs.size();
    if (xs.empty()) return s;
    std::sort(xs.begin(), xs.end());
    auto pick = [&](double p) {
        size_t i = static_cast<size_t>(p * xs.size());
        if (i >= xs.size()) i = xs.size() - 1;
        return xs[i];
    };
    s.min_ms = xs.front();
    s.max_ms = xs.back();
    s.median_ms = pick(0.5);
    s.p95_ms = pick(0.95);
    s.p99_ms = pick(0.99);
    double sum = 0; for (double x : xs) sum += x;
    s.mean_ms = sum / xs.size();
    return s;
}

void print_stats(const char* label, const Stats& s) {
    std::printf("    %-24s min=%.3f med=%.3f mean=%.3f p95=%.3f p99=%.3f max=%.3f ms\n",
                label, s.min_ms, s.median_ms, s.mean_ms,
                s.p95_ms, s.p99_ms, s.max_ms);
}

// Card reader / script reader for scripted fixtures.
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

OCG_DuelOptions make_opts(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xC10;
    opts.seed[2] = 0xCAFE;
    opts.seed[3] = 0xBABE;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &manifest_card_reader;
    opts.scriptReader = &script_reader;
    opts.logHandler = &noop_log;
    opts.cardReaderDone = &noop_card_done;
    return opts;
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

// Build a fixture duel for a specific single card code. Used for
// chunk-5b/5c/9a-T2 fixtures.
OCG_Duel build_single_card_duel(uint32_t code, uint64_t seed) {
    OCG_DuelOptions opts = make_opts(seed);
    opts.enableUnsafeLibraries = 1;
    OCG_Duel d = nullptr;
    if (OCG_CreateDuel(&d, &opts) != OCG_DUEL_CREATION_SUCCESS) return nullptr;
    script_reader(nullptr, d, "constant.lua");
    script_reader(nullptr, d, "utility.lua");
    for (const auto& e : g_manifest) {
        if (e.id == code) {
            uint32_t loc, seq, pos;
            choose_placement(e.type, loc, seq, pos);
            OCG_NewCardInfo info{};
            info.code = code; info.loc = loc; info.seq = seq;
            info.pos = pos; info.team = 0; info.duelist = 0; info.con = 0;
            OCG_DuelNewCard(d, &info);
            break;
        }
    }
    return d;
}

// Benchmark save + load on a pre-built duel. Takes N timing samples.
// load runs on a fresh duel each time (cold cache).
void bench_fixture(const char* label, OCG_Duel duel,
                   OCG_DuelOptions load_opts, int n) {
    if (duel == nullptr) {
        std::printf("  %s: duel==nullptr, skipping\n", label);
        return;
    }
    // One save to get size + sanity check
    void* blob0 = nullptr; uint32_t size0 = 0;
    int s = OCG_DuelSaveState(duel, &blob0, &size0);
    if (s != 0) {
        std::printf("  %s: save refused (status=%d), skipping\n", label, s);
        if (blob0) OCG_FreeSaveBuffer(blob0);
        return;
    }
    std::string blob_copy(static_cast<const char*>(blob0), size0);
    OCG_FreeSaveBuffer(blob0);

    std::printf("\n  === %s (blob=%u bytes) ===\n", label, size0);

    // Save timing
    std::vector<double> save_ms;
    save_ms.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        void* blob = nullptr; uint32_t size = 0;
        OCG_DuelSaveState(duel, &blob, &size);
        auto t1 = std::chrono::steady_clock::now();
        OCG_FreeSaveBuffer(blob);
        save_ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    auto save_stats = compute_stats(save_ms);
    print_stats("save", save_stats);

    // Load timing (cold cache — fresh duel each iteration)
    std::vector<double> load_ms;
    load_ms.reserve(n);
    for (int i = 0; i < n; ++i) {
        OCG_DuelOptions opts = load_opts;
        OCG_Duel loaded = nullptr;
        auto t0 = std::chrono::steady_clock::now();
        OCG_DuelLoadState(blob_copy.data(),
                          static_cast<uint32_t>(blob_copy.size()),
                          &opts, &loaded);
        auto t1 = std::chrono::steady_clock::now();
        if (loaded) OCG_DestroyDuel(loaded);
        load_ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    auto load_stats = compute_stats(load_ms);
    print_stats("load (cold)", load_stats);

    // Budget check
    bool save_ok = save_stats.p95_ms < 5.0;
    bool load_ok = load_stats.p95_ms < 50.0;
    std::printf("    budget: save p95 %.3fms < 5.0ms ? %s ; "
                "load p95 %.3fms < 50ms ? %s\n",
                save_stats.p95_ms, save_ok ? "OK" : "MISS",
                load_stats.p95_ms, load_ok ? "OK" : "MISS");
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    load_manifest();

    constexpr int N = 100;
    std::printf("=== Chunk 10 perf bench (N=%d per fixture) ===\n", N);
    std::printf("Budgets per plan §9.2: save p95 < 5ms, load p95 < 50ms\n");

    OCG_DuelOptions load_opts = make_opts(0);
    load_opts.enableUnsafeLibraries = 1;

    // Fixture 1: vanilla 2-card duel (chunk 5a) — minimal size reference
    {
        OCG_Duel d = nullptr;
        OCG_DuelOptions opts = make_opts(0x5A01);
        opts.enableUnsafeLibraries = 1;
        OCG_CreateDuel(&d, &opts);
        bench_fixture("chunk-5a vanilla (no cards)", d, load_opts, N);
        OCG_DestroyDuel(d);
    }

    // Fixture 2-5: Type-C single-card fixtures
    struct Fix { const char* name; uint32_t code; };
    Fix type_c[] = {
        {"chunk-5b Dueltaining (c19162134)", 19162134},
        {"chunk-5b Branded (c14220547)", 14220547},
        {"chunk-5b Hydor (c30339825)", 30339825},
        {"chunk-5c Vendread Reunion (c2266498)", 2266498},
        {"chunk-5c Magnum (c43227, C-fn-in-table)", 43227},
        {"chunk-9a-T2 Earthbound Aslla (c10875327, SelfDestroy)", 10875327},
        {"chunk-9a-T2 ACB Amber Mammoth (c18847598, SelfToGrave)", 18847598},
    };
    for (auto& f : type_c) {
        OCG_Duel d = build_single_card_duel(f.code, 0xC100 + f.code);
        bench_fixture(f.name, d, load_opts, N);
        if (d) OCG_DestroyDuel(d);
    }

    // Fixture: envpool-style boundary — vanilla duel + StartDuel + drive
    // to first pause. Matches chunk-9a's test_chunk9a_processor_state_
    // round_trip fixture.
    {
        OCG_DuelOptions opts = make_opts(0x9A1E1);
        opts.enableUnsafeLibraries = 1;
        OCG_Duel d = nullptr;
        OCG_CreateDuel(&d, &opts);
        script_reader(nullptr, d, "constant.lua");
        script_reader(nullptr, d, "utility.lua");
        // Populate a simple deck via scripted helper
        // (mirrors test_save's populate_simple_deck logic, inline-minimal)
        auto add_card = [&](uint32_t code, uint8_t team, uint32_t loc, uint32_t seq) {
            OCG_NewCardInfo info{};
            info.code = code; info.team = team; info.duelist = 0;
            info.con = team; info.loc = loc; info.seq = seq; info.pos = 0x1;
            OCG_DuelNewCard(d, &info);
        };
        constexpr uint32_t LOC_DECK = 0x1;
        // 9 cards per player — the chunk-5a populate_simple_deck
        // vanilla pattern.
        for (uint8_t t = 0; t < 2; ++t) {
            for (uint32_t i = 0; i < 9; ++i) {
                add_card(4031928 + i, t, LOC_DECK, 0);
            }
        }
        OCG_StartDuel(d);
        // Drive to first pause
        while (true) {
            int status = OCG_DuelProcess(d);
            uint32_t len = 0; OCG_DuelGetMessage(d, &len);
            if (status == OCG_DUEL_STATUS_END ||
                status == OCG_DUEL_STATUS_AWAITING) break;
        }
        bench_fixture("chunk-9a-T1 envpool boundary (post-StartDuel)",
                       d, load_opts, N);
        OCG_DestroyDuel(d);
    }

    std::printf("\n=== Summary ===\n");
    std::printf("Perf report will be written to "
                "src/docs/phase_p1_primitive_1_perf.md\n");

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
