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

// Internal headers — required only for the NOT_MSG_BOUNDARY synthesis
// test below, which pokes interpreter::call_depth directly to simulate
// "save called inside a Lua call" without needing to actually run a
// Process loop. Static-lib linkage makes the symbols visible.
#include "duel.h"
#include "interpreter.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
        {"byte_equal_intra_path", &test_byte_equal_intra_path},
        {"blob_parses_with_expected_shape", &test_blob_parses_with_expected_shape},
        {"refuse_null_pointer_args", &test_refuse_null_pointer_args},
        {"distinct_seeds_yield_distinct_blobs", &test_distinct_seeds_yield_distinct_blobs},
        {"refuse_not_msg_boundary", &test_refuse_not_msg_boundary},
        {"free_null_is_safe", &test_free_null_is_safe},
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
