// test_save.cpp — chunk 3: save side of state serialization.
//
// Coverage per phase_p1_primitive_1_plan.md §8.4 (intra-path byte-equal,
// CI gate) plus §5.1 refuse-path detection plus a smoke test that the
// serialized blob is non-empty and parseable.
//
// Vanilla-only fixture: minimal duel created via OCG_CreateDuel with stub
// callbacks. No cards, no StartDuel, no Lua side effects. The point is to
// validate the SAVE path works end-to-end and produces deterministic
// bytes; chunk 4-5 fixtures will exercise card/effect/Lua paths.

#include "ocg_state.pb.h"
#include "ocgapi.h"
#include "ocgapi_types.h"

// Internal headers — required for the NOT_MSG_BOUNDARY synthesis test
// (pokes interpreter::call_depth directly) and for the chunk-4 load
// tests that mutate field_info / player[] on the duel before saving.
// Static-lib linkage makes the symbols visible to the test binary.
#include "duel.h"
#include "field.h"
#include "interpreter.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace {

#define CHECK_EQ(actual, expected, label)                                    \
    do {                                                                     \
        const auto _a = (actual);                                            \
        const auto _e = (expected);                                          \
        if (!(_a == _e)) {                                                   \
            std::fprintf(stderr,                                             \
                "FAIL %s:%d: %s mismatch: got %lld, want %lld\n",            \
                __FILE__, __LINE__, (label),                                 \
                static_cast<long long>(_a),                                  \
                static_cast<long long>(_e));                                 \
            return false;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_TRUE(cond, label)                                              \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s false\n",                   \
                __FILE__, __LINE__, (label));                                \
            return false;                                                    \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Stub callbacks for OCG_CreateDuel — never actually fired in chunk 3
// fixtures (no card lookup, no script load), but required to be non-null
// to pass OCG_CreateDuel's null-checks.
// ---------------------------------------------------------------------------

void stub_card_reader(void* /*payload*/, uint32_t /*code*/, OCG_CardData* data) {
    if (data) {
        std::memset(data, 0, sizeof(*data));
    }
}

int stub_script_reader(void* /*payload*/, OCG_Duel /*duel*/,
                       const char* /*name*/) {
    return 0;
}

void stub_log_handler(void* /*payload*/, const char* /*string*/, int /*type*/) {}

void stub_card_reader_done(void* /*payload*/, OCG_CardData* /*data*/) {}

OCG_Duel make_vanilla_duel(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xCAFEBABEDEADBEEFULL;
    opts.seed[2] = 0x0123456789ABCDEFULL;
    opts.seed[3] = 0xFEDCBA9876543210ULL;
    opts.flags = 0;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &stub_card_reader;
    opts.scriptReader = &stub_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    OCG_Duel duel = nullptr;
    const int status = OCG_CreateDuel(&duel, &opts);
    if (status != OCG_DUEL_CREATION_SUCCESS || duel == nullptr) {
        std::fprintf(stderr, "OCG_CreateDuel failed: status=%d\n", status);
        return nullptr;
    }
    return duel;
}

// ---------------------------------------------------------------------------
// §8.4 byte-equal intra-path (CI gate)
// ---------------------------------------------------------------------------

bool test_byte_equal_intra_path() {
    OCG_Duel duel = make_vanilla_duel(0xAAAA);
    CHECK_TRUE(duel != nullptr, "create duel");

    void* buf1 = nullptr;
    uint32_t size1 = 0;
    int s1 = OCG_DuelSaveState(duel, &buf1, &size1);
    CHECK_EQ(s1, OCG_SAVE_OK, "save 1 status");
    CHECK_TRUE(buf1 != nullptr, "save 1 buffer");
    CHECK_TRUE(size1 > 0, "save 1 size");

    void* buf2 = nullptr;
    uint32_t size2 = 0;
    int s2 = OCG_DuelSaveState(duel, &buf2, &size2);
    CHECK_EQ(s2, OCG_SAVE_OK, "save 2 status");
    CHECK_TRUE(buf2 != nullptr, "save 2 buffer");

    CHECK_EQ(size1, size2, "size match");
    if (std::memcmp(buf1, buf2, size1) != 0) {
        // Find first divergent byte and report — most useful diagnostic
        // for a determinism bug (some unordered_set iteration leaked).
        const uint8_t* a = static_cast<const uint8_t*>(buf1);
        const uint8_t* b = static_cast<const uint8_t*>(buf2);
        for (uint32_t i = 0; i < size1; ++i) {
            if (a[i] != b[i]) {
                std::fprintf(stderr,
                    "FAIL: byte %u differs: 0x%02x vs 0x%02x (size=%u)\n",
                    i, a[i], b[i], size1);
                break;
            }
        }
        OCG_FreeSaveBuffer(buf1);
        OCG_FreeSaveBuffer(buf2);
        OCG_DestroyDuel(duel);
        return false;
    }

    OCG_FreeSaveBuffer(buf1);
    OCG_FreeSaveBuffer(buf2);
    OCG_DestroyDuel(duel);
    return true;
}

// ---------------------------------------------------------------------------
// Smoke: serialized blob parses back into a DuelState whose fields match
// what we expect from a vanilla post-CreateDuel state.
// ---------------------------------------------------------------------------

bool test_blob_parses_with_expected_shape() {
    OCG_Duel duel = make_vanilla_duel(0xBEEF);
    CHECK_TRUE(duel != nullptr, "create duel");

    void* buf = nullptr;
    uint32_t size = 0;
    int s = OCG_DuelSaveState(duel, &buf, &size);
    CHECK_EQ(s, OCG_SAVE_OK, "save status");

    ocg::state::DuelState parsed;
    CHECK_TRUE(parsed.ParseFromArray(buf, static_cast<int>(size)),
               "parse blob");

    // Schema header
    CHECK_EQ(parsed.schema_version(), 1u, "schema_version");
    CHECK_EQ(parsed.timestamp_unix(), 0ull,
             "timestamp_unix is fixed at 0 (deterministic)");
    CHECK_EQ(parsed.save_safety(), ocg::state::DuelState::SAVE_SAFETY_OK,
             "save_safety OK");

    // RNG state captured (4 uint64)
    CHECK_EQ(parsed.rng().xoshiro_state_size(), 4, "rng state size");
    // First seed word matches what we passed to CreateDuel
    CHECK_EQ(parsed.rng().xoshiro_state(0), 0xBEEFull, "rng[0] == seed_lo");

    // Field exists; player slot count is fixed at 2
    CHECK_TRUE(parsed.has_field_info(), "has field_info");
    CHECK_EQ(parsed.players_size(), 2, "two players");
    CHECK_EQ(parsed.players(0).lp(), 8000, "p0 starting LP");
    CHECK_EQ(parsed.players(1).lp(), 8000, "p1 starting LP");
    CHECK_EQ(parsed.players(0).start_count(), 5, "p0 start hand size");

    // Empty zones — pre-StartDuel state has no main deck loaded yet.
    // mzone is fixed-size 7, szone fixed-size 8; all entries are handle 0.
    CHECK_EQ(parsed.players(0).list_mzone_size(), 7, "p0 mzone slots");
    CHECK_EQ(parsed.players(0).list_szone_size(), 8, "p0 szone slots");
    for (int i = 0; i < 7; ++i) {
        CHECK_EQ(parsed.players(0).list_mzone(i), 0u, "p0 mzone empty");
    }

    // A vanilla post-CreateDuel state has at most one card (field.temp_card,
    // a scratch slot allocated by the field constructor and registered in
    // duel.cards). No effects, no groups, no chain. We don't tighten this
    // to a specific count — that's an implementation detail of the engine
    // ctor and the byte-equal test already pins determinism.
    CHECK_TRUE(parsed.cards_size() <= 1,
               "vanilla state has 0 or 1 cards (engine ctor scratch slot)");
    CHECK_EQ(parsed.effects_size(), 0, "no effects");
    CHECK_EQ(parsed.groups_size(), 0, "no groups");

    // Empty chain
    CHECK_EQ(parsed.chain().links_size(), 0, "empty chain");

    // Lua reconstruction is empty in chunk 3 (no-Lua path) — the message
    // is present (we mutable_lua()'d it) but has no closures.
    CHECK_TRUE(parsed.has_lua(), "lua message present");
    CHECK_EQ(parsed.lua().closures_size(), 0,
             "no closures (chunk 3 no-Lua path)");

    OCG_FreeSaveBuffer(buf);
    OCG_DestroyDuel(duel);
    return true;
}

// ---------------------------------------------------------------------------
// Refuse path: bad arguments
// ---------------------------------------------------------------------------

bool test_refuse_null_pointer_args() {
    OCG_Duel duel = make_vanilla_duel(1);
    CHECK_TRUE(duel != nullptr, "create duel");

    // Null buffer outparam → INTERNAL (caller error)
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(duel, nullptr, &size),
             OCG_SAVE_ERR_INTERNAL, "null buffer outparam");
    // Null size outparam → INTERNAL
    void* buf = nullptr;
    CHECK_EQ(OCG_DuelSaveState(duel, &buf, nullptr),
             OCG_SAVE_ERR_INTERNAL, "null size outparam");
    // Null duel handle → INTERNAL
    CHECK_EQ(OCG_DuelSaveState(nullptr, &buf, &size),
             OCG_SAVE_ERR_INTERNAL, "null duel handle");
    CHECK_TRUE(buf == nullptr, "buf untouched on error");
    CHECK_EQ(size, 0u, "size zeroed on error");

    OCG_DestroyDuel(duel);
    return true;
}

// ---------------------------------------------------------------------------
// Two distinct seeds → distinct serialized bytes (regression check that
// RNG state actually round-trips into the blob, not lost in a copy).
// ---------------------------------------------------------------------------

bool test_distinct_seeds_yield_distinct_blobs() {
    OCG_Duel d1 = make_vanilla_duel(0x1111);
    OCG_Duel d2 = make_vanilla_duel(0x2222);
    CHECK_TRUE(d1 != nullptr && d2 != nullptr, "create both");

    void *b1 = nullptr, *b2 = nullptr;
    uint32_t s1 = 0, s2 = 0;
    CHECK_EQ(OCG_DuelSaveState(d1, &b1, &s1), OCG_SAVE_OK, "save d1");
    CHECK_EQ(OCG_DuelSaveState(d2, &b2, &s2), OCG_SAVE_OK, "save d2");

    // Sizes likely identical (same shape) but contents differ at the
    // RNG bytes — assert the serialized blobs are NOT equal.
    bool same = (s1 == s2 && std::memcmp(b1, b2, s1) == 0);
    CHECK_TRUE(!same,
               "distinct seeds produce distinct serialized output");

    OCG_FreeSaveBuffer(b1);
    OCG_FreeSaveBuffer(b2);
    OCG_DestroyDuel(d1);
    OCG_DestroyDuel(d2);
    return true;
}

// ---------------------------------------------------------------------------
// §5.1 refuse path: NOT_MSG_BOUNDARY when interpreter::call_depth > 0
// ---------------------------------------------------------------------------

bool test_refuse_not_msg_boundary() {
    OCG_Duel ocg_duel = make_vanilla_duel(0xCAFE);
    CHECK_TRUE(ocg_duel != nullptr, "create duel");

    // Synthetically bump call_depth to mimic "we're inside a Lua call".
    // Real engine code increments this in interpreter::call_function /
    // call_coroutine; outside Process call_depth is 0. The save path
    // checks d.lua->call_depth > 0 and refuses.
    auto* d = static_cast<duel*>(ocg_duel);
    CHECK_TRUE(d->lua != nullptr, "duel has interpreter");
    CHECK_EQ(d->lua->call_depth, 0, "call_depth is 0 at MSG boundary");

    d->lua->call_depth = 1;  // simulate mid-Lua

    void* buf = nullptr;
    uint32_t size = 0;
    int s = OCG_DuelSaveState(ocg_duel, &buf, &size);
    CHECK_EQ(s, OCG_SAVE_ERR_NOT_MSG_BOUNDARY,
             "save refuses with NOT_MSG_BOUNDARY when call_depth > 0");
    CHECK_TRUE(buf == nullptr, "no buffer allocated on refuse");
    CHECK_EQ(size, 0u, "size 0 on refuse");

    // Restore state and verify save works again.
    d->lua->call_depth = 0;
    s = OCG_DuelSaveState(ocg_duel, &buf, &size);
    CHECK_EQ(s, OCG_SAVE_OK, "save succeeds after call_depth reset");
    CHECK_TRUE(buf != nullptr && size > 0, "buffer + size populated");

    OCG_FreeSaveBuffer(buf);
    OCG_DestroyDuel(ocg_duel);
    return true;
}

// ---------------------------------------------------------------------------
// CHUNK 4: Load + round-trip tests
// ---------------------------------------------------------------------------

// Builds OCG_DuelOptions identical to make_vanilla_duel's, so a load can
// reuse the same callbacks the original duel had. Saved state overrides
// the seed at deserialization, so the seed_lo argument here is irrelevant
// for behavior — but we pass a deliberately-different value to confirm
// the saved RNG wins over the options seed.
OCG_DuelOptions make_load_options(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xDEADC0DEDEADC0DEULL;
    opts.seed[2] = 0xC0FFEEC0FFEE0000ULL;
    opts.seed[3] = 0x0123456789ABCDEFULL;
    opts.flags = 0;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &stub_card_reader;
    opts.scriptReader = &stub_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    return opts;
}

bool test_load_round_trip_byte_equal() {
    // Save → load → save → assert byte-equal across the two saves.
    // This is the strongest chunk-4 invariant: the load path preserves
    // every bit of state the save path captured. With vanilla fixtures
    // (no cards), the second save must produce exactly the same bytes
    // as the first.
    OCG_Duel orig = make_vanilla_duel(0xABCD1234ABCD1234ULL);
    CHECK_TRUE(orig != nullptr, "create orig");

    void* blob1 = nullptr;
    uint32_t size1 = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob1, &size1), OCG_SAVE_OK,
             "save orig");

    OCG_Duel loaded = nullptr;
    OCG_DuelOptions opts = make_load_options(0xCAFE);  // seed deliberately
                                                       // different from orig
    int load_status = OCG_DuelLoadState(blob1, size1, &opts, &loaded);
    CHECK_EQ(load_status, OCG_LOAD_OK, "load");
    CHECK_TRUE(loaded != nullptr, "load produced a duel");

    void* blob2 = nullptr;
    uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "save loaded");

    CHECK_EQ(size1, size2, "save sizes match");
    if (std::memcmp(blob1, blob2, size1) != 0) {
        const uint8_t* a = static_cast<const uint8_t*>(blob1);
        const uint8_t* b = static_cast<const uint8_t*>(blob2);
        for (uint32_t i = 0; i < size1; ++i) {
            if (a[i] != b[i]) {
                std::fprintf(stderr,
                    "FAIL: round-trip byte %u differs: 0x%02x vs 0x%02x\n",
                    i, a[i], b[i]);
                break;
            }
        }
        OCG_FreeSaveBuffer(blob1);
        OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig);
        OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob1);
    OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

