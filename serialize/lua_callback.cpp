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

    // Pointer-equality membership lookups. We never dereference `obj` until
    // confirmed in one of the engine's own sets (eliminates the false-
    // positive risk per plan §13.1).
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

std::string format_refuse_reason(lua_State* L, int idx,
                                  uint32_t card_konami_id,
                                  const char* slot_name,
                                  int upvalue_idx) {
    char buf[kRefuseReasonMaxLen];
    const char* type_name = lua_typename(L, lua_type(L, idx));

    // Prefix shared by all types
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
            // Replace non-printable for safety
            for (size_t i = 0; i < shown; ++i) {
                if (preview[i] < 32 || preview[i] == 127) preview[i] = '?';
            }
            std::snprintf(details, sizeof(details),
                          "string len=%zu \"%s%s\"",
                          len, preview, len > 32 ? "..." : "");
            break;
        }
        case LUA_TTABLE: {
            // Walk first 8 entries, list key/value type names
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
                lua_pop(L, 1);  // remove value, keep key for next iteration
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
            // Try to read __name metafield (Lua 5.3+)
            const char* name = "(none)";
            if (lua_getmetatable(L, idx)) {
                lua_getfield(L, -1, "__name");
                if (lua_isstring(L, -1)) {
                    name = lua_tostring(L, -1);
                }
                lua_pop(L, 2);  // pop __name + metatable
            }
            std::snprintf(details, sizeof(details),
                          "userdata size=%zu __name=%s",
                          lua_rawlen(L, idx), name);
            break;
        }
        case LUA_TFUNCTION: {
            // Try lua_getinfo to get source + line
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

    std::snprintf(buf, sizeof(buf), "%s%s", prefix, details);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Read the value at stack index `idx` and write to `out` as a CapturedArg
// (per plan §13.4). Caller has already classified the upvalue and confirmed
// it's a known type. For unknown types, caller calls format_refuse_reason
// instead.
// ---------------------------------------------------------------------------

bool capture_known_upvalue(lua_State* L, int idx, UpvalueKind kind,
                            HandleTable<card>& hc,
                            HandleTable<effect>& he,
                            HandleTable<group>& hg,
                            ocg::state::CapturedArg* out) {
    switch (kind) {
        case UpvalueKind::NIL:
            // We treat nil upvalues as "no value" — captured int 0 as a
            // placeholder. (Per plan §13.2, nil is typically refused, but
            // since the audit didn't see nil captures we can also accept
            // them as int 0 to be more permissive.)
            out->set_i(0);
            return true;
        case UpvalueKind::BOOLEAN:
            out->set_b(lua_toboolean(L, idx) != 0);
            return true;
        case UpvalueKind::INTEGER:
            out->set_i(lua_tointeger(L, idx));
            return true;
        case UpvalueKind::NUMBER:
            out->set_d(lua_tonumber(L, idx));
            return true;
        case UpvalueKind::STRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            out->set_s(s, len);
            return true;
        }
        case UpvalueKind::CARD: {
            void* payload = lua_touserdata(L, idx);
            card* obj = *static_cast<card**>(payload);
            out->set_card_handle(hc.assign(obj));
            return true;
        }
        case UpvalueKind::EFFECT: {
            void* payload = lua_touserdata(L, idx);
            effect* obj = *static_cast<effect**>(payload);
            out->set_effect_handle(he.assign(obj));
            return true;
        }
        case UpvalueKind::GROUP: {
            void* payload = lua_touserdata(L, idx);
            group* obj = *static_cast<group**>(payload);
            out->set_group_handle(hg.assign(obj));
            return true;
        }
        case UpvalueKind::UNKNOWN:
            return false;
    }
    return false;
}

// lua_dump writer callback — appends bytes to the std::string in `ud`.
int dump_writer(lua_State* /*L*/, const void* p, size_t sz, void* ud) {
    static_cast<std::string*>(ud)->append(static_cast<const char*>(p), sz);
    return 0;
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
        default:
            return UpvalueKind::UNKNOWN;
    }
}

