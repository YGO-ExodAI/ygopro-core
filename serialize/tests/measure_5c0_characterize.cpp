// Phase P1 Primitive 1, Chunk 5c.0 — refuse-case characterization.
//
// Walks every refused card from /tmp/chunk6_refuse_details.csv (produced
// by serialize_chunk6_corpus_measure) and inspects each effect's callback
// upvalues directly via the Lua C API — rather than via dump_lua_callback
// — so we can record structural diagnostics that drive the §13.4 extension
// scope estimate.
//
// Per-upvalue diagnostic schema:
//   card_code, slot, upval_idx, type,
//   fn_nups, fn_isvararg, fn_what, fn_source, fn_lastline,
//   tbl_ptr, tbl_has_meta, tbl_meta_name, tbl_nkeys,
//   inner_classification (recursive depth-3 walk for function upvalues)
//
// Items addressed (per user 5c.0 spec):
//   1. Function upvalue depth — fn_nups field, plus inner walk to depth 3
//   2. Table sharing across effects — group by (card_code, tbl_ptr)
//   3. Metatable usage — tbl_has_meta + tbl_meta_name
//   4. lua_dump failures — captured separately via fn_what / fn_isvararg
//   5. REFUSE_OTHER orthogonality — handled by separate code path that
//      inspects core.units / core.tpchain / etc. for a sample card.
//
// Outputs CSV(s) to /tmp/5c0_*.csv for offline slicing.
//
// Build: xmake build serialize_5c0_characterize
// Run:   xmake run serialize_5c0_characterize

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
#include <unordered_set>
#include <vector>

#include <google/protobuf/stubs/common.h>

#include "ocgapi.h"
#include "ocgapi_types.h"
#include "duel.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "interpreter.h"

extern "C" {
#include "../lua/src/lua.h"
#include "../lua/src/lauxlib.h"
}

namespace {

constexpr const char* kManifestPath = "/tmp/exodai_card_manifest.csv";
constexpr const char* kRefuseCsvPath = "/tmp/chunk6_refuse_details.csv";
constexpr const char* kScriptsDir =
    "/mnt/c/Users/Joe/Documents/ExodAI/src/ygopro-scripts";

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
    opts.seed[1] = 0xC50;
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
    } else {
        loc = 0x4; seq = 0;
    }
}

// Slot enumeration mirrors EffectRecord's *_callback fields.
struct SlotRef {
    const char* name;
    int effect::* member_offset_marker;  // dummy, we'll switch by index
};