bool test_load_preserves_rng_state() {
    // Save captures RNG state; load restores it. After load, the duel's
    // RNG must produce the same future values as the original would have.
    // We probe via duel::get_rng().get_state() (no Process needed).
    OCG_Duel orig = make_vanilla_duel(0x7777EEEE7777EEEEULL);
    CHECK_TRUE(orig != nullptr, "create orig");

    auto* d_orig = static_cast<duel*>(orig);
    const auto orig_state = d_orig->get_rng().get_state();

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_Duel loaded = nullptr;
    OCG_DuelOptions opts = make_load_options(0x9999);  // different seed
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");
    auto* d_loaded = static_cast<duel*>(loaded);
    const auto loaded_state = d_loaded->get_rng().get_state();

    CHECK_EQ(orig_state[0], loaded_state[0], "rng[0]");
    CHECK_EQ(orig_state[1], loaded_state[1], "rng[1]");
    CHECK_EQ(orig_state[2], loaded_state[2], "rng[2]");
    CHECK_EQ(orig_state[3], loaded_state[3], "rng[3]");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

bool test_load_preserves_field_info() {
    OCG_Duel orig = make_vanilla_duel(1);
    CHECK_TRUE(orig != nullptr, "create orig");

    // Mutate a few field_info fields so we can verify they came back
    // through. (Vanilla post-CreateDuel has defaults; mutate to detect
    // load really restoring rather than just inheriting from ctor.)
    auto* d_orig = static_cast<duel*>(orig);
    d_orig->game_field->infos.turn_id = 17;
    d_orig->game_field->infos.phase = 0x40;
    d_orig->game_field->infos.turn_player = 1;
    d_orig->game_field->infos.event_id = 999;

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_Duel loaded = nullptr;
    OCG_DuelOptions opts = make_load_options(2);
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");
    auto* d_loaded = static_cast<duel*>(loaded);
    CHECK_EQ(d_loaded->game_field->infos.turn_id, 17, "turn_id");
    CHECK_EQ(d_loaded->game_field->infos.phase, 0x40, "phase");
    CHECK_EQ(d_loaded->game_field->infos.turn_player, 1, "turn_player");
    CHECK_EQ(d_loaded->game_field->infos.event_id, 999u, "event_id");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

bool test_load_preserves_player_lp() {
    OCG_Duel orig = make_vanilla_duel(1);
    CHECK_TRUE(orig != nullptr, "create orig");

    auto* d_orig = static_cast<duel*>(orig);
    d_orig->game_field->player[0].lp = 4321;
    d_orig->game_field->player[1].lp = 1234;
    d_orig->game_field->player[0].used_location = 0xCAFE;

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_Duel loaded = nullptr;
    OCG_DuelOptions opts = make_load_options(2);
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");
    auto* d_loaded = static_cast<duel*>(loaded);
    CHECK_EQ(d_loaded->game_field->player[0].lp, 4321, "p0 lp");
    CHECK_EQ(d_loaded->game_field->player[1].lp, 1234, "p1 lp");
    CHECK_EQ(d_loaded->game_field->player[0].used_location, 0xCAFEu,
             "p0 used_location");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

bool test_load_strict_output_not_empty() {
    // Plan §3.1: *out must be nullptr on entry. Non-null returns
    // OCG_LOAD_ERR_OUTPUT_NOT_EMPTY without touching either pointer.
    OCG_Duel orig = make_vanilla_duel(1);
    CHECK_TRUE(orig != nullptr, "create orig");
    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    // Pretend the caller forgot to null out_duel.
    OCG_Duel out = orig;  // non-null
    OCG_DuelOptions opts = make_load_options(1);
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &out),
             OCG_LOAD_ERR_OUTPUT_NOT_EMPTY,
             "non-null *out_duel rejected");
    CHECK_TRUE(out == orig, "out untouched on rejection");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    return true;
}

bool test_load_malformed_blob() {
    OCG_DuelOptions opts = make_load_options(1);

    // Random garbage bytes
    const uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0xC0, 0xFF, 0xEE, 0x00,
                                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    OCG_Duel out = nullptr;
    int s = OCG_DuelLoadState(garbage, sizeof(garbage), &opts, &out);
    CHECK_EQ(s, OCG_LOAD_ERR_MALFORMED, "garbage rejected");
    CHECK_TRUE(out == nullptr, "no duel allocated");

    // Empty buffer
    out = nullptr;
    CHECK_EQ(OCG_DuelLoadState(nullptr, 0, &opts, &out),
             OCG_LOAD_ERR_MALFORMED, "null buffer rejected");
    CHECK_EQ(OCG_DuelLoadState(garbage, 0, &opts, &out),
             OCG_LOAD_ERR_MALFORMED, "zero size rejected");

    return true;
}

bool test_load_wrong_schema_version() {
    // Build a syntactically-valid blob with schema_version = 999.
    ocg::state::DuelState s;
    s.set_schema_version(999);
    auto* rng = s.mutable_rng();
    for (int i = 0; i < 4; ++i) rng->add_xoshiro_state(0);
    s.add_players()->set_lp(8000);
    s.add_players()->set_lp(8000);
    s.mutable_field_info();
    std::string bytes;
    CHECK_TRUE(s.SerializeToString(&bytes), "serialize bad-version blob");

    OCG_DuelOptions opts = make_load_options(1);
    OCG_Duel out = nullptr;
    int status = OCG_DuelLoadState(bytes.data(),
                                   static_cast<uint32_t>(bytes.size()),
                                   &opts, &out);
    CHECK_EQ(status, OCG_LOAD_ERR_SCHEMA_VERSION,
             "schema version 999 rejected");
    CHECK_TRUE(out == nullptr, "no duel allocated");
    return true;
}

bool test_load_refuse_tag() {
    // Build a syntactically-valid blob tagged save_safety=REFUSE.
    ocg::state::DuelState s;
    s.set_schema_version(1);
    s.set_save_safety(ocg::state::DuelState::SAVE_SAFETY_REFUSE);
    s.set_refuse_reason("test fixture");
    auto* rng = s.mutable_rng();
    for (int i = 0; i < 4; ++i) rng->add_xoshiro_state(0);
    s.add_players()->set_lp(8000);
    s.add_players()->set_lp(8000);
    s.mutable_field_info();
    std::string bytes;
    CHECK_TRUE(s.SerializeToString(&bytes), "serialize refuse-tagged blob");

    OCG_DuelOptions opts = make_load_options(1);
    OCG_Duel out = nullptr;
    int status = OCG_DuelLoadState(bytes.data(),
                                   static_cast<uint32_t>(bytes.size()),
                                   &opts, &out);
    CHECK_EQ(status, OCG_LOAD_ERR_REFUSE_TAG,
             "refuse-tagged blob rejected");
    CHECK_TRUE(out == nullptr, "no duel allocated");
    return true;
}

bool test_load_null_options_rejected() {
    OCG_Duel orig = make_vanilla_duel(1);
    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_Duel out = nullptr;
    int status = OCG_DuelLoadState(blob, size, nullptr, &out);
    CHECK_EQ(status, OCG_LOAD_ERR_INTERNAL, "null options rejected");
    CHECK_TRUE(out == nullptr, "no duel allocated");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    return true;
}

// (chunk-5b card_set test moved below the chunk-5a fixtures it depends on)

// ---------------------------------------------------------------------------
// Perf scaffold (chunk 4) — vanilla fixtures only.
//
// Per phase_p1_primitive_1_plan.md §9 the real perf harness is chunk 10
// work (full sample-state suite, p95/p99, cold-vs-warm cache split). This
// is a "ballpark" check at chunk 4: if vanilla save/load is anywhere near
// the §9.2 budgets (save <5ms p95, load <50ms p95), we're on track for
// the real benchmark; if it's already over budget on the empty fixture,
// we should investigate now.
// ---------------------------------------------------------------------------

bool test_perf_scaffold_vanilla() {
    constexpr int kIterations = 100;
    std::vector<double> save_ms;
    std::vector<double> load_ms;
    save_ms.reserve(kIterations);
    load_ms.reserve(kIterations);

    OCG_Duel orig = make_vanilla_duel(0xCA11AB1ECA11AB1EULL);
    CHECK_TRUE(orig != nullptr, "create orig");

    // Pre-roll one save to warm the protobuf init paths so the first
    // measurement isn't an outlier.
    {
        void* warm_buf = nullptr;
        uint32_t warm_size = 0;
        OCG_DuelSaveState(orig, &warm_buf, &warm_size);
        OCG_FreeSaveBuffer(warm_buf);
    }

    void* blob = nullptr;
    uint32_t blob_size = 0;

    for (int i = 0; i < kIterations; ++i) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        void* buf = nullptr;
        uint32_t size = 0;
        const int s = OCG_DuelSaveState(orig, &buf, &size);
        const auto t1 = std::chrono::high_resolution_clock::now();
        if (s != OCG_SAVE_OK) {
            std::fprintf(stderr, "perf: save failed at iter %d\n", i);
            return false;
        }
        save_ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (i == 0) {
            blob = buf;
            blob_size = size;
        } else {
            OCG_FreeSaveBuffer(buf);
        }
    }

    OCG_DuelOptions opts = make_load_options(1);
    for (int i = 0; i < kIterations; ++i) {
        OCG_Duel out = nullptr;
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int s = OCG_DuelLoadState(blob, blob_size, &opts, &out);
        const auto t1 = std::chrono::high_resolution_clock::now();
        if (s != OCG_LOAD_OK || out == nullptr) {
            std::fprintf(stderr, "perf: load failed at iter %d\n", i);
            return false;
        }
        load_ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
        OCG_DestroyDuel(out);
    }

    auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const double median = v[v.size() / 2];
        const double p95 = v[static_cast<size_t>(v.size() * 0.95)];
        const double p99 = v[static_cast<size_t>(v.size() * 0.99)];
        const double minv = v.front();
        const double maxv = v.back();
        return std::tuple<double, double, double, double, double>{
            minv, median, p95, p99, maxv};
    };

    auto [s_min, s_med, s_p95, s_p99, s_max] = stats(save_ms);
    auto [l_min, l_med, l_p95, l_p99, l_max] = stats(load_ms);

    std::printf(
        "  perf (vanilla, n=%d, blob=%u bytes):\n"
        "    save: min=%.3f med=%.3f p95=%.3f p99=%.3f max=%.3f ms\n"
        "    load: min=%.3f med=%.3f p95=%.3f p99=%.3f max=%.3f ms\n"
        "    budgets (plan §9.2): save p95 < 5ms, load p95 < 50ms\n"
        "    save p95 budget: %s\n"
        "    load p95 budget: %s\n",
        kIterations, blob_size,
        s_min, s_med, s_p95, s_p99, s_max,
        l_min, l_med, l_p95, l_p99, l_max,
        s_p95 < 5.0 ? "OK" : "OVER",
        l_p95 < 50.0 ? "OK" : "OVER");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);

    // Don't fail the test on budget miss at chunk 4 — perf is a real
    // gate at chunk 10. This is a ballpark signal for the status report.
    return true;
}

