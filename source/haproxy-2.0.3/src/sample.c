/*
 * Sample management functions.
 *
 * Copyright 2009-2010 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 * Copyright (C) 2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

#include <types/global.h>

#include <common/chunk.h>
#include <common/hash.h>
#include <common/http.h>
#include <common/initcall.h>
#include <common/standard.h>
#include <common/uri_auth.h>
#include <common/base64.h>

#include <proto/arg.h>
#include <proto/auth.h>
#include <proto/log.h>
#include <proto/proxy.h>
#include <proto/protocol_buffers.h>
#include <proto/sample.h>
#include <proto/stick_table.h>
#include <proto/vars.h>

#include <import/sha1.h>
#include <import/xxhash.h>

/* sample type names */
const char *smp_to_type[SMP_TYPES] = {
	[SMP_T_ANY]  = "any",
	[SMP_T_BOOL] = "bool",
	[SMP_T_SINT] = "sint",
	[SMP_T_ADDR] = "addr",
	[SMP_T_IPV4] = "ipv4",
	[SMP_T_IPV6] = "ipv6",
	[SMP_T_STR]  = "str",
	[SMP_T_BIN]  = "bin",
	[SMP_T_METH] = "meth",
};

/* static sample used in sample_process() when <p> is NULL */
static THREAD_LOCAL struct sample temp_smp;

/* list head of all known sample fetch keywords */
static struct sample_fetch_kw_list sample_fetches = {
	.list = LIST_HEAD_INIT(sample_fetches.list)
};

/* list head of all known sample format conversion keywords */
static struct sample_conv_kw_list sample_convs = {
	.list = LIST_HEAD_INIT(sample_convs.list)
};

const unsigned int fetch_cap[SMP_SRC_ENTRIES] = {
	[SMP_SRC_INTRN] = (SMP_VAL_FE_CON_ACC | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_LISTN] = (SMP_VAL_FE_CON_ACC | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_FTEND] = (SMP_VAL_FE_CON_ACC | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L4CLI] = (SMP_VAL_FE_CON_ACC | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L5CLI] = (SMP_VAL___________ | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_TRACK] = (SMP_VAL_FE_CON_ACC | SMP_VAL_FE_SES_ACC | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L6REQ] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________),

	[SMP_SRC_HRQHV] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________),

	[SMP_SRC_HRQHP] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_FE_REQ_CNT |
	                   SMP_VAL_FE_HRQ_HDR | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_HRQBO] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL_FE_HRQ_BDY | SMP_VAL_FE_SET_BCK |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________),

	[SMP_SRC_BKEND] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL_BE_REQ_CNT | SMP_VAL_BE_HRQ_HDR | SMP_VAL_BE_HRQ_BDY |
	                   SMP_VAL_BE_SET_SRV | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_SERVR] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL_BE_SRV_CON | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L4SRV] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L5SRV] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_L6RES] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL___________),

	[SMP_SRC_HRSHV] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL___________),

	[SMP_SRC_HRSHP] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL_BE_RES_CNT |
	                   SMP_VAL_BE_HRS_HDR | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_HRSBO] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL_BE_HRS_BDY | SMP_VAL_BE_STO_RUL |
	                   SMP_VAL_FE_RES_CNT | SMP_VAL_FE_HRS_HDR | SMP_VAL_FE_HRS_BDY |
	                   SMP_VAL___________),

	[SMP_SRC_RQFIN] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_RSFIN] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_TXFIN] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL_FE_LOG_END),

	[SMP_SRC_SSFIN] = (SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL___________ | SMP_VAL___________ | SMP_VAL___________ |
	                   SMP_VAL_FE_LOG_END),
};

static const char *fetch_src_names[SMP_SRC_ENTRIES] = {
	[SMP_SRC_INTRN] = "internal state",
	[SMP_SRC_LISTN] = "listener",
	[SMP_SRC_FTEND] = "frontend",
	[SMP_SRC_L4CLI] = "client address",
	[SMP_SRC_L5CLI] = "client-side connection",
	[SMP_SRC_TRACK] = "track counters",
	[SMP_SRC_L6REQ] = "request buffer",
	[SMP_SRC_HRQHV] = "HTTP request headers",
	[SMP_SRC_HRQHP] = "HTTP request",
	[SMP_SRC_HRQBO] = "HTTP request body",
	[SMP_SRC_BKEND] = "backend",
	[SMP_SRC_SERVR] = "server",
	[SMP_SRC_L4SRV] = "server address",
	[SMP_SRC_L5SRV] = "server-side connection",
	[SMP_SRC_L6RES] = "response buffer",
	[SMP_SRC_HRSHV] = "HTTP response headers",
	[SMP_SRC_HRSHP] = "HTTP response",
	[SMP_SRC_HRSBO] = "HTTP response body",
	[SMP_SRC_RQFIN] = "request buffer statistics",
	[SMP_SRC_RSFIN] = "response buffer statistics",
	[SMP_SRC_TXFIN] = "transaction statistics",
	[SMP_SRC_SSFIN] = "session statistics",
};

static const char *fetch_ckp_names[SMP_CKP_ENTRIES] = {
	[SMP_CKP_FE_CON_ACC] = "frontend tcp-request connection rule",
	[SMP_CKP_FE_SES_ACC] = "frontend tcp-request session rule",
	[SMP_CKP_FE_REQ_CNT] = "frontend tcp-request content rule",
	[SMP_CKP_FE_HRQ_HDR] = "frontend http-request header rule",
	[SMP_CKP_FE_HRQ_BDY] = "frontend http-request body rule",
	[SMP_CKP_FE_SET_BCK] = "frontend use-backend rule",
	[SMP_CKP_BE_REQ_CNT] = "backend tcp-request content rule",
	[SMP_CKP_BE_HRQ_HDR] = "backend http-request header rule",
	[SMP_CKP_BE_HRQ_BDY] = "backend http-request body rule",
	[SMP_CKP_BE_SET_SRV] = "backend use-server, balance or stick-match rule",
	[SMP_CKP_BE_SRV_CON] = "server source selection",
	[SMP_CKP_BE_RES_CNT] = "backend tcp-response content rule",
	[SMP_CKP_BE_HRS_HDR] = "backend http-response header rule",
	[SMP_CKP_BE_HRS_BDY] = "backend http-response body rule",
	[SMP_CKP_BE_STO_RUL] = "backend stick-store rule",
	[SMP_CKP_FE_RES_CNT] = "frontend tcp-response content rule",
	[SMP_CKP_FE_HRS_HDR] = "frontend http-response header rule",
	[SMP_CKP_FE_HRS_BDY] = "frontend http-response body rule",
	[SMP_CKP_FE_LOG_END] = "logs",
};

/* This function returns the type of the data returned by the sample_expr.
 * It assumes that the <expr> and all of its converters are properly
 * initialized.
 */
inline
int smp_expr_output_type(struct sample_expr *expr)
{
	struct sample_conv_expr *smp_expr;

	if (!LIST_ISEMPTY(&expr->conv_exprs)) {
		smp_expr = LIST_PREV(&expr->conv_exprs, struct sample_conv_expr *, list);
		return smp_expr->conv->out_type;
	}
	return expr->fetch->out_type;
}


/* fill the trash with a comma-delimited list of source names for the <use> bit
 * field which must be composed of a non-null set of SMP_USE_* flags. The return
 * value is the pointer to the string in the trash buffer.
 */
const char *sample_src_names(unsigned int use)
{
	int bit;

	trash.data = 0;
	trash.area[0] = '\0';
	for (bit = 0; bit < SMP_SRC_ENTRIES; bit++) {
		if (!(use & ~((1 << bit) - 1)))
			break; /* no more bits */

		if (!(use & (1 << bit)))
			continue; /* bit not set */

		trash.data += snprintf(trash.area + trash.data,
				       trash.size - trash.data, "%s%s",
				       (use & ((1 << bit) - 1)) ? "," : "",
				       fetch_src_names[bit]);
	}
	return trash.area;
}

/* return a pointer to the correct sample checkpoint name, or "unknown" when
 * the flags are invalid. Only the lowest bit is used, higher bits are ignored
 * if set.
 */
const char *sample_ckp_names(unsigned int use)
{
	int bit;

	for (bit = 0; bit < SMP_CKP_ENTRIES; bit++)
		if (use & (1 << bit))
			return fetch_ckp_names[bit];
	return "unknown sample check place, please report this bug";
}

/*
 * Registers the sample fetch keyword list <kwl> as a list of valid keywords
 * for next parsing sessions. The fetch keywords capabilities are also computed
 * from their ->use field.
 */
void sample_register_fetches(struct sample_fetch_kw_list *kwl)
{
	struct sample_fetch *sf;
	int bit;

	for (sf = kwl->kw; sf->kw != NULL; sf++) {
		for (bit = 0; bit < SMP_SRC_ENTRIES; bit++)
			if (sf->use & (1 << bit))
				sf->val |= fetch_cap[bit];
	}
	LIST_ADDQ(&sample_fetches.list, &kwl->list);
}

/*
 * Registers the sample format coverstion keyword list <pckl> as a list of valid keywords for next
 * parsing sessions.
 */
void sample_register_convs(struct sample_conv_kw_list *pckl)
{
	LIST_ADDQ(&sample_convs.list, &pckl->list);
}

/*
 * Returns the pointer on sample fetch keyword structure identified by
 * string of <len> in buffer <kw>.
 *
 */
struct sample_fetch *find_sample_fetch(const char *kw, int len)
{
	int index;
	struct sample_fetch_kw_list *kwl;

	list_for_each_entry(kwl, &sample_fetches.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if (strncmp(kwl->kw[index].kw, kw, len) == 0 &&
			    kwl->kw[index].kw[len] == '\0')
				return &kwl->kw[index];
		}
	}
	return NULL;
}

/* This function browses the list of available sample fetches. <current> is
 * the last used sample fetch. If it is the first call, it must set to NULL.
 * <idx> is the index of the next sample fetch entry. It is used as private
 * value. It is useless to initiate it.
 *
 * It returns always the new fetch_sample entry, and NULL when the end of
 * the list is reached.
 */
struct sample_fetch *sample_fetch_getnext(struct sample_fetch *current, int *idx)
{
	struct sample_fetch_kw_list *kwl;
	struct sample_fetch *base;

	if (!current) {
		/* Get first kwl entry. */
		kwl = LIST_NEXT(&sample_fetches.list, struct sample_fetch_kw_list *, list);
		(*idx) = 0;
	} else {
		/* Get kwl corresponding to the curret entry. */
		base = current + 1 - (*idx);
		kwl = container_of(base, struct sample_fetch_kw_list, kw);
	}

	while (1) {

		/* Check if kwl is the last entry. */
		if (&kwl->list == &sample_fetches.list)
			return NULL;

		/* idx contain the next keyword. If it is available, return it. */
		if (kwl->kw[*idx].kw) {
			(*idx)++;
			return &kwl->kw[(*idx)-1];
		}

		/* get next entry in the main list, and return NULL if the end is reached. */
		kwl = LIST_NEXT(&kwl->list, struct sample_fetch_kw_list *, list);

		/* Set index to 0, ans do one other loop. */
		(*idx) = 0;
	}
}

/* This function browses the list of available converters. <current> is
 * the last used converter. If it is the first call, it must set to NULL.
 * <idx> is the index of the next converter entry. It is used as private
 * value. It is useless to initiate it.
 *
 * It returns always the next sample_conv entry, and NULL when the end of
 * the list is reached.
 */
struct sample_conv *sample_conv_getnext(struct sample_conv *current, int *idx)
{
	struct sample_conv_kw_list *kwl;
	struct sample_conv *base;

	if (!current) {
		/* Get first kwl entry. */
		kwl = LIST_NEXT(&sample_convs.list, struct sample_conv_kw_list *, list);
		(*idx) = 0;
	} else {
		/* Get kwl corresponding to the curret entry. */
		base = current + 1 - (*idx);
		kwl = container_of(base, struct sample_conv_kw_list, kw);
	}

