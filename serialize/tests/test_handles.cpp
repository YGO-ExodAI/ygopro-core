// test_handles.cpp — §8.2 of phase_p1_primitive_1_plan.md
//
// Handle assign / resolve over synthetic types with deliberate pointer
// cycles. Validates HandleTable's invariants:
//   - HANDLE_NULL (0) is reserved.
//   - assign(nullptr) → 0; resolve(0) → nullptr.
//   - Real objects get monotonic handles starting at 1.
//   - Same pointer assigned twice returns same handle (caching).
//   - Pointer cycles round-trip through serialize → reload without loss.
//
// Synthetic types stand in for `card`/`effect`/`group` so this test has
// zero dependency on the engine — keeps the chunk-2 boundary clean and
// makes failures easier to attribute.

#include "handle_table.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace ocgs = ocg::serialize;

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

// Synthetic stand-in for `card` — pointer-rich enough to exercise cycles.
struct FakeCard {
    int code = 0;
    FakeCard* equip_target = nullptr;       // → another FakeCard (or nullptr)
    std::vector<FakeCard*> equipped_by;     // back-references
    struct FakeEffect* trigger = nullptr;   // → effect
};

struct FakeEffect {
    int eff_code = 0;
    FakeCard* owner = nullptr;              // → card (cycle: card.trigger →
                                            //   effect.owner → card)
    struct FakeGroup* targets = nullptr;
};

struct FakeGroup {
    std::vector<FakeCard*> members;
};

// ---------------------------------------------------------------------------
// HandleTable invariants — assign side
// ---------------------------------------------------------------------------

bool test_null_sentinel() {
    ocgs::HandleTable<FakeCard> t;
    CHECK_EQ(t.assign(nullptr), ocgs::HANDLE_NULL, "assign(nullptr)");
    CHECK_EQ(t.size(), 0u, "size after null assign");
    CHECK_TRUE(t.lookup(ocgs::HANDLE_NULL) == nullptr, "lookup(0)");
    return true;
}

bool test_monotonic_handles() {
    FakeCard a, b, c;
    ocgs::HandleTable<FakeCard> t;
    CHECK_EQ(t.assign(&a), 1u, "first handle");
    CHECK_EQ(t.assign(&b), 2u, "second handle");
    CHECK_EQ(t.assign(&c), 3u, "third handle");
    CHECK_EQ(t.size(), 3u, "size");
    return true;
}

bool test_caching_same_pointer_same_handle() {
    FakeCard a, b;
    ocgs::HandleTable<FakeCard> t;
    const auto h_a = t.assign(&a);
    const auto h_b = t.assign(&b);
    CHECK_EQ(t.assign(&a), h_a, "re-assign &a returns same handle");
    CHECK_EQ(t.assign(&b), h_b, "re-assign &b returns same handle");
    CHECK_EQ(t.assign(&a), h_a, "re-assign &a third time still same");
    CHECK_EQ(t.size(), 2u, "size unchanged by re-assigns");
    return true;
}

bool test_in_handle_order_iteration() {
    FakeCard a, b, c;
    ocgs::HandleTable<FakeCard> t;
    t.assign(&a);
    t.assign(&b);
    t.assign(&c);
    auto v = t.in_handle_order();
    CHECK_EQ(v.size(), 3u, "iteration size");
    CHECK_TRUE(v[0] == &a, "iter[0] == &a (handle 1)");
    CHECK_TRUE(v[1] == &b, "iter[1] == &b (handle 2)");
    CHECK_TRUE(v[2] == &c, "iter[2] == &c (handle 3)");
    return true;
}

bool test_clear_resets() {
    FakeCard a;
    ocgs::HandleTable<FakeCard> t;
    t.assign(&a);
    CHECK_EQ(t.size(), 1u, "before clear");
    t.clear();
    CHECK_EQ(t.size(), 0u, "after clear");
    CHECK_EQ(t.assign(&a), 1u, "post-clear handle restarts at 1");
    return true;
}

// ---------------------------------------------------------------------------
// HandleResolver invariants — deserialize side
// ---------------------------------------------------------------------------

