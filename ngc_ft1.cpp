#include "./ngc_ft1.h"

#include "ngc_ext.hpp"

#include <vector>
#include <array>
#include <deque>
// TODO: should i really use both?
#include <unordered_map>
#include <map>
#include <optional>
#include <cassert>
#include <cstdio>

struct SendSequenceBuffer {
	struct SSBEntry {
		std::vector<uint8_t> data; // the data (variable size, but smaller than 500)
		float time_since_activity {0.f};
	};

	// sequence_id -> entry
	std::map<uint16_t, SSBEntry> entries;

	uint16_t next_seq_id {0};

	void erase(uint16_t seq) {
		entries.erase(seq);
	}

	// inflight chunks
	size_t size(void) const {
		return entries.size();
	}

	uint16_t add(std::vector<uint8_t>&& data) {
		entries[next_seq_id] = {data, 0.f};
		return next_seq_id++;
	}

	template<typename FN>
	void for_each(float time_delta, FN&& fn) {
		for (auto& [id, entry] : entries) {
			entry.time_since_activity += time_delta;
			fn(id, entry.data, entry.time_since_activity);
		}
	}
};

struct RecvSequenceBuffer {
	struct RSBEntry {
		std::vector<uint8_t> data;
	};

	// sequence_id -> entry
	std::map<uint16_t, RSBEntry> entries;

	uint16_t next_seq_id {0};

	// list of seq_ids to ack, this is seperate bc rsbentries are deleted once processed
	std::deque<uint16_t> ack_seq_ids;

	void erase(uint16_t seq) {
		entries.erase(seq);
	}

	// inflight chunks
	size_t size(void) const {
		return entries.size();
	}

	void add(uint16_t seq_id, std::vector<uint8_t>&& data) {
		entries[seq_id] = {data};
		ack_seq_ids.push_back(seq_id);
		if (ack_seq_ids.size() > 3) { // TODO: magic
			ack_seq_ids.pop_front();
		}
	}

	bool canPop(void) const {
		return entries.count(next_seq_id);
	}

	std::vector<uint8_t> pop(void) {
		assert(canPop());
		auto tmp_data = entries.at(next_seq_id).data;
		erase(next_seq_id);
		next_seq_id++;
		return tmp_data;
	}

	// for acking, might be bad since its front
	std::vector<uint16_t> frontSeqIDs(size_t count = 5) const {
		std::vector<uint16_t> seq_ids;
		auto it = entries.cbegin();
		for (size_t i = 0; i < 5 && it != entries.cend(); i++, it++) {
			seq_ids.push_back(it->first);
		}

		return seq_ids;
	}
};

struct NGC_FT1 {
	NGC_FT1_options options;

	std::unordered_map<NGC_FT1_file_kind, NGC_FT1_recv_request_cb*> cb_recv_request;
	std::unordered_map<NGC_FT1_file_kind, NGC_FT1_recv_init_cb*> cb_recv_init;
	std::unordered_map<NGC_FT1_file_kind, NGC_FT1_recv_data_cb*> cb_recv_data;
	std::unordered_map<NGC_FT1_file_kind, NGC_FT1_send_data_cb*> cb_send_data;
	std::unordered_map<NGC_FT1_file_kind, void*> ud_recv_request;
	std::unordered_map<NGC_FT1_file_kind, void*> ud_recv_init;
	std::unordered_map<NGC_FT1_file_kind, void*> ud_recv_data;
	std::unordered_map<NGC_FT1_file_kind, void*> ud_send_data;

	struct Group {
		struct Peer {
			struct RecvTransfer {
				NGC_FT1_file_kind file_kind;
				std::vector<uint8_t> file_id;

				enum class State {
					INITED, //init acked, but no data received yet (might be dropped)
					RECV, // receiving data
				} state;

				// float time_since_last_activity ?
				size_t file_size {0};
				size_t file_size_current {0};

