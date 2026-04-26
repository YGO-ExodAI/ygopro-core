#include "load_state.h"

#include "handle_table.h"
#include "lua_callback.h"
#include "ocg_state.pb.h"
#include "save_state.h"  // kSchemaVersion

#include "../card.h"
#include "../duel.h"
#include "../effect.h"
#include "../field.h"
#include "../group.h"
#include "../interpreter.h"
// Chunk 9a Tier 1: ProcessorState load needs the variant types.
#include "../processor_unit.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>      // chunk 10 LoadProfile
#include <cstdio>      // chunk 10 LoadProfile
#include <cstdlib>     // chunk 10 LoadProfile (getenv)
#include <new>
#include <variant>
#include <vector>

namespace ocg::serialize {

namespace pb = ::ocg::state;

namespace {

void load_field_info(const pb::FieldInfo& src, field_info* dst) {
    dst->event_id = src.event_id();
    dst->field_id = src.field_id();
    dst->copy_id = static_cast<uint16_t>(src.copy_id());
    dst->turn_id = static_cast<int16_t>(src.turn_id());
    dst->turn_id_by_player[0] = static_cast<int16_t>(src.turn_id_player_0());
    dst->turn_id_by_player[1] = static_cast<int16_t>(src.turn_id_player_1());
    dst->card_id = src.card_id();
    dst->phase = static_cast<uint16_t>(src.phase());
    dst->turn_player = static_cast<uint8_t>(src.turn_player());
    dst->priorities[0] = static_cast<uint8_t>(src.priority_player_0());
    dst->priorities[1] = static_cast<uint8_t>(src.priority_player_1());
    dst->can_shuffle = src.can_shuffle();
}

void load_player_state(const pb::PlayerState& src, player_info* dst,
                       const HandleResolver<card>& hc) {
    dst->lp = src.lp();
    dst->start_lp = src.start_lp();
    dst->start_count = src.start_count();
    dst->draw_count = src.draw_count();
    dst->used_location = src.used_location();
    dst->disabled_location = src.disabled_location();
    dst->extra_p_count = src.extra_p_count();
    dst->exchanges = src.exchanges();
    dst->tag_index = src.tag_index();
    dst->recharge = src.recharge();

    // Restore zones from handles. mzone/szone are fixed-size (7/8); the
    // player_info ctor pre-fills with nullptrs. For these we OVERWRITE
    // the existing slots; for the variable-length zones (hand/main/etc.)
    // we clear the ctor's empties and append.
    auto fill_fixed_zone = [&](card_vector& dst_zone,
                               const auto& src_handles, size_t expected) {
        for (size_t i = 0; i < expected && i < dst_zone.size() &&
                            i < static_cast<size_t>(src_handles.size()); ++i) {
            dst_zone[i] = hc.lookup(src_handles[i]);
        }
    };
    fill_fixed_zone(dst->list_mzone, src.list_mzone(), 7);
    fill_fixed_zone(dst->list_szone, src.list_szone(), 8);

    auto fill_var_zone = [&](card_vector& dst_zone, const auto& src_handles) {
        dst_zone.clear();
        dst_zone.reserve(src_handles.size());
        for (uint32_t h : src_handles) {
            card* c = hc.lookup(h);
            // h==0 (HANDLE_NULL) lookups give nullptr — variable zones
            // shouldn't carry empties but we let the engine sort it out
            // (load is a faithful reproduction; any malformed shape is
            // a save-side bug).
            dst_zone.push_back(c);
        }
    };
    fill_var_zone(dst->list_main, src.list_main());
    fill_var_zone(dst->list_grave, src.list_grave());
    fill_var_zone(dst->list_hand, src.list_hand());
    fill_var_zone(dst->list_remove, src.list_remove());
    fill_var_zone(dst->list_extra, src.list_extra());
}

void load_card_state(const pb::CardStateSnapshot& src, card_state* dst,
                     const HandleResolver<card>& hc,
                     const HandleResolver<effect>& he) {
    dst->code = src.code();
    dst->code2 = src.code2();
    dst->setcodes.clear();
    for (auto sc : src.setcodes()) {
        dst->setcodes.insert(static_cast<uint16_t>(sc));
    }
    dst->type = src.type();
    dst->level = src.level();
    dst->rank = src.rank();
    dst->link = src.link();
    dst->link_marker = src.link_marker();
    dst->lscale = src.lscale();
    dst->rscale = src.rscale();
    dst->attribute = src.attribute();
    dst->race = src.race();
    dst->attack = src.attack();
    dst->defense = src.defense();
    dst->base_attack = src.base_attack();
    dst->base_defense = src.base_defense();
    dst->controler = static_cast<uint8_t>(src.controler());
    dst->location = static_cast<uint8_t>(src.location());
    dst->sequence = src.sequence();
    dst->position = src.position();
    dst->reason = src.reason();
    dst->pzone = src.pzone();
    dst->reason_card = hc.lookup(src.reason_card_handle());
    dst->reason_effect = he.lookup(src.reason_effect_handle());
    dst->reason_player = static_cast<uint8_t>(src.reason_player());
}

void load_card_record(const pb::CardRecord& src, card* dst,
                      const HandleResolver<card>& hc,
                      const HandleResolver<effect>& he) {
    load_card_state(src.current(), &dst->current, hc, he);
    load_card_state(src.previous(), &dst->previous, hc, he);
    load_card_state(src.temp(), &dst->temp, hc, he);

    // Counters
    dst->counters.clear();
    for (const auto& ct : src.counters()) {
        dst->counters[static_cast<uint16_t>(ct.counter_id())] = {
            static_cast<uint16_t>(ct.count_resettable()),
            static_cast<uint16_t>(ct.count_unresettable()),
        };
    }

    // Equipment / overlay cross-refs
    dst->equiping_target = hc.lookup(src.equip_target_handle());
    dst->pre_equip_target = hc.lookup(src.pre_equip_target_handle());
    dst->overlay_target = hc.lookup(src.overlay_target_handle());
    dst->pre_overlay_target = hc.lookup(src.pre_overlay_target_handle());

    dst->equiping_cards.clear();
    for (uint32_t h : src.equip_cards()) {
        if (card* c = hc.lookup(h)) dst->equiping_cards.insert(c);
    }
    dst->xyz_materials.clear();
    for (uint32_t h : src.overlay_cards()) {
        if (card* c = hc.lookup(h)) dst->xyz_materials.push_back(c);
    }
    dst->relations.clear();
    for (const auto& r : src.relations()) {
        if (card* c = hc.lookup(r.card_handle())) {
            dst->relations[c] = r.relation_flags();
        }
    }

    // Chunk 5a scalars
    dst->owner = static_cast<uint8_t>(src.owner());
    dst->cardid = src.cardid();
    dst->fieldid = src.fieldid();
    dst->fieldid_r = src.fieldid_r();
    dst->turnid = static_cast<uint16_t>(src.turnid());
    dst->turn_counter = static_cast<uint16_t>(src.turn_counter());
    dst->status = src.status();
    dst->cover = src.cover();
    dst->spsummon_code = src.spsummon_code();

    // Chunk 5b: card_set fields for effect-targeting load-stability.
    dst->material_cards.clear();
    for (uint32_t h : src.material_cards()) {
        if (card* c = hc.lookup(h)) dst->material_cards.insert(c);
    }
    dst->effect_target_owner.clear();
    for (uint32_t h : src.effect_target_owner()) {
        if (card* c = hc.lookup(h)) dst->effect_target_owner.insert(c);
    }
    dst->effect_target_cards.clear();
    for (uint32_t h : src.effect_target_cards()) {
        if (card* c = hc.lookup(h)) dst->effect_target_cards.insert(c);
    }

    // Effect refs / unique_effect: chunk 5b adds effect load (this commit).
    // The actual rebinding happens in the deserialize_duel main loop after
    // all effects are allocated, since effect handles aren't resolvable
    // until then.
}

// ---------------------------------------------------------------------------
// Chunk 5b Wave 2: effect / group / chain-link / card-effect-ref load
// helpers. Called from the named passes in deserialize_duel per plan §13.3.
// ---------------------------------------------------------------------------

void load_effect_record_scalars(const pb::EffectRecord& src, effect* dst,
                                 const HandleResolver<card>& hc) {
    // Mirror of write_effect_record from save_state.cpp.
    dst->count_limit = static_cast<uint8_t>(src.count_limit());
    dst->count_limit_max = static_cast<uint8_t>(src.count_limit_max());
    dst->count_flag = static_cast<uint8_t>(src.count_flag());
    dst->count_hopt_index = static_cast<uint8_t>(src.count_hopt_index());
    dst->effect_owner = static_cast<uint8_t>(src.effect_owner());
    dst->type = static_cast<uint16_t>(src.type());
    dst->copy_id = static_cast<uint16_t>(src.copy_id());
    dst->range = static_cast<uint16_t>(src.range());
    dst->s_range = static_cast<uint16_t>(src.s_range());
    dst->o_range = static_cast<uint16_t>(src.o_range());
    dst->reset_count = static_cast<uint16_t>(src.reset_count());
    dst->active_location = static_cast<uint16_t>(src.active_location());
    dst->active_sequence = static_cast<uint16_t>(src.active_sequence());
    dst->status = static_cast<uint16_t>(src.status());
    dst->code = src.code();
    dst->flag[0] = src.flag_lo();
    dst->flag[1] = src.flag_hi();
    dst->id = src.id();
    dst->initial_id = src.initial_id();
    dst->reset_flag = src.reset_flag();
    dst->count_code = src.count_code();
    dst->category = src.category();
    dst->hint_timing[0] = src.hint_timing_lo();
    dst->hint_timing[1] = src.hint_timing_hi();
    dst->card_type = src.card_type();
    dst->active_type = src.active_type();
    dst->label_object = src.label_object();
    // Note: condition / cost / target / value / operation are int32 lua
    // refs in C++. Set later via restore_lua_callback after the Lua VM
    // has the bytecode loaded. They stay 0 (no callback) until restoration.
    dst->owner = hc.lookup(src.owner_card_handle());
    dst->handler = hc.lookup(src.handler_card_handle());
    dst->active_handler = hc.lookup(src.active_handler_card_handle());
    dst->description = src.description();
    dst->label.clear();
    for (auto v : src.label()) dst->label.push_back(v);
}

// Restore the Lua callback bytecode + upvalues for an effect's slot.
// Returns true on success, false if the saved callback was malformed
// (load_error filled).
//
// Chunk 5c: takes a LuaLoadContext (per-card scoped). The context's
// table_handle_to_lua_ref carries previously-materialized tables so
// table_ref CapturedArgs resolve to the same Lua table instance — the
// load-side mirror of save's per-card table registry.
bool restore_effect_callback(const pb::LuaCallback& src, int32_t* dst_ref,
                              lua_State* L,
                              LuaLoadContext& ctx,
                              std::string* load_error) {
    if (!src.present()) {
        *dst_ref = 0;
        return true;
    }
    *dst_ref = restore_lua_callback(L, src, ctx, load_error);
    if (*dst_ref == 0 && !load_error->empty()) {
        return false;
    }
    return true;
}

// Chunk 9a Tier 1+2 / 9b Tier 3: ProcessorState load. Materializes
// core.units (Tier 1 variants) and core.subunits (Tier 2 variants) and
// the Tier 3 Process<true> variants from the saved per-type messages.
// Per-list dispatcher reuses the variant switch since both lists are
// std::list<processor_unit>.
//
// Returns true on success, false (with load_error filled) on malformed
// data or unknown-tier variant.
bool load_processor_unit_into(
        const ocg::state::ProcessorUnit& src_unit,
        std::list<processor_unit>& dst,
        HandleResolver<card>& hc,
        const HandleResolver<effect>& he,
        HandleResolver<group>& hg,
        const char* list_name,
        int idx,
        std::string* load_error) {
    switch (src_unit.unit_case()) {
        // ── Tier 1 ──────────────────────────────────────────────
        case ocg::state::ProcessorUnit::kAdjust: {
            const auto& m = src_unit.adjust();
            Processors::emplace_variant<Processors::Adjust>(
                dst, static_cast<uint16_t>(m.step()));
            return true;
        }
        case ocg::state::ProcessorUnit::kTurn: {
            const auto& m = src_unit.turn();
            Processors::emplace_variant<Processors::Turn>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.turn_player()));
            if (auto* t = Processors::get_opt_variant<Processors::Turn>(
                    dst.back())) {
                t->has_performed_second_battle_phase =
                    m.has_performed_second_battle_phase();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectIdleCmd: {
            const auto& m = src_unit.select_idle_cmd();
            Processors::emplace_variant<Processors::SelectIdleCmd>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectPlace: {
            const auto& m = src_unit.select_place();
            Processors::emplace_variant<Processors::SelectPlace>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.flag(),
                static_cast<uint8_t>(m.count()));
            if (auto* p = Processors::get_opt_variant<Processors::SelectPlace>(
                    dst.back())) {
                p->disable_field = m.disable_field();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kIdleCommand: {
            const auto& m = src_unit.idle_command();
            Processors::emplace_variant<Processors::IdleCommand>(
                dst, static_cast<uint16_t>(m.step()));
            if (auto* p = Processors::get_opt_variant<Processors::IdleCommand>(
                    dst.back())) {
                p->phase_to_change_to = static_cast<uint8_t>(m.phase_to_change_to());
                p->card_to_reposition = hc.lookup(m.card_to_reposition_handle());
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kPhaseEvent: {
            const auto& m = src_unit.phase_event();
            Processors::emplace_variant<Processors::PhaseEvent>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint16_t>(m.phase()));
            if (auto* p = Processors::get_opt_variant<Processors::PhaseEvent>(
                    dst.back())) {
                p->is_opponent = m.is_opponent();
                p->priority_passed = m.priority_passed();
            }
            return true;
        }
        // ── Tier 2 ──────────────────────────────────────────────
        case ocg::state::ProcessorUnit::kSelfDestroy: {
            const auto& m = src_unit.self_destroy();
            Processors::emplace_variant<Processors::SelfDestroy>(
                dst, static_cast<uint16_t>(m.step()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelfToGrave: {
            const auto& m = src_unit.self_to_grave();
            Processors::emplace_variant<Processors::SelfToGrave>(
                dst, static_cast<uint16_t>(m.step()));
            return true;
        }
        // ── Tier 3 (chunk 9b) ───────────────────────────────────
        case ocg::state::ProcessorUnit::kSelectBattleCmd: {
            const auto& m = src_unit.select_battle_cmd();
            Processors::emplace_variant<Processors::SelectBattleCmd>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectChain: {
            const auto& m = src_unit.select_chain();
            Processors::emplace_variant<Processors::SelectChain>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint8_t>(m.spe_count()),
                m.forced());
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectCard: {
            const auto& m = src_unit.select_card();
            Processors::emplace_variant<Processors::SelectCard>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.cancelable(),
                static_cast<uint8_t>(m.min()),
                static_cast<uint8_t>(m.max()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectCardCodes: {
            const auto& m = src_unit.select_card_codes();
            Processors::emplace_variant<Processors::SelectCardCodes>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.cancelable(),
                static_cast<uint8_t>(m.min()),
                static_cast<uint8_t>(m.max()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectUnselectCard: {
            const auto& m = src_unit.select_unselect_card();
            Processors::emplace_variant<Processors::SelectUnselectCard>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.cancelable(),
                static_cast<uint8_t>(m.min()),
                static_cast<uint8_t>(m.max()),
                m.finishable());
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectPosition: {
            const auto& m = src_unit.select_position();
            Processors::emplace_variant<Processors::SelectPosition>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.code(),
                static_cast<uint8_t>(m.positions()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectTributeP: {
            const auto& m = src_unit.select_tribute_p();
            Processors::emplace_variant<Processors::SelectTributeP>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.cancelable(),
                static_cast<uint8_t>(m.min()),
                static_cast<uint8_t>(m.max()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectCounter: {
            const auto& m = src_unit.select_counter();
            Processors::emplace_variant<Processors::SelectCounter>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint16_t>(m.countertype()),
                static_cast<uint16_t>(m.count()),
                static_cast<uint8_t>(m.self()),
                static_cast<uint8_t>(m.oppo()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectSum: {
            const auto& m = src_unit.select_sum();
            Processors::emplace_variant<Processors::SelectSum>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.acc(), m.min(), m.max());
            return true;
        }
        case ocg::state::ProcessorUnit::kSortCard: {
            const auto& m = src_unit.sort_card();
            Processors::emplace_variant<Processors::SortCard>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.is_chain());
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectYesNo: {
            const auto& m = src_unit.select_yes_no();
            Processors::emplace_variant<Processors::SelectYesNo>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.description());
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectEffectYesNo: {
            const auto& m = src_unit.select_effect_yes_no();
            // SelectEffectYesNo carries a card* — resolve via hc. h==0
            // → nullptr (which is what new SelectEffectYesNo() expects
            // for the absent case; the engine's emit will then produce
            // a deterministic but functionally-degenerate prompt).
            card* pcard = hc.lookup(m.pcard_handle());
            Processors::emplace_variant<Processors::SelectEffectYesNo>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.description(),
                pcard);
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectOption: {
            const auto& m = src_unit.select_option();
            Processors::emplace_variant<Processors::SelectOption>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kAnnounceRace: {
            const auto& m = src_unit.announce_race();
            Processors::emplace_variant<Processors::AnnounceRace>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint8_t>(m.count()),
                m.available());
            return true;
        }
        case ocg::state::ProcessorUnit::kAnnounceAttribute: {
            const auto& m = src_unit.announce_attribute();
            Processors::emplace_variant<Processors::AnnounceAttribute>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint8_t>(m.count()),
                m.available());
            return true;
        }
        case ocg::state::ProcessorUnit::kAnnounceCard: {
            const auto& m = src_unit.announce_card();
            Processors::emplace_variant<Processors::AnnounceCard>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kAnnounceNumber: {
            const auto& m = src_unit.announce_number();
            Processors::emplace_variant<Processors::AnnounceNumber>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kRps: {
            const auto& m = src_unit.rps();
            Processors::emplace_variant<Processors::RockPaperScissors>(
                dst, static_cast<uint16_t>(m.step()),
                m.repeat());
            if (auto* p = Processors::get_opt_variant<Processors::RockPaperScissors>(
                    dst.back())) {
                p->hand0 = static_cast<uint8_t>(m.hand0());
            }
            return true;
        }
        // ── Tier 4 (chunk 9c): Process<false> non-Select variants ────────
        case ocg::state::ProcessorUnit::kAddChain: {
            const auto& m = src_unit.add_chain();
            Processors::emplace_variant<Processors::AddChain>(
                dst, static_cast<uint16_t>(m.step()));
            if (auto* p = Processors::get_opt_variant<Processors::AddChain>(
                    dst.back())) {
                p->is_activated_effect = m.is_activated_effect();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSolveChain: {
            const auto& m = src_unit.solve_chain();
            Processors::emplace_variant<Processors::SolveChain>(
                dst, static_cast<uint16_t>(m.step()),
                m.skip_trigger(), m.skip_freechain(), m.skip_new());
            if (auto* p = Processors::get_opt_variant<Processors::SolveChain>(
                    dst.back())) {
                p->backed_up_operation = m.backed_up_operation();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kPointEvent: {
            const auto& m = src_unit.point_event();
            Processors::emplace_variant<Processors::PointEvent>(
                dst, static_cast<uint16_t>(m.step()),
                m.skip_trigger(), m.skip_freechain(), m.skip_new());
            return true;
        }
        case ocg::state::ProcessorUnit::kQuickEffect: {
            const auto& m = src_unit.quick_effect();
            Processors::emplace_variant<Processors::QuickEffect>(
                dst, static_cast<uint16_t>(m.step()),
                m.skip_freechain(),
                static_cast<uint8_t>(m.priority_player()));
            if (auto* p = Processors::get_opt_variant<Processors::QuickEffect>(
                    dst.back())) {
                p->is_opponent = m.is_opponent();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kForcedBattle: {
            const auto& m = src_unit.forced_battle();
            Processors::emplace_variant<Processors::ForcedBattle>(
                dst, static_cast<uint16_t>(m.step()));
            if (auto* p = Processors::get_opt_variant<Processors::ForcedBattle>(
                    dst.back())) {
                p->backup_phase = static_cast<uint16_t>(m.backup_phase());
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSortChain: {
            const auto& m = src_unit.sort_chain();
            Processors::emplace_variant<Processors::SortChain>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kAttackDisable: {
            const auto& m = src_unit.attack_disable();
            Processors::emplace_variant<Processors::AttackDisable>(
                dst, static_cast<uint16_t>(m.step()));
            return true;
        }
        case ocg::state::ProcessorUnit::kActivateEffect: {
            const auto& m = src_unit.activate_effect();
            effect* peff = he.lookup(m.peffect_handle());
            Processors::emplace_variant<Processors::ActivateEffect>(
                dst, static_cast<uint16_t>(m.step()), peff);
            return true;
        }
        case ocg::state::ProcessorUnit::kSolveContinuous: {
            const auto& m = src_unit.solve_continuous();
            Processors::emplace_variant<Processors::SolveContinuous>(
                dst, static_cast<uint16_t>(m.step()));
            if (auto* p = Processors::get_opt_variant<Processors::SolveContinuous>(
                    dst.back())) {
                p->reason_player = static_cast<uint8_t>(m.reason_player());
                p->reason_effect = he.lookup(m.reason_effect_handle());
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kExecuteCost: {
            const auto& m = src_unit.execute_cost();
            effect* teff = he.lookup(m.triggering_effect_handle());
            Processors::emplace_variant<Processors::ExecuteCost>(
                dst, static_cast<uint16_t>(m.step()),
                teff,
                static_cast<uint8_t>(m.triggering_player()));
            if (auto* p = Processors::get_opt_variant<Processors::ExecuteCost>(
                    dst.back())) {
                p->shuffle_check_was_disabled = m.shuffle_check_was_disabled();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kExecuteOperation: {
            const auto& m = src_unit.execute_operation();
            effect* teff = he.lookup(m.triggering_effect_handle());
            Processors::emplace_variant<Processors::ExecuteOperation>(
                dst, static_cast<uint16_t>(m.step()),
                teff,
                static_cast<uint8_t>(m.triggering_player()));
            if (auto* p = Processors::get_opt_variant<Processors::ExecuteOperation>(
                    dst.back())) {
                p->shuffle_check_was_disabled = m.shuffle_check_was_disabled();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kExecuteTarget: {
            const auto& m = src_unit.execute_target();
            effect* teff = he.lookup(m.triggering_effect_handle());
            Processors::emplace_variant<Processors::ExecuteTarget>(
                dst, static_cast<uint16_t>(m.step()),
                teff,
                static_cast<uint8_t>(m.triggering_player()));
            if (auto* p = Processors::get_opt_variant<Processors::ExecuteTarget>(
                    dst.back())) {
                p->shuffle_check_was_disabled = m.shuffle_check_was_disabled();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kMoveToField: {
            const auto& m = src_unit.move_to_field();
            card* target = hc.lookup(m.target_card_handle());
            Processors::emplace_variant<Processors::MoveToField>(
                dst, static_cast<uint16_t>(m.step()),
                target,
                m.enable(),
                static_cast<uint8_t>(m.ret()),
                m.pzone(),
                static_cast<uint8_t>(m.zone()),
                m.rule(),
                static_cast<uint8_t>(m.location_reason()),
                m.confirm());
            return true;
        }
        case ocg::state::ProcessorUnit::kBattleCommand: {
            const auto& m = src_unit.battle_command();
            group* gbattle = hg.lookup(m.cards_destroyed_by_battle_group_handle());
            Processors::emplace_variant<Processors::BattleCommand>(
                dst, static_cast<uint16_t>(m.step()),
                gbattle,
                m.forced_attack());
            if (auto* p = Processors::get_opt_variant<Processors::BattleCommand>(
                    dst.back())) {
                p->phase_to_change_to = static_cast<uint16_t>(m.phase_to_change_to());
                p->forced_attack_done = m.forced_attack_done();
                p->is_replaying_attack = m.is_replaying_attack();
                p->attack_announce_failed = m.attack_announce_failed();
                p->repeat_battle_phase = m.repeat_battle_phase();
                p->second_battle_phase_is_optional = m.second_battle_phase_is_optional();
                p->previous_point_event_had_any_trigger_to_resolve =
                    m.previous_point_event_had_any_trigger_to_resolve();
                p->reason_player = static_cast<uint8_t>(m.reason_player());
                p->damage_change_effect = he.lookup(m.damage_change_effect_handle());
                p->reason_card = hc.lookup(m.reason_card_handle());
                for (const auto& entry : m.must_attack_map()) {
                    effect* peff = he.lookup(entry.effect_handle());
                    card* pcard = hc.lookup(entry.card_handle());
                    if (peff) p->must_attack_map.emplace(peff, pcard);
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSelfDestroyUnique: {
            const auto& m = src_unit.self_destroy_unique();
            card* unique = hc.lookup(m.unique_card_handle());
            Processors::emplace_variant<Processors::SelfDestroyUnique>(
                dst, static_cast<uint16_t>(m.step()),
                unique,
                static_cast<uint8_t>(m.playerid()));
            return true;
        }
        case ocg::state::ProcessorUnit::kSelectDisField: {
            const auto& m = src_unit.select_dis_field();
            Processors::emplace_variant<Processors::SelectDisField>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.flag(),
                static_cast<uint8_t>(m.count()));
            return true;
        }
        // ── Tier 5 (chunk 9d): SpellSet / SummonRule / MonsterSet clusters ──
        case ocg::state::ProcessorUnit::kSpellSet: {
            const auto& m = src_unit.spell_set();
            card* target = hc.lookup(m.target_card_handle());
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::SpellSet>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.setplayer()),
                static_cast<uint8_t>(m.toplayer()),
                target, reff);
            return true;
        }
        case ocg::state::ProcessorUnit::kSpellSetGroup: {
            const auto& m = src_unit.spell_set_group();
            group* ptarget = hg.lookup(m.ptarget_group_handle());
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::SpellSetGroup>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.setplayer()),
                static_cast<uint8_t>(m.toplayer()),
                ptarget, m.confirm(), reff);
            if (auto* p = Processors::get_opt_variant<Processors::SpellSetGroup>(
                    dst.back())) {
                for (const uint32_t h : m.set_card_handles()) {
                    if (card* c = hc.lookup(h)) p->set_cards.insert(c);
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSummonRule: {
            const auto& m = src_unit.summon_rule();
            card* target = hc.lookup(m.target_card_handle());
            effect* proc = he.lookup(m.summon_procedure_effect_handle());
            Processors::emplace_variant<Processors::SummonRule>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.sumplayer()),
                target, proc,
                m.ignore_count(),
                static_cast<uint8_t>(m.min_tribute()),
                m.zone());
            if (auto* p = Processors::get_opt_variant<Processors::SummonRule>(
                    dst.back())) {
                p->max_allowed_tributes =
                    static_cast<uint8_t>(m.max_allowed_tributes());
                p->extra_summon_effect =
                    he.lookup(m.extra_summon_effect_handle());
                for (const uint32_t h : m.tribute_card_handles()) {
                    if (card* c = hc.lookup(h)) p->tributes.insert(c);
                }
                for (const uint32_t h : m.summon_cost_effect_handles()) {
                    p->summon_cost_effects.push_back(he.lookup(h));
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSpSummonRule: {
            const auto& m = src_unit.sp_summon_rule();
            card* target = hc.lookup(m.target_card_handle());
            Processors::emplace_variant<Processors::SpSummonRule>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.sumplayer()),
                target, m.summon_type(),
                m.is_mid_chain(),
                he.lookup(m.summon_proc_effect_handle()));
            if (auto* p = Processors::get_opt_variant<Processors::SpSummonRule>(
                    dst.back())) {
                p->cards_to_summon_g =
                    hg.lookup(m.cards_to_summon_group_handle());
                for (const uint32_t h : m.spsummon_cost_effect_handles()) {
                    p->spsummon_cost_effects.push_back(he.lookup(h));
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSpSummonRuleGroup: {
            const auto& m = src_unit.sp_summon_rule_group();
            Processors::emplace_variant<Processors::SpSummonRuleGroup>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.sumplayer()),
                m.summon_type());
            return true;
        }
        case ocg::state::ProcessorUnit::kMonsterSet: {
            const auto& m = src_unit.monster_set();
            card* target = hc.lookup(m.target_card_handle());
            effect* proc = he.lookup(m.summon_procedure_effect_handle());
            Processors::emplace_variant<Processors::MonsterSet>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.setplayer()),
                target, proc,
                m.ignore_count(),
                static_cast<uint8_t>(m.min_tribute()),
                m.zone());
            if (auto* p = Processors::get_opt_variant<Processors::MonsterSet>(
                    dst.back())) {
                p->max_allowed_tributes =
                    static_cast<uint8_t>(m.max_allowed_tributes());
                p->extra_summon_effect =
                    he.lookup(m.extra_summon_effect_handle());
                for (const uint32_t h : m.tribute_card_handles()) {
                    if (card* c = hc.lookup(h)) p->tributes.insert(c);
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kFlipSummon: {
            const auto& m = src_unit.flip_summon();
            card* target = hc.lookup(m.target_card_handle());
            Processors::emplace_variant<Processors::FlipSummon>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.sumplayer()),
                target);
            if (auto* p = Processors::get_opt_variant<Processors::FlipSummon>(
                    dst.back())) {
                for (const uint32_t h : m.flip_summon_cost_effect_handles()) {
                    p->flip_summon_cost_effects.push_back(he.lookup(h));
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kSpSummon: {
            const auto& m = src_unit.sp_summon();
            effect* reff = he.lookup(m.reason_effect_handle());
            group* targets = hg.lookup(m.targets_group_handle());
            Processors::emplace_variant<Processors::SpSummon>(
                dst, static_cast<uint16_t>(m.step()),
                reff,
                static_cast<uint8_t>(m.reason_player()),
                targets, m.zone());
            return true;
        }
        case ocg::state::ProcessorUnit::kSpSummonStep: {
            const auto& m = src_unit.sp_summon_step();
            group* targets = hg.lookup(m.targets_group_handle());
            card* target = hc.lookup(m.target_card_handle());
            Processors::emplace_variant<Processors::SpSummonStep>(
                dst, static_cast<uint16_t>(m.step()),
                targets, target, m.zone());
            if (auto* p = Processors::get_opt_variant<Processors::SpSummonStep>(
                    dst.back())) {
                for (const uint32_t h : m.spsummon_cost_effect_handles()) {
                    p->spsummon_cost_effects.push_back(he.lookup(h));
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kChangePos: {
            const auto& m = src_unit.change_pos();
            effect* reff = he.lookup(m.reason_effect_handle());
            group* targets = hg.lookup(m.targets_group_handle());
            Processors::emplace_variant<Processors::ChangePos>(
                dst, static_cast<uint16_t>(m.step()),
                targets, reff,
                static_cast<uint8_t>(m.reason_player()),
                m.enable());
            if (auto* p = Processors::get_opt_variant<Processors::ChangePos>(
                    dst.back())) {
                p->oppo_selection = m.oppo_selection();
                for (const uint32_t h : m.to_grave_card_handles()) {
                    if (card* c = hc.lookup(h)) p->to_grave_set.insert(c);
                }
            }
            return true;
        }
        // ── Tier 6 (chunk 9e): Draw / Damage / DamageStep / Equip cluster ──
        case ocg::state::ProcessorUnit::kDraw: {
            const auto& m = src_unit.draw();
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::Draw>(
                dst, static_cast<uint16_t>(m.step()),
                reff, m.reason(),
                static_cast<uint8_t>(m.reason_player()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint16_t>(m.count()));
            if (auto* p = Processors::get_opt_variant<Processors::Draw>(
                    dst.back())) {
                for (const uint32_t h : m.drawn_card_handles()) {
                    if (card* c = hc.lookup(h)) p->drawn_set.insert(c);
                }
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kDamage: {
            const auto& m = src_unit.damage();
            card* reason_card = hc.lookup(m.reason_card_handle());
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::Damage>(
                dst, static_cast<uint16_t>(m.step()),
                reff, m.reason(),
                static_cast<uint8_t>(m.reason_player()),
                reason_card,
                static_cast<uint8_t>(m.playerid()),
                m.amount(), m.is_step());
            if (auto* p = Processors::get_opt_variant<Processors::Damage>(
                    dst.back())) {
                p->is_reflected = m.is_reflected();
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kRecover: {
            const auto& m = src_unit.recover();
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::Recover>(
                dst, static_cast<uint16_t>(m.step()),
                reff, m.reason(),
                static_cast<uint8_t>(m.reason_player()),
                static_cast<uint8_t>(m.playerid()),
                m.amount(), m.is_step());
            return true;
        }
        case ocg::state::ProcessorUnit::kDamageStep: {
            const auto& m = src_unit.damage_step();
            card* attacker = hc.lookup(m.attacker_card_handle());
            card* attack_target = hc.lookup(m.attack_target_card_handle());
            Processors::emplace_variant<Processors::DamageStep>(
                dst, static_cast<uint16_t>(m.step()),
                attacker, attack_target, m.new_attack());
            if (auto* p = Processors::get_opt_variant<Processors::DamageStep>(
                    dst.back())) {
                p->backup_phase = static_cast<uint16_t>(m.backup_phase());
                p->cards_destroyed_by_battle =
                    hg.lookup(m.cards_destroyed_by_battle_group_handle());
            }
            return true;
        }
        case ocg::state::ProcessorUnit::kEquip: {
            const auto& m = src_unit.equip();
            card* equip_card = hc.lookup(m.equip_card_handle());
            card* target = hc.lookup(m.target_card_handle());
            Processors::emplace_variant<Processors::Equip>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.equip_player()),
                equip_card, target,
                m.faceup(), m.is_step());
            return true;
        }
        case ocg::state::ProcessorUnit::kPayLpCost: {
            const auto& m = src_unit.pay_lp_cost();
            Processors::emplace_variant<Processors::PayLPCost>(
                dst, static_cast<uint16_t>(m.step()),
                static_cast<uint8_t>(m.playerid()),
                m.cost());
            return true;
        }
        case ocg::state::ProcessorUnit::kRemoveCounter: {
            const auto& m = src_unit.remove_counter();
            card* pcard = hc.lookup(m.pcard_handle());
            Processors::emplace_variant<Processors::RemoveCounter>(
                dst, static_cast<uint16_t>(m.step()),
                m.reason(), pcard,
                static_cast<uint8_t>(m.rplayer()),
                static_cast<uint8_t>(m.self()),
                static_cast<uint8_t>(m.oppo()),
                static_cast<uint16_t>(m.countertype()),
                static_cast<uint16_t>(m.count()));
            return true;
        }
        case ocg::state::ProcessorUnit::kTossCoin: {
            const auto& m = src_unit.toss_coin();
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::TossCoin>(
                dst, static_cast<uint16_t>(m.step()),
                reff,
                static_cast<uint8_t>(m.reason_player()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint8_t>(m.count()));
            return true;
        }
        case ocg::state::ProcessorUnit::kTossDice: {
            const auto& m = src_unit.toss_dice();
            effect* reff = he.lookup(m.reason_effect_handle());
            Processors::emplace_variant<Processors::TossDice>(
                dst, static_cast<uint16_t>(m.step()),
                reff,
                static_cast<uint8_t>(m.reason_player()),
                static_cast<uint8_t>(m.playerid()),
                static_cast<uint8_t>(m.count1()),
                static_cast<uint8_t>(m.count2()));
            return true;
        }
        case ocg::state::ProcessorUnit::UNIT_NOT_SET:
            if (load_error) {
                *load_error = "ProcessorUnit in '" +
                              std::string(list_name) + "' at index " +
                              std::to_string(idx) +
                              " has no variant set (malformed blob)";
            }
            return false;
        default:
            if (load_error) {
                *load_error = "ProcessorUnit in '" +
                              std::string(list_name) + "' at index " +
                              std::to_string(idx) + " has variant case (" +
                              std::to_string(src_unit.unit_case()) +
                              ") not handled by this loader (Process<false> "
                              "non-Select variants — BattleCommand, Destroy, "
                              "Release, etc. — wait on their own pass).";
            }
            return false;
    }
}

// Chunk 9b: load field.processor scratch state. Mirror of
// write_processor_scratch. Pointer fields resolve through
// HandleResolver<card>/effect; handle 0 → nullptr (variable-zone
// entries can carry empties because save-side already filtered, but
// we let the engine sort it out — load is a faithful reproduction).
void load_processor_scratch(const ocg::state::ProcessorState& src,
                             processor& core,
                             const HandleResolver<card>& hc,
                             const HandleResolver<effect>& he) {
    auto fill_card_vec = [&](card_vector& dst, const auto& src_handles) {
        dst.clear();
        dst.reserve(src_handles.size());
        for (uint32_t h : src_handles) {
            if (card* c = hc.lookup(h)) dst.push_back(c);
        }
    };
    fill_card_vec(core.summonable_cards,    src.summonable_cards());
    fill_card_vec(core.spsummonable_cards,  src.spsummonable_cards());
    fill_card_vec(core.repositionable_cards,src.repositionable_cards());
    fill_card_vec(core.msetable_cards,      src.msetable_cards());
    fill_card_vec(core.ssetable_cards,      src.ssetable_cards());
    fill_card_vec(core.attackable_cards,    src.attackable_cards());
    fill_card_vec(core.select_cards,        src.select_cards());
    fill_card_vec(core.unselect_cards,      src.unselect_cards());
    fill_card_vec(core.must_select_cards,   src.must_select_cards());

    core.select_cards_codes.clear();
    core.select_cards_codes.reserve(src.select_cards_codes_size());
    for (const auto& p : src.select_cards_codes()) {
        core.select_cards_codes.emplace_back(p.code(), p.info());
    }

    core.select_options.clear();
    for (uint64_t opt : src.select_options()) core.select_options.push_back(opt);

    core.select_effects.clear();
    for (uint32_t h : src.select_effects()) {
        if (effect* e = he.lookup(h)) core.select_effects.push_back(e);
    }

    core.to_bp = src.to_bp();
    core.to_m2 = src.to_m2();
    core.to_ep = src.to_ep();
    core.skip_m2 = src.skip_m2();
    core.hint_timing[0] = src.hint_timing_0();
    core.hint_timing[1] = src.hint_timing_1();
    core.chain_attack = src.chain_attack();
    core.chain_attacker_id = src.chain_attacker_id();
}

// Chunk 9b: load select_chains. Mirror of write_select_chains.
// PendingChain only carries chain_id, triggering_player,
// triggering_effect_handle — sufficient for step==1 size check and
// step==1 dispatch (the handler reads chain.triggering_effect for
// activation). Other chain fields (triggering_card, target_player,
// flag, …) default-construct.
void load_select_chains(const ocg::state::ChainStack& src,
                         processor& core,
                         const HandleResolver<effect>& he) {
    core.select_chains.clear();
    for (const auto& p : src.select_chains()) {
        core.select_chains.emplace_back();
        chain& dst = core.select_chains.back();
        dst.chain_id = static_cast<uint16_t>(p.chain_id());
        dst.triggering_player = static_cast<uint8_t>(p.triggering_player());
        dst.triggering_effect = he.lookup(p.triggering_effect_handle());
    }
}

bool load_processor_state(const ocg::state::ProcessorState& src,
                           processor& core,
                           HandleResolver<card>& hc,
                           const HandleResolver<effect>& he,
                           HandleResolver<group>& hg,
                           std::string* load_error) {
    core.units.clear();
    for (int i = 0; i < src.units_size(); ++i) {
        if (!load_processor_unit_into(src.units(i), core.units, hc, he, hg,
                                       "units", i, load_error)) {
            return false;
        }
    }
    core.subunits.clear();
    for (int i = 0; i < src.subunits_size(); ++i) {
        if (!load_processor_unit_into(src.subunits(i), core.subunits, hc, he, hg,
                                       "subunits", i, load_error)) {
            return false;
        }
    }
    load_processor_scratch(src, core, hc, he);
    return true;
}

// Pass 6: populate card.{single,field,equip,target,xmaterial}_effect from
// saved EffectRefs after both cards (pass 1) and effects (pass 3) exist.
void load_card_effect_container_refs(const pb::CardRecord& src, card* dst,
                                      const HandleResolver<effect>& he) {
    auto fill = [&he](card::effect_container& container,
                      const auto& src_refs) {
        container.clear();
        for (const auto& er : src_refs) {
            if (effect* e = he.lookup(er.effect_handle())) {
                container.emplace(er.key(), e);
            }
        }
    };
    fill(dst->single_effect, src.single_effect());
    fill(dst->field_effect, src.field_effect());
    fill(dst->equip_effect, src.equip_effect());
    fill(dst->target_effect, src.target_effect());
    fill(dst->xmaterial_effect, src.xmaterial_effect());
}

// Group load: populate the group's container from saved card_handles.
void load_group_record(const pb::GroupRecord& src, group* dst,
                       const HandleResolver<card>& hc) {
    dst->is_readonly = src.is_readonly() ? 1 : 0;
    dst->container.clear();
    for (uint32_t h : src.card_handles()) {
        if (card* c = hc.lookup(h)) dst->container.insert(c);
    }
}

// Chain link load: mirror of write_chain_link.
void load_chain_link(const pb::ChainLink& src, chain* dst,
                     const HandleResolver<card>& hc,
                     const HandleResolver<effect>& he,
                     const HandleResolver<group>& hg) {
    load_card_state(src.triggering_state(), &dst->triggering_state, hc, he);
    dst->chain_count = static_cast<uint8_t>(src.chain_count());
    dst->chain_id = static_cast<uint16_t>(src.chain_id());
    dst->triggering_player = static_cast<uint8_t>(src.triggering_player());
    dst->triggering_controler = static_cast<uint8_t>(src.triggering_controler());
    dst->triggering_position = static_cast<uint8_t>(src.triggering_position());
    dst->target_player = static_cast<uint8_t>(src.target_player());
    dst->disable_player = static_cast<uint8_t>(src.disable_player());
    dst->triggering_summon_location = static_cast<uint8_t>(
        src.triggering_summon_location());
    dst->triggering_summon_proc_complete = src.triggering_summon_proc_complete();
    dst->was_just_sent = src.was_just_sent();
    dst->triggering_location = static_cast<uint16_t>(src.triggering_location());
    dst->triggering_sequence = src.triggering_sequence();
    dst->triggering_status = src.triggering_status();
    dst->triggering_summon_type = src.triggering_summon_type();
    dst->replace_op = src.replace_op();
    dst->target_param = src.target_param();
    dst->flag = src.flag();
    dst->event_id = src.event_id();
    dst->triggering_effect = he.lookup(src.triggering_effect_handle());
    dst->target_cards = hg.lookup(src.target_cards_group_handle());
    dst->disable_reason = he.lookup(src.disable_reason_handle());
    // opinfos / triggering_event left as defaults — chunk 5b doesn't
    // populate them on save either (chunk-3 stub still applies).
}

}  // namespace

// Chunk 10/8a: env-gated load-path profiler. Enable via
// EXODAI_LOAD_PROFILE=1 (or any non-empty value). Emits per-pass wall
// time to stderr in a CSV-friendly line. No-op (one getenv read) when
// disabled.
namespace {
struct LoadProfile {
    bool enabled;
    std::chrono::steady_clock::time_point t_prev;
    LoadProfile() : enabled(std::getenv("EXODAI_LOAD_PROFILE") != nullptr),
                    t_prev(std::chrono::steady_clock::now()) {}
    void mark(const char* phase) {
        if (!enabled) return;
        auto now = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(now - t_prev).count();
        std::fprintf(stderr, "LOAD_PROFILE %s %.3f us\n", phase, us);
        t_prev = now;
    }
};
}  // namespace

OCG_LoadStatus deserialize_duel(const void* buffer, std::size_t size,
                                const OCG_DuelOptions& options,
                                duel** out_duel,
                                std::string* load_error) {
    LoadProfile prof;
    load_error->clear();

    // Strict output-empty contract per plan §3.1.
    if (out_duel == nullptr) {
        *load_error = "out_duel pointer is null";
        return OCG_LOAD_ERR_INTERNAL;
    }
    if (*out_duel != nullptr) {
        *load_error =
            "*out_duel must be nullptr on entry (caller must destroy "
            "any prior duel and reset the pointer first)";
        return OCG_LOAD_ERR_OUTPUT_NOT_EMPTY;
    }
    if (buffer == nullptr || size == 0) {
        *load_error = "buffer is null or empty";
        return OCG_LOAD_ERR_MALFORMED;
    }

    pb::DuelState state;
    if (!state.ParseFromArray(buffer, static_cast<int>(size))) {
        *load_error = "protobuf ParseFromArray failed";
        return OCG_LOAD_ERR_MALFORMED;
    }
    prof.mark("proto_parse");

    if (state.schema_version() != kSchemaVersion) {
        // chunk-9b: v1 → v2 bumped because v1 blobs lacked field.core
        // scratch state (summonable_cards, select_chains, etc.). Loading
        // a v1 blob through a v2 engine would silently produce a duel
        // with empty validation lists at SelectIdleCmd::step==1, which
        // is the exact crash the v2 schema fixes. Hard-fail with a
        // re-record instruction rather than letting it through.
        *load_error =
            "save file schema version " +
            std::to_string(state.schema_version()) +
            " is incompatible with engine schema version " +
            std::to_string(kSchemaVersion) +
            "; re-record your save (no migration path is supplied — "
            "v1 blobs would silently load with empty Select* validation "
            "state)";
        return OCG_LOAD_ERR_SCHEMA_VERSION;
    }

    if (state.save_safety() == pb::DuelState::SAVE_SAFETY_REFUSE) {
        *load_error = "blob was tagged save_safety=REFUSE at save time";
        return OCG_LOAD_ERR_REFUSE_TAG;
    }

    // Construct fresh duel via engine constructor (allocates field +
    // interpreter + temp_card via the standard path). The seed in
    // `options` is overwritten below from the saved RNG state.
    auto* d = new (std::nothrow) duel(options);
    if (d == nullptr) {
        *load_error = "duel allocation failed";
        return OCG_LOAD_ERR_INTERNAL;
    }
    prof.mark("duel_ctor");

    // Load bootstrap scripts into the fresh Lua VM before any new_card call.
    // Mirrors what YGO_CreateDuel does in edopro.h: constant.lua + utility.lua
    // are loaded immediately after OCG_CreateDuel but before the first
    // OCG_DuelNewCard call.  utility.lua defines GetID(), which every
    // c<code>.lua script calls at the top level (line 3).  Without these two
    // loads, all card scripts fail with "attempt to call a nil value (global
    // 'GetID')" and the engine stalls after OCG_DuelLoadState returns.
    // See: YGO_CreateDuel in ygoenv/edopro/edopro.h.
    d->read_script("constant.lua");
    d->read_script("utility.lua");

    // Restore RNG state (must follow construction; ctor seeds from options).
    if (state.rng().xoshiro_state_size() != 4) {
        delete d;
        *load_error = "rng state size != 4";
        return OCG_LOAD_ERR_MALFORMED;
    }
    {
        RNG::Xoshiro256StarStar::StateType s{};
        s[0] = state.rng().xoshiro_state(0);
        s[1] = state.rng().xoshiro_state(1);
        s[2] = state.rng().xoshiro_state(2);
        s[3] = state.rng().xoshiro_state(3);
        d->set_rng_state(s);
    }

    // Chunk-5b Wave 3: lua.closures fail-loud lifted. The legacy
    // ClosureRegistration array (LuaReconstruction.closures) is no longer
    // populated by the saver — chunk 5b uses per-effect LuaCallback
    // bytecode dump on EffectRecord.*_callback instead. Field is left
    // in the schema (unread) for blob backward-compatibility; loader
    // simply ignores its contents.

    // 9-pass walk per plan §13.3. Functions named by dependency to make
    // ordering structural rather than comment-only. Debug asserts at
    // each pass entry validate prior-pass invariants.

    HandleResolver<card> hc;
    HandleResolver<effect> he;
    HandleResolver<group> hg;

    // -----------------------------------------------------------------
    // Pass 1: allocate cards via duel::new_card(data_code, /*run_initial_effect=*/false).
    //
    // We skip initial_effect because pass 3 re-allocates all effects from
    // the saved EffectRecords. Before chunk 8c, load did run initial_effect
    // per card and then pass 1.5 swept up the engine-created artifacts.
    // The sweep was correctness-complete but wasteful: new_card dominated
    // load time (850-3180µs in chunk 10's profile), and everything it
    // produced was immediately discarded. With the skip flag, pass 1.5
    // is no longer needed.
    // -----------------------------------------------------------------
    std::vector<card*> allocated_cards;
    allocated_cards.reserve(state.cards_size());
    int new_card_count = 0;
    for (const auto& cr : state.cards()) {
        const uint32_t code = cr.data_code() != 0 ? cr.data_code()
                                                   : cr.current().code();
        card* c = d->new_card(code, /*run_initial_effect=*/false);
        ++new_card_count;
        allocated_cards.push_back(c);
        hc.register_handle(cr.handle(), c);
    }
    (void)new_card_count;  // suppress unused-warning when asserts are off

    prof.mark("pass1_alloc_cards");

    // -----------------------------------------------------------------
    // Pass 2: load card scalars + card-to-card cross-refs.
    // -----------------------------------------------------------------
    for (int i = 0; i < state.cards_size(); ++i) {
        load_card_record(state.cards(i), allocated_cards[i], hc, he);
    }

    prof.mark("pass2_card_scalars");

    // -----------------------------------------------------------------
    // Pass 3: allocate effects via duel::new_effect().
    // -----------------------------------------------------------------
    assert(static_cast<int>(hc.bound_count()) == state.cards_size() &&
           "pass 3 invariant: all cards allocated");
    std::vector<effect*> allocated_effects;
    allocated_effects.reserve(state.effects_size());
    for (const auto& er : state.effects()) {
        effect* e = d->new_effect();
        allocated_effects.push_back(e);
        he.register_handle(er.handle(), e);
    }

    prof.mark("pass3_alloc_effects");

    // -----------------------------------------------------------------
    // Pass 4: load effect scalars + Lua callback restoration via
    //         restore_lua_callback (plan §13.4).
    // -----------------------------------------------------------------
    assert(static_cast<int>(he.bound_count()) == state.effects_size() &&
           "pass 4 invariant: all effects allocated");
    lua_State* L = (d->lua != nullptr) ? d->lua->lua_state : nullptr;
    // Chunk 5c: per-card LuaLoadContext mirrors the save-side per-card
    // registry. Reset between owners so table_ref handles resolve within
    // the correct scope. Owner-change detection works because save-side
    // emits effects in the same card-grouped order.
    LuaLoadContext lua_ctx{hc, he, hg, {}, 0};
    card* prev_owner = nullptr;
    for (int i = 0; i < state.effects_size(); ++i) {
        const auto& src = state.effects(i);
        effect* e = allocated_effects[i];
        load_effect_record_scalars(src, e, hc);
        if (e->owner != prev_owner) {
            lua_ctx.reset_per_card();
            prev_owner = e->owner;
        }
        if (L != nullptr) {
            std::string err;
            const struct {
                const pb::LuaCallback& cb;
                int32_t* slot;
            } slots[] = {
                {src.condition_callback(), &e->condition},
                {src.cost_callback(),      &e->cost},
                {src.target_callback(),    &e->target},
                {src.value_callback(),     &e->value},
                {src.operation_callback(), &e->operation},
            };
            for (const auto& s : slots) {
                if (!restore_effect_callback(s.cb, s.slot, L, lua_ctx, &err)) {
                    delete d;
                    *load_error = "Lua callback restore failed: " + err;
                    return OCG_LOAD_ERR_MALFORMED;
                }
            }
        }
    }

    prof.mark("pass4_lua_callbacks");

    // -----------------------------------------------------------------
    // Pass 5: allocate groups + populate container from card_handles.
    // -----------------------------------------------------------------
    std::vector<group*> allocated_groups;
    allocated_groups.reserve(state.groups_size());
    for (const auto& gr : state.groups()) {
        group* g = d->new_group();
        allocated_groups.push_back(g);
        hg.register_handle(gr.handle(), g);
        load_group_record(gr, g, hc);
    }

    prof.mark("pass5_groups");

    // -----------------------------------------------------------------
    // Pass 6: third card pass — populate card.{single,field,equip,target,
    //         xmaterial}_effect from saved EffectRefs.
    // -----------------------------------------------------------------
    assert(static_cast<int>(hc.bound_count()) == state.cards_size() &&
           static_cast<int>(he.bound_count()) == state.effects_size() &&
           "pass 6 invariant: cards + effects allocated");
    for (int i = 0; i < state.cards_size(); ++i) {
        load_card_effect_container_refs(state.cards(i), allocated_cards[i], he);
    }

    // -----------------------------------------------------------------
    // Pass 7: load chain links (current_chain) + select_chains.
    // chunk 9b: select_chains references effect handles only — pass 7
    // is the right place since effects exist by pass 4.
    // -----------------------------------------------------------------
    if (d->game_field != nullptr && state.has_chain()) {
        d->game_field->core.current_chain.clear();
        for (const auto& link_pb : state.chain().links()) {
            d->game_field->core.current_chain.emplace_back();
            load_chain_link(link_pb, &d->game_field->core.current_chain.back(),
                            hc, he, hg);
        }
        load_select_chains(state.chain(), d->game_field->core, he);
    }

    // -----------------------------------------------------------------
    // Pass 8: load field_info_post_card_walk. MUST follow card walk
    //         (pass 1) — each new_card bumps field.infos.card_id.
    // -----------------------------------------------------------------
    if (d->game_field != nullptr && state.has_field_info()) {
        load_field_info(state.field_info(), &d->game_field->infos);
    }

    // -----------------------------------------------------------------
    // Pass 9: load player zones (zone vectors of card_handles).
    // -----------------------------------------------------------------
    if (d->game_field != nullptr) {
        const int n = state.players_size();
        if (n != 2) {
            for (uint32_t i = 1; i <= state.cards_size(); ++i) {
                if (card* c = hc.lookup(i)) d->delete_card(c);
            }
            delete d;
            *load_error = "expected exactly 2 PlayerState entries, got " +
                          std::to_string(n);
            return OCG_LOAD_ERR_MALFORMED;
        }
        for (int p = 0; p < 2; ++p) {
            load_player_state(state.players(p), &d->game_field->player[p], hc);
        }
    }

    // -----------------------------------------------------------------
    // Pass 10 (chunk 9a Tier 1+2 / 9b Tier 3 + scratch): ProcessorState.
    // Materializes core.units / core.subunits from the saved per-type
    // messages, then fills the field.processor scratch state
    // (summonable_cards, select_chains-equivalent, to_bp/to_ep, etc.)
    // that step==1 of every Select* handler validates against.
    //
    // Pass ordering: must follow Pass 1 (cards), Pass 3 (effects),
    // Pass 9 (player zones — cards must be assigned to zones before
    // scratch lookups; many scratch entries are zone references).
    // -----------------------------------------------------------------
    if (d->game_field != nullptr && state.has_processor()) {
        if (!load_processor_state(state.processor(),
                                   d->game_field->core, hc, he, hg, load_error)) {
            for (uint32_t i = 1; i <= state.cards_size(); ++i) {
                if (card* c = hc.lookup(i)) d->delete_card(c);
            }
            delete d;
            return OCG_LOAD_ERR_MALFORMED;
        }
    }

    prof.mark("pass6-10_post_lua");

    *out_duel = d;
    return OCG_LOAD_OK;
}

}  // namespace ocg::serialize
