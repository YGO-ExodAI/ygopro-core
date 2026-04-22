#include "load_state.h"

#include "ocg_state.pb.h"
#include "save_state.h"  // kSchemaVersion

#include "../duel.h"
#include "../field.h"

#include <array>
#include <new>

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

void load_player_state(const pb::PlayerState& src, player_info* dst) {
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

    // For chunk 4 (no-card path): zone vectors are validated to be all-
    // handle-0 (empty slots) on the save side because vanilla state has
    // no cards. The loaded duel's player_info ctor already initialized
    // list_mzone (size 7) and list_szone (size 8) with nullptr entries;
    // for the chunk-4 vanilla case we don't need to repopulate them.
    //
    // Chunk 5+ will need: handle resolver registers cards in a first pass,
    // then this fn rewrites the zones from src.list_mzone() / etc. handles.
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

    // Restore field-wide state (only if a field exists, which it always
    // does post-construction; the check is defensive).
    if (d->game_field != nullptr && state.has_field_info()) {
        load_field_info(state.field_info(), &d->game_field->infos);
    }

    // Restore per-player scalars + zones. For chunk 4 vanilla (no cards),
    // load_player_state's zone-restore is a no-op; the player_info ctor
    // already zeroed the fixed-size mzone/szone arrays.
    if (d->game_field != nullptr) {
        const int n = state.players_size();
        if (n != 2) {
            delete d;
            *load_error = "expected exactly 2 PlayerState entries, got " +
                          std::to_string(n);
            return OCG_LOAD_ERR_MALFORMED;
        }
        for (int p = 0; p < 2; ++p) {
            load_player_state(state.players(p), &d->game_field->player[p]);
        }
    }

    // Chunk 4 fail-loud: refuse to load any blob that contains state our
    // chunk-3/4 walks can't restore. Mirrors the save-side fail-loud in
    // save_state.cpp. First non-vanilla fixture to trip this should drive
    // the chunk-5 implementation work; loading + then silently producing
    // a duel missing cards/effects/groups would be much worse.
    if (state.cards_size() != 0 || state.effects_size() != 0 ||
        state.groups_size() != 0 ||
        (state.has_chain() && state.chain().links_size() != 0) ||
        state.lua().closures_size() != 0) {
        delete d;
        *load_error =
            "chunk-4 stub: load path does not yet restore cards/effects/"
            "groups/chain-links/lua-reconstruction. Save side already "
            "refuses to emit these; if you're seeing this error, the blob "
            "came from a future chunk's save path.";
        return OCG_LOAD_ERR_INTERNAL;
    }

    *out_duel = d;
    return OCG_LOAD_OK;
}

}  // namespace ocg::serialize