				// sequence id based reassembly
				RecvSequenceBuffer rsb;
			};
			std::array<std::optional<RecvTransfer>, 256> recv_transfers;
			size_t next_recv_transfer_idx {0}; // next id will be 0

			struct SendTransfer {
				NGC_FT1_file_kind file_kind;
				std::vector<uint8_t> file_id;

				enum class State {
					INIT_SENT, // keep this state until ack or deny or giveup

					SENDING, // we got the ack and are now sending data

					FINISHING, // we sent all data but acks still outstanding????

					// delete
				} state;

				size_t inits_sent {1}; // is sent when creating

				float time_since_activity {0.f};
				size_t file_size {0};
				size_t file_size_current {0};

				// sequence array
				// list of sent but not acked seq_ids
				SendSequenceBuffer ssb;
			};
			std::array<std::optional<SendTransfer>, 256> send_transfers;
			size_t next_send_transfer_idx {0}; // next id will be 0
		};
		std::map<uint32_t, Peer> peers;
	};
	std::map<uint32_t, Group> groups;
};

// send pkgs
static bool _send_pkg_FT1_REQUEST(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t file_kind, const uint8_t* file_id, size_t file_id_size);
static bool _send_pkg_FT1_INIT(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t file_kind, uint64_t file_size, uint8_t transfer_id, const uint8_t* file_id, size_t file_id_size);
static bool _send_pkg_FT1_INIT_ACK(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id);
static bool _send_pkg_FT1_DATA(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id, uint16_t sequence_id, const uint8_t* data, size_t data_size);
static bool _send_pkg_FT1_DATA_ACK(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id, const uint16_t* seq_ids, size_t seq_ids_size);

// handle pkgs
static void _handle_FT1_REQUEST(Tox* tox, NGC_EXT_CTX* ngc_ext_ctx, uint32_t group_number, uint32_t peer_number, const uint8_t *data, size_t length, void* user_data);
static void _handle_FT1_INIT(Tox* tox, NGC_EXT_CTX* ngc_ext_ctx, uint32_t group_number, uint32_t peer_number, const uint8_t *data, size_t length, void* user_data);
static void _handle_FT1_INIT_ACK(Tox* tox, NGC_EXT_CTX* ngc_ext_ctx, uint32_t group_number, uint32_t peer_number, const uint8_t *data, size_t length, void* user_data);
static void _handle_FT1_DATA(Tox* tox, NGC_EXT_CTX* ngc_ext_ctx, uint32_t group_number, uint32_t peer_number, const uint8_t *data, size_t length, void* user_data);
static void _handle_FT1_DATA_ACK(Tox* tox, NGC_EXT_CTX* ngc_ext_ctx, uint32_t group_number, uint32_t peer_number, const uint8_t *data, size_t length, void* user_data);

NGC_FT1* NGC_FT1_new(const struct NGC_FT1_options* options) {
	NGC_FT1* ngc_ft1_ctx = new NGC_FT1;
	ngc_ft1_ctx->options = *options;
	return ngc_ft1_ctx;
}

bool NGC_FT1_register_ext(NGC_FT1* ngc_ft1_ctx, NGC_EXT_CTX* ngc_ext_ctx) {
	ngc_ext_ctx->callbacks[NGC_EXT::FT1_REQUEST] = _handle_FT1_REQUEST;
	ngc_ext_ctx->callbacks[NGC_EXT::FT1_INIT] = _handle_FT1_INIT;
	ngc_ext_ctx->callbacks[NGC_EXT::FT1_INIT_ACK] = _handle_FT1_INIT_ACK;
	ngc_ext_ctx->callbacks[NGC_EXT::FT1_DATA] = _handle_FT1_DATA;
	ngc_ext_ctx->callbacks[NGC_EXT::FT1_DATA_ACK] = _handle_FT1_DATA_ACK;

	ngc_ext_ctx->user_data[NGC_EXT::FT1_REQUEST] = ngc_ft1_ctx;
	ngc_ext_ctx->user_data[NGC_EXT::FT1_INIT] = ngc_ft1_ctx;
	ngc_ext_ctx->user_data[NGC_EXT::FT1_INIT_ACK] = ngc_ft1_ctx;
	ngc_ext_ctx->user_data[NGC_EXT::FT1_DATA] = ngc_ft1_ctx;
	ngc_ext_ctx->user_data[NGC_EXT::FT1_DATA_ACK] = ngc_ft1_ctx;

	return true;
}

