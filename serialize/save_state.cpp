#include "save_state.h"

#include "handle_table.h"
#include "lua_callback.h"
#include "ocg_state.pb.h"
#include "refuse_detect.h"

#include "../card.h"
#include "../duel.h"
#include "../effect.h"
#include "../field.h"
#include "../group.h"
#include "../interpreter.h"
// Chunk 9a Tier 1: ProcessorState walk needs the variant + Processor::*
// type definitions for std::visit dispatch.
#include "../processor_unit.h"

#include <algorithm>
#include <type_traits>
#include <typeinfo>
#include <variant>
#include <vector>

namespace ocg::serialize {

namespace pb = ::ocg::state;

// ---------------------------------------------------------------------------
// Helpers — write each engine subtree into its protobuf counterpart.
// Cross-references resolved through the three handle tables.
// ---------------------------------------------------------------------------

namespace {

// B1 fix (Option A): liveness guard for save-side effect handle assignment.
// `card_state::reason_effect`, `chain::triggering_effect`, and
// `chain::disable_reason` are plain `effect*` fields with no lifecycle
// management — `duel::delete_effect` frees the effect but never clears
// these references. Routing a freed pointer through `he.assign` adds it
// to the effect handle table, and the effect-record write loop then
// dereferences freed memory at save_state.cpp:260. Returning 0 (null
// handle) for non-live effects keeps the freed pointer out of the
// handle table.
inline uint32_t assign_effect_if_live(
        HandleTable<effect>& he, effect* peff,
        const std::unordered_set<effect*>& live_effects) {
    if (peff == nullptr) return 0;
    if (live_effects.count(peff) == 0) return 0;
    return he.assign(peff);
}

void write_card_state(const card_state& src, pb::CardStateSnapshot* dst,
                      HandleTable<card>& hc, HandleTable<effect>& he,
                      const std::unordered_set<effect*>& live_effects) {
    dst->set_code(src.code);
    dst->set_code2(src.code2);
    for (uint16_t sc : src.setcodes) dst->add_setcodes(sc);
    dst->set_type(src.type);
    dst->set_level(src.level);
    dst->set_rank(src.rank);
    dst->set_link(src.link);
    dst->set_link_marker(src.link_marker);
    dst->set_lscale(src.lscale);
    dst->set_rscale(src.rscale);
    dst->set_attribute(src.attribute);
    dst->set_race(src.race);
    dst->set_attack(src.attack);
    dst->set_defense(src.defense);
    dst->set_base_attack(src.base_attack);
    dst->set_base_defense(src.base_defense);
    dst->set_controler(src.controler);
    dst->set_location(src.location);
    dst->set_sequence(src.sequence);
    dst->set_position(src.position);
    dst->set_reason(src.reason);
    dst->set_pzone(src.pzone);
    dst->set_reason_card_handle(hc.assign(src.reason_card));
    dst->set_reason_effect_handle(
        assign_effect_if_live(he, src.reason_effect, live_effects));
    dst->set_reason_player(src.reason_player);
}

void write_field_info(const field_info& src, pb::FieldInfo* dst) {
    dst->set_event_id(src.event_id);
    dst->set_field_id(src.field_id);
    dst->set_copy_id(src.copy_id);
    dst->set_turn_id(src.turn_id);
    dst->set_turn_id_player_0(src.turn_id_by_player[0]);
    dst->set_turn_id_player_1(src.turn_id_by_player[1]);
    dst->set_card_id(src.card_id);
    dst->set_phase(src.phase);
    dst->set_turn_player(src.turn_player);
    dst->set_priority_player_0(src.priorities[0]);
    dst->set_priority_player_1(src.priorities[1]);
    dst->set_can_shuffle(src.can_shuffle);
}

void write_player_state(const player_info& src, pb::PlayerState* dst,
                        HandleTable<card>& hc) {
    dst->set_lp(src.lp);
    dst->set_start_lp(src.start_lp);
    dst->set_start_count(src.start_count);
    dst->set_draw_count(src.draw_count);
    dst->set_used_location(src.used_location);
    dst->set_disabled_location(src.disabled_location);
    dst->set_extra_p_count(src.extra_p_count);
    dst->set_exchanges(src.exchanges);
    dst->set_tag_index(src.tag_index);
    dst->set_recharge(src.recharge);
    for (card* c : src.list_mzone) dst->add_list_mzone(hc.assign(c));
    for (card* c : src.list_szone) dst->add_list_szone(hc.assign(c));
    for (card* c : src.list_main) dst->add_list_main(hc.assign(c));
    for (card* c : src.list_grave) dst->add_list_grave(hc.assign(c));
    for (card* c : src.list_hand) dst->add_list_hand(hc.assign(c));
    for (card* c : src.list_remove) dst->add_list_remove(hc.assign(c));
    for (card* c : src.list_extra) dst->add_list_extra(hc.assign(c));
}

void write_effect_refs(const card::effect_container& src,
                       google::protobuf::RepeatedPtrField<pb::EffectRef>* dst,
                       HandleTable<effect>& he) {
    // multimap iteration is ordered by key, but for equal keys order
    // is insertion-order-defined for multimap (deterministic).
    for (const auto& kv : src) {
        auto* er = dst->Add();
        er->set_key(kv.first);
        er->set_effect_handle(he.assign(kv.second));
    }
}

void write_card_record(const card& src, pb::CardRecord* dst,
                       HandleTable<card>& hc, HandleTable<effect>& he,
                       const std::unordered_set<effect*>& live_effects) {
    write_card_state(src.current, dst->mutable_current(), hc, he,
                     live_effects);
    write_card_state(src.previous, dst->mutable_previous(), hc, he,
                     live_effects);
    write_card_state(src.temp, dst->mutable_temp(), hc, he, live_effects);

    write_effect_refs(src.single_effect, dst->mutable_single_effect(), he);
    write_effect_refs(src.field_effect, dst->mutable_field_effect(), he);
    write_effect_refs(src.equip_effect, dst->mutable_equip_effect(), he);
    write_effect_refs(src.target_effect, dst->mutable_target_effect(), he);
    write_effect_refs(src.xmaterial_effect, dst->mutable_xmaterial_effect(), he);

    // counter_map = std::map<uint16, array<uint16,2>> — ordered iteration.
    for (const auto& kv : src.counters) {
        auto* ct = dst->add_counters();
        ct->set_counter_id(kv.first);
        ct->set_count_resettable(kv.second[0]);
        ct->set_count_unresettable(kv.second[1]);
    }

    dst->set_equip_target_handle(hc.assign(src.equiping_target));
    dst->set_pre_equip_target_handle(hc.assign(src.pre_equip_target));
    dst->set_overlay_target_handle(hc.assign(src.overlay_target));
    dst->set_pre_overlay_target_handle(hc.assign(src.pre_overlay_target));
    // equiping_cards / xyz_materials → repeated handles. Sort by cardid
    // for stable output (card_set is unordered_set).
    {
        std::vector<card*> v(src.equiping_cards.begin(), src.equiping_cards.end());
        std::sort(v.begin(), v.end(), [](card* a, card* b) {
            return a->cardid < b->cardid;
        });
        for (card* c : v) dst->add_equip_cards(hc.assign(c));
    }
    for (card* c : src.xyz_materials) dst->add_overlay_cards(hc.assign(c));
    // relations: unordered_map<card*, uint32_t> — sort by cardid.
    {
        std::vector<std::pair<card*, uint32_t>> v(src.relations.begin(),
                                                   src.relations.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first->cardid < b.first->cardid;
        });
        for (const auto& kv : v) {
            auto* r = dst->add_relations();
            r->set_card_handle(hc.assign(kv.first));
            r->set_relation_flags(kv.second);
        }
    }

