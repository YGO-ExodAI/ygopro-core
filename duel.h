/*
 * Copyright (c) 2010-2015, Argon Sun (Fluorohydride)
 * Copyright (c) 2016-2024, Edoardo Lolletti (edo9300) <edoardo762@gmail.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef DUEL_H_
#define DUEL_H_

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility> //std::forward
#include <vector>
#include "common.h"
#include "group.h"
#include "interpreter.h"
#include "ocgapi_types.h"
#include "RNG/Xoshiro256.hpp"

class card;
class effect;
class field;
struct loc_info;

struct card_data {
	uint32_t code{};
	uint32_t alias{};
	std::set<uint16_t> setcodes;
	uint32_t type{};
	uint32_t level{};
	uint32_t attribute{};
	uint64_t race{};
	int32_t attack{};
	int32_t defense{};
	uint32_t lscale{};
	uint32_t rscale{};
	uint32_t link_marker{};
	card_data(const OCG_CardData& data);
	card_data() = default;
};

class duel {
public:
	class duel_message {
	private:
		template<typename T>
		void write_internal(T data) {
			write(&data, sizeof(T));
		}
	public:
		std::vector<uint8_t> data;
		explicit duel_message(uint8_t _message);
		void write(const void* buff, size_t size);
		void write(loc_info loc);
		template<typename T, typename T2>
		ForceInline void write(T2 data) {
			write_internal<T>(static_cast<T>(data));
		}
	};
	std::vector<uint8_t> buff;
	std::vector<uint8_t> query_buffer;
	field* game_field;
	interpreter* lua;
	std::unordered_set<card*> cards;
	std::unordered_set<card*> assumes;
	std::unordered_set<group*> groups;
	std::unordered_set<group*> sgroups;
	std::unordered_set<effect*> effects;
	std::unordered_set<effect*> uncopy;

	std::unordered_map<uint32_t, card_data> data_cache;

	enum class SCRIPT_LOAD_STATUS : uint8_t {
		NOT_LOADED,
		LOAD_SUCCEDED,
		LOAD_FAILED,
		LOADING,
	};

	std::unordered_map<uint32_t/* hashed string */, SCRIPT_LOAD_STATUS> loaded_scripts;
	
	duel() = delete;
	explicit duel(const OCG_DuelOptions& options);
	~duel();
	void clear();
	
	// run_initial_effect=false skips the initial_effect Lua call in
	// register_card. Used by serialize::load_state's pass 1, which
	// re-allocates effects from saved EffectRecords (pass 3) and
	// doesn't want the engine's initial_effect artifacts mixed in.
	card* new_card(uint32_t code, bool run_initial_effect = true);
	template<typename... Args>
	group* new_group(Args&&... args) {
		group* pgroup = new group(this, std::forward<Args>(args)...);
		groups.insert(pgroup);
		if(lua->call_depth)
			sgroups.insert(pgroup);
		lua->register_group(pgroup);
		return pgroup;
	}
	effect* new_effect();
	void delete_card(card* pcard);
	void delete_group(group* pgroup);
	void delete_effect(effect* peffect);
	void release_script_group();
	void restore_assumes();
	void generate_buffer();
	void write_buffer(const void* data, size_t size);
	void clear_buffer();
	void set_response(const void* resp, size_t len);
	int32_t get_next_integer(int32_t l, int32_t h);
	duel_message* new_message(uint8_t message);
	const card_data& read_card(uint32_t code);
	inline void handle_message(const char* message, OCG_LogTypes type) {
		handle_message_callback(handle_message_payload, message, type);
	}
	inline int read_script(const char* name) {
		return read_script_callback(read_script_payload, this, name);
	}

	// ===================================================================
	// SERIALIZATION_API — ExodAI Phase P1 Primitive 1
	// ===================================================================
	// Narrow accessors that exist solely so save_state.cpp / load_state.cpp
	// can reach internal state without making large field sets public. Each
	// is a one-liner that exposes existing private state for read or
	// targeted write; none change engine behavior.
	//
	// Intended consumers: ocgcore/serialize/*. General-purpose engine code
	// should NOT use these — engine code paths already have direct access
	// to `random` (and any future fields here). If you find yourself
	// reaching for one of these from inside ocgcore proper, the right
	// answer is usually to add a friend or a different abstraction; these
	// accessors are scoped to serialization only.
	//
	// The SERIALIZATION_API set will grow across chunks 5-7. When the
	// chunk-7 fork SHA bumps land, this block becomes the canonical home
	// for "yes, serialization needs this; no, this isn't general API."

	// chunk 3: RNG state read for save. The Xoshiro state is the only
	// piece of randomness (deck shuffle, coin flip, dice, effect RNG all
	// draw from this single stream); the serializer captures it as
	// 4×uint64.
	const RNG::Xoshiro256StarStar& get_rng() const { return random; }

	// chunk 4: RNG state write for load. Replaces the current Xoshiro
	// state with the saved state during deserialization. Caller is
	// responsible for ensuring the duel is in a well-defined state
	// otherwise.
	void set_rng_state(const RNG::Xoshiro256StarStar::StateType& s) {
		random = RNG::Xoshiro256StarStar(s);
	}
	// ===================================================================
	// END SERIALIZATION_API
	// ===================================================================
private:
	std::deque<duel_message> messages;
	RNG::Xoshiro256StarStar random;
	OCG_DataReader read_card_callback;
	OCG_ScriptReader read_script_callback;
	OCG_LogHandler handle_message_callback;
	OCG_DataReaderDone read_card_done_callback;
	void* read_card_payload;
	void* read_script_payload;
	void* handle_message_payload;
	void* read_card_done_payload;
};

#endif /* DUEL_H_ */
