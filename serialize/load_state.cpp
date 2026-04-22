#include "load_state.h"

#include "handle_table.h"
#include "ocg_state.pb.h"
#include "save_state.h"  // kSchemaVersion

#include "../card.h"
#include "../duel.h"
#include "../field.h"

#include <algorithm>
#include <array>
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

    // Effect refs / unique_effect deferred to chunk 5b — those require
    // the effect-load path to exist. For chunk 5a vanilla fixtures we
    // assert these are empty on the save side.
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

    // Chunk-5a fail-loud: still refuse subtrees the chunk-5a walks don't
    // handle. Effects, groups, chain.links, and Lua reconstruction are
    // all chunk-5b deliverables. Any blob with these set was emitted by
    // a future chunk's save path.
    if (state.effects_size() != 0 || state.groups_size() != 0 ||
        (state.has_chain() && state.chain().links_size() != 0) ||
        state.lua().closures_size() != 0) {
        delete d;
        *load_error =
            "chunk-5a stub: load path doesn't yet restore effects, "
            "groups, chain-links, or lua-reconstruction. Save side also "
            "refuses to emit these. If you're seeing this error, the blob "
            "came from a future chunk's save path.";
        return OCG_LOAD_ERR_INTERNAL;
    }

    // Allocate fresh cards in handle order. Each card is created via
    // duel::new_card(code), which triggers the host's cardReader to
    // populate card_data. Then we register the new pointer under the
    // saved handle so cross-references resolve in the second pass.
    HandleResolver<card> hc;
    HandleResolver<effect> he;  // empty in 5a; passed to load_card_state
                                // for reason_effect lookups (always 0 in 5a)
    {
        std::vector<card*> allocated_cards;
        allocated_cards.reserve(state.cards_size());
        for (const auto& cr : state.cards()) {
            // Per chunk-5a save fail-loud, no card has effect refs set.
            // Defensive double-check on load: if any card's effect_refs
            // are non-empty we have a malformed blob.
            if (cr.single_effect_size() != 0 || cr.field_effect_size() != 0 ||
                cr.equip_effect_size() != 0 || cr.target_effect_size() != 0 ||
                cr.xmaterial_effect_size() != 0) {
                for (card* c : allocated_cards) d->delete_card(c);
                delete d;
                *load_error =
                    "chunk-5a stub: card has non-empty effect_container "
                    "refs; effect load is chunk-5b work";
                return OCG_LOAD_ERR_INTERNAL;
            }
            // data_code is the canonical card identity passed to new_card
            // (which calls cardReader to populate card.data). Falls back
            // to current.code for blobs from older walks where data_code
            // wasn't set.
            const uint32_t code =
                cr.data_code() != 0 ? cr.data_code() : cr.current().code();
            card* c = d->new_card(code);
            allocated_cards.push_back(c);
            hc.register_handle(cr.handle(), c);
        }
        // Second pass: now that all card pointers are registered, restore
        // per-card fields including cross-refs (equip_target / etc.).
        for (int i = 0; i < state.cards_size(); ++i) {
            load_card_record(state.cards(i), allocated_cards[i], hc, he);
        }
    }

    // Restore field-wide state. Order matters: this MUST come AFTER the
    // new_card() loop above. Each new_card() bumps field.infos.card_id
    // (via interpreter.cpp:101 in lua->register_card), so loading
    // field_info before the card walk would have its card_id counter
    // overwritten by the card walk's increments. Loading after means the
    // counter ends up at the saved value, ready for any subsequent
    // new_card() calls during Process.
    if (d->game_field != nullptr && state.has_field_info()) {
        load_field_info(state.field_info(), &d->game_field->infos);
    }

    // Restore per-player scalars + zones. The HandleResolver is now
    // populated; zone handles resolve cleanly to allocated cards.
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