void NGC_FT1_kill(NGC_FT1* ngc_ft1_ctx) {
	delete ngc_ft1_ctx;
}

void NGC_FT1_iterate(Tox *tox, NGC_FT1* ngc_ft1_ctx, float time_delta) {
	assert(ngc_ft1_ctx);

	for (auto& [group_number, group] : ngc_ft1_ctx->groups) {
		for (auto& [peer_number, peer] : group.peers) {
			//for (auto& tf_opt : peer.send_transfers) {
			for (size_t idx = 0; idx < peer.send_transfers.size(); idx++) {
				auto& tf_opt = peer.send_transfers[idx];
				if (tf_opt) {
					auto& tf = tf_opt.value();

					tf.time_since_activity += time_delta;

					switch (tf.state) {
						using State = NGC_FT1::Group::Peer::SendTransfer::State;
						case State::INIT_SENT:
							if (tf.time_since_activity >= ngc_ft1_ctx->options.init_retry_timeout_after) {
								if (tf.inits_sent >= 3) {
									// delete, timed out 3 times
									fprintf(stderr, "FT: warning, ft init timed out, deleting\n");
									tf_opt.reset();
									continue; // dangerous control flow
								} else {
									// timed out, resend
									fprintf(stderr, "FT: warning, ft init timed out, resending\n");
									_send_pkg_FT1_INIT(tox, group_number, peer_number, tf.file_kind, tf.file_size, idx, tf.file_id.data(), tf.file_id.size());
									tf.inits_sent++;
									tf.time_since_activity = 0.f;
								}
							}
							break;
						case State::SENDING: {
								tf.ssb.for_each(time_delta, [&](uint16_t id, const std::vector<uint8_t>& data, float& time_since_activity) {
									// no ack after 5 sec -> resend
									if (time_since_activity >= ngc_ft1_ctx->options.sending_resend_without_ack_after) {
										_send_pkg_FT1_DATA(tox, group_number, peer_number, idx, id, data.data(), data.size());
										time_since_activity = 0.f;
									}
								});

								if (tf.time_since_activity >= ngc_ft1_ctx->options.sending_give_up_after) {
									// no ack after 30sec, close ft
									// TODO: notify app
									fprintf(stderr, "FT: warning, sending ft in progress timed out, deleting\n");
									tf_opt.reset();
									continue; // dangerous control flow
								}

								assert(ngc_ft1_ctx->cb_send_data.count(tf.file_kind));

								// if chunks in flight < window size (2)
								while (tf.ssb.size() < ngc_ft1_ctx->options.packet_window_size) {
									std::vector<uint8_t> new_data;

									// TODO: parameterize packet size? -> only if JF increases lossy packet size >:)
									size_t chunk_size = std::min<size_t>(490u, tf.file_size - tf.file_size_current);
									if (chunk_size == 0) {
										tf.state = State::FINISHING;
										break; // we done
									}

									new_data.resize(chunk_size);

									ngc_ft1_ctx->cb_send_data[tf.file_kind](
										tox,
										group_number, peer_number,
										idx,
										tf.file_size_current,
										new_data.data(), new_data.size(),
										ngc_ft1_ctx->ud_send_data.count(tf.file_kind) ? ngc_ft1_ctx->ud_send_data.at(tf.file_kind) : nullptr
									);
									uint16_t seq_id = tf.ssb.add(std::move(new_data));
									_send_pkg_FT1_DATA(tox, group_number, peer_number, idx, seq_id, tf.ssb.entries.at(seq_id).data.data(), tf.ssb.entries.at(seq_id).data.size());

#if defined(EXTRA_LOGGING) && EXTRA_LOGGING == 1
									fprintf(stderr, "FT: sent data size: %ld (seq %d)\n", chunk_size, seq_id);
#endif

									tf.file_size_current += chunk_size;
								}
							}
							break;
						case State::FINISHING: // we still have unacked packets
							tf.ssb.for_each(time_delta, [&](uint16_t id, const std::vector<uint8_t>& data, float& time_since_activity) {
								// no ack after 5 sec -> resend
								if (time_since_activity >= ngc_ft1_ctx->options.sending_resend_without_ack_after) {
									_send_pkg_FT1_DATA(tox, group_number, peer_number, idx, id, data.data(), data.size());
									time_since_activity = 0.f;
								}
							});
							if (tf.time_since_activity >= ngc_ft1_ctx->options.sending_give_up_after) {
								// no ack after 30sec, close ft
								// TODO: notify app
								fprintf(stderr, "FT: warning, sending ft finishing timed out, deleting\n");
								tf_opt.reset();
							}
							break;
						default: // invalid state, delete
							fprintf(stderr, "FT: error, ft in invalid state, deleting\n");
							tf_opt.reset();
							continue;
					}
				}
			}
		}
	}
}

