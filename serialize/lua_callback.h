// Lua callback dump+restore for chunk 5b — ExodAI Phase P1 Primitive 1.
//
// One side per direction:
//   dump_lua_callback   — save side: serialize a Lua function ref (effect's
//                         condition/cost/target/value/operation slot) into
//                         bytecode + classified upvalues.
//   restore_lua_callback — load side: re-create the function from bytecode,
//                         set its upvalues, register in Lua registry, return
//                         the new ref.
//
// Plus the userdata classifier per plan §13.1 (membership-set approach).
//
// All three live in the ocg::serialize namespace alongside HandleTable /
// HandleResolver. Engine includes are minimal — only what's needed for
// the membership-set classifier (duel, card, effect, group sets).

#pragma once

#include "../ocgapi_types.h"

#include <cstdint>
#include <string>
#include <vector>

class duel;
class card;
class effect;
class group;
struct lua_State;

namespace ocg::state {
class CapturedArg;
class LuaCallback;
}

namespace ocg::serialize {

template <typename T> class HandleTable;
template <typename T> class HandleResolver;

// ---------------------------------------------------------------------------
// Classifier output for an upvalue (plan §13.1)
// ---------------------------------------------------------------------------

enum class UpvalueKind {
    NIL,          // lua nil
    BOOLEAN,
    INTEGER,      // lua_isinteger
    NUMBER,       // lua_isnumber but not integer (double)
    STRING,
    CARD,         // engine card userdata (membership-set confirmed)
    EFFECT,       // engine effect userdata
    GROUP,        // engine group userdata
    UNKNOWN,      // anything else: table, function, thread, userdata of
                  // unknown shape, lightuserdata, etc.
};

// Classify the value at stack index `idx` per plan §13.1's membership-set
// approach. Engine handles are detected by reading the userdata payload as
// a pointer and checking against the duel's existing membership tables
// (duel.cards / .effects / .groups / .sgroups). No metatable enumeration.
//
// Pure read of the Lua stack; doesn't modify it.
UpvalueKind classify_upvalue(lua_State* L, int idx, const duel& d);

// ---------------------------------------------------------------------------
// Dump a Lua function ref (the engine's int32 callback ref, e.g.
// effect.condition) into bytecode + classified upvalues.
//
// On success: writes to `out`, returns OCG_SAVE_OK.
// On unknown-upvalue: writes informative reason to `refuse_reason` per
//   plan §13.2 format, returns OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE.
// On any other failure (function ref invalid, lua_dump error): returns
//   OCG_SAVE_ERR_INTERNAL with a reason.
//
// The `slot_name` and `card_konami_id` arguments feed the refuse-reason
// string formatting so logs surface enough context to classify new
// upvalue patterns without re-instrumentation (per plan §13.2).
// ---------------------------------------------------------------------------
OCG_SaveStatus dump_lua_callback(lua_State* L, int32_t lua_ref, const duel& d,
                                  HandleTable<card>& hc,
                                  HandleTable<effect>& he,
                                  HandleTable<group>& hg,
                                  uint32_t card_konami_id,
                                  const char* slot_name,
                                  ocg::state::LuaCallback* out,
                                  std::string* refuse_reason);

// ---------------------------------------------------------------------------
// Restore a saved LuaCallback into a fresh Lua function ref.
//
// On success: returns the new int32 ref (suitable for storing in
// effect.condition / etc.), >0.
// On failure: returns 0 and writes to `load_error`. Caller treats 0 as
// "no callback bound" — distinct from "callback present but invalid"
// (which sets load_error and additionally treats the load as malformed).
// ---------------------------------------------------------------------------
int32_t restore_lua_callback(lua_State* L,
                             const ocg::state::LuaCallback& saved,
                             HandleResolver<card>& hc,
                             HandleResolver<effect>& he,
                             HandleResolver<group>& hg,
                             std::string* load_error);

}  // namespace ocg::serialize
