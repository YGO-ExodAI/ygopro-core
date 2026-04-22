#include "save_state.h"

#include "handle_table.h"
#include "ocg_state.pb.h"
#include "refuse_detect.h"

#include "../card.h"
#include "../duel.h"
#include "../effect.h"
#include "../field.h"
#include "../group.h"
#include "../interpreter.h"

#include <algorithm>
#include <vector>

namespace ocg::serialize {

namespace pb = ::ocg::state;

// ---------------------------------------------------------------------------
// Helpers — write each engine subtree into its protobuf counterpart.
// Cross-references resolved through the three handle tables.
// ---------------------------------------------------------------------------

namespace {

void write_card_state(const card_state& src, pb::CardStateSnapshot* dst,
                      HandleTable<card>& hc, HandleTable<effect>& he) {
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
    dst->set_reason_effect_handle(he.assign(src.reason_effect));
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
                       HandleTable<card>& hc, HandleTable<effect>& he) {
    write_card_state(src.current, dst->mutable_current(), hc, he);
    write_card_state(src.previous, dst->mutable_previous(), hc, he);
    write_card_state(src.temp, dst->mutable_temp(), hc, he);

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
}

void write_effect_record(const effect& src, pb::EffectRecord* dst,
                         HandleTable<card>& hc) {
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
    dst->set_condition_ref(src.condition);
    dst->set_cost_ref(src.cost);
    dst->set_target_ref(src.target);
    dst->set_value_ref(src.value);
    dst->set_operation_ref(src.operation);
    dst->set_owner_card_handle(hc.assign(src.owner));
    dst->set_handler_card_handle(hc.assign(src.handler));
    dst->set_active_handler_card_handle(hc.assign(src.active_handler));
    dst->set_description(src.description);
    for (auto v : src.label) dst->add_label(static_cast<int64_t>(v));
}

// Chain stack and processor-state writers are deliberately minimal in
// chunk 3 — at fresh OCG_CreateDuel time both are empty. The full walk
// lands when we have non-trivial fixtures (chunks 4-5 fold these in).
void write_chain_link(const chain& src, pb::ChainLink* dst,
                      HandleTable<card>& hc, HandleTable<effect>& he,
                      HandleTable<group>& hg) {
    write_card_state(src.triggering_state, dst->mutable_triggering_state(),
                     hc, he);
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
    dst->set_triggering_effect_handle(he.assign(src.triggering_effect));
    dst->set_target_cards_group_handle(hg.assign(src.target_cards));
    dst->set_disable_reason_handle(he.assign(src.disable_reason));
    // opinfos / possibleopinfos / triggering_event left for chunk 4-5
    // when fixtures actually exercise them.
}

void write_chain(const field& f, pb::ChainStack* dst,
                 HandleTable<card>& hc, HandleTable<effect>& he,
                 HandleTable<group>& hg) {
    for (const auto& link : f.core.current_chain) {
        write_chain_link(link, dst->add_links(), hc, he, hg);
    }
    // tpchain / ntpchain / select_chains: stub for chunk 3 (vanilla
    // duels never have these populated). Full walk in chunk 4-5.
}

void write_processor(const processor& /*core*/, pb::ProcessorState* /*dst*/,
                     HandleTable<card>& /*hc*/, HandleTable<effect>& /*he*/,
                     HandleTable<group>& /*hg*/) {
    // Stub for chunk 3. Vanilla post-CreateDuel state has no processor
    // units pushed (those come from StartDuel). Chunk 4 implements when
    // fixtures begin exercising actual game flow.
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

    // 1b. Fail-loud on chunks-3/4 stubs (per chunk-4 user decision: option 1
    // "fail-loud is cheaper than remembering"). The processor and pending-
    // chain writers are no-ops; if the state is non-empty we'd silently
    // drop it. Refuse with a clear reason so the first non-vanilla fixture
    // that hits this errors instead of silently corrupting state.
    //
    // current_chain (the active chain stack) IS handled by chunk 3's
    // write_chain_link, so it's not in this check. The list below is
    // exactly the set of subtrees still stubbed in save_state.cpp's
    // write_chain / write_processor.
    if (d.game_field) {
        const auto& core = d.game_field->core;
        if (!core.units.empty() || !core.subunits.empty() ||
            !core.tpchain.empty() || !core.ntpchain.empty() ||
            !core.select_chains.empty()) {
            *refuse_reason =
                "chunk-3/4 stub: ProcessorState (units/subunits) and "
                "pending-chain lists (tpchain/ntpchain/select_chains) are "
                "not yet wired into the save path; chunk 5 lands them. "
                "First non-vanilla fixture that hits this should drive the "
                "implementation work.";
            return OCG_SAVE_ERR_INTERNAL;
        }
    }

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
                    ht_cards, ht_effects, ht_groups);
        write_processor(d.game_field->core, state.mutable_processor(),
                        ht_cards, ht_effects, ht_groups);
    }

    for (card* c : ht_cards.in_handle_order()) {
        auto* cr = state.add_cards();
        cr->set_handle(ht_cards.assign(c));
        write_card_record(*c, cr, ht_cards, ht_effects);
    }

    for (effect* e : ht_effects.in_handle_order()) {
        auto* er = state.add_effects();
        er->set_handle(ht_effects.assign(e));
        write_effect_record(*e, er, ht_cards);
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