	while (1) {
		/* Check if kwl is the last entry. */
		if (&kwl->list == &sample_convs.list)
			return NULL;

		/* idx contain the next keyword. If it is available, return it. */
		if (kwl->kw[*idx].kw) {
			(*idx)++;
			return &kwl->kw[(*idx)-1];
		}

		/* get next entry in the main list, and return NULL if the end is reached. */
		kwl = LIST_NEXT(&kwl->list, struct sample_conv_kw_list *, list);

		/* Set index to 0, ans do one other loop. */
		(*idx) = 0;
	}
}

/*
 * Returns the pointer on sample format conversion keyword structure identified by
 * string of <len> in buffer <kw>.
 *
 */
struct sample_conv *find_sample_conv(const char *kw, int len)
{
	int index;
	struct sample_conv_kw_list *kwl;

	list_for_each_entry(kwl, &sample_convs.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if (strncmp(kwl->kw[index].kw, kw, len) == 0 &&
			    kwl->kw[index].kw[len] == '\0')
				return &kwl->kw[index];
		}
	}
	return NULL;
}

/******************************************************************/
/*          Sample casts functions                                */
/******************************************************************/

static int c_ip2int(struct sample *smp)
{
	smp->data.u.sint = ntohl(smp->data.u.ipv4.s_addr);
	smp->data.type = SMP_T_SINT;
	return 1;
}

static int c_ip2str(struct sample *smp)
{
	struct buffer *trash = get_trash_chunk();

	if (!inet_ntop(AF_INET, (void *)&smp->data.u.ipv4, trash->area, trash->size))
		return 0;

	trash->data = strlen(trash->area);
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	smp->flags &= ~SMP_F_CONST;

	return 1;
}

static int c_ip2ipv6(struct sample *smp)
{
	v4tov6(&smp->data.u.ipv6, &smp->data.u.ipv4);
	smp->data.type = SMP_T_IPV6;
	return 1;
}

static int c_ipv62ip(struct sample *smp)
{
	if (!v6tov4(&smp->data.u.ipv4, &smp->data.u.ipv6))
		return 0;
	smp->data.type = SMP_T_IPV4;
	return 1;
}