bool test_resolver_null_and_unbound() {
    ocgs::HandleResolver<FakeCard> r;
    CHECK_TRUE(r.lookup(ocgs::HANDLE_NULL) == nullptr, "resolve(0)");
    CHECK_TRUE(r.lookup(42) == nullptr, "resolve(unbound) returns nullptr");
    CHECK_TRUE(!r.is_bound(0), "is_bound(0) false");
    CHECK_TRUE(!r.is_bound(42), "is_bound(unbound) false");
    return true;
}

bool test_resolver_register_and_resolve() {
    FakeCard a, b;
    ocgs::HandleResolver<FakeCard> r;
    r.register_handle(1, &a);
    r.register_handle(2, &b);
    CHECK_TRUE(r.lookup(1) == &a, "resolve(1) == &a");
    CHECK_TRUE(r.lookup(2) == &b, "resolve(2) == &b");
    CHECK_TRUE(r.is_bound(1), "is_bound(1)");
    CHECK_TRUE(r.is_bound(2), "is_bound(2)");
    CHECK_EQ(r.bound_count(), 2u, "bound count");
    return true;
}

bool test_resolver_sparse_handles() {
    // Handles need not be contiguous (loader may register out of order).
    FakeCard a, b;
    ocgs::HandleResolver<FakeCard> r;
    r.register_handle(7, &a);
    r.register_handle(3, &b);
    CHECK_TRUE(r.lookup(7) == &a, "sparse resolve hi");
    CHECK_TRUE(r.lookup(3) == &b, "sparse resolve lo");
    CHECK_TRUE(r.lookup(1) == nullptr, "unbound between bound");
    CHECK_TRUE(r.lookup(5) == nullptr, "unbound between bound");
    CHECK_EQ(r.bound_count(), 2u, "bound count sparse");
    return true;
}

// ---------------------------------------------------------------------------
// Cycle round-trip — the headline test for §8.2
// ---------------------------------------------------------------------------