// ---------------------------------------------------------------------------
// CHUNK 5a: vanilla-monster fixtures (real card data + zone population +
// MSG-stream test)
// ---------------------------------------------------------------------------

// Hardcoded card-data table for the chunk-5a test fixture. Three vanilla
// normal monsters — no scripts to load, no effects to register, simplest
// possible card semantics. Data values are illustrative, not pulled from
// the real cards.cdb (consistency between save and load is what matters
// for the chunk-5a invariants; semantic correctness comes when chunk 5b
// fixtures need real script behavior).
struct VanillaCard {
    uint32_t code;
    uint32_t type;
    uint32_t level;
    uint32_t attribute;
    uint64_t race;
    int32_t attack;
    int32_t defense;
};
constexpr VanillaCard kVanillaCards[] = {
    // Dark Magician
    {46986414, 0x10 | 0x1, 7, 0x20, 0x2, 2500, 2100},
    // Blue-Eyes White Dragon
    {89631139, 0x10 | 0x1, 8, 0x10, 0x2000, 3000, 2500},
    // Kuriboh — small low-level fixture for variety
    {40640057, 0x10 | 0x1, 1, 0x20, 0x2, 300, 200},
};

void chunk5a_card_reader(void* /*payload*/, uint32_t code, OCG_CardData* data) {
    if (data == nullptr) return;
    std::memset(data, 0, sizeof(*data));
    for (const auto& v : kVanillaCards) {
        if (v.code == code) {
            data->code = v.code;
            data->alias = 0;
            data->setcodes = nullptr;
            data->type = v.type;
            data->level = v.level;
            data->attribute = v.attribute;
            data->race = v.race;
            data->attack = v.attack;
            data->defense = v.defense;
            data->lscale = 0;
            data->rscale = 0;
            data->link_marker = 0;
            return;
        }
    }
    // Unknown code → leave data zeroed; engine treats as "no such card".
}

