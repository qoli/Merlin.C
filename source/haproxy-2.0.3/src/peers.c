/*
 * Peer synchro management.
 *
 * Copyright 2010 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/time.h>
#include <common/standard.h>
#include <common/hathreads.h>

#include <types/global.h>
#include <types/listener.h>
#include <types/obj_type.h>
#include <types/peers.h>
#include <types/stats.h>

#include <proto/acl.h>
#include <proto/applet.h>
#include <proto/channel.h>
#include <proto/cli.h>
#include <proto/dict.h>
#include <proto/fd.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/hdr_idx.h>
#include <proto/mux_pt.h>
#include <proto/peers.h>
#include <proto/proxy.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/signal.h>
#include <proto/stick_table.h>
#include <proto/stream_interface.h>
#include <proto/task.h>


/*******************************/
/* Current peer learning state */
/*******************************/

/******************************/
/* Current peers section resync state */
/******************************/
#define PEERS_F_RESYNC_LOCAL        0x00000001 /* Learn from local finished or no more needed */
#define PEERS_F_RESYNC_REMOTE       0x00000002 /* Learn from remote finished or no more needed */
#define PEERS_F_RESYNC_ASSIGN       0x00000004 /* A peer was assigned to learn our lesson */
#define PEERS_F_RESYNC_PROCESS      0x00000008 /* The assigned peer was requested for resync */
#define PEERS_F_DONOTSTOP           0x00010000 /* Main table sync task block process during soft stop
                                                  to push data to new process */

#define PEERS_RESYNC_STATEMASK      (PEERS_F_RESYNC_LOCAL|PEERS_F_RESYNC_REMOTE)
#define PEERS_RESYNC_FROMLOCAL      0x00000000
#define PEERS_RESYNC_FROMREMOTE     PEERS_F_RESYNC_LOCAL
#define PEERS_RESYNC_FINISHED       (PEERS_F_RESYNC_LOCAL|PEERS_F_RESYNC_REMOTE)

/***********************************/
/* Current shared table sync state */
/***********************************/
#define SHTABLE_F_TEACH_STAGE1      0x00000001 /* Teach state 1 complete */
#define SHTABLE_F_TEACH_STAGE2      0x00000002 /* Teach state 2 complete */

/******************************/
/* Remote peer teaching state */
/******************************/
#define PEER_F_TEACH_PROCESS        0x00000001 /* Teach a lesson to current peer */
#define PEER_F_TEACH_FINISHED       0x00000008 /* Teach conclude, (wait for confirm) */
#define PEER_F_TEACH_COMPLETE       0x00000010 /* All that we know already taught to current peer, used only for a local peer */
#define PEER_F_LEARN_ASSIGN         0x00000100 /* Current peer was assigned for a lesson */
#define PEER_F_LEARN_NOTUP2DATE     0x00000200 /* Learn from peer finished but peer is not up to date */
#define PEER_F_ALIVE                0x20000000 /* Used to flag a peer a alive. */
#define PEER_F_HEARTBEAT            0x40000000 /* Heartbeat message to send. */
#define PEER_F_DWNGRD               0x80000000 /* When this flag is enabled, we must downgrade the supported version announced during peer sessions. */

#define PEER_TEACH_RESET            ~(PEER_F_TEACH_PROCESS|PEER_F_TEACH_FINISHED) /* PEER_F_TEACH_COMPLETE should never be reset */
#define PEER_LEARN_RESET            ~(PEER_F_LEARN_ASSIGN|PEER_F_LEARN_NOTUP2DATE)

#define PEER_RESYNC_TIMEOUT         5000 /* 5 seconds */
#define PEER_RECONNECT_TIMEOUT      5000 /* 5 seconds */
#define PEER_HEARTBEAT_TIMEOUT      3000 /* 3 seconds */

/*****************************/
/* Sync message class        */
/*****************************/
enum {
	PEER_MSG_CLASS_CONTROL = 0,
	PEER_MSG_CLASS_ERROR,
	PEER_MSG_CLASS_STICKTABLE = 10,
	PEER_MSG_CLASS_RESERVED = 255,
};

/*****************************/
/* control message types     */
/*****************************/
enum {
	PEER_MSG_CTRL_RESYNCREQ = 0,
	PEER_MSG_CTRL_RESYNCFINISHED,
	PEER_MSG_CTRL_RESYNCPARTIAL,
	PEER_MSG_CTRL_RESYNCCONFIRM,
	PEER_MSG_CTRL_HEARTBEAT,
};

/*****************************/
/* error message types       */
/*****************************/
enum {
	PEER_MSG_ERR_PROTOCOL = 0,
	PEER_MSG_ERR_SIZELIMIT,
};

/*
 * Parameters used by functions to build peer protocol messages. */
struct peer_prep_params {
	struct {
		struct peer *peer;
	} hello;
	struct {
		unsigned int st1;
	} error_status;
	struct {
		struct stksess *stksess;
		struct shared_table *shared_table;
		unsigned int updateid;
		int use_identifier;
		int use_timed;
		struct peer *peer;
	} updt;
	struct {
		struct shared_table *shared_table;
	} swtch;
	struct {
		struct shared_table *shared_table;
	} ack;
	struct {
		unsigned char head[2];
	} control;
	struct {
		unsigned char head[2];
	} error;
};

/*******************************/
/* stick table sync mesg types */
/* Note: ids >= 128 contains   */
/* id message cotains data     */
/*******************************/
#define PEER_MSG_STKT_UPDATE           0x80
#define PEER_MSG_STKT_INCUPDATE        0x81
#define PEER_MSG_STKT_DEFINE           0x82
#define PEER_MSG_STKT_SWITCH           0x83
#define PEER_MSG_STKT_ACK              0x84
#define PEER_MSG_STKT_UPDATE_TIMED     0x85
#define PEER_MSG_STKT_INCUPDATE_TIMED  0x86
/* All the stick-table message identifiers abova have the #7 bit set */
#define PEER_MSG_STKT_BIT                 7
#define PEER_MSG_STKT_BIT_MASK         (1 << PEER_MSG_STKT_BIT)

/* The maximum length of an encoded data length. */
#define PEER_MSG_ENC_LENGTH_MAXLEN    5

/* Minimum 64-bits value encoded with 2 bytes */
#define PEER_ENC_2BYTES_MIN                                  0xf0 /*               0xf0 (or 240) */
/* 3 bytes */
#define PEER_ENC_3BYTES_MIN  ((1ULL << 11) | PEER_ENC_2BYTES_MIN) /*              0x8f0 (or 2288) */
/* 4 bytes */
#define PEER_ENC_4BYTES_MIN  ((1ULL << 18) | PEER_ENC_3BYTES_MIN) /*            0x408f0 (or 264432) */
/* 5 bytes */
#define PEER_ENC_5BYTES_MIN  ((1ULL << 25) | PEER_ENC_4BYTES_MIN) /*          0x20408f0 (or 33818864) */
/* 6 bytes */
#define PEER_ENC_6BYTES_MIN  ((1ULL << 32) | PEER_ENC_5BYTES_MIN) /*        0x1020408f0 (or 4328786160) */
/* 7 bytes */
#define PEER_ENC_7BYTES_MIN  ((1ULL << 39) | PEER_ENC_6BYTES_MIN) /*       0x81020408f0 (or 554084600048) */
/* 8 bytes */
#define PEER_ENC_8BYTES_MIN  ((1ULL << 46) | PEER_ENC_7BYTES_MIN) /*     0x4081020408f0 (or 70922828777712) */
/* 9 bytes */
#define PEER_ENC_9BYTES_MIN  ((1ULL << 53) | PEER_ENC_8BYTES_MIN) /*   0x204081020408f0 (or 9078122083518704) */
/* 10 bytes */
#define PEER_ENC_10BYTES_MIN ((1ULL << 60) | PEER_ENC_9BYTES_MIN) /* 0x10204081020408f0 (or 1161999626690365680) */

/* #7 bit used to detect the last byte to be encoded */
#define PEER_ENC_STOP_BIT         7
/* The byte minimum value with #7 bit set */
#define PEER_ENC_STOP_BYTE        (1 << PEER_ENC_STOP_BIT)
/* The left most number of bits set for PEER_ENC_2BYTES_MIN */
#define PEER_ENC_2BYTES_MIN_BITS  4

#define PEER_MSG_HEADER_LEN               2

#define PEER_STKT_CACHE_MAX_ENTRIES       128

/**********************************/
/* Peer Session IO handler states */
/**********************************/

enum {
	PEER_SESS_ST_ACCEPT = 0,     /* Initial state for session create by an accept, must be zero! */
	PEER_SESS_ST_GETVERSION,     /* Validate supported protocol version */
	PEER_SESS_ST_GETHOST,        /* Validate host ID correspond to local host id */
	PEER_SESS_ST_GETPEER,        /* Validate peer ID correspond to a known remote peer id */
	/* after this point, data were possibly exchanged */
	PEER_SESS_ST_SENDSUCCESS,    /* Send ret code 200 (success) and wait for message */
	PEER_SESS_ST_CONNECT,        /* Initial state for session create on a connect, push presentation into buffer */
	PEER_SESS_ST_GETSTATUS,      /* Wait for the welcome message */
	PEER_SESS_ST_WAITMSG,        /* Wait for data messages */
	PEER_SESS_ST_EXIT,           /* Exit with status code */
	PEER_SESS_ST_ERRPROTO,       /* Send error proto message before exit */
	PEER_SESS_ST_ERRSIZE,        /* Send error size message before exit */
	PEER_SESS_ST_END,            /* Killed session */
};

/***************************************************/
/* Peer Session status code - part of the protocol */
/***************************************************/

#define PEER_SESS_SC_CONNECTCODE    100 /* connect in progress */
#define PEER_SESS_SC_CONNECTEDCODE  110 /* tcp connect success */

#define PEER_SESS_SC_SUCCESSCODE    200 /* accept or connect successful */

#define PEER_SESS_SC_TRYAGAIN       300 /* try again later */

#define PEER_SESS_SC_ERRPROTO       501 /* error protocol */
#define PEER_SESS_SC_ERRVERSION     502 /* unknown protocol version */
#define PEER_SESS_SC_ERRHOST        503 /* bad host name */
#define PEER_SESS_SC_ERRPEER        504 /* unknown peer */

#define PEER_SESSION_PROTO_NAME         "HAProxyS"
#define PEER_MAJOR_VER        2
#define PEER_MINOR_VER        1
#define PEER_DWNGRD_MINOR_VER 0

static size_t proto_len = sizeof(PEER_SESSION_PROTO_NAME) - 1;
struct peers *cfg_peers = NULL;
static void peer_session_forceshutdown(struct peer *peer);

static struct ebpt_node *dcache_tx_insert(struct dcache *dc,
                                          struct dcache_tx_entry *i);
static inline void flush_dcache(struct peer *peer);

static const char *statuscode_str(int statuscode)
{
	switch (statuscode) {
	case PEER_SESS_SC_CONNECTCODE:
		return "CONN";
	case PEER_SESS_SC_CONNECTEDCODE:
		return "HSHK";
	case PEER_SESS_SC_SUCCESSCODE:
		return "ESTA";
	case PEER_SESS_SC_TRYAGAIN:
		return "RETR";
	case PEER_SESS_SC_ERRPROTO:
		return "PROT";
	case PEER_SESS_SC_ERRVERSION:
		return "VERS";
	case PEER_SESS_SC_ERRHOST:
		return "NAME";
	case PEER_SESS_SC_ERRPEER:
		return "UNKN";
	default:
		return "NONE";
	}
}

/* This function encode an uint64 to 'dynamic' length format.
   The encoded value is written at address *str, and the
   caller must assure that size after *str is large enought.
   At return, the *str is set at the next Byte after then
   encoded integer. The function returns then length of the
   encoded integer in Bytes */
int intencode(uint64_t i, char **str) {
	int idx = 0;
	unsigned char *msg;

	msg = (unsigned char *)*str;
	if (i < PEER_ENC_2BYTES_MIN) {
		msg[0] = (unsigned char)i;
		*str = (char *)&msg[idx+1];
		return (idx+1);
	}

	msg[idx] =(unsigned char)i | PEER_ENC_2BYTES_MIN;
	i = (i - PEER_ENC_2BYTES_MIN) >> PEER_ENC_2BYTES_MIN_BITS;
	while (i >= PEER_ENC_STOP_BYTE) {
		msg[++idx] = (unsigned char)i | PEER_ENC_STOP_BYTE;
		i = (i - PEER_ENC_STOP_BYTE) >> PEER_ENC_STOP_BIT;
	}
	msg[++idx] = (unsigned char)i;
	*str = (char *)&msg[idx+1];
	return (idx+1);
}


/* This function returns the decoded integer or 0
   if decode failed
   *str point on the beginning of the integer to decode
   at the end of decoding *str point on the end of the
   encoded integer or to null if end is reached */