// Drives the full save→load cycle over a synthetic graph with deliberate
// pointer cycles and verifies the reloaded graph has the same topology.
//
// Topology: card A equips card B (A.equip_target = B; B.equipped_by.push(A));
// card B's trigger is effect X; effect X's owner is card B (cycle:
// B.trigger → X.owner → B); effect X's targets is group G;
// group G contains [A, B]; card A.equipped_by = [].
bool test_cycle_roundtrip() {
    // --- Build the original graph ---
    FakeCard a; a.code = 0xAAAA;
    FakeCard b; b.code = 0xBBBB;
    FakeEffect x; x.eff_code = 0xEFEF;
    FakeGroup g;

    a.equip_target = &b;
    b.equipped_by.push_back(&a);
    b.trigger = &x;
    x.owner = &b;
    x.targets = &g;
    g.members = {&a, &b};

    // --- Serialize side: assign handles for everything reachable ---
    ocgs::HandleTable<FakeCard> save_cards;
    ocgs::HandleTable<FakeEffect> save_effects;
    ocgs::HandleTable<FakeGroup> save_groups;

    const uint32_t h_a = save_cards.assign(&a);
    const uint32_t h_b = save_cards.assign(&b);
    const uint32_t h_x = save_effects.assign(&x);
    const uint32_t h_g = save_groups.assign(&g);

    CHECK_EQ(h_a, 1u, "card a handle");
    CHECK_EQ(h_b, 2u, "card b handle");
    CHECK_EQ(h_x, 1u, "effect x handle");
    CHECK_EQ(h_g, 1u, "group g handle");

    // Build a serialized representation (just plain structs; in chunk 3
    // this is replaced by the protobuf CardRecord / EffectRecord / etc.).
    struct CardRow {
        uint32_t handle;
        int code;
        uint32_t equip_target_handle;       // card handle
        uint32_t trigger_handle;            // effect handle
        std::vector<uint32_t> equipped_by;  // card handles
    };
    struct EffectRow {
        uint32_t handle;
        int eff_code;
        uint32_t owner_handle;              // card handle
        uint32_t targets_handle;            // group handle
    };
    struct GroupRow {
        uint32_t handle;
        std::vector<uint32_t> members;      // card handles
    };

    std::vector<CardRow> card_rows;
    std::vector<EffectRow> effect_rows;
    std::vector<GroupRow> group_rows;

    for (FakeCard* c : save_cards.in_handle_order()) {
        CardRow row{};
        row.handle = save_cards.assign(c);  // cached lookup
        row.code = c->code;
        row.equip_target_handle = save_cards.assign(c->equip_target);
        row.trigger_handle = save_effects.assign(c->trigger);
        for (FakeCard* eq : c->equipped_by) {
            row.equipped_by.push_back(save_cards.assign(eq));
        }
        card_rows.push_back(row);
    }
    for (FakeEffect* e : save_effects.in_handle_order()) {
        EffectRow row{};
        row.handle = save_effects.assign(e);
        row.eff_code = e->eff_code;
        row.owner_handle = save_cards.assign(e->owner);
        row.targets_handle = save_groups.assign(e->targets);
        effect_rows.push_back(row);
    }
    for (FakeGroup* gp : save_groups.in_handle_order()) {
        GroupRow row{};
        row.handle = save_groups.assign(gp);
        for (FakeCard* m : gp->members) {
            row.members.push_back(save_cards.assign(m));
        }
        group_rows.push_back(row);
    }

    CHECK_EQ(card_rows.size(), 2u, "serialized card count");
    CHECK_EQ(effect_rows.size(), 1u, "serialized effect count");
    CHECK_EQ(group_rows.size(), 1u, "serialized group count");
    // Sanity: the cycle is captured in the rows.
    CHECK_EQ(card_rows[0].equip_target_handle, h_b, "row[a].equip_target = h_b");
    CHECK_EQ(card_rows[1].trigger_handle, h_x, "row[b].trigger = h_x");
    CHECK_EQ(effect_rows[0].owner_handle, h_b, "row[x].owner = h_b");
    CHECK_EQ(effect_rows[0].targets_handle, h_g, "row[x].targets = h_g");

    // --- Deserialize side: allocate fresh objects, register handles,
    // then fix up cross-references. Two-pass walk.
    std::vector<std::unique_ptr<FakeCard>> new_cards_storage;
    std::vector<std::unique_ptr<FakeEffect>> new_effects_storage;
    std::vector<std::unique_ptr<FakeGroup>> new_groups_storage;

    ocgs::HandleResolver<FakeCard> load_cards;
    ocgs::HandleResolver<FakeEffect> load_effects;
    ocgs::HandleResolver<FakeGroup> load_groups;

    // Pass 1: allocate empty objects, register their handles. After this
    // pass, every cross-reference can be resolved (because every target
    // exists, even if its own fields aren't filled yet).
    for (const auto& row : card_rows) {
        auto p = std::make_unique<FakeCard>();
        p->code = row.code;
        load_cards.register_handle(row.handle, p.get());
        new_cards_storage.push_back(std::move(p));
    }
    for (const auto& row : effect_rows) {
        auto p = std::make_unique<FakeEffect>();
        p->eff_code = row.eff_code;
        load_effects.register_handle(row.handle, p.get());
        new_effects_storage.push_back(std::move(p));
    }
    for (const auto& row : group_rows) {
        auto p = std::make_unique<FakeGroup>();
        load_groups.register_handle(row.handle, p.get());
        new_groups_storage.push_back(std::move(p));
    }

    // Pass 2: resolve cross-references.
    for (size_t i = 0; i < card_rows.size(); ++i) {
        const auto& row = card_rows[i];
        FakeCard* p = new_cards_storage[i].get();
        p->equip_target = load_cards.lookup(row.equip_target_handle);
        p->trigger = load_effects.lookup(row.trigger_handle);
        for (uint32_t h : row.equipped_by) {
            p->equipped_by.push_back(load_cards.lookup(h));
        }
    }
    for (size_t i = 0; i < effect_rows.size(); ++i) {
        const auto& row = effect_rows[i];
        FakeEffect* p = new_effects_storage[i].get();
        p->owner = load_cards.lookup(row.owner_handle);
        p->targets = load_groups.lookup(row.targets_handle);
    }
    for (size_t i = 0; i < group_rows.size(); ++i) {
        const auto& row = group_rows[i];
        FakeGroup* p = new_groups_storage[i].get();
        for (uint32_t h : row.members) {
            p->members.push_back(load_cards.lookup(h));
        }
    }

    // --- Verify reloaded topology mirrors the original ---
    FakeCard* na = load_cards.lookup(h_a);
    FakeCard* nb = load_cards.lookup(h_b);
    FakeEffect* nx = load_effects.lookup(h_x);
    FakeGroup* ng = load_groups.lookup(h_g);

    CHECK_TRUE(na != nullptr && nb != nullptr && nx != nullptr && ng != nullptr,
               "all reloaded ptrs non-null");
    CHECK_TRUE(na != &a, "reloaded card a is a fresh allocation");
    CHECK_TRUE(nb != &b, "reloaded card b is a fresh allocation");
    CHECK_EQ(na->code, 0xAAAA, "reloaded card a code");
    CHECK_EQ(nb->code, 0xBBBB, "reloaded card b code");
    CHECK_EQ(nx->eff_code, 0xEFEF, "reloaded effect x code");

    // The crucial part: cross-reference graph is identical.
    CHECK_TRUE(na->equip_target == nb,
               "reloaded a.equip_target points at reloaded b (cycle preserved)");
    CHECK_TRUE(nb->trigger == nx, "reloaded b.trigger points at reloaded x");
    CHECK_TRUE(nx->owner == nb, "reloaded x.owner points at reloaded b");
    CHECK_TRUE(nx->targets == ng, "reloaded x.targets points at reloaded g");
    CHECK_EQ(nb->equipped_by.size(), 1u, "reloaded b.equipped_by size");
    CHECK_TRUE(nb->equipped_by[0] == na, "reloaded b.equipped_by[0] = a");
    CHECK_EQ(ng->members.size(), 2u, "reloaded g.members size");
    CHECK_TRUE(ng->members[0] == na, "reloaded g.members[0] = a");
    CHECK_TRUE(ng->members[1] == nb, "reloaded g.members[1] = b");

    return true;
}

