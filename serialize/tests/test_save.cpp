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
#include "card.h"      // chunk 9b: card.cardid / data.code access in tests
#include "duel.h"
#include "effect.h"    // chunk 9b: effect scalar field assignment in tests
#include "field.h"
#include "interpreter.h"
#include "processor_unit.h"  // chunk 9b: emplace_variant / get_opt_variant
#include "serialize/save_state.h"  // for direct refuse-reason inspection
#include "serialize/load_state.h"  // for direct deserialize_duel inspection (chunk 9b)

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
    CHECK_EQ(parsed.schema_version(), 2u, "schema_version");
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
    s.set_schema_version(2);
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
// CHUNK 9a Tier 1: ProcessorState round-trip at a real engine boundary.
//
// Drives a vanilla-deck duel through StartDuel + the initial setup MSGs
// until the engine first awaits a player decision. At that point
// core.units is non-empty (typically Adjust + Turn + a Select* unit at
// step 1). Save → load → re-save must be byte-equal.
//
// Verifies Tier 1 (Adjust / Turn / SelectIdleCmd / SelectPlace) is
// sufficient for the dominant ygoenv-reset boundary, and that the
// round-trip preserves engine state. If the boundary's units stack
// contains a Tier 2/3 type (e.g. SelectChain, SelectCard), save will
// refuse with the chunk-9a stub message — that's a signal to expand
// Tier 1's coverage list.
// ---------------------------------------------------------------------------