void NGC_FT1_register_callback_recv_request(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_request_cb* callback,
	void* user_data
) {
	assert(ngc_ft1_ctx);

	ngc_ft1_ctx->cb_recv_request[file_kind] = callback;
	ngc_ft1_ctx->ud_recv_request[file_kind] = user_data;
}

void NGC_FT1_register_callback_recv_init(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_init_cb* callback,
	void* user_data
) {
	assert(ngc_ft1_ctx);

	ngc_ft1_ctx->cb_recv_init[file_kind] = callback;
	ngc_ft1_ctx->ud_recv_init[file_kind] = user_data;
}

void NGC_FT1_register_callback_recv_data(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_data_cb* callback,
	void* user_data
) {
	assert(ngc_ft1_ctx);

	ngc_ft1_ctx->cb_recv_data[file_kind] = callback;
	ngc_ft1_ctx->ud_recv_data[file_kind] = user_data;
}

void NGC_FT1_register_callback_send_data(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_send_data_cb* callback,
	void* user_data
) {
	assert(ngc_ft1_ctx);

	ngc_ft1_ctx->cb_send_data[file_kind] = callback;
	ngc_ft1_ctx->ud_send_data[file_kind] = user_data;
}

void NGC_FT1_send_request_private(
	Tox *tox, NGC_FT1* ngc_ft1_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	NGC_FT1_file_kind file_kind,

	const uint8_t* file_id,
	size_t file_id_size
) {
	assert(tox);
	assert(ngc_ft1_ctx);

	// record locally that we sent(or want to send) the request?

	_send_pkg_FT1_REQUEST(tox, group_number, peer_number, file_kind, file_id, file_id_size);
}

