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
#include <unordered_map>
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
// Classifier output for an upvalue (plan §13.1; chunk 5c extension)
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
    LUA_FUNCTION, // chunk 5c: dumpable Lua function (lua_dump-able)
    TABLE,        // chunk 5c: table (recursively walked)
    UNKNOWN,      // anything else: C function, thread, userdata of
                  // unknown shape, lightuserdata, large table, etc.
};

// ---------------------------------------------------------------------------
// Per-card save/load context (chunk 5c).
//
// Threads handle tables PLUS a per-card table-pointer registry through
// the recursive dump/restore path. The table registry preserves
// intra-card table identity: when the same lua_topointer appears as an
// upvalue of multiple closures within a single card, the second+ sighting
// emits CapturedArg.table_ref(handle) rather than a duplicate TableDef.
//
// Per-card scope means cross-card sharing is NOT preserved by this
// mechanism. The chunk-5c cross-card test fixture surfaces whether
// cross-card sharing is prevalent enough in the corpus to warrant a
// duel-wide registry. If the cross-card test fails, that's a known
// finding to report — not a 5c implementation bug.
// ---------------------------------------------------------------------------

struct LuaSaveContext {
    HandleTable<card>& hc;
    HandleTable<effect>& he;
    HandleTable<group>& hg;

    // Table-pointer registry. Cleared between cards via reset_per_card().
    std::unordered_map<const void*, uint32_t> table_registry;
    uint32_t next_table_handle = 1;

    // Recursion depth guard. 5c.0 measured max 8 inner-function-count;
    // we cap at 6 to match the observed tail with one level of margin.
    int max_depth = 6;
    int current_depth = 0;

    void reset_per_card() {
        table_registry.clear();
        next_table_handle = 1;
        current_depth = 0;
    }
};

struct LuaLoadContext {
    HandleResolver<card>& hc;
    HandleResolver<effect>& he;
    HandleResolver<group>& hg;

    // handle → Lua registry ref. The Lua state owns the table; we hold
    // a registry ref that prevents GC. Cleared between cards via
    // reset_per_card() — the Lua refs become unreferenced and collectible.
    std::unordered_map<uint32_t, int> table_handle_to_lua_ref;

    int current_depth = 0;

    void reset_per_card() {
        // We don't luaL_unref here — the GC will reclaim once nothing
        // else references the tables. Cleanup at duel-destruction time
        // happens via lua_close.
        table_handle_to_lua_ref.clear();
        current_depth = 0;
    }
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
                                  LuaSaveContext& ctx,
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
                             LuaLoadContext& ctx,
                             std::string* load_error);

}  // namespace ocg::serialize