uint64_t intdecode(char **str, char *end)
{
	unsigned char *msg;
	uint64_t i;
	int shift;

	if (!*str)
		return 0;

	msg = (unsigned char *)*str;
	if (msg >= (unsigned char *)end)
		goto fail;

	i = *(msg++);
	if (i >= PEER_ENC_2BYTES_MIN) {
		shift = PEER_ENC_2BYTES_MIN_BITS;
		do {
			if (msg >= (unsigned char *)end)
				goto fail;
			i += (uint64_t)*msg << shift;
			shift += PEER_ENC_STOP_BIT;
		} while (*(msg++) >= PEER_ENC_STOP_BYTE);
	}
	*str = (char *)msg;
	return i;

 fail:
	*str = NULL;
	return 0;
}

/*
 * Build a "hello" peer protocol message.
 * Return the number of written bytes written to build this messages if succeeded,
 * 0 if not.
 */
static int peer_prepare_hellomsg(char *msg, size_t size, struct peer_prep_params *p)
{
	int min_ver, ret;
	struct peer *peer;

	peer = p->hello.peer;
	min_ver = (peer->flags & PEER_F_DWNGRD) ? PEER_DWNGRD_MINOR_VER : PEER_MINOR_VER;
	/* Prepare headers */
	ret = snprintf(msg, size, PEER_SESSION_PROTO_NAME " %u.%u\n%s\n%s %d %d\n",
	              PEER_MAJOR_VER, min_ver, peer->id, localpeer, (int)getpid(), relative_pid);
	if (ret >= size)
		return 0;

	return ret;
}

/*
 * Build a "handshake succeeded" status message.
 * Return the number of written bytes written to build this messages if succeeded,
 * 0 if not.
 */
static int peer_prepare_status_successmsg(char *msg, size_t size, struct peer_prep_params *p)
{
	int ret;

	ret = snprintf(msg, size, "%d\n", PEER_SESS_SC_SUCCESSCODE);
	if (ret >= size)
		return 0;

	return ret;
}

/*
 * Build an error status message.
 * Return the number of written bytes written to build this messages if succeeded,
 * 0 if not.
 */
static int peer_prepare_status_errormsg(char *msg, size_t size, struct peer_prep_params *p)
{
	int ret;
	unsigned int st1;

	st1 = p->error_status.st1;
	ret = snprintf(msg, size, "%d\n", st1);
	if (ret >= size)
		return 0;

	return ret;
}

/* Set the stick-table UPDATE message type byte at <msg_type> address,
 * depending on <use_identifier> and <use_timed> boolean parameters.
 * Always successful.
 */
static inline void peer_set_update_msg_type(char *msg_type, int use_identifier, int use_timed)
{
	if (use_timed) {
		if (use_identifier)
			*msg_type = PEER_MSG_STKT_UPDATE_TIMED;
		else
			*msg_type = PEER_MSG_STKT_INCUPDATE_TIMED;
	}
	else {
		if (use_identifier)
			*msg_type = PEER_MSG_STKT_UPDATE;
		else
			*msg_type = PEER_MSG_STKT_INCUPDATE;
	}
}
/*
 * This prepare the data update message on the stick session <ts>, <st> is the considered
 * stick table.
 *  <msg> is a buffer of <size> to receive data message content
 * If function returns 0, the caller should consider we were unable to encode this message (TODO:
 * check size)
 */
static int peer_prepare_updatemsg(char *msg, size_t size, struct peer_prep_params *p)
{
	uint32_t netinteger;
	unsigned short datalen;
	char *cursor, *datamsg;
	unsigned int data_type;
	void *data_ptr;
	struct stksess *ts;
	struct shared_table *st;
	unsigned int updateid;
	int use_identifier;
	int use_timed;
	struct peer *peer;

	ts = p->updt.stksess;
	st = p->updt.shared_table;
	updateid = p->updt.updateid;
	use_identifier = p->updt.use_identifier;
	use_timed = p->updt.use_timed;
	peer = p->updt.peer;

	cursor = datamsg = msg + PEER_MSG_HEADER_LEN + PEER_MSG_ENC_LENGTH_MAXLEN;

	/* construct message */

	/* check if we need to send the update identifer */
	if (!st->last_pushed || updateid < st->last_pushed || ((updateid - st->last_pushed) != 1)) {
		use_identifier = 1;
	}

	/* encode update identifier if needed */
	if (use_identifier)  {
		netinteger = htonl(updateid);
		memcpy(cursor, &netinteger, sizeof(netinteger));
		cursor += sizeof(netinteger);
	}

	if (use_timed) {
		netinteger = htonl(tick_remain(now_ms, ts->expire));
		memcpy(cursor, &netinteger, sizeof(netinteger));
		cursor += sizeof(netinteger);
	}

	/* encode the key */
	if (st->table->type == SMP_T_STR) {
		int stlen = strlen((char *)ts->key.key);

		intencode(stlen, &cursor);
		memcpy(cursor, ts->key.key, stlen);
		cursor += stlen;
	}
	else if (st->table->type == SMP_T_SINT) {
		netinteger = htonl(*((uint32_t *)ts->key.key));
		memcpy(cursor, &netinteger, sizeof(netinteger));
		cursor += sizeof(netinteger);
	}
	else {
		memcpy(cursor, ts->key.key, st->table->key_size);
		cursor += st->table->key_size;
	}

	HA_RWLOCK_RDLOCK(STK_SESS_LOCK, &ts->lock);
	/* encode values */
	for (data_type = 0 ; data_type < STKTABLE_DATA_TYPES ; data_type++) {

		data_ptr = stktable_data_ptr(st->table, ts, data_type);
		if (data_ptr) {
			switch (stktable_data_types[data_type].std_type) {
				case STD_T_SINT: {
					int data;

					data = stktable_data_cast(data_ptr, std_t_sint);
					intencode(data, &cursor);
					break;
				}
				case STD_T_UINT: {
					unsigned int data;

					data = stktable_data_cast(data_ptr, std_t_uint);
					intencode(data, &cursor);
					break;
				}
				case STD_T_ULL: {
					unsigned long long data;

					data = stktable_data_cast(data_ptr, std_t_ull);
					intencode(data, &cursor);
					break;
				}
				case STD_T_FRQP: {
					struct freq_ctr_period *frqp;

					frqp = &stktable_data_cast(data_ptr, std_t_frqp);
					intencode((unsigned int)(now_ms - frqp->curr_tick), &cursor);
					intencode(frqp->curr_ctr, &cursor);
					intencode(frqp->prev_ctr, &cursor);
					break;
				}
				case STD_T_DICT: {
					struct dict_entry *de;
					struct ebpt_node *cached_de;
					struct dcache_tx_entry cde = { };
					char *beg, *end;
					size_t value_len, data_len;
					struct dcache *dc;

					de = stktable_data_cast(data_ptr, std_t_dict);
					if (!de)
						break;

					dc = peer->dcache;
					cde.entry.key = de;
					cached_de = dcache_tx_insert(dc, &cde);
					if (cached_de == &cde.entry) {
						if (cde.id + 1 >= PEER_ENC_2BYTES_MIN)
							break;
						/* Encode the length of the remaining data -> 1 */
						intencode(1, &cursor);
						/* Encode the cache entry ID */
						intencode(cde.id + 1, &cursor);
					}
					else {
						/* Leave enough room to encode the remaining data length. */
						end = beg = cursor + PEER_MSG_ENC_LENGTH_MAXLEN;
						/* Encode the dictionary entry key */
						intencode(cde.id + 1, &end);
						/* Encode the length of the dictionary entry data */
						value_len = de->len;
						intencode(value_len, &end);
						/* Copy the data */
						memcpy(end, de->value.key, value_len);
						end += value_len;
						/* Encode the length of the data */
						data_len = end - beg;
						intencode(data_len, &cursor);
						memmove(cursor, beg, data_len);
						cursor += data_len;
					}
					break;
				}
			}
		}
	}
	HA_RWLOCK_RDUNLOCK(STK_SESS_LOCK, &ts->lock);

	/* Compute datalen */
	datalen = (cursor - datamsg);

	/*  prepare message header */
	msg[0] = PEER_MSG_CLASS_STICKTABLE;
	peer_set_update_msg_type(&msg[1], use_identifier, use_timed);
	cursor = &msg[2];
	intencode(datalen, &cursor);

	/* move data after header */
	memmove(cursor, datamsg, datalen);

	/* return header size + data_len */
	return (cursor - msg) + datalen;
}

/*
 * This prepare the switch table message to targeted share table <st>.
 *  <msg> is a buffer of <size> to receive data message content
 * If function returns 0, the caller should consider we were unable to encode this message (TODO:
 * check size)
 */
static int peer_prepare_switchmsg(char *msg, size_t size, struct peer_prep_params *params)
{
	int len;
	unsigned short datalen;
	struct buffer *chunk;
	char *cursor, *datamsg, *chunkp, *chunkq;
	uint64_t data = 0;
	unsigned int data_type;
	struct shared_table *st;

	st = params->swtch.shared_table;
	cursor = datamsg = msg + PEER_MSG_HEADER_LEN + PEER_MSG_ENC_LENGTH_MAXLEN;

	/* Encode data */

	/* encode local id */
	intencode(st->local_id, &cursor);

	/* encode table name */
	len = strlen(st->table->nid);
	intencode(len, &cursor);
	memcpy(cursor, st->table->nid, len);
	cursor += len;

	/* encode table type */

	intencode(st->table->type, &cursor);

	/* encode table key size */
	intencode(st->table->key_size, &cursor);

	chunk = get_trash_chunk();
	chunkp = chunkq = chunk->area;
	/* encode available known data types in table */
	for (data_type = 0 ; data_type < STKTABLE_DATA_TYPES ; data_type++) {
		if (st->table->data_ofs[data_type]) {
			switch (stktable_data_types[data_type].std_type) {
				case STD_T_SINT:
				case STD_T_UINT:
				case STD_T_ULL:
				case STD_T_DICT:
					data |= 1 << data_type;
					break;
				case STD_T_FRQP:
					data |= 1 << data_type;
					intencode(data_type, &chunkq);
					intencode(st->table->data_arg[data_type].u, &chunkq);
					break;
			}
		}
	}
	intencode(data, &cursor);

	/* Encode stick-table entries duration. */
	intencode(st->table->expire, &cursor);

	if (chunkq > chunkp) {
		chunk->data = chunkq - chunkp;
		memcpy(cursor, chunk->area, chunk->data);
		cursor += chunk->data;
	}

	/* Compute datalen */
	datalen = (cursor - datamsg);

	/*  prepare message header */
	msg[0] = PEER_MSG_CLASS_STICKTABLE;
	msg[1] = PEER_MSG_STKT_DEFINE;
	cursor = &msg[2];
	intencode(datalen, &cursor);

	/* move data after header */
	memmove(cursor, datamsg, datalen);

	/* return header size + data_len */
	return (cursor - msg) + datalen;
}

/*
 * This prepare the acknowledge message on the stick session <ts>, <st> is the considered
 * stick table.
 *  <msg> is a buffer of <size> to receive data message content
 * If function returns 0, the caller should consider we were unable to encode this message (TODO:
 * check size)
 */
static int peer_prepare_ackmsg(char *msg, size_t size, struct peer_prep_params *p)
{
	unsigned short datalen;
	char *cursor, *datamsg;
	uint32_t netinteger;
	struct shared_table *st;

	cursor = datamsg = msg + PEER_MSG_HEADER_LEN + PEER_MSG_ENC_LENGTH_MAXLEN;

	st = p->ack.shared_table;
	intencode(st->remote_id, &cursor);
	netinteger = htonl(st->last_get);
	memcpy(cursor, &netinteger, sizeof(netinteger));
	cursor += sizeof(netinteger);

	/* Compute datalen */
	datalen = (cursor - datamsg);

	/*  prepare message header */
	msg[0] = PEER_MSG_CLASS_STICKTABLE;
	msg[1] = PEER_MSG_STKT_ACK;
	cursor = &msg[2];
	intencode(datalen, &cursor);

	/* move data after header */
	memmove(cursor, datamsg, datalen);

	/* return header size + data_len */
	return (cursor - msg) + datalen;
}

/*
 * Function to deinit connected peer
 */
void __peer_session_deinit(struct peer *peer)
{
	struct stream_interface *si;
	struct stream *s;
	struct peers *peers;

	if (!peer->appctx)
		return;

	si = peer->appctx->owner;
	if (!si)
		return;

	s = si_strm(si);
	if (!s)
		return;

	peers = strm_fe(s)->parent;
	if (!peers)
		return;

	if (peer->appctx->st0 == PEER_SESS_ST_WAITMSG)
		HA_ATOMIC_SUB(&connected_peers, 1);

	HA_ATOMIC_SUB(&active_peers, 1);

	flush_dcache(peer);

	/* Re-init current table pointers to force announcement on re-connect */
	peer->remote_table = peer->last_local_table = NULL;
	peer->appctx = NULL;
	if (peer->flags & PEER_F_LEARN_ASSIGN) {
		/* unassign current peer for learning */
		peer->flags &= ~(PEER_F_LEARN_ASSIGN);
		peers->flags &= ~(PEERS_F_RESYNC_ASSIGN|PEERS_F_RESYNC_PROCESS);

		/* reschedule a resync */
		peers->resync_timeout = tick_add(now_ms, MS_TO_TICKS(5000));
	}
	/* reset teaching and learning flags to 0 */
	peer->flags &= PEER_TEACH_RESET;
	peer->flags &= PEER_LEARN_RESET;
	task_wakeup(peers->sync_task, TASK_WOKEN_MSG);
}

