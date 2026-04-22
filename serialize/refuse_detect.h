// Refuse-path detection — ExodAI Phase P1 Primitive 1 (chunk 3).
//
// Per phase_p1_primitive_1_plan.md §5.1, OCG_DuelSaveState must refuse
// in two cases:
//   1. Mid-process / inside Lua callback (NOT_MSG_BOUNDARY)
//   2. Unsafe Type-C closure pattern not in the allow-list (REFUSE_UNSAFE_LUA)
//
// Chunk 3 implements (1) only; (2) lands in chunk 6 once the Lua
// reconstruction path exists. The detect_refuse() interface returns
// both via a single status enum so the chunk-6 work doesn't need to
// reshape the signature.

#pragma once

class duel;

namespace ocg::serialize {

enum class RefuseReason {
    NONE = 0,                  // safe to save
    NOT_MSG_BOUNDARY = 1,      // mid-Process or inside a Lua call
    UNSAFE_LUA_CLOSURE = 2,    // chunk-6 placeholder (always NONE in chunk 3)
};

// Returns NONE if the duel is in a safe-to-save state at this instant.
// Otherwise returns the specific refuse reason. Never blocks.
//
// Detection signal for NOT_MSG_BOUNDARY (chunk 3 — high confidence):
//   interpreter::call_depth > 0 means we're inside a Lua call, which
//   means we're inside OCG_DuelProcess. At a true MSG boundary (post-
//   Process return, post-GetMessage drain, pre-SetResponse), call_depth
//   is 0 because Process unwinds all Lua frames before returning.
RefuseReason detect_refuse(const duel& d);

// Human-readable explanation for refuse_reason; goes into the JSON
// sidecar's `refuse_reason` field at the Python layer (chunk 7).
const char* refuse_reason_str(RefuseReason r);

}  // namespace ocg::serialize
