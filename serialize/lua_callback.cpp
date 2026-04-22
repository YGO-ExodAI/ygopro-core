#include "lua_callback.h"

#include "handle_table.h"
#include "ocg_state.pb.h"

#include "../card.h"
#include "../duel.h"
#include "../effect.h"
#include "../group.h"
#include "../interpreter.h"
#include "../lua_obj.h"

extern "C" {
#include "../lua/src/lua.h"
#include "../lua/src/lauxlib.h"
}

#include <cstdio>
#include <cstring>
#include <string>

namespace ocg::serialize {

namespace {

// ---------------------------------------------------------------------------
// Forward decls (chunk 5c — recursive dump/restore)
// ---------------------------------------------------------------------------

// Captures one upvalue value at stack idx into `out`. Returns OK or refuses;
// recursive for function/table values. For UNKNOWN the caller fills
// refuse_reason and returns the appropriate status.
OCG_SaveStatus capture_value_recursive(lua_State* L, int idx, const duel& d,
                                        LuaSaveContext& ctx,
                                        uint32_t card_konami_id,
                                        const char* slot_name,
                                        int upvalue_idx,
                                        ocg::state::CapturedArg* out,
                                        std::string* refuse_reason);

OCG_SaveStatus dump_function_recursive(lua_State* L, int fn_idx, const duel& d,
                                        LuaSaveContext& ctx,
                                        uint32_t card_konami_id,
                                        const char* slot_name,
                                        ocg::state::LuaCallback* out,
                                        std::string* refuse_reason);

bool restore_value_recursive(lua_State* L,
                              const ocg::state::CapturedArg& arg,
                              LuaLoadContext& ctx,
                              std::string* load_error);

int32_t restore_function_recursive(lua_State* L,
                                    const ocg::state::LuaCallback& saved,
                                    LuaLoadContext& ctx,
                                    std::string* load_error);

// ---------------------------------------------------------------------------
// Plan §13.1: membership-set classifier. Engine handles are detected by
// reading the userdata payload as a pointer and checking against the duel's
// existing membership tables. No metatable enumeration.
// ---------------------------------------------------------------------------

UpvalueKind classify_userdata(lua_State* L, int idx, const duel& d) {
    if (lua_type(L, idx) != LUA_TUSERDATA) return UpvalueKind::UNKNOWN;
    if (lua_rawlen(L, idx) != sizeof(void*)) return UpvalueKind::UNKNOWN;

    void* payload = lua_touserdata(L, idx);
    lua_obj* obj = *static_cast<lua_obj**>(payload);
    if (obj == nullptr) return UpvalueKind::UNKNOWN;

    if (d.cards.find(static_cast<card*>(obj)) != d.cards.end()) {
        return UpvalueKind::CARD;
    }
    if (d.effects.find(static_cast<effect*>(obj)) != d.effects.end()) {
        return UpvalueKind::EFFECT;
    }
    if (d.groups.find(static_cast<group*>(obj)) != d.groups.end()) {
        return UpvalueKind::GROUP;
    }
    if (d.sgroups.find(static_cast<group*>(obj)) != d.sgroups.end()) {
        return UpvalueKind::GROUP;
    }
    return UpvalueKind::UNKNOWN;
}

// ---------------------------------------------------------------------------
// Build the informative refuse-reason string per plan §13.2.
// 256-char cap; truncates string-detail and table-detail with `...` suffix.
// ---------------------------------------------------------------------------

constexpr size_t kRefuseReasonMaxLen = 256;
// Chunk 5c: a "small" table for recursive walk. Above this size we refuse
// (the script is doing something we don't have a sized fixture for).
constexpr int kMaxTableKeysToWalk = 64;

std::string format_refuse_reason(lua_State* L, int idx,
                                  uint32_t card_konami_id,
                                  const char* slot_name,
                                  int upvalue_idx,
                                  const char* extra = nullptr) {
    char buf[kRefuseReasonMaxLen];
    const char* type_name = lua_typename(L, lua_type(L, idx));

    char prefix[160];
    std::snprintf(prefix, sizeof(prefix),
                  "upvalue type=%s at card=%u slot=%s upvalue_idx=%d; ",
                  type_name, card_konami_id,
                  slot_name ? slot_name : "(unknown)",
                  upvalue_idx);

    char details[160];
    details[0] = '\0';

    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            std::snprintf(details, sizeof(details), "nil");
            break;
        case LUA_TBOOLEAN:
            std::snprintf(details, sizeof(details), "bool=%s",
                          lua_toboolean(L, idx) ? "true" : "false");
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                std::snprintf(details, sizeof(details), "int=%lld",
                              static_cast<long long>(lua_tointeger(L, idx)));
            } else {
                std::snprintf(details, sizeof(details), "float=%g",
                              lua_tonumber(L, idx));
            }
            break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            char preview[40];
            const size_t shown = len < 32 ? len : 32;
            std::memcpy(preview, s, shown);
            preview[shown] = '\0';
            for (size_t i = 0; i < shown; ++i) {
                if (preview[i] < 32 || preview[i] == 127) preview[i] = '?';
            }
            std::snprintf(details, sizeof(details),
                          "string len=%zu \"%s%s\"",
                          len, preview, len > 32 ? "..." : "");
            break;
        }
        case LUA_TTABLE: {
            char keytypes[80];
            char valtypes[80];
            keytypes[0] = '\0';
            valtypes[0] = '\0';
            int count = 0, total = 0;
            lua_pushnil(L);
            while (lua_next(L, idx > 0 ? idx : idx - 1) != 0) {
                ++total;
                if (count < 8) {
                    const char* k_t = lua_typename(L, lua_type(L, -2));
                    const char* v_t = lua_typename(L, lua_type(L, -1));
                    if (count > 0) {
                        std::strncat(keytypes, ",",
                                     sizeof(keytypes) - std::strlen(keytypes) - 1);
                        std::strncat(valtypes, ",",
                                     sizeof(valtypes) - std::strlen(valtypes) - 1);
                    }
                    std::strncat(keytypes, k_t,
                                 sizeof(keytypes) - std::strlen(keytypes) - 1);
                    std::strncat(valtypes, v_t,
                                 sizeof(valtypes) - std::strlen(valtypes) - 1);
                    ++count;
                }
                lua_pop(L, 1);
            }
            if (total > count) {
                std::snprintf(details, sizeof(details),
                              "table: %d keys, types [%s,...] / [%s,...] (+%d)",
                              total, keytypes, valtypes, total - count);
            } else {
                std::snprintf(details, sizeof(details),
                              "table: %d keys, types [%s] / [%s]",
                              total, keytypes, valtypes);
            }
            break;
        }
        case LUA_TUSERDATA: {
            const char* name = "(none)";
            if (lua_getmetatable(L, idx)) {
                lua_getfield(L, -1, "__name");
                if (lua_isstring(L, -1)) {
                    name = lua_tostring(L, -1);
                }
                lua_pop(L, 2);
            }
            std::snprintf(details, sizeof(details),
                          "userdata size=%zu __name=%s",
                          lua_rawlen(L, idx), name);
            break;
        }
        case LUA_TFUNCTION: {
            // Distinguish Lua vs C function for the refuse path. Chunk-5c
            // does NOT support C functions (no bytecode); they continue to
            // refuse with a detailed reason.
            const bool is_c = lua_iscfunction(L, idx) != 0;
            if (is_c) {
                std::snprintf(details, sizeof(details),
                              "function (C function — no bytecode dump path)");
            } else {
                lua_pushvalue(L, idx);
                lua_Debug ar;
                std::memset(&ar, 0, sizeof(ar));
                if (lua_getinfo(L, ">S", &ar)) {
                    std::snprintf(details, sizeof(details),
                                  "function source=%s line=%d",
                                  ar.short_src ? ar.short_src : "?",
                                  ar.linedefined);
                } else {
                    std::snprintf(details, sizeof(details),
                                  "function (lua_getinfo failed)");
                }
            }
            break;
        }
        case LUA_TTHREAD:
            std::snprintf(details, sizeof(details), "thread (coroutine)");
            break;
        case LUA_TLIGHTUSERDATA:
            std::snprintf(details, sizeof(details), "lightuserdata ptr=%p",
                          lua_touserdata(L, idx));
            break;
        default:
            std::snprintf(details, sizeof(details),
                          "(unrecognized lua_type %d)", lua_type(L, idx));
            break;
    }

    if (extra && *extra) {
        std::snprintf(buf, sizeof(buf), "%s%s; %s", prefix, details, extra);
    } else {
        std::snprintf(buf, sizeof(buf), "%s%s", prefix, details);
    }
    return std::string(buf);
}