bool test_null_handle_round_trips() {
    // Cards with null cross-references must round-trip cleanly (nullptr
    // → handle 0 → nullptr after reload).
    FakeCard solo;
    solo.code = 42;
    solo.equip_target = nullptr;
    solo.trigger = nullptr;

    ocgs::HandleTable<FakeCard> save_cards;
    ocgs::HandleTable<FakeEffect> save_effects;
    save_cards.assign(&solo);

    const uint32_t h_target = save_cards.assign(solo.equip_target);  // null
    const uint32_t h_trigger = save_effects.assign(solo.trigger);    // null
    CHECK_EQ(h_target, ocgs::HANDLE_NULL, "null equip_target → 0");
    CHECK_EQ(h_trigger, ocgs::HANDLE_NULL, "null trigger → 0");

    ocgs::HandleResolver<FakeCard> load_cards;
    ocgs::HandleResolver<FakeEffect> load_effects;
    auto reloaded = std::make_unique<FakeCard>();
    reloaded->code = solo.code;
    load_cards.register_handle(1, reloaded.get());

    reloaded->equip_target = load_cards.lookup(h_target);
    reloaded->trigger = load_effects.lookup(h_trigger);
    CHECK_TRUE(reloaded->equip_target == nullptr,
               "resolved null equip_target stays null");
    CHECK_TRUE(reloaded->trigger == nullptr, "resolved null trigger stays null");
    return true;
}

}  // namespace

int main() {
    int failed = 0;

    struct Test {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"null_sentinel", &test_null_sentinel},
        {"monotonic_handles", &test_monotonic_handles},
        {"caching_same_pointer_same_handle", &test_caching_same_pointer_same_handle},
        {"in_handle_order_iteration", &test_in_handle_order_iteration},
        {"clear_resets", &test_clear_resets},
        {"resolver_null_and_unbound", &test_resolver_null_and_unbound},
        {"resolver_register_and_resolve", &test_resolver_register_and_resolve},
        {"resolver_sparse_handles", &test_resolver_sparse_handles},
        {"cycle_roundtrip", &test_cycle_roundtrip},
        {"null_handle_round_trips", &test_null_handle_round_trips},
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

    if (failed == 0) {
        std::printf("test_handles: ALL %zu PASSED\n",
                    sizeof(tests) / sizeof(tests[0]));
        return 0;
    } else {
        std::printf("test_handles: %d FAILED\n", failed);
        return 1;
    }
}