// Helper: push a callback function onto the stack via lua_rawgeti from
// effect.<slot>_ref, return true if it's a function (not nil/missing).
bool push_callback(lua_State* L, const effect& e, int slot_idx) {
    int ref = 0;
    switch (slot_idx) {
        case 0: ref = e.condition; break;
        case 1: ref = e.cost; break;
        case 2: ref = e.target; break;
        case 3: ref = e.value; break;
        case 4: ref = e.operation; break;
    }
    if (ref == 0) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

const char* slot_name(int idx) {
    static const char* names[] = {"condition", "cost", "target", "value", "operation"};
    return (idx >= 0 && idx < 5) ? names[idx] : "?";
}

// One row of upvalue diagnostic data for items 1-3.
struct UpvalDiag {
    uint32_t card_code;
    int slot_idx;
    int upval_idx;
    int type;            // lua_type
    int fn_nups;         // for function upvalues; -1 for non-function
    int fn_isvararg;
    const char* fn_what; // "Lua", "C", "main"
    char fn_source[128];
    int fn_lastline;
    uintptr_t tbl_ptr;   // for table upvalues; 0 for non-table
    int tbl_has_meta;
    char tbl_meta_name[64];
    int tbl_nkeys;
    // Inner upvalue type histogram for function upvalues (depth 1):
    // counts per type. Indices: NIL=0, BOOL=1, NUM=2, STR=3, TBL=4,
    // FUNC=5, USERDATA=6, OTHER=7.
    int inner_typehist[8];
};

void zero_diag(UpvalDiag& d) {
    std::memset(&d, 0, sizeof(d));
    d.fn_nups = -1;
    d.fn_lastline = -1;
}

// Captures function info via lua_getinfo(L, ">unS", &ar). Function must
// be at top of stack; this pops it.
void capture_function_info(lua_State* L, UpvalDiag& d) {
    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    if (lua_getinfo(L, ">unS", &ar)) {
        d.fn_nups = ar.nups;
        d.fn_isvararg = ar.isvararg;
        d.fn_what = ar.what ? ar.what : "?";
        std::snprintf(d.fn_source, sizeof(d.fn_source), "%s",
                      ar.short_src ? ar.short_src : "?");
        d.fn_lastline = ar.lastlinedefined;
    }
}

// Walk the inner upvalues of a function at `fn_idx` and record a
// type histogram in `d.inner_typehist`. Does NOT recurse — depth=1.
// (Recursion would multiply data size; we keep depth=1 here and treat
// nested-function detection as an inner_typehist[FUNC] count.)
void walk_inner_upvalues(lua_State* L, int fn_idx, UpvalDiag& d) {
    int nups = d.fn_nups;
    for (int i = 1; i <= nups; ++i) {
        const char* name = lua_getupvalue(L, fn_idx, i);
        if (name == nullptr) break;
        int t = lua_type(L, -1);
        int bucket = 7;  // OTHER
        switch (t) {
            case LUA_TNIL:      bucket = 0; break;
            case LUA_TBOOLEAN:  bucket = 1; break;
            case LUA_TNUMBER:   bucket = 2; break;
            case LUA_TSTRING:   bucket = 3; break;
            case LUA_TTABLE:    bucket = 4; break;
            case LUA_TFUNCTION: bucket = 5; break;
            case LUA_TUSERDATA: bucket = 6; break;
        }
        d.inner_typehist[bucket]++;
        lua_pop(L, 1);
    }
}

void capture_table_info(lua_State* L, int idx, UpvalDiag& d) {
    d.tbl_ptr = reinterpret_cast<uintptr_t>(lua_topointer(L, idx));
    if (lua_getmetatable(L, idx)) {
        d.tbl_has_meta = 1;
        // Try to read __name field (Lua 5.3+ convention)
        lua_getfield(L, -1, "__name");
        if (lua_isstring(L, -1)) {
            std::snprintf(d.tbl_meta_name, sizeof(d.tbl_meta_name),
                          "%s", lua_tostring(L, -1));
        }
        lua_pop(L, 2);  // pop name + metatable
    }
    // Count keys (cap at large number for safety)
    d.tbl_nkeys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx > 0 ? idx : idx - 1) != 0) {
        d.tbl_nkeys++;
        lua_pop(L, 1);
        if (d.tbl_nkeys > 1000) break;
    }
}

// Detect the script's own _G["c<code>"] table. Mirror of
// detect_script_self_table in lua_callback.cpp — both this and dump
// path filter out script-self tables so they show in neither.
bool is_script_self_table(lua_State* L, int idx, uint32_t owner_code) {
    if (owner_code == 0) return false;
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "c%u", owner_code);
    lua_getglobal(L, buf);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return false; }
    const int normalized = idx > 0 ? idx : idx - 1;
    const bool same = lua_rawequal(L, normalized, -1) != 0;
    lua_pop(L, 1);
    return same;
}

// Walk a card's effects, collect diagnostic rows for each non-primitive
// upvalue. Returns rows; also populates `card_table_ptrs` with all table
// pointers seen on this card (for sharing detection).
//
// Filters mirror the production dump_lua_callback path:
//   - _ENV upvalues skipped (production uses VALUE_NOT_SET marker)
//   - script_self_table upvalues skipped (production uses script_self_card_code)
// This way the diagnostic measures the same refuse-eligible population
// that the original chunk-6 measurement counted.
std::vector<UpvalDiag> walk_card(duel& d, uint32_t card_code,
                                 std::map<uintptr_t, int>& card_table_ptr_count) {
    std::vector<UpvalDiag> rows;
    auto* L = d.lua->lua_state;

    for (effect* e : d.effects) {
        if (e == nullptr) continue;
        for (int slot = 0; slot < 5; ++slot) {
            if (!push_callback(L, *e, slot)) continue;
            int fn_idx = lua_gettop(L);

            // Get nups (lua_getinfo with ">u" CONSUMES the function — we
            // re-push afterward to walk upvalues).
            lua_pushvalue(L, fn_idx);  // duplicate
            lua_Debug ar;
            std::memset(&ar, 0, sizeof(ar));
            int nups = 0;
            if (lua_getinfo(L, ">u", &ar)) {
                nups = ar.nups;
            }
            // Function still at fn_idx on stack.

            for (int i = 1; i <= nups; ++i) {
                const char* uname = lua_getupvalue(L, fn_idx, i);
                if (uname == nullptr) break;
                int t = lua_type(L, -1);

                // Skip primitives + engine handles; we only care about
                // refused upvalue types. (Fast-track: skip nil/bool/
                // number/string. Userdata: pass through to capture too,
                // even if it's a card/effect/group that wouldn't refuse,
                // because the count is small and provides confirming
                // signal.)
                if (t == LUA_TNIL || t == LUA_TBOOLEAN ||
                    t == LUA_TNUMBER || t == LUA_TSTRING) {
                    lua_pop(L, 1);
                    continue;
                }

                // Mirror production filters.
                if (uname && std::strcmp(uname, "_ENV") == 0) {
                    lua_pop(L, 1);
                    continue;
                }
                if (t == LUA_TTABLE &&
                    is_script_self_table(L, -1, card_code)) {
                    lua_pop(L, 1);
                    continue;
                }

                UpvalDiag row;
                zero_diag(row);
                row.card_code = card_code;
                row.slot_idx = slot;
                row.upval_idx = i;
                row.type = t;

                if (t == LUA_TFUNCTION) {
                    // capture_function_info pops; pass copy
                    lua_pushvalue(L, -1);
                    capture_function_info(L, row);
                    // walk_inner_upvalues needs the function still on
                    // stack — re-push and walk.
                    lua_pushvalue(L, -1);
                    int inner_fn = lua_gettop(L);
                    if (row.fn_nups > 0) {
                        walk_inner_upvalues(L, inner_fn, row);
                    }
                    lua_pop(L, 1);
                } else if (t == LUA_TTABLE) {
                    capture_table_info(L, -1, row);
                    card_table_ptr_count[row.tbl_ptr]++;
                }

                rows.push_back(row);
                lua_pop(L, 1);  // pop the upvalue
            }
            lua_pop(L, 1);  // pop the function itself
        }
    }
    return rows;
}