// lua_dump writer callback — appends bytes to the std::string in `ud`.
int dump_writer(lua_State* /*L*/, const void* p, size_t sz, void* ud) {
    static_cast<std::string*>(ud)->append(static_cast<const char*>(p), sz);
    return 0;
}

// ---------------------------------------------------------------------------
// Chunk 5c-A: C-function-name registry. Walks known engine namespaces to
// build a void*→qualified-name map (e.g. "Card.IsLocation", "aux.AND").
// Used to serialize C function upvalues by name — they can't be lua_dump'd.
// ---------------------------------------------------------------------------

// Recursively walk a table, recording function values. Path is the
// dotted name prefix for entries within this table (e.g. "Card.").
// Bounded by max_depth to avoid pathological cycles in user tables.
void walk_table_for_c_functions(
        lua_State* L, int tbl_idx, const std::string& prefix,
        std::unordered_map<const void*, std::string>& reg,
        int max_depth, int current_depth) {
    if (current_depth >= max_depth) return;
    luaL_checkstack(L, 4, nullptr);
    const int abs_tbl = tbl_idx > 0 ? tbl_idx : lua_absindex(L, tbl_idx);

    lua_pushnil(L);
    while (lua_next(L, abs_tbl) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key_str = lua_tostring(L, -2);
            const int vt = lua_type(L, -1);
            if (vt == LUA_TFUNCTION && lua_iscfunction(L, -1)) {
                const void* ptr = lua_topointer(L, -1);
                if (ptr && reg.find(ptr) == reg.end()) {
                    reg[ptr] = prefix + key_str;
                }
            } else if (vt == LUA_TTABLE) {
                // Recurse into nested namespace tables — but skip self-
                // referential entries and the script's `s` tables (those
                // start with 'c' followed by digits, like "c19162134").
                // Walking script self-tables would explode the registry
                // and isn't what we want — those don't expose C functions
                // worth referencing by name.
                bool skip = false;
                if (key_str[0] == 'c' && key_str[1] >= '0' && key_str[1] <= '9') {
                    skip = true;
                }
                // Avoid double-walking core globals to prevent the
                // registry-explosion from _G recursion through itself
                // (e.g. _G._G or package.loaded.<self>).
                if (!skip && std::strcmp(key_str, "_G") != 0 &&
                    std::strcmp(key_str, "loaded") != 0 &&
                    std::strcmp(key_str, "_LOADED") != 0 &&
                    std::strcmp(key_str, "package") != 0) {
                    walk_table_for_c_functions(L, -1,
                        prefix + key_str + ".",
                        reg, max_depth, current_depth + 1);
                }
            }
        }
        lua_pop(L, 1);
    }
}