// Detects the canonical `local s,id=GetID()` pattern: is this upvalue
// the script's own _G["c<owning_card_code>"] table? Returns 0 if not,
// or the konami code if yes (which identifies which _G[...] table to
// re-resolve on load).
//
// Only meaningful when the upvalue is a table AND the owning card has
// a non-zero data.code (vanilla cards have code 0 and no script).
uint32_t detect_script_self_table(lua_State* L, int idx,
                                   uint32_t owning_card_code) {
    if (owning_card_code == 0) return 0;
    if (lua_type(L, idx) != LUA_TTABLE) return 0;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "c%u", owning_card_code);
    lua_getglobal(L, buf);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    // lua_rawequal compares without invoking metamethods. The script's
    // _G["c<code>"] table is unique-by-identity per duel.
    const int normalized = idx > 0 ? idx : idx - 1;  // adjust for our push
    const bool same = lua_rawequal(L, normalized, -1) != 0;
    lua_pop(L, 1);
    return same ? owning_card_code : 0;
}

OCG_SaveStatus dump_lua_callback(lua_State* L, int32_t lua_ref, const duel& d,
                                  HandleTable<card>& hc,
                                  HandleTable<effect>& he,
                                  HandleTable<group>& hg,
                                  uint32_t card_konami_id,
                                  const char* slot_name,
                                  ocg::state::LuaCallback* out,
                                  std::string* refuse_reason) {
    out->set_present(false);
    if (lua_ref == 0) {
        // No callback set on this slot. Mark absent and return OK.
        return OCG_SAVE_OK;
    }

    luaL_checkstack(L, 4, nullptr);
    const int top_before = lua_gettop(L);

    // Push the function onto the stack from the registry.
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
    if (!lua_isfunction(L, -1)) {
        // Engine sometimes stores non-function references in effect
        // callback slots. Examples: Effect.SetValue can take a literal
        // integer (then `value` is the int, not a Lua-ref); SetLabel
        // stores integers via SetLabelObject. In these cases the slot
        // doesn't have a callable Lua function — there's nothing to
        // dump. Treat as "no callback present" (which is correct: load
        // restores effect.<slot> from the int32 *_ref field, and if it
        // pointed at a non-function on save it'll point at the same
        // non-function value on load).
        //
        // Note: this means the saved effect's int32 *_ref fields ARE
        // load-relevant for these cases. The chunk-3 walk already saves
        // them as effect_record.{condition,cost,...}_ref. Load-side
        // load_effect_record_scalars doesn't currently restore those —
        // see TODO in pass 4 of deserialize_duel for chunk-6 follow-up.
        lua_settop(L, top_before);
        out->set_present(false);
        return OCG_SAVE_OK;
    }

    // Get upvalue count via lua_getinfo with ">u" (consumes the function).
    lua_Debug ar;
    std::memset(&ar, 0, sizeof(ar));
    lua_pushvalue(L, -1);  // duplicate so we still have the function for dump
    if (!lua_getinfo(L, ">u", &ar)) {
        lua_settop(L, top_before);
        if (refuse_reason) {
            *refuse_reason = "lua_getinfo failed for callback ref " +
                             std::to_string(lua_ref);
        }
        return OCG_SAVE_ERR_INTERNAL;
    }
    const int nups = ar.nups;

    // Walk + capture each upvalue. The function is at stack top.
    //
    // Lua functions have an implicit _ENV upvalue (the script's
    // environment table) at one of their upvalue slots — typically slot
    // 1 for top-level functions, but inner closures may inherit it at
    // various positions. _ENV is a table; classify_upvalue returns
    // UNKNOWN for tables, which would refuse virtually any callback.
    //
    // Mechanism: lua_getupvalue returns the upvalue's NAME as its return
    // value. For _ENV the name is "_ENV". We emit an empty CapturedArg
    // (oneof unset = VALUE_NOT_SET) at that slot position. On the load
    // side, restore_lua_callback skips lua_setupvalue for any empty
    // CapturedArg — luaL_loadbuffer's freshly-loaded function already
    // has its _ENV pointing at the load-side _G, which is exactly what
    // we want. (Save preserves positional alignment, so non-_ENV
    // upvalues end up at the right slots.)
    for (int i = 1; i <= nups; ++i) {
        const char* upname = lua_getupvalue(L, -1, i);
        if (!upname) {
            lua_settop(L, top_before);
            if (refuse_reason) {
                *refuse_reason = "lua_getupvalue(" + std::to_string(i) +
                                 ") returned NULL despite nups=" +
                                 std::to_string(nups);
            }
            return OCG_SAVE_ERR_INTERNAL;
        }
        const int up_idx = lua_gettop(L);
        // Always emit a CapturedArg for positional alignment with
        // load-side lua_setupvalue indexes. _ENV gets an empty
        // (VALUE_NOT_SET) marker; load skips lua_setupvalue for those.
        ocg::state::CapturedArg* arg = out->add_upvalues();
        const bool is_env = (std::strcmp(upname, "_ENV") == 0);
        if (is_env) {
            // Emit empty arg; load side preserves the default _ENV.
            lua_pop(L, 1);
            continue;
        }
        UpvalueKind kind = classify_upvalue(L, up_idx, d);
        // Special-case: `local s = GetID()` table from ProjectIgnis
        // scripts. If this is a table that matches _G["c<owner>"], emit
        // a script_self_card_code marker — load resolves it back via
        // _G[...] which exists post-card-script-load.
        if (kind == UpvalueKind::UNKNOWN &&
            lua_type(L, up_idx) == LUA_TTABLE) {
            const uint32_t self_code =
                detect_script_self_table(L, up_idx, card_konami_id);
            if (self_code != 0) {
                arg->set_script_self_card_code(self_code);
                lua_pop(L, 1);
                continue;
            }
        }
        if (kind == UpvalueKind::UNKNOWN) {
            if (refuse_reason) {
                *refuse_reason = format_refuse_reason(L, up_idx,
                                                       card_konami_id,
                                                       slot_name, i);
            }
            lua_settop(L, top_before);
            return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
        }
        if (!capture_known_upvalue(L, up_idx, kind, hc, he, hg, arg)) {
            lua_settop(L, top_before);
            if (refuse_reason) {
                *refuse_reason = "capture_known_upvalue failed for upvalue " +
                                 std::to_string(i);
            }
            return OCG_SAVE_ERR_INTERNAL;
        }
        lua_pop(L, 1);  // pop the upvalue
    }

    // Now dump the function's bytecode. lua_dump consumes the function.
    std::string bytecode;
    const int dump_result = lua_dump(L, &dump_writer, &bytecode, /*strip=*/0);
    if (dump_result != 0) {
        lua_settop(L, top_before);
        if (refuse_reason) {
            *refuse_reason = "lua_dump returned " +
                             std::to_string(dump_result);
        }
        return OCG_SAVE_ERR_INTERNAL;
    }

    out->set_present(true);
    out->set_bytecode(std::move(bytecode));
    lua_settop(L, top_before);
    return OCG_SAVE_OK;
}