bool test_chunk9a_processor_state_round_trip() {
    constexpr uint64_t kSeed = 0x9A1E1ULL;

    OCG_Duel orig = make_chunk5a_duel(kSeed);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);
    OCG_StartDuel(orig);
    auto stream_orig = drive_to_first_pause(orig);
    CHECK_TRUE(!stream_orig.empty(),
               "orig produced non-empty MSG stream pre-pause");

    auto* d_orig = static_cast<duel*>(orig);
    if (d_orig->game_field != nullptr) {
        const auto& core = d_orig->game_field->core;
        std::printf("  9a tier1: post-StartDuel boundary has units=%zu "
                    "subunits=%zu sel_chains=%zu cur_chain=%zu\n",
                    core.units.size(), core.subunits.size(),
                    core.select_chains.size(), core.current_chain.size());
    }

    void* blob1 = nullptr;
    uint32_t size1 = 0;
    int s = OCG_DuelSaveState(orig, &blob1, &size1);
    if (s != OCG_SAVE_OK) {
        std::string out, reason;
        ocg::serialize::serialize_duel(*d_orig, &out, &reason);
        std::fprintf(stderr,
            "FAIL: 9a tier1 save status=%d reason=%s\n", s, reason.c_str());
        OCG_DestroyDuel(orig);
        return false;
    }
    std::printf("  9a tier1: saved %u bytes\n", size1);

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob1, size1, &opts, &loaded), OCG_LOAD_OK,
             "9a tier1 load");

    void* blob2 = nullptr;
    uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "9a tier1 re-save");

    if (size1 != size2 || std::memcmp(blob1, blob2, size1) != 0) {
        std::fprintf(stderr,
            "FAIL: 9a tier1 round-trip blobs differ (size1=%u size2=%u)\n",
            size1, size2);
        std::FILE* fp = std::fopen("/tmp/9a_tier1_orig.pb", "wb");
        if (fp) { std::fwrite(blob1, 1, size1, fp); std::fclose(fp); }
        fp = std::fopen("/tmp/9a_tier1_loaded.pb", "wb");
        if (fp) { std::fwrite(blob2, 1, size2, fp); std::fclose(fp); }
        std::fprintf(stderr,
            "  blobs dumped to /tmp/9a_tier1_{orig,loaded}.pb\n");
        OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
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
// CHUNK 5b Wave 3: Type-C fixture infrastructure
//
// Wave 1 + Wave 2 lifted fail-loud on cards/effects/groups/chain.links and
// landed the bytecode-dump escape hatch. Wave 3 wires up real-card-script
// fixtures to exercise the code paths.
//
// For Type-C closures we need:
//   - cardReader returning real card metadata (type/level/attribute/race)
//   - scriptReader loading actual .lua files from src/ygopro-scripts/
// ---------------------------------------------------------------------------

constexpr const char* kScriptsDir =
    "/mnt/c/Users/Joe/Documents/ExodAI/src/ygopro-scripts";

// Card data for Type-C fixtures. Same shape as the vanilla VanillaCard
// table but with type-bits set to identify them as spells/traps where
// relevant (Dueltaining is TYPE_SPELL+TYPE_FIELD).
struct ScriptedCard {
    uint32_t code;
    uint32_t type;
    uint32_t level;
    uint32_t attribute;
    uint64_t race;
    int32_t attack;
    int32_t defense;
};

// Type/location/pos constants per src/ygopro-scripts/constant.lua.
// (Repeated here so this test stays self-contained — the engine doesn't
// expose them via a C header.)
constexpr uint32_t kTypeMonster    = 0x1;
constexpr uint32_t kTypeSpell      = 0x2;
constexpr uint32_t kTypeEffect     = 0x20;
constexpr uint32_t kTypeRitual     = 0x80;
constexpr uint32_t kTypeContinuous = 0x20000;
constexpr uint32_t kTypeField      = 0x80000;

constexpr ScriptedCard kScriptedCards[] = {
    // Dueltaining (c19162134) — Field Spell. initial_effect registers
    // s.spcon(0)/s.drop(0)/s.btcon(0/1)/s.chcon(0/1)/s.damcon(0/1) — all
    // integer-capture Type-C closures.
    {19162134, kTypeSpell | kTypeField, 0, 0, 0, 0, 0},

    // Branded in Central Dogmatika (c14220547) — Continuous Spell. Two
    // Type-C closures via s.condition(TYPE_RITUAL) and s.condition(TYPE_FUSION).
    // Inner closure has integer `typ` upvalue.
    {14220547, kTypeSpell | kTypeContinuous, 0, 0, 0, 0, 0},

    // Hydor, the Base of All Things (c30339825) — Effect Monster. Type-C
    // closures via s.sptg(true|false) / s.spop(true|false). Inner closure
    // has boolean `water` upvalue.
    {30339825, kTypeMonster | kTypeEffect, 4, 0x8 /*ATTR_WATER*/, 0, 1500, 1500},

    // Vendread Reunion (c2266498) — Ritual Spell. Wrapper-sequencing
    // fixture: s.registerloccount(func) wraps target/operation in an outer
    // closure whose upvalue is the inner closure produced by Ritual.CreateProc.
    // Function-typed upvalue exercises the §13.4 refuse path (UNKNOWN type).
    {2266498, kTypeSpell | kTypeRitual, 0, 0, 0, 0, 0},

    // Earthbound Immortal Aslla piscu (c10875327) — Effect Monster.
    // Tier 2 fixture: initial_effect queues a SelfDestroy subunit (the
    // "destroy this card if you control no Field Spell" rule). Picked
    // from the §15 measurement's 47-card SelfDestroy class.
    {10875327, kTypeMonster | kTypeEffect, 10, 0x4 /*ATTR_DARK*/, 0, 2500, 2500},

    // Advanced Crystal Beast Amber Mammoth (c18847598) — Effect Monster.
    // Tier 2 fixture: initial_effect queues a SelfToGrave subunit. Picked
    // from the §15 measurement's 10-card SelfToGrave class. Crystal Beast
    // archetype concentration per the user's fixture-selection note.
    {18847598, kTypeMonster | kTypeEffect, 4, 0x10 /*ATTR_EARTH*/, 0, 1000, 1800},

    // §8.7 Type-C #1: Forbidden Dark Contract with the Swamp King (c10833828)
    // — Continuous Spell. Chunk 9b §8.7 regression fixture (3rd Type-C
    // closure example, completes the three originally specified).
    {10833828, kTypeSpell | kTypeContinuous, 0, 0, 0, 0, 0},

    // §8.7 Type-C #3: Darklord Eveningstar (c10136446) — Effect/Fusion
    // Monster. Chunk 9b §8.7 regression fixture.
    {10136446, kTypeMonster | kTypeEffect, 7, 0x4 /*ATTR_DARK*/, 0, 2800, 2000},
};

void scripted_card_reader(void* /*payload*/, uint32_t code,
                           OCG_CardData* data) {
    if (data == nullptr) return;
    std::memset(data, 0, sizeof(*data));
    // Try scripted cards first.
    for (const auto& v : kScriptedCards) {
        if (v.code == code) {
            data->code = v.code;
            data->type = v.type;
            data->level = v.level;
            data->attribute = v.attribute;
            data->race = v.race;
            data->attack = v.attack;
            data->defense = v.defense;
            return;
        }
    }
    // Fall through to vanilla cards.
    for (const auto& v : kVanillaCards) {
        if (v.code == code) {
            data->code = v.code;
            data->type = v.type;
            data->level = v.level;
            data->attribute = v.attribute;
            data->race = v.race;
            data->attack = v.attack;
            data->defense = v.defense;
            return;
        }
    }
    // Unknown — leave zeroed.
}

// Loads a script file from the ProjectIgnis corpus on disk and feeds
// it to the engine via OCG_LoadScript. Returns 1 on success, 0 if not
// found or failed to read (the engine treats 0 as "no script", which
// is fine for cards without per-card scripts).
int scripted_script_reader(void* /*payload*/, OCG_Duel duel,
                            const char* name) {
    // name is like "./script/c19162134.lua"; strip the leading "./script/"
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

OCG_Duel make_scripted_duel(uint64_t seed_lo) {
    OCG_DuelOptions opts{};
    opts.seed[0] = seed_lo;
    opts.seed[1] = 0xAA;
    opts.seed[2] = 0xBB;
    opts.seed[3] = 0xCC;
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &scripted_card_reader;
    opts.scriptReader = &scripted_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    OCG_Duel duel = nullptr;
    if (OCG_CreateDuel(&duel, &opts) != OCG_DUEL_CREATION_SUCCESS) {
        return nullptr;
    }
    // Bootstrap the script library — same pattern as ygoenv's
    // edopro.h:3687-3688. constant.lua + utility.lua provide the globals
    // (TYPE_*, EFFECT_*, aux.*, etc.) that card scripts depend on.
    scripted_script_reader(nullptr, duel, "constant.lua");
    scripted_script_reader(nullptr, duel, "utility.lua");
    return duel;
}

OCG_DuelOptions make_scripted_load_options() {
    OCG_DuelOptions opts{};
    opts.team1 = OCG_Player{8000, 5, 1};
    opts.team2 = OCG_Player{8000, 5, 1};
    opts.cardReader = &scripted_card_reader;
    opts.scriptReader = &scripted_script_reader;
    opts.logHandler = &stub_log_handler;
    opts.cardReaderDone = &stub_card_reader_done;
    return opts;
}

// ---------------------------------------------------------------------------
// Type-C fixture round-trip helper.
//
// Pattern: scripted_duel + DuelNewCard (which fires initial_effect and
// registers Type-C closures) → save → load → re-save → assert byte-equal.
// On byte-equal failure, dumps both blobs to /tmp/<tag>_{orig,loaded}.pb
// for external `protoc --decode_raw` diagnosis.
//
// On save refusal, calls serialize_duel directly to recover the
// refuse_reason (the C API drops it).
// ---------------------------------------------------------------------------
bool scripted_round_trip(const char* tag, uint32_t code,
                         uint32_t loc, uint32_t seq, uint32_t pos,
                         uint64_t seed_lo) {
    OCG_Duel orig = make_scripted_duel(seed_lo);
    CHECK_TRUE(orig != nullptr, "create orig");

    OCG_NewCardInfo info{};
    info.team = 0;
    info.duelist = 0;
    info.code = code;
    info.con = 0;
    info.loc = loc;
    info.seq = seq;
    info.pos = pos;
    OCG_DuelNewCard(orig, &info);

    auto* d_orig = static_cast<duel*>(orig);
    const size_t effect_count = d_orig->effects.size();
    CHECK_TRUE(effect_count > 0, "initial_effect registered effects");
    std::printf("  %s: %zu effects, %zu cards, %zu groups\n",
                tag, effect_count, d_orig->cards.size(), d_orig->groups.size());

    void* blob1 = nullptr;
    uint32_t size1 = 0;
    int s = OCG_DuelSaveState(orig, &blob1, &size1);
    if (s != OCG_SAVE_OK) {
        std::fprintf(stderr,
            "FAIL: %s save returned status=%d\n", tag, s);
        // Re-run via the C++ entry point to recover the refuse_reason
        // (the C API doesn't surface it).
        std::string out, reason;
        ocg::serialize::serialize_duel(*d_orig, &out, &reason);
        std::fprintf(stderr, "  refuse_reason: %s\n", reason.c_str());
        OCG_DestroyDuel(orig);
        return false;
    }

    OCG_DuelOptions opts = make_scripted_load_options();
    OCG_Duel loaded = nullptr;
    int ls = OCG_DuelLoadState(blob1, size1, &opts, &loaded);
    CHECK_EQ(ls, OCG_LOAD_OK, "load");

    void* blob2 = nullptr;
    uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "re-save after load");

    if (size1 != size2 || std::memcmp(blob1, blob2, size1) != 0) {
        std::fprintf(stderr,
            "FAIL: %s round-trip blobs differ (size1=%u size2=%u)\n",
            tag, size1, size2);
        char path[256];
        std::snprintf(path, sizeof(path), "/tmp/%s_orig.pb", tag);
        if (auto* fp = std::fopen(path, "wb")) {
            std::fwrite(blob1, 1, size1, fp); std::fclose(fp);
        }
        std::snprintf(path, sizeof(path), "/tmp/%s_loaded.pb", tag);
        if (auto* fp = std::fopen(path, "wb")) {
            std::fwrite(blob2, 1, size2, fp); std::fclose(fp);
        }
        std::fprintf(stderr,
            "  blobs dumped to /tmp/%s_{orig,loaded}.pb\n", tag);
        OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Type-C fixture #1: Dueltaining (c19162134) — Field Spell. Integer
// captures via s.spcon(0) / s.drop(0) / s.btcon(0/1) / s.chcon(0/1) /
// s.damcon(0/1).
// ---------------------------------------------------------------------------
bool test_chunk5b_dueltaining_round_trip() {
    constexpr uint32_t LOC_SZONE = 0x8;       // per constant.lua
    constexpr uint32_t POS_FACEUP_ATK = 0x1;  // per constant.lua
    return scripted_round_trip("dueltaining", 19162134,
                               LOC_SZONE, /*seq=*/5, POS_FACEUP_ATK, 0x5BD1);
}

// ---------------------------------------------------------------------------
// Type-C fixture #2: Branded in Central Dogmatika (c14220547) — Continuous
// Spell. Integer captures via s.condition(TYPE_RITUAL) and
// s.condition(TYPE_FUSION). Inner closure has integer `typ` upvalue.
// ---------------------------------------------------------------------------
bool test_chunk5b_branded_round_trip() {
    constexpr uint32_t LOC_SZONE = 0x8;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("branded", 14220547,
                               LOC_SZONE, /*seq=*/0, POS_FACEUP_ATK, 0x5BD2);
}

// ---------------------------------------------------------------------------
// Type-C fixture #3: Hydor, the Base of All Things (c30339825) — Effect
// Monster. Boolean captures via s.sptg(true|false) / s.spop(true|false).
// Inner closure has boolean `water` upvalue.
// ---------------------------------------------------------------------------
bool test_chunk5b_hydor_round_trip() {
    // initial_effect registers e1 (EFFECT_TYPE_ACTIVATE — defaults to hand
    // range for monsters) and e2 (EFFECT_TYPE_IGNITION with
    // SetRange(LOCATION_GRAVE)). Place in hand so e1 is in-range; both
    // effects register regardless.
    constexpr uint32_t LOC_HAND = 0x2;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("hydor", 30339825,
                               LOC_HAND, /*seq=*/0, POS_FACEUP_ATK, 0x5BD3);
}

// ---------------------------------------------------------------------------
// Wrapper-sequencing fixture: Vendread Reunion (c2266498) — Ritual Spell.
//
// s.registerloccount(func) returns a closure whose upvalue is the
// `func` parameter — a function. Pre-5c this refused with
// OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE. Chunk 5c added recursive
// function-upvalue dump (§13.4 extension), so Vendread now ROUND-TRIPS:
// the wrapper closure dumps the inner function recursively as a
// LuaCallback nested in CapturedArg.function_def.
// ---------------------------------------------------------------------------
bool test_chunk5b_vendread_wrapper_refuse() {
    constexpr uint32_t LOC_HAND = 0x2;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("vendread", 2266498,
                               LOC_HAND, /*seq=*/0, POS_FACEUP_ATK, 0x5BD4);
}

// ---------------------------------------------------------------------------
// Chunk 5c fixtures: extended classifier coverage.
// ---------------------------------------------------------------------------

// Empty-table fixture. Many cards have closures that capture a
// freshly-created `local set = {}` table for accumulator semantics.
// Pre-5c these refused as UNKNOWN; 5c handles via TableDef with empty
// entries.
//
// Picking a card whose initial_effect produces an empty-table upvalue
// from the chunk-6 corpus — see /tmp/chunk6_refuse_details.csv for
// "table: 0 keys" matches. c1174075 was one of the earliest in the
// chunk-6 top patterns.
bool test_chunk5c_empty_table_round_trip() {
    constexpr uint32_t LOC_MZONE = 0x4;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("empty_table", 1174075,
                               LOC_MZONE, /*seq=*/0, POS_FACEUP_ATK, 0x5C01);
}

// Function-valued-table fixture (the dispatch pattern). c43227 (Magnum
// the Reliever) uses Fusion.AddProcMix → aux.FilterBoolFunctionEx,
// which produces a closure whose effect.condition has a Card.IsLocation
// C function as inner upvalue. Pre-5c-A this refused; chunk 5c-A's
// C-function-name registry resolves Card.IsLocation by qualified name.
// Now round-trips byte-equal.
bool test_chunk5c_function_table_round_trip() {
    constexpr uint32_t LOC_MZONE = 0x4;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("function_table", 43227,
                               LOC_MZONE, /*seq=*/0, POS_FACEUP_ATK, 0x5C02);
}

// Intra-card shared-table fixture. The chunk-5c.0 characterization found
// 22.5% of refused cards have intra-card sharing (same lua_topointer
// captured by multiple closures). Pick a card that exhibits this — the
// Pendulum.AddProcedure family is one such, since `Pendulum.Condition()`
// and `Pendulum.Operation()` both reference shared `Pendulum.*` tables.
//
// c20281581 (Performapal Momoncarpet) calls Pendulum.AddProcedure but
// also hits the C-function (lua_dump=1) refuse path — we can't use it
// for a positive round-trip test. Pick a different Pendulum card or
// any card whose 5c.0 sharing detection flagged it. For now, the cross-
// card test below exercises the registry mechanism via two cards; the
// intra-card path is exercised by any card whose closures share a table
// (which is implicit in the empty/function-table fixtures above).
//
// (Marker test — kept as documentation. Real sharing coverage comes
// from the chunk-6 corpus re-run.)

// Cross-card shared-table fixture. Per the 5c task scope, surface
// whether per-card registry is sufficient. Two cards from the same
// archetype that BOTH capture a constant table defined at script-set
// scope (e.g., Crystal Beast set table). If per-card registry is enough,
// each card emits its own TableDef independently — both succeed but the
// loaded-side tables are independent (no cross-card identity preserved).
// If the captured table mutates and cross-card identity matters, this
// test would surface that as a divergence. For now we just assert that
// both cards round-trip without refuse — the qualitative finding is in
// the corpus measurement.
//
// Picking two Crystal Beast cards (well-known archetype with a shared
// `s.listed_series` and `aux.AddCodeList` patterns):
//   c39111158 - Crystal Beast Sapphire Pegasus
//   c52232652 - Crystal Beast Topaz Tiger
// Both have initial_effect that registers Crystal-Beast-specific
// closures; if cross-card sharing is real for this archetype, refuse
// rate / divergence will show in the corpus measurement.
bool test_chunk5c_cross_card_shared_round_trip() {
    OCG_Duel orig = make_scripted_duel(0x5C03);
    CHECK_TRUE(orig != nullptr, "create orig");

    OCG_NewCardInfo info_a{};
    info_a.code = 39111158;  // Sapphire Pegasus
    info_a.team = 0; info_a.duelist = 0; info_a.con = 0;
    info_a.loc = 0x4; info_a.seq = 0; info_a.pos = 0x1;
    OCG_DuelNewCard(orig, &info_a);

    OCG_NewCardInfo info_b{};
    info_b.code = 52232652;  // Topaz Tiger
    info_b.team = 0; info_b.duelist = 0; info_b.con = 0;
    info_b.loc = 0x4; info_b.seq = 1; info_b.pos = 0x1;
    OCG_DuelNewCard(orig, &info_b);

    auto* d_orig = static_cast<duel*>(orig);
    std::printf("  cross_card: %zu effects, %zu cards, %zu groups\n",
                d_orig->effects.size(), d_orig->cards.size(),
                d_orig->groups.size());

    void* blob1 = nullptr; uint32_t size1 = 0;
    int s = OCG_DuelSaveState(orig, &blob1, &size1);
    if (s != OCG_SAVE_OK) {
        std::string out, reason;
        ocg::serialize::serialize_duel(*d_orig, &out, &reason);
        std::fprintf(stderr,
            "FAIL: cross_card save status=%d reason=%s\n",
            s, reason.c_str());
        OCG_DestroyDuel(orig);
        return false;
    }

    OCG_DuelOptions opts = make_scripted_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob1, size1, &opts, &loaded), OCG_LOAD_OK,
             "cross_card load");

    void* blob2 = nullptr; uint32_t size2 = 0;
    CHECK_EQ(OCG_DuelSaveState(loaded, &blob2, &size2), OCG_SAVE_OK,
             "cross_card re-save");

    if (size1 != size2 || std::memcmp(blob1, blob2, size1) != 0) {
        std::fprintf(stderr,
            "FAIL: cross_card byte-equal differs (size1=%u size2=%u)\n",
            size1, size2);
        std::FILE* fp = std::fopen("/tmp/cross_card_orig.pb", "wb");
        if (fp) { std::fwrite(blob1, 1, size1, fp); std::fclose(fp); }
        fp = std::fopen("/tmp/cross_card_loaded.pb", "wb");
        if (fp) { std::fwrite(blob2, 1, size2, fp); std::fclose(fp); }
        OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
        OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob1); OCG_FreeSaveBuffer(blob2);
    OCG_DestroyDuel(orig); OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// CHUNK 9a Tier 2: subunit round-trip — single-card-add scenarios where
// initial_effect queues a SelfDestroy or SelfToGrave subunit.
//
// Per §15 characterization, both variants are trivial (only `step`
// field). Round-trip = scripted_round_trip helper byte-equal — no
// special handling needed since the scripted_round_trip path doesn't
// call StartDuel and therefore doesn't trip the chain_lists guard.
// ---------------------------------------------------------------------------

bool test_chunk9a_tier2_self_destroy_round_trip() {
    // Earthbound Immortal Aslla piscu (c10875327): SelfDestroy variant.
    constexpr uint32_t LOC_HAND = 0x2;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("9a_tier2_self_destroy", 10875327,
                               LOC_HAND, /*seq=*/0, POS_FACEUP_ATK, 0x9A21);
}

bool test_chunk9a_tier2_self_to_grave_round_trip() {
    // Advanced Crystal Beast Amber Mammoth (c18847598): SelfToGrave variant.
    constexpr uint32_t LOC_HAND = 0x2;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("9a_tier2_self_to_grave", 18847598,
                               LOC_HAND, /*seq=*/0, POS_FACEUP_ATK, 0x9A22);
}

// ---------------------------------------------------------------------------
// §8.7 regression matrix — Type-C closure fixtures (partial).
//
// The §8.7 plan calls for 3 specific Type-C card fixtures as named
// regression tests: Forbidden Dark Contract (c10833828), Dueltaining
// (c19162134), Darklord Eveningstar (c10136446). Dueltaining is
// already covered by chunk5b_dueltaining_round_trip; these two fill
// out the set.
//
// The remaining §8.7 cases (mid-chain, activation window, post-control-
// swap, Xyz materials, continuous effect on field, pending deck
// search, token on field, nonzero counters, equipment chain) all
// require multi-step scripted setup beyond the single-card-add
// pattern. Deferred to morning or later work — per user guidance,
// partial suite is better than no suite; flaky is worse than absent.
// ---------------------------------------------------------------------------
bool test_chunk9b_forbidden_dark_contract_round_trip() {
    constexpr uint32_t LOC_SZONE = 0x8;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("§8.7_forbidden_dark_contract", 10833828,
                               LOC_SZONE, /*seq=*/0, POS_FACEUP_ATK, 0x87C1);
}

bool test_chunk9b_darklord_eveningstar_round_trip() {
    constexpr uint32_t LOC_HAND = 0x2;
    constexpr uint32_t POS_FACEUP_ATK = 0x1;
    return scripted_round_trip("§8.7_darklord_eveningstar", 10136446,
                               LOC_HAND, /*seq=*/0, POS_FACEUP_ATK, 0x87C3);
}

// ---------------------------------------------------------------------------
// MSG-stream verification helper for Type-C fixtures.
//
// Sequence:
//   1. Control C: scripted_duel + DuelNewCard + StartDuel + drive to first
//      pause; capture MSG stream.
//   2. Original A: scripted_duel + DuelNewCard + save (pre-StartDuel).
//   3. Loaded B: load A's blob + StartDuel + drive to first pause; capture
//      MSG stream.
//   4. Assert C's stream == B's stream byte-for-byte.
//
// Proves: bytecode-restored Type-C closures actually execute correctly
// post-load (not just that the blob round-trips byte-equal).
// ---------------------------------------------------------------------------
bool scripted_msg_stream_verify(const char* tag, uint32_t code,
                                uint32_t loc, uint32_t seq, uint32_t pos,
                                uint64_t seed_lo) {
    auto build_with_card = [&](OCG_Duel d) {
        OCG_NewCardInfo info{};
        info.team = 0;
        info.duelist = 0;
        info.code = code;
        info.con = 0;
        info.loc = loc;
        info.seq = seq;
        info.pos = pos;
        OCG_DuelNewCard(d, &info);
    };

    // Control: build + start + drive
    OCG_Duel control = make_scripted_duel(seed_lo);
    CHECK_TRUE(control != nullptr, "create control");
    build_with_card(control);
    OCG_StartDuel(control);
    auto stream_control = drive_to_first_pause(control);

    // Original: build + save (pre-StartDuel, same as chunk-5a pattern to
    // avoid the processor-state fail-loud).
    OCG_Duel orig = make_scripted_duel(seed_lo);
    CHECK_TRUE(orig != nullptr, "create orig");
    build_with_card(orig);
    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save orig pre-StartDuel");

    // Loaded: load + start + drive
    OCG_DuelOptions opts = make_scripted_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");
    OCG_StartDuel(loaded);
    auto stream_loaded = drive_to_first_pause(loaded);

    CHECK_TRUE(!stream_control.empty(),
               "control produced non-empty MSG stream");
    if (stream_control.size() != stream_loaded.size() ||
        stream_control != stream_loaded) {
        std::fprintf(stderr,
            "FAIL: %s MSG stream divergence: control=%zu bytes, "
            "loaded=%zu bytes\n", tag,
            stream_control.size(), stream_loaded.size());
        for (size_t i = 0; i < stream_control.size() &&
                            i < stream_loaded.size(); ++i) {
            if (stream_control[i] != stream_loaded[i]) {
                std::fprintf(stderr,
                    "  first diff at byte %zu: control=0x%02x loaded=0x%02x\n",
                    i, stream_control[i], stream_loaded[i]);
                break;
            }
        }
        OCG_FreeSaveBuffer(blob);
        OCG_DestroyDuel(orig);
        OCG_DestroyDuel(control);
        OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(control);
    OCG_DestroyDuel(loaded);
    return true;
}

bool test_chunk5b_dueltaining_msg_stream() {
    return scripted_msg_stream_verify("dueltaining", 19162134,
                                       0x8, 5, 0x1, 0x5BD1);
}

bool test_chunk5b_branded_msg_stream() {
    return scripted_msg_stream_verify("branded", 14220547,
                                       0x8, 0, 0x1, 0x5BD2);
}

bool test_chunk5b_hydor_msg_stream() {
    return scripted_msg_stream_verify("hydor", 30339825,
                                       0x2, 0, 0x1, 0x5BD3);
}

// ---------------------------------------------------------------------------
// CHUNK 9b: regression test suite for the smoke-test abort fix.
//
// Pre-9b, save→load at any Select* decision boundary produced a duel
// with empty validation lists (summonable_cards / select_chains / etc.
// are populated by the previous-step processor and live in
// field.processor — which chunk-9a never serialized). Post-load
// SetResponse + Process tripped MSG_RETRY → unhandled std::runtime_error
// → std::terminate from envpool's worker thread.
//
// These tests verify:
//   1. The smoke-test abort scenario itself: save → load → SetResponse
//      → Process → no MSG_RETRY emitted (the headline fix).
//   2. field.processor scratch fields round-trip with handle resolution.
//   3. select_chains save honours the B1 UAF guard
//      (assign_effect_if_live) so a dead-effect chain is dropped to
//      handle 0 instead of dereferencing freed memory.
//   4. v1 blobs are rejected with a re-record instruction (not silently
//      loaded with empty Select* state — which would mask the bug).
//   5. Synthetic round-trip of Tier 3 ProcessorUnit variants
//      (SelectChain etc.) — the proto/save/load wiring is correct
//      independent of whether the YugiKaiba corpus surfaces them.
// ---------------------------------------------------------------------------

// Scan a generate_buffer-format byte stream for MSG_RETRY (=1). Format
// per duel::generate_buffer: each message is [uint32 size][size bytes
// where bytes[0] = msg_type, bytes[1..] = payload].
bool stream_has_msg_retry(const std::vector<uint8_t>& bytes) {
    size_t pos = 0;
    while (pos + 4 <= bytes.size()) {
        uint32_t sz = 0;
        std::memcpy(&sz, &bytes[pos], 4);
        pos += 4;
        if (sz == 0 || pos + sz > bytes.size()) break;
        if (bytes[pos] == MSG_RETRY) return true;
        pos += sz;
    }
    return false;
}

// Find the offset (start of size header) of the first MSG_SELECT_IDLECMD
// in a generate_buffer stream, or std::string::npos if not present.
// The scratch-list bug surfaces specifically at this prompt — finding
// it in the orig stream confirms the fixture genuinely reaches the
// failure mode the eval described.
size_t stream_find_msg(const std::vector<uint8_t>& bytes, uint8_t msg_type) {
    size_t pos = 0;
    while (pos + 4 <= bytes.size()) {
        uint32_t sz = 0;
        std::memcpy(&sz, &bytes[pos], 4);
        const size_t header_pos = pos;
        pos += 4;
        if (sz == 0 || pos + sz > bytes.size()) break;
        if (bytes[pos] == msg_type) return header_pos;
        pos += sz;
    }
    return std::string::npos;
}

// Headline regression test: drive a vanilla deck through StartDuel into
// the first SelectIdleCmd::step==1 pause. Save, load. SetResponse with
// the to-EP action (t=7, which validates against core.to_ep — an
// otherwise-empty processor field that v1 dropped). Process. Assert
// no MSG_RETRY. Pre-9b this would terminate; post-9b it advances.
bool test_chunk9b_save_load_step_no_msg_retry() {
    constexpr uint64_t kSeed = 0x9B01ULL;

    OCG_Duel orig = make_chunk5a_duel(kSeed);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);
    OCG_StartDuel(orig);
    auto stream_orig = drive_to_first_pause(orig);
    CHECK_TRUE(!stream_orig.empty(), "orig produced non-empty stream");

    // Confirm the fixture actually reaches a SelectIdleCmd prompt.
    // If the stream doesn't contain MSG_SELECT_IDLECMD, the simple-deck
    // first-turn path may have changed and the test isn't probing the
    // bug surface anymore — fail loudly so it's diagnosed not silently
    // skipped.
    CHECK_TRUE(
        stream_find_msg(stream_orig, MSG_SELECT_IDLECMD) != std::string::npos,
        "orig stream contains MSG_SELECT_IDLECMD (paused at idle prompt)");

    auto* d_orig = static_cast<duel*>(orig);
    // At step==1 of SelectIdleCmd, core.to_ep is set (vanilla MP1 turn).
    // If it isn't, the test's chosen action below would itself
    // legitimately MSG_RETRY — fail before that confuses diagnosis.
    CHECK_TRUE(d_orig->game_field != nullptr, "orig game_field");
    CHECK_TRUE(d_orig->game_field->core.to_ep,
               "orig core.to_ep is set at SelectIdleCmd boundary");

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save at SelectIdleCmd boundary");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");

    auto* d_loaded = static_cast<duel*>(loaded);
    CHECK_TRUE(d_loaded->game_field != nullptr, "loaded game_field");
    CHECK_TRUE(d_loaded->game_field->core.to_ep,
               "loaded core.to_ep restored (chunk 9b: was always false pre-fix)");

    // SelectIdleCmd response encoding: low 16 bits = command type t,
    // high 16 bits = sub-index s. t=7 → To-EP. No s needed.
    int32_t response = 7;
    OCG_DuelSetResponse(loaded, &response, sizeof(response));

    // Drain Process; MSG_RETRY would appear here pre-9b. Cap iterations
    // so a regression in load doesn't infinite-loop the test.
    std::vector<uint8_t> stream_loaded;
    for (int iter = 0; iter < 1000; ++iter) {
        const int status = OCG_DuelProcess(loaded);
        uint32_t len = 0;
        void* msgs = OCG_DuelGetMessage(loaded, &len);
        if (msgs && len > 0) {
            const uint8_t* p = static_cast<const uint8_t*>(msgs);
            stream_loaded.insert(stream_loaded.end(), p, p + len);
        }
        if (status == OCG_DUEL_STATUS_END ||
            status == OCG_DUEL_STATUS_AWAITING) {
            break;
        }
    }

    if (stream_has_msg_retry(stream_loaded)) {
        std::fprintf(stderr,
            "FAIL: post-load Step emitted MSG_RETRY — chunk-9b regression. "
            "Loaded duel rejected the t=7 (to_ep) response that v2 schema "
            "is supposed to make valid by restoring core.to_ep / "
            "core.summonable_cards / core.select_chains.\n");
        OCG_FreeSaveBuffer(blob);
        OCG_DestroyDuel(orig);
        OCG_DestroyDuel(loaded);
        return false;
    }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// Verify field.processor scratch fields round-trip *semantically* (not
// just byte-equal — that's covered by chunk9a_processor_state_round_trip).
// At the SelectIdleCmd boundary, summonable_cards may be empty for the
// vanilla-monster fixture (no special-summon-procedure-having cards in
// hand), but core.to_ep / core.to_bp / core.hint_timing should match.
bool test_chunk9b_processor_scratch_semantic_round_trip() {
    constexpr uint64_t kSeed = 0x9B02ULL;

    OCG_Duel orig = make_chunk5a_duel(kSeed);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);
    OCG_StartDuel(orig);
    drive_to_first_pause(orig);

    auto* d_orig = static_cast<duel*>(orig);
    const auto& core_orig = d_orig->game_field->core;
    const bool   orig_to_bp        = core_orig.to_bp;
    const bool   orig_to_m2        = core_orig.to_m2;
    const bool   orig_to_ep        = core_orig.to_ep;
    const bool   orig_skip_m2      = core_orig.skip_m2;
    const uint32_t orig_ht0        = core_orig.hint_timing[0];
    const uint32_t orig_ht1        = core_orig.hint_timing[1];
    const size_t orig_summ_n       = core_orig.summonable_cards.size();
    const size_t orig_spsumm_n     = core_orig.spsummonable_cards.size();
    const size_t orig_repos_n      = core_orig.repositionable_cards.size();
    const size_t orig_mset_n       = core_orig.msetable_cards.size();
    const size_t orig_sset_n       = core_orig.ssetable_cards.size();
    const size_t orig_sel_chains_n = core_orig.select_chains.size();

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");

    auto* d_loaded = static_cast<duel*>(loaded);
    const auto& core_loaded = d_loaded->game_field->core;
    CHECK_EQ(core_loaded.to_bp,         orig_to_bp,        "to_bp");
    CHECK_EQ(core_loaded.to_m2,         orig_to_m2,        "to_m2");
    CHECK_EQ(core_loaded.to_ep,         orig_to_ep,        "to_ep");
    CHECK_EQ(core_loaded.skip_m2,       orig_skip_m2,      "skip_m2");
    CHECK_EQ(core_loaded.hint_timing[0], orig_ht0,         "hint_timing[0]");
    CHECK_EQ(core_loaded.hint_timing[1], orig_ht1,         "hint_timing[1]");
    CHECK_EQ(core_loaded.summonable_cards.size(),    orig_summ_n,
             "summonable_cards size");
    CHECK_EQ(core_loaded.spsummonable_cards.size(),  orig_spsumm_n,
             "spsummonable_cards size");
    CHECK_EQ(core_loaded.repositionable_cards.size(),orig_repos_n,
             "repositionable_cards size");
    CHECK_EQ(core_loaded.msetable_cards.size(),      orig_mset_n,
             "msetable_cards size");
    CHECK_EQ(core_loaded.ssetable_cards.size(),      orig_sset_n,
             "ssetable_cards size");
    CHECK_EQ(core_loaded.select_chains.size(),       orig_sel_chains_n,
             "select_chains size");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// Synthetically populate summonable_cards + select_chains, save, load,
// verify the entries resolved back to the right cards / effects (by
// cardid / initial_id, since pointer values won't match cross-allocation).
bool test_chunk9b_synthetic_scratch_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0x9B03);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    auto* d_orig = static_cast<duel*>(orig);
    auto& core_orig = d_orig->game_field->core;

    // Pick three cards from p0 deck. Push them into summonable_cards
    // and msetable_cards. Capture cardids for post-load identity.
    CHECK_TRUE(d_orig->game_field->player[0].list_main.size() >= 3,
               "need 3 cards");
    card* a = d_orig->game_field->player[0].list_main[0];
    card* b = d_orig->game_field->player[0].list_main[1];
    card* c = d_orig->game_field->player[0].list_main[2];
    const uint32_t a_id = a->cardid;
    const uint32_t b_id = b->cardid;
    const uint32_t c_id = c->cardid;
    core_orig.summonable_cards.push_back(a);
    core_orig.summonable_cards.push_back(b);
    core_orig.msetable_cards.push_back(c);
    core_orig.select_options.push_back(0xCAFE);
    core_orig.select_options.push_back(0xBEEF);
    core_orig.to_bp = true;
    core_orig.to_m2 = false;
    core_orig.to_ep = true;
    core_orig.hint_timing[0] = 0x12345678;
    core_orig.hint_timing[1] = 0xAABBCCDD;

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK, "save");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");

    auto* d_loaded = static_cast<duel*>(loaded);
    const auto& core_loaded = d_loaded->game_field->core;

    CHECK_EQ(core_loaded.summonable_cards.size(), 2u, "summonable_cards size");
    CHECK_EQ(core_loaded.summonable_cards[0]->cardid, a_id, "summ[0] cardid");
    CHECK_EQ(core_loaded.summonable_cards[1]->cardid, b_id, "summ[1] cardid");
    CHECK_EQ(core_loaded.msetable_cards.size(),    1u, "msetable size");
    CHECK_EQ(core_loaded.msetable_cards[0]->cardid, c_id, "msetable[0]");
    CHECK_EQ(core_loaded.select_options.size(),   2u, "select_options size");
    CHECK_EQ(core_loaded.select_options[0], 0xCAFEull, "select_options[0]");
    CHECK_EQ(core_loaded.select_options[1], 0xBEEFull, "select_options[1]");
    CHECK_EQ(core_loaded.to_bp, true,               "to_bp");
    CHECK_EQ(core_loaded.to_m2, false,              "to_m2");
    CHECK_EQ(core_loaded.to_ep, true,               "to_ep");
    CHECK_EQ(core_loaded.hint_timing[0], 0x12345678u, "hint_timing[0]");
    CHECK_EQ(core_loaded.hint_timing[1], 0xAABBCCDDu, "hint_timing[1]");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// B1 regression. Populate select_chains with a synthetic chain whose