// Build the registry. Walks a fixed set of engine namespaces — covers
// the patterns observed in the script corpus (Card.IsLocation,
// aux.FilterBoolFunctionEx, etc.). Functions outside these namespaces
// will refuse on save with an informative reason.
void build_c_function_registry(lua_State* L,
        std::unordered_map<const void*, std::string>& reg) {
    luaL_checkstack(L, 4, nullptr);

    // Engine namespaces. These cover the vast majority of C functions
    // exposed to scripts. Order doesn't matter — first sighting wins,
    // but each function appears under one canonical name.
    static const char* kNamespaces[] = {
        "Card", "Duel", "Effect", "Group", "Debug",
        "aux", "Auxiliary",
        // Lua stdlib
        "math", "string", "table", "io", "os", "coroutine",
        nullptr,
    };
    for (size_t i = 0; kNamespaces[i] != nullptr; ++i) {
        lua_getglobal(L, kNamespaces[i]);
        if (lua_type(L, -1) == LUA_TTABLE) {
            std::string prefix = std::string(kNamespaces[i]) + ".";
            walk_table_for_c_functions(L, -1, prefix, reg,
                                        /*max_depth=*/4,
                                        /*current_depth=*/0);
        }
        lua_pop(L, 1);
    }

    // Top-level _G C functions (e.g. type, tostring, ipairs, pairs).
    lua_pushglobaltable(L);
    walk_table_for_c_functions(L, -1, "", reg,
                                /*max_depth=*/2,  // shallow at top level
                                /*current_depth=*/0);
    lua_pop(L, 1);
}

// Detects the canonical `local s,id=GetID()` pattern: is this upvalue
// the script's own _G["c<owning_card_code>"] table?
uint32_t detect_script_self_table(lua_State* L, int idx,
                                   uint32_t owning_card_code) {
    if (owning_card_code == 0) return 0;
    if (lua_type(L, idx) != LUA_TTABLE) return 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "c%u", owning_card_code);
    lua_getglobal(L, buf);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 0; }
    const int normalized = idx > 0 ? idx : idx - 1;
    const bool same = lua_rawequal(L, normalized, -1) != 0;
    lua_pop(L, 1);
    return same ? owning_card_code : 0;
}

// ---------------------------------------------------------------------------
// Chunk 5c: dump a table at stack idx into a TableDef (or table_ref if
// the same pointer was already emitted in this card's save scope).
// ---------------------------------------------------------------------------