int32_t restore_lua_callback(lua_State* L,
                              const ocg::state::LuaCallback& saved,
                              HandleResolver<card>& hc,
                              HandleResolver<effect>& he,
                              HandleResolver<group>& hg,
                              std::string* load_error) {
    if (!saved.present()) {
        return 0;  // No callback in this slot — return ref 0.
    }
    if (saved.bytecode().empty()) {
        if (load_error) *load_error = "LuaCallback present=true but bytecode empty";
        return 0;
    }

    luaL_checkstack(L, 4, nullptr);
    const int top_before = lua_gettop(L);

    // Load the function from bytecode. luaL_loadbuffer pushes a function
    // onto the stack on success.
    const int load_result = luaL_loadbuffer(L,
                                             saved.bytecode().data(),
                                             saved.bytecode().size(),
                                             "@reconstructed_callback");
    if (load_result != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        if (load_error) {
            *load_error = std::string("luaL_loadbuffer failed: ") +
                          (msg ? msg : "(no error message)");
        }
        lua_settop(L, top_before);
        return 0;
    }

    // Set each upvalue. The function is at stack top.
    //
    // VALUE_NOT_SET marks "preserve default" — used for _ENV upvalues
    // (see save side comment). luaL_loadbuffer already set _ENV to
    // load-side _G; skipping lua_setupvalue keeps that.
    for (int i = 0; i < saved.upvalues_size(); ++i) {
        const int up_idx = i + 1;  // Lua API is 1-indexed
        const ocg::state::CapturedArg& arg = saved.upvalues(i);

        if (arg.value_case() == ocg::state::CapturedArg::VALUE_NOT_SET) {
            // _ENV or other "preserve default" marker. Skip without
            // pushing/setting anything.
            continue;
        }

        // Push the value onto the stack per its type.
        switch (arg.value_case()) {
            case ocg::state::CapturedArg::kB:
                lua_pushboolean(L, arg.b() ? 1 : 0);
                break;
            case ocg::state::CapturedArg::kI:
                lua_pushinteger(L, static_cast<lua_Integer>(arg.i()));
                break;
            case ocg::state::CapturedArg::kD:
                lua_pushnumber(L, arg.d());
                break;
            case ocg::state::CapturedArg::kS:
                lua_pushlstring(L, arg.s().data(), arg.s().size());
                break;
            case ocg::state::CapturedArg::kCardHandle: {
                card* c = hc.lookup(arg.card_handle());
                if (c == nullptr) lua_pushnil(L);
                else interpreter::pushobject(L, c->ref_handle);
                break;
            }
            case ocg::state::CapturedArg::kEffectHandle: {
                effect* e = he.lookup(arg.effect_handle());
                if (e == nullptr) lua_pushnil(L);
                else interpreter::pushobject(L, e->ref_handle);
                break;
            }
            case ocg::state::CapturedArg::kGroupHandle: {
                group* g = hg.lookup(arg.group_handle());
                if (g == nullptr) lua_pushnil(L);
                else interpreter::pushobject(L, g->ref_handle);
                break;
            }
            case ocg::state::CapturedArg::kScriptSelfCardCode: {
                // Look up _G["c<code>"] — the script's class table,
                // already populated by the engine's load_card_script
                // path when the owning card was re-allocated in pass 1.
                char buf[32];
                std::snprintf(buf, sizeof(buf), "c%u",
                              arg.script_self_card_code());
                lua_getglobal(L, buf);
                if (lua_isnil(L, -1)) {
                    if (load_error) {
                        *load_error = std::string("script_self table _G[\"") +
                                      buf + "\"] not loaded — card script "
                                      "should have been loaded in pass 1";
                    }
                    lua_settop(L, top_before);
                    return 0;
                }
                break;
            }
            case ocg::state::CapturedArg::VALUE_NOT_SET:
                // unreachable — handled above
                break;
        }

        // Set as upvalue. lua_setupvalue pops the value and returns the
        // upvalue's name (NULL on failure / out-of-range).
        const char* upname = lua_setupvalue(L, -2, up_idx);
        if (upname == nullptr) {
            // Out of range or function has fewer upvalues than saved.
            if (load_error) {
                *load_error = "lua_setupvalue(" + std::to_string(up_idx) +
                              ") returned NULL — upvalue count mismatch";
            }
            lua_settop(L, top_before);
            return 0;
        }
    }

    // Register the function in the Lua registry and return the ref.
    const int32_t new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // luaL_ref pops the function from the stack.
    return new_ref;
}

}  // namespace ocg::serialize
