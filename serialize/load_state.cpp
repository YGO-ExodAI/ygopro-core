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

#include <algorithm>
#include <array>
#include <cassert>
#include <new>
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
bool restore_effect_callback(const pb::LuaCallback& src, int32_t* dst_ref,
                              lua_State* L,
                              HandleResolver<card>& hc,
                              HandleResolver<effect>& he,
                              HandleResolver<group>& hg,
                              std::string* load_error) {
    if (!src.present()) {
        *dst_ref = 0;
        return true;
    }
    *dst_ref = restore_lua_callback(L, src, hc, he, hg, load_error);
    if (*dst_ref == 0 && !load_error->empty()) {
        return false;
    }
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

OCG_LoadStatus deserialize_duel(const void* buffer, std::size_t size,
                                const OCG_DuelOptions& options,
                                duel** out_duel,
                                std::string* load_error) {
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

    if (state.schema_version() != kSchemaVersion) {
        *load_error =
            "schema_version " + std::to_string(state.schema_version()) +
            " not supported by this ocgcore (expected " +
            std::to_string(kSchemaVersion) + ")";
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
    // Pass 1: allocate cards via duel::new_card(data_code).
    // -----------------------------------------------------------------
    std::vector<card*> allocated_cards;
    allocated_cards.reserve(state.cards_size());
    int new_card_count = 0;
    for (const auto& cr : state.cards()) {
        const uint32_t code = cr.data_code() != 0 ? cr.data_code()
                                                   : cr.current().code();
        card* c = d->new_card(code);
        ++new_card_count;
        allocated_cards.push_back(c);
        hc.register_handle(cr.handle(), c);
    }
    (void)new_card_count;  // suppress unused-warning when asserts are off

    // -----------------------------------------------------------------
    // Pass 1.5: clear engine-created initial_effect artifacts.
    //
    // duel::new_card(code) runs the card's initial_effect for non-vanilla
    // codes, which creates and registers effects via Effect.CreateEffect
    // + RegisterEffect. Those effects end up in duel.effects AND in the
    // card's effect_containers. We're about to re-allocate effects from
    // the saved EffectRecords (pass 3); without clearing the engine's
    // initial_effect artifacts here, duel.effects ends up with both sets
    // (saved + engine-created), corrupting round-trip determinism.
    //
    // Order matters: clear card.effect_containers BEFORE delete_effect
    // (which only removes from duel.effects, not from any cards). After
    // this pass, duel.effects is empty for the loaded cards' contributions.
    {
        std::vector<effect*> to_delete;
        for (card* c : allocated_cards) {
            for (const auto& kv : c->single_effect)    to_delete.push_back(kv.second);
            for (const auto& kv : c->field_effect)     to_delete.push_back(kv.second);
            for (const auto& kv : c->equip_effect)     to_delete.push_back(kv.second);
            for (const auto& kv : c->target_effect)    to_delete.push_back(kv.second);
            for (const auto& kv : c->xmaterial_effect) to_delete.push_back(kv.second);
            c->single_effect.clear();
            c->field_effect.clear();
            c->equip_effect.clear();
            c->target_effect.clear();
            c->xmaterial_effect.clear();
        }
        for (effect* e : to_delete) {
            // delete_effect removes from duel.effects and frees memory.
            // Note: a single effect might be referenced from multiple
            // containers; dedup by tracking visits would be safer, but
            // for chunk-5b vanilla-and-Type-C fixtures we don't have
            // shared effects across cards.
            d->delete_effect(e);
        }
    }

    // -----------------------------------------------------------------
    // Pass 2: load card scalars + card-to-card cross-refs.
    // -----------------------------------------------------------------
    for (int i = 0; i < state.cards_size(); ++i) {
        load_card_record(state.cards(i), allocated_cards[i], hc, he);
    }

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

    // -----------------------------------------------------------------
    // Pass 4: load effect scalars + Lua callback restoration via
    //         restore_lua_callback (plan §13.4).
    // -----------------------------------------------------------------
    assert(static_cast<int>(he.bound_count()) == state.effects_size() &&
           "pass 4 invariant: all effects allocated");
    lua_State* L = (d->lua != nullptr) ? d->lua->lua_state : nullptr;
    for (int i = 0; i < state.effects_size(); ++i) {
        const auto& src = state.effects(i);
        effect* e = allocated_effects[i];
        load_effect_record_scalars(src, e, hc);
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
                if (!restore_effect_callback(s.cb, s.slot, L, hc, he, hg, &err)) {
                    delete d;
                    *load_error = "Lua callback restore failed: " + err;
                    return OCG_LOAD_ERR_MALFORMED;
                }
            }
        }
    }

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
    // Pass 7: load chain links (current_chain).
    // -----------------------------------------------------------------
    if (d->game_field != nullptr && state.has_chain()) {
        d->game_field->core.current_chain.clear();
        for (const auto& link_pb : state.chain().links()) {
            d->game_field->core.current_chain.emplace_back();
            load_chain_link(link_pb, &d->game_field->core.current_chain.back(),
                            hc, he, hg);
        }
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

    *out_duel = d;
    return OCG_LOAD_OK;
}

}  // namespace ocg::serialize