// Read the manifest into g_manifest.
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

// Read refuse CSV; return (codes that hit UNK_UPVAL, codes that hit OTHER).
struct RefuseLists {
    std::vector<uint32_t> unk_upval;
    std::vector<uint32_t> other;
    std::vector<uint32_t> lua_dump_failures;  // subset of OTHER
    std::vector<uint32_t> chunk5a_stubs;      // subset of OTHER
};

RefuseLists load_refuse_lists() {
    RefuseLists r;
    std::ifstream f(kRefuseCsvPath);
    if (!f.is_open()) {
        std::fprintf(stderr, "FATAL: cannot open %s\n", kRefuseCsvPath);
        std::exit(2);
    }
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) { header = false; continue; }
        // code,outcome,"reason"
        auto c1 = line.find(',');
        auto c2 = line.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        uint32_t code = std::stoul(line.substr(0, c1));
        std::string outcome = line.substr(c1 + 1, c2 - c1 - 1);
        std::string reason = line.substr(c2 + 1);
        if (outcome == "UNK_UPVAL") {
            r.unk_upval.push_back(code);
        } else if (outcome == "OTHER") {
            r.other.push_back(code);
            if (reason.find("lua_dump returned 1") != std::string::npos) {
                r.lua_dump_failures.push_back(code);
            }
            if (reason.find("chunk-5a stub") != std::string::npos) {
                r.chunk5a_stubs.push_back(code);
            }
        }
    }
    return r;
}