OCG_SaveStatus dump_table(lua_State* L, int idx, const duel& d,
                           LuaSaveContext& ctx,
                           uint32_t card_konami_id,
                           const char* slot_name,
                           int upvalue_idx,
                           ocg::state::CapturedArg* out,
                           std::string* refuse_reason) {
    const void* tbl_ptr = lua_topointer(L, idx);

    // Sharing check: if this pointer has been emitted in the current card,
    // emit a table_ref instead.
    auto it = ctx.table_registry.find(tbl_ptr);
    if (it != ctx.table_registry.end()) {
        out->set_table_ref(it->second);
        return OCG_SAVE_OK;
    }

    // First sighting. Walk the table; bail if it exceeds the size cap.
    // Pre-count keys for the refuse decision.
    //
    // Stack discipline: lua_pushnil + the lua_next loop is self-balancing
    // — lua_next pops the previous key + pushes new (key, value), and we
    // explicitly pop the value each iteration. When lua_next returns 0
    // it leaves the stack as it was after the initial pushnil minus that
    // nil — i.e., balanced with the entry depth.
    //
    // Early-exit on size cap: at that point the stack has [..., key]
    // (the new key is on top, value already popped). One pop restores
    // entry depth. (Pre-5c trap: an extra pop here corrupted the stack
    // and caused "invalid key to 'next'" panics from later iterations.)
    int total_keys = 0;
    {
        lua_pushnil(L);
        while (lua_next(L, idx > 0 ? idx : idx - 1) != 0) {
            ++total_keys;
            lua_pop(L, 1);  // pop value, key remains for next lua_next
            if (total_keys > kMaxTableKeysToWalk) {
                lua_pop(L, 1);  // pop the key (no nil-state — lua_next
                                // consumed the original nil)
                if (refuse_reason) {
                    char extra[80];
                    std::snprintf(extra, sizeof(extra),
                                  "exceeds chunk-5c size cap (>%d keys)",
                                  kMaxTableKeysToWalk);
                    *refuse_reason = format_refuse_reason(
                        L, idx, card_konami_id, slot_name, upvalue_idx, extra);
                }
                return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
            }
        }
    }

    // Assign a new handle and record before recursing — this lets a
    // self-referential table emit table_ref(handle) for itself, breaking
    // the cycle. (Defensive; not observed in 5c.0 but cheap to support.)
    const uint32_t handle = ctx.next_table_handle++;
    ctx.table_registry[tbl_ptr] = handle;

    auto* tdef = out->mutable_table_def();
    tdef->set_handle(handle);

    // Walk + capture entries.
    lua_pushnil(L);
    while (lua_next(L, idx > 0 ? idx : idx - 1) != 0) {
        // key at -2, value at -1
        auto* entry = tdef->add_entries();
        OCG_SaveStatus s;

        // Capture key
        s = capture_value_recursive(L, -2, d, ctx, card_konami_id, slot_name,
                                     upvalue_idx, entry->mutable_key(),
                                     refuse_reason);
        if (s != OCG_SAVE_OK) {
            lua_pop(L, 2);
            return s;
        }

        // Capture value
        s = capture_value_recursive(L, -1, d, ctx, card_konami_id, slot_name,
                                     upvalue_idx, entry->mutable_value(),
                                     refuse_reason);
        if (s != OCG_SAVE_OK) {
            lua_pop(L, 2);
            return s;
        }
        lua_pop(L, 1);  // pop value, keep key for lua_next
    }

    return OCG_SAVE_OK;
}

// ---------------------------------------------------------------------------
// Chunk 5c: capture a value (any type) into a CapturedArg.
//
// Replaces capture_known_upvalue. Now handles tables (recursive, via
// dump_table) and Lua functions (recursive, via dump_function_recursive).
// C functions, threads, lightuserdata, and other UNKNOWN types refuse.
//
// Caller must NOT pre-classify; this function dispatches on lua_type.
// (Exception: the top-level dump_lua_callback still pre-checks _ENV and
// script_self_table for fast paths — those return before getting here.)
// ---------------------------------------------------------------------------

