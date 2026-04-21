// HandleTable<T> — assign / resolve integer handles for engine pointers.
//
// ExodAI Phase P1 Primitive 1 (chunk 2). Used to translate raw `card*`,
// `effect*`, `group*` pointers into stable integer ids on serialization,
// and back to (newly-allocated) pointers on deserialization. See
// phase_p1_primitive_1_plan.md §3.3.
//
// Invariants:
//   - Handle 0 is the null-sentinel ("no reference"). assign(nullptr) → 0.
//     resolve(0) → nullptr.
//   - Real objects receive handles 1, 2, 3, ... assigned monotonically in
//     first-seen order on the assign side.
//   - The same pointer assigned twice returns the same handle (caching).
//   - On the resolve side, register_handle establishes a (handle → ptr)
//     mapping; lookup is O(1).
//   - assign() must never produce 0 for a non-null pointer; doing so would
//     collide with the null-sentinel and corrupt cross-references. Asserted
//     in debug builds.
//
// This header is intentionally engine-free: depends only on STL. Tests
// instantiate it over synthetic types (test_handles.cpp) without needing
// anything from card.h / effect.h / etc.
//
// Usage (serialize):
//     HandleTable<card> cards;
//     cards.assign(some_card);                 // returns 1
//     cards.assign(other_card);                // returns 2
//     cards.assign(some_card);                 // returns 1 (cached)
//     cards.assign(nullptr);                   // returns 0
//     for (auto* c : cards.in_handle_order()) { /* serialize c */ }
//
// Usage (deserialize):
//     HandleResolver<card> cards;
//     cards.register_handle(1, fresh_card_a);
//     cards.register_handle(2, fresh_card_b);
//     card* p = cards.lookup(1);               // → fresh_card_a
//     card* n = cards.lookup(0);               // → nullptr

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ocg::serialize {

// Reserved handle for "no reference". Real objects start at HANDLE_FIRST.
constexpr uint32_t HANDLE_NULL = 0;
constexpr uint32_t HANDLE_FIRST = 1;

// ---------------------------------------------------------------------------
// HandleTable: serialize side. Assigns integer handles to live pointers in
// the order they're first seen. Caches by pointer identity so repeated
// assigns of the same pointer return the same handle.
// ---------------------------------------------------------------------------

template <typename T>
class HandleTable {
public:
    HandleTable() {
        // Reserve index 0 to keep handles 1-aligned with vector indices.
        by_handle_.push_back(nullptr);
    }

    // Returns HANDLE_NULL (0) for nullptr, or the cached/newly-assigned
    // handle for a real pointer. Never returns 0 for a non-null pointer.
    uint32_t assign(T* ptr) {
        if (ptr == nullptr) {
            return HANDLE_NULL;
        }
        auto it = by_ptr_.find(ptr);
        if (it != by_ptr_.end()) {
            return it->second;
        }
        const uint32_t handle = static_cast<uint32_t>(by_handle_.size());
        assert(handle != HANDLE_NULL && "handle 0 reserved as null-sentinel");
        by_handle_.push_back(ptr);
        by_ptr_.emplace(ptr, handle);
        return handle;
    }

    // True iff ptr has been assigned previously (cache hit possible).
    bool contains(T* ptr) const {
        if (ptr == nullptr) return false;
        return by_ptr_.find(ptr) != by_ptr_.end();
    }

    // Number of distinct non-null pointers assigned (excludes the
    // sentinel slot at index 0).
    size_t size() const {
        return by_handle_.size() - 1;
    }

    // Iterate assigned pointers in handle order (handle 1 first). Used at
    // serialization time to emit `repeated CardRecord cards = ...` in
    // matching order.
    std::vector<T*> in_handle_order() const {
        return std::vector<T*>(by_handle_.begin() + 1, by_handle_.end());
    }

    // Look up the pointer for a given handle on the assign side (rarely
    // needed; mostly for asserts and round-trip checks). Returns nullptr
    // for HANDLE_NULL or out-of-range handles.
    T* lookup(uint32_t handle) const {
        if (handle == HANDLE_NULL || handle >= by_handle_.size()) {
            return nullptr;
        }
        return by_handle_[handle];
    }

    void clear() {
        by_handle_.clear();
        by_handle_.push_back(nullptr);
        by_ptr_.clear();
    }

private:
    // Index 0 is always nullptr (the null-sentinel). Real objects at 1..N.
    std::vector<T*> by_handle_;
    std::unordered_map<T*, uint32_t> by_ptr_;
};

// ---------------------------------------------------------------------------
// HandleResolver: deserialize side. Holds a (handle → newly-allocated ptr)
// table built up as the loader instantiates objects, then resolves
// cross-references when fixing up pointer fields.
// ---------------------------------------------------------------------------

template <typename T>
class HandleResolver {
public:
    HandleResolver() {
        // Reserve index 0 to keep handles 1-aligned with vector indices.
        by_handle_.push_back(nullptr);
    }

    // Bind a handle to a freshly-allocated pointer. Handle must be > 0.
    // Calling with a handle that's already bound is a programming error
    // (asserted in debug); the loader should produce each handle once.
    void register_handle(uint32_t handle, T* ptr) {
        assert(handle != HANDLE_NULL && "handle 0 is reserved");
        assert(ptr != nullptr && "register_handle expects a real pointer");
        if (handle >= by_handle_.size()) {
            by_handle_.resize(handle + 1, nullptr);
        }
        assert(by_handle_[handle] == nullptr && "handle already bound");
        by_handle_[handle] = ptr;
    }

    // Resolve handle → pointer. Returns nullptr for HANDLE_NULL. Returns
    // nullptr for unknown handles too — loader callers should treat that
    // as a malformed-input error and surface OCG_LOAD_ERR_MALFORMED.
    T* lookup(uint32_t handle) const {
        if (handle == HANDLE_NULL || handle >= by_handle_.size()) {
            return nullptr;
        }
        return by_handle_[handle];
    }

    // True iff handle has been bound (i.e., lookup will return non-null
    // for a non-zero input). Useful to distinguish "null sentinel" from
    // "malformed input" in error paths.
    bool is_bound(uint32_t handle) const {
        if (handle == HANDLE_NULL || handle >= by_handle_.size()) {
            return false;
        }
        return by_handle_[handle] != nullptr;
    }

    size_t bound_count() const {
        size_t n = 0;
        for (size_t i = 1; i < by_handle_.size(); ++i) {
            if (by_handle_[i] != nullptr) ++n;
        }
        return n;
    }

    void clear() {
        by_handle_.clear();
        by_handle_.push_back(nullptr);
    }

private:
    std::vector<T*> by_handle_;
};

}  // namespace ocg::serialize