/*
 * Callback to release a session with a peer
 */
static void peer_session_release(struct appctx *appctx)
{
	struct peer *peer = appctx->ctx.peers.ptr;

	/* appctx->ctx.peers.ptr is not a peer session */
	if (appctx->st0 < PEER_SESS_ST_SENDSUCCESS)
		return;

	/* peer session identified */
	if (peer) {
		HA_SPIN_LOCK(PEER_LOCK, &peer->lock);
		if (peer->appctx == appctx)
			__peer_session_deinit(peer);
		HA_SPIN_UNLOCK(PEER_LOCK, &peer->lock);
	}
}

/* Retrieve the major and minor versions of peers protocol
 * announced by a remote peer. <str> is a null-terminated
 * string with the following format: "<maj_ver>.<min_ver>".
 */
static int peer_get_version(const char *str,
                            unsigned int *maj_ver, unsigned int *min_ver)
{
	unsigned int majv, minv;
	const char *pos, *saved;
	const char *end;

	saved = pos = str;
	end = str + strlen(str);

	majv = read_uint(&pos, end);
	if (saved == pos || *pos++ != '.')
		return -1;

	saved = pos;
	minv = read_uint(&pos, end);
	if (saved == pos || pos != end)
		return -1;

	*maj_ver = majv;
	*min_ver = minv;

	return 0;
}

/*
 * Parse a line terminated by an optional '\r' character, followed by a mandatory
 * '\n' character.
 * Returns 1 if succeeded or 0 if a '\n' character could not be found, and -1 if
 * a line could not be read because the communication channel is closed.
 */
static inline int peer_getline(struct appctx  *appctx)
{
	int n;
	struct stream_interface *si = appctx->owner;

	n = co_getline(si_oc(si), trash.area, trash.size);
	if (!n)
		return 0;

	if (n < 0 || trash.area[n - 1] != '\n') {
		appctx->st0 = PEER_SESS_ST_END;
		return -1;
	}

	if (n > 1 && (trash.area[n - 2] == '\r'))
		trash.area[n - 2] = 0;
	else
		trash.area[n - 1] = 0;

	co_skip(si_oc(si), n);

	return n;
}

/*
 * Send a message after having called <peer_prepare_msg> to build it.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_msg(struct appctx *appctx,
                                int (*peer_prepare_msg)(char *, size_t, struct peer_prep_params *),
                                struct peer_prep_params *params)
{
	int ret, msglen;
	struct stream_interface *si = appctx->owner;

	msglen = peer_prepare_msg(trash.area, trash.size, params);
	if (!msglen) {
		/* internal error: message does not fit in trash */
		appctx->st0 = PEER_SESS_ST_END;
		return 0;
	}

	/* message to buffer */
	ret = ci_putblk(si_ic(si), trash.area, msglen);
	if (ret <= 0) {
		if (ret == -1) {
			/* No more write possible */
			si_rx_room_blk(si);
			return -1;
		}
		appctx->st0 = PEER_SESS_ST_END;
	}

	return ret;
}

/*
 * Send a hello message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_hellomsg(struct appctx *appctx, struct peer *peer)
{
	struct peer_prep_params p = {
		.hello.peer = peer,
	};

	return peer_send_msg(appctx, peer_prepare_hellomsg, &p);
}

/*
 * Send a success peer handshake status message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_status_successmsg(struct appctx *appctx)
{
	return peer_send_msg(appctx, peer_prepare_status_successmsg, NULL);
}

/*
 * Send a peer handshake status error message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_status_errormsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.error_status.st1 = appctx->st1,
	};

	return peer_send_msg(appctx, peer_prepare_status_errormsg, &p);
}

/*
 * Send a stick-table switch message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_switchmsg(struct shared_table *st, struct appctx *appctx)
{
	struct peer_prep_params p = {
		.swtch.shared_table = st,
	};

	return peer_send_msg(appctx, peer_prepare_switchmsg, &p);
}

/*
 * Send a stick-table update acknowledgement message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_ackmsg(struct shared_table *st, struct appctx *appctx)
{
	struct peer_prep_params p = {
		.ack.shared_table = st,
	};

	return peer_send_msg(appctx, peer_prepare_ackmsg, &p);
}

/*
 * Send a stick-table update message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_updatemsg(struct shared_table *st, struct appctx *appctx, struct stksess *ts,
                                      unsigned int updateid, int use_identifier, int use_timed)
{
	struct peer_prep_params p = {
		.updt.stksess = ts,
		.updt.shared_table = st,
		.updt.updateid = updateid,
		.updt.use_identifier = use_identifier,
		.updt.use_timed = use_timed,
		.updt.peer = appctx->ctx.peers.ptr,
	};

	return peer_send_msg(appctx, peer_prepare_updatemsg, &p);
}

/*
 * Build a peer protocol control class message.
 * Returns the number of written bytes used to build the message if succeeded,
 * 0 if not.
 */
static int peer_prepare_control_msg(char *msg, size_t size, struct peer_prep_params *p)
{
	if (size < sizeof p->control.head)
		return 0;

	msg[0] = p->control.head[0];
	msg[1] = p->control.head[1];

	return 2;
}

/*
 * Send a stick-table synchronization request message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_resync_reqmsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.control.head = { PEER_MSG_CLASS_CONTROL, PEER_MSG_CTRL_RESYNCREQ, },
	};

	return peer_send_msg(appctx, peer_prepare_control_msg, &p);
}

/*
 * Send a stick-table synchronization confirmation message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_resync_confirmsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.control.head = { PEER_MSG_CLASS_CONTROL, PEER_MSG_CTRL_RESYNCCONFIRM, },
	};

	return peer_send_msg(appctx, peer_prepare_control_msg, &p);
}

/*
 * Send a stick-table synchronization finished message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_resync_finishedmsg(struct appctx *appctx, struct peer *peer)
{
	struct peer_prep_params p = {
		.control.head = { PEER_MSG_CLASS_CONTROL, },
	};

	p.control.head[1] = (peer->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FINISHED ?
		PEER_MSG_CTRL_RESYNCFINISHED : PEER_MSG_CTRL_RESYNCPARTIAL;

	return peer_send_msg(appctx, peer_prepare_control_msg, &p);
}

/*
 * Send a heartbeat message.
 * Return 0 if the message could not be built modifying the appctx st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_heartbeatmsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.control.head = { PEER_MSG_CLASS_CONTROL, PEER_MSG_CTRL_HEARTBEAT, },
	};

	return peer_send_msg(appctx, peer_prepare_control_msg, &p);
}

/*
 * Build a peer protocol error class message.
 * Returns the number of written bytes used to build the message if succeeded,
 * 0 if not.
 */
static int peer_prepare_error_msg(char *msg, size_t size, struct peer_prep_params *p)
{
	if (size < sizeof p->error.head)
		return 0;

	msg[0] = p->error.head[0];
	msg[1] = p->error.head[1];

	return 2;
}

/*
 * Send a "size limit reached" error message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_error_size_limitmsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.error.head = { PEER_MSG_CLASS_ERROR, PEER_MSG_ERR_SIZELIMIT, },
	};

	return peer_send_msg(appctx, peer_prepare_error_msg, &p);
}

/*
 * Send a "peer protocol" error message.
 * Return 0 if the message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appctx st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_error_protomsg(struct appctx *appctx)
{
	struct peer_prep_params p = {
		.error.head = { PEER_MSG_CLASS_ERROR, PEER_MSG_ERR_PROTOCOL, },
	};

	return peer_send_msg(appctx, peer_prepare_error_msg, &p);
}

/*
 * Function used to lookup for recent stick-table updates associated with
 * <st> shared stick-table when a lesson must be taught a peer (PEER_F_LEARN_ASSIGN flag set).
 */
static inline struct stksess *peer_teach_process_stksess_lookup(struct shared_table *st)
{
	struct eb32_node *eb;

	eb = eb32_lookup_ge(&st->table->updates, st->last_pushed+1);
	if (!eb) {
		eb = eb32_first(&st->table->updates);
		if (!eb || ((int)(eb->key - st->last_pushed) <= 0)) {
			st->table->commitupdate = st->last_pushed = st->table->localupdate;
			return NULL;
		}
	}

	if ((int)(eb->key - st->table->localupdate) > 0) {
		st->table->commitupdate = st->last_pushed = st->table->localupdate;
		return NULL;
	}

	return eb32_entry(eb, struct stksess, upd);
}

/*
 * Function used to lookup for recent stick-table updates associated with
 * <st> shared stick-table during teach state 1 step.
 */
static inline struct stksess *peer_teach_stage1_stksess_lookup(struct shared_table *st)
{
	struct eb32_node *eb;

	eb = eb32_lookup_ge(&st->table->updates, st->last_pushed+1);
	if (!eb) {
		st->flags |= SHTABLE_F_TEACH_STAGE1;
		eb = eb32_first(&st->table->updates);
		if (eb)
			st->last_pushed = eb->key - 1;
		return NULL;
	}

	return eb32_entry(eb, struct stksess, upd);
}

/*
 * Function used to lookup for recent stick-table updates associated with
 * <st> shared stick-table during teach state 2 step.
 */
static inline struct stksess *peer_teach_stage2_stksess_lookup(struct shared_table *st)
{
	struct eb32_node *eb;

	eb = eb32_lookup_ge(&st->table->updates, st->last_pushed+1);
	if (!eb || eb->key > st->teaching_origin) {
		st->flags |= SHTABLE_F_TEACH_STAGE2;
		return NULL;
	}

	return eb32_entry(eb, struct stksess, upd);
}

/*
 * Generic function to emit update messages for <st> stick-table when a lesson must
 * be taught to the peer <p>.
 * <locked> must be set to 1 if the shared table <st> is already locked when entering
 * this function, 0 if not.
 *
 * This function temporary unlock/lock <st> when it sends stick-table updates or
 * when decrementing its refcount in case of any error when it sends this updates.
 *
 * Return 0 if any message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 * If it returns 0 or -1, this function leave <st> locked if already locked when entering this function
 * unlocked if not already locked when entering this function.
 */
static inline int peer_send_teachmsgs(struct appctx *appctx, struct peer *p,
                                      struct stksess *(*peer_stksess_lookup)(struct shared_table *),
                                      struct shared_table *st, int locked)
{
	int ret, new_pushed, use_timed;

	ret = 1;
	use_timed = 0;
	if (st != p->last_local_table) {
		ret = peer_send_switchmsg(st, appctx);
		if (ret <= 0)
			return ret;

		p->last_local_table = st;
	}

	if (peer_stksess_lookup != peer_teach_process_stksess_lookup)
		use_timed = !(p->flags & PEER_F_DWNGRD);

	/* We force new pushed to 1 to force identifier in update message */
	new_pushed = 1;

	if (!locked)
		HA_SPIN_LOCK(STK_TABLE_LOCK, &st->table->lock);

	while (1) {
		struct stksess *ts;
		unsigned updateid;

		/* push local updates */
		ts = peer_stksess_lookup(st);
		if (!ts)
			break;

		updateid = ts->upd.key;
		ts->ref_cnt++;
		HA_SPIN_UNLOCK(STK_TABLE_LOCK, &st->table->lock);

		ret = peer_send_updatemsg(st, appctx, ts, updateid, new_pushed, use_timed);
		if (ret <= 0) {
			HA_SPIN_LOCK(STK_TABLE_LOCK, &st->table->lock);
			ts->ref_cnt--;
			if (!locked)
				HA_SPIN_UNLOCK(STK_TABLE_LOCK, &st->table->lock);
			return ret;
		}

		HA_SPIN_LOCK(STK_TABLE_LOCK, &st->table->lock);
		ts->ref_cnt--;
		st->last_pushed = updateid;

		if (peer_stksess_lookup == peer_teach_process_stksess_lookup &&
		    (int)(st->last_pushed - st->table->commitupdate) > 0)
			st->table->commitupdate = st->last_pushed;

		/* identifier may not needed in next update message */
		new_pushed = 0;
	}

 out:
	if (!locked)
		HA_SPIN_UNLOCK(STK_TABLE_LOCK, &st->table->lock);
	return 1;
}

/*
 * Function to emit update messages for <st> stick-table when a lesson must
 * be taught to the peer <p> (PEER_F_LEARN_ASSIGN flag set).
 *
 * Note that <st> shared stick-table is locked when calling this function.
 *
 * Return 0 if any message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_teach_process_msgs(struct appctx *appctx, struct peer *p,
                                               struct shared_table *st)
{
	return peer_send_teachmsgs(appctx, p, peer_teach_process_stksess_lookup, st, 1);
}

/*
 * Function to emit update messages for <st> stick-table when a lesson must
 * be taught to the peer <p> during teach state 1 step.
 *
 * Return 0 if any message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_teach_stage1_msgs(struct appctx *appctx, struct peer *p,
                                              struct shared_table *st)
{
	return peer_send_teachmsgs(appctx, p, peer_teach_stage1_stksess_lookup, st, 0);
}

/*
 * Function to emit update messages for <st> stick-table when a lesson must
 * be taught to the peer <p> during teach state 1 step.
 *
 * Return 0 if any message could not be built modifying the appcxt st0 to PEER_SESS_ST_END value.
 * Returns -1 if there was not enough room left to send the message,
 * any other negative returned value must  be considered as an error with an appcxt st0
 * returned value equal to PEER_SESS_ST_END.
 */
