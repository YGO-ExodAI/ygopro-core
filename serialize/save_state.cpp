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

// Chunk 9b: lift the select_chains save refuse. Each entry's
// triggering_effect is the only meaningful field for step==1
// validation (size check) and step==1 dispatch (selection by index).
// Apply the same B1 liveness guard as current_chain.
//
// tpchain / ntpchain are NOT lifted here — §15 characterization (eval
// + this run) saw zero corpus population. They stay refused at the
// pre-walk gate in serialize_duel.
void write_select_chains(const processor& core, pb::ChainStack* dst,
                          HandleTable<effect>& he,
                          const std::unordered_set<effect*>& live_effects) {
    for (const auto& ch : core.select_chains) {
        auto* p = dst->add_select_chains();
        p->set_chain_id(ch.chain_id);
        p->set_triggering_player(ch.triggering_player);
        p->set_triggering_effect_handle(
            assign_effect_if_live(he, ch.triggering_effect, live_effects));
    }
}

void write_chain(const field& f, pb::ChainStack* dst,
                 HandleTable<card>& hc, HandleTable<effect>& he,
                 HandleTable<group>& hg,
                 const std::unordered_set<effect*>& live_effects) {
    for (const auto& link : f.core.current_chain) {
        write_chain_link(link, dst->add_links(), hc, he, hg, live_effects);
    }
    // chunk 9b: lift select_chains save (tpchain/ntpchain still stubbed —
    // §15 saw zero corpus population, refuse stays at the pre-walk gate).
    write_select_chains(f.core, dst, he, live_effects);
}