static int c_ipv62str(struct sample *smp)
{
	struct buffer *trash = get_trash_chunk();

	if (!inet_ntop(AF_INET6, (void *)&smp->data.u.ipv6, trash->area, trash->size))
		return 0;

	trash->data = strlen(trash->area);
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

/*
static int c_ipv62ip(struct sample *smp)
{
	return v6tov4(&smp->data.u.ipv4, &smp->data.u.ipv6);
}
*/

static int c_int2ip(struct sample *smp)
{
	smp->data.u.ipv4.s_addr = htonl((unsigned int)smp->data.u.sint);
	smp->data.type = SMP_T_IPV4;
	return 1;
}

static int c_int2ipv6(struct sample *smp)
{
	smp->data.u.ipv4.s_addr = htonl((unsigned int)smp->data.u.sint);
	v4tov6(&smp->data.u.ipv6, &smp->data.u.ipv4);
	smp->data.type = SMP_T_IPV6;
	return 1;
}

static int c_str2addr(struct sample *smp)
{
	if (!buf2ip(smp->data.u.str.area, smp->data.u.str.data, &smp->data.u.ipv4)) {
		if (!buf2ip6(smp->data.u.str.area, smp->data.u.str.data, &smp->data.u.ipv6))
			return 0;
		smp->data.type = SMP_T_IPV6;
		smp->flags &= ~SMP_F_CONST;
		return 1;
	}
	smp->data.type = SMP_T_IPV4;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int c_str2ip(struct sample *smp)
{
	if (!buf2ip(smp->data.u.str.area, smp->data.u.str.data, &smp->data.u.ipv4))
		return 0;
	smp->data.type = SMP_T_IPV4;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int c_str2ipv6(struct sample *smp)
{
	if (!buf2ip6(smp->data.u.str.area, smp->data.u.str.data, &smp->data.u.ipv6))
		return 0;
	smp->data.type = SMP_T_IPV6;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

/*
 * The NULL char always enforces the end of string if it is met.
 * Data is never changed, so we can ignore the CONST case
 */
static int c_bin2str(struct sample *smp)
{
	int i;

	for (i = 0; i < smp->data.u.str.data; i++) {
		if (!smp->data.u.str.area[i]) {
			smp->data.u.str.data = i;
			break;
		}
	}
	return 1;
}

static int c_int2str(struct sample *smp)
{
	struct buffer *trash = get_trash_chunk();
	char *pos;

	pos = lltoa_r(smp->data.u.sint, trash->area, trash->size);
	if (!pos)
		return 0;

	trash->size = trash->size - (pos - trash->area);
	trash->area = pos;
	trash->data = strlen(pos);
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

/* This function unconditionally duplicates data and removes the "const" flag.
 * For strings and binary blocks, it also provides a known allocated size with
 * a length that is capped to the size, and ensures a trailing zero is always
 * appended for strings. This is necessary for some operations which may
 * require to extend the length. It returns 0 if it fails, 1 on success.
 */
int smp_dup(struct sample *smp)
{
	struct buffer *trash;

	switch (smp->data.type) {
	case SMP_T_BOOL:
	case SMP_T_SINT:
	case SMP_T_ADDR:
	case SMP_T_IPV4:
	case SMP_T_IPV6:
		/* These type are not const. */
		break;

	case SMP_T_METH:
		if (smp->data.u.meth.meth != HTTP_METH_OTHER)
			break;
		/* Fall through */

	case SMP_T_STR:
		trash = get_trash_chunk();
		trash->data = smp->data.type == SMP_T_STR ?
		    smp->data.u.str.data : smp->data.u.meth.str.data;
		if (trash->data > trash->size - 1)
			trash->data = trash->size - 1;

		memcpy(trash->area, smp->data.type == SMP_T_STR ?
		    smp->data.u.str.area : smp->data.u.meth.str.area,
		    trash->data);
		trash->area[trash->data] = 0;
		smp->data.u.str = *trash;
		break;

	case SMP_T_BIN:
		trash = get_trash_chunk();
		trash->data = smp->data.u.str.data;
		if (trash->data > trash->size)
			trash->data = trash->size;

		memcpy(trash->area, smp->data.u.str.area, trash->data);
		smp->data.u.str = *trash;
		break;

	default:
		/* Other cases are unexpected. */
		return 0;
	}

	/* remove const flag */
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

int c_none(struct sample *smp)
{
	return 1;
}

static int c_str2int(struct sample *smp)
{
	const char *str;
	const char *end;

	if (smp->data.u.str.data == 0)
		return 0;

	str = smp->data.u.str.area;
	end = smp->data.u.str.area + smp->data.u.str.data;

	smp->data.u.sint = read_int64(&str, end);
	smp->data.type = SMP_T_SINT;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int c_str2meth(struct sample *smp)
{
	enum http_meth_t meth;
	int len;

	meth = find_http_meth(smp->data.u.str.area, smp->data.u.str.data);
	if (meth == HTTP_METH_OTHER) {
		len = smp->data.u.str.data;
		smp->data.u.meth.str.area = smp->data.u.str.area;
		smp->data.u.meth.str.data = len;
	}
	else
		smp->flags &= ~SMP_F_CONST;
	smp->data.u.meth.meth = meth;
	smp->data.type = SMP_T_METH;
	return 1;
}

static int c_meth2str(struct sample *smp)
{
	int len;
	enum http_meth_t meth;

	if (smp->data.u.meth.meth == HTTP_METH_OTHER) {
		/* The method is unknown. Copy the original pointer. */
		len = smp->data.u.meth.str.data;
		smp->data.u.str.area = smp->data.u.meth.str.area;
		smp->data.u.str.data = len;
		smp->data.type = SMP_T_STR;
	}
	else if (smp->data.u.meth.meth < HTTP_METH_OTHER) {
		/* The method is known, copy the pointer containing the string. */
		meth = smp->data.u.meth.meth;
		smp->data.u.str.area = http_known_methods[meth].ptr;
		smp->data.u.str.data = http_known_methods[meth].len;
		smp->flags |= SMP_F_CONST;
		smp->data.type = SMP_T_STR;
	}
	else {
		/* Unknown method */
		return 0;
	}
	return 1;
}

static int c_addr2bin(struct sample *smp)
{
	struct buffer *chk = get_trash_chunk();

	if (smp->data.type == SMP_T_IPV4) {
		chk->data = 4;
		memcpy(chk->area, &smp->data.u.ipv4, chk->data);
	}
	else if (smp->data.type == SMP_T_IPV6) {
		chk->data = 16;
		memcpy(chk->area, &smp->data.u.ipv6, chk->data);
	}
	else
		return 0;

	smp->data.u.str = *chk;
	smp->data.type = SMP_T_BIN;
	return 1;
}

static int c_int2bin(struct sample *smp)
{
	struct buffer *chk = get_trash_chunk();

	*(unsigned long long int *) chk->area = my_htonll(smp->data.u.sint);
	chk->data = 8;

	smp->data.u.str = *chk;
	smp->data.type = SMP_T_BIN;
	return 1;
}


/*****************************************************************/
/*      Sample casts matrix:                                     */
/*           sample_casts[from type][to type]                    */
/*           NULL pointer used for impossible sample casts       */
/*****************************************************************/

sample_cast_fct sample_casts[SMP_TYPES][SMP_TYPES] = {
/*            to:  ANY     BOOL       SINT       ADDR        IPV4      IPV6        STR         BIN         METH */
/* from:  ANY */ { c_none, c_none,    c_none,    c_none,     c_none,   c_none,     c_none,     c_none,     c_none,     },
/*       BOOL */ { c_none, c_none,    c_none,    NULL,       NULL,     NULL,       c_int2str,  NULL,       NULL,       },
/*       SINT */ { c_none, c_none,    c_none,    c_int2ip,   c_int2ip, c_int2ipv6, c_int2str,  c_int2bin,  NULL,       },
/*       ADDR */ { c_none, NULL,      NULL,      NULL,       NULL,     NULL,       NULL,       NULL,       NULL,       },
/*       IPV4 */ { c_none, NULL,      c_ip2int,  c_none,     c_none,   c_ip2ipv6,  c_ip2str,   c_addr2bin, NULL,       },
/*       IPV6 */ { c_none, NULL,      NULL,      c_none,     c_ipv62ip,c_none,     c_ipv62str, c_addr2bin, NULL,       },
/*        STR */ { c_none, c_str2int, c_str2int, c_str2addr, c_str2ip, c_str2ipv6, c_none,     c_none,     c_str2meth, },
/*        BIN */ { c_none, NULL,      NULL,      NULL,       NULL,     NULL,       c_bin2str,  c_none,     c_str2meth, },
/*       METH */ { c_none, NULL,      NULL,      NULL,       NULL,     NULL,       c_meth2str, c_meth2str, c_none,     }
};

/*
 * Parse a sample expression configuration:
 *        fetch keyword followed by format conversion keywords.
 * Returns a pointer on allocated sample expression structure.
 * The caller must have set al->ctx.
 */
struct sample_expr *sample_parse_expr(char **str, int *idx, const char *file, int line, char **err_msg, struct arg_list *al)
{
	const char *begw; /* beginning of word */
	const char *endw; /* end of word */
	const char *endt; /* end of term */
	struct sample_expr *expr;
	struct sample_fetch *fetch;
	struct sample_conv *conv;
	unsigned long prev_type;
	char *fkw = NULL;
	char *ckw = NULL;
	int err_arg;

	begw = str[*idx];
	for (endw = begw; *endw && *endw != '(' && *endw != ','; endw++);

	if (endw == begw) {
		memprintf(err_msg, "missing fetch method");
		goto out_error;
	}

	/* keep a copy of the current fetch keyword for error reporting */
	fkw = my_strndup(begw, endw - begw);

	fetch = find_sample_fetch(begw, endw - begw);
	if (!fetch) {
		memprintf(err_msg, "unknown fetch method '%s'", fkw);
		goto out_error;
	}

	endt = endw;
	if (*endt == '(') {
		/* look for the end of this term and skip the opening parenthesis */
		endt = ++endw;
		while (*endt && *endt != ')')
			endt++;
		if (*endt != ')') {
			memprintf(err_msg, "missing closing ')' after arguments to fetch keyword '%s'", fkw);
			goto out_error;
		}
	}

	/* At this point, we have :
	 *   - begw : beginning of the keyword
	 *   - endw : end of the keyword, first character not part of keyword
	 *            nor the opening parenthesis (so first character of args
	 *            if present).
	 *   - endt : end of the term (=endw or last parenthesis if args are present)
	 */

	if (fetch->out_type >= SMP_TYPES) {
		memprintf(err_msg, "returns type of fetch method '%s' is unknown", fkw);
		goto out_error;
	}
	prev_type = fetch->out_type;

	expr = calloc(1, sizeof(*expr));
	if (!expr)
		goto out_error;

	LIST_INIT(&(expr->conv_exprs));
	expr->fetch = fetch;
	expr->arg_p = empty_arg_list;

	/* Note that we call the argument parser even with an empty string,
	 * this allows it to automatically create entries for mandatory
	 * implicit arguments (eg: local proxy name).
	 */
	al->kw = expr->fetch->kw;
	al->conv = NULL;
	if (make_arg_list(endw, endt - endw, fetch->arg_mask, &expr->arg_p, err_msg, NULL, &err_arg, al) < 0) {
		memprintf(err_msg, "fetch method '%s' : %s", fkw, *err_msg);
		goto out_error;
	}

	if (!expr->arg_p) {
		expr->arg_p = empty_arg_list;
	}
	else if (fetch->val_args && !fetch->val_args(expr->arg_p, err_msg)) {
		memprintf(err_msg, "invalid args in fetch method '%s' : %s", fkw, *err_msg);
		goto out_error;
	}

	/* Now process the converters if any. We have two supported syntaxes
	 * for the converters, which can be combined :
	 *  - comma-delimited list of converters just after the keyword and args ;
	 *  - one converter per keyword
	 * The combination allows to have each keyword being a comma-delimited
	 * series of converters.
	 *
	 * We want to process the former first, then the latter. For this we start
	 * from the beginning of the supposed place in the exiting conv chain, which
	 * starts at the last comma (endt).
	 */

	while (1) {
		struct sample_conv_expr *conv_expr;

		if (*endt == ')') /* skip last closing parenthesis */
			endt++;

		if (*endt && *endt != ',') {
			if (ckw)
				memprintf(err_msg, "missing comma after converter '%s'", ckw);
			else
				memprintf(err_msg, "missing comma after fetch keyword '%s'", fkw);
			goto out_error;
		}

		while (*endt == ',') /* then trailing commas */
			endt++;

		begw = endt; /* start of converter */

		if (!*begw) {
			/* none ? skip to next string */
			(*idx)++;
			begw = str[*idx];
			if (!begw || !*begw)
				break;
		}

		for (endw = begw; *endw && *endw != '(' && *endw != ','; endw++);

		free(ckw);
		ckw = my_strndup(begw, endw - begw);

		conv = find_sample_conv(begw, endw - begw);
		if (!conv) {
			/* we found an isolated keyword that we don't know, it's not ours */
			if (begw == str[*idx])
				break;
			memprintf(err_msg, "unknown converter '%s'", ckw);
			goto out_error;
		}

		endt = endw;
		if (*endt == '(') {
			/* look for the end of this term */
			while (*endt && *endt != ')')
				endt++;
			if (*endt != ')') {
				memprintf(err_msg, "syntax error: missing ')' after converter '%s'", ckw);
				goto out_error;
			}
		}

		if (conv->in_type >= SMP_TYPES || conv->out_type >= SMP_TYPES) {
			memprintf(err_msg, "returns type of converter '%s' is unknown", ckw);
			goto out_error;
		}

		/* If impossible type conversion */
		if (!sample_casts[prev_type][conv->in_type]) {
			memprintf(err_msg, "converter '%s' cannot be applied", ckw);
			goto out_error;
		}

		prev_type = conv->out_type;
		conv_expr = calloc(1, sizeof(*conv_expr));
		if (!conv_expr)
			goto out_error;

		LIST_ADDQ(&(expr->conv_exprs), &(conv_expr->list));
		conv_expr->conv = conv;

		if (endt != endw) {
			int err_arg;

			if (!conv->arg_mask) {
				memprintf(err_msg, "converter '%s' does not support any args", ckw);
				goto out_error;
			}

			al->kw = expr->fetch->kw;
			al->conv = conv_expr->conv->kw;
			if (make_arg_list(endw + 1, endt - endw - 1, conv->arg_mask, &conv_expr->arg_p, err_msg, NULL, &err_arg, al) < 0) {
				memprintf(err_msg, "invalid arg %d in converter '%s' : %s", err_arg+1, ckw, *err_msg);
				goto out_error;
			}

			if (!conv_expr->arg_p)
				conv_expr->arg_p = empty_arg_list;

			if (conv->val_args && !conv->val_args(conv_expr->arg_p, conv, file, line, err_msg)) {
				memprintf(err_msg, "invalid args in converter '%s' : %s", ckw, *err_msg);
				goto out_error;
			}
		}
		else if (ARGM(conv->arg_mask)) {
			memprintf(err_msg, "missing args for converter '%s'", ckw);
			goto out_error;
		}
	}

 out:
	free(fkw);
	free(ckw);
	return expr;

out_error:
	/* TODO: prune_sample_expr(expr); */
	expr = NULL;
	goto out;
}

/*
 * Process a fetch + format conversion of defined by the sample expression <expr>
 * on request or response considering the <opt> parameter.
 * Returns a pointer on a typed sample structure containing the result or NULL if
 * sample is not found or when format conversion failed.
 *  If <p> is not null, function returns results in structure pointed by <p>.
 *  If <p> is null, functions returns a pointer on a static sample structure.
 *
 * Note: the fetch functions are required to properly set the return type. The
 * conversion functions must do so too. However the cast functions do not need
 * to since they're made to cast mutiple types according to what is required.
 *
 * The caller may indicate in <opt> if it considers the result final or not.
 * The caller needs to check the SMP_F_MAY_CHANGE flag in p->flags to verify
 * if the result is stable or not, according to the following table :
 *
 * return MAY_CHANGE FINAL   Meaning for the sample
 *  NULL      0        *     Not present and will never be (eg: header)
 *  NULL      1        0     Not present yet, could change (eg: POST param)
 *  NULL      1        1     Not present yet, will not change anymore
 *   smp      0        *     Present and will not change (eg: header)
 *   smp      1        0     Present, may change (eg: request length)
 *   smp      1        1     Present, last known value (eg: request length)
 */
struct sample *sample_process(struct proxy *px, struct session *sess,
                              struct stream *strm, unsigned int opt,
                              struct sample_expr *expr, struct sample *p)
{
	struct sample_conv_expr *conv_expr;

	if (p == NULL) {
		p = &temp_smp;
		memset(p, 0, sizeof(*p));
	}

	smp_set_owner(p, px, sess, strm, opt);
	if (!expr->fetch->process(expr->arg_p, p, expr->fetch->kw, expr->fetch->private))
		return NULL;

	list_for_each_entry(conv_expr, &expr->conv_exprs, list) {
		/* we want to ensure that p->type can be casted into
		 * conv_expr->conv->in_type. We have 3 possibilities :
		 *  - NULL   => not castable.
		 *  - c_none => nothing to do (let's optimize it)
		 *  - other  => apply cast and prepare to fail
		 */
		if (!sample_casts[p->data.type][conv_expr->conv->in_type])
			return NULL;

		if (sample_casts[p->data.type][conv_expr->conv->in_type] != c_none &&
		    !sample_casts[p->data.type][conv_expr->conv->in_type](p))
			return NULL;

		/* OK cast succeeded */

		if (!conv_expr->conv->process(conv_expr->arg_p, p, conv_expr->conv->private))
			return NULL;
	}
	return p;
}

/*
 * Resolve all remaining arguments in proxy <p>. Returns the number of
 * errors or 0 if everything is fine.
 */
int smp_resolve_args(struct proxy *p)
{
	struct arg_list *cur, *bak;
	const char *ctx, *where;
	const char *conv_ctx, *conv_pre, *conv_pos;
	struct userlist *ul;
	struct my_regex *reg;
	struct arg *arg;
	int cfgerr = 0;
	int rflags;

	list_for_each_entry_safe(cur, bak, &p->conf.args.list, list) {
		struct proxy *px;
		struct server *srv;
		struct stktable *t;
		char *pname, *sname, *stktname;
		char *err;

		arg = cur->arg;

		/* prepare output messages */
		conv_pre = conv_pos = conv_ctx = "";
		if (cur->conv) {
			conv_ctx = cur->conv;
			conv_pre = "conversion keyword '";
			conv_pos = "' for ";
		}

		where = "in";
		ctx = "sample fetch keyword";
		switch (cur->ctx) {
		case ARGC_STK:   where = "in stick rule in"; break;
		case ARGC_TRK:   where = "in tracking rule in"; break;
		case ARGC_LOG:   where = "in log-format string in"; break;
		case ARGC_LOGSD: where = "in log-format-sd string in"; break;
		case ARGC_HRQ:   where = "in http-request header format string in"; break;
		case ARGC_HRS:   where = "in http-response header format string in"; break;
		case ARGC_UIF:   where = "in unique-id-format string in"; break;
		case ARGC_RDR:   where = "in redirect format string in"; break;
		case ARGC_CAP:   where = "in capture rule in"; break;
		case ARGC_ACL:   ctx = "ACL keyword"; break;
		case ARGC_SRV:   where = "in server directive in"; break;
		case ARGC_SPOE:  where = "in spoe-message directive in"; break;
		}

		/* set a few default settings */
		px = p;
		pname = p->id;

		switch (arg->type) {
		case ARGT_SRV:
			if (!arg->data.str.data) {
				ha_alert("parsing [%s:%d] : missing server name in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				continue;
			}

			/* we support two formats : "bck/srv" and "srv" */
			sname = strrchr(arg->data.str.area, '/');

			if (sname) {
				*sname++ = '\0';
				pname = arg->data.str.area;

				px = proxy_be_by_name(pname);
				if (!px) {
					ha_alert("parsing [%s:%d] : unable to find proxy '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
						 cur->file, cur->line, pname,
						 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
					cfgerr++;
					break;
				}
			}
			else
				sname = arg->data.str.area;

			srv = findserver(px, sname);
			if (!srv) {
				ha_alert("parsing [%s:%d] : unable to find server '%s' in proxy '%s', referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line, sname, pname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.srv = srv;
			break;

		case ARGT_FE:
			if (arg->data.str.data) {
				pname = arg->data.str.area;
				px = proxy_fe_by_name(pname);
			}

			if (!px) {
				ha_alert("parsing [%s:%d] : unable to find frontend '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line, pname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			if (!(px->cap & PR_CAP_FE)) {
				ha_alert("parsing [%s:%d] : proxy '%s', referenced in arg %d of %s%s%s%s '%s' %s proxy '%s', has not frontend capability.\n",
					 cur->file, cur->line, pname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.prx = px;
			break;

		case ARGT_BE:
			if (arg->data.str.data) {
				pname = arg->data.str.area;
				px = proxy_be_by_name(pname);
			}

			if (!px) {
				ha_alert("parsing [%s:%d] : unable to find backend '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line, pname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			if (!(px->cap & PR_CAP_BE)) {
				ha_alert("parsing [%s:%d] : proxy '%s', referenced in arg %d of %s%s%s%s '%s' %s proxy '%s', has not backend capability.\n",
					 cur->file, cur->line, pname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.prx = px;
			break;

		case ARGT_TAB:
			if (arg->data.str.data)
				stktname = arg->data.str.area;
			else
				stktname = px->id;

			t = stktable_find_by_name(stktname);
			if (!t) {
				ha_alert("parsing [%s:%d] : unable to find table '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line, stktname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			if (!t->size) {
				ha_alert("parsing [%s:%d] : no table in proxy '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line, stktname,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			if (t->proxy && (p->bind_proc & ~t->proxy->bind_proc)) {
				ha_alert("parsing [%s:%d] : stick-table '%s' not present on all processes covered by proxy '%s'.\n",
					 cur->file, cur->line, t->proxy->id, p->id);
				cfgerr++;
				break;
			}

			if (t->proxies_list != p) {
				p->next_stkt_ref = t->proxies_list;
				t->proxies_list = p;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.t = t;
			break;

		case ARGT_USR:
			if (!arg->data.str.data) {
				ha_alert("parsing [%s:%d] : missing userlist name in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			if (p->uri_auth && p->uri_auth->userlist &&
			    !strcmp(p->uri_auth->userlist->name, arg->data.str.area))
				ul = p->uri_auth->userlist;
			else
				ul = auth_find_userlist(arg->data.str.area);

			if (!ul) {
				ha_alert("parsing [%s:%d] : unable to find userlist '%s' referenced in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line,
					 arg->data.str.area,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				break;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.usr = ul;
			break;

		case ARGT_REG:
			if (!arg->data.str.data) {
				ha_alert("parsing [%s:%d] : missing regex in arg %d of %s%s%s%s '%s' %s proxy '%s'.\n",
					 cur->file, cur->line,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id);
				cfgerr++;
				continue;
			}

			rflags = 0;
			rflags |= (arg->type_flags & ARGF_REG_ICASE) ? REG_ICASE : 0;
			err = NULL;

			if (!(reg = regex_comp(arg->data.str.area, !(rflags & REG_ICASE), 1 /* capture substr */, &err))) {
				ha_alert("parsing [%s:%d] : error in regex '%s' in arg %d of %s%s%s%s '%s' %s proxy '%s' : %s.\n",
					 cur->file, cur->line,
					 arg->data.str.area,
					 cur->arg_pos + 1, conv_pre, conv_ctx, conv_pos, ctx, cur->kw, where, p->id, err);
				cfgerr++;
				continue;
			}

			free(arg->data.str.area);
			arg->data.str.area = NULL;
			arg->unresolved = 0;
			arg->data.reg = reg;
			break;


		}

		LIST_DEL(&cur->list);
		free(cur);
	} /* end of args processing */

	return cfgerr;
}

/*
 * Process a fetch + format conversion as defined by the sample expression
 * <expr> on request or response considering the <opt> parameter. The output is
 * not explicitly set to <smp_type>, but shall be compatible with it as
 * specified by 'sample_casts' table. If a stable sample can be fetched, or an
 * unstable one when <opt> contains SMP_OPT_FINAL, the sample is converted and
 * returned without the SMP_F_MAY_CHANGE flag. If an unstable sample is found
 * and <opt> does not contain SMP_OPT_FINAL, then the sample is returned as-is
 * with its SMP_F_MAY_CHANGE flag so that the caller can check it and decide to
 * take actions (eg: wait longer). If a sample could not be found or could not
 * be converted, NULL is returned. The caller MUST NOT use the sample if the
 * SMP_F_MAY_CHANGE flag is present, as it is used only as a hint that there is
 * still hope to get it after waiting longer, and is not converted to string.
 * The possible output combinations are the following :
 *
 * return MAY_CHANGE FINAL   Meaning for the sample
 *  NULL      *        *     Not present and will never be (eg: header)
 *   smp      0        *     Final value converted (eg: header)
 *   smp      1        0     Not present yet, may appear later (eg: header)
 *   smp      1        1     never happens (either flag is cleared on output)
 */
struct sample *sample_fetch_as_type(struct proxy *px, struct session *sess,
                                   struct stream *strm, unsigned int opt,
                                   struct sample_expr *expr, int smp_type)
{
	struct sample *smp = &temp_smp;

	memset(smp, 0, sizeof(*smp));

	if (!sample_process(px, sess, strm, opt, expr, smp)) {
		if ((smp->flags & SMP_F_MAY_CHANGE) && !(opt & SMP_OPT_FINAL))
			return smp;
		return NULL;
	}

	if (!sample_casts[smp->data.type][smp_type])
		return NULL;

	if (!sample_casts[smp->data.type][smp_type](smp))
		return NULL;

	smp->flags &= ~SMP_F_MAY_CHANGE;
	return smp;
}

static void release_sample_arg(struct arg *p)
{
	struct arg *p_back = p;

	if (!p)
		return;

	while (p->type != ARGT_STOP) {
		if (p->type == ARGT_STR || p->unresolved) {
			free(p->data.str.area);
			p->data.str.area = NULL;
			p->unresolved = 0;
		}
		else if (p->type == ARGT_REG) {
			regex_free(p->data.reg);
			p->data.reg = NULL;
		}
		p++;
	}

	if (p_back != empty_arg_list)
		free(p_back);
}

void release_sample_expr(struct sample_expr *expr)
{
	struct sample_conv_expr *conv_expr, *conv_exprb;

	if (!expr)
		return;

	list_for_each_entry_safe(conv_expr, conv_exprb, &expr->conv_exprs, list)
		release_sample_arg(conv_expr->arg_p);
	release_sample_arg(expr->arg_p);
	free(expr);
}

/*****************************************************************/
/*    Sample format convert functions                            */
/*    These functions set the data type on return.               */
/*****************************************************************/

#ifdef DEBUG_EXPR
static int sample_conv_debug(const struct arg *arg_p, struct sample *smp, void *private)
{
	int i;
	struct sample tmp;

	if (!(global.mode & MODE_QUIET) || (global.mode & (MODE_VERBOSE | MODE_STARTING))) {
		fprintf(stderr, "[debug converter] type: %s ", smp_to_type[smp->data.type]);
		if (!sample_casts[smp->data.type][SMP_T_STR]) {
			fprintf(stderr, "(undisplayable)");
		} else {

			/* Copy sample fetch. This put the sample as const, the
			 * cast will copy data if a transformation is required.
			 */
			memcpy(&tmp, smp, sizeof(struct sample));
			tmp.flags = SMP_F_CONST;

			if (!sample_casts[smp->data.type][SMP_T_STR](&tmp))
				fprintf(stderr, "(undisplayable)");

			else {
				/* Display the displayable chars*. */
				fprintf(stderr, "<");
				for (i = 0; i < tmp.data.u.str.data; i++) {
					if (isprint(tmp.data.u.str.area[i]))
						fputc(tmp.data.u.str.area[i],
						      stderr);
					else
						fputc('.', stderr);
				}
			}
			fprintf(stderr, ">\n");
		}
	}
	return 1;
}
#endif

static int sample_conv_base642bin(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct buffer *trash = get_trash_chunk();
	int bin_len;

	trash->data = 0;
	bin_len = base64dec(smp->data.u.str.area, smp->data.u.str.data,
			    trash->area, trash->size);
	if (bin_len < 0)
		return 0;

	trash->data = bin_len;
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_BIN;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int sample_conv_bin2base64(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct buffer *trash = get_trash_chunk();
	int b64_len;

	trash->data = 0;
	b64_len = a2base64(smp->data.u.str.area, smp->data.u.str.data,
			   trash->area, trash->size);
	if (b64_len < 0)
		return 0;

	trash->data = b64_len;
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int sample_conv_sha1(const struct arg *arg_p, struct sample *smp, void *private)
{
	blk_SHA_CTX ctx;
	struct buffer *trash = get_trash_chunk();

	memset(&ctx, 0, sizeof(ctx));

	blk_SHA1_Init(&ctx);
	blk_SHA1_Update(&ctx, smp->data.u.str.area, smp->data.u.str.data);
	blk_SHA1_Final((unsigned char *) trash->area, &ctx);

	trash->data = 20;
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_BIN;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int sample_conv_bin2hex(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct buffer *trash = get_trash_chunk();
	unsigned char c;
	int ptr = 0;

	trash->data = 0;
	while (ptr < smp->data.u.str.data && trash->data <= trash->size - 2) {
		c = smp->data.u.str.area[ptr++];
		trash->area[trash->data++] = hextab[(c >> 4) & 0xF];
		trash->area[trash->data++] = hextab[c & 0xF];
	}
	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

static int sample_conv_hex2int(const struct arg *arg_p, struct sample *smp, void *private)
{
	long long int n = 0;
	int i, c;

	for (i = 0; i < smp->data.u.str.data; i++) {
		if ((c = hex2i(smp->data.u.str.area[i])) < 0)
			return 0;
		n = (n << 4) + c;
	}

	smp->data.u.sint = n;
	smp->data.type = SMP_T_SINT;
	smp->flags &= ~SMP_F_CONST;
	return 1;
}

/* hashes the binary input into a 32-bit unsigned int */
static int sample_conv_djb2(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = hash_djb2(smp->data.u.str.area,
				     smp->data.u.str.data);
	if (arg_p && arg_p->data.sint)
		smp->data.u.sint = full_hash(smp->data.u.sint);
	smp->data.type = SMP_T_SINT;
	return 1;
}

static int sample_conv_length(const struct arg *arg_p, struct sample *smp, void *private)
{
	int i = smp->data.u.str.data;
	smp->data.u.sint = i;
	smp->data.type = SMP_T_SINT;
	return 1;
}


static int sample_conv_str2lower(const struct arg *arg_p, struct sample *smp, void *private)
{
	int i;

	if (!smp_make_rw(smp))
		return 0;

	for (i = 0; i < smp->data.u.str.data; i++) {
		if ((smp->data.u.str.area[i] >= 'A') && (smp->data.u.str.area[i] <= 'Z'))
			smp->data.u.str.area[i] += 'a' - 'A';
	}
	return 1;
}

static int sample_conv_str2upper(const struct arg *arg_p, struct sample *smp, void *private)
{
	int i;

	if (!smp_make_rw(smp))
		return 0;

	for (i = 0; i < smp->data.u.str.data; i++) {
		if ((smp->data.u.str.area[i] >= 'a') && (smp->data.u.str.area[i] <= 'z'))
			smp->data.u.str.area[i] += 'A' - 'a';
	}
	return 1;
}

/* takes the IPv4 mask in args[0] and an optional IPv6 mask in args[1] */
static int sample_conv_ipmask(const struct arg *args, struct sample *smp, void *private)
{
	/* Attempt to convert to IPv4 to apply the correct mask. */
	c_ipv62ip(smp);

	if (smp->data.type == SMP_T_IPV4) {
		smp->data.u.ipv4.s_addr &= args[0].data.ipv4.s_addr;
		smp->data.type = SMP_T_IPV4;
	}
	else if (smp->data.type == SMP_T_IPV6) {
		/* IPv6 cannot be converted without an IPv6 mask. */
		if (args[1].type != ARGT_IPV6)
			return 0;

		*(uint32_t*)&smp->data.u.ipv6.s6_addr[0]  &= *(uint32_t*)&args[1].data.ipv6.s6_addr[0];
		*(uint32_t*)&smp->data.u.ipv6.s6_addr[4]  &= *(uint32_t*)&args[1].data.ipv6.s6_addr[4];
		*(uint32_t*)&smp->data.u.ipv6.s6_addr[8]  &= *(uint32_t*)&args[1].data.ipv6.s6_addr[8];
		*(uint32_t*)&smp->data.u.ipv6.s6_addr[12] &= *(uint32_t*)&args[1].data.ipv6.s6_addr[12];
		smp->data.type = SMP_T_IPV6;
	}

	return 1;
}

/* takes an UINT value on input supposed to represent the time since EPOCH,
 * adds an optional offset found in args[1] and emits a string representing
 * the local time in the format specified in args[1] using strftime().
 */
static int sample_conv_ltime(const struct arg *args, struct sample *smp, void *private)
{
	struct buffer *temp;
	/* With high numbers, the date returned can be negative, the 55 bits mask prevent this. */
	time_t curr_date = smp->data.u.sint & 0x007fffffffffffffLL;
	struct tm *tm;

	/* add offset */
	if (args[1].type == ARGT_SINT)
		curr_date += args[1].data.sint;

	tm = localtime(&curr_date);
	if (!tm)
		return 0;
	temp = get_trash_chunk();
	temp->data = strftime(temp->area, temp->size, args[0].data.str.area,
			      tm);
	smp->data.u.str = *temp;
	smp->data.type = SMP_T_STR;
	return 1;
}

/* hashes the binary input into a 32-bit unsigned int */
static int sample_conv_sdbm(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = hash_sdbm(smp->data.u.str.area,
				     smp->data.u.str.data);
	if (arg_p && arg_p->data.sint)
		smp->data.u.sint = full_hash(smp->data.u.sint);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* takes an UINT value on input supposed to represent the time since EPOCH,
 * adds an optional offset found in args[1] and emits a string representing
 * the UTC date in the format specified in args[1] using strftime().
 */
static int sample_conv_utime(const struct arg *args, struct sample *smp, void *private)
{
	struct buffer *temp;
	/* With high numbers, the date returned can be negative, the 55 bits mask prevent this. */
	time_t curr_date = smp->data.u.sint & 0x007fffffffffffffLL;
	struct tm *tm;

	/* add offset */
	if (args[1].type == ARGT_SINT)
		curr_date += args[1].data.sint;

	tm = gmtime(&curr_date);
	if (!tm)
		return 0;
	temp = get_trash_chunk();
	temp->data = strftime(temp->area, temp->size, args[0].data.str.area,
			      tm);
	smp->data.u.str = *temp;
	smp->data.type = SMP_T_STR;
	return 1;
}

/* hashes the binary input into a 32-bit unsigned int */
static int sample_conv_wt6(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = hash_wt6(smp->data.u.str.area,
				    smp->data.u.str.data);
	if (arg_p && arg_p->data.sint)
		smp->data.u.sint = full_hash(smp->data.u.sint);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* hashes the binary input into a 32-bit unsigned int using xxh.
 * The seed of the hash defaults to 0 but can be changd in argument 1.
 */
static int sample_conv_xxh32(const struct arg *arg_p, struct sample *smp, void *private)
{
	unsigned int seed;

	if (arg_p && arg_p->data.sint)
		seed = arg_p->data.sint;
	else
		seed = 0;
	smp->data.u.sint = XXH32(smp->data.u.str.area, smp->data.u.str.data,
				 seed);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* hashes the binary input into a 64-bit unsigned int using xxh.
 * In fact, the function returns a 64 bit unsigned, but the sample
 * storage of haproxy only proposes 64-bits signed, so the value is
 * cast as signed. This cast doesn't impact the hash repartition.
 * The seed of the hash defaults to 0 but can be changd in argument 1.
 */
static int sample_conv_xxh64(const struct arg *arg_p, struct sample *smp, void *private)
{
	unsigned long long int seed;

	if (arg_p && arg_p->data.sint)
		seed = (unsigned long long int)arg_p->data.sint;
	else
		seed = 0;
	smp->data.u.sint = (long long int)XXH64(smp->data.u.str.area,
						smp->data.u.str.data, seed);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* hashes the binary input into a 32-bit unsigned int */
static int sample_conv_crc32(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = hash_crc32(smp->data.u.str.area,
				      smp->data.u.str.data);
	if (arg_p && arg_p->data.sint)
		smp->data.u.sint = full_hash(smp->data.u.sint);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* hashes the binary input into crc32c (RFC4960, Appendix B [8].) */
static int sample_conv_crc32c(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = hash_crc32c(smp->data.u.str.area,
				       smp->data.u.str.data);
	if (arg_p && arg_p->data.sint)
		smp->data.u.sint = full_hash(smp->data.u.sint);
	smp->data.type = SMP_T_SINT;
	return 1;
}

/* This function escape special json characters. The returned string can be
 * safely set between two '"' and used as json string. The json string is
 * defined like this:
 *
 *    any Unicode character except '"' or '\' or control character
 *    \", \\, \/, \b, \f, \n, \r, \t, \u + four-hex-digits
 *
 * The enum input_type contain all the allowed mode for decoding the input
 * string.
 */
enum input_type {
	IT_ASCII = 0,
	IT_UTF8,
	IT_UTF8S,
	IT_UTF8P,
	IT_UTF8PS,
};
static int sample_conv_json_check(struct arg *arg, struct sample_conv *conv,
                                  const char *file, int line, char **err)
{
	if (!arg) {
		memprintf(err, "Unexpected empty arg list");
		return 0;
	}

	if (arg->type != ARGT_STR) {
		memprintf(err, "Unexpected arg type");
		return 0;
	}

	if (strcmp(arg->data.str.area, "") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_ASCII;
		return 1;
	}

	else if (strcmp(arg->data.str.area, "ascii") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_ASCII;
		return 1;
	}

	else if (strcmp(arg->data.str.area, "utf8") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_UTF8;
		return 1;
	}

	else if (strcmp(arg->data.str.area, "utf8s") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_UTF8S;
		return 1;
	}

	else if (strcmp(arg->data.str.area, "utf8p") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_UTF8P;
		return 1;
	}

	else if (strcmp(arg->data.str.area, "utf8ps") == 0) {
		arg->type = ARGT_SINT;
		arg->data.sint = IT_UTF8PS;
		return 1;
	}

	memprintf(err, "Unexpected input code type. "
	               "Allowed value are 'ascii', 'utf8', 'utf8s', 'utf8p' and 'utf8ps'");
	return 0;
}

static int sample_conv_json(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct buffer *temp;
	char _str[7]; /* \u + 4 hex digit + null char for sprintf. */
	const char *str;
	int len;
	enum input_type input_type = IT_ASCII;
	unsigned int c;
	unsigned int ret;
	char *p;

	if (arg_p)
		input_type = arg_p->data.sint;

	temp = get_trash_chunk();
	temp->data = 0;

	p = smp->data.u.str.area;
	while (p < smp->data.u.str.area + smp->data.u.str.data) {

		if (input_type == IT_ASCII) {
			/* Read input as ASCII. */
			c = *(unsigned char *)p;
			p++;
		}
		else {
			/* Read input as UTF8. */
			ret = utf8_next(p,
					smp->data.u.str.data - ( p - smp->data.u.str.area),
					&c);
			p += utf8_return_length(ret);

			if (input_type == IT_UTF8 && utf8_return_code(ret) != UTF8_CODE_OK)
					return 0;
			if (input_type == IT_UTF8S && utf8_return_code(ret) != UTF8_CODE_OK)
					continue;
			if (input_type == IT_UTF8P && utf8_return_code(ret) & (UTF8_CODE_INVRANGE|UTF8_CODE_BADSEQ))
					return 0;
			if (input_type == IT_UTF8PS && utf8_return_code(ret) & (UTF8_CODE_INVRANGE|UTF8_CODE_BADSEQ))
					continue;

			/* Check too big values. */
			if ((unsigned int)c > 0xffff) {
				if (input_type == IT_UTF8 || input_type == IT_UTF8P)
					return 0;
				continue;
			}
		}

		/* Convert character. */
		if (c == '"') {
			len = 2;
			str = "\\\"";
		}
		else if (c == '\\') {
			len = 2;
			str = "\\\\";
		}
		else if (c == '/') {
			len = 2;
			str = "\\/";
		}
		else if (c == '\b') {
			len = 2;
			str = "\\b";
		}
		else if (c == '\f') {
			len = 2;
			str = "\\f";
		}
		else if (c == '\r') {
			len = 2;
			str = "\\r";
		}
		else if (c == '\n') {
			len = 2;
			str = "\\n";
		}
		else if (c == '\t') {
			len = 2;
			str = "\\t";
		}
		else if (c > 0xff || !isprint(c)) {
			/* isprint generate a segfault if c is too big. The man says that
			 * c must have the value of an unsigned char or EOF.
			 */
			len = 6;
			_str[0] = '\\';
			_str[1] = 'u';
			snprintf(&_str[2], 5, "%04x", (unsigned short)c);
			str = _str;
		}
		else {
			len = 1;
			str = (char *)&c;
		}

		/* Check length */
		if (temp->data + len > temp->size)
			return 0;

		/* Copy string. */
		memcpy(temp->area + temp->data, str, len);
		temp->data += len;
	}

	smp->flags &= ~SMP_F_CONST;
	smp->data.u.str = *temp;
	smp->data.type = SMP_T_STR;

	return 1;
}

/* This sample function is designed to extract some bytes from an input buffer.
 * First arg is the offset.
 * Optional second arg is the length to truncate */
static int sample_conv_bytes(const struct arg *arg_p, struct sample *smp, void *private)
{
	if (smp->data.u.str.data <= arg_p[0].data.sint) {
		smp->data.u.str.data = 0;
		return 1;
	}

	if (smp->data.u.str.size)
			smp->data.u.str.size -= arg_p[0].data.sint;
	smp->data.u.str.data -= arg_p[0].data.sint;
	smp->data.u.str.area += arg_p[0].data.sint;

	if ((arg_p[1].type == ARGT_SINT) && (arg_p[1].data.sint < smp->data.u.str.data))
		smp->data.u.str.data = arg_p[1].data.sint;

	return 1;
}

static int sample_conv_field_check(struct arg *args, struct sample_conv *conv,
                                  const char *file, int line, char **err)
{
	struct arg *arg = args;

	if (!arg) {
		memprintf(err, "Unexpected empty arg list");
		return 0;
	}

	if (arg->type != ARGT_SINT) {
		memprintf(err, "Unexpected arg type");
		return 0;
	}

	if (!arg->data.sint) {
		memprintf(err, "Unexpected value 0 for index");
		return 0;
	}

	arg++;

	if (arg->type != ARGT_STR) {
		memprintf(err, "Unexpected arg type");
		return 0;
	}

	if (!arg->data.str.data) {
		memprintf(err, "Empty separators list");
		return 0;
	}

	return 1;
}

/* This sample function is designed to a return selected part of a string (field).
 * First arg is the index of the field (start at 1)
 * Second arg is a char list of separators (type string)
 */
static int sample_conv_field(const struct arg *arg_p, struct sample *smp, void *private)
{
	int field;
	char *start, *end;
	int i;
	int count = (arg_p[2].type == ARGT_SINT) ? arg_p[2].data.sint : 1;

	if (!arg_p[0].data.sint)
		return 0;

	if (arg_p[0].data.sint < 0) {
		field = -1;
		end = start = smp->data.u.str.area + smp->data.u.str.data;
		while (start > smp->data.u.str.area) {
			for (i = 0 ; i < arg_p[1].data.str.data; i++) {
				if (*(start-1) == arg_p[1].data.str.area[i]) {
					if (field == arg_p[0].data.sint) {
						if (count == 1)
							goto found;
						else if (count > 1)
							count--;
					} else {
						end = start-1;
						field--;
					}
					break;
				}
			}
			start--;
		}
	} else {
		field = 1;
		end = start = smp->data.u.str.area;
		while (end - smp->data.u.str.area < smp->data.u.str.data) {
			for (i = 0 ; i < arg_p[1].data.str.data; i++) {
				if (*end == arg_p[1].data.str.area[i]) {
					if (field == arg_p[0].data.sint) {
						if (count == 1)
							goto found;
						else if (count > 1)
							count--;
					} else {
						start = end+1;
						field++;
					}
					break;
				}
			}
			end++;
		}
	}

	/* Field not found */
	if (field != arg_p[0].data.sint) {
		smp->data.u.str.data = 0;
		return 1;
	}
found:
	smp->data.u.str.data = end - start;
	/* If ret string is len 0, no need to
           change pointers or to update size */
	if (!smp->data.u.str.data)
		return 1;

	smp->data.u.str.area = start;

	/* Compute remaining size if needed
           Note: smp->data.u.str.size cannot be set to 0 */
	if (smp->data.u.str.size)
		smp->data.u.str.size -= start - smp->data.u.str.area;

	return 1;
}

/* This sample function is designed to return a word from a string.
 * First arg is the index of the word (start at 1)
 * Second arg is a char list of words separators (type string)
 */
static int sample_conv_word(const struct arg *arg_p, struct sample *smp, void *private)
{
	int word;
	char *start, *end;
	int i, issep, inword;
	int count = (arg_p[2].type == ARGT_SINT) ? arg_p[2].data.sint : 1;

	if (!arg_p[0].data.sint)
		return 0;

	word = 0;
	inword = 0;
	if (arg_p[0].data.sint < 0) {
		end = start = smp->data.u.str.area + smp->data.u.str.data;
		while (start > smp->data.u.str.area) {
			issep = 0;
			for (i = 0 ; i < arg_p[1].data.str.data; i++) {
				if (*(start-1) == arg_p[1].data.str.area[i]) {
					issep = 1;
					break;
				}
			}
			if (!inword) {
				if (!issep) {
					if (word != arg_p[0].data.sint) {
						word--;
						end = start;
					}
					inword = 1;
				}
			}
			else if (issep) {
				if (word == arg_p[0].data.sint) {
					if (count == 1)
						goto found;
					else if (count > 1)
						count--;
				}
				inword = 0;
			}
			start--;
		}
	} else {
		end = start = smp->data.u.str.area;
		while (end - smp->data.u.str.area < smp->data.u.str.data) {
			issep = 0;
			for (i = 0 ; i < arg_p[1].data.str.data; i++) {
				if (*end == arg_p[1].data.str.area[i]) {
					issep = 1;
					break;
				}
			}
			if (!inword) {
				if (!issep) {
					if (word != arg_p[0].data.sint) {
						word++;
						start = end;
					}
					inword = 1;
				}
			}
			else if (issep) {
				if (word == arg_p[0].data.sint) {
					if (count == 1)
						goto found;
					else if (count > 1)
						count--;
				}
				inword = 0;
			}
			end++;
		}
	}

	/* Field not found */
	if (word != arg_p[0].data.sint) {
		smp->data.u.str.data = 0;
		return 1;
	}
found:
	smp->data.u.str.data = end - start;
	/* If ret string is len 0, no need to
           change pointers or to update size */
	if (!smp->data.u.str.data)
		return 1;

	smp->data.u.str.area = start;

	/* Compute remaining size if needed
           Note: smp->data.u.str.size cannot be set to 0 */
	if (smp->data.u.str.size)
		smp->data.u.str.size -= start - smp->data.u.str.area;

	return 1;
}

static int sample_conv_regsub_check(struct arg *args, struct sample_conv *conv,
                                    const char *file, int line, char **err)
{
	struct arg *arg = args;
	char *p;
	int len;

	/* arg0 is a regex, it uses type_flag for ICASE and global match */
	arg[0].type_flags = 0;

	if (arg[2].type != ARGT_STR)
		return 1;

	p = arg[2].data.str.area;
	len = arg[2].data.str.data;
	while (len) {
		if (*p == 'i') {
			arg[0].type_flags |= ARGF_REG_ICASE;
		}
		else if (*p == 'g') {
			arg[0].type_flags |= ARGF_REG_GLOB;
		}
		else {
			memprintf(err, "invalid regex flag '%c', only 'i' and 'g' are supported", *p);
			return 0;
		}
		p++;
		len--;
	}
	return 1;
}

/* This sample function is designed to do the equivalent of s/match/replace/ on
 * the input string. It applies a regex and restarts from the last matched
 * location until nothing matches anymore. First arg is the regex to apply to
 * the input string, second arg is the replacement expression.
 */
static int sample_conv_regsub(const struct arg *arg_p, struct sample *smp, void *private)
{
	char *start, *end;
	struct my_regex *reg = arg_p[0].data.reg;
	regmatch_t pmatch[MAX_MATCH];
	struct buffer *trash = get_trash_chunk();
	int flag, max;
	int found;

	start = smp->data.u.str.area;
	end = start + smp->data.u.str.data;

	flag = 0;
	while (1) {
		/* check for last round which is used to copy remaining parts
		 * when not running in global replacement mode.
		 */
		found = 0;
		if ((arg_p[0].type_flags & ARGF_REG_GLOB) || !(flag & REG_NOTBOL)) {
			/* Note: we can have start == end on empty strings or at the end */
			found = regex_exec_match2(reg, start, end - start, MAX_MATCH, pmatch, flag);
		}

		if (!found)
			pmatch[0].rm_so = end - start;

		/* copy the heading non-matching part (which may also be the tail if nothing matches) */
		max = trash->size - trash->data;
		if (max && pmatch[0].rm_so > 0) {
			if (max > pmatch[0].rm_so)
				max = pmatch[0].rm_so;
			memcpy(trash->area + trash->data, start, max);
			trash->data += max;
		}

		if (!found)
			break;

		/* replace the matching part */
		max = trash->size - trash->data;
		if (max) {
			if (max > arg_p[1].data.str.data)
				max = arg_p[1].data.str.data;
			memcpy(trash->area + trash->data,
			       arg_p[1].data.str.area, max);
			trash->data += max;
		}

		/* stop here if we're done with this string */
		if (start >= end)
			break;

		/* We have a special case for matches of length 0 (eg: "x*y*").
		 * These ones are considered to match in front of a character,
		 * so we have to copy that character and skip to the next one.
		 */
		if (!pmatch[0].rm_eo) {
			if (trash->data < trash->size)
				trash->area[trash->data++] = start[pmatch[0].rm_eo];
			pmatch[0].rm_eo++;
		}

		start += pmatch[0].rm_eo;
		flag |= REG_NOTBOL;
	}

	smp->data.u.str = *trash;
	return 1;
}

/* This function check an operator entry. It expects a string.
 * The string can be an integer or a variable name.
 */
static int check_operator(struct arg *args, struct sample_conv *conv,
                          const char *file, int line, char **err)
{
	const char *str;
	const char *end;

	/* Try to decode a variable. */
	if (vars_check_arg(&args[0], NULL))
		return 1;

	/* Try to convert an integer */
	str = args[0].data.str.area;
	end = str + strlen(str);
	args[0].data.sint = read_int64(&str, end);
	if (*str != '\0') {
		memprintf(err, "expects an integer or a variable name");
		return 0;
	}
	args[0].type = ARGT_SINT;
	return 1;
}

/* This function returns a sample struct filled with an arg content.
 * If the arg contain an integer, the integer is returned in the
 * sample. If the arg contains a variable descriptor, it returns the
 * variable value.
 *
 * This function returns 0 if an error occurs, otherwise it returns 1.
 */
static inline int sample_conv_var2smp(const struct arg *arg, struct sample *smp)
{
	switch (arg->type) {
	case ARGT_SINT:
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = arg->data.sint;
		return 1;
	case ARGT_VAR:
		if (!vars_get_by_desc(&arg->data.var, smp))
			return 0;
		if (!sample_casts[smp->data.type][SMP_T_SINT])
			return 0;
		if (!sample_casts[smp->data.type][SMP_T_SINT](smp))
			return 0;
		return 1;
	default:
		return 0;
	}
}

/* Takes a SINT on input, applies a binary twos complement and returns the SINT
 * result.
 */
static int sample_conv_binary_cpl(const struct arg *arg_p, struct sample *smp, void *private)
{
	smp->data.u.sint = ~smp->data.u.sint;
	return 1;
}

/* Takes a SINT on input, applies a binary "and" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 */
static int sample_conv_binary_and(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;
	smp->data.u.sint &= tmp.data.u.sint;
	return 1;
}

/* Takes a SINT on input, applies a binary "or" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 */
static int sample_conv_binary_or(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;
	smp->data.u.sint |= tmp.data.u.sint;
	return 1;
}

/* Takes a SINT on input, applies a binary "xor" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 */
static int sample_conv_binary_xor(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;
	smp->data.u.sint ^= tmp.data.u.sint;
	return 1;
}

static inline long long int arith_add(long long int a, long long int b)
{
	/* Prevent overflow and makes capped calculus.
	 * We must ensure that the check calculus doesn't
	 * exceed the signed 64 bits limits.
	 *
	 *        +----------+----------+
	 *        |   a<0    |   a>=0   |
	 * +------+----------+----------+
	 * | b<0  | MIN-a>b  | no check |
	 * +------+----------+----------+
	 * | b>=0 | no check | MAX-a<b  |
	 * +------+----------+----------+
	 */
	if ((a ^ b) >= 0) {
		/* signs are differents. */
		if (a < 0) {
			if (LLONG_MIN - a > b)
				return LLONG_MIN;
		}
		if (LLONG_MAX - a < b)
			return LLONG_MAX;
	}
	return a + b;
}

/* Takes a SINT on input, applies an arithmetic "add" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 */
static int sample_conv_arith_add(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;
	smp->data.u.sint = arith_add(smp->data.u.sint, tmp.data.u.sint);
	return 1;
}

/* Takes a SINT on input, applies an arithmetic "sub" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 */
static int sample_conv_arith_sub(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;

	/* We cannot represent -LLONG_MIN because abs(LLONG_MIN) is greater
	 * than abs(LLONG_MAX). So, the following code use LLONG_MAX in place
	 * of -LLONG_MIN and correct the result.
	 */
	if (tmp.data.u.sint == LLONG_MIN) {
		smp->data.u.sint = arith_add(smp->data.u.sint, LLONG_MAX);
		if (smp->data.u.sint < LLONG_MAX)
			smp->data.u.sint++;
		return 1;
	}

	/* standard subtraction: we use the "add" function and negate
	 * the second operand.
	 */
	smp->data.u.sint = arith_add(smp->data.u.sint, -tmp.data.u.sint);
	return 1;
}

/* Takes a SINT on input, applies an arithmetic "mul" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 * If the result makes an overflow, then the largest possible quantity is
 * returned.
 */
static int sample_conv_arith_mul(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	struct sample tmp;
	long long int c;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;

	/* prevent divide by 0 during the check */
	if (!smp->data.u.sint || !tmp.data.u.sint) {
		smp->data.u.sint = 0;
		return 1;
	}

	/* The multiply between LLONG_MIN and -1 returns a
	 * "floting point exception".
	 */
	if (smp->data.u.sint == LLONG_MIN && tmp.data.u.sint == -1) {
		smp->data.u.sint = LLONG_MAX;
		return 1;
	}

	/* execute standard multiplication. */
	c = smp->data.u.sint * tmp.data.u.sint;

	/* check for overflow and makes capped multiply. */
	if (smp->data.u.sint != c / tmp.data.u.sint) {
		if ((smp->data.u.sint < 0) == (tmp.data.u.sint < 0)) {
			smp->data.u.sint = LLONG_MAX;
			return 1;
		}
		smp->data.u.sint = LLONG_MIN;
		return 1;
	}
	smp->data.u.sint = c;
	return 1;
}

/* Takes a SINT on input, applies an arithmetic "div" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 * If arg_p makes the result overflow, then the largest possible quantity is
 * returned.
 */
static int sample_conv_arith_div(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;

	if (tmp.data.u.sint) {
		/* The divide between LLONG_MIN and -1 returns a
		 * "floting point exception".
		 */
		if (smp->data.u.sint == LLONG_MIN && tmp.data.u.sint == -1) {
			smp->data.u.sint = LLONG_MAX;
			return 1;
		}
		smp->data.u.sint /= tmp.data.u.sint;
		return 1;
	}
	smp->data.u.sint = LLONG_MAX;
	return 1;
}

/* Takes a SINT on input, applies an arithmetic "mod" with the SINT directly in
 * arg_p or in the variable described in arg_p, and returns the SINT result.
 * If arg_p makes the result overflow, then 0 is returned.
 */
static int sample_conv_arith_mod(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	struct sample tmp;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (!sample_conv_var2smp(arg_p, &tmp))
		return 0;

	if (tmp.data.u.sint) {
		/* The divide between LLONG_MIN and -1 returns a
		 * "floting point exception".
		 */
		if (smp->data.u.sint == LLONG_MIN && tmp.data.u.sint == -1) {
			smp->data.u.sint = 0;
			return 1;
		}
		smp->data.u.sint %= tmp.data.u.sint;
		return 1;
	}
	smp->data.u.sint = 0;
	return 1;
}

/* Takes an SINT on input, applies an arithmetic "neg" and returns the SINT
 * result.
 */
static int sample_conv_arith_neg(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	if (smp->data.u.sint == LLONG_MIN)
		smp->data.u.sint = LLONG_MAX;
	else
		smp->data.u.sint = -smp->data.u.sint;
	return 1;
}

/* Takes a SINT on input, returns true is the value is non-null, otherwise
 * false. The output is a BOOL.
 */
static int sample_conv_arith_bool(const struct arg *arg_p,
                                  struct sample *smp, void *private)
{
	smp->data.u.sint = !!smp->data.u.sint;
	smp->data.type = SMP_T_BOOL;
	return 1;
}

/* Takes a SINT on input, returns false is the value is non-null, otherwise
 * truee. The output is a BOOL.
 */
static int sample_conv_arith_not(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	smp->data.u.sint = !smp->data.u.sint;
	smp->data.type = SMP_T_BOOL;
	return 1;
}

/* Takes a SINT on input, returns true is the value is odd, otherwise false.
 * The output is a BOOL.
 */
static int sample_conv_arith_odd(const struct arg *arg_p,
                                 struct sample *smp, void *private)
{
	smp->data.u.sint = smp->data.u.sint & 1;
	smp->data.type = SMP_T_BOOL;
	return 1;
}

/* Takes a SINT on input, returns true is the value is even, otherwise false.
 * The output is a BOOL.
 */
static int sample_conv_arith_even(const struct arg *arg_p,
                                  struct sample *smp, void *private)
{
	smp->data.u.sint = !(smp->data.u.sint & 1);
	smp->data.type = SMP_T_BOOL;
	return 1;
}

/* appends an optional const string, an optional variable contents and another
 * optional const string to an existing string.
 */
static int sample_conv_concat(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct buffer *trash;
	struct sample tmp;
	int max;

	trash = get_trash_chunk();
	trash->data = smp->data.u.str.data;
	if (trash->data > trash->size - 1)
		trash->data = trash->size - 1;

	memcpy(trash->area, smp->data.u.str.area, trash->data);
	trash->area[trash->data] = 0;

	/* append first string */
	max = arg_p[0].data.str.data;
	if (max > trash->size - 1 - trash->data)
		max = trash->size - 1 - trash->data;

	if (max) {
		memcpy(trash->area + trash->data, arg_p[0].data.str.area, max);
		trash->data += max;
		trash->area[trash->data] = 0;
	}

	/* append second string (variable) if it's found and we can turn it
	 * into a string.
	 */
	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (arg_p[1].type == ARGT_VAR && vars_get_by_desc(&arg_p[1].data.var, &tmp) &&
	    (sample_casts[tmp.data.type][SMP_T_STR] == c_none ||
	     sample_casts[tmp.data.type][SMP_T_STR](&tmp))) {

		max = tmp.data.u.str.data;
		if (max > trash->size - 1 - trash->data)
			max = trash->size - 1 - trash->data;

		if (max) {
			memcpy(trash->area + trash->data, tmp.data.u.str.area,
			       max);
			trash->data += max;
			trash->area[trash->data] = 0;
		}
	}

	/* append third string */
	max = arg_p[2].data.str.data;
	if (max > trash->size - 1 - trash->data)
		max = trash->size - 1 - trash->data;

	if (max) {
		memcpy(trash->area + trash->data, arg_p[2].data.str.area, max);
		trash->data += max;
		trash->area[trash->data] = 0;
	}

	smp->data.u.str = *trash;
	smp->data.type = SMP_T_STR;
	return 1;
}

/* This function checks the "concat" converter's arguments and extracts the
 * variable name and its scope.
 */
static int smp_check_concat(struct arg *args, struct sample_conv *conv,
                           const char *file, int line, char **err)
{
	/* Try to decode a variable. */
	if (args[1].data.str.data > 0 && !vars_check_arg(&args[1], NULL)) {
		memprintf(err, "failed to register variable name '%s'",
			  args[1].data.str.area);
		return 0;
	}
	return 1;
}

/* compares string with a variable containing a string. Return value
 * is compatible with strcmp(3)'s return value.
 */
static int sample_conv_strcmp(const struct arg *arg_p, struct sample *smp, void *private)
{
	struct sample tmp;
	int max, result;

	smp_set_owner(&tmp, smp->px, smp->sess, smp->strm, smp->opt);
	if (arg_p[0].type != ARGT_VAR)
		return 0;
	if (!vars_get_by_desc(&arg_p[0].data.var, &tmp))
		return 0;
	if (!sample_casts[tmp.data.type][SMP_T_STR](&tmp))
		return 0;

	max = MIN(smp->data.u.str.data, tmp.data.u.str.data);
	result = strncmp(smp->data.u.str.area, tmp.data.u.str.area, max);
	if (result == 0) {
		if (smp->data.u.str.data != tmp.data.u.str.data) {
			if (smp->data.u.str.data < tmp.data.u.str.data) {
				result = -1;
			}
			else {
				result = 1;
			}
		}
	}

	smp->data.u.sint = result;
	smp->data.type = SMP_T_SINT;
	return 1;
}

#define GRPC_MSG_COMPRESS_FLAG_SZ 1 /* 1 byte */
#define GRPC_MSG_LENGTH_SZ        4 /* 4 bytes */
#define GRPC_MSG_HEADER_SZ        (GRPC_MSG_COMPRESS_FLAG_SZ + GRPC_MSG_LENGTH_SZ)

/*
 * Extract the field value of an input binary sample. Takes a mandatory argument:
 * the protocol buffers field identifier (dotted notation) internally represented
 * as an array of unsigned integers and its size.
 * Return 1 if the field was found, 0 if not.
 */
static int sample_conv_ungrpc(const struct arg *arg_p, struct sample *smp, void *private)
{
	unsigned char *pos;
	size_t grpc_left;

	pos = (unsigned char *)smp->data.u.str.area;
	grpc_left = smp->data.u.str.data;

	while (grpc_left > GRPC_MSG_HEADER_SZ) {
		size_t grpc_msg_len, left;

		grpc_msg_len = left = ntohl(*(uint32_t *)(pos + GRPC_MSG_COMPRESS_FLAG_SZ));

		pos += GRPC_MSG_HEADER_SZ;
		grpc_left -= GRPC_MSG_HEADER_SZ;

		if (grpc_left < left)
			return 0;

		if (protobuf_field_lookup(arg_p, smp, &pos, &left))
			return 1;

		grpc_left -= grpc_msg_len;
	}

	return 0;
}

static int sample_conv_protobuf(const struct arg *arg_p, struct sample *smp, void *private)
{
	unsigned char *pos;
	size_t left;

	pos = (unsigned char *)smp->data.u.str.area;
	left = smp->data.u.str.data;

	return protobuf_field_lookup(arg_p, smp, &pos, &left);
}

static int sample_conv_protobuf_check(struct arg *args, struct sample_conv *conv,
                                      const char *file, int line, char **err)
{
	if (!args[1].type) {
		args[1].type = ARGT_SINT;
		args[1].data.sint = PBUF_T_BINARY;
	}
	else {
		int pbuf_type;

		pbuf_type = protobuf_type(args[1].data.str.area);
		if (pbuf_type == -1) {
			memprintf(err, "Wrong protocol buffer type '%s'", args[1].data.str.area);
			return 0;
		}

		args[1].type = ARGT_SINT;
		args[1].data.sint = pbuf_type;
	}

	return 1;
}

/* This function checks the "strcmp" converter's arguments and extracts the
 * variable name and its scope.
 */
static int smp_check_strcmp(struct arg *args, struct sample_conv *conv,
                           const char *file, int line, char **err)
{
	/* Try to decode a variable. */
	if (vars_check_arg(&args[0], NULL))
		return 1;

	memprintf(err, "failed to register variable name '%s'",
		  args[0].data.str.area);
	return 0;
}

/************************************************************************/
/*       All supported sample fetch functions must be declared here     */
/************************************************************************/

/* force TRUE to be returned at the fetch level */
static int
smp_fetch_true(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (!smp_make_rw(smp))
		return 0;

	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = 1;
	return 1;
}

/* force FALSE to be returned at the fetch level */
static int
smp_fetch_false(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = 0;
	return 1;
}

/* retrieve environment variable $1 as a string */
static int
smp_fetch_env(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	char *env;

	if (!args || args[0].type != ARGT_STR)
		return 0;

	env = getenv(args[0].data.str.area);
	if (!env)
		return 0;

	smp->data.type = SMP_T_STR;
	smp->flags = SMP_F_CONST;
	smp->data.u.str.area = env;
	smp->data.u.str.data = strlen(env);
	return 1;
}

/* retrieve the current local date in epoch time, and applies an optional offset
 * of args[0] seconds.
 */
static int
smp_fetch_date(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.u.sint = date.tv_sec;

	/* add offset */
	if (args && args[0].type == ARGT_SINT)
		smp->data.u.sint += args[0].data.sint;

	smp->data.type = SMP_T_SINT;
	smp->flags |= SMP_F_VOL_TEST | SMP_F_MAY_CHANGE;
	return 1;
}

/* retrieve the current microsecond part of the date  */
static int
smp_fetch_date_us(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.u.sint = date.tv_usec;
	smp->data.type = SMP_T_SINT;
	smp->flags |= SMP_F_VOL_TEST | SMP_F_MAY_CHANGE;
	return 1;
}


/* returns the hostname */
static int
smp_fetch_hostname(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_STR;
	smp->flags = SMP_F_CONST;
	smp->data.u.str.area = hostname;
	smp->data.u.str.data = strlen(hostname);
	return 1;
}

/* returns the number of processes */
static int
smp_fetch_nbproc(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = global.nbproc;
	return 1;
}

/* returns the number of the current process (between 1 and nbproc */
static int
smp_fetch_proc(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = relative_pid;
	return 1;
}

/* returns the number of the current thread (between 1 and nbthread */
static int
smp_fetch_thread(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = tid;
	return 1;
}

/* generate a random 32-bit integer for whatever purpose, with an optional
 * range specified in argument.
 */
static int
smp_fetch_rand(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.u.sint = random();

	/* reduce if needed. Don't do a modulo, use all bits! */
	if (args && args[0].type == ARGT_SINT)
		smp->data.u.sint = (smp->data.u.sint * args[0].data.sint) / ((u64)RAND_MAX+1);

	smp->data.type = SMP_T_SINT;
	smp->flags |= SMP_F_VOL_TEST | SMP_F_MAY_CHANGE;
	return 1;
}

/* returns true if the current process is stopping */
static int
smp_fetch_stopping(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = stopping;
	return 1;
}

/* returns the number of calls of the current stream's process_stream() */
static int
smp_fetch_cpu_calls(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->task->calls;
	return 1;
}

/* returns the average number of nanoseconds spent processing the stream per call */
static int
smp_fetch_cpu_ns_avg(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->task->calls ? smp->strm->task->cpu_time / smp->strm->task->calls : 0;
	return 1;
}

/* returns the total number of nanoseconds spent processing the stream */
static int
smp_fetch_cpu_ns_tot(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->task->cpu_time;
	return 1;
}

/* returns the average number of nanoseconds per call spent waiting for other tasks to be processed */
static int
smp_fetch_lat_ns_avg(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->task->calls ? smp->strm->task->lat_time / smp->strm->task->calls : 0;
	return 1;
}

/* returns the total number of nanoseconds per call spent waiting for other tasks to be processed */
static int
smp_fetch_lat_ns_tot(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->task->lat_time;
	return 1;
}

static int smp_fetch_const_str(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->flags |= SMP_F_CONST;
	smp->data.type = SMP_T_STR;
	smp->data.u.str.area = args[0].data.str.area;
	smp->data.u.str.data = args[0].data.str.data;
	return 1;
}

static int smp_check_const_bool(struct arg *args, char **err)
{
	if (strcasecmp(args[0].data.str.area, "true") == 0 ||
	    strcasecmp(args[0].data.str.area, "1") == 0) {
		args[0].type = ARGT_SINT;
		args[0].data.sint = 1;
		return 1;
	}
	if (strcasecmp(args[0].data.str.area, "false") == 0 ||
	    strcasecmp(args[0].data.str.area, "0") == 0) {
		args[0].type = ARGT_SINT;
		args[0].data.sint = 0;
		return 1;
	}
	memprintf(err, "Expects 'true', 'false', '0' or '1'");
	return 0;
}

static int smp_fetch_const_bool(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = args[0].data.sint;
	return 1;
}

static int smp_fetch_const_int(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = args[0].data.sint;
	return 1;
}

static int smp_fetch_const_ipv4(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_IPV4;
	smp->data.u.ipv4 = args[0].data.ipv4;
	return 1;
}

static int smp_fetch_const_ipv6(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_IPV6;
	smp->data.u.ipv6 = args[0].data.ipv6;
	return 1;
}

static int smp_check_const_bin(struct arg *args, char **err)
{
	char *binstr = NULL;
	int binstrlen;

	if (!parse_binary(args[0].data.str.area, &binstr, &binstrlen, err))
		return 0;
	args[0].type = ARGT_STR;
	args[0].data.str.area = binstr;
	args[0].data.str.data = binstrlen;
	return 1;
}

static int smp_fetch_const_bin(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->flags |= SMP_F_CONST;
	smp->data.type = SMP_T_BIN;
	smp->data.u.str.area = args[0].data.str.area;
	smp->data.u.str.data = args[0].data.str.data;
	return 1;
}

static int smp_check_const_meth(struct arg *args, char **err)
{
	enum http_meth_t meth;
	int i;

	meth = find_http_meth(args[0].data.str.area, args[0].data.str.data);
	if (meth != HTTP_METH_OTHER) {
		args[0].type = ARGT_SINT;
		args[0].data.sint = meth;
	} else {
		/* Check method avalaibility. A methos is a token defined as :
		 * tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
		 *         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
		 * token = 1*tchar
		 */
		for (i = 0; i < args[0].data.str.data; i++) {
			if (!HTTP_IS_TOKEN(args[0].data.str.area[i])) {
				memprintf(err, "expects valid method.");
				return 0;
			}
		}
	}
	return 1;
}

static int smp_fetch_const_meth(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_METH;
	if (args[0].type == ARGT_SINT) {
		smp->flags &= ~SMP_F_CONST;
		smp->data.u.meth.meth = args[0].data.sint;
		smp->data.u.meth.str.area = "";
		smp->data.u.meth.str.data = 0;
	} else {
		smp->flags |= SMP_F_CONST;
		smp->data.u.meth.meth = HTTP_METH_OTHER;
		smp->data.u.meth.str.area = args[0].data.str.area;
		smp->data.u.meth.str.data = args[0].data.str.data;
	}
	return 1;
}

/* Note: must not be declared <const> as its list will be overwritten.
 * Note: fetches that may return multiple types must be declared as the lowest
 * common denominator, the type that can be casted into all other ones. For
 * instance IPv4/IPv6 must be declared IPv4.
 */
static struct sample_fetch_kw_list smp_kws = {ILH, {
	{ "always_false", smp_fetch_false, 0,            NULL, SMP_T_BOOL, SMP_USE_INTRN },
	{ "always_true",  smp_fetch_true,  0,            NULL, SMP_T_BOOL, SMP_USE_INTRN },
	{ "env",          smp_fetch_env,   ARG1(1,STR),  NULL, SMP_T_STR,  SMP_USE_INTRN },
	{ "date",         smp_fetch_date,  ARG1(0,SINT), NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "date_us",      smp_fetch_date_us,  0,         NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "hostname",     smp_fetch_hostname, 0,         NULL, SMP_T_STR,  SMP_USE_INTRN },
	{ "nbproc",       smp_fetch_nbproc,0,            NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "proc",         smp_fetch_proc,  0,            NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "thread",       smp_fetch_thread,  0,          NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "rand",         smp_fetch_rand,  ARG1(0,SINT), NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "stopping",     smp_fetch_stopping, 0,         NULL, SMP_T_BOOL, SMP_USE_INTRN },
	{ "stopping",     smp_fetch_stopping, 0,         NULL, SMP_T_BOOL, SMP_USE_INTRN },

	{ "cpu_calls",    smp_fetch_cpu_calls,  0,       NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "cpu_ns_avg",   smp_fetch_cpu_ns_avg, 0,       NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "cpu_ns_tot",   smp_fetch_cpu_ns_tot, 0,       NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "lat_ns_avg",   smp_fetch_lat_ns_avg, 0,       NULL, SMP_T_SINT, SMP_USE_INTRN },
	{ "lat_ns_tot",   smp_fetch_lat_ns_tot, 0,       NULL, SMP_T_SINT, SMP_USE_INTRN },

	{ "str",  smp_fetch_const_str,  ARG1(1,STR),  NULL                , SMP_T_STR,  SMP_USE_INTRN },
	{ "bool", smp_fetch_const_bool, ARG1(1,STR),  smp_check_const_bool, SMP_T_BOOL, SMP_USE_INTRN },
	{ "int",  smp_fetch_const_int,  ARG1(1,SINT), NULL                , SMP_T_SINT, SMP_USE_INTRN },
	{ "ipv4", smp_fetch_const_ipv4, ARG1(1,IPV4), NULL                , SMP_T_IPV4, SMP_USE_INTRN },
	{ "ipv6", smp_fetch_const_ipv6, ARG1(1,IPV6), NULL                , SMP_T_IPV6, SMP_USE_INTRN },
	{ "bin",  smp_fetch_const_bin,  ARG1(1,STR),  smp_check_const_bin , SMP_T_BIN,  SMP_USE_INTRN },
	{ "meth", smp_fetch_const_meth, ARG1(1,STR),  smp_check_const_meth, SMP_T_METH, SMP_USE_INTRN },

	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &smp_kws);

/* Note: must not be declared <const> as its list will be overwritten */
static struct sample_conv_kw_list sample_conv_kws = {ILH, {
#ifdef DEBUG_EXPR
	{ "debug",  sample_conv_debug,     0,            NULL, SMP_T_ANY,  SMP_T_ANY  },
#endif

	{ "b64dec", sample_conv_base642bin,0,            NULL, SMP_T_STR,  SMP_T_BIN  },
	{ "base64", sample_conv_bin2base64,0,            NULL, SMP_T_BIN,  SMP_T_STR  },
	{ "upper",  sample_conv_str2upper, 0,            NULL, SMP_T_STR,  SMP_T_STR  },
	{ "lower",  sample_conv_str2lower, 0,            NULL, SMP_T_STR,  SMP_T_STR  },
	{ "length", sample_conv_length,    0,            NULL, SMP_T_STR,  SMP_T_SINT },
	{ "hex",    sample_conv_bin2hex,   0,            NULL, SMP_T_BIN,  SMP_T_STR  },
	{ "hex2i",  sample_conv_hex2int,   0,            NULL, SMP_T_STR,  SMP_T_SINT },
	{ "ipmask", sample_conv_ipmask,    ARG2(1,MSK4,MSK6), NULL, SMP_T_ADDR, SMP_T_IPV4 },
	{ "ltime",  sample_conv_ltime,     ARG2(1,STR,SINT), NULL, SMP_T_SINT, SMP_T_STR },
	{ "utime",  sample_conv_utime,     ARG2(1,STR,SINT), NULL, SMP_T_SINT, SMP_T_STR },
	{ "crc32",  sample_conv_crc32,     ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "crc32c", sample_conv_crc32c,    ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "djb2",   sample_conv_djb2,      ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "sdbm",   sample_conv_sdbm,      ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "wt6",    sample_conv_wt6,       ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "xxh32",  sample_conv_xxh32,     ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "xxh64",  sample_conv_xxh64,     ARG1(0,SINT), NULL, SMP_T_BIN,  SMP_T_SINT  },
	{ "json",   sample_conv_json,      ARG1(1,STR),  sample_conv_json_check, SMP_T_STR,  SMP_T_STR },
	{ "bytes",  sample_conv_bytes,     ARG2(1,SINT,SINT), NULL, SMP_T_BIN,  SMP_T_BIN },
	{ "field",  sample_conv_field,     ARG3(2,SINT,STR,SINT), sample_conv_field_check, SMP_T_STR,  SMP_T_STR },
	{ "word",   sample_conv_word,      ARG3(2,SINT,STR,SINT), sample_conv_field_check, SMP_T_STR,  SMP_T_STR },
	{ "regsub", sample_conv_regsub,    ARG3(2,REG,STR,STR), sample_conv_regsub_check, SMP_T_STR, SMP_T_STR },
	{ "sha1",   sample_conv_sha1,      0,            NULL, SMP_T_BIN,  SMP_T_BIN  },
	{ "concat", sample_conv_concat,    ARG3(1,STR,STR,STR), smp_check_concat, SMP_T_STR,  SMP_T_STR },
	{ "strcmp", sample_conv_strcmp,    ARG1(1,STR), smp_check_strcmp, SMP_T_STR,  SMP_T_SINT },

	/* gRPC converters. */
	{ "ungrpc", sample_conv_ungrpc,    ARG2(1,PBUF_FNUM,STR), sample_conv_protobuf_check, SMP_T_BIN, SMP_T_BIN  },
	{ "protobuf", sample_conv_protobuf, ARG2(1,PBUF_FNUM,STR), sample_conv_protobuf_check, SMP_T_BIN, SMP_T_BIN  },

	{ "and",    sample_conv_binary_and, ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "or",     sample_conv_binary_or,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "xor",    sample_conv_binary_xor, ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "cpl",    sample_conv_binary_cpl,           0, NULL, SMP_T_SINT, SMP_T_SINT  },
	{ "bool",   sample_conv_arith_bool,           0, NULL, SMP_T_SINT, SMP_T_BOOL },
	{ "not",    sample_conv_arith_not,            0, NULL, SMP_T_SINT, SMP_T_BOOL },
	{ "odd",    sample_conv_arith_odd,            0, NULL, SMP_T_SINT, SMP_T_BOOL },
	{ "even",   sample_conv_arith_even,           0, NULL, SMP_T_SINT, SMP_T_BOOL },
	{ "add",    sample_conv_arith_add,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "sub",    sample_conv_arith_sub,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "mul",    sample_conv_arith_mul,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "div",    sample_conv_arith_div,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "mod",    sample_conv_arith_mod,  ARG1(1,STR), check_operator, SMP_T_SINT, SMP_T_SINT  },
	{ "neg",    sample_conv_arith_neg,            0, NULL, SMP_T_SINT, SMP_T_SINT  },

	{ NULL, NULL, 0, 0, 0 },
}};

INITCALL1(STG_REGISTER, sample_register_convs, &sample_conv_kws);