bool NGC_FT1_send_init_private(
	Tox *tox, NGC_FT1* ngc_ft1_ctx,
	uint32_t group_number, uint32_t peer_number,
	NGC_FT1_file_kind file_kind,
	const uint8_t* file_id, size_t file_id_size,
	size_t file_size,
	uint8_t* transfer_id
) {
	//fprintf(stderr, "TODO: init ft for %08X\n", msg_id);
	fprintf(stderr, "FT: init ft\n");

	if (tox_group_peer_get_connection_status(tox, group_number, peer_number, nullptr) == TOX_CONNECTION_NONE) {
		fprintf(stderr, "FT: error: cant init ft, peer offline\n");
		return false;
	}

	auto& peer = ngc_ft1_ctx->groups[group_number].peers[peer_number];

	// allocate transfer_id
	size_t idx = peer.next_send_transfer_idx;
	peer.next_send_transfer_idx = (peer.next_send_transfer_idx + 1) % 256;
	{ // TODO: extract
		size_t i = idx;
		bool found = false;
		do {
			if (!peer.send_transfers[i].has_value()) {
				// free slot
				idx = i;
				found = true;
				break;
			}

			i = (i + 1) % 256;
		} while (i != idx);

		if (!found) {
			fprintf(stderr, "FT: error: cant init ft, no free transfer slot\n");
			return false;
		}
	}

	_send_pkg_FT1_INIT(tox, group_number, peer_number, file_kind, file_size, idx, file_id, file_id_size);

	peer.send_transfers[idx] = NGC_FT1::Group::Peer::SendTransfer{
		file_kind,
		std::vector(file_id, file_id+file_id_size),
		NGC_FT1::Group::Peer::SendTransfer::State::INIT_SENT,
		1,
		0.f,
		file_size,
		0,
	};

	if (transfer_id != nullptr) {
		*transfer_id = idx;
	}

	return true;
}