int chunk5a_script_reader(void* /*payload*/, OCG_Duel /*duel*/,
                          const char* /*name*/) {
    // Vanilla normal monsters carry no per-card scripts in ProjectIgnis.
    // 0 = "not loaded" (engine accepts and proceeds).
    return 0;
}

OCG_Duel make_chunk5a_duel(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xCAFE;
    opts.seed[2] = 0xBEEF;
    opts.seed[3] = 0xF00D;
    opts.flags = 0;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &chunk5a_card_reader;
    opts.scriptReader = &chunk5a_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    OCG_Duel duel = nullptr;
    if (OCG_CreateDuel(&duel, &opts) != OCG_DUEL_CREATION_SUCCESS) {
        return nullptr;
    }
    return duel;
}

OCG_DuelOptions make_chunk5a_load_options() {
    OCG_DuelOptions opts{};
    opts.seed[0] = 0xDEAD;  // overridden by load
    opts.seed[1] = 0xBEEF;
    opts.seed[2] = 0;
    opts.seed[3] = 0;
    opts.flags = 0;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &chunk5a_card_reader;
    opts.scriptReader = &chunk5a_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    return opts;
}

// Adds a small deck-list to player 0 (cards in the main deck list, which
// is where StartDuel will draw from). Returns the number of cards added.
// Each card is added at LOCATION_DECK with a sequential seq.
int populate_simple_deck(OCG_Duel ocg_duel) {
    int n = 0;
    for (int copy = 0; copy < 3; ++copy) {
        for (const auto& v : kVanillaCards) {
            OCG_NewCardInfo info{};
            info.team = 0;
            info.duelist = 0;
            info.code = v.code;
            info.con = 0;
            info.loc = 0x01;  // LOCATION_DECK
            info.seq = static_cast<uint32_t>(n);
            info.pos = 0x08;  // POS_FACEDOWN_DEFENSE
            OCG_DuelNewCard(ocg_duel, &info);
            ++n;
        }
    }
    // Symmetric for player 1
    for (int copy = 0; copy < 3; ++copy) {
        for (const auto& v : kVanillaCards) {
            OCG_NewCardInfo info{};
            info.team = 1;
            info.duelist = 0;
            info.code = v.code;
            info.con = 1;
            info.loc = 0x01;
            info.seq = static_cast<uint32_t>(n);
            info.pos = 0x08;
            OCG_DuelNewCard(ocg_duel, &info);
            ++n;
        }
    }
    return n;
}