static inline int peer_send_teach_stage2_msgs(struct appctx *appctx, struct peer *p,
                                              struct shared_table *st)
{
	return peer_send_teachmsgs(appctx, p, peer_teach_stage2_stksess_lookup, st, 0);
}


/*
 * Function used to parse a stick-table update message after it has been received
 * by <p> peer with <msg_cur> as address of the pointer to the position in the
 * receipt buffer with <msg_end> being position of the end of the stick-table message.
 * Update <msg_curr> accordingly to the peer protocol specs if no peer protocol error
 * was encountered.
 * <exp> must be set if the stick-table entry expires.
 * <updt> must be set for  PEER_MSG_STKT_UPDATE or PEER_MSG_STKT_UPDATE_TIMED stick-table
 * messages, in this case the stick-table udpate message is received with a stick-table
 * update ID.
 * <totl> is the length of the stick-table update message computed upon receipt.
 */
static int peer_treat_updatemsg(struct appctx *appctx, struct peer *p, int updt, int exp,
                                char **msg_cur, char *msg_end, int msg_len, int totl)
{
	struct stream_interface *si = appctx->owner;
	struct shared_table *st = p->remote_table;
	struct stksess *ts, *newts;
	uint32_t update;
	int expire;
	unsigned int data_type;
	void *data_ptr;

	/* Here we have data message */
	if (!st)
		goto ignore_msg;

	expire = MS_TO_TICKS(st->table->expire);

	if (updt) {
		if (msg_len < sizeof(update))
			goto malformed_exit;

		memcpy(&update, *msg_cur, sizeof(update));
		*msg_cur += sizeof(update);
		st->last_get = htonl(update);
	}
	else {
		st->last_get++;
	}

	if (exp) {
		size_t expire_sz = sizeof expire;

		if (*msg_cur + expire_sz > msg_end)
			goto malformed_exit;

		memcpy(&expire, *msg_cur, expire_sz);
		*msg_cur += expire_sz;
		expire = ntohl(expire);
	}

	newts = stksess_new(st->table, NULL);
	if (!newts)
		goto ignore_msg;

	if (st->table->type == SMP_T_STR) {
		unsigned int to_read, to_store;

		to_read = intdecode(msg_cur, msg_end);
		if (!*msg_cur)
			goto malformed_free_newts;

		to_store = MIN(to_read, st->table->key_size - 1);
		if (*msg_cur + to_store > msg_end)
			goto malformed_free_newts;

		memcpy(newts->key.key, *msg_cur, to_store);
		newts->key.key[to_store] = 0;
		*msg_cur += to_read;
	}
	else if (st->table->type == SMP_T_SINT) {
		unsigned int netinteger;

		if (*msg_cur + sizeof(netinteger) > msg_end)
			goto malformed_free_newts;

		memcpy(&netinteger, *msg_cur, sizeof(netinteger));
		netinteger = ntohl(netinteger);
		memcpy(newts->key.key, &netinteger, sizeof(netinteger));
		*msg_cur += sizeof(netinteger);
	}
	else {
		if (*msg_cur + st->table->key_size > msg_end)
			goto malformed_free_newts;

		memcpy(newts->key.key, *msg_cur, st->table->key_size);
		*msg_cur += st->table->key_size;
	}

	/* lookup for existing entry */
	ts = stktable_set_entry(st->table, newts);
	if (ts != newts) {
		stksess_free(st->table, newts);
		newts = NULL;
	}

	HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);

	for (data_type = 0 ; data_type < STKTABLE_DATA_TYPES ; data_type++) {
		uint64_t decoded_int;

		if (!((1 << data_type) & st->remote_data))
			continue;

		decoded_int = intdecode(msg_cur, msg_end);
		if (!*msg_cur)
			goto malformed_unlock;

		switch (stktable_data_types[data_type].std_type) {
		case STD_T_SINT:
			data_ptr = stktable_data_ptr(st->table, ts, data_type);
			if (data_ptr)
				stktable_data_cast(data_ptr, std_t_sint) = decoded_int;
			break;

		case STD_T_UINT:
			data_ptr = stktable_data_ptr(st->table, ts, data_type);
			if (data_ptr)
				stktable_data_cast(data_ptr, std_t_uint) = decoded_int;
			break;

		case STD_T_ULL:
			data_ptr = stktable_data_ptr(st->table, ts, data_type);
			if (data_ptr)
				stktable_data_cast(data_ptr, std_t_ull) = decoded_int;
			break;

		case STD_T_FRQP: {
			struct freq_ctr_period data;

			/* First bit is reserved for the freq_ctr_period lock
			Note: here we're still protected by the stksess lock
			so we don't need to update the update the freq_ctr_period
			using its internal lock */

			data.curr_tick = tick_add(now_ms, -decoded_int) & ~0x1;
			data.curr_ctr = intdecode(msg_cur, msg_end);
			if (!*msg_cur)
				goto malformed_unlock;

			data.prev_ctr = intdecode(msg_cur, msg_end);
			if (!*msg_cur)
				goto malformed_unlock;

			data_ptr = stktable_data_ptr(st->table, ts, data_type);
			if (data_ptr)
				stktable_data_cast(data_ptr, std_t_frqp) = data;
			break;
		}
		case STD_T_DICT: {
			struct buffer *chunk;
			size_t data_len, value_len;
			unsigned int id;
			struct dict_entry *de;
			struct dcache *dc;
			char *end;

			data_len = decoded_int;
			if (*msg_cur + data_len > msg_end)
				goto malformed_unlock;

			/* Compute the end of the current data, <msg_end> being at the end of
			 * the entire message.
			 */
			end = *msg_cur + data_len;
			id = intdecode(msg_cur, end);
			if (!*msg_cur || !id)
				goto malformed_unlock;

			dc = p->dcache;
			if (*msg_cur == end) {
				/* Dictionary entry key without value. */
				if (id > dc->max_entries)
					break;
				/* IDs sent over the network are numbered from 1. */
				de = dc->rx[id - 1].de;
			}
			else {
				chunk = get_trash_chunk();
				value_len = intdecode(msg_cur, end);
				if (!*msg_cur || *msg_cur + value_len > end ||
					unlikely(value_len + 1 >= chunk->size))
					goto malformed_unlock;

				chunk_memcpy(chunk, *msg_cur, value_len);
				chunk->area[chunk->data] = '\0';
				*msg_cur += value_len;

				de = dict_insert(&server_name_dict, chunk->area);
				dc->rx[id - 1].de = de;
			}
			if (de) {
				data_ptr = stktable_data_ptr(st->table, ts, data_type);
				if (data_ptr)
					stktable_data_cast(data_ptr, std_t_dict) = de;
			}
			break;
		}
		}
	}
	/* Force new expiration */
	ts->expire = tick_add(now_ms, expire);

	HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);
	stktable_touch_remote(st->table, ts, 1);
	return 1;

 ignore_msg:
	/* skip consumed message */
	co_skip(si_oc(si), totl);
	return 0;

 malformed_unlock:
	/* malformed message */
	HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);
	stktable_touch_remote(st->table, ts, 1);
	appctx->st0 = PEER_SESS_ST_ERRPROTO;
	return 0;

 malformed_free_newts:
	/* malformed message */
	stksess_free(st->table, newts);
 malformed_exit:
	appctx->st0 = PEER_SESS_ST_ERRPROTO;
	return 0;
}

/*
 * Function used to parse a stick-table update acknowledgement message after it
 * has been received by <p> peer with <msg_cur> as address of the pointer to the position in the
 * receipt buffer with <msg_end> being the position of the end of the stick-table message.
 * Update <msg_curr> accordingly to the peer protocol specs if no peer protocol error
 * was encountered.
 * Return 1 if succeeded, 0 if not with the appctx state st0 set to PEER_SESS_ST_ERRPROTO.
 */
static inline int peer_treat_ackmsg(struct appctx *appctx, struct peer *p,
                                    char **msg_cur, char *msg_end)
{
	/* ack message */
	uint32_t table_id ;
	uint32_t update;
	struct shared_table *st;

	table_id = intdecode(msg_cur, msg_end);
	if (!*msg_cur || (*msg_cur + sizeof(update) > msg_end)) {
		/* malformed message */
		appctx->st0 = PEER_SESS_ST_ERRPROTO;
		return 0;
	}

	memcpy(&update, *msg_cur, sizeof(update));
	update = ntohl(update);

	for (st = p->tables; st; st = st->next) {
		if (st->local_id == table_id) {
			st->update = update;
			break;
		}
	}

	return 1;
}

/*
 * Function used to parse a stick-table switch message after it has been received
 * by <p> peer with <msg_cur> as address of the pointer to the position in the
 * receipt buffer with <msg_end> being the position of the end of the stick-table message.
 * Update <msg_curr> accordingly to the peer protocol specs if no peer protocol error
 * was encountered.
 * Return 1 if succeeded, 0 if not with the appctx state st0 set to PEER_SESS_ST_ERRPROTO.
 */
static inline int peer_treat_switchmsg(struct appctx *appctx, struct peer *p,
                                      char **msg_cur, char *msg_end)
{
	struct shared_table *st;
	int table_id;

	table_id = intdecode(msg_cur, msg_end);
	if (!*msg_cur) {
		/* malformed message */
		appctx->st0 = PEER_SESS_ST_ERRPROTO;
		return 0;
	}

	p->remote_table = NULL;
	for (st = p->tables; st; st = st->next) {
		if (st->remote_id == table_id) {
			p->remote_table = st;
			break;
		}
	}

	return 1;
}

/*
 * Function used to parse a stick-table definition message after it has been received
 * by <p> peer with <msg_cur> as address of the pointer to the position in the
 * receipt buffer with <msg_end> being the position of the end of the stick-table message.
 * Update <msg_curr> accordingly to the peer protocol specs if no peer protocol error
 * was encountered.
 * <totl> is the length of the stick-table update message computed upon receipt.
 * Return 1 if succeeded, 0 if not with the appctx state st0 set to PEER_SESS_ST_ERRPROTO.
 */
static inline int peer_treat_definemsg(struct appctx *appctx, struct peer *p,
                                      char **msg_cur, char *msg_end, int totl)
{
	struct stream_interface *si = appctx->owner;
	int table_id_len;
	struct shared_table *st;
	int table_type;
	int table_keylen;
	int table_id;
	uint64_t table_data;

	table_id = intdecode(msg_cur, msg_end);
	if (!*msg_cur)
		goto malformed_exit;

	table_id_len = intdecode(msg_cur, msg_end);
	if (!*msg_cur)
		goto malformed_exit;

	p->remote_table = NULL;
	if (!table_id_len || (*msg_cur + table_id_len) >= msg_end)
		goto malformed_exit;

	for (st = p->tables; st; st = st->next) {
		/* Reset IDs */
		if (st->remote_id == table_id)
			st->remote_id = 0;

		if (!p->remote_table && (table_id_len == strlen(st->table->nid)) &&
		    (memcmp(st->table->nid, *msg_cur, table_id_len) == 0))
			p->remote_table = st;
	}

	if (!p->remote_table)
		goto ignore_msg;

	*msg_cur += table_id_len;
	if (*msg_cur >= msg_end)
		goto malformed_exit;

	table_type = intdecode(msg_cur, msg_end);
	if (!*msg_cur)
		goto malformed_exit;

	table_keylen = intdecode(msg_cur, msg_end);
	if (!*msg_cur)
		goto malformed_exit;

	table_data = intdecode(msg_cur, msg_end);
	if (!*msg_cur)
		goto malformed_exit;

	if (p->remote_table->table->type != table_type
		|| p->remote_table->table->key_size != table_keylen) {
		p->remote_table = NULL;
		goto ignore_msg;
	}

	p->remote_table->remote_data = table_data;
	p->remote_table->remote_id = table_id;
	return 1;

 ignore_msg:
	co_skip(si_oc(si), totl);
	return 0;

 malformed_exit:
	/* malformed message */
	appctx->st0 = PEER_SESS_ST_ERRPROTO;
	return 0;
}

/*
 * Receive a stick-table message.
 * Returns 1 if there was no error, if not, returns 0 if not enough data were available,
 * -1 if there was an error updating the appctx state st0 accordingly.
 */