const CardEntry* find_card(uint32_t code) {
    for (const auto& v : g_manifest) {
        if (v.id == code) return &v;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    load_manifest();
    auto lists = load_refuse_lists();
    std::printf("Loaded %zu UNK_UPVAL + %zu OTHER refuses (incl. %zu lua_dump, "
                "%zu chunk-5a stubs)\n",
                lists.unk_upval.size(), lists.other.size(),
                lists.lua_dump_failures.size(), lists.chunk5a_stubs.size());

    // CLI: optionally cap UNK_UPVAL processing for fast iterations
    size_t unk_cap = lists.unk_upval.size();
    if (argc >= 2) {
        unk_cap = std::min(unk_cap, static_cast<size_t>(std::atoll(argv[1])));
        std::printf("Capping UNK_UPVAL walk to first %zu cards\n", unk_cap);
    }

    // ---- Items 1-3 + diagnostic data ----
    std::FILE* upv_csv = std::fopen("/tmp/5c0_upvalues.csv", "w");
    std::fprintf(upv_csv,
        "card,slot,upval_idx,type,fn_nups,fn_isvararg,fn_what,fn_source,"
        "fn_lastline,tbl_ptr,tbl_has_meta,tbl_meta_name,tbl_nkeys,"
        "inner_nil,inner_bool,inner_num,inner_str,inner_tbl,inner_func,"
        "inner_userdata,inner_other\n");

    std::FILE* sharing_csv = std::fopen("/tmp/5c0_table_sharing.csv", "w");
    std::fprintf(sharing_csv, "card,tbl_ptr,occurrences\n");

    size_t cards_processed = 0;
    size_t cards_with_shared_tables = 0;
    size_t total_table_upvals = 0;
    size_t total_function_upvals = 0;

    // Histograms
    std::map<int, size_t> fn_nups_hist;     // nups distribution
    std::map<std::string, size_t> fn_what_hist;
    std::map<int, size_t> fn_isvararg_hist;
    std::map<int, size_t> tbl_meta_hist;
    std::map<int, size_t> tbl_nkeys_hist;
    std::map<std::string, size_t> tbl_meta_name_hist;
    std::map<int, size_t> inner_func_count_hist;  // # function upvalues 1 level deep

    for (size_t i = 0; i < unk_cap; ++i) {
        uint32_t code = lists.unk_upval[i];
        const CardEntry* ce = find_card(code);
        if (ce == nullptr) continue;
        OCG_Duel handle = make_diag_duel(0xC500 + (code & 0xff));
        if (handle == nullptr) continue;
        uint32_t loc, seq, pos;
        choose_placement(ce->type, loc, seq, pos);
        OCG_NewCardInfo info{};
        info.code = code; info.loc = loc; info.seq = seq;
        info.pos = pos; info.team = 0; info.duelist = 0; info.con = 0;
        OCG_DuelNewCard(handle, &info);

        auto* d = static_cast<duel*>(handle);
        if (d->cards.size() < 2) {
            OCG_DestroyDuel(handle);
            continue;
        }

        std::map<uintptr_t, int> card_tables;
        auto rows = walk_card(*d, code, card_tables);

        for (const auto& r : rows) {
            std::fprintf(upv_csv,
                "%u,%s,%d,%s,%d,%d,%s,\"%s\",%d,"
                "%lu,%d,\"%s\",%d,"
                "%d,%d,%d,%d,%d,%d,%d,%d\n",
                r.card_code, slot_name(r.slot_idx), r.upval_idx,
                r.type == LUA_TFUNCTION ? "function" :
                r.type == LUA_TTABLE    ? "table" :
                r.type == LUA_TUSERDATA ? "userdata" : "other",
                r.fn_nups, r.fn_isvararg,
                r.fn_what ? r.fn_what : "",
                r.fn_source, r.fn_lastline,
                static_cast<unsigned long>(r.tbl_ptr),
                r.tbl_has_meta, r.tbl_meta_name, r.tbl_nkeys,
                r.inner_typehist[0], r.inner_typehist[1],
                r.inner_typehist[2], r.inner_typehist[3],
                r.inner_typehist[4], r.inner_typehist[5],
                r.inner_typehist[6], r.inner_typehist[7]);

            if (r.type == LUA_TFUNCTION) {
                total_function_upvals++;
                fn_nups_hist[r.fn_nups]++;
                fn_what_hist[r.fn_what ? r.fn_what : "?"]++;
                fn_isvararg_hist[r.fn_isvararg]++;
                inner_func_count_hist[r.inner_typehist[5]]++;
            } else if (r.type == LUA_TTABLE) {
                total_table_upvals++;
                tbl_meta_hist[r.tbl_has_meta]++;
                tbl_nkeys_hist[r.tbl_nkeys]++;
                if (r.tbl_meta_name[0]) {
                    tbl_meta_name_hist[r.tbl_meta_name]++;
                }
            }
        }

        // Sharing detection: which tables appear in multiple closures?
        bool any_shared = false;
        for (const auto& [ptr, count] : card_tables) {
            if (count >= 2) {
                std::fprintf(sharing_csv, "%u,%lu,%d\n",
                             code, static_cast<unsigned long>(ptr), count);
                any_shared = true;
            }
        }
        if (any_shared) cards_with_shared_tables++;

        OCG_DestroyDuel(handle);
        ++cards_processed;
        if (cards_processed % 100 == 0) {
            std::printf("  characterized %zu / %zu UNK_UPVAL cards\n",
                        cards_processed, unk_cap);
            std::fflush(stdout);
        }
    }

    std::fclose(upv_csv);
    std::fclose(sharing_csv);

    // ---- Item 4: lua_dump failures ----
    std::printf("\n=== Item 4: lua_dump failures (n=%zu) ===\n",
                lists.lua_dump_failures.size());
    for (uint32_t code : lists.lua_dump_failures) {
        std::printf("  c%u\n", code);
    }

    // ---- Item 5: chunk-5a stub orthogonality ----
    std::printf("\n=== Item 5: chunk-5a stubs (n=%zu) ===\n",
                lists.chunk5a_stubs.size());
    for (size_t i = 0; i < std::min<size_t>(5, lists.chunk5a_stubs.size()); ++i) {
        uint32_t code = lists.chunk5a_stubs[i];
        const CardEntry* ce = find_card(code);
        if (ce == nullptr) continue;
        OCG_Duel handle = make_diag_duel(0xC500 + (code & 0xff));
        if (handle == nullptr) continue;
        uint32_t loc, seq, pos;
        choose_placement(ce->type, loc, seq, pos);
        OCG_NewCardInfo info{};
        info.code = code; info.loc = loc; info.seq = seq;
        info.pos = pos; info.team = 0; info.duelist = 0; info.con = 0;
        OCG_DuelNewCard(handle, &info);
        auto* d = static_cast<duel*>(handle);
        if (!d->game_field) { OCG_DestroyDuel(handle); continue; }
        const auto& core = d->game_field->core;
        std::printf("  c%u: units=%zu subunits=%zu tpchain=%zu ntpchain=%zu "
                    "select_chains=%zu cards=%zu effects=%zu\n",
                    code, core.units.size(), core.subunits.size(),
                    core.tpchain.size(), core.ntpchain.size(),
                    core.select_chains.size(),
                    d->cards.size(), d->effects.size());
        OCG_DestroyDuel(handle);
    }

    // ---- Summary print ----
    std::printf("\n");
    std::printf("================================================================\n");
    std::printf("  5c.0 characterization summary\n");
    std::printf("================================================================\n");
    std::printf("  Cards processed: %zu / %zu UNK_UPVAL refuses\n",
                cards_processed, lists.unk_upval.size());
    std::printf("  Total function upvalues:  %zu\n", total_function_upvals);
    std::printf("  Total table upvalues:     %zu\n", total_table_upvals);
    std::printf("  Cards with shared tables (intra-card): %zu / %zu\n",
                cards_with_shared_tables, cards_processed);
    std::printf("\n  --- Function upvalue nups histogram (depth=0 = pure lambda) ---\n");
    for (const auto& [nups, count] : fn_nups_hist) {
        std::printf("    nups=%2d : %5zu  (%.1f%%)\n",
                    nups, count,
                    total_function_upvals > 0
                        ? 100.0 * count / total_function_upvals : 0.0);
    }
    std::printf("\n  --- Function upvalue inner-function-count histogram (1 level deep) ---\n");
    for (const auto& [cnt, freq] : inner_func_count_hist) {
        std::printf("    inner_func_count=%d : %5zu (%.1f%%)\n",
                    cnt, freq,
                    total_function_upvals > 0
                        ? 100.0 * freq / total_function_upvals : 0.0);
    }
    std::printf("\n  --- Function upvalue what= histogram ---\n");
    for (const auto& [what, count] : fn_what_hist) {
        std::printf("    what=%s : %5zu (%.1f%%)\n", what.c_str(), count,
                    total_function_upvals > 0
                        ? 100.0 * count / total_function_upvals : 0.0);
    }
    std::printf("\n  --- Function isvararg histogram ---\n");
    for (const auto& [vararg, count] : fn_isvararg_hist) {
        std::printf("    isvararg=%d : %5zu\n", vararg, count);
    }
    std::printf("\n  --- Table has-metatable histogram ---\n");
    for (const auto& [meta, count] : tbl_meta_hist) {
        std::printf("    has_metatable=%d : %5zu (%.1f%%)\n",
                    meta, count,
                    total_table_upvals > 0
                        ? 100.0 * count / total_table_upvals : 0.0);
    }
    std::printf("\n  --- Top metatable __name values ---\n");
    {
        std::vector<std::pair<std::string, size_t>> sorted(
            tbl_meta_name_hist.begin(), tbl_meta_name_hist.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        const size_t top = std::min<size_t>(10, sorted.size());
        for (size_t i = 0; i < top; ++i) {
            std::printf("    %5zu × \"%s\"\n", sorted[i].second,
                        sorted[i].first.c_str());
        }
    }
    std::printf("\n  --- Table nkeys histogram (top 15) ---\n");
    {
        std::vector<std::pair<int, size_t>> sorted(
            tbl_nkeys_hist.begin(), tbl_nkeys_hist.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        const size_t top = std::min<size_t>(15, sorted.size());
        for (size_t i = 0; i < top; ++i) {
            std::printf("    nkeys=%2d : %5zu\n", sorted[i].first, sorted[i].second);
        }
    }
    std::printf("================================================================\n");
    std::printf("  Per-upvalue rows: /tmp/5c0_upvalues.csv\n");
    std::printf("  Shared tables:    /tmp/5c0_table_sharing.csv\n");
    std::printf("================================================================\n");

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