bool test_chunk5a_card_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0xC5A1);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    void* blob1 = nullptr;
    uint32_t size1 = 0;
    int s1 = OCG_DuelSaveState(orig, &blob1, &size1);
    CHECK_EQ(s1, OCG_SAVE_OK, "save orig with cards");
    CHECK_TRUE(size1 > 0, "non-empty blob");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob1, size1, &opts, &loaded), OCG_LOAD_OK,
             "load");
    CHECK_TRUE(loaded != nullptr, "load produced duel");

    void* blob2 = nullptr;
    uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "save loaded");

    CHECK_EQ(size1, size2, "round-trip sizes match");
    if (std::memcmp(blob1, blob2, size1) != 0) {
        const uint8_t* a = static_cast<const uint8_t*>(blob1);
        const uint8_t* b = static_cast<const uint8_t*>(blob2);
        for (uint32_t i = 0; i < size1; ++i) {
            if (a[i] != b[i]) {
                std::fprintf(stderr,
                    "FAIL: card round-trip byte %u differs: 0x%02x vs 0x%02x\n",
                    i, a[i], b[i]);
                break;
            }
        }
        OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
        return false;
    }

    // Sanity-check loaded zones — the round-trip byte-equal above is
    // the strict invariant; these are just enough to confirm the loaded
    // duel actually has cards in the deck rather than empty zones.
    auto* d = static_cast<duel*>(loaded);
    CHECK_EQ(d->game_field->player[0].list_main.size(), 9u,
             "loaded p0 deck size matches");
    CHECK_EQ(d->game_field->player[1].list_main.size(), 9u,
             "loaded p1 deck size matches");
    // Every loaded card pointer should be non-null and have one of the
    // three known codes. (Don't assert specific positions — the engine's
    // add_card may reorder cards within LOCATION_DECK relative to the
    // sequence I passed; the round-trip invariant doesn't care.)
    int dm_count = 0, bewd_count = 0, kuriboh_count = 0;
    for (card* c : d->game_field->player[0].list_main) {
        CHECK_TRUE(c != nullptr, "loaded p0 card non-null");
        if (c->data.code == 46986414u) ++dm_count;
        else if (c->data.code == 89631139u) ++bewd_count;
        else if (c->data.code == 40640057u) ++kuriboh_count;
        else CHECK_TRUE(false, "loaded card has unknown code");
    }
    CHECK_EQ(dm_count, 3, "p0 has 3 Dark Magicians");
    CHECK_EQ(bewd_count, 3, "p0 has 3 Blue-Eyes");
    CHECK_EQ(kuriboh_count, 3, "p0 has 3 Kuribohs");

    OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
    return true;
}