static inline int peer_recv_msg(struct appctx *appctx, char *msg_head, size_t msg_head_sz,
                                uint32_t *msg_len, int *totl)
{
	int reql;
	struct stream_interface *si = appctx->owner;

	reql = co_getblk(si_oc(si), msg_head, 2 * sizeof(char), *totl);
	if (reql <= 0) /* closed or EOL not found */
		goto incomplete;

	*totl += reql;

	if (!(msg_head[1] & PEER_MSG_STKT_BIT_MASK))
		return 1;

	/* Read and Decode message length */
	reql = co_getblk(si_oc(si), &msg_head[2], sizeof(char), *totl);
	if (reql <= 0) /* closed */
		goto incomplete;

	*totl += reql;

	if ((unsigned int)msg_head[2] < PEER_ENC_2BYTES_MIN) {
		*msg_len = msg_head[2];
	}
	else {
		int i;
		char *cur;
		char *end;

		for (i = 3 ; i < msg_head_sz ; i++) {
			reql = co_getblk(si_oc(si), &msg_head[i], sizeof(char), *totl);
			if (reql <= 0) /* closed */
				goto incomplete;

			*totl += reql;

			if (!(msg_head[i] & PEER_MSG_STKT_BIT_MASK))
				break;
		}

		if (i == msg_head_sz) {
			/* malformed message */
			appctx->st0 = PEER_SESS_ST_ERRPROTO;
			return -1;
		}
		end = msg_head + msg_head_sz;
		cur = &msg_head[2];
		*msg_len = intdecode(&cur, end);
		if (!cur) {
			/* malformed message */
			appctx->st0 = PEER_SESS_ST_ERRPROTO;
			return -1;
		}
	}

	/* Read message content */
	if (*msg_len) {
		if (*msg_len > trash.size) {
			/* Status code is not success, abort */
			appctx->st0 = PEER_SESS_ST_ERRSIZE;
			return -1;
		}

		reql = co_getblk(si_oc(si), trash.area, *msg_len, *totl);
		if (reql <= 0) /* closed */
			goto incomplete;
		*totl += reql;
	}

	return 1;

 incomplete:
	if (reql < 0) {
		/* there was an error */
		appctx->st0 = PEER_SESS_ST_END;
		return -1;
	}

	return 0;
}

/*
 * Treat the awaited message with <msg_head> as header.*
 * Return 1 if succeeded, 0 if not.
 */
static inline int peer_treat_awaited_msg(struct appctx *appctx, struct peer *peer, unsigned char *msg_head,
                                         char **msg_cur, char *msg_end, int msg_len, int totl)
{
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct peers *peers = strm_fe(s)->parent;

	if (msg_head[0] == PEER_MSG_CLASS_CONTROL) {
		if (msg_head[1] == PEER_MSG_CTRL_RESYNCREQ) {
			struct shared_table *st;
			/* Reset message: remote need resync */

			/* prepare tables fot a global push */
			for (st = peer->tables; st; st = st->next) {
				st->teaching_origin = st->last_pushed = st->table->update;
				st->flags = 0;
			}

			/* reset teaching flags to 0 */
			peer->flags &= PEER_TEACH_RESET;

			/* flag to start to teach lesson */
			peer->flags |= PEER_F_TEACH_PROCESS;
		}
		else if (msg_head[1] == PEER_MSG_CTRL_RESYNCFINISHED) {
			if (peer->flags & PEER_F_LEARN_ASSIGN) {
				peer->flags &= ~PEER_F_LEARN_ASSIGN;
				peers->flags &= ~(PEERS_F_RESYNC_ASSIGN|PEERS_F_RESYNC_PROCESS);
				peers->flags |= (PEERS_F_RESYNC_LOCAL|PEERS_F_RESYNC_REMOTE);
			}
			peer->confirm++;
		}
		else if (msg_head[1] == PEER_MSG_CTRL_RESYNCPARTIAL) {
			if (peer->flags & PEER_F_LEARN_ASSIGN) {
				peer->flags &= ~PEER_F_LEARN_ASSIGN;
				peers->flags &= ~(PEERS_F_RESYNC_ASSIGN|PEERS_F_RESYNC_PROCESS);

				peer->flags |= PEER_F_LEARN_NOTUP2DATE;
				peers->resync_timeout = tick_add(now_ms, MS_TO_TICKS(PEER_RESYNC_TIMEOUT));
				task_wakeup(peers->sync_task, TASK_WOKEN_MSG);
			}
			peer->confirm++;
		}
		else if (msg_head[1] == PEER_MSG_CTRL_RESYNCCONFIRM)  {
			struct shared_table *st;

			/* If stopping state */
			if (stopping) {
				/* Close session, push resync no more needed */
				peer->flags |= PEER_F_TEACH_COMPLETE;
				appctx->st0 = PEER_SESS_ST_END;
				return 0;
			}
			for (st = peer->tables; st; st = st->next) {
				st->update = st->last_pushed = st->teaching_origin;
				st->flags = 0;
			}

			/* reset teaching flags to 0 */
			peer->flags &= PEER_TEACH_RESET;
		}
		else if (msg_head[1] == PEER_MSG_CTRL_HEARTBEAT) {
			peer->reconnect = tick_add(now_ms, MS_TO_TICKS(PEER_RECONNECT_TIMEOUT));
		}
	}
	else if (msg_head[0] == PEER_MSG_CLASS_STICKTABLE) {
		if (msg_head[1] == PEER_MSG_STKT_DEFINE) {
			if (!peer_treat_definemsg(appctx, peer, msg_cur, msg_end, totl))
				return 0;
		}
		else if (msg_head[1] == PEER_MSG_STKT_SWITCH) {
			if (!peer_treat_switchmsg(appctx, peer, msg_cur, msg_end))
				return 0;
		}
		else if (msg_head[1] == PEER_MSG_STKT_UPDATE ||
		         msg_head[1] == PEER_MSG_STKT_INCUPDATE ||
		         msg_head[1] == PEER_MSG_STKT_UPDATE_TIMED ||
		         msg_head[1] == PEER_MSG_STKT_INCUPDATE_TIMED) {
			int update, expire;

			update = msg_head[1] == PEER_MSG_STKT_UPDATE || msg_head[1] == PEER_MSG_STKT_UPDATE_TIMED;
			expire = msg_head[1] == PEER_MSG_STKT_UPDATE_TIMED || msg_head[1] == PEER_MSG_STKT_INCUPDATE_TIMED;
			if (!peer_treat_updatemsg(appctx, peer, update, expire,
			                          msg_cur, msg_end, msg_len, totl))
				return 0;

		}
		else if (msg_head[1] == PEER_MSG_STKT_ACK) {
			if (!peer_treat_ackmsg(appctx, peer, msg_cur, msg_end))
				return 0;
		}
	}
	else if (msg_head[0] == PEER_MSG_CLASS_RESERVED) {
		appctx->st0 = PEER_SESS_ST_ERRPROTO;
		return 0;
	}

	return 1;
}


/*
 * Send any message to <peer> peer.
 * Returns 1 if succeeded, or -1 or 0 if failed.
 * -1 means an internal error occured, 0 is for a peer protocol error leading
 * to a peer state change (from the peer I/O handler point of view).
 */
static inline int peer_send_msgs(struct appctx *appctx, struct peer *peer)
{
	int repl;
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct peers *peers = strm_fe(s)->parent;

	/* Need to request a resync */
	if ((peer->flags & PEER_F_LEARN_ASSIGN) &&
		(peers->flags & PEERS_F_RESYNC_ASSIGN) &&
		!(peers->flags & PEERS_F_RESYNC_PROCESS)) {

		repl = peer_send_resync_reqmsg(appctx);
		if (repl <= 0)
			return repl;

		peers->flags |= PEERS_F_RESYNC_PROCESS;
	}

	/* Nothing to read, now we start to write */
	if (peer->tables) {
		struct shared_table *st;
		struct shared_table *last_local_table;

		last_local_table = peer->last_local_table;
		if (!last_local_table)
			last_local_table = peer->tables;
		st = last_local_table->next;

		while (1) {
			if (!st)
				st = peer->tables;

			/* It remains some updates to ack */
			if (st->last_get != st->last_acked) {
				repl = peer_send_ackmsg(st, appctx);
				if (repl <= 0)
					return repl;

				st->last_acked = st->last_get;
			}

			if (!(peer->flags & PEER_F_TEACH_PROCESS)) {
				HA_SPIN_LOCK(STK_TABLE_LOCK, &st->table->lock);
				if (!(peer->flags & PEER_F_LEARN_ASSIGN) &&
					((int)(st->last_pushed - st->table->localupdate) < 0)) {

					repl = peer_send_teach_process_msgs(appctx, peer, st);
					if (repl <= 0) {
						HA_SPIN_UNLOCK(STK_TABLE_LOCK, &st->table->lock);
						return repl;
					}
				}
				HA_SPIN_UNLOCK(STK_TABLE_LOCK, &st->table->lock);
			}
			else {
				if (!(st->flags & SHTABLE_F_TEACH_STAGE1)) {
					repl = peer_send_teach_stage1_msgs(appctx, peer, st);
					if (repl <= 0)
						return repl;
				}

				if (!(st->flags & SHTABLE_F_TEACH_STAGE2)) {
					repl = peer_send_teach_stage2_msgs(appctx, peer, st);
					if (repl <= 0)
						return repl;
				}
			}

			if (st == last_local_table)
				break;
			st = st->next;
		}
	}

	if ((peer->flags & PEER_F_TEACH_PROCESS) && !(peer->flags & PEER_F_TEACH_FINISHED)) {
		repl = peer_send_resync_finishedmsg(appctx, peer);
		if (repl <= 0)
			return repl;

		/* flag finished message sent */
		peer->flags |= PEER_F_TEACH_FINISHED;
	}

	/* Confirm finished or partial messages */
	while (peer->confirm) {
		repl = peer_send_resync_confirmsg(appctx);
		if (repl <= 0)
			return repl;

		peer->confirm--;
	}

	return 1;
}

/*
 * Read and parse a first line of a "hello" peer protocol message.
 * Returns 0 if could not read a line, -1 if there was a read error or
 * the line is malformed, 1 if succeeded.
 */
static inline int peer_getline_version(struct appctx *appctx,
                                       unsigned int *maj_ver, unsigned int *min_ver)
{
	int reql;

	reql = peer_getline(appctx);
	if (!reql)
		return 0;

	if (reql < 0)
		return -1;

	/* test protocol */
	if (strncmp(PEER_SESSION_PROTO_NAME " ", trash.area, proto_len + 1) != 0) {
		appctx->st0 = PEER_SESS_ST_EXIT;
		appctx->st1 = PEER_SESS_SC_ERRPROTO;
		return -1;
	}
	if (peer_get_version(trash.area + proto_len + 1, maj_ver, min_ver) == -1 ||
		*maj_ver != PEER_MAJOR_VER || *min_ver > PEER_MINOR_VER) {
		appctx->st0 = PEER_SESS_ST_EXIT;
		appctx->st1 = PEER_SESS_SC_ERRVERSION;
		return -1;
	}

	return 1;
}

/*
 * Read and parse a second line of a "hello" peer protocol message.
 * Returns 0 if could not read a line, -1 if there was a read error or
 * the line is malformed, 1 if succeeded.
 */
static inline int peer_getline_host(struct appctx *appctx)
{
	int reql;

	reql = peer_getline(appctx);
	if (!reql)
		return 0;

	if (reql < 0)
		return -1;

	/* test hostname match */
	if (strcmp(localpeer, trash.area) != 0) {
		appctx->st0 = PEER_SESS_ST_EXIT;
		appctx->st1 = PEER_SESS_SC_ERRHOST;
		return -1;
	}

	return 1;
}

/*
 * Read and parse a last line of a "hello" peer protocol message.
 * Returns 0 if could not read a character, -1 if there was a read error or
 * the line is malformed, 1 if succeeded.
 * Set <curpeer> accordingly (the remote peer sending the "hello" message).
 */
static inline int peer_getline_last(struct appctx *appctx, struct peer **curpeer)
{
	char *p;
	int reql;
	struct peer *peer;
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct peers *peers = strm_fe(s)->parent;

	reql = peer_getline(appctx);
	if (!reql)
		return 0;

	if (reql < 0)
		return -1;

	/* parse line "<peer name> <pid> <relative_pid>" */
	p = strchr(trash.area, ' ');
	if (!p) {
		appctx->st0 = PEER_SESS_ST_EXIT;
		appctx->st1 = PEER_SESS_SC_ERRPROTO;
		return -1;
	}
	*p = 0;

	/* lookup known peer */
	for (peer = peers->remote; peer; peer = peer->next) {
		if (strcmp(peer->id, trash.area) == 0)
			break;
	}

	/* if unknown peer */
	if (!peer) {
		appctx->st0 = PEER_SESS_ST_EXIT;
		appctx->st1 = PEER_SESS_SC_ERRPEER;
		return -1;
	}
	*curpeer = peer;

	return 1;
}

/*
 * Init <peer> peer after having accepted it at peer protocol level.
 */