OCG_SaveStatus capture_value_recursive(lua_State* L, int idx, const duel& d,
                                        LuaSaveContext& ctx,
                                        uint32_t card_konami_id,
                                        const char* slot_name,
                                        int upvalue_idx,
                                        ocg::state::CapturedArg* out,
                                        std::string* refuse_reason) {
    // Normalize idx — when called from inside a recursive context the
    // caller may pass negative indexes that shift as we push/pop.
    const int abs_idx = idx > 0 ? idx : lua_absindex(L, idx);

    switch (lua_type(L, abs_idx)) {
        case LUA_TNIL:
            out->set_i(0);  // permissive: nil → integer 0 (per plan §13.2 alt)
            return OCG_SAVE_OK;
        case LUA_TBOOLEAN:
            out->set_b(lua_toboolean(L, abs_idx) != 0);
            return OCG_SAVE_OK;
        case LUA_TNUMBER:
            if (lua_isinteger(L, abs_idx)) {
                out->set_i(lua_tointeger(L, abs_idx));
            } else {
                out->set_d(lua_tonumber(L, abs_idx));
            }
            return OCG_SAVE_OK;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, abs_idx, &len);
            out->set_s(s, len);
            return OCG_SAVE_OK;
        }
        case LUA_TUSERDATA: {
            UpvalueKind kind = classify_userdata(L, abs_idx, d);
            switch (kind) {
                case UpvalueKind::CARD: {
                    void* payload = lua_touserdata(L, abs_idx);
                    card* obj = *static_cast<card**>(payload);
                    out->set_card_handle(ctx.hc.assign(obj));
                    return OCG_SAVE_OK;
                }
                case UpvalueKind::EFFECT: {
                    void* payload = lua_touserdata(L, abs_idx);
                    effect* obj = *static_cast<effect**>(payload);
                    out->set_effect_handle(ctx.he.assign(obj));
                    return OCG_SAVE_OK;
                }
                case UpvalueKind::GROUP: {
                    void* payload = lua_touserdata(L, abs_idx);
                    group* obj = *static_cast<group**>(payload);
                    out->set_group_handle(ctx.hg.assign(obj));
                    return OCG_SAVE_OK;
                }
                default:
                    if (refuse_reason) {
                        *refuse_reason = format_refuse_reason(
                            L, abs_idx, card_konami_id, slot_name, upvalue_idx);
                    }
                    return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
            }
        }
        case LUA_TTABLE:
            // Special-case script_self table at any nesting depth.
            {
                const uint32_t self_code =
                    detect_script_self_table(L, abs_idx, card_konami_id);
                if (self_code != 0) {
                    out->set_script_self_card_code(self_code);
                    return OCG_SAVE_OK;
                }
            }
            return dump_table(L, abs_idx, d, ctx, card_konami_id, slot_name,
                              upvalue_idx, out, refuse_reason);
        case LUA_TFUNCTION:
            // Chunk 5c-A: C functions resolved by qualified name via
            // the registry built at first encounter.
            if (lua_iscfunction(L, abs_idx)) {
                if (!ctx.c_function_registry_built) {
                    build_c_function_registry(L, ctx.c_function_registry);
                    ctx.c_function_registry_built = true;
                }
                const void* fn_ptr = lua_topointer(L, abs_idx);
                auto it = ctx.c_function_registry.find(fn_ptr);
                if (it != ctx.c_function_registry.end()) {
                    out->set_c_function_name(it->second);
                    return OCG_SAVE_OK;
                }
                // Not in any known namespace — refuse with informative
                // reason. The reason already says "C function" via
                // format_refuse_reason; append the not-found note.
                if (refuse_reason) {
                    *refuse_reason = format_refuse_reason(
                        L, abs_idx, card_konami_id, slot_name, upvalue_idx,
                        "not found in chunk-5c-A name registry");
                }
                return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
            }
            // Lua function — recursive dump.
            return dump_function_recursive(L, abs_idx, d, ctx,
                                            card_konami_id, slot_name,
                                            out->mutable_function_def(),
                                            refuse_reason);
        default:
            if (refuse_reason) {
                *refuse_reason = format_refuse_reason(
                    L, abs_idx, card_konami_id, slot_name, upvalue_idx);
            }
            return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
    }
}

// ---------------------------------------------------------------------------
// Chunk 5c: dump a Lua function at stack idx into a LuaCallback message.
//
// Used both for top-level effect callbacks and for recursive function
// upvalues. Depth-bounded by ctx.max_depth.
//
// Function must be a Lua function (not C). Caller's responsibility to
// check; this fn refuses with INTERNAL if it sees a C function.
// ---------------------------------------------------------------------------