// Helper: drain Process+GetMessage into a flat byte stream until the
// engine returns END or AWAITING. AWAITING means the engine wants a
// response; for the chunk-5a vanilla-monsters fixture, StartDuel + the
// initial draw both proceed without awaiting, so we expect to see
// AWAITING (game's first decision point) or END.
//
// Returns the captured raw MSG bytes (the same bytes EDOPro / WindBot
// see). Comparison between two duels is byte-equal on these bytes.
std::vector<uint8_t> drive_to_first_pause(OCG_Duel duel) {
    std::vector<uint8_t> out;
    while (true) {
        const int status = OCG_DuelProcess(duel);
        uint32_t len = 0;
        void* msgs = OCG_DuelGetMessage(duel, &len);
        if (msgs && len > 0) {
            const uint8_t* p = static_cast<const uint8_t*>(msgs);
            out.insert(out.end(), p, p + len);
        }
        if (status == OCG_DUEL_STATUS_END ||
            status == OCG_DUEL_STATUS_AWAITING) {
            break;
        }
        // Shouldn't loop forever in vanilla-monsters StartDuel; defensive cap.
        if (out.size() > 1u << 20) {
            std::fprintf(stderr,
                "drive_to_first_pause: msg stream exceeded 1 MB; aborting\n");
            break;
        }
    }
    return out;
}

