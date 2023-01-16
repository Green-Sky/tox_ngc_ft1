#ifndef C_NGC_FT1_H
#define C_NGC_FT1_H

// this is a c header

#include <tox/tox.h>

#include "ngc_ext.h"

#ifdef __cplusplus
extern "C" {
#endif


// ========== struct / typedef ==========

typedef struct NGC_FT1 NGC_FT1;

struct NGC_FT1_options {
	// TODO
	size_t acks_per_packet; // 5

	float init_retry_timeout_after; // 10sec

	float sending_resend_without_ack_after; // 5sec
	float sending_give_up_after; // 30sec

	size_t packet_window_size; // 2
};

// uint16_t ?
// ffs c does not allow types
typedef enum NGC_FT1_file_kind /*: uint8_t*/ {
	//INVALID = 0u,

	// id:
	// group (implicit)
	// peer pub key + msg_id
	NGC_HS1_MESSAGE_BY_ID = 1u, // history sync PoC 1

	// id: TOX_FILE_ID_LENGTH (32) bytes
	// this is basically and id and probably not a hash, like the tox friend api
	// this id can be unique between 2 peers
	ID = 8u,

	// id: hash of the info, like a torrent infohash (using the same hash as the data)
	// TODO: determain internal format
	// draft: (for single file)
	//   - 256 bytes | filename
	//   - 8bytes | file size
	//   - fixed chunk size of 4kb
	//   - array of chunk hashes (ids) [
	//     - SHAX bytes
	//   - ]
	HASH_SHA1_INFO,
	HASH_SHA2_INFO,

	// id: hash of the content
	// TODO: fixed chunk size or variable (defined in info)
	// if "variable" sized, it can be aliased with TORRENT_VX_CHUNK in the implementation
	HASH_SHA1_CHUNK,
	HASH_SHA2_CHUNK,

	// :)
	// draft for fun and profit
	// TODO: should we even support v1?
	// TODO: design the same thing again for tox? (msg_pack instead of bencode?)
	// id: infohash
	TORRENT_V1_METAINFO,
	// id: sha1
	TORRENT_V1_CHUNK, // alias with SHA1_CHUNK?

	// id: infohash
	TORRENT_V2_METAINFO, // meta info is kind of more complicated than that <.<
	// id: sha256
	TORRENT_V2_CHUNK,
} NGC_FT1_file_kind;

// ========== init / kill ==========
// (see tox api)
NGC_FT1* NGC_FT1_new(const struct NGC_FT1_options* options);
bool NGC_FT1_register_ext(NGC_FT1* ngc_ft1_ctx, NGC_EXT_CTX* ngc_ext_ctx);
void NGC_FT1_kill(NGC_FT1* ngc_ft1_ctx);

// ========== iterate ==========
// time_delta in seconds
void NGC_FT1_iterate(Tox *tox, NGC_FT1* ngc_ft1_ctx, float time_delta);

// TODO: announce
// ========== request ==========

// TODO: public variant?
void NGC_FT1_send_request_private(
	Tox *tox, NGC_FT1* ngc_ft1_ctx,
	uint32_t group_number, uint32_t peer_number,
	NGC_FT1_file_kind file_kind,
	const uint8_t* file_id, size_t file_id_size
);

typedef void NGC_FT1_recv_request_cb(
	Tox *tox,
	uint32_t group_number, uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	void* user_data
);

void NGC_FT1_register_callback_recv_request(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_request_cb* callback,
	void* user_data
);

// ========== send/accept ==========

// public does not make sense here
bool NGC_FT1_send_init_private(
	Tox *tox, NGC_FT1* ngc_ft1_ctx,
	uint32_t group_number, uint32_t peer_number,
	NGC_FT1_file_kind file_kind,
	const uint8_t* file_id, size_t file_id_size,
	size_t file_size,
	uint8_t* transfer_id
);

// return true to accept, false to deny
typedef bool NGC_FT1_recv_init_cb(
	Tox *tox,
	uint32_t group_number, uint32_t peer_number,
	const uint8_t* file_id, size_t file_id_size,
	const uint8_t transfer_id,
	const size_t file_size,
	void* user_data
);

void NGC_FT1_register_callback_recv_init(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_init_cb* callback,
	void* user_data
);

// ========== data ==========

typedef void NGC_FT1_recv_data_cb(
	Tox *tox,

	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,

	size_t data_offset, const uint8_t* data, size_t data_size,
	void* user_data
);

void NGC_FT1_register_callback_recv_data(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_recv_data_cb* callback,
	void* user_data
);

// request to fill data_size bytes into data
typedef void NGC_FT1_send_data_cb(
	Tox *tox,

	uint32_t group_number,
	uint32_t peer_number,
	uint8_t transfer_id,

	size_t data_offset, uint8_t* data, size_t data_size,
	void* user_data
);

void NGC_FT1_register_callback_send_data(
	NGC_FT1* ngc_ft1_ctx,
	NGC_FT1_file_kind file_kind,
	NGC_FT1_send_data_cb* callback,
	void* user_data
);


// ========== peer online/offline ==========
//void NGC_FT1_peer_online(Tox* tox, NGC_FT1* ngc_hs1_ctx, uint32_t group_number, uint32_t peer_number, bool online);

#ifdef __cplusplus
}
#endif

#endif // C_NGC_FT1_H