OCG_SaveStatus dump_function_recursive(lua_State* L, int fn_idx, const duel& d,
                                        LuaSaveContext& ctx,
                                        uint32_t card_konami_id,
                                        const char* slot_name,
                                        ocg::state::LuaCallback* out,
                                        std::string* refuse_reason) {
    if (ctx.current_depth >= ctx.max_depth) {
        if (refuse_reason) {
            *refuse_reason = "function-upvalue recursion exceeded depth cap " +
                             std::to_string(ctx.max_depth);
        }
        return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
    }

    luaL_checkstack(L, 4, nullptr);
    const int abs_fn_idx = fn_idx > 0 ? fn_idx : lua_absindex(L, fn_idx);

    out->set_present(true);

    // Get nups (consumes a copy of the function).
    lua_pushvalue(L, abs_fn_idx);
    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    if (!lua_getinfo(L, ">u", &ar)) {
        if (refuse_reason) {
            *refuse_reason = "lua_getinfo failed in recursive dump";
        }
        return OCG_SAVE_ERR_INTERNAL;
    }
    const int nups = ar.nups;

    // Walk + capture upvalues.
    ctx.current_depth++;
    for (int i = 1; i <= nups; ++i) {
        const char* upname = lua_getupvalue(L, abs_fn_idx, i);
        if (!upname) {
            ctx.current_depth--;
            if (refuse_reason) {
                *refuse_reason = "lua_getupvalue(" + std::to_string(i) +
                                 ") returned NULL in recursive dump";
            }
            return OCG_SAVE_ERR_INTERNAL;
        }
        const int up_idx = lua_gettop(L);
        ocg::state::CapturedArg* arg = out->add_upvalues();

        if (std::strcmp(upname, "_ENV") == 0) {
            // VALUE_NOT_SET marker
            lua_pop(L, 1);
            continue;
        }

        OCG_SaveStatus s = capture_value_recursive(
            L, up_idx, d, ctx, card_konami_id, slot_name, i,
            arg, refuse_reason);
        lua_pop(L, 1);
        if (s != OCG_SAVE_OK) {
            ctx.current_depth--;
            return s;
        }
    }
    ctx.current_depth--;

    // Now dump bytecode.
    //
    // Stack discipline: lua_dump requires the function at the top of the
    // stack but does NOT pop it (per Lua 5.3 manual). Push a copy, dump,
    // then explicitly pop the copy. Failing to pop leaks one stack slot
    // per recursion level — undetectable until the stack overflows or
    // until a subsequent lua_next sees an unexpected key type and
    // panics with "invalid key to 'next'".
    lua_pushvalue(L, abs_fn_idx);
    std::string bytecode;
    const int dump_result = lua_dump(L, &dump_writer, &bytecode, /*strip=*/0);
    lua_pop(L, 1);  // pop the copy lua_dump left on the stack
    if (dump_result != 0) {
        if (refuse_reason) {
            *refuse_reason = "lua_dump returned " +
                             std::to_string(dump_result) +
                             " in recursive dump";
        }
        return OCG_SAVE_ERR_INTERNAL;
    }
    out->set_bytecode(std::move(bytecode));
    return OCG_SAVE_OK;
}

}  // namespace

UpvalueKind classify_upvalue(lua_State* L, int idx, const duel& d) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     return UpvalueKind::NIL;
        case LUA_TBOOLEAN: return UpvalueKind::BOOLEAN;
        case LUA_TNUMBER:
            return lua_isinteger(L, idx) ? UpvalueKind::INTEGER
                                          : UpvalueKind::NUMBER;
        case LUA_TSTRING:  return UpvalueKind::STRING;
        case LUA_TUSERDATA:
            return classify_userdata(L, idx, d);
        case LUA_TTABLE:
            return UpvalueKind::TABLE;
        case LUA_TFUNCTION:
            return lua_iscfunction(L, idx) ? UpvalueKind::UNKNOWN
                                            : UpvalueKind::LUA_FUNCTION;
        default:
            return UpvalueKind::UNKNOWN;
    }
}

OCG_SaveStatus dump_lua_callback(lua_State* L, int32_t lua_ref, const duel& d,
                                  LuaSaveContext& ctx,
                                  uint32_t card_konami_id,
                                  const char* slot_name,
                                  ocg::state::LuaCallback* out,
                                  std::string* refuse_reason) {
    out->set_present(false);
    if (lua_ref == 0) return OCG_SAVE_OK;

    luaL_checkstack(L, 4, nullptr);
    const int top_before = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
    if (!lua_isfunction(L, -1)) {
        // Engine sometimes stores non-function references in callback
        // slots (e.g. SetValue with a literal int). Treat as absent.
        lua_settop(L, top_before);
        out->set_present(false);
        return OCG_SAVE_OK;
    }
    if (lua_iscfunction(L, -1)) {
        // Chunk 5c-A: top-level C-function callback, resolved by
        // qualified name via the registry. The 6 "lua_dump returned 1"
        // cases from chunk-6 + the broader cohort exposed by 5c's
        // recursive walk land here.
        //
        // We encode this as LuaCallback.present=true with bytecode
        // empty AND a single CapturedArg.c_function_name in upvalues[0].
        // Load side detects this shape and pushes the named C function
        // directly instead of luaL_loadbuffer. (Reusing the existing
        // upvalues field avoids a schema bump for one variant — the
        // schema already supports CapturedArg.c_function_name; we just
        // use it at slot 0 instead of as a captured upvalue.)
        if (!ctx.c_function_registry_built) {
            build_c_function_registry(L, ctx.c_function_registry);
            ctx.c_function_registry_built = true;
        }
        const void* fn_ptr = lua_topointer(L, -1);
        auto it = ctx.c_function_registry.find(fn_ptr);
        if (it == ctx.c_function_registry.end()) {
            if (refuse_reason) {
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                    "top-level callback at slot=%s on card=%u is a "
                    "C function not in chunk-5c-A name registry "
                    "(check engine namespace coverage)",
                    slot_name ? slot_name : "?", card_konami_id);
                *refuse_reason = buf;
            }
            lua_settop(L, top_before);
            return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
        }
        out->set_present(true);
        // Empty bytecode + single CapturedArg.c_function_name marks
        // "this is a C function callback".
        auto* arg = out->add_upvalues();
        arg->set_c_function_name(it->second);
        lua_settop(L, top_before);
        return OCG_SAVE_OK;
    }

    // The function is at top of stack; let dump_function_recursive handle
    // upvalue walking + bytecode dump + recursion.
    OCG_SaveStatus s = dump_function_recursive(L, -1, d, ctx,
                                                card_konami_id, slot_name,
                                                out, refuse_reason);
    lua_settop(L, top_before);
    return s;
}