bool test_chunk5a_msg_stream_baseline_vs_load() {
    // The headline chunk-5a test (per user note #2): real MSG-stream
    // comparison on a non-vanilla state.
    //
    // Sequence:
    //   1. Control duel C: CreateDuel + DuelNewCard + StartDuel + Process
    //      until first pause; capture MSG stream.
    //   2. Original duel A: CreateDuel + DuelNewCard (same cards, same seed);
    //      save here (pre-StartDuel) to avoid hitting chunk-5a's
    //      processor-state fail-loud.
    //   3. Loaded duel B: load A's blob + StartDuel + Process until first
    //      pause; capture MSG stream.
    //   4. Assert C's stream == B's stream. The MSGs include shuffled deck
    //      reveal etc., which depend on the RNG state save+load preserves.
    constexpr uint64_t kSeed = 0x5EED1234ABCDEF00ULL;

    // Control: build, start, drive
    OCG_Duel control = make_chunk5a_duel(kSeed);
    CHECK_TRUE(control != nullptr, "create control");
    populate_simple_deck(control);
    OCG_StartDuel(control);
    auto stream_control = drive_to_first_pause(control);

    // Original: build, save (pre-StartDuel)
    OCG_Duel orig = make_chunk5a_duel(kSeed);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);
    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save orig pre-StartDuel");

    // Loaded: load, start, drive
    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");
    OCG_StartDuel(loaded);
    auto stream_loaded = drive_to_first_pause(loaded);

    CHECK_TRUE(!stream_control.empty(), "control produced non-empty MSG stream");
    CHECK_EQ(stream_control.size(), stream_loaded.size(),
             "MSG stream sizes match");
    if (stream_control != stream_loaded) {
        for (size_t i = 0; i < stream_control.size() &&
                            i < stream_loaded.size(); ++i) {
            if (stream_control[i] != stream_loaded[i]) {
                std::fprintf(stderr,
                    "FAIL: MSG stream byte %zu differs: control=0x%02x "
                    "loaded=0x%02x (control size=%zu, loaded size=%zu)\n",
                    i, stream_control[i], stream_loaded[i],
                    stream_control.size(), stream_loaded.size());
                break;
            }
        }
        OCG_FreeSaveBuffer(blob);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(control); OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(control); OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// CHUNK 5b Wave 1: card schema extensions for effect-targeting load-stability
// (clarification #3). Mutate effect_target_cards / effect_target_owner /
// material_cards on a save-side duel, save → load → save, assert byte-equal
// AND that the new fields restored correctly.
// ---------------------------------------------------------------------------

bool test_chunk5b_card_set_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0xC5B1);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);  // 18 cards across 2 players

    auto* d_orig = static_cast<duel*>(orig);

    // Pick three cards from p0's deck and wire up cross-references in the
    // new chunk-5b card_set fields. These would normally be populated by
    // effect resolution mid-game; we mutate directly to test the schema.
    CHECK_TRUE(d_orig->game_field->player[0].list_main.size() >= 3,
               "need at least 3 cards");
    card* a = d_orig->game_field->player[0].list_main[0];
    card* b = d_orig->game_field->player[0].list_main[1];
    card* c = d_orig->game_field->player[0].list_main[2];
    CHECK_TRUE(a && b && c, "card pointers non-null");

    a->effect_target_cards.insert(b);
    a->effect_target_cards.insert(c);
    a->effect_target_owner.insert(b);
    a->material_cards.insert(c);
    b->material_cards.insert(a);

    void* blob1 = nullptr;
    uint32_t size1 = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob1, &size1), OCG_SAVE_OK,
             "save with mutated card_sets");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob1, size1, &opts, &loaded), OCG_LOAD_OK,
             "load");

    void* blob2 = nullptr;
    uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "re-save");

    CHECK_EQ(size1, size2, "round-trip sizes match");
    if (std::memcmp(blob1, blob2, size1) != 0) {
        const uint8_t* x = static_cast<const uint8_t*>(blob1);
        const uint8_t* y = static_cast<const uint8_t*>(blob2);
        for (uint32_t i = 0; i < size1; ++i) {
            if (x[i] != y[i]) {
                std::fprintf(stderr,
                    "FAIL: card_set round-trip byte %u differs: "
                    "0x%02x vs 0x%02x\n", i, x[i], y[i]);
                break;
            }
        }
        OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
        return false;
    }

    // Direct verification on loaded duel.
    auto* d_loaded = static_cast<duel*>(loaded);
    auto find_by_cardid = [&](uint32_t cardid) -> card* {
        for (card* p : d_loaded->cards) {
            if (p && p->cardid == cardid) return p;
        }
        return nullptr;
    };
    card* a_loaded = find_by_cardid(a->cardid);
    card* b_loaded = find_by_cardid(b->cardid);
    card* c_loaded = find_by_cardid(c->cardid);
    CHECK_TRUE(a_loaded && b_loaded && c_loaded,
               "loaded counterparts located");
    CHECK_EQ(a_loaded->effect_target_cards.size(), 2u,
             "a.effect_target_cards size");
    CHECK_TRUE(a_loaded->effect_target_cards.count(b_loaded) == 1,
               "a.effect_target_cards contains b");
    CHECK_TRUE(a_loaded->effect_target_cards.count(c_loaded) == 1,
               "a.effect_target_cards contains c");
    CHECK_EQ(a_loaded->effect_target_owner.size(), 1u,
             "a.effect_target_owner size");
    CHECK_TRUE(a_loaded->effect_target_owner.count(b_loaded) == 1,
               "a.effect_target_owner contains b");
    CHECK_EQ(a_loaded->material_cards.size(), 1u, "a.material_cards size");
    CHECK_TRUE(a_loaded->material_cards.count(c_loaded) == 1,
               "a.material_cards contains c");
    CHECK_EQ(b_loaded->material_cards.size(), 1u, "b.material_cards size");
    CHECK_TRUE(b_loaded->material_cards.count(a_loaded) == 1,
               "b.material_cards contains a");

    OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Free-buffer is safe on null
