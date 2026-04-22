#include "refuse_detect.h"

#include "../duel.h"
#include "../interpreter.h"

namespace ocg::serialize {

RefuseReason detect_refuse(const duel& d) {
    // call_depth > 0 means we're inside an active Lua callback chain,
    // which only happens inside OCG_DuelProcess. At an MSG boundary
    // (post-Process, post-GetMessage, pre-SetResponse) Process has
    // unwound all Lua frames and call_depth == 0. See scoping doc
    // §10.1 Q1.2/Q1.3 for the proof that the Lua coroutine_map and
    // param_list are empty at MSG boundaries — call_depth == 0 is the
    // structural correlate.
    if (d.lua != nullptr && d.lua->call_depth > 0) {
        return RefuseReason::NOT_MSG_BOUNDARY;
    }

    // Chunk 6 will populate the unsafe-Lua-closure check here.
    // Currently always returns NONE for the closure path.

    return RefuseReason::NONE;
}

const char* refuse_reason_str(RefuseReason r) {
    switch (r) {
        case RefuseReason::NONE:
            return "ok";
        case RefuseReason::NOT_MSG_BOUNDARY:
            return "save called mid-process / inside Lua call (call_depth > 0)";
        case RefuseReason::UNSAFE_LUA_CLOSURE:
            return "effect registered via unknown Type-C closure generator";
    }
    return "unknown";
}

}  // namespace ocg::serialize