// ---------------------------------------------------------------------------
// Load side
// ---------------------------------------------------------------------------

namespace {

// Push a value at the load site for the given CapturedArg.
// Returns true on success (one value pushed onto the stack).
// On failure: returns false and writes to load_error; nothing pushed.
bool restore_value_recursive(lua_State* L,
                              const ocg::state::CapturedArg& arg,
                              LuaLoadContext& ctx,
                              std::string* load_error) {
    switch (arg.value_case()) {
        case ocg::state::CapturedArg::VALUE_NOT_SET:
            // Caller (function-restore path) treats this as "skip
            // setupvalue" rather than pushing a value. But we may also
            // see VALUE_NOT_SET inside a TableEntry (shouldn't happen but
            // be defensive) — push nil.
            lua_pushnil(L);
            return true;
        case ocg::state::CapturedArg::kB:
            lua_pushboolean(L, arg.b() ? 1 : 0);
            return true;
        case ocg::state::CapturedArg::kI:
            lua_pushinteger(L, static_cast<lua_Integer>(arg.i()));
            return true;
        case ocg::state::CapturedArg::kD:
            lua_pushnumber(L, arg.d());
            return true;
        case ocg::state::CapturedArg::kS:
            lua_pushlstring(L, arg.s().data(), arg.s().size());
            return true;
        case ocg::state::CapturedArg::kCardHandle: {
            card* c = ctx.hc.lookup(arg.card_handle());
            if (c == nullptr) lua_pushnil(L);
            else interpreter::pushobject(L, c->ref_handle);
            return true;
        }
        case ocg::state::CapturedArg::kEffectHandle: {
            effect* e = ctx.he.lookup(arg.effect_handle());
            if (e == nullptr) lua_pushnil(L);
            else interpreter::pushobject(L, e->ref_handle);
            return true;
        }
        case ocg::state::CapturedArg::kGroupHandle: {
            group* g = ctx.hg.lookup(arg.group_handle());
            if (g == nullptr) lua_pushnil(L);
            else interpreter::pushobject(L, g->ref_handle);
            return true;
        }
        case ocg::state::CapturedArg::kScriptSelfCardCode: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "c%u", arg.script_self_card_code());
            lua_getglobal(L, buf);
            if (lua_isnil(L, -1)) {
                if (load_error) {
                    *load_error = std::string("script_self table _G[\"") +
                                  buf + "\"] not loaded";
                }
                lua_pop(L, 1);
                return false;
            }
            return true;
        }
        case ocg::state::CapturedArg::kFunctionDef: {
            // Recursive function load. luaL_loadbuffer + setupvalues.
            int32_t ref = restore_function_recursive(L, arg.function_def(),
                                                       ctx, load_error);
            if (ref == 0) return false;
            // restore_function_recursive registered the function in
            // LUA_REGISTRYINDEX. Push it back onto the stack as the
            // value to set as upvalue, then unref the registry slot
            // (the upvalue now holds the strong reference).
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return true;
        }
        case ocg::state::CapturedArg::kTableDef: {
            // First sighting: materialize table, register handle, push.
            const auto& tdef = arg.table_def();
            lua_createtable(L, 0, tdef.entries_size());
            const int tbl_idx = lua_gettop(L);
            // Register BEFORE walking entries — supports self-referential
            // tables (matches save side's pre-recursion registry insert).
            lua_pushvalue(L, tbl_idx);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            ctx.table_handle_to_lua_ref[tdef.handle()] = ref;

            for (const auto& entry : tdef.entries()) {
                if (!restore_value_recursive(L, entry.key(), ctx, load_error)) {
                    return false;
                }
                if (!restore_value_recursive(L, entry.value(), ctx, load_error)) {
                    lua_pop(L, 1);  // pop key
                    return false;
                }
                // settable consumes key and value; tbl_idx unchanged.
                lua_settable(L, tbl_idx);
            }
            return true;
        }
        case ocg::state::CapturedArg::kTableRef: {
            auto it = ctx.table_handle_to_lua_ref.find(arg.table_ref());
            if (it == ctx.table_handle_to_lua_ref.end()) {
                if (load_error) {
                    *load_error = "table_ref(" +
                                  std::to_string(arg.table_ref()) +
                                  ") not found in load context — save-side "
                                  "registry mismatch";
                }
                return false;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
            return true;
        }
        case ocg::state::CapturedArg::kCFunctionName: {
            // Chunk 5c-A: walk the qualified name (e.g. "Card.IsLocation")
            // by lua_getglobal + nested lua_getfield. Push the resolved
            // C function on success.
            const std::string& qname = arg.c_function_name();
            // Tokenize on '.'.
            size_t start = 0;
            size_t dot = qname.find('.', start);
            std::string head = qname.substr(start, dot - start);
            lua_getglobal(L, head.c_str());
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                if (load_error) {
                    *load_error = "c_function_name '" + qname +
                                  "' root '" + head + "' not in _G";
                }
                return false;
            }
            while (dot != std::string::npos) {
                start = dot + 1;
                dot = qname.find('.', start);
                std::string seg = qname.substr(start, dot - start);
                if (lua_type(L, -1) != LUA_TTABLE) {
                    lua_pop(L, 1);
                    if (load_error) {
                        *load_error = "c_function_name '" + qname +
                                      "' segment '" + seg +
                                      "' parent is not a table";
                    }
                    return false;
                }
                lua_getfield(L, -1, seg.c_str());
                lua_remove(L, -2);  // remove the parent table
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    if (load_error) {
                        *load_error = "c_function_name '" + qname +
                                      "' segment '" + seg + "' not found";
                    }
                    return false;
                }
            }
            // Final value should be a C function. (Defensive — could be
            // a Lua function if the namespace changed between save and
            // load; we accept either since the calling code just uses
            // whatever's at this name now.)
            return true;
        }
    }
    if (load_error) *load_error = "unhandled CapturedArg variant";
    return false;
}