// Chunk 9a Tier 1+2 / 9b Tier 3: ProcessorState save with type-gated walk.
// Walks both core.units (Tier 1) and core.subunits (Tier 2) into the
// ProcessorState message. Per-list, refuses with informative reason if
// any entry is a variant type not in the current tier's coverage.
//
// Tier coverage (post-9b):
//   Tier 1 (units, dominant at decision boundaries):
//     Adjust / Turn / SelectIdleCmd / SelectPlace / IdleCommand /
//     PhaseEvent
//   Tier 2 (subunits, queued by effect-monster initial_effect):
//     SelfDestroy / SelfToGrave
//   Tier 3 (chunk-9b, schema v2, every Process<true> variant):
//     SelectBattleCmd / SelectChain / SelectCard / SelectCardCodes /
//     SelectUnselectCard / SelectPosition / SelectTributeP /
//     SelectCounter / SelectSum / SortCard / SelectYesNo /
//     SelectEffectYesNo / SelectOption / AnnounceRace /
//     AnnounceAttribute / AnnounceCard / AnnounceNumber /
//     RockPaperScissors
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
        HandleTable<effect>& he,
        HandleTable<group>& hg,
        const std::unordered_set<effect*>& live_effects,
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
            // ── Tier 3 variants (chunk 9b) ──────────────────────
            } else if constexpr (std::is_same_v<T, Processors::SelectBattleCmd>) {
                auto* m = dst_unit->mutable_select_battle_cmd();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectChain>) {
                auto* m = dst_unit->mutable_select_chain();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_spe_count(v.spe_count);
                m->set_forced(v.forced);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectCard>) {
                auto* m = dst_unit->mutable_select_card();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_cancelable(v.cancelable);
                m->set_min(v.min);
                m->set_max(v.max);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectCardCodes>) {
                auto* m = dst_unit->mutable_select_card_codes();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_cancelable(v.cancelable);
                m->set_min(v.min);
                m->set_max(v.max);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectUnselectCard>) {
                auto* m = dst_unit->mutable_select_unselect_card();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_cancelable(v.cancelable);
                m->set_min(v.min);
                m->set_max(v.max);
                m->set_finishable(v.finishable);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectPosition>) {
                auto* m = dst_unit->mutable_select_position();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_code(v.code);
                m->set_positions(v.positions);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectTributeP>) {
                auto* m = dst_unit->mutable_select_tribute_p();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_cancelable(v.cancelable);
                m->set_min(v.min);
                m->set_max(v.max);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectCounter>) {
                auto* m = dst_unit->mutable_select_counter();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_countertype(v.countertype);
                m->set_count(v.count);
                m->set_self(v.self);
                m->set_oppo(v.oppo);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectSum>) {
                auto* m = dst_unit->mutable_select_sum();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_acc(v.acc);
                m->set_min(v.min);
                m->set_max(v.max);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SortCard>) {
                auto* m = dst_unit->mutable_sort_card();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_is_chain(v.is_chain);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectYesNo>) {
                auto* m = dst_unit->mutable_select_yes_no();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_description(v.description);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectEffectYesNo>) {
                auto* m = dst_unit->mutable_select_effect_yes_no();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                // pcard is the only pointer in any Tier 3 unit. Routed
                // through hc_assign_safe so cards not in the zone walk
                // (e.g. mid-summon temp_card edge cases) become 0
                // rather than orphan handles.
                m->set_pcard_handle(hc_assign_safe(hc, v.pcard));
                m->set_description(v.description);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectOption>) {
                auto* m = dst_unit->mutable_select_option();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::AnnounceRace>) {
                auto* m = dst_unit->mutable_announce_race();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_count(v.count);
                m->set_available(v.available);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::AnnounceAttribute>) {
                auto* m = dst_unit->mutable_announce_attribute();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_count(v.count);
                m->set_available(v.available);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::AnnounceCard>) {
                auto* m = dst_unit->mutable_announce_card();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::AnnounceNumber>) {
                auto* m = dst_unit->mutable_announce_number();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::RockPaperScissors>) {
                auto* m = dst_unit->mutable_rps();
                m->set_step(v.step);
                m->set_repeat(v.repeat);
                m->set_hand0(v.hand0);
                return OCG_SAVE_OK;
            // ── Tier 4 (chunk 9c): Process<false> non-Select variants ─────
            } else if constexpr (std::is_same_v<T, Processors::AddChain>) {
                auto* m = dst_unit->mutable_add_chain();
                m->set_step(v.step);
                m->set_is_activated_effect(v.is_activated_effect);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SolveChain>) {
                auto* m = dst_unit->mutable_solve_chain();
                m->set_step(v.step);
                m->set_skip_trigger(v.skip_trigger);
                m->set_skip_freechain(v.skip_freechain);
                m->set_skip_new(v.skip_new);
                m->set_backed_up_operation(v.backed_up_operation);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::PointEvent>) {
                auto* m = dst_unit->mutable_point_event();
                m->set_step(v.step);
                m->set_skip_trigger(v.skip_trigger);
                m->set_skip_freechain(v.skip_freechain);
                m->set_skip_new(v.skip_new);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::QuickEffect>) {
                auto* m = dst_unit->mutable_quick_effect();
                m->set_step(v.step);
                m->set_skip_freechain(v.skip_freechain);
                m->set_is_opponent(v.is_opponent);
                m->set_priority_player(v.priority_player);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ForcedBattle>) {
                auto* m = dst_unit->mutable_forced_battle();
                m->set_step(v.step);
                m->set_backup_phase(v.backup_phase);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SortChain>) {
                auto* m = dst_unit->mutable_sort_chain();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::AttackDisable>) {
                auto* m = dst_unit->mutable_attack_disable();
                m->set_step(v.step);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ActivateEffect>) {
                auto* m = dst_unit->mutable_activate_effect();
                m->set_step(v.step);
                m->set_peffect_handle(
                    assign_effect_if_live(he, v.peffect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SolveContinuous>) {
                auto* m = dst_unit->mutable_solve_continuous();
                m->set_step(v.step);
                m->set_reason_player(v.reason_player);
                m->set_reason_effect_handle(
                    assign_effect_if_live(he, v.reason_effect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ExecuteCost>) {
                auto* m = dst_unit->mutable_execute_cost();
                m->set_step(v.step);
                m->set_triggering_player(v.triggering_player);
                m->set_shuffle_check_was_disabled(v.shuffle_check_was_disabled);
                m->set_triggering_effect_handle(
                    assign_effect_if_live(he, v.triggering_effect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ExecuteOperation>) {
                auto* m = dst_unit->mutable_execute_operation();
                m->set_step(v.step);
                m->set_triggering_player(v.triggering_player);
                m->set_shuffle_check_was_disabled(v.shuffle_check_was_disabled);
                m->set_triggering_effect_handle(
                    assign_effect_if_live(he, v.triggering_effect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ExecuteTarget>) {
                auto* m = dst_unit->mutable_execute_target();
                m->set_step(v.step);
                m->set_triggering_player(v.triggering_player);
                m->set_shuffle_check_was_disabled(v.shuffle_check_was_disabled);
                m->set_triggering_effect_handle(
                    assign_effect_if_live(he, v.triggering_effect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::MoveToField>) {
                auto* m = dst_unit->mutable_move_to_field();
                m->set_step(v.step);
                m->set_enable(v.enable);
                m->set_ret(v.ret);
                m->set_pzone(v.pzone);
                m->set_zone(v.zone);
                m->set_rule(v.rule);
                m->set_location_reason(v.location_reason);
                m->set_confirm(v.confirm);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::BattleCommand>) {
                auto* m = dst_unit->mutable_battle_command();
                m->set_step(v.step);
                m->set_phase_to_change_to(v.phase_to_change_to);
                m->set_forced_attack(v.forced_attack);
                m->set_forced_attack_done(v.forced_attack_done);
                m->set_is_replaying_attack(v.is_replaying_attack);
                m->set_attack_announce_failed(v.attack_announce_failed);
                m->set_repeat_battle_phase(v.repeat_battle_phase);
                m->set_second_battle_phase_is_optional(
                    v.second_battle_phase_is_optional);
                m->set_previous_point_event_had_any_trigger_to_resolve(
                    v.previous_point_event_had_any_trigger_to_resolve);
                m->set_reason_player(v.reason_player);
                m->set_damage_change_effect_handle(
                    assign_effect_if_live(he, v.damage_change_effect, live_effects));
                m->set_cards_destroyed_by_battle_group_handle(
                    hg.assign(v.cards_destroyed_by_battle));
                m->set_reason_card_handle(hc_assign_safe(hc, v.reason_card));
                for (const auto& [peff, pcard] : v.must_attack_map) {
                    auto* entry = m->add_must_attack_map();
                    entry->set_effect_handle(
                        assign_effect_if_live(he, peff, live_effects));
                    entry->set_card_handle(hc_assign_safe(hc, pcard));
                }
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelfDestroyUnique>) {
                auto* m = dst_unit->mutable_self_destroy_unique();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_unique_card_handle(hc_assign_safe(hc, v.unique_card));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SelectDisField>) {
                auto* m = dst_unit->mutable_select_dis_field();
                m->set_step(v.step);
                m->set_playerid(v.playerid);
                m->set_count(v.count);
                m->set_flag(v.flag);
                m->set_disable_field(v.disable_field);
                return OCG_SAVE_OK;
            // ── Tier 5 (chunk 9d): SpellSet / SummonRule / MonsterSet clusters ──
            } else if constexpr (std::is_same_v<T, Processors::SpellSet>) {
                auto* m = dst_unit->mutable_spell_set();
                m->set_step(v.step);
                m->set_setplayer(v.setplayer);
                m->set_toplayer(v.toplayer);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                m->set_reason_effect_handle(
                    assign_effect_if_live(he, v.reason_effect, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SpellSetGroup>) {
                auto* m = dst_unit->mutable_spell_set_group();
                m->set_step(v.step);
                m->set_setplayer(v.setplayer);
                m->set_toplayer(v.toplayer);
                m->set_confirm(v.confirm);
                m->set_ptarget_group_handle(hg.assign(v.ptarget));
                m->set_reason_effect_handle(
                    assign_effect_if_live(he, v.reason_effect, live_effects));
                for (card* c : v.set_cards)
                    m->add_set_card_handles(hc_assign_safe(hc, c));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SummonRule>) {
                auto* m = dst_unit->mutable_summon_rule();
                m->set_step(v.step);
                m->set_sumplayer(v.sumplayer);
                m->set_min_tribute(v.min_tribute);
                m->set_max_allowed_tributes(v.max_allowed_tributes);
                m->set_ignore_count(v.ignore_count);
                m->set_zone(v.zone);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                m->set_summon_procedure_effect_handle(
                    assign_effect_if_live(he, v.summon_procedure_effect, live_effects));
                m->set_extra_summon_effect_handle(
                    assign_effect_if_live(he, v.extra_summon_effect, live_effects));
                for (card* c : v.tributes)
                    m->add_tribute_card_handles(hc_assign_safe(hc, c));
                for (effect* e : v.summon_cost_effects)
                    m->add_summon_cost_effect_handles(
                        assign_effect_if_live(he, e, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SpSummonRule>) {
                auto* m = dst_unit->mutable_sp_summon_rule();
                m->set_step(v.step);
                m->set_sumplayer(v.sumplayer);
                m->set_is_mid_chain(v.is_mid_chain);
                m->set_summon_type(v.summon_type);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                m->set_summon_proc_effect_handle(
                    assign_effect_if_live(he, v.summon_proc_effect, live_effects));
                m->set_cards_to_summon_group_handle(hg.assign(v.cards_to_summon_g));
                for (effect* e : v.spsummon_cost_effects)
                    m->add_spsummon_cost_effect_handles(
                        assign_effect_if_live(he, e, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SpSummonRuleGroup>) {
                auto* m = dst_unit->mutable_sp_summon_rule_group();
                m->set_step(v.step);
                m->set_sumplayer(v.sumplayer);
                m->set_summon_type(v.summon_type);
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::MonsterSet>) {
                auto* m = dst_unit->mutable_monster_set();
                m->set_step(v.step);
                m->set_setplayer(v.setplayer);
                m->set_min_tribute(v.min_tribute);
                m->set_max_allowed_tributes(v.max_allowed_tributes);
                m->set_ignore_count(v.ignore_count);
                m->set_zone(v.zone);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                m->set_summon_procedure_effect_handle(
                    assign_effect_if_live(he, v.summon_procedure_effect, live_effects));
                m->set_extra_summon_effect_handle(
                    assign_effect_if_live(he, v.extra_summon_effect, live_effects));
                for (card* c : v.tributes)
                    m->add_tribute_card_handles(hc_assign_safe(hc, c));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::FlipSummon>) {
                auto* m = dst_unit->mutable_flip_summon();
                m->set_step(v.step);
                m->set_sumplayer(v.sumplayer);
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                for (effect* e : v.flip_summon_cost_effects)
                    m->add_flip_summon_cost_effect_handles(
                        assign_effect_if_live(he, e, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SpSummon>) {
                auto* m = dst_unit->mutable_sp_summon();
                m->set_step(v.step);
                m->set_reason_player(v.reason_player);
                m->set_zone(v.zone);
                m->set_reason_effect_handle(
                    assign_effect_if_live(he, v.reason_effect, live_effects));
                m->set_targets_group_handle(hg.assign(v.targets));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::SpSummonStep>) {
                auto* m = dst_unit->mutable_sp_summon_step();
                m->set_step(v.step);
                m->set_zone(v.zone);
                m->set_targets_group_handle(hg.assign(v.targets));
                m->set_target_card_handle(hc_assign_safe(hc, v.target));
                for (effect* e : v.spsummon_cost_effects)
                    m->add_spsummon_cost_effect_handles(
                        assign_effect_if_live(he, e, live_effects));
                return OCG_SAVE_OK;
            } else if constexpr (std::is_same_v<T, Processors::ChangePos>) {
                auto* m = dst_unit->mutable_change_pos();
                m->set_step(v.step);
                m->set_reason_player(v.reason_player);
                m->set_enable(v.enable);
                m->set_oppo_selection(v.oppo_selection);
                m->set_reason_effect_handle(
                    assign_effect_if_live(he, v.reason_effect, live_effects));
                m->set_targets_group_handle(hg.assign(v.targets));
                for (card* c : v.to_grave_set)
                    m->add_to_grave_card_handles(hc_assign_safe(hc, c));
                return OCG_SAVE_OK;
            } else {
                if (refuse_reason) {
                    char buf[240];
                    std::snprintf(buf, sizeof(buf),
                        "chunk-9a/b stub: processor unit in '%s' "
                        "at index %d is type '%s' (not in current tier "
                        "coverage). The Process<false> non-Select "
                        "variants (BattleCommand, Destroy, …) wait on "
                        "their own characterization pass.",
                        list_name, unit_idx, typeid(T).name());
                    *refuse_reason = buf;
                }
                return OCG_SAVE_ERR_REFUSE_UNKNOWN_UPVALUE_TYPE;
            }
        }, u);
}

// Chunk 9b: write field.processor scratch state (the lists/flags
// validated at step==1 of every Select* handler). The card_vector
// members reference cards already in zone walks, so handle assignment
// always succeeds via the existing ht_cards. select_chains has the
// only effect-pointer hazard — routed through assign_effect_if_live to
// match the B1 UAF guard already applied to current_chain.
void write_processor_scratch(
        const processor& core, pb::ProcessorState* dst,
        HandleTable<card>& hc, HandleTable<effect>& he,
        const std::unordered_set<effect*>& live_effects) {
    auto write_card_vec = [&](const card_vector& src,
                               google::protobuf::RepeatedField<uint32_t>* out) {
        for (card* c : src) out->Add(hc_assign_safe(hc, c));
    };
    write_card_vec(core.summonable_cards,    dst->mutable_summonable_cards());
    write_card_vec(core.spsummonable_cards,  dst->mutable_spsummonable_cards());
    write_card_vec(core.repositionable_cards,dst->mutable_repositionable_cards());
    write_card_vec(core.msetable_cards,      dst->mutable_msetable_cards());
    write_card_vec(core.ssetable_cards,      dst->mutable_ssetable_cards());
    write_card_vec(core.attackable_cards,    dst->mutable_attackable_cards());
    write_card_vec(core.select_cards,        dst->mutable_select_cards());
    write_card_vec(core.unselect_cards,      dst->mutable_unselect_cards());
    write_card_vec(core.must_select_cards,   dst->mutable_must_select_cards());

    for (const auto& kv : core.select_cards_codes) {
        auto* p = dst->add_select_cards_codes();
        p->set_code(kv.first);
        p->set_info(kv.second);
    }

    for (uint64_t opt : core.select_options) dst->add_select_options(opt);

    // select_effects: effect pointers, B1-guarded.
    for (effect* e : core.select_effects) {
        dst->add_select_effects(assign_effect_if_live(he, e, live_effects));
    }

    dst->set_to_bp(core.to_bp);
    dst->set_to_m2(core.to_m2);
    dst->set_to_ep(core.to_ep);
    dst->set_skip_m2(core.skip_m2);
    dst->set_hint_timing_0(core.hint_timing[0]);
    dst->set_hint_timing_1(core.hint_timing[1]);
    dst->set_chain_attack(core.chain_attack);
    dst->set_chain_attacker_id(core.chain_attacker_id);
}

OCG_SaveStatus write_processor(const processor& core, pb::ProcessorState* dst,
                                HandleTable<card>& hc,
                                HandleTable<effect>& he,
                                HandleTable<group>& hg,
                                const std::unordered_set<effect*>& live_effects,
                                std::string* refuse_reason) {
    int idx = 0;
    for (const auto& u : core.units) {
        OCG_SaveStatus s = write_processor_unit_inner(
            u, dst->add_units(), hc, he, hg, live_effects,
            "units", idx++, refuse_reason);
        if (s != OCG_SAVE_OK) return s;
    }
    idx = 0;
    for (const auto& u : core.subunits) {
        OCG_SaveStatus s = write_processor_unit_inner(
            u, dst->add_subunits(), hc, he, hg, live_effects,
            "subunits", idx++, refuse_reason);
        if (s != OCG_SAVE_OK) return s;
    }

    // Chunk 9b: scratch-state lists/flags consumed at step==1 of every
    // Select* handler (playerop.cpp:18-870). Without these, every legal
    // response is rejected → MSG_RETRY → terminate.
    write_processor_scratch(core, dst, hc, he, live_effects);
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
        // chunk 9b: select_chains is now serialized via write_select_chains.
        // tpchain / ntpchain still refuse — §15 characterization (eval +
        // 2026-04-25 re-walk) saw zero corpus population at any decision
        // boundary in the YugiKaiba starter format. First fixture that
        // hits one of those drives a follow-up characterization pass.
        if (!core.tpchain.empty() || !core.ntpchain.empty()) {
            *refuse_reason =
                "chunk-9b stub: tpchain / ntpchain are not yet wired "
                "into the save path. §15 characterization observed zero "
                "population in the YugiKaiba corpus; first fixture that "
                "hits this drives a follow-up characterization pass.";
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
            ht_cards, ht_effects, ht_groups, d.effects, refuse_reason);
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