static bool _send_pkg_FT1_REQUEST(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t file_kind, const uint8_t* file_id, size_t file_id_size) {
	// - 1 byte packet id
	// - 1 byte (TODO: more?) file_kind
	// - X bytes file_id
	std::vector<uint8_t> pkg;
	pkg.push_back(NGC_EXT::FT1_REQUEST);
	pkg.push_back(file_kind);
	for (size_t i = 0; i < file_id_size; i++) {
		pkg.push_back(file_id[i]);
	}

	// lossless
	return tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

static bool _send_pkg_FT1_INIT(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t file_kind, uint64_t file_size, uint8_t transfer_id, const uint8_t* file_id, size_t file_id_size) {
	// - 1 byte packet id
	// - 1 byte (file_kind)
	// - 8 bytes (data size)
	// - 1 byte (temporary_file_tf_id, for this peer only, technically just a prefix to distinguish between simultainious fts)
	// - X bytes (file_kind dependent id, differnt sizes)

	std::vector<uint8_t> pkg;
	pkg.push_back(NGC_EXT::FT1_INIT);
	pkg.push_back(file_kind);
	for (size_t i = 0; i < sizeof(file_size); i++) {
		pkg.push_back((file_size>>(i*8)) & 0xff);
	}
	pkg.push_back(transfer_id);
	for (size_t i = 0; i < file_id_size; i++) {
		pkg.push_back(file_id[i]);
	}

	// lossless
	return tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

static bool _send_pkg_FT1_INIT_ACK(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id) {
	// send ack
	// - 1 byte packet id
	// - 1 byte transfer_id
	std::vector<uint8_t> pkg;
	pkg.push_back(NGC_EXT::FT1_INIT_ACK);
	pkg.push_back(transfer_id);

	// lossless
	return tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

static bool _send_pkg_FT1_DATA(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id, uint16_t sequence_id, const uint8_t* data, size_t data_size) {
	assert(data_size > 0);

	// TODO
	// check header_size+data_size <= max pkg size

	std::vector<uint8_t> pkg;
	pkg.push_back(NGC_EXT::FT1_DATA);
	pkg.push_back(transfer_id);
	pkg.push_back(sequence_id & 0xff);
	pkg.push_back((sequence_id >> (1*8)) & 0xff);

	// TODO: optimize
	for (size_t i = 0; i < data_size; i++) {
		pkg.push_back(data[i]);
	}

	// lossless?
	return tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

static bool _send_pkg_FT1_DATA_ACK(const Tox* tox, uint32_t group_number, uint32_t peer_number, uint8_t transfer_id, const uint16_t* seq_ids, size_t seq_ids_size) {
	std::vector<uint8_t> pkg;
	pkg.push_back(NGC_EXT::FT1_DATA_ACK);
	pkg.push_back(transfer_id);

	// TODO: optimize
	for (size_t i = 0; i < seq_ids_size; i++) {
		pkg.push_back(seq_ids[i] & 0xff);
		pkg.push_back((seq_ids[i] >> (1*8)) & 0xff);
	}

	// lossless?
	return tox_group_send_custom_private_packet(tox, group_number, peer_number, true, pkg.data(), pkg.size(), nullptr);
}

#define _DATA_HAVE(x, error) if ((length - curser) < (x)) { error; }

static void _handle_FT1_REQUEST(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	NGC_FT1* ngc_ft1_ctx = static_cast<NGC_FT1*>(user_data);
	size_t curser = 0;

	// TODO: might be uint16_t or even larger
	uint8_t file_kind_u8;
	_DATA_HAVE(sizeof(file_kind_u8), fprintf(stderr, "FT: packet too small, missing file_kind\n"); return)
	file_kind_u8 = data[curser++];
	auto file_kind = static_cast<NGC_FT1_file_kind>(file_kind_u8);

	fprintf(stderr, "FT: got FT request with file_kind %u [", file_kind_u8);
	for (size_t curser_copy = curser; curser_copy < length; curser_copy++) {
		fprintf(stderr, "%02X", data[curser_copy]);
	}
	fprintf(stderr, "]\n");

	NGC_FT1_recv_request_cb* fn_ptr = nullptr;
	if (ngc_ft1_ctx->cb_recv_request.count(file_kind)) {
		fn_ptr = ngc_ft1_ctx->cb_recv_request.at(file_kind);
	}
	void* ud_ptr = nullptr;
	if (ngc_ft1_ctx->ud_recv_request.count(file_kind)) {
		ud_ptr = ngc_ft1_ctx->ud_recv_request.at(file_kind);
	}
	if (fn_ptr) {
		fn_ptr(tox, group_number, peer_number, data+curser, length-curser, ud_ptr);
	} else {
		fprintf(stderr, "FT: missing cb for requests\n");
	}
}

static void _handle_FT1_INIT(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	NGC_FT1* ngc_ft1_ctx = static_cast<NGC_FT1*>(user_data);
	size_t curser = 0;

	// - 1 byte (file_kind)

	// TODO: might be uint16_t or even larger
	uint8_t file_kind_u8;
	_DATA_HAVE(sizeof(file_kind_u8), fprintf(stderr, "FT: packet too small, missing file_kind\n"); return)
	file_kind_u8 = data[curser++];
	auto file_kind = static_cast<NGC_FT1_file_kind>(file_kind_u8);

	// - 8 bytes (data size)
	size_t file_size {0u};
	_DATA_HAVE(sizeof(file_size), fprintf(stderr, "FT: packet too small, missing file_size\n"); return)
	for (size_t i = 0; i < sizeof(file_size); i++, curser++) {
		file_size |= size_t(data[curser]) << (i*8);
	}

	// - 1 byte (temporary_file_tf_id, for this peer only, technically just a prefix to distinguish between simultainious fts)
	uint8_t transfer_id;
	_DATA_HAVE(sizeof(transfer_id), fprintf(stderr, "FT: packet too small, missing transfer_id\n"); return)
	transfer_id = data[curser++];

	// - X bytes (file_kind dependent id, differnt sizes)

	const std::vector file_id(data+curser, data+curser+(length-curser));
	fprintf(stderr, "FT: got FT init with file_kind:%u file_size:%lu tf_id:%u [", file_kind_u8, file_size, transfer_id);
	for (size_t curser_copy = curser; curser_copy < length; curser_copy++) {
		fprintf(stderr, "%02X", data[curser_copy]);
	}
	fprintf(stderr, "]\n");

	// check if slot free ?
	// did we allready ack this and the other side just did not see the ack?

	NGC_FT1_recv_init_cb* fn_ptr = nullptr;
	if (ngc_ft1_ctx->cb_recv_init.count(file_kind)) {
		fn_ptr = ngc_ft1_ctx->cb_recv_init.at(file_kind);
	}
	void* ud_ptr = nullptr;
	if (ngc_ft1_ctx->ud_recv_init.count(file_kind)) {
		ud_ptr = ngc_ft1_ctx->ud_recv_init.at(file_kind);
	}
	bool accept_ft;
	if (fn_ptr) {
		// last part of message (file_id) is not yet parsed, just give it to cb
		accept_ft = fn_ptr(tox, group_number, peer_number, data+curser, length-curser, transfer_id, file_size, ud_ptr);
	} else {
		fprintf(stderr, "FT: missing cb for init\n");
		accept_ft = false;
	}

	if (accept_ft) {
		_send_pkg_FT1_INIT_ACK(tox, group_number, peer_number, transfer_id);
#if defined(EXTRA_LOGGING) && EXTRA_LOGGING == 1
		fprintf(stderr, "FT: accepted init\n");
#endif
		auto& peer = ngc_ft1_ctx->groups[group_number].peers[peer_number];
		if (peer.recv_transfers[transfer_id].has_value()) {
			fprintf(stderr, "FT: overwriting existing recv_transfer %d\n", transfer_id);
		}

		peer.recv_transfers[transfer_id] = NGC_FT1::Group::Peer::RecvTransfer{
			file_kind,
			file_id,
			NGC_FT1::Group::Peer::RecvTransfer::State::INITED,
			file_size,
			0u,
		};
	} else {
		// TODO deny?
		fprintf(stderr, "FT: rejected init\n");
	}
}

static void _handle_FT1_INIT_ACK(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	NGC_FT1* ngc_ft1_ctx = static_cast<NGC_FT1*>(user_data);
	size_t curser = 0;

	// - 1 byte (transfer_id)
	uint8_t transfer_id;
	_DATA_HAVE(sizeof(transfer_id), fprintf(stderr, "FT: packet too small, missing transfer_id\n"); return)
	transfer_id = data[curser++];

	// we now should start sending data

	auto& groups = ngc_ft1_ctx->groups;
	if (!groups.count(group_number)) {
		fprintf(stderr, "FT: init_ack for unknown group\n");
		return;
	}

	NGC_FT1::Group::Peer& peer = groups[group_number].peers[peer_number];
	if (!peer.send_transfers[transfer_id].has_value()) {
		fprintf(stderr, "FT: inti_ack for unknown transfer\n");
		return;
	}

	NGC_FT1::Group::Peer::SendTransfer& transfer = peer.send_transfers[transfer_id].value();

	using State = NGC_FT1::Group::Peer::SendTransfer::State;
	if (transfer.state != State::INIT_SENT) {
		fprintf(stderr, "FT: inti_ack but not in INIT_SENT state\n");
		return;
	}

	// iterate will now call NGC_FT1_send_data_cb
	transfer.state = State::SENDING;
	transfer.time_since_activity = 0.f;
}

static void _handle_FT1_DATA(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data, size_t length,
	void* user_data
) {
	NGC_FT1* ngc_ft1_ctx = static_cast<NGC_FT1*>(user_data);
	size_t curser = 0;

	// - 1 byte (transfer_id)
	uint8_t transfer_id;
	_DATA_HAVE(sizeof(transfer_id), fprintf(stderr, "FT: packet too small, missing transfer_id\n"); return)
	transfer_id = data[curser++];

	// - 2 bytes (sequence_id)
	uint16_t sequence_id;
	_DATA_HAVE(sizeof(sequence_id), fprintf(stderr, "FT: packet too small, missing sequence_id\n"); return)
	sequence_id = data[curser++];
	sequence_id |= data[curser++] << (1*8);

	if (curser == length) {
		fprintf(stderr, "FT: data of size 0!\n");
		return;
	}

	auto& groups = ngc_ft1_ctx->groups;
	if (!groups.count(group_number)) {
		fprintf(stderr, "FT: data for unknown group\n");
		return;
	}

	NGC_FT1::Group::Peer& peer = groups[group_number].peers[peer_number];
	if (!peer.recv_transfers[transfer_id].has_value()) {
		fprintf(stderr, "FT: data for unknown transfer\n");
		return;
	}

	auto& transfer = peer.recv_transfers[transfer_id].value();

	// do reassembly, ignore dups
	transfer.rsb.add(sequence_id, std::vector<uint8_t>(data+curser, data+curser+(length-curser)));

	NGC_FT1_recv_data_cb* fn_ptr = nullptr;
	if (ngc_ft1_ctx->cb_recv_data.count(transfer.file_kind)) {
		fn_ptr = ngc_ft1_ctx->cb_recv_data.at(transfer.file_kind);
	}

	void* ud_ptr = nullptr;
	if (ngc_ft1_ctx->ud_recv_data.count(transfer.file_kind)) {
		ud_ptr = ngc_ft1_ctx->ud_recv_data.at(transfer.file_kind);
	}

	if (!fn_ptr) {
		fprintf(stderr, "FT: missing cb for recv_data\n");
		return;
	}

	// loop for chunks without holes
	while (transfer.rsb.canPop()) {
		auto data = transfer.rsb.pop();

		fn_ptr(tox, group_number, peer_number, transfer_id, transfer.file_size_current, data.data(), data.size(), ud_ptr);

		transfer.file_size_current += data.size();
	}

	// send acks
	std::vector<uint16_t> ack_seq_ids(transfer.rsb.ack_seq_ids.cbegin(), transfer.rsb.ack_seq_ids.cend());
	if (!ack_seq_ids.empty()) {
		_send_pkg_FT1_DATA_ACK(tox, group_number, peer_number, transfer_id, ack_seq_ids.data(), ack_seq_ids.size());
	}
}

static void _handle_FT1_DATA_ACK(
	Tox* tox,
	NGC_EXT_CTX* ngc_ext_ctx,

	uint32_t group_number,
	uint32_t peer_number,

	const uint8_t *data,
	size_t length,
	void* user_data
) {
	NGC_FT1* ngc_ft1_ctx = static_cast<NGC_FT1*>(user_data);
	size_t curser = 0;

	// - 1 byte (transfer_id)
	uint8_t transfer_id;
	_DATA_HAVE(sizeof(transfer_id), fprintf(stderr, "FT: packet too small, missing transfer_id\n"); return)
	transfer_id = data[curser++];

	auto& groups = ngc_ft1_ctx->groups;
	if (!groups.count(group_number)) {
		fprintf(stderr, "FT: data_ack for unknown group\n");
		return;
	}

	NGC_FT1::Group::Peer& peer = groups[group_number].peers[peer_number];
	if (!peer.send_transfers[transfer_id].has_value()) {
		fprintf(stderr, "FT: data_ack for unknown transfer\n");
		return;
	}

	NGC_FT1::Group::Peer::SendTransfer& transfer = peer.send_transfers[transfer_id].value();

	using State = NGC_FT1::Group::Peer::SendTransfer::State;
	if (transfer.state != State::SENDING && transfer.state != State::FINISHING) {
		fprintf(stderr, "FT: data_ack but not in SENDING or FINISHING state (%d)\n", int(transfer.state));
		return;
	}

	_DATA_HAVE(sizeof(uint16_t), fprintf(stderr, "FT: packet too small, atleast 1 seq_id\n"); return)

	if ((length - curser) % sizeof(uint16_t) != 0) {
		fprintf(stderr, "FT: data_ack with misaligned data\n");
		return;
	}

	transfer.time_since_activity = 0.f;

	while (curser < length) {
		uint16_t seq_id = data[curser++];
		seq_id |= data[curser++] << (1*8);

		transfer.ssb.erase(seq_id);
	}

	// delete if all packets acked
	if (transfer.file_size == transfer.file_size_current && transfer.ssb.size() == 0) {
		fprintf(stderr, "FT: %d done\n", transfer_id);
		peer.send_transfers[transfer_id] = std::nullopt;
	}
}

#undef _DATA_HAVE

