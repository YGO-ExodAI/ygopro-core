// Load side of state serialization — ExodAI Phase P1 Primitive 1 (chunk 4).
//
// Mirror of save_state.h. Parses a protobuf DuelState blob, allocates a
// fresh duel via the engine constructor (which requires callbacks via
// OCG_DuelOptions), then overlays the saved state onto the new duel.
//
// Lua reconstruction is not consumed in chunk 4 (no-Lua path); chunk 5
// adds the closure re-registration logic. Vanilla blobs (no cards / no
// effects / no Lua reconstruction) round-trip cleanly here.
//
// API note (chunk-4 amendment to phase_p1_primitive_1_plan.md §3.1):
// the public OCG_DuelLoadState now takes an OCG_DuelOptions* in addition
// to the buffer + outparam. The original draft signature lacked it, but
// the engine duel constructor needs callbacks (cardReader / scriptReader
// / logHandler) to be reachable by the loaded duel — those have to come
// from the caller. Saved state OVERRIDES seed / starting-LP / draw-count
// fields from the options at deserialization time.

#pragma once

#include "../ocgapi_types.h"

#include <string>

class duel;

namespace ocg::serialize {

// Parse a serialized DuelState blob and allocate a fresh duel populated
// from it. *out_duel must be nullptr on entry (strict output-empty
// contract per phase_p1_primitive_1_plan.md §3.1). On success, *out_duel
// owns a heap-allocated duel; caller frees via OCG_DestroyDuel (i.e.
// `delete static_cast<duel*>(*out_duel);` on the C-side).
//
// Returns one of OCG_LoadStatus (cast to int by the C API wrapper).
//
// On any non-OK return, *out_duel stays nullptr and *load_error is filled
// with a short human-readable explanation.
OCG_LoadStatus deserialize_duel(const void* buffer, std::size_t size,
                                const OCG_DuelOptions& options,
                                duel** out_duel,
                                std::string* load_error);

}  // namespace ocg::serialize
