/*
 * include/types/ssl_sock.h
 * SSL settings for listeners and servers
 *
 * Copyright (C) 2012 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _TYPES_SSL_SOCK_H
#define _TYPES_SSL_SOCK_H
#ifdef USE_OPENSSL

#include <ebmbtree.h>

#include <common/hathreads.h>
#include <common/openssl-compat.h>

struct pkey_info {
	uint8_t sig;          /* TLSEXT_signature_[rsa,ecdsa,...] */
	uint16_t bits;        /* key size in bits */
};

struct sni_ctx {
	SSL_CTX *ctx;             /* context associated to the certificate */
	int order;                /* load order for the certificate */
	uint8_t neg;              /* reject if match */
	struct pkey_info kinfo;   /* pkey info */
	struct ssl_bind_conf *conf; /* ssl "bind" conf for the certificate */
	struct ebmb_node name;    /* node holding the servername value */
};

struct tls_version_filter {
	uint16_t flags;     /* ssl options */
	uint8_t  min;      /* min TLS version */
	uint8_t  max;      /* max TLS version */
};

extern struct list tlskeys_reference;

struct tls_sess_key_128 {
	unsigned char name[16];
	unsigned char aes_key[16];
	unsigned char hmac_key[16];
} __attribute__((packed));

struct tls_sess_key_256 {
	unsigned char name[16];
	unsigned char aes_key[32];
	unsigned char hmac_key[32];
} __attribute__((packed));

union tls_sess_key{
	unsigned char name[16];
	struct tls_sess_key_128 key_128;
	struct tls_sess_key_256 key_256;
} __attribute__((packed));

struct tls_keys_ref {
	struct list list; /* Used to chain refs. */
	char *filename;
	int unique_id; /* Each pattern reference have unique id. */
	int refcount;  /* number of users of this tls_keys_ref. */
	union tls_sess_key *tlskeys;
	int tls_ticket_enc_index;
	int key_size_bits;
	__decl_hathreads(HA_RWLOCK_T lock); /* lock used to protect the ref */
};

/* shared ssl session */
struct sh_ssl_sess_hdr {
	struct ebmb_node key;
	unsigned char key_data[SSL_MAX_SSL_SESSION_ID_LENGTH];
};

#endif /* USE_OPENSSL */
#endif /* _TYPES_SSL_SOCK_H */