// triggering_effect was deleted via duel.delete_effect (which doesn't
// scrub the chain's effect pointer). assign_effect_if_live must catch
// this and write handle 0 — same shape as the original B1 UAF guard
// for chain::triggering_effect / card_state::reason_effect. If the
// guard fires, save returns OK with no UAF and the loaded blob has
// triggering_effect == nullptr (the effect simply doesn't exist on
// the load side, so this is the correct semantic).
bool test_chunk9b_select_chains_freed_effect_dropped() {
    OCG_Duel orig = make_chunk5a_duel(0x9B04);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    auto* d_orig = static_cast<duel*>(orig);

    // Allocate a fresh effect via duel::new_effect, attach to the
    // first p0 card, then push a chain referencing it. Then delete
    // the effect (mimicking mid-resolution cleanup paths the engine
    // takes).
    effect* e = d_orig->new_effect();
    e->initial_id = 0xBADC0DE;
    e->id = 0xBADC0DE;
    card* owner = d_orig->game_field->player[0].list_main[0];
    e->owner = owner;
    e->handler = owner;
    owner->single_effect.emplace(0x100, e);

    chain c{};
    c.chain_id = 99;
    c.triggering_player = 0;
    c.triggering_effect = e;
    d_orig->game_field->core.select_chains.push_back(c);

    // Now free the effect. The chain's pointer is now dangling; B1's
    // assign_effect_if_live should catch it via the live_effects set.
    d_orig->delete_effect(e);

    void* blob = nullptr;
    uint32_t size = 0;
    int s = OCG_DuelSaveState(orig, &blob, &size);
    CHECK_EQ(s, OCG_SAVE_OK,
             "save with freed-effect chain — guard kicks in, no UAF");
    CHECK_TRUE(size > 0, "non-empty blob");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");

    auto* d_loaded = static_cast<duel*>(loaded);
    CHECK_EQ(d_loaded->game_field->core.select_chains.size(), 1u,
             "select_chains entry preserved");
    // Effect handle was 0 (dropped) → load resolves to nullptr.
    CHECK_TRUE(
        d_loaded->game_field->core.select_chains.front().triggering_effect == nullptr,
        "loaded chain.triggering_effect is nullptr (B1 guard fired)");

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// Verify the v1→v2 schema bump is loud, not silent. A blob tagged v1
// must be rejected with a re-record instruction. (load_wrong_schema_version
// already covers v=999 → REJECT; this test specifically pins the
// pre-fix v=1 case which is the realistic regression the bump
// guards against.)
bool test_chunk9b_v1_blob_rejected_loud() {
    ocg::state::DuelState s;
    s.set_schema_version(1);  // pre-fix value
    auto* rng = s.mutable_rng();
    for (int i = 0; i < 4; ++i) rng->add_xoshiro_state(0);
    s.add_players()->set_lp(8000);
    s.add_players()->set_lp(8000);
    s.mutable_field_info();
    std::string bytes;
    CHECK_TRUE(s.SerializeToString(&bytes), "serialize v1 blob");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel out = nullptr;
    int status = OCG_DuelLoadState(bytes.data(),
                                   static_cast<uint32_t>(bytes.size()),
                                   &opts, &out);
    CHECK_EQ(status, OCG_LOAD_ERR_SCHEMA_VERSION,
             "v1 blob rejected with SCHEMA_VERSION error");
    CHECK_TRUE(out == nullptr, "no duel allocated");

    // Direct serialize_duel call to introspect the load_error message
    // (the public ABI returns only the status code; the loud-message
    // assertion goes through the internal entry point, mirrored from
    // test_load_wrong_schema_version's pattern).
    {
        std::string load_error;
        duel* d = nullptr;
        OCG_LoadStatus s2 = ocg::serialize::deserialize_duel(
            bytes.data(), bytes.size(), opts, &d, &load_error);
        CHECK_EQ(s2, OCG_LOAD_ERR_SCHEMA_VERSION, "internal entry status");
        CHECK_TRUE(load_error.find("re-record") != std::string::npos,
                   "load_error mentions 're-record' (loud rejection)");
        CHECK_TRUE(load_error.find("schema version 1") != std::string::npos ||
                   load_error.find("schema version") != std::string::npos,
                   "load_error mentions schema version");
        if (d) delete d;  // defensive; deserialize_duel should not allocate on error
    }
    return true;
}

// Synthetic round-trip of every Tier 3 ProcessorUnit variant. Push
// one of each into core.units, save, load, verify each variant
// materialized correctly. Replaces the case-by-case "drive engine
// into specific Select* state" approach (which would require a much
// richer scripted-card fixture for each variant) with a synthetic
// fixture that covers the proto/save/load wiring directly.
bool test_chunk9b_tier3_synthetic_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0x9B05);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    auto* d_orig = static_cast<duel*>(orig);
    auto& units = d_orig->game_field->core.units;
    units.clear();

    // Pick a card to embed in SelectEffectYesNo's pcard field.
    card* pcard = d_orig->game_field->player[0].list_main[0];
    const uint32_t pcard_id = pcard->cardid;

    // Push one of each Tier 3 variant. Specific field values chosen to
    // be distinctive (avoid 0/1 collisions) so a copy-paste bug in
    // save or load surfaces immediately. See processor_unit.h for
    // each constructor signature.
    Processors::emplace_variant<Processors::SelectBattleCmd>(units,
        uint16_t{1}, uint8_t{0});
    Processors::emplace_variant<Processors::SelectChain>(units,
        uint16_t{1}, uint8_t{1}, uint8_t{3}, true);
    Processors::emplace_variant<Processors::SelectCard>(units,
        uint16_t{1}, uint8_t{0}, true, uint8_t{1}, uint8_t{2});
    Processors::emplace_variant<Processors::SelectCardCodes>(units,
        uint16_t{1}, uint8_t{0}, false, uint8_t{1}, uint8_t{1});
    Processors::emplace_variant<Processors::SelectUnselectCard>(units,
        uint16_t{1}, uint8_t{1}, true, uint8_t{0}, uint8_t{3}, false);
    Processors::emplace_variant<Processors::SelectPosition>(units,
        uint16_t{1}, uint8_t{0}, uint32_t{46986414}, uint8_t{0x05});
    Processors::emplace_variant<Processors::SelectTributeP>(units,
        uint16_t{1}, uint8_t{0}, true, uint8_t{1}, uint8_t{2});
    Processors::emplace_variant<Processors::SelectCounter>(units,
        uint16_t{1}, uint8_t{0}, uint16_t{0xC0DE}, uint16_t{3},
        uint8_t{1}, uint8_t{0});
    Processors::emplace_variant<Processors::SelectSum>(units,
        uint16_t{1}, uint8_t{0}, int32_t{1500}, int32_t{1}, int32_t{5});
    Processors::emplace_variant<Processors::SortCard>(units,
        uint16_t{1}, uint8_t{0}, true);
    Processors::emplace_variant<Processors::SelectYesNo>(units,
        uint16_t{1}, uint8_t{0}, uint64_t{0xCAFEBABE});
    Processors::emplace_variant<Processors::SelectEffectYesNo>(units,
        uint16_t{1}, uint8_t{0}, uint64_t{0xDEADBEEF}, pcard);
    Processors::emplace_variant<Processors::SelectOption>(units,
        uint16_t{1}, uint8_t{0});
    Processors::emplace_variant<Processors::AnnounceRace>(units,
        uint16_t{1}, uint8_t{0}, uint8_t{2}, uint64_t{0xFFEE});
    Processors::emplace_variant<Processors::AnnounceAttribute>(units,
        uint16_t{1}, uint8_t{0}, uint8_t{1}, uint32_t{0x40});
    Processors::emplace_variant<Processors::AnnounceCard>(units,
        uint16_t{1}, uint8_t{0});
    Processors::emplace_variant<Processors::AnnounceNumber>(units,
        uint16_t{1}, uint8_t{0});
    Processors::emplace_variant<Processors::RockPaperScissors>(units,
        uint16_t{1}, true);

    const size_t expected_units = units.size();

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save with all Tier 3 units");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load");

    auto* d_loaded = static_cast<duel*>(loaded);
    auto& units_loaded = d_loaded->game_field->core.units;
    CHECK_EQ(units_loaded.size(), expected_units, "unit count round-trips");

    auto it = units_loaded.begin();
    auto pop = [&]() { processor_unit& u = *it; ++it; return &u; };

    if (auto* p = Processors::get_opt_variant<Processors::SelectBattleCmd>(*pop())) {
        CHECK_EQ(p->step, 1, "SelectBattleCmd.step");
        CHECK_EQ(p->playerid, 0, "SelectBattleCmd.playerid");
    } else { CHECK_TRUE(false, "SelectBattleCmd"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectChain>(*pop())) {
        CHECK_EQ(p->step, 1, "SelectChain.step");
        CHECK_EQ(p->playerid, 1, "SelectChain.playerid");
        CHECK_EQ(p->spe_count, 3, "SelectChain.spe_count");
        CHECK_TRUE(p->forced, "SelectChain.forced");
    } else { CHECK_TRUE(false, "SelectChain"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectCard>(*pop())) {
        CHECK_EQ(p->step, 1, "SelectCard.step");
        CHECK_TRUE(p->cancelable, "SelectCard.cancelable");
        CHECK_EQ(p->min, 1, "SelectCard.min");
        CHECK_EQ(p->max, 2, "SelectCard.max");
    } else { CHECK_TRUE(false, "SelectCard"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectCardCodes>(*pop())) {
        CHECK_EQ(p->step, 1, "SelectCardCodes.step");
        CHECK_TRUE(!p->cancelable, "SelectCardCodes.cancelable");
        CHECK_EQ(p->max, 1, "SelectCardCodes.max");
    } else { CHECK_TRUE(false, "SelectCardCodes"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectUnselectCard>(*pop())) {
        CHECK_EQ(p->playerid, 1, "SelectUnselectCard.playerid");
        CHECK_TRUE(p->cancelable, "SelectUnselectCard.cancelable");
        CHECK_EQ(p->max, 3, "SelectUnselectCard.max");
        CHECK_TRUE(!p->finishable, "SelectUnselectCard.finishable");
    } else { CHECK_TRUE(false, "SelectUnselectCard"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectPosition>(*pop())) {
        CHECK_EQ(p->code, 46986414u, "SelectPosition.code");
        CHECK_EQ(p->positions, 0x05u, "SelectPosition.positions");
    } else { CHECK_TRUE(false, "SelectPosition"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectTributeP>(*pop())) {
        CHECK_TRUE(p->cancelable, "SelectTributeP.cancelable");
        CHECK_EQ(p->max, 2, "SelectTributeP.max");
    } else { CHECK_TRUE(false, "SelectTributeP"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectCounter>(*pop())) {
        CHECK_EQ(p->countertype, 0xC0DEu, "SelectCounter.countertype");
        CHECK_EQ(p->count, 3u,            "SelectCounter.count");
        CHECK_EQ(p->self, 1u,             "SelectCounter.self");
        CHECK_EQ(p->oppo, 0u,             "SelectCounter.oppo");
    } else { CHECK_TRUE(false, "SelectCounter"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectSum>(*pop())) {
        CHECK_EQ(p->acc, 1500, "SelectSum.acc");
        CHECK_EQ(p->min, 1,    "SelectSum.min");
        CHECK_EQ(p->max, 5,    "SelectSum.max");
    } else { CHECK_TRUE(false, "SelectSum"); }

    if (auto* p = Processors::get_opt_variant<Processors::SortCard>(*pop())) {
        CHECK_TRUE(p->is_chain, "SortCard.is_chain");
    } else { CHECK_TRUE(false, "SortCard"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectYesNo>(*pop())) {
        CHECK_EQ(p->description, 0xCAFEBABEull, "SelectYesNo.description");
    } else { CHECK_TRUE(false, "SelectYesNo"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectEffectYesNo>(*pop())) {
        CHECK_EQ(p->description, 0xDEADBEEFull, "SelectEffectYesNo.description");
        CHECK_TRUE(p->pcard != nullptr,
                   "SelectEffectYesNo.pcard non-null after load");
        CHECK_EQ(p->pcard->cardid, pcard_id,
                 "SelectEffectYesNo.pcard cardid round-trips");
    } else { CHECK_TRUE(false, "SelectEffectYesNo"); }

    if (auto* p = Processors::get_opt_variant<Processors::SelectOption>(*pop())) {
        CHECK_EQ(p->step, 1, "SelectOption.step");
    } else { CHECK_TRUE(false, "SelectOption"); }

    if (auto* p = Processors::get_opt_variant<Processors::AnnounceRace>(*pop())) {
        CHECK_EQ(p->count, 2u,             "AnnounceRace.count");
        CHECK_EQ(p->available, 0xFFEEull,  "AnnounceRace.available");
    } else { CHECK_TRUE(false, "AnnounceRace"); }

    if (auto* p = Processors::get_opt_variant<Processors::AnnounceAttribute>(*pop())) {
        CHECK_EQ(p->count, 1u,           "AnnounceAttribute.count");
        CHECK_EQ(p->available, 0x40u,    "AnnounceAttribute.available");
    } else { CHECK_TRUE(false, "AnnounceAttribute"); }

    if (auto* p = Processors::get_opt_variant<Processors::AnnounceCard>(*pop())) {
        CHECK_EQ(p->step, 1, "AnnounceCard.step");
    } else { CHECK_TRUE(false, "AnnounceCard"); }

    if (auto* p = Processors::get_opt_variant<Processors::AnnounceNumber>(*pop())) {
        CHECK_EQ(p->step, 1, "AnnounceNumber.step");
    } else { CHECK_TRUE(false, "AnnounceNumber"); }

    if (auto* p = Processors::get_opt_variant<Processors::RockPaperScissors>(*pop())) {
        CHECK_TRUE(p->repeat, "RPS.repeat");
    } else { CHECK_TRUE(false, "RockPaperScissors"); }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Tier 5 (chunk 9d) — synthetic round-trip for SpellSet / SummonRule /
// MonsterSet clusters (10 variants).  Mirrors test_chunk9b_tier3_synthetic_round_trip.
// ---------------------------------------------------------------------------

bool test_chunk9d_tier5_synthetic_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0x9D05);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    auto* d_orig = static_cast<duel*>(orig);
    auto& units = d_orig->game_field->core.units;
    units.clear();

    // Pick two cards for pointer fields — one for target, one for tribute.
    card* pcard0 = d_orig->game_field->player[0].list_main[0];
    card* pcard1 = d_orig->game_field->player[0].list_main[1];
    const uint32_t pcard0_id = pcard0->cardid;
    const uint32_t pcard1_id = pcard1->cardid;

    // SpellSet: step=2, setplayer=0, toplayer=0, target=pcard0, reff=nullptr.
    Processors::emplace_variant<Processors::SpellSet>(
        units, uint16_t{2}, uint8_t{0}, uint8_t{0}, pcard0, nullptr);

    // SpellSetGroup: step=1, setplayer=0, toplayer=1, ptarget=nullptr,
    //               confirm=true, reff=nullptr.  set_cards populated post-emplace.
    Processors::emplace_variant<Processors::SpellSetGroup>(
        units, uint16_t{1}, uint8_t{0}, uint8_t{1},
        static_cast<group*>(nullptr), true, nullptr);
    if (auto* p = Processors::get_opt_variant<Processors::SpellSetGroup>(
            units.back())) {
        p->set_cards.insert(pcard0);
        p->set_cards.insert(pcard1);
    }

    // SummonRule: step=3, sumplayer=0, target=pcard0, proc=nullptr,
    //            ignore_count=false, min_tribute=1, zone=0x1F.
    Processors::emplace_variant<Processors::SummonRule>(
        units, uint16_t{3}, uint8_t{0}, pcard0, nullptr,
        false, uint8_t{1}, uint32_t{0x1F});
    if (auto* p = Processors::get_opt_variant<Processors::SummonRule>(
            units.back())) {
        p->max_allowed_tributes = 2;
        p->tributes.insert(pcard1);
    }

    // SpSummonRule: step=1, sumplayer=1, target=pcard0, summon_type=0x20,
    //              is_mid_chain=true, proc=nullptr.
    Processors::emplace_variant<Processors::SpSummonRule>(
        units, uint16_t{1}, uint8_t{1}, pcard0, uint32_t{0x20},
        true, nullptr);

    // SpSummonRuleGroup: step=2, sumplayer=0, summon_type=0x10.
    Processors::emplace_variant<Processors::SpSummonRuleGroup>(
        units, uint16_t{2}, uint8_t{0}, uint32_t{0x10});

    // MonsterSet: step=4, setplayer=0, target=pcard1, proc=nullptr,
    //            ignore_count=true, min_tribute=0, zone=0x1F.
    Processors::emplace_variant<Processors::MonsterSet>(
        units, uint16_t{4}, uint8_t{0}, pcard1, nullptr,
        true, uint8_t{0}, uint32_t{0x1F});
    if (auto* p = Processors::get_opt_variant<Processors::MonsterSet>(
            units.back())) {
        p->max_allowed_tributes = 1;
    }

    // FlipSummon: step=1, sumplayer=1, target=pcard0.
    Processors::emplace_variant<Processors::FlipSummon>(
        units, uint16_t{1}, uint8_t{1}, pcard0);

    // SpSummon: step=2, reason_effect=nullptr, reason_player=0,
    //           targets=nullptr, zone=0xFF.
    Processors::emplace_variant<Processors::SpSummon>(
        units, uint16_t{2}, nullptr, uint8_t{0},
        static_cast<group*>(nullptr), uint32_t{0xFF});

    // SpSummonStep: step=1, targets=nullptr, target=pcard1, zone=0x01.
    Processors::emplace_variant<Processors::SpSummonStep>(
        units, uint16_t{1},
        static_cast<group*>(nullptr), pcard1, uint32_t{0x01});

    // ChangePos: step=3, targets=nullptr, reason_effect=nullptr,
    //            reason_player=1, enable=true.
    Processors::emplace_variant<Processors::ChangePos>(
        units, uint16_t{3},
        static_cast<group*>(nullptr), nullptr,
        uint8_t{1}, true);
    if (auto* p = Processors::get_opt_variant<Processors::ChangePos>(
            units.back())) {
        p->oppo_selection = true;
        p->to_grave_set.insert(pcard0);
    }

    const size_t expected_units = units.size();

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save with all Tier 5 units");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load Tier 5 blob");

    auto* d_loaded = static_cast<duel*>(loaded);
    auto& ul = d_loaded->game_field->core.units;
    CHECK_EQ(ul.size(), expected_units, "unit count round-trips");

    auto it = ul.begin();
    auto pop = [&]() -> processor_unit& { processor_unit& u = *it; ++it; return u; };

    if (auto* p = Processors::get_opt_variant<Processors::SpellSet>(pop())) {
        CHECK_EQ(p->step, 2,          "SpellSet.step");
        CHECK_EQ(p->setplayer, 0,     "SpellSet.setplayer");
        CHECK_EQ(p->toplayer, 0,      "SpellSet.toplayer");
        CHECK_TRUE(p->target != nullptr, "SpellSet.target non-null");
        CHECK_EQ(p->target->cardid, pcard0_id, "SpellSet.target cardid");
    } else { CHECK_TRUE(false, "SpellSet variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SpellSetGroup>(pop())) {
        CHECK_EQ(p->step, 1,          "SpellSetGroup.step");
        CHECK_EQ(p->toplayer, 1,      "SpellSetGroup.toplayer");
        CHECK_TRUE(p->confirm,        "SpellSetGroup.confirm");
        CHECK_EQ(p->set_cards.size(), size_t{2}, "SpellSetGroup.set_cards size");
    } else { CHECK_TRUE(false, "SpellSetGroup variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SummonRule>(pop())) {
        CHECK_EQ(p->step, 3,          "SummonRule.step");
        CHECK_EQ(p->sumplayer, 0,     "SummonRule.sumplayer");
        CHECK_EQ(p->min_tribute, 1,   "SummonRule.min_tribute");
        CHECK_EQ(p->max_allowed_tributes, 2, "SummonRule.max_allowed_tributes");
        CHECK_TRUE(!p->ignore_count,  "SummonRule.ignore_count");
        CHECK_EQ(p->zone, 0x1Fu,      "SummonRule.zone");
        CHECK_TRUE(p->target != nullptr, "SummonRule.target non-null");
        CHECK_EQ(p->tributes.size(), size_t{1}, "SummonRule.tributes size");
    } else { CHECK_TRUE(false, "SummonRule variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SpSummonRule>(pop())) {
        CHECK_EQ(p->step, 1,          "SpSummonRule.step");
        CHECK_EQ(p->sumplayer, 1,     "SpSummonRule.sumplayer");
        CHECK_TRUE(p->is_mid_chain,   "SpSummonRule.is_mid_chain");
        CHECK_EQ(p->summon_type, 0x20u, "SpSummonRule.summon_type");
        CHECK_TRUE(p->target != nullptr, "SpSummonRule.target non-null");
    } else { CHECK_TRUE(false, "SpSummonRule variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SpSummonRuleGroup>(pop())) {
        CHECK_EQ(p->step, 2,          "SpSummonRuleGroup.step");
        CHECK_EQ(p->sumplayer, 0,     "SpSummonRuleGroup.sumplayer");
        CHECK_EQ(p->summon_type, 0x10u, "SpSummonRuleGroup.summon_type");
    } else { CHECK_TRUE(false, "SpSummonRuleGroup variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::MonsterSet>(pop())) {
        CHECK_EQ(p->step, 4,          "MonsterSet.step");
        CHECK_EQ(p->setplayer, 0,     "MonsterSet.setplayer");
        CHECK_EQ(p->min_tribute, 0,   "MonsterSet.min_tribute");
        CHECK_EQ(p->max_allowed_tributes, 1, "MonsterSet.max_allowed_tributes");
        CHECK_TRUE(p->ignore_count,   "MonsterSet.ignore_count");
        CHECK_TRUE(p->target != nullptr, "MonsterSet.target non-null");
        CHECK_EQ(p->target->cardid, pcard1_id, "MonsterSet.target cardid");
    } else { CHECK_TRUE(false, "MonsterSet variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::FlipSummon>(pop())) {
        CHECK_EQ(p->step, 1,          "FlipSummon.step");
        CHECK_EQ(p->sumplayer, 1,     "FlipSummon.sumplayer");
        CHECK_TRUE(p->target != nullptr, "FlipSummon.target non-null");
        CHECK_EQ(p->target->cardid, pcard0_id, "FlipSummon.target cardid");
    } else { CHECK_TRUE(false, "FlipSummon variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SpSummon>(pop())) {
        CHECK_EQ(p->step, 2,          "SpSummon.step");
        CHECK_EQ(p->reason_player, 0, "SpSummon.reason_player");
        CHECK_EQ(p->zone, 0xFFu,      "SpSummon.zone");
    } else { CHECK_TRUE(false, "SpSummon variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::SpSummonStep>(pop())) {
        CHECK_EQ(p->step, 1,          "SpSummonStep.step");
        CHECK_EQ(p->zone, 0x01u,      "SpSummonStep.zone");
        CHECK_TRUE(p->target != nullptr, "SpSummonStep.target non-null");
        CHECK_EQ(p->target->cardid, pcard1_id, "SpSummonStep.target cardid");
    } else { CHECK_TRUE(false, "SpSummonStep variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::ChangePos>(pop())) {
        CHECK_EQ(p->step, 3,          "ChangePos.step");
        CHECK_EQ(p->reason_player, 1, "ChangePos.reason_player");
        CHECK_TRUE(p->enable,         "ChangePos.enable");
        CHECK_TRUE(p->oppo_selection, "ChangePos.oppo_selection");
        CHECK_EQ(p->to_grave_set.size(), size_t{1}, "ChangePos.to_grave_set size");
    } else { CHECK_TRUE(false, "ChangePos variant"); }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
    return true;
}

// ---------------------------------------------------------------------------
// Tier 6 (chunk 9e) — synthetic round-trip for Draw / Damage / DamageStep /
// Equip / PayLPCost / RemoveCounter / TossCoin / TossDice / Recover (9 types).
// Mirrors test_chunk9d_tier5_synthetic_round_trip.
// ---------------------------------------------------------------------------

bool test_chunk9e_tier6_synthetic_round_trip() {
    OCG_Duel orig = make_chunk5a_duel(0x9E06);
    CHECK_TRUE(orig != nullptr, "create orig");
    populate_simple_deck(orig);

    auto* d_orig = static_cast<duel*>(orig);
    auto& units = d_orig->game_field->core.units;
    units.clear();

    card* pcard0 = d_orig->game_field->player[0].list_main[0];
    card* pcard1 = d_orig->game_field->player[0].list_main[1];
    const uint32_t pcard0_id = pcard0->cardid;
    const uint32_t pcard1_id = pcard1->cardid;

    // Draw: step=1, count=2, reason_player=0, playerid=0, reason=0x01,
    //       reff=nullptr, drawn_set={pcard0}.
    Processors::emplace_variant<Processors::Draw>(
        units, uint16_t{1}, nullptr, uint32_t{0x01},
        uint8_t{0}, uint8_t{0}, uint16_t{2});
    if (auto* p = Processors::get_opt_variant<Processors::Draw>(
            units.back())) {
        p->drawn_set.insert(pcard0);
    }

    // Damage: step=2, reff=nullptr, reason=0x02, reason_player=1,
    //         reason_card=pcard1, playerid=0, amount=800, is_step=true.
    Processors::emplace_variant<Processors::Damage>(
        units, uint16_t{2}, nullptr, uint32_t{0x02},
        uint8_t{1}, pcard1, uint8_t{0}, uint32_t{800}, true);
    if (auto* p = Processors::get_opt_variant<Processors::Damage>(
            units.back())) {
        p->is_reflected = true;
    }

    // Recover: step=3, reff=nullptr, reason=0x04, reason_player=0,
    //          playerid=1, amount=500, is_step=false.
    Processors::emplace_variant<Processors::Recover>(
        units, uint16_t{3}, nullptr, uint32_t{0x04},
        uint8_t{0}, uint8_t{1}, uint32_t{500}, false);

    // DamageStep: step=4, attacker=pcard0, attack_target=pcard1, new_attack=true.
    Processors::emplace_variant<Processors::DamageStep>(
        units, uint16_t{4}, pcard0, pcard1, true);
    if (auto* p = Processors::get_opt_variant<Processors::DamageStep>(
            units.back())) {
        p->backup_phase = 0x04;
    }

    // Equip: step=5, equip_player=0, equip_card=pcard0, target=pcard1,
    //        faceup=true, is_step=false.
    Processors::emplace_variant<Processors::Equip>(
        units, uint16_t{5}, uint8_t{0}, pcard0, pcard1, true, false);

    // PayLPCost: step=6, playerid=1, cost=1000.
    Processors::emplace_variant<Processors::PayLPCost>(
        units, uint16_t{6}, uint8_t{1}, uint32_t{1000});

    // RemoveCounter: step=7, reason=0x08, pcard=pcard0, rplayer=0,
    //               self=1, oppo=0, countertype=0x11, count=2.
    Processors::emplace_variant<Processors::RemoveCounter>(
        units, uint16_t{7}, uint32_t{0x08}, pcard0,
        uint8_t{0}, uint8_t{1}, uint8_t{0},
        uint16_t{0x11}, uint16_t{2});

    // TossCoin: step=8, reff=nullptr, reason_player=0, playerid=0, count=3.
    Processors::emplace_variant<Processors::TossCoin>(
        units, uint16_t{8}, nullptr, uint8_t{0}, uint8_t{0}, uint8_t{3});

    // TossDice: step=9, reff=nullptr, reason_player=1, playerid=0,
    //           count1=2, count2=1.
    Processors::emplace_variant<Processors::TossDice>(
        units, uint16_t{9}, nullptr, uint8_t{1}, uint8_t{0},
        uint8_t{2}, uint8_t{1});

    const size_t expected_units = units.size();

    void* blob = nullptr;
    uint32_t size = 0;
    CHECK_EQ(OCG_DuelSaveState(orig, &blob, &size), OCG_SAVE_OK,
             "save with all Tier 6 units");

    OCG_DuelOptions opts = make_chunk5a_load_options();
    OCG_Duel loaded = nullptr;
    CHECK_EQ(OCG_DuelLoadState(blob, size, &opts, &loaded), OCG_LOAD_OK,
             "load Tier 6 blob");

    auto* d_loaded = static_cast<duel*>(loaded);
    auto& ul = d_loaded->game_field->core.units;
    CHECK_EQ(ul.size(), expected_units, "unit count round-trips");

    auto it = ul.begin();
    auto pop = [&]() -> processor_unit& { processor_unit& u = *it; ++it; return u; };

    if (auto* p = Processors::get_opt_variant<Processors::Draw>(pop())) {
        CHECK_EQ(p->step, 1,            "Draw.step");
        CHECK_EQ(p->count, 2,           "Draw.count");
        CHECK_EQ(p->reason_player, 0,   "Draw.reason_player");
        CHECK_EQ(p->playerid, 0,        "Draw.playerid");
        CHECK_EQ(p->reason, 0x01u,      "Draw.reason");
        CHECK_EQ(p->drawn_set.size(), size_t{1}, "Draw.drawn_set size");
    } else { CHECK_TRUE(false, "Draw variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::Damage>(pop())) {
        CHECK_EQ(p->step, 2,            "Damage.step");
        CHECK_EQ(p->reason_player, 1,   "Damage.reason_player");
        CHECK_EQ(p->playerid, 0,        "Damage.playerid");
        CHECK_TRUE(p->is_step,          "Damage.is_step");
        CHECK_TRUE(p->is_reflected,     "Damage.is_reflected");
        CHECK_EQ(p->amount, 800u,       "Damage.amount");
        CHECK_EQ(p->reason, 0x02u,      "Damage.reason");
        CHECK_TRUE(p->reason_card != nullptr, "Damage.reason_card non-null");
        CHECK_EQ(p->reason_card->cardid, pcard1_id, "Damage.reason_card cardid");
    } else { CHECK_TRUE(false, "Damage variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::Recover>(pop())) {
        CHECK_EQ(p->step, 3,            "Recover.step");
        CHECK_EQ(p->reason_player, 0,   "Recover.reason_player");
        CHECK_EQ(p->playerid, 1,        "Recover.playerid");
        CHECK_TRUE(!p->is_step,         "Recover.is_step");
        CHECK_EQ(p->amount, 500u,       "Recover.amount");
        CHECK_EQ(p->reason, 0x04u,      "Recover.reason");
    } else { CHECK_TRUE(false, "Recover variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::DamageStep>(pop())) {
        CHECK_EQ(p->step, 4,            "DamageStep.step");
        CHECK_EQ(p->backup_phase, 4,    "DamageStep.backup_phase");
        CHECK_TRUE(p->new_attack,       "DamageStep.new_attack");
        CHECK_TRUE(p->attacker != nullptr, "DamageStep.attacker non-null");
        CHECK_EQ(p->attacker->cardid, pcard0_id, "DamageStep.attacker cardid");
        CHECK_TRUE(p->attack_target != nullptr, "DamageStep.attack_target non-null");
        CHECK_EQ(p->attack_target->cardid, pcard1_id, "DamageStep.attack_target cardid");
    } else { CHECK_TRUE(false, "DamageStep variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::Equip>(pop())) {
        CHECK_EQ(p->step, 5,            "Equip.step");
        CHECK_EQ(p->equip_player, 0,    "Equip.equip_player");
        CHECK_TRUE(!p->is_step,         "Equip.is_step");
        CHECK_TRUE(p->faceup,           "Equip.faceup");
        CHECK_TRUE(p->equip_card != nullptr, "Equip.equip_card non-null");
        CHECK_EQ(p->equip_card->cardid, pcard0_id, "Equip.equip_card cardid");
        CHECK_TRUE(p->target != nullptr, "Equip.target non-null");
        CHECK_EQ(p->target->cardid, pcard1_id, "Equip.target cardid");
    } else { CHECK_TRUE(false, "Equip variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::PayLPCost>(pop())) {
        CHECK_EQ(p->step, 6,            "PayLPCost.step");
        CHECK_EQ(p->playerid, 1,        "PayLPCost.playerid");
        CHECK_EQ(p->cost, 1000u,        "PayLPCost.cost");
    } else { CHECK_TRUE(false, "PayLPCost variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::RemoveCounter>(pop())) {
        CHECK_EQ(p->step, 7,            "RemoveCounter.step");
        CHECK_EQ(p->rplayer, 0,         "RemoveCounter.rplayer");
        CHECK_EQ(p->self, 1,            "RemoveCounter.self");
        CHECK_EQ(p->oppo, 0,            "RemoveCounter.oppo");
        CHECK_EQ(p->countertype, 0x11,  "RemoveCounter.countertype");
        CHECK_EQ(p->count, 2,           "RemoveCounter.count");
        CHECK_EQ(p->reason, 0x08u,      "RemoveCounter.reason");
        CHECK_TRUE(p->pcard != nullptr, "RemoveCounter.pcard non-null");
        CHECK_EQ(p->pcard->cardid, pcard0_id, "RemoveCounter.pcard cardid");
    } else { CHECK_TRUE(false, "RemoveCounter variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::TossCoin>(pop())) {
        CHECK_EQ(p->step, 8,            "TossCoin.step");
        CHECK_EQ(p->playerid, 0,        "TossCoin.playerid");
        CHECK_EQ(p->reason_player, 0,   "TossCoin.reason_player");
        CHECK_EQ(p->count, 3,           "TossCoin.count");
    } else { CHECK_TRUE(false, "TossCoin variant"); }

    if (auto* p = Processors::get_opt_variant<Processors::TossDice>(pop())) {
        CHECK_EQ(p->step, 9,            "TossDice.step");
        CHECK_EQ(p->playerid, 0,        "TossDice.playerid");
        CHECK_EQ(p->reason_player, 1,   "TossDice.reason_player");
        CHECK_EQ(p->count1, 2,          "TossDice.count1");
        CHECK_EQ(p->count2, 1,          "TossDice.count2");
    } else { CHECK_TRUE(false, "TossDice variant"); }

    OCG_FreeSaveBuffer(blob);
    OCG_DestroyDuel(orig);
    OCG_DestroyDuel(loaded);
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
        // Chunk 9a Tier 1 — ProcessorState round-trip at engine boundary
        {"chunk9a_processor_state_round_trip",
         &test_chunk9a_processor_state_round_trip},
        // Chunk 9a Tier 2 — subunit round-trips
        {"chunk9a_tier2_self_destroy_round_trip",
         &test_chunk9a_tier2_self_destroy_round_trip},
        {"chunk9a_tier2_self_to_grave_round_trip",
         &test_chunk9a_tier2_self_to_grave_round_trip},
        // Chunk 9b §8.7 Type-C regression fixtures (partial matrix)
        {"chunk9b_forbidden_dark_contract_round_trip",
         &test_chunk9b_forbidden_dark_contract_round_trip},
        {"chunk9b_darklord_eveningstar_round_trip",
         &test_chunk9b_darklord_eveningstar_round_trip},
        // Chunk 5b Wave 1 — card_set fields for effect-targeting
        {"chunk5b_card_set_round_trip", &test_chunk5b_card_set_round_trip},
        // Chunk 5b Wave 3 — Type-C fixtures (byte-equal round-trip)
        {"chunk5b_dueltaining_round_trip", &test_chunk5b_dueltaining_round_trip},
        {"chunk5b_branded_round_trip",     &test_chunk5b_branded_round_trip},
        {"chunk5b_hydor_round_trip",       &test_chunk5b_hydor_round_trip},
        // Chunk 5b Wave 3 — wrapper-sequencing (5c: now round-trips,
        // not refuse, since recursive function-upvalue dump is in place)
        {"chunk5b_vendread_wrapper_refuse",
         &test_chunk5b_vendread_wrapper_refuse},
        // Chunk 5c — extended classifier (function/table upvalues)
        {"chunk5c_empty_table_round_trip",
         &test_chunk5c_empty_table_round_trip},
        {"chunk5c_function_table_round_trip",
         &test_chunk5c_function_table_round_trip},
        {"chunk5c_cross_card_shared_round_trip",
         &test_chunk5c_cross_card_shared_round_trip},
        // Chunk 5b Wave 3 — MSG-stream verification (callbacks fire post-load)
        {"chunk5b_dueltaining_msg_stream", &test_chunk5b_dueltaining_msg_stream},
        {"chunk5b_branded_msg_stream",     &test_chunk5b_branded_msg_stream},
        {"chunk5b_hydor_msg_stream",       &test_chunk5b_hydor_msg_stream},
        // Chunk 9b — smoke-test abort regression suite (schema v2,
        // field.processor scratch, Tier 3 ProcessorUnit variants).
        {"chunk9b_save_load_step_no_msg_retry",
         &test_chunk9b_save_load_step_no_msg_retry},
        {"chunk9b_processor_scratch_semantic_round_trip",
         &test_chunk9b_processor_scratch_semantic_round_trip},
        {"chunk9b_synthetic_scratch_round_trip",
         &test_chunk9b_synthetic_scratch_round_trip},
        {"chunk9b_select_chains_freed_effect_dropped",
         &test_chunk9b_select_chains_freed_effect_dropped},
        {"chunk9b_v1_blob_rejected_loud",
         &test_chunk9b_v1_blob_rejected_loud},
        {"chunk9b_tier3_synthetic_round_trip",
         &test_chunk9b_tier3_synthetic_round_trip},
        // Chunk 9d Tier 5 — SpellSet / SummonRule / MonsterSet clusters
        {"chunk9d_tier5_synthetic_round_trip",
         &test_chunk9d_tier5_synthetic_round_trip},
        // Chunk 9e Tier 6 — Draw / Damage / DamageStep / Equip cluster
        {"chunk9e_tier6_synthetic_round_trip",
         &test_chunk9e_tier6_synthetic_round_trip},
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