static inline void init_accepted_peer(struct peer *peer, struct peers *peers)
{
	struct shared_table *st;

	/* Register status code */
	peer->statuscode = PEER_SESS_SC_SUCCESSCODE;

	/* Awake main task */
	task_wakeup(peers->sync_task, TASK_WOKEN_MSG);

	/* Init confirm counter */
	peer->confirm = 0;

	/* Init cursors */
	for (st = peer->tables; st ; st = st->next) {
		st->last_get = st->last_acked = 0;
		st->teaching_origin = st->last_pushed = st->update;
	}

	/* reset teaching and learning flags to 0 */
	peer->flags &= PEER_TEACH_RESET;
	peer->flags &= PEER_LEARN_RESET;

	/* if current peer is local */
	if (peer->local) {
		/* if current host need resyncfrom local and no process assined  */
		if ((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMLOCAL &&
		    !(peers->flags & PEERS_F_RESYNC_ASSIGN)) {
			/* assign local peer for a lesson, consider lesson already requested */
			peer->flags |= PEER_F_LEARN_ASSIGN;
			peers->flags |= (PEERS_F_RESYNC_ASSIGN|PEERS_F_RESYNC_PROCESS);
		}

	}
	else if ((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMREMOTE &&
	         !(peers->flags & PEERS_F_RESYNC_ASSIGN)) {
		/* assign peer for a lesson  */
		peer->flags |= PEER_F_LEARN_ASSIGN;
		peers->flags |= PEERS_F_RESYNC_ASSIGN;
	}
}

/*
 * Init <peer> peer after having connected it at peer protocol level.
 */
static inline void init_connected_peer(struct peer *peer, struct peers *peers)
{
	struct shared_table *st;

	/* Init cursors */
	for (st = peer->tables; st ; st = st->next) {
		st->last_get = st->last_acked = 0;
		st->teaching_origin = st->last_pushed = st->update;
	}

	/* Init confirm counter */
	peer->confirm = 0;

	/* reset teaching and learning flags to 0 */
	peer->flags &= PEER_TEACH_RESET;
	peer->flags &= PEER_LEARN_RESET;

	/* If current peer is local */
	if (peer->local) {
		/* flag to start to teach lesson */
		peer->flags |= PEER_F_TEACH_PROCESS;
	}
	else if ((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMREMOTE &&
	         !(peers->flags & PEERS_F_RESYNC_ASSIGN)) {
		/* If peer is remote and resync from remote is needed,
		and no peer currently assigned */

		/* assign peer for a lesson */
		peer->flags |= PEER_F_LEARN_ASSIGN;
		peers->flags |= PEERS_F_RESYNC_ASSIGN;
	}
}

/*
 * IO Handler to handle message exchance with a peer
 */
static void peer_io_handler(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct peers *curpeers = strm_fe(s)->parent;
	struct peer *curpeer = NULL;
	int reql = 0;
	int repl = 0;
	unsigned int maj_ver, min_ver;
	int prev_state;

	/* Check if the input buffer is available. */
	if (si_ic(si)->buf.size == 0) {
		si_rx_room_blk(si);
		goto out;
	}

	while (1) {
		prev_state = appctx->st0;
switchstate:
		maj_ver = min_ver = (unsigned int)-1;
		switch(appctx->st0) {
			case PEER_SESS_ST_ACCEPT:
				prev_state = appctx->st0;
				appctx->ctx.peers.ptr = NULL;
				appctx->st0 = PEER_SESS_ST_GETVERSION;
				/* fall through */
			case PEER_SESS_ST_GETVERSION:
				prev_state = appctx->st0;
				reql = peer_getline_version(appctx, &maj_ver, &min_ver);
				if (reql <= 0) {
					if (!reql)
						goto out;
					goto switchstate;
				}

				appctx->st0 = PEER_SESS_ST_GETHOST;
				/* fall through */
			case PEER_SESS_ST_GETHOST:
				prev_state = appctx->st0;
				reql = peer_getline_host(appctx);
				if (reql <= 0) {
					if (!reql)
						goto out;
					goto switchstate;
				}

				appctx->st0 = PEER_SESS_ST_GETPEER;
				/* fall through */
			case PEER_SESS_ST_GETPEER: {
				prev_state = appctx->st0;
				reql = peer_getline_last(appctx, &curpeer);
				if (reql <= 0) {
					if (!reql)
						goto out;
					goto switchstate;
				}

				HA_SPIN_LOCK(PEER_LOCK, &curpeer->lock);
				if (curpeer->appctx && curpeer->appctx != appctx) {
					if (curpeer->local) {
						/* Local connection, reply a retry */
						appctx->st0 = PEER_SESS_ST_EXIT;
						appctx->st1 = PEER_SESS_SC_TRYAGAIN;
						goto switchstate;
					}

					/* we're killing a connection, we must apply a random delay before
					 * retrying otherwise the other end will do the same and we can loop
					 * for a while.
					 */
					curpeer->reconnect = tick_add(now_ms, MS_TO_TICKS(50 + random() % 2000));
					peer_session_forceshutdown(curpeer);
				}
				if (maj_ver != (unsigned int)-1 && min_ver != (unsigned int)-1) {
					if (min_ver == PEER_DWNGRD_MINOR_VER) {
						curpeer->flags |= PEER_F_DWNGRD;
					}
					else {
						curpeer->flags &= ~PEER_F_DWNGRD;
					}
				}
				curpeer->appctx = appctx;
				appctx->ctx.peers.ptr = curpeer;
				appctx->st0 = PEER_SESS_ST_SENDSUCCESS;
				_HA_ATOMIC_ADD(&active_peers, 1);
				/* fall through */
			}
			case PEER_SESS_ST_SENDSUCCESS: {
				prev_state = appctx->st0;
				if (!curpeer) {
					curpeer = appctx->ctx.peers.ptr;
					HA_SPIN_LOCK(PEER_LOCK, &curpeer->lock);
					if (curpeer->appctx != appctx) {
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
				}

				repl = peer_send_status_successmsg(appctx);
				if (repl <= 0) {
					if (repl == -1)
						goto out;
					goto switchstate;
				}

				init_accepted_peer(curpeer, curpeers);

				/* switch to waiting message state */
				_HA_ATOMIC_ADD(&connected_peers, 1);
				appctx->st0 = PEER_SESS_ST_WAITMSG;
				goto switchstate;
			}
			case PEER_SESS_ST_CONNECT: {
				prev_state = appctx->st0;
				if (!curpeer) {
					curpeer = appctx->ctx.peers.ptr;
					HA_SPIN_LOCK(PEER_LOCK, &curpeer->lock);
					if (curpeer->appctx != appctx) {
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
				}

				repl = peer_send_hellomsg(appctx, curpeer);
				if (repl <= 0) {
					if (repl == -1)
						goto out;
					goto switchstate;
				}

				/* switch to the waiting statuscode state */
				appctx->st0 = PEER_SESS_ST_GETSTATUS;
				/* fall through */
			}
			case PEER_SESS_ST_GETSTATUS: {
				prev_state = appctx->st0;
				if (!curpeer) {
					curpeer = appctx->ctx.peers.ptr;
					HA_SPIN_LOCK(PEER_LOCK, &curpeer->lock);
					if (curpeer->appctx != appctx) {
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
				}

				if (si_ic(si)->flags & CF_WRITE_PARTIAL)
					curpeer->statuscode = PEER_SESS_SC_CONNECTEDCODE;

				reql = peer_getline(appctx);
				if (!reql)
					goto out;

				if (reql < 0)
					goto switchstate;

				/* Register status code */
				curpeer->statuscode = atoi(trash.area);

				/* Awake main task */
				task_wakeup(curpeers->sync_task, TASK_WOKEN_MSG);

				/* If status code is success */
				if (curpeer->statuscode == PEER_SESS_SC_SUCCESSCODE) {
					init_connected_peer(curpeer, curpeers);
				}
				else {
					if (curpeer->statuscode == PEER_SESS_SC_ERRVERSION)
						curpeer->flags |= PEER_F_DWNGRD;
					/* Status code is not success, abort */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				_HA_ATOMIC_ADD(&connected_peers, 1);
				appctx->st0 = PEER_SESS_ST_WAITMSG;
				/* fall through */
			}
			case PEER_SESS_ST_WAITMSG: {
				uint32_t msg_len = 0;
				char *msg_cur = trash.area;
				char *msg_end = trash.area;
				unsigned char msg_head[7];
				int totl = 0;

				prev_state = appctx->st0;
				if (!curpeer) {
					curpeer = appctx->ctx.peers.ptr;
					HA_SPIN_LOCK(PEER_LOCK, &curpeer->lock);
					if (curpeer->appctx != appctx) {
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
				}

				reql = peer_recv_msg(appctx, (char *)msg_head, sizeof msg_head, &msg_len, &totl);
				if (reql <= 0) {
					if (reql == -1)
						goto switchstate;
					goto send_msgs;
				}

				msg_end += msg_len;
				if (!peer_treat_awaited_msg(appctx, curpeer, msg_head, &msg_cur, msg_end, msg_len, totl))
					goto switchstate;

				curpeer->flags |= PEER_F_ALIVE;

				/* skip consumed message */
				co_skip(si_oc(si), totl);
				/* loop on that state to peek next message */
				goto switchstate;

send_msgs:
				if (curpeer->flags & PEER_F_HEARTBEAT) {
					curpeer->flags &= ~PEER_F_HEARTBEAT;
					repl = peer_send_heartbeatmsg(appctx);
					if (repl <= 0) {
						if (repl == -1)
							goto out;
						goto switchstate;
					}
				}
				/* we get here when a peer_recv_msg() returns 0 in reql */
				repl = peer_send_msgs(appctx, curpeer);
				if (repl <= 0) {
					if (repl == -1)
						goto out;
					goto switchstate;
				}

				/* noting more to do */
				goto out;
			}
			case PEER_SESS_ST_EXIT:
				if (prev_state == PEER_SESS_ST_WAITMSG)
					_HA_ATOMIC_SUB(&connected_peers, 1);
				prev_state = appctx->st0;
				if (peer_send_status_errormsg(appctx) == -1)
					goto out;
				appctx->st0 = PEER_SESS_ST_END;
				goto switchstate;
			case PEER_SESS_ST_ERRSIZE: {
				if (prev_state == PEER_SESS_ST_WAITMSG)
					_HA_ATOMIC_SUB(&connected_peers, 1);
				prev_state = appctx->st0;
				if (peer_send_error_size_limitmsg(appctx) == -1)
					goto out;
				appctx->st0 = PEER_SESS_ST_END;
				goto switchstate;
			}
			case PEER_SESS_ST_ERRPROTO: {
				if (prev_state == PEER_SESS_ST_WAITMSG)
					_HA_ATOMIC_SUB(&connected_peers, 1);
				prev_state = appctx->st0;
				if (peer_send_error_protomsg(appctx) == -1)
					goto out;
				appctx->st0 = PEER_SESS_ST_END;
				prev_state = appctx->st0;
				/* fall through */
			}
			case PEER_SESS_ST_END: {
				if (prev_state == PEER_SESS_ST_WAITMSG)
					_HA_ATOMIC_SUB(&connected_peers, 1);
				prev_state = appctx->st0;
				if (curpeer) {
					HA_SPIN_UNLOCK(PEER_LOCK, &curpeer->lock);
					curpeer = NULL;
				}
				si_shutw(si);
				si_shutr(si);
				si_ic(si)->flags |= CF_READ_NULL;
				goto out;
			}
		}
	}
out:
	si_oc(si)->flags |= CF_READ_DONTWAIT;

	if (curpeer)
		HA_SPIN_UNLOCK(PEER_LOCK, &curpeer->lock);
	return;
}

static struct applet peer_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<PEER>", /* used for logging */
	.fct = peer_io_handler,
	.release = peer_session_release,
};


/*
 * Use this function to force a close of a peer session
 */
static void peer_session_forceshutdown(struct peer *peer)
{
	struct appctx *appctx = peer->appctx;

	/* Note that the peer sessions which have just been created
	 * (->st0 == PEER_SESS_ST_CONNECT) must not
	 * be shutdown, if not, the TCP session will never be closed
	 * and stay in CLOSE_WAIT state after having been closed by
	 * the remote side.
	 */
	if (!appctx || appctx->st0 == PEER_SESS_ST_CONNECT)
		return;

	if (appctx->applet != &peer_applet)
		return;

	__peer_session_deinit(peer);

	appctx->st0 = PEER_SESS_ST_END;
	appctx_wakeup(appctx);
}

/* Pre-configures a peers frontend to accept incoming connections */
void peers_setup_frontend(struct proxy *fe)
{
	fe->last_change = now.tv_sec;
	fe->cap = PR_CAP_FE | PR_CAP_BE;
	fe->maxconn = 0;
	fe->conn_retries = CONN_RETRIES;
	fe->timeout.client = MS_TO_TICKS(5000);
	fe->accept = frontend_accept;
	fe->default_target = &peer_applet.obj_type;
	fe->options2 |= PR_O2_INDEPSTR | PR_O2_SMARTCON | PR_O2_SMARTACC;
	fe->bind_proc = 0; /* will be filled by users */
}

/*
 * Create a new peer session in assigned state (connect will start automatically)
 */
static struct appctx *peer_session_create(struct peers *peers, struct peer *peer)
{
	struct proxy *p = peers->peers_fe; /* attached frontend */
	struct appctx *appctx;
	struct session *sess;
	struct stream *s;
	struct connection *conn;
	struct conn_stream *cs;

	peer->reconnect = tick_add(now_ms, MS_TO_TICKS(PEER_RECONNECT_TIMEOUT));
	peer->heartbeat = tick_add(now_ms, MS_TO_TICKS(PEER_HEARTBEAT_TIMEOUT));
	peer->statuscode = PEER_SESS_SC_CONNECTCODE;
	s = NULL;

	appctx = appctx_new(&peer_applet, tid_bit);
	if (!appctx)
		goto out_close;

	appctx->st0 = PEER_SESS_ST_CONNECT;
	appctx->ctx.peers.ptr = (void *)peer;

	sess = session_new(p, NULL, &appctx->obj_type);
	if (!sess) {
		ha_alert("out of memory in peer_session_create().\n");
		goto out_free_appctx;
	}

	if ((s = stream_new(sess, &appctx->obj_type)) == NULL) {
		ha_alert("Failed to initialize stream in peer_session_create().\n");
		goto out_free_sess;
	}

	/* The tasks below are normally what is supposed to be done by
	 * fe->accept().
	 */
	s->flags = SF_ASSIGNED|SF_ADDR_SET;

	/* applet is waiting for data */
	si_cant_get(&s->si[0]);
	appctx_wakeup(appctx);

	/* initiate an outgoing connection */
	s->si[1].flags |= SI_FL_NOLINGER;
	si_set_state(&s->si[1], SI_ST_ASS);

	/* automatically prepare the stream interface to connect to the
	 * pre-initialized connection in si->conn.
	 */
	if (unlikely((conn = conn_new()) == NULL))
		goto out_free_strm;

	if (unlikely((cs = cs_new(conn)) == NULL))
		goto out_free_conn;

	conn->target = s->target = peer_session_target(peer, s);
	memcpy(&conn->addr.to, &peer->addr, sizeof(conn->addr.to));

	conn_prepare(conn, peer->proto, peer_xprt(peer));
	if (conn_install_mux(conn, &mux_pt_ops, cs, s->be, NULL) < 0)
		goto out_free_cs;
	si_attach_cs(&s->si[1], cs);

	s->do_log = NULL;
	s->uniq_id = 0;

	s->res.flags |= CF_READ_DONTWAIT;

	peer->appctx = appctx;
	task_wakeup(s->task, TASK_WOKEN_INIT);
	_HA_ATOMIC_ADD(&active_peers, 1);
	return appctx;

	/* Error unrolling */
out_free_cs:
	cs_free(cs);
 out_free_conn:
	conn_free(conn);
 out_free_strm:
	LIST_DEL(&s->list);
	pool_free(pool_head_stream, s);
 out_free_sess:
	session_free(sess);
 out_free_appctx:
	appctx_free(appctx);
 out_close:
	return NULL;
}

/*
 * Task processing function to manage re-connect, peer session
 * tasks wakeup on local update and heartbeat.
 */
static struct task *process_peer_sync(struct task * task, void *context, unsigned short state)
{
	struct peers *peers = context;
	struct peer *ps;
	struct shared_table *st;

	task->expire = TICK_ETERNITY;

	if (!peers->peers_fe) {
		/* this one was never started, kill it */
		signal_unregister_handler(peers->sighandler);
		task_destroy(peers->sync_task);
		peers->sync_task = NULL;
		return NULL;
	}

	/* Acquire lock for all peers of the section */
	for (ps = peers->remote; ps; ps = ps->next)
		HA_SPIN_LOCK(PEER_LOCK, &ps->lock);

	if (!stopping) {
		/* Normal case (not soft stop)*/

		if (((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMLOCAL) &&
		     (!nb_oldpids || tick_is_expired(peers->resync_timeout, now_ms)) &&
		     !(peers->flags & PEERS_F_RESYNC_ASSIGN)) {
			/* Resync from local peer needed
			   no peer was assigned for the lesson
			   and no old local peer found
			       or resync timeout expire */

			/* flag no more resync from local, to try resync from remotes */
			peers->flags |= PEERS_F_RESYNC_LOCAL;

			/* reschedule a resync */
			peers->resync_timeout = tick_add(now_ms, MS_TO_TICKS(PEER_RESYNC_TIMEOUT));
		}

		/* For each session */
		for (ps = peers->remote; ps; ps = ps->next) {
			/* For each remote peers */
			if (!ps->local) {
				if (!ps->appctx) {
					/* no active peer connection */
					if (ps->statuscode == 0 ||
					    ((ps->statuscode == PEER_SESS_SC_CONNECTCODE ||
					      ps->statuscode == PEER_SESS_SC_SUCCESSCODE ||
					      ps->statuscode == PEER_SESS_SC_CONNECTEDCODE) &&
					     tick_is_expired(ps->reconnect, now_ms))) {
						/* connection never tried
						 * or previous peer connection established with success
						 * or previous peer connection failed while connecting
						 * and reconnection timer is expired */

						/* retry a connect */
						ps->appctx = peer_session_create(peers, ps);
					}
					else if (!tick_is_expired(ps->reconnect, now_ms)) {
						/* If previous session failed during connection
						 * but reconnection timer is not expired */

						/* reschedule task for reconnect */
						task->expire = tick_first(task->expire, ps->reconnect);
					}
					/* else do nothing */
				} /* !ps->appctx */
				else if (ps->statuscode == PEER_SESS_SC_SUCCESSCODE) {
					/* current peer connection is active and established */
					if (((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMREMOTE) &&
					    !(peers->flags & PEERS_F_RESYNC_ASSIGN) &&
					    !(ps->flags & PEER_F_LEARN_NOTUP2DATE)) {
						/* Resync from a remote is needed
						 * and no peer was assigned for lesson
						 * and current peer may be up2date */

						/* assign peer for the lesson */
						ps->flags |= PEER_F_LEARN_ASSIGN;
						peers->flags |= PEERS_F_RESYNC_ASSIGN;

						/* wake up peer handler to handle a request of resync */
						appctx_wakeup(ps->appctx);
					}
					else {
						int update_to_push = 0;

						/* Awake session if there is data to push */
						for (st = ps->tables; st ; st = st->next) {
							if ((int)(st->last_pushed - st->table->localupdate) < 0) {
								/* wake up the peer handler to push local updates */
								update_to_push = 1;
								/* There is no need to send a heartbeat message
								 * when some updates must be pushed. The remote
								 * peer will consider <ps> peer as alive when it will
								 * receive these updates.
								 */
								ps->flags &= ~PEER_F_HEARTBEAT;
								/* Re-schedule another one later. */
								ps->heartbeat = tick_add(now_ms, MS_TO_TICKS(PEER_HEARTBEAT_TIMEOUT));
								/* We are going to send updates, let's ensure we will
								 * come back to send heartbeat messages or to reconnect.
								 */
								task->expire = tick_first(ps->reconnect, ps->heartbeat);
								appctx_wakeup(ps->appctx);
								break;
							}
						}
						/* When there are updates to send we do not reconnect
						 * and do not send heartbeat message either.
						 */
						if (!update_to_push) {
							if (tick_is_expired(ps->reconnect, now_ms)) {
								if (ps->flags & PEER_F_ALIVE) {
									/* This peer was alive during a 'reconnect' period.
									 * Flag it as not alive again for the next period.
									 */
									ps->flags &= ~PEER_F_ALIVE;
									ps->reconnect = tick_add(now_ms, MS_TO_TICKS(PEER_RECONNECT_TIMEOUT));
								}
								else  {
									ps->reconnect = tick_add(now_ms, MS_TO_TICKS(50 + random() % 2000));
									peer_session_forceshutdown(ps);
								}
							}
							else if (tick_is_expired(ps->heartbeat, now_ms)) {
								ps->heartbeat = tick_add(now_ms, MS_TO_TICKS(PEER_HEARTBEAT_TIMEOUT));
								ps->flags |= PEER_F_HEARTBEAT;
								appctx_wakeup(ps->appctx);
							}
							task->expire = tick_first(ps->reconnect, ps->heartbeat);
						}
					}
					/* else do nothing */
				} /* SUCCESSCODE */
			} /* !ps->peer->local */
		} /* for */

		/* Resync from remotes expired: consider resync is finished */
		if (((peers->flags & PEERS_RESYNC_STATEMASK) == PEERS_RESYNC_FROMREMOTE) &&
		    !(peers->flags & PEERS_F_RESYNC_ASSIGN) &&
		    tick_is_expired(peers->resync_timeout, now_ms)) {
			/* Resync from remote peer needed
			 * no peer was assigned for the lesson
			 * and resync timeout expire */

			/* flag no more resync from remote, consider resync is finished */
			peers->flags |= PEERS_F_RESYNC_REMOTE;
		}

		if ((peers->flags & PEERS_RESYNC_STATEMASK) != PEERS_RESYNC_FINISHED) {
			/* Resync not finished*/
			/* reschedule task to resync timeout if not expired, to ended resync if needed */
			if (!tick_is_expired(peers->resync_timeout, now_ms))
				task->expire = tick_first(task->expire, peers->resync_timeout);
		}
	} /* !stopping */
	else {
		/* soft stop case */
		if (state & TASK_WOKEN_SIGNAL) {
			/* We've just received the signal */
			if (!(peers->flags & PEERS_F_DONOTSTOP)) {
				/* add DO NOT STOP flag if not present */
				_HA_ATOMIC_ADD(&jobs, 1);
				peers->flags |= PEERS_F_DONOTSTOP;
				ps = peers->local;
				for (st = ps->tables; st ; st = st->next)
					st->table->syncing++;
			}

			/* disconnect all connected peers */
			for (ps = peers->remote; ps; ps = ps->next) {
				/* we're killing a connection, we must apply a random delay before
				 * retrying otherwise the other end will do the same and we can loop
				 * for a while.
				 */
				ps->reconnect = tick_add(now_ms, MS_TO_TICKS(50 + random() % 2000));
				if (ps->appctx) {
					peer_session_forceshutdown(ps);
				}
			}
		}

		ps = peers->local;
		if (ps->flags & PEER_F_TEACH_COMPLETE) {
			if (peers->flags & PEERS_F_DONOTSTOP) {
				/* resync of new process was complete, current process can die now */
				_HA_ATOMIC_SUB(&jobs, 1);
				peers->flags &= ~PEERS_F_DONOTSTOP;
				for (st = ps->tables; st ; st = st->next)
					st->table->syncing--;
			}
		}
		else if (!ps->appctx) {
			/* If there's no active peer connection */
			if (ps->statuscode == 0 ||
			    ps->statuscode == PEER_SESS_SC_SUCCESSCODE ||
			    ps->statuscode == PEER_SESS_SC_CONNECTEDCODE ||
			    ps->statuscode == PEER_SESS_SC_TRYAGAIN) {
				/* connection never tried
				 * or previous peer connection was successfully established
				 * or previous tcp connect succeeded but init state incomplete
				 * or during previous connect, peer replies a try again statuscode */

				/* connect to the peer */
				peer_session_create(peers, ps);
			}
			else {
				/* Other error cases */
				if (peers->flags & PEERS_F_DONOTSTOP) {
					/* unable to resync new process, current process can die now */
					_HA_ATOMIC_SUB(&jobs, 1);
					peers->flags &= ~PEERS_F_DONOTSTOP;
					for (st = ps->tables; st ; st = st->next)
						st->table->syncing--;
				}
			}
		}
		else if (ps->statuscode == PEER_SESS_SC_SUCCESSCODE ) {
			/* current peer connection is active and established
			 * wake up all peer handlers to push remaining local updates */
			for (st = ps->tables; st ; st = st->next) {
				if ((int)(st->last_pushed - st->table->localupdate) < 0) {
					appctx_wakeup(ps->appctx);
					break;
				}
			}
		}
	} /* stopping */

	/* Release lock for all peers of the section */
	for (ps = peers->remote; ps; ps = ps->next)
		HA_SPIN_UNLOCK(PEER_LOCK, &ps->lock);

	/* Wakeup for re-connect */
	return task;
}


/*
 * returns 0 in case of error.
 */
int peers_init_sync(struct peers *peers)
{
	struct peer * curpeer;

	for (curpeer = peers->remote; curpeer; curpeer = curpeer->next) {
		peers->peers_fe->maxconn += 3;
	}

	peers->sync_task = task_new(MAX_THREADS_MASK);
	if (!peers->sync_task)
		return 0;

	peers->sync_task->process = process_peer_sync;
	peers->sync_task->context = (void *)peers;
	peers->sighandler = signal_register_task(0, peers->sync_task, 0);
	task_wakeup(peers->sync_task, TASK_WOKEN_INIT);
	return 1;
}

/*
 * Allocate a cache a dictionary entries used upon transmission.
 */
static struct dcache_tx *new_dcache_tx(size_t max_entries)
{
	struct dcache_tx *d;
	struct ebpt_node *entries;

	d = malloc(sizeof *d);
	entries = calloc(max_entries, sizeof *entries);
	if (!d || !entries)
		goto err;

	d->lru_key = 0;
	d->prev_lookup = NULL;
	d->cached_entries = EB_ROOT_UNIQUE;
	d->entries = entries;

	return d;

 err:
	free(d);
	free(entries);
	return NULL;
}

static void free_dcache_tx(struct dcache_tx *dc)
{
	free(dc->entries);
	dc->entries = NULL;
	free(dc);
}

/*
 * Allocate a cache of dictionary entries with <name> as name and <max_entries>
 * as maximum of entries.
 * Return the dictionay cache if succeeded, NULL if not.
 * Must be deallocated calling free_dcache().
 */
static struct dcache *new_dcache(size_t max_entries)
{
	struct dcache_tx *dc_tx;
	struct dcache *dc;
	struct dcache_rx *dc_rx;

	dc = calloc(1, sizeof *dc);
	dc_tx = new_dcache_tx(max_entries);
	dc_rx = calloc(max_entries, sizeof *dc_rx);
	if (!dc || !dc_tx || !dc_rx)
		goto err;

	dc->tx = dc_tx;
	dc->rx = dc_rx;
	dc->max_entries = max_entries;

	return dc;

 err:
	free(dc);
	free(dc_tx);
	free(dc_rx);
	return NULL;
}

/*
 * Deallocate a cache of dictionary entries.
 */
static inline void free_dcache(struct dcache *dc)
{
	free_dcache_tx(dc->tx);
	dc->tx = NULL;
	free(dc->rx); dc->rx = NULL;
	free(dc);
}


/*
 * Look for the dictionary entry with the value of <i> in <d> cache of dictionary
 * entries used upon transmission.
 * Return the entry if found, NULL if not.
 */
static struct ebpt_node *dcache_tx_lookup_value(struct dcache_tx *d,
                                                struct dcache_tx_entry *i)
{
	return ebpt_lookup(&d->cached_entries, i->entry.key);
}

/*
 * Flush <dc> cache.
 * Always succeeds.
 */
static inline void flush_dcache(struct peer *peer)
{
	int i;
	struct dcache *dc = peer->dcache;

	for (i = 0; i < dc->max_entries; i++)
		ebpt_delete(&dc->tx->entries[i]);

	memset(dc->rx, 0, dc->max_entries * sizeof *dc->rx);
}

/*
 * Insert a dictionary entry in <dc> cache part used upon transmission (->tx)
 * with information provided by <i> dictionary cache entry (especially the value
 * to be inserted if not already). Return <i> if already present in the cache
 * or something different of <i> if not.
 */
static struct ebpt_node *dcache_tx_insert(struct dcache *dc, struct dcache_tx_entry *i)
{
	struct dcache_tx *dc_tx;
	struct ebpt_node *o;

	dc_tx = dc->tx;

	if (dc_tx->prev_lookup && dc_tx->prev_lookup->key == i->entry.key) {
		o = dc_tx->prev_lookup;
	} else {
		o = dcache_tx_lookup_value(dc_tx, i);
		if (o) {
			/* Save it */
			dc_tx->prev_lookup = o;
		}
	}

	if (o) {
		/* Copy the ID. */
		i->id = o - dc->tx->entries;
		return &i->entry;
	}

	/* The new entry to put in cache */
	dc_tx->prev_lookup = o = &dc_tx->entries[dc_tx->lru_key];

	ebpt_delete(o);
	o->key = i->entry.key;
	ebpt_insert(&dc_tx->cached_entries, o);
	i->id = dc_tx->lru_key;

	/* Update the index for the next entry to put in cache */
	dc_tx->lru_key = (dc_tx->lru_key + 1) & (dc->max_entries - 1);

	return o;
}

/*
 * Allocate a dictionary cache for each peer of <peers> section.
 * Return 1 if succeeded, 0 if not.
 */
int peers_alloc_dcache(struct peers *peers)
{
	struct peer *p;

	for (p = peers->remote; p; p = p->next) {
		p->dcache = new_dcache(PEER_STKT_CACHE_MAX_ENTRIES);
		if (!p->dcache)
			return 0;
	}

	return 1;
}

/*
 * Function used to register a table for sync on a group of peers
 *
 */
void peers_register_table(struct peers *peers, struct stktable *table)
{
	struct shared_table *st;
	struct peer * curpeer;
	int id = 0;

	for (curpeer = peers->remote; curpeer; curpeer = curpeer->next) {
		st = calloc(1,sizeof(*st));
		st->table = table;
		st->next = curpeer->tables;
		if (curpeer->tables)
			id = curpeer->tables->local_id;
		st->local_id = id + 1;

		curpeer->tables = st;
	}

	table->sync_task = peers->sync_task;
}

/*
 * Parse the "show peers" command arguments.
 * Returns 0 if succeeded, 1 if not with the ->msg of the appctx set as
 * error message.
 */
static int cli_parse_show_peers(char **args, char *payload, struct appctx *appctx, void *private)
{
	appctx->ctx.cfgpeers.target = NULL;

	if (*args[2]) {
		struct peers *p;

		for (p = cfg_peers; p; p = p->next) {
			if (!strcmp(p->id, args[2])) {
				appctx->ctx.cfgpeers.target = p;
				break;
			}
		}

		if (!p) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "No such peers\n";
			appctx->st0 = CLI_ST_PRINT;
			return 1;
		}
	}

	return 0;
}

/*
 * This function dumps the peer state information of <peers> "peers" section.
 * Returns 0 if the output buffer is full and needs to be called again, non-zero if not.
 * Dedicated to be called by cli_io_handler_show_peers() cli I/O handler.
 */
static int peers_dump_head(struct buffer *msg, struct stream_interface *si, struct peers *peers)
{
	struct tm tm;

	get_localtime(peers->last_change, &tm);
	chunk_appendf(msg, "%p: [%02d/%s/%04d:%02d:%02d:%02d] id=%s state=%d flags=0x%x resync_timeout=%s task_calls=%u\n",
	              peers,
	              tm.tm_mday, monthname[tm.tm_mon], tm.tm_year+1900,
	              tm.tm_hour, tm.tm_min, tm.tm_sec,
	              peers->id, peers->state, peers->flags,
	              peers->resync_timeout ?
			             tick_is_expired(peers->resync_timeout, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(peers->resync_timeout - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
	              peers->sync_task ? peers->sync_task->calls : 0);

	if (ci_putchk(si_ic(si), msg) == -1) {
		si_rx_room_blk(si);
		return 0;
	}

	return 1;
}

/*
 * This function dumps <peer> state information.
 * Returns 0 if the output buffer is full and needs to be called again, non-zero
 * if not. Dedicated to be called by cli_io_handler_show_peers() cli I/O handler.
 */
static int peers_dump_peer(struct buffer *msg, struct stream_interface *si, struct peer *peer)
{
	struct connection *conn;
	char pn[INET6_ADDRSTRLEN];
	struct stream_interface *peer_si;
	struct stream *peer_s;
	struct appctx *appctx;
	struct shared_table *st;

	addr_to_str(&peer->addr, pn, sizeof pn);
	chunk_appendf(msg, "  %p: id=%s(%s) addr=%s:%d status=%s reconnect=%s confirm=%u\n",
	              peer, peer->id,
	              peer->local ? "local" : "remote",
	              pn, get_host_port(&peer->addr),
	              statuscode_str(peer->statuscode),
	              peer->reconnect ?
			             tick_is_expired(peer->reconnect, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(peer->reconnect - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
	              peer->confirm);

	chunk_appendf(&trash, "        flags=0x%x", peer->flags);

	appctx = peer->appctx;
	if (!appctx)
		goto end;

	chunk_appendf(&trash, " appctx:%p st0=%d st1=%d task_calls=%u", appctx, appctx->st0, appctx->st1,
	                                                                appctx->t ? appctx->t->calls : 0);

	peer_si = peer->appctx->owner;
	if (!peer_si)
		goto end;

	peer_s = si_strm(peer_si);
	if (!peer_s)
		goto end;

	chunk_appendf(&trash, " state=%s", si_state_str(si_opposite(peer_si)->state));

	conn = objt_conn(strm_orig(peer_s));
	if (conn)
		chunk_appendf(&trash, "\n        xprt=%s", conn_get_xprt_name(conn));

	switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
	case AF_INET:
	case AF_INET6:
		chunk_appendf(&trash, " src=%s:%d", pn, get_host_port(&conn->addr.from));
		break;
	case AF_UNIX:
		chunk_appendf(&trash, " src=unix:%d", strm_li(peer_s)->luid);
		break;
	}

	if (conn)
		conn_get_to_addr(conn);

	switch (conn ? addr_to_str(&conn->addr.to, pn, sizeof(pn)) : AF_UNSPEC) {
	case AF_INET:
	case AF_INET6:
		chunk_appendf(&trash, " addr=%s:%d", pn, get_host_port(&conn->addr.to));
		break;
	case AF_UNIX:
		chunk_appendf(&trash, " addr=unix:%d", strm_li(peer_s)->luid);
		break;
	}

	if (peer->remote_table)
		chunk_appendf(&trash, "\n        remote_table:%p id=%s local_id=%d remote_id=%d",
		              peer->remote_table,
		              peer->remote_table->table->id,
		              peer->remote_table->local_id,
		              peer->remote_table->remote_id);

	if (peer->last_local_table)
		chunk_appendf(&trash, "\n        last_local_table:%p id=%s local_id=%d remote_id=%d",
		              peer->last_local_table,
		              peer->last_local_table->table->id,
		              peer->last_local_table->local_id,
		              peer->last_local_table->remote_id);

	if (peer->tables) {
		chunk_appendf(&trash, "\n        shared tables:");
		for (st = peer->tables; st; st = st->next) {
			int i, count;
			struct stktable *t;
			struct dcache *dcache;

			t = st->table;
			dcache = peer->dcache;

			chunk_appendf(&trash, "\n          %p local_id=%d remote_id=%d "
			              "flags=0x%x remote_data=0x%llx",
			              st, st->local_id, st->remote_id,
			              st->flags, (unsigned long long)st->remote_data);
			chunk_appendf(&trash, "\n              last_acked=%u last_pushed=%u last_get=%u"
			              " teaching_origin=%u update=%u",
			              st->last_acked, st->last_pushed, st->last_get,
			              st->teaching_origin, st->update);
			chunk_appendf(&trash, "\n              table:%p id=%s update=%u localupdate=%u"
			              " commitupdate=%u syncing=%u",
			              t, t->id, t->update, t->localupdate, t->commitupdate, t->syncing);
			chunk_appendf(&trash, "\n        TX dictionary cache:");
			count = 0;
			for (i = 0; i < dcache->max_entries; i++) {
				struct ebpt_node *node;
				struct dict_entry *de;

				node = &dcache->tx->entries[i];
				if (!node->key)
					break;

				if (!count++)
					chunk_appendf(&trash, "\n        ");
				de = node->key;
				chunk_appendf(&trash, "  %3u -> %s", i, (char *)de->value.key);
				count &= 0x3;
			}
			chunk_appendf(&trash, "\n        RX dictionary cache:");
			count = 0;
			for (i = 0; i < dcache->max_entries; i++) {
				if (!count++)
					chunk_appendf(&trash, "\n        ");
				chunk_appendf(&trash, "  %3u -> %s", i,
				              dcache->rx[i].de ?
				                  (char *)dcache->rx[i].de->value.key : "-");
				count &= 0x3;
			}
		}
	}

 end:
	chunk_appendf(&trash, "\n");
	if (ci_putchk(si_ic(si), msg) == -1) {
		si_rx_room_blk(si);
		return 0;
	}

	return 1;
}

/*
 * This function dumps all the peers of "peers" section.
 * Returns 0 if the output buffer is full and needs to be called
 * again, non-zero if not. It proceeds in an isolated thread, so
 * there is no thread safety issue here.
 */
static int cli_io_handler_show_peers(struct appctx *appctx)
{
	int show_all;
	int ret = 0, first_peers = 1;
	struct stream_interface *si = appctx->owner;

	thread_isolate();

	show_all = !appctx->ctx.cfgpeers.target;

	chunk_reset(&trash);

	while (appctx->st2 != STAT_ST_FIN) {
		switch (appctx->st2) {
		case STAT_ST_INIT:
			if (show_all)
				appctx->ctx.cfgpeers.peers = cfg_peers;
			else
				appctx->ctx.cfgpeers.peers = appctx->ctx.cfgpeers.target;

			appctx->st2 = STAT_ST_LIST;
			/* fall through */

		case STAT_ST_LIST:
			if (!appctx->ctx.cfgpeers.peers) {
				/* No more peers list. */
				appctx->st2 = STAT_ST_END;
			}
			else {
				if (!first_peers)
					chunk_appendf(&trash, "\n");
				else
					first_peers = 0;
				if (!peers_dump_head(&trash, si, appctx->ctx.cfgpeers.peers))
					goto out;

				appctx->ctx.cfgpeers.peer = appctx->ctx.cfgpeers.peers->remote;
				appctx->ctx.cfgpeers.peers = appctx->ctx.cfgpeers.peers->next;
				appctx->st2 = STAT_ST_INFO;
			}
			break;

		case STAT_ST_INFO:
			if (!appctx->ctx.cfgpeers.peer) {
				/* End of peer list */
				if (show_all)
					appctx->st2 = STAT_ST_LIST;
			    else
					appctx->st2 = STAT_ST_END;
			}
			else {
				if (!peers_dump_peer(&trash, si, appctx->ctx.cfgpeers.peer))
					goto out;

				appctx->ctx.cfgpeers.peer = appctx->ctx.cfgpeers.peer->next;
			}
		    break;

		case STAT_ST_END:
			appctx->st2 = STAT_ST_FIN;
			break;
		}
	}
	ret = 1;
 out:
	thread_release();
	return ret;
}

/*
 * CLI keywords.
 */
static struct cli_kw_list cli_kws = {{ }, {
	{ { "show", "peers", NULL }, "show peers [peers section]: dump some information about all the peers or this peers section", cli_parse_show_peers, cli_io_handler_show_peers, },
	{},
}};

/* Register cli keywords */
INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