// Recursive function restore. Returns LUA_REGISTRYINDEX ref (>0) on success,
// 0 on failure (load_error filled).
//
// Caller can either keep the ref (top-level effect slot) or push the
// function back onto the stack via lua_rawgeti(L, LUA_REGISTRYINDEX, ref)
// for use as an upvalue (recursive case in restore_value_recursive does
// this and unrefs immediately).
int32_t restore_function_recursive(lua_State* L,
                                    const ocg::state::LuaCallback& saved,
                                    LuaLoadContext& ctx,
                                    std::string* load_error) {
    if (!saved.present()) return 0;

    // Chunk 5c-A: top-level C-function callback shape. Save side encodes
    // these as present=true + bytecode empty + a single CapturedArg
    // c_function_name in upvalues[0]. Detect that shape, push the named
    // C function, register it.
    if (saved.bytecode().empty()) {
        if (saved.upvalues_size() == 1 &&
            saved.upvalues(0).value_case() ==
                ocg::state::CapturedArg::kCFunctionName) {
            if (!restore_value_recursive(L, saved.upvalues(0), ctx, load_error)) {
                return 0;
            }
            // Function on stack top; register and return ref.
            return luaL_ref(L, LUA_REGISTRYINDEX);
        }
        if (load_error) *load_error =
            "LuaCallback present=true but bytecode empty (and not a "
            "5c-A C-function-callback shape)";
        return 0;
    }

    luaL_checkstack(L, 4, nullptr);
    const int top_before = lua_gettop(L);

    const int load_result = luaL_loadbuffer(L,
                                             saved.bytecode().data(),
                                             saved.bytecode().size(),
                                             "@reconstructed_callback");
    if (load_result != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (load_error) {
            *load_error = std::string("luaL_loadbuffer failed: ") +
                          (msg ? msg : "(none)");
        }
        lua_settop(L, top_before);
        return 0;
    }

    // Set upvalues. Function at stack top.
    const int fn_idx = lua_gettop(L);
    for (int i = 0; i < saved.upvalues_size(); ++i) {
        const int up_idx = i + 1;
        const auto& arg = saved.upvalues(i);

        if (arg.value_case() == ocg::state::CapturedArg::VALUE_NOT_SET) {
            continue;  // _ENV preserved
        }

        if (!restore_value_recursive(L, arg, ctx, load_error)) {
            lua_settop(L, top_before);
            return 0;
        }
        // Now the value is on top; function below at fn_idx.
        const char* upname = lua_setupvalue(L, fn_idx, up_idx);
        if (upname == nullptr) {
            if (load_error) {
                *load_error = "lua_setupvalue(" + std::to_string(up_idx) +
                              ") returned NULL — upvalue count mismatch";
            }
            lua_settop(L, top_before);
            return 0;
        }
    }

    // Register and return ref. luaL_ref pops the function from the stack.
    const int32_t new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return new_ref;
}

}  // namespace

int32_t restore_lua_callback(lua_State* L,
                              const ocg::state::LuaCallback& saved,
                              LuaLoadContext& ctx,
                              std::string* load_error) {
    return restore_function_recursive(L, saved, ctx, load_error);
}

}  // namespace ocg::serialize
