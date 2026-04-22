#include "bytecode_cache.h"

namespace ocg::serialize {

bool BytecodeCache::lookup(uint32_t /*card_code*/,
                           const std::string& /*script_sha256*/,
                           std::string* /*out*/) const {
    // Chunk-3 stub. Chunk 5 fills the implementation.
    return false;
}

void BytecodeCache::insert(uint32_t /*card_code*/,
                           const std::string& /*script_sha256*/,
                           std::string /*bytecode*/) {
    // Chunk-3 stub. Chunk 5 fills the implementation.
}

void BytecodeCache::clear() {
    // Chunk-3 stub: nothing to clear.
}

size_t BytecodeCache::size_bytes() const {
    return 0;
}

BytecodeCache& global_bytecode_cache() {
    static BytecodeCache instance;
    return instance;
}

}  // namespace ocg::serialize