// ---------------------------------------------------------------------------

bool test_free_null_is_safe() {
    OCG_FreeSaveBuffer(nullptr);  // must not crash
    return true;
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    int failed = 0;

    struct Test {
        const char* name;
        bool (*fn)();
    } tests[] = {
        // Chunk 3
        {"byte_equal_intra_path", &test_byte_equal_intra_path},
        {"blob_parses_with_expected_shape", &test_blob_parses_with_expected_shape},
        {"refuse_null_pointer_args", &test_refuse_null_pointer_args},
        {"distinct_seeds_yield_distinct_blobs", &test_distinct_seeds_yield_distinct_blobs},
        {"refuse_not_msg_boundary", &test_refuse_not_msg_boundary},
        {"free_null_is_safe", &test_free_null_is_safe},
        // Chunk 4 (load + round-trip)
        {"load_round_trip_byte_equal", &test_load_round_trip_byte_equal},
        {"load_preserves_rng_state", &test_load_preserves_rng_state},
        {"load_preserves_field_info", &test_load_preserves_field_info},
        {"load_preserves_player_lp", &test_load_preserves_player_lp},
        {"load_strict_output_not_empty", &test_load_strict_output_not_empty},
        {"load_malformed_blob", &test_load_malformed_blob},
        {"load_wrong_schema_version", &test_load_wrong_schema_version},
        {"load_refuse_tag", &test_load_refuse_tag},
        {"load_null_options_rejected", &test_load_null_options_rejected},
        // Chunk 5a (vanilla cards + MSG-stream)
        {"chunk5a_card_round_trip", &test_chunk5a_card_round_trip},
        {"chunk5a_msg_stream_baseline_vs_load",
         &test_chunk5a_msg_stream_baseline_vs_load},
        // Chunk 5b Wave 1 — card_set fields for effect-targeting
        {"chunk5b_card_set_round_trip", &test_chunk5b_card_set_round_trip},
        // Chunk 4 perf scaffold (informational; not gated)
        {"perf_scaffold_vanilla", &test_perf_scaffold_vanilla},
    };

    for (const auto& t : tests) {
        std::printf("[ RUN  ] %s\n", t.name);
        if (t.fn()) {
            std::printf("[ PASS ] %s\n", t.name);
        } else {
            std::printf("[ FAIL ] %s\n", t.name);
            ++failed;
        }
    }

    google::protobuf::ShutdownProtobufLibrary();
    if (failed == 0) {
        std::printf("test_save: ALL %zu PASSED\n",
                    sizeof(tests) / sizeof(tests[0]));
        return 0;
    } else {
        std::printf("test_save: %d FAILED\n", failed);
        return 1;
    }
}
