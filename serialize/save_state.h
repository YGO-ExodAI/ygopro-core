// Save side of state serialization — ExodAI Phase P1 Primitive 1 (chunk 3).
//
// Walks the engine's `duel` tree (field + players + cards + effects + groups
// + chain + processor + RNG) into a protobuf DuelState message and serializes
// it to bytes. Lua reconstruction (the `LuaReconstruction` subtree) is left
// empty in chunk 3; chunk 5 populates it via Type-C closure capture.
//
// Determinism contract (per phase_p1_primitive_1_plan.md §10.4):
//   - Two saves of the same in-memory state produce byte-equal output.
//   - DuelState.timestamp_unix is intentionally fixed to 0 in the protobuf
//     (the real save timestamp lives in the JSON sidecar that the Python
//     layer writes alongside; chunk 7). This keeps byte-equal CI checks
//     stable regardless of wall-clock between the two saves.
//   - Iteration over `unordered_set` containers is replaced by walks
//     through stable orders (zone vectors → field structures → handle
//     tables in monotonic order).
//
// Refuse-path contract:
//   - Returns OCG_SAVE_ERR_NOT_MSG_BOUNDARY without producing any bytes
//     when called inside a Lua call (interpreter::call_depth > 0). See
//     refuse_detect.h.

#pragma once

#include "../ocgapi_types.h"

#include <string>

class duel;

namespace ocg::serialize {

// Write a serialized DuelState to *out.
//
// Returns one of OCG_SaveStatus (cast to int by the C API wrapper):
//   OCG_SAVE_OK                   — *out contains the serialized bytes.
//   OCG_SAVE_ERR_NOT_MSG_BOUNDARY — refused; *out is empty.
//   OCG_SAVE_ERR_INTERNAL         — unexpected protobuf or walk error;
//                                   *out is empty.
//
// On any non-OK status, *refuse_reason is filled with a short human-
// readable explanation suitable for the JSON sidecar / log output.
//
// Schema constants currently used (chunk 3):
//   schema_version  = 1
//   engine_build_hash = 0  (chunk 7 will wire the real ocgcore SHA)
//   timestamp_unix  = 0  (intentionally; see header comment)
OCG_SaveStatus serialize_duel(const duel& d,
                              std::string* out,
                              std::string* refuse_reason);

// Schema version tag the chunk-3 serializer writes into DuelState.
// Bump on breaking schema changes; loaders compare against this.
constexpr uint32_t kSchemaVersion = 1;

}  // namespace ocg::serialize
