// test_schema.cpp — §8.1 of phase_p1_primitive_1_plan.md
//
// Schema roundtrip: hand-construct a DuelState message touching every
// top-level subtree, serialize to bytes, parse back, assert the parsed
// message is equal to the original. Catches schema drift and protobuf
// plumbing bugs without needing the engine.

#include "ocg_state.pb.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ocgs = ocg::state;

namespace {

#define CHECK_EQ(actual, expected, label)                                    \
    do {                                                                     \
        if (!((actual) == (expected))) {                                     \
            std::fprintf(stderr,                                             \
                "FAIL %s:%d: %s mismatch: got %lld, want %lld\n",            \
                __FILE__, __LINE__, (label),                                 \
                static_cast<long long>(actual),                              \
                static_cast<long long>(expected));                           \
            return false;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_TRUE(cond, label)                                              \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s false\n",                   \
                __FILE__, __LINE__, (label));                                \
            return false;                                                    \
        }                                                                    \
    } while (0)

// Hand-build a DuelState that touches every top-level subtree. Field
// values are arbitrary but deliberately distinctive (avoid 0/1 collisions
// that could mask copy bugs).
ocgs::DuelState make_fixture() {
    ocgs::DuelState s;
    s.set_schema_version(7);
    s.set_engine_build_hash(0xDEADBEEFCAFEBABEULL);
    s.set_timestamp_unix(1714000000);
    s.set_save_safety(ocgs::DuelState::SAVE_SAFETY_OK);
    s.set_refuse_reason("");

    auto* rng = s.mutable_rng();
    rng->add_xoshiro_state(0x0102030405060708ULL);
    rng->add_xoshiro_state(0x1112131415161718ULL);
    rng->add_xoshiro_state(0x2122232425262728ULL);
    rng->add_xoshiro_state(0x3132333435363738ULL);

    auto* fi = s.mutable_field_info();
    fi->set_event_id(1234);
    fi->set_field_id(5);
    fi->set_copy_id(2);
    fi->set_turn_id(11);
    fi->set_turn_id_player_0(6);
    fi->set_turn_id_player_1(5);
    fi->set_card_id(789);
    fi->set_phase(0x0040);  // PHASE_BATTLE
    fi->set_turn_player(1);
    fi->set_priority_player_0(0);
    fi->set_priority_player_1(0);
    fi->set_can_shuffle(true);

    for (int p = 0; p < 2; ++p) {
        auto* ps = s.add_players();
        ps->set_lp(p == 0 ? 7400 : 8000);
        ps->set_start_lp(8000);
        ps->set_start_count(5);
        ps->set_draw_count(1);
        ps->set_used_location(0x42);
        ps->set_disabled_location(0);
        ps->set_extra_p_count(0);
        ps->set_exchanges(0);
        ps->set_tag_index(0);
        ps->set_recharge(false);
        // Fixed-size mzone (7) and szone (8) per player_info ctor.
        for (int z = 0; z < 7; ++z) ps->add_list_mzone(0);
        for (int z = 0; z < 8; ++z) ps->add_list_szone(0);
        // Hand: a couple of card handles.
        if (p == 0) {
            ps->add_list_hand(1);  // → CardRecord handle 1
            ps->add_list_hand(2);
        }
    }

    // Two cards, with cross-references (card 1 equips card 2; card 2's
    // equip_target is card 1). Round-trip should preserve both directions.
    {
        auto* c1 = s.add_cards();
        c1->set_handle(1);
        auto* cur = c1->mutable_current();
        cur->set_code(46986414);  // Dark Magician
        cur->set_attack(2500);
        cur->set_defense(2100);
        cur->set_attribute(0x40);  // DARK
        cur->set_race(0x10);       // SPELLCASTER
        cur->set_controler(0);
        cur->set_location(0x02);   // LOCATION_HAND
        cur->set_sequence(0);
        cur->set_position(0x05);   // POS_FACEUP_ATTACK
        cur->set_reason_card_handle(2);
        cur->set_reason_effect_handle(1);

        auto* prev = c1->mutable_previous();
        prev->set_code(0);

        auto* tmp = c1->mutable_temp();
        tmp->set_code(0);

        // Two effect refs
        auto* er = c1->add_single_effect();
        er->set_effect_handle(1);
        er->set_key(0x100);
        er = c1->add_field_effect();
        er->set_effect_handle(2);
        er->set_key(0x200);

        // One counter
        auto* ct = c1->add_counters();
        ct->set_counter_id(3);
        ct->set_count_resettable(2);
        ct->set_count_unresettable(0);

        c1->set_equip_target_handle(2);
        c1->add_equip_cards(2);
        c1->add_overlay_cards(2);

        auto* rel = c1->add_relations();
        rel->set_card_handle(2);
        rel->set_relation_flags(0x80);
    }
    {
        auto* c2 = s.add_cards();
        c2->set_handle(2);
        auto* cur = c2->mutable_current();
        cur->set_code(89631139);  // Blue-Eyes White Dragon
        cur->set_attack(3000);
        cur->set_defense(2500);
        c2->mutable_previous();
        c2->mutable_temp();
        c2->set_equip_target_handle(1);  // back-ref to card 1
    }

    // Two effects, with handler/owner cross-refs to cards.
    {
        auto* e1 = s.add_effects();
        e1->set_handle(1);
        e1->set_type(0x0010);          // EFFECT_TYPE_TRIGGER_O
        e1->set_code(0x12345);
        e1->set_id(101);
        e1->set_initial_id(101);
        e1->set_owner_card_handle(1);
        e1->set_handler_card_handle(1);
        e1->set_active_handler_card_handle(0);
        e1->set_condition_ref(7);
        e1->set_target_ref(8);
        e1->set_operation_ref(9);
        e1->add_label(0x111);
        e1->add_label(0x222);
    }
    {
        auto* e2 = s.add_effects();
        e2->set_handle(2);
        e2->set_type(0x0001);          // EFFECT_TYPE_FIELD
        e2->set_code(0x67890);
        e2->set_id(102);
        e2->set_initial_id(102);
        e2->set_owner_card_handle(2);
        e2->set_handler_card_handle(2);
    }

    // One group containing both cards.
    {
        auto* g1 = s.add_groups();
        g1->set_handle(1);
        g1->add_card_handles(1);
        g1->add_card_handles(2);
        g1->set_is_readonly(false);
    }

    // Chain stack with one active link referencing effect 1 + group 1.
    {
        auto* chain = s.mutable_chain();
        auto* link = chain->add_links();
        link->set_chain_count(1);
        link->set_chain_id(42);
        link->set_triggering_player(0);
        link->set_triggering_controler(0);
        link->set_triggering_position(0x05);
        link->set_target_player(1);
        link->set_triggering_summon_proc_complete(true);
        link->set_was_just_sent(false);
        link->set_triggering_location(0x02);
        link->set_triggering_sequence(0);
        link->set_triggering_status(0xFF);
        link->set_triggering_summon_type(0x10);
        link->set_replace_op(-1);
        link->set_target_param(0x55);
        link->set_flag(0xAA);
        link->set_event_id(1234);
        link->set_triggering_effect_handle(1);
        link->set_target_cards_group_handle(1);
        link->set_disable_reason_handle(0);
        auto* trig = link->mutable_triggering_state();
        trig->set_code(46986414);
        trig->set_controler(0);
        auto* op = link->add_opinfos();
        op->set_key(0x1000);
        op->set_op_player(0);
        op->set_op_param(123);
        op->set_op_target_group_handle(1);
        op->set_op_target_card_handle(0);
        auto* ev = link->mutable_triggering_event();
        ev->set_event_code(0x42);
        ev->set_event_player(0);
        ev->set_event_value(99);
        ev->set_event_reason_effect_handle(1);

        auto* pending = chain->add_select_chains();
        pending->set_chain_id(43);
        pending->set_triggering_effect_handle(2);
        pending->set_triggering_player(1);
    }

    // Processor state with one unit.
    {
        auto* pr = s.mutable_processor();
        auto* u = pr->add_units();
        u->set_unit_type(0x10);
        u->set_step(3);
        u->set_peffect_handle(1);
        u->set_ptarget_handle(1);
        u->set_arg1(0x111);
        u->set_arg2(0x222);
        u->set_arg3(0x333);
        u->set_arg4(0x444);
        pr->set_select_min(1);
        pr->set_select_max(3);
        pr->set_select_cancelable(true);
        pr->set_select_hint(0xCAFE);
        pr->add_select_card_handles(1);
        pr->add_select_card_handles(2);
    }

    // Lua reconstruction with one Type-C closure registration that
    // exercises every CapturedArg variant.
    {
        auto* lua = s.mutable_lua();
        auto* cr = lua->add_closures();
        cr->set_card_handle(1);
        cr->set_effect_handle(1);
        cr->set_slot(ocgs::ClosureRegistration::SLOT_OPERATION);
        cr->set_generator_name("s.extrafil");
        cr->add_captured_args()->set_b(true);
        cr->add_captured_args()->set_i(-12345);
        cr->add_captured_args()->set_d(3.14159);
        cr->add_captured_args()->set_s("hello");
        cr->add_captured_args()->set_card_handle(1);
        cr->add_captured_args()->set_group_handle(1);
        cr->add_captured_args()->set_effect_handle(1);
    }

    return s;
}

bool test_roundtrip_serialize_parse() {
    const ocgs::DuelState before = make_fixture();
    std::string bytes;
    CHECK_TRUE(before.SerializeToString(&bytes), "SerializeToString");
    CHECK_TRUE(!bytes.empty(), "non-empty bytes");

    ocgs::DuelState after;
    CHECK_TRUE(after.ParseFromString(bytes), "ParseFromString");

    // Top-level scalars
    CHECK_EQ(after.schema_version(), 7u, "schema_version");
    CHECK_EQ(after.engine_build_hash(), 0xDEADBEEFCAFEBABEULL,
             "engine_build_hash");
    CHECK_EQ(after.timestamp_unix(), 1714000000ull, "timestamp_unix");
    CHECK_EQ(after.save_safety(), ocgs::DuelState::SAVE_SAFETY_OK,
             "save_safety");

    // RNG
    CHECK_EQ(after.rng().xoshiro_state_size(), 4, "rng size");
    CHECK_EQ(after.rng().xoshiro_state(0), 0x0102030405060708ULL, "rng[0]");
    CHECK_EQ(after.rng().xoshiro_state(3), 0x3132333435363738ULL, "rng[3]");

    // FieldInfo
    CHECK_EQ(after.field_info().event_id(), 1234u, "event_id");
    CHECK_EQ(after.field_info().turn_id(), 11, "turn_id");
    CHECK_EQ(after.field_info().turn_player(), 1u, "turn_player");
    CHECK_TRUE(after.field_info().can_shuffle(), "can_shuffle");

    // Players
    CHECK_EQ(after.players_size(), 2, "players size");
    CHECK_EQ(after.players(0).lp(), 7400, "p0 lp");
    CHECK_EQ(after.players(1).lp(), 8000, "p1 lp");
    CHECK_EQ(after.players(0).list_mzone_size(), 7, "p0 mzone size");
    CHECK_EQ(after.players(0).list_szone_size(), 8, "p0 szone size");
    CHECK_EQ(after.players(0).list_hand_size(), 2, "p0 hand size");
    CHECK_EQ(after.players(0).list_hand(0), 1u, "p0 hand[0] handle");

    // Cards + cross-references
    CHECK_EQ(after.cards_size(), 2, "cards count");
    CHECK_EQ(after.cards(0).handle(), 1u, "card[0] handle");
    CHECK_EQ(after.cards(0).current().code(), 46986414u, "card[0] code");
    CHECK_EQ(after.cards(0).current().attack(), 2500, "card[0] atk");
    CHECK_EQ(after.cards(0).current().reason_card_handle(), 2u,
             "card[0] reason_card_handle");
    CHECK_EQ(after.cards(0).single_effect_size(), 1, "card[0] single_eff");
    CHECK_EQ(after.cards(0).single_effect(0).effect_handle(), 1u,
             "card[0] single_eff[0]");
    CHECK_EQ(after.cards(0).counters_size(), 1, "card[0] counters");
    CHECK_EQ(after.cards(0).counters(0).count_resettable(), 2u,
             "card[0] counter resettable");
    CHECK_EQ(after.cards(0).equip_target_handle(), 2u, "card[0] equip_target");
    CHECK_EQ(after.cards(0).overlay_cards(0), 2u, "card[0] overlay[0]");
    CHECK_EQ(after.cards(0).relations(0).relation_flags(), 0x80u,
             "card[0] relation flags");
    // Cycle back-ref
    CHECK_EQ(after.cards(1).handle(), 2u, "card[1] handle");
    CHECK_EQ(after.cards(1).equip_target_handle(), 1u, "card[1] equip_target");

    // Effects + cross-references
    CHECK_EQ(after.effects_size(), 2, "effects count");
    CHECK_EQ(after.effects(0).owner_card_handle(), 1u, "e0 owner");
    CHECK_EQ(after.effects(0).operation_ref(), 9, "e0 op ref");
    CHECK_EQ(after.effects(0).label_size(), 2, "e0 labels");
    CHECK_EQ(after.effects(0).label(1), 0x222, "e0 label[1]");

    // Group
    CHECK_EQ(after.groups_size(), 1, "groups");
    CHECK_EQ(after.groups(0).card_handles_size(), 2, "g0 size");
    CHECK_EQ(after.groups(0).card_handles(1), 2u, "g0 card[1]");

    // Chain
    CHECK_EQ(after.chain().links_size(), 1, "chain links");
    const auto& link = after.chain().links(0);
    CHECK_EQ(link.chain_id(), 42u, "link chain_id");
    CHECK_EQ(link.triggering_effect_handle(), 1u, "link trigger eff");
    CHECK_EQ(link.target_cards_group_handle(), 1u, "link target group");
    CHECK_EQ(link.opinfos_size(), 1, "link opinfos");
    CHECK_EQ(link.opinfos(0).op_target_group_handle(), 1u,
             "link opinfos[0] target group");
    CHECK_EQ(link.triggering_event().event_code(), 0x42u, "link event_code");
    CHECK_EQ(after.chain().select_chains_size(), 1, "select_chains");
    CHECK_EQ(after.chain().select_chains(0).triggering_effect_handle(), 2u,
             "select_chains[0] eff");

    // Processor
    CHECK_EQ(after.processor().units_size(), 1, "proc units");
    CHECK_EQ(after.processor().units(0).peffect_handle(), 1, "proc unit eff");
    CHECK_EQ(after.processor().units(0).arg3(), 0x333, "proc unit arg3");
    CHECK_EQ(after.processor().select_card_handles_size(), 2,
             "proc select cards");
    CHECK_TRUE(after.processor().select_cancelable(), "proc cancelable");

    // Lua reconstruction with all CapturedArg variants
    CHECK_EQ(after.lua().closures_size(), 1, "lua closures");
    const auto& cr = after.lua().closures(0);
    CHECK_EQ(cr.card_handle(), 1u, "cr card");
    CHECK_EQ(cr.slot(), ocgs::ClosureRegistration::SLOT_OPERATION, "cr slot");
    CHECK_TRUE(cr.generator_name() == "s.extrafil", "cr gen name");
    CHECK_EQ(cr.captured_args_size(), 7, "cr args size");
    CHECK_TRUE(cr.captured_args(0).b(), "arg0 bool");
    CHECK_EQ(cr.captured_args(1).i(), -12345, "arg1 int");
    CHECK_TRUE(cr.captured_args(2).d() > 3.14 && cr.captured_args(2).d() < 3.15,
               "arg2 double");
    CHECK_TRUE(cr.captured_args(3).s() == "hello", "arg3 string");
    CHECK_EQ(cr.captured_args(4).card_handle(), 1u, "arg4 card");
    CHECK_EQ(cr.captured_args(5).group_handle(), 1u, "arg5 group");
    CHECK_EQ(cr.captured_args(6).effect_handle(), 1u, "arg6 effect");

    return true;
}

bool test_byte_equal_serialize_twice() {
    // Same fixture serialized twice → identical bytes (intra-path
    // determinism check; analog of §8.4 byte-equal but in C++ unit form).
    const ocgs::DuelState s = make_fixture();
    std::string a, b;
    CHECK_TRUE(s.SerializeToString(&a), "serialize 1");
    CHECK_TRUE(s.SerializeToString(&b), "serialize 2");
    CHECK_TRUE(a == b, "byte-equal across two serializations");
    return true;
}

bool test_default_message_round_trips() {
    // A default-constructed DuelState (all fields zero / empty) must
    // round-trip cleanly. Catches optional/oneof zero-vs-unset confusion.
    ocgs::DuelState empty;
    std::string bytes;
    CHECK_TRUE(empty.SerializeToString(&bytes), "serialize empty");
    ocgs::DuelState parsed;
    CHECK_TRUE(parsed.ParseFromString(bytes), "parse empty");
    CHECK_EQ(parsed.schema_version(), 0u, "empty schema_version");
    CHECK_EQ(parsed.players_size(), 0, "empty players");
    CHECK_EQ(parsed.cards_size(), 0, "empty cards");
    return true;
}

}  // namespace

int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    int failed = 0;

    struct Test {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"roundtrip_serialize_parse", &test_roundtrip_serialize_parse},
        {"byte_equal_serialize_twice", &test_byte_equal_serialize_twice},
        {"default_message_round_trips", &test_default_message_round_trips},
    };

    for (const auto& t : tests) {
        std::printf("[ RUN  ] %s\n", t.name);
        if (t.fn()) {
            std::printf("[ PASS ] %s\n", t.name);
        } else {
            std::printf("[ FAIL ] %s\n", t.name);
            ++failed;
        }
    }

    google::protobuf::ShutdownProtobufLibrary();
    if (failed == 0) {
        std::printf("test_schema: ALL %zu PASSED\n",
                    sizeof(tests) / sizeof(tests[0]));
        return 0;
    } else {
        std::printf("test_schema: %d FAILED\n", failed);
        return 1;
    }
}