    // Chunk 5a scalars beyond card_state.
    dst->set_owner(src.owner);
    dst->set_cardid(src.cardid);
    dst->set_fieldid(src.fieldid);
    dst->set_fieldid_r(src.fieldid_r);
    dst->set_turnid(src.turnid);
    dst->set_turn_counter(src.turn_counter);
    dst->set_status(src.status);
    dst->set_cover(src.cover);
    dst->set_spsummon_code(src.spsummon_code);
    dst->set_data_code(src.data.code);  // canonical id; load passes to new_card

    // Chunk 5b: card_set fields for effect-targeting load-stability.
    // Sort each by cardid for stable serialization (card_set is
    // unordered_set in C++).
    auto sort_card_set = [](const card_set& s) {
        std::vector<card*> v(s.begin(), s.end());
        std::sort(v.begin(), v.end(), [](card* a, card* b) {
            return a->cardid < b->cardid;
        });
        return v;
    };
    for (card* c : sort_card_set(src.material_cards)) {
        dst->add_material_cards(hc.assign(c));
    }
    for (card* c : sort_card_set(src.effect_target_owner)) {
        dst->add_effect_target_owner(hc.assign(c));
    }
    for (card* c : sort_card_set(src.effect_target_cards)) {
        dst->add_effect_target_cards(hc.assign(c));
    }
}

// Returns OCG_SAVE_OK on success, an error status on failure (with
// refuse_reason filled). The Lua callback dumps may produce
// OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE per plan §13.4.
//
// Card-pointer guard: hc_assign_safe wraps hc.assign so that pointers
// not already in the zone-walk handle table return 0 instead of being
// assigned a new orphan handle. This catches the engine's temp_card
// (and any similar engine-internal cards) which would otherwise get
// handles that have no corresponding CardRecord in the saved blob,
// breaking load. Discovered via the chunk-5b 27-byte byte-equal gap
// diagnostic.
static uint32_t hc_assign_safe(HandleTable<card>& hc, card* ptr) {
    if (ptr == nullptr) return 0;
    if (!hc.contains(ptr)) return 0;
    return hc.assign(ptr);
}

OCG_SaveStatus write_effect_record(const effect& src, pb::EffectRecord* dst,
                                    const duel& d,
                                    LuaSaveContext& lua_ctx,
                                    std::string* refuse_reason) {
    HandleTable<card>& hc = lua_ctx.hc;
    HandleTable<effect>& he = lua_ctx.he;
    HandleTable<group>& hg = lua_ctx.hg;
    dst->set_count_limit(src.count_limit);
    dst->set_count_limit_max(src.count_limit_max);
    dst->set_count_flag(src.count_flag);
    dst->set_count_hopt_index(src.count_hopt_index);
    dst->set_effect_owner(src.effect_owner);
    dst->set_type(src.type);
    dst->set_copy_id(src.copy_id);
    dst->set_range(src.range);
    dst->set_s_range(src.s_range);
    dst->set_o_range(src.o_range);
    dst->set_reset_count(src.reset_count);
    dst->set_active_location(src.active_location);
    dst->set_active_sequence(src.active_sequence);
    dst->set_status(src.status);
    dst->set_code(src.code);
    dst->set_flag_lo(src.flag[0]);
    dst->set_flag_hi(src.flag[1]);
    dst->set_id(src.id);
    dst->set_initial_id(src.initial_id);
    dst->set_reset_flag(src.reset_flag);
    dst->set_count_code(src.count_code);
    dst->set_category(src.category);
    dst->set_hint_timing_lo(src.hint_timing[0]);
    dst->set_hint_timing_hi(src.hint_timing[1]);
    dst->set_card_type(src.card_type);
    dst->set_active_type(src.active_type);
    dst->set_label_object(src.label_object);
    // Chunk-5b fix: do NOT write the int32 *_ref fields. They were
    // chunk-3 schema placeholders capturing the engine's Lua registry
    // indexes — which are meaningless across save/load boundaries
    // (luaL_ref assigns fresh sequential values on the load side, so
    // the original scattered values would never re-appear). Wave 2's
    // LuaCallback bytecode dump is the actual callback transport; the
    // *_ref fields stay zero in the wire format and are ignored on load.
    // Ref: 27-byte byte-equal gap diagnostic, chunk-5b session.
    dst->set_owner_card_handle(hc_assign_safe(hc, src.owner));
    dst->set_handler_card_handle(hc_assign_safe(hc, src.handler));
    dst->set_active_handler_card_handle(hc_assign_safe(hc, src.active_handler));
    dst->set_description(src.description);
    for (auto v : src.label) dst->add_label(static_cast<int64_t>(v));

    // Chunk 5b Wave 2: Lua callback bytecode dump per plan §13.4.
    // The owning card's konami id provides context for refuse reasons.
    const uint32_t card_id = src.owner ? src.owner->data.code : 0;
    lua_State* L = d.lua ? d.lua->lua_state : nullptr;
    if (L == nullptr) {
        // No Lua VM at all (defensive). All callback slots stay
        // present=false. This is unusual but not an error.
        return OCG_SAVE_OK;
    }
    struct Slot { int32_t ref; const char* name; pb::LuaCallback* out; };
    Slot slots[] = {
        {src.condition, "condition", dst->mutable_condition_callback()},
        {src.cost,      "cost",      dst->mutable_cost_callback()},
        {src.target,    "target",    dst->mutable_target_callback()},
        {src.value,     "value",     dst->mutable_value_callback()},
        {src.operation, "operation", dst->mutable_operation_callback()},
    };
    for (const auto& s : slots) {
        const auto status = dump_lua_callback(L, s.ref, d, lua_ctx,
                                               card_id, s.name,
                                               s.out, refuse_reason);
        if (status != OCG_SAVE_OK) return status;
    }
    return OCG_SAVE_OK;
}

// Chain stack and processor-state writers are deliberately minimal in
// chunk 3 — at fresh OCG_CreateDuel time both are empty. The full walk
// lands when we have non-trivial fixtures (chunks 4-5 fold these in).
void write_chain_link(const chain& src, pb::ChainLink* dst,
                      HandleTable<card>& hc, HandleTable<effect>& he,
                      HandleTable<group>& hg,
                      const std::unordered_set<effect*>& live_effects) {
    write_card_state(src.triggering_state, dst->mutable_triggering_state(),
                     hc, he, live_effects);
    dst->set_chain_count(src.chain_count);
    dst->set_chain_id(src.chain_id);
    dst->set_triggering_player(src.triggering_player);
    dst->set_triggering_controler(src.triggering_controler);
    dst->set_triggering_position(src.triggering_position);
    dst->set_target_player(src.target_player);
    dst->set_disable_player(src.disable_player);
    dst->set_triggering_summon_location(src.triggering_summon_location);
    dst->set_triggering_summon_proc_complete(src.triggering_summon_proc_complete);
    dst->set_was_just_sent(src.was_just_sent);
    dst->set_triggering_location(src.triggering_location);
    dst->set_triggering_sequence(src.triggering_sequence);
    dst->set_triggering_status(src.triggering_status);
    dst->set_triggering_summon_type(src.triggering_summon_type);
    dst->set_replace_op(src.replace_op);
    dst->set_target_param(src.target_param);
    dst->set_flag(src.flag);
    dst->set_event_id(src.event_id);
    dst->set_triggering_effect_handle(
        assign_effect_if_live(he, src.triggering_effect, live_effects));
    dst->set_target_cards_group_handle(hg.assign(src.target_cards));
    dst->set_disable_reason_handle(
        assign_effect_if_live(he, src.disable_reason, live_effects));
    // opinfos / possibleopinfos / triggering_event left for chunk 4-5
    // when fixtures actually exercise them.
}

void write_chain(const field& f, pb::ChainStack* dst,
                 HandleTable<card>& hc, HandleTable<effect>& he,
                 HandleTable<group>& hg,
                 const std::unordered_set<effect*>& live_effects) {
    for (const auto& link : f.core.current_chain) {
        write_chain_link(link, dst->add_links(), hc, he, hg, live_effects);
    }
    // tpchain / ntpchain / select_chains: stub for chunk 3 (vanilla
    // duels never have these populated). Full walk in chunk 4-5.
}

// Chunk 9a Tier 1+2: ProcessorState save with type-gated walk.
// Walks both core.units (Tier 1) and core.subunits (Tier 2) into the
// ProcessorState message. Per-list, refuses with informative reason if
// any entry is a variant type not in the current tier's coverage.
//
// Tier coverage:
//   Tier 1 (units, in handler stack at decision boundaries):
//     Adjust / Turn / SelectIdleCmd / SelectPlace / IdleCommand /
//     PhaseEvent
//   Tier 2 (subunits, queued by effect-monster initial_effect):
//     SelfDestroy / SelfToGrave
//
// Same handler used for both lists since they're the same
// processor_unit variant — coverage tier is enforced per-list by
// which handler-cases match.
//
// Returns OCG_SAVE_OK on success, REFUSE_UNKNOWN_UPVALUE_TYPE on a
// non-covered variant.
OCG_SaveStatus write_processor_unit_inner(
        const processor_unit& u,
        pb::ProcessorUnit* dst_unit,
        HandleTable<card>& hc,
        const char* list_name,
        int unit_idx,
        std::string* refuse_reason) {
    return std::visit(
        [&](const auto& v) -> OCG_SaveStatus {
            using T = std::decay_t<decltype(v)>;
            // ── Tier 1 variants ──────────────────────────────────
            if constexpr (std::is_same_v<T, Processors::Adjust>) {
                auto* m = dst_unit->mutable_adjust();
                m->set_step(v.step);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::Turn>) {
                auto* m = dst_unit->mutable_turn();
                m->set_step(v.step);
                m->set_turn_player(v.turn_player);
                m->set_has_performed_second_battle_phase(
                    v.has_performed_second_battle_phase);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectIdleCmd>) {
                auto* m = dst_unit->mutable_select_idle_cmd();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectPlace>) {
                auto* m = dst_unit->mutable_select_place();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_count(v.count);
                m->set_flag(v.flag);
                m->set_disable_field(v.disable_field);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::IdleCommand>) {
                auto* m = dst_unit->mutable_idle_command();
                m->set_step(v.step);
                m->set_phase_to_change_to(v.phase_to_change_to);
                m->set_card_to_reposition_handle(
                    hc_assign_safe(hc, v.card_to_reposition));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::PhaseEvent>) {
                auto* m = dst_unit->mutable_phase_event();
                m->set_step(v.step);
                m->set_phase(v.phase);
                m->set_is_opponent(v.is_opponent);
                m->set_priority_passed(v.priority_passed);
                return OCG_SAVE_OK;
            // ── Tier 2 variants ──────────────────────────────────
            } else if constexpr (std::is_same_v<T, Processors::SelfDestroy>) {
                auto* m = dst_unit->mutable_self_destroy();
                m->set_step(v.step);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelfToGrave>) {
                auto* m = dst_unit->mutable_self_to_grave();
                m->set_step(v.step);
                return OCG_SAVE_OK;
            } else {
                if (refuse_reason) {
                    char buf[240];
                    std::snprintf(buf, sizeof(buf),
                        "chunk-9a Tier 1+2 stub: processor unit in '%s' "
                        "at index %d is type '%s' (not in current tier "
                        "coverage). Tier 3 (or wider Tier 2 if this is "
                        "the next dominant variant) lifts this.",
                        list_name, unit_idx, typeid(T).name());
                    *refuse_reason = buf;
                }
                return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
            }
        }, u);
}

OCG_SaveStatus write_processor(const processor& core, pb::ProcessorState* dst,
                                HandleTable<card>& hc,
                                HandleTable<effect>& /*he*/,
                                HandleTable<group>& /*hg*/,
                                std::string* refuse_reason) {
    int idx = 0;
    for (const auto& u : core.units) {
        OCG_SaveStatus s = write_processor_unit_inner(
            u, dst->add_units(), hc, "units", idx++, refuse_reason);
        if (s != OCG_SAVE_OK) return s;
    }
    idx = 0;
    for (const auto& u : core.subunits) {
        OCG_SaveStatus s = write_processor_unit_inner(
            u, dst->add_subunits(), hc, "subunits", idx++, refuse_reason);
        if (s != OCG_SAVE_OK) return s;
    }
    return OCG_SAVE_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Top-level entry point
// ---------------------------------------------------------------------------

OCG_SaveStatus serialize_duel(const duel& d, std::string* out,
                              std::string* refuse_reason) {
    out->clear();
    refuse_reason->clear();

    // 1. Refuse path
    const RefuseReason reason = detect_refuse(d);
    if (reason != RefuseReason::NONE) {
        *refuse_reason = refuse_reason_str(reason);
        switch (reason) {
            case RefuseReason::NOT_MSG_BOUNDARY:
                return OCG_SAVE_ERR_NOT_MSG_BOUNDARY;
            case RefuseReason::UNSAFE_LUA_CLOSURE:
                return OCG_SAVE_ERR_REFUSE_UNSAFE_LUA;
            default:
                return OCG_SAVE_ERR_INTERNAL;
        }
    }

    // 1b. Fail-loud on remaining stubs (per chunk-4 user decision:
    // "fail-loud is cheaper than remembering"). Subtrees the current
    // walks don't handle refuse explicitly so the first fixture that
    // hits one errors clearly instead of silently dropping state.
    //
    // Lifted in chunk 5a: cards and current_chain (chunk 3 already wrote
    // current_chain, chunk 5a adds load + extends save fields). Lifted
    // in chunk 5b: effects, groups, lua.closures.
    //
    // Still stubbed and refused: ProcessorState (units/subunits), pending
    // chain lists (tpchain/ntpchain/select_chains), full effect+group
    // walks. These land in chunk 5b alongside the Lua closure work.
    // Chunk 9a Tier 2 lifted the subunits guard — write_processor
    // below handles both core.units (Tier 1 variants) and core.subunits
    // (Tier 2 variants), refusing on out-of-coverage types per-list.
    // Still refusing on chain_lists (tpchain/ntpchain/select_chains) —
    // §15 characterization saw zero population in the corpus, so
    // lifting without a characterization pass would be speculative
    // implement.
    if (d.game_field) {
        const auto& core = d.game_field->core;
        if (!core.tpchain.empty() || !core.ntpchain.empty() ||
            !core.select_chains.empty()) {
            *refuse_reason =
                "chunk-9a Tier 2 stub: tpchain / ntpchain / "
                "select_chains are not yet wired into the save path. "
                "§15 characterization observed zero population at "
                "single-card-add scenarios; first fixture that hits "
                "this drives a follow-up characterization pass.";
            return OCG_SAVE_ERR_INTERNAL;
        }
    }
    // Chunk 5b Wave 2: effects, groups, card.effect_container refs, and
    // chain links are wired. Lua callbacks dump per plan §13.4.
    // Chunk 9a Tier 1+2: core.units + core.subunits handled by
    // write_processor (Tier-coverage variants only). Tier 3 (if
    // chosen) expands variant coverage further; chain_lists wait for
    // a separate characterization pass.

    // 2. Build handle tables in deterministic order.
    HandleTable<card> ht_cards;
    HandleTable<effect> ht_effects;
    HandleTable<group> ht_groups;

    // Skip the engine's internal scratch slot (field.temp_card) — it's
    // an implementation detail of the field constructor, not user state.
    // Including it would create a cross-side load problem: the freshly-
    // constructed duel on the load side already has its own temp_card
    // pointer, and the saved CardRecord can't be bound to that engine-
    // managed slot without reaching into field internals.
    card* const temp_card = (d.game_field != nullptr) ? d.game_field->temp_card
                                                       : nullptr;

    auto assign_zone = [&](const card_vector& zone) {
        for (card* c : zone) {
            if (c && c != temp_card) ht_cards.assign(c);
        }
    };

    if (d.game_field) {
        for (int p = 0; p < 2; ++p) {
            const auto& pi = d.game_field->player[p];
            assign_zone(pi.list_main);
            assign_zone(pi.list_hand);
            assign_zone(pi.list_mzone);
            assign_zone(pi.list_szone);
            assign_zone(pi.list_grave);
            assign_zone(pi.list_remove);
            assign_zone(pi.list_extra);
        }
    }

    // Pick up cards not in any zone (xyz materials sometimes orphan
    // mid-summon; defensive sweep). Sort by cardid for stable order.
    {
        std::vector<card*> orphans;
        orphans.reserve(d.cards.size());
        for (card* c : d.cards) {
            if (c && c != temp_card && !ht_cards.contains(c)) {
                orphans.push_back(c);
            }
        }
        std::sort(orphans.begin(), orphans.end(),
                  [](card* a, card* b) { return a->cardid < b->cardid; });
        for (card* c : orphans) ht_cards.assign(c);
    }

    // Effects: walk each card's containers in card-handle order, then
    // any remaining (field-scope) effects sorted by initial_id.
    {
        const auto cards_in_order = ht_cards.in_handle_order();
        for (card* c : cards_in_order) {
            for (const auto& kv : c->single_effect)    ht_effects.assign(kv.second);
            for (const auto& kv : c->field_effect)     ht_effects.assign(kv.second);
            for (const auto& kv : c->equip_effect)     ht_effects.assign(kv.second);
            for (const auto& kv : c->target_effect)    ht_effects.assign(kv.second);
            for (const auto& kv : c->xmaterial_effect) ht_effects.assign(kv.second);
        }
        std::vector<effect*> orphans;
        for (effect* e : d.effects) {
            if (e && !ht_effects.contains(e)) orphans.push_back(e);
        }
        std::sort(orphans.begin(), orphans.end(),
                  [](effect* a, effect* b) { return a->initial_id < b->initial_id; });
        for (effect* e : orphans) ht_effects.assign(e);
    }

    // Groups: sort by content-derived key (lex on sorted cardid sequence).
    {
        std::vector<group*> all(d.groups.begin(), d.groups.end());
        std::sort(all.begin(), all.end(), [](group* a, group* b) {
            std::vector<uint32_t> ka, kb;
            ka.reserve(a->container.size());
            kb.reserve(b->container.size());
            for (card* c : a->container) if (c) ka.push_back(c->cardid);
            for (card* c : b->container) if (c) kb.push_back(c->cardid);
            std::sort(ka.begin(), ka.end());
            std::sort(kb.begin(), kb.end());
            return ka < kb;
        });
        for (group* g : all) ht_groups.assign(g);
    }

    // 3. Build the protobuf message
    pb::DuelState state;
    state.set_schema_version(kSchemaVersion);
    state.set_engine_build_hash(0);  // chunk 7 wires real ocgcore SHA
    state.set_timestamp_unix(0);     // intentional: keeps blob deterministic
    state.set_save_safety(pb::DuelState::SAVE_SAFETY_OK);

    {
        const auto& s = d.get_rng().get_state();
        auto* rng = state.mutable_rng();
        for (uint64_t v : s) rng->add_xoshiro_state(v);
    }

    if (d.game_field) {
        write_field_info(d.game_field->infos, state.mutable_field_info());
        for (int p = 0; p < 2; ++p) {
            write_player_state(d.game_field->player[p], state.add_players(),
                               ht_cards);
        }
        write_chain(*d.game_field, state.mutable_chain(),
                    ht_cards, ht_effects, ht_groups, d.effects);
        // Chunk 9a Tier 1: write_processor can refuse on non-Tier-1
        // unit types. Surface the refuse with the format the corpus
        // measurement expects.
        OCG_SaveStatus pstatus = write_processor(
            d.game_field->core, state.mutable_processor(),
            ht_cards, ht_effects, ht_groups, refuse_reason);
        if (pstatus != OCG_SAVE_OK) return pstatus;
    }

    for (card* c : ht_cards.in_handle_order()) {
        auto* cr = state.add_cards();
        cr->set_handle(ht_cards.assign(c));
        write_card_record(*c, cr, ht_cards, ht_effects, d.effects);
    }

    // Chunk 5c: thread a LuaSaveContext through write_effect_record. The
    // context carries the per-card table-pointer registry that preserves
    // intra-card table-upvalue identity (5c.0 measured 22.5% sharing
    // incidence). Reset between owners. Effects from one card are
    // contiguous in handle order (the assignment loop above walks
    // card-by-card), so owner-change detection is sufficient to scope
    // the registry correctly.
    LuaSaveContext lua_ctx{ht_cards, ht_effects, ht_groups, {}, 1, 6, 0};
    card* prev_owner = nullptr;
    for (effect* e : ht_effects.in_handle_order()) {
        if (e && e->owner != prev_owner) {
            lua_ctx.reset_per_card();
            prev_owner = e->owner;
        }
        auto* er = state.add_effects();
        er->set_handle(ht_effects.assign(e));
        const auto eff_status = write_effect_record(*e, er, d, lua_ctx,
                                                     refuse_reason);
        if (eff_status != OCG_SAVE_OK) {
            // Lua dump can refuse with REFUSE_UNKNOWN_UPVALUE_TYPE per
            // plan §13.4. Surface it directly; the refuse_reason already
            // has the informative format from format_refuse_reason.
            return eff_status;
        }
    }

    for (group* g : ht_groups.in_handle_order()) {
        auto* gr = state.add_groups();
        gr->set_handle(ht_groups.assign(g));
        gr->set_is_readonly(g->is_readonly != 0);
        std::vector<card*> members(g->container.begin(), g->container.end());
        std::sort(members.begin(), members.end(), [](card* a, card* b) {
            if (!a || !b) return a < b;
            return a->cardid < b->cardid;
        });
        for (card* c : members) gr->add_card_handles(ht_cards.assign(c));
    }

    // Lua reconstruction is left empty in chunk 3 (no-Lua path).
    // mutable_lua() materializes the empty submessage so its presence is
    // explicit on the wire, which simplifies chunk-5 incremental landing.
    (void)state.mutable_lua();

    // 4. Serialize
    if (!state.SerializeToString(out)) {
        *refuse_reason = "protobuf SerializeToString failed";
        return OCG_SAVE_ERR_INTERNAL;
    }
    return OCG_SAVE_OK;
}

}  // namespace ocg::serialize
