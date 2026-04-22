// Lua bytecode cache scaffolding — ExodAI Phase P1 Primitive 1 (chunk 3).
//
// This is the SCAFFOLDING. The cache itself does nothing useful in chunk 3
// (the load path that would consume it doesn't exist yet — chunk 4 lands
// load, chunk 5 lands the Lua reconstruction path that drives the cache).
// Present here so that:
//   1. The chunk-3 chunk boundary leaves a complete-ish module shape for
//      review (no "TODO add bytecode_cache.h later" notes).
//   2. The header-include surface is stable from chunk 3 onward; chunk 5
//      grows the impl without touching consumer call sites.
//
// Per phase_p1_primitive_1_plan.md §1 (resolved decisions) and §10
// chunk 4 sub-piece "Bytecode cache scaffolding (cache present but
// unused)".
//
// Design (planned for chunk 5):
//   - Key:   (uint32_t card_code, std::string script_sha256)
//   - Value: compiled Lua chunk (Lua bytecode bytes) ready to feed
//            into luaL_loadbufferx() during reconstruction.
//   - LRU eviction with configurable max-bytes. Chunk-3 stub holds
//     no entries; eviction logic is a no-op.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ocg::serialize {

class BytecodeCache {
public:
    BytecodeCache() = default;

    // Lookup. Chunk-3 stub always returns false (cache is empty).
    // Chunk 5: returns true and fills *out with the cached bytecode.
    bool lookup(uint32_t card_code, const std::string& script_sha256,
                std::string* out) const;

    // Insert. Chunk-3 stub is a no-op. Chunk 5: inserts under LRU policy.
    void insert(uint32_t card_code, const std::string& script_sha256,
                std::string bytecode);

    // Evict everything. Chunk-3 stub is a no-op (nothing to clear).
    void clear();

    // Size in bytes. Chunk-3 stub returns 0.
    size_t size_bytes() const;
};

// Process-wide singleton shared by save and load paths. Chunk 5 wires
// load against this; chunk 3 reserves the access pattern.
BytecodeCache& global_bytecode_cache();

}  // namespace ocg::serialize
