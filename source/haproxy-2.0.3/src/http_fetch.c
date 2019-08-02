/*
 * HTTP samples fetching
 *
 * Copyright 2000-2018 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <sys/types.h>

#include <ctype.h>
#include <string.h>
#include <time.h>

#include <common/base64.h>
#include <common/chunk.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/h1.h>
#include <common/http.h>
#include <common/htx.h>
#include <common/initcall.h>
#include <common/memory.h>
#include <common/standard.h>
#include <common/version.h>

#include <types/global.h>

#include <proto/arg.h>
#include <proto/auth.h>
#include <proto/hdr_idx.h>
#include <proto/http_fetch.h>
#include <proto/http_htx.h>
#include <proto/log.h>
#include <proto/obj_type.h>
#include <proto/proto_http.h>
#include <proto/sample.h>
#include <proto/stream.h>


/* this struct is used between calls to smp_fetch_hdr() or smp_fetch_cookie() */
static THREAD_LOCAL struct hdr_ctx static_hdr_ctx;
static THREAD_LOCAL struct http_hdr_ctx static_http_hdr_ctx;

#define SMP_REQ_CHN(smp) (smp->strm ? &smp->strm->req : NULL)
#define SMP_RES_CHN(smp) (smp->strm ? &smp->strm->res : NULL)

/*
 * Returns the data from Authorization header. Function may be called more
 * than once so data is stored in txn->auth_data. When no header is found
 * or auth method is unknown auth_method is set to HTTP_AUTH_WRONG to avoid
 * searching again for something we are unable to find anyway. However, if
 * the result if valid, the cache is not reused because we would risk to
 * have the credentials overwritten by another stream in parallel.
 */

static int get_http_auth(struct sample *smp, struct htx *htx)
{
	struct stream *s = smp->strm;
	struct http_txn *txn = s->txn;
	struct buffer auth_method;
	char *h, *p;
	int len;

#ifdef DEBUG_AUTH
	printf("Auth for stream %p: %d\n", s, txn->auth.method);
#endif
	if (txn->auth.method == HTTP_AUTH_WRONG)
		return 0;

	txn->auth.method = HTTP_AUTH_WRONG;

	if (htx) {
		/* HTX version */
		struct http_hdr_ctx ctx = { .blk = NULL };
		struct ist hdr;

		if (txn->flags & TX_USE_PX_CONN)
			hdr = ist("Proxy-Authorization");
		else
			hdr = ist("Authorization");

		ctx.blk = NULL;
		if (!http_find_header(htx, hdr, &ctx, 0))
			return 0;

		p   = memchr(ctx.value.ptr, ' ', ctx.value.len);
		len = p - ctx.value.ptr;
		if (!p || len <= 0)
			return 0;

		if (chunk_initlen(&auth_method, ctx.value.ptr, 0, len) != 1)
			return 0;

		chunk_initlen(&txn->auth.method_data, p + 1, 0, ctx.value.len - len - 1);
	}
	else {
		/* LEGACY version */
		struct hdr_ctx ctx = { .idx = 0 };

		if (txn->flags & TX_USE_PX_CONN) {
			h = "Proxy-Authorization";
			len = strlen(h);
		} else {
			h = "Authorization";
			len = strlen(h);
		}

		if (!http_find_header2(h, len, ci_head(&s->req), &txn->hdr_idx, &ctx))
			return 0;

		h = ctx.line + ctx.val;

		p = memchr(h, ' ', ctx.vlen);
		len = p - h;
		if (!p || len <= 0)
			return 0;

		if (chunk_initlen(&auth_method, h, 0, len) != 1)
			return 0;

		chunk_initlen(&txn->auth.method_data, p + 1, 0, ctx.vlen - len - 1);
	}

	if (!strncasecmp("Basic", auth_method.area, auth_method.data)) {
		struct buffer *http_auth = get_trash_chunk();

		len = base64dec(txn->auth.method_data.area,
				txn->auth.method_data.data,
				http_auth->area, global.tune.bufsize - 1);

		if (len < 0)
			return 0;


		http_auth->area[len] = '\0';

		p = strchr(http_auth->area, ':');

		if (!p)
			return 0;

		txn->auth.user = http_auth->area;
		*p = '\0';
		txn->auth.pass = p+1;

		txn->auth.method = HTTP_AUTH_BASIC;
		return 1;
	}

	return 0;
}

/* This function ensures that the prerequisites for an L7 fetch are ready,
 * which means that a request or response is ready. If some data is missing,
 * a parsing attempt is made. This is useful in TCP-based ACLs which are able
 * to extract data from L7. If <vol> is non-null during a prefetch, another
 * test is made to ensure the required information is not gone.
 *
 * The function returns :
 *   NULL with SMP_F_MAY_CHANGE in the sample flags if some data is missing to
 *     decide whether or not an HTTP message is present ;
 *   NULL if the requested data cannot be fetched or if it is certain that
 *     we'll never have any HTTP message there ;
 *   The HTX message if ready
 */
struct htx *smp_prefetch_htx(struct sample *smp, struct channel *chn, int vol)
{
	struct stream *s = smp->strm;
	struct http_txn *txn = NULL;
	struct htx *htx = NULL;
	struct http_msg *msg;
	struct htx_sl *sl;

	/* Note: it is possible that <s> is NULL when called before stream
	 * initialization (eg: tcp-request connection), so this function is the
	 * one responsible for guarding against this case for all HTTP users.
	 */
	if (!s || !chn)
		return NULL;

	if (!s->txn) {
		if (unlikely(!http_alloc_txn(s)))
			return NULL; /* not enough memory */
		http_init_txn(s);
		txn = s->txn;
	}
	txn = s->txn;
	msg = (!(chn->flags & CF_ISRESP) ? &txn->req : &txn->rsp);
	smp->data.type = SMP_T_BOOL;

	if (IS_HTX_STRM(s)) {
		htx = htxbuf(&chn->buf);

		if (msg->msg_state == HTTP_MSG_ERROR || (htx->flags & HTX_FL_PARSING_ERROR))
			return NULL;

		if (msg->msg_state < HTTP_MSG_BODY) {
			/* Analyse not yet started */
			if (htx_is_empty(htx) || htx->first == -1) {
				/* Parsing is done by the mux, just wait */
				smp->flags |= SMP_F_MAY_CHANGE;
				return NULL;
			}
		}
		sl = http_get_stline(htx);
		if (vol && !sl) {
			/* The start-line was already forwarded, it is too late to fetch anything */
			return NULL;
		}
	}
	else { /* RAW mode */
		struct buffer *buf;
		struct h1m h1m;
		struct http_hdr hdrs[global.tune.max_http_hdr];
		union h1_sl h1sl;
		unsigned int flags = HTX_FL_NONE;
		int ret;

		/* no HTTP fetch on the response in TCP mode */
		if (chn->flags & CF_ISRESP)
			return NULL;

		/* Now we are working on the request only */
		buf = &chn->buf;
		if (b_head(buf) + b_data(buf) > b_wrap(buf))
			b_slow_realign(buf, trash.area, 0);

		h1m_init_req(&h1m);
		ret = h1_headers_to_hdr_list(b_head(buf), b_stop(buf),
					     hdrs, sizeof(hdrs)/sizeof(hdrs[0]), &h1m, &h1sl);
		if (ret <= 0) {
			/* Invalid or too big*/
			if (ret < 0 || channel_full(&s->req, global.tune.maxrewrite))
				return NULL;

			/* wait for a full request */
			smp->flags |= SMP_F_MAY_CHANGE;
			return NULL;
		}

		/* OK we just got a valid HTTP mesage. We have to convert it
		 * into an HTX message.
		 */
		if (unlikely(h1sl.rq.v.len == 0)) {
			/* try to convert HTTP/0.9 requests to HTTP/1.0 */
			if (h1sl.rq.meth != HTTP_METH_GET || !h1sl.rq.u.len)
				return NULL;
			h1sl.rq.v = ist("HTTP/1.0");
		}

		/* Set HTX start-line flags */
		if (h1m.flags & H1_MF_VER_11)
			flags |= HTX_SL_F_VER_11;
		if (h1m.flags & H1_MF_XFER_ENC)
			flags |= HTX_SL_F_XFER_ENC;
		flags |= HTX_SL_F_XFER_LEN;
		if (h1m.flags & H1_MF_CHNK)
			flags |= HTX_SL_F_CHNK;
		else if (h1m.flags & H1_MF_CLEN)
			flags |= HTX_SL_F_CLEN;

		htx = htx_from_buf(get_trash_chunk());
		sl = htx_add_stline(htx, HTX_BLK_REQ_SL, flags, h1sl.rq.m, h1sl.rq.u, h1sl.rq.v);
		if (!sl || !htx_add_all_headers(htx, hdrs))
			return NULL;
		sl->info.req.meth = h1sl.rq.meth;
	}

	/* OK we just got a valid HTTP message. If not already done by
	 * HTTP analyzers, we have some minor preparation to perform so
	 * that further checks can rely on HTTP tests.
	 */
	if (sl && msg->msg_state < HTTP_MSG_BODY) {
		if (!(chn->flags & CF_ISRESP)) {
			txn->meth = sl->info.req.meth;
			if (txn->meth == HTTP_METH_GET || txn->meth == HTTP_METH_HEAD)
				s->flags |= SF_REDIRECTABLE;
		}
		else
			txn->status = sl->info.res.status;
		if (sl->flags & HTX_SL_F_VER_11)
			msg->flags |= HTTP_MSGF_VER_11;
	}

	/* everything's OK */
	smp->data.u.sint = 1;
	return htx;
}

/* This function ensures that the prerequisites for an L7 fetch are ready,
 * which means that a request or response is ready. If some data is missing,
 * a parsing attempt is made. This is useful in TCP-based ACLs which are able
 * to extract data from L7. If <req_vol> is non-null during a request prefetch,
 * another test is made to ensure the required information is not gone.
 *
 * The function returns :
 *   0 with SMP_F_MAY_CHANGE in the sample flags if some data is missing to
 *     decide whether or not an HTTP message is present ;
 *   0 if the requested data cannot be fetched or if it is certain that
 *     we'll never have any HTTP message there ;
 *   1 if an HTTP message is ready
 */
int smp_prefetch_http(struct proxy *px, struct stream *s, unsigned int opt,
                      struct channel *chn, struct sample *smp, int req_vol)
{
	struct http_txn *txn;
	struct http_msg *msg;

	/* Note: it is possible that <s> is NULL when called before stream
	 * initialization (eg: tcp-request connection), so this function is the
	 * one responsible for guarding against this case for all HTTP users.
	 */
	if (!s || !chn)
		return 0;

	if (!s->txn) {
		if (unlikely(!http_alloc_txn(s)))
			return 0; /* not enough memory */
		http_init_txn(s);
	}
	txn = s->txn;
	smp->data.type = SMP_T_BOOL;

	if (chn->flags & CF_ISRESP) {
		/* Check for a dependency on a response */
		if (txn->rsp.msg_state < HTTP_MSG_BODY) {
			smp->flags |= SMP_F_MAY_CHANGE;
			return 0;
		}
		goto end;
	}

	/* Check for a dependency on a request */
	msg = &txn->req;

	if (req_vol && (smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) {
		return 0;  /* data might have moved and indexes changed */
	}

	/* If the buffer does not leave enough free space at the end, we must
	 * first realign it.
	 */
	if (ci_head(chn) > b_orig(&chn->buf) &&
	    ci_head(chn) + ci_data(chn) > b_wrap(&chn->buf) - global.tune.maxrewrite)
		channel_slow_realign(chn, trash.area);

	if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
		if (msg->msg_state == HTTP_MSG_ERROR)
			return 0;

		/* Try to decode HTTP request */
		if (likely(msg->next < ci_data(chn)))
			http_msg_analyzer(msg, &txn->hdr_idx);

		/* Still no valid request ? */
		if (unlikely(msg->msg_state < HTTP_MSG_BODY)) {
			if ((msg->msg_state == HTTP_MSG_ERROR) ||
			    channel_full(chn, global.tune.maxrewrite)) {
				return 0;
			}
			/* wait for final state */
			smp->flags |= SMP_F_MAY_CHANGE;
			return 0;
		}

		/* OK we just got a valid HTTP message. We have some minor
		 * preparation to perform so that further checks can rely
		 * on HTTP tests.
		 */

		/* If the message was parsed but was too large, we must absolutely
		 * return an error so that it is not processed. At the moment this
		 * cannot happen, but if the parsers are to change in the future,
		 * we want this check to be maintained.
		 */
		if (unlikely(ci_head(chn) + ci_data(chn) >
			     b_wrap(&chn->buf) - global.tune.maxrewrite)) {
			msg->err_state = msg->msg_state;
			msg->msg_state = HTTP_MSG_ERROR;
			smp->data.u.sint = 1;
			return 1;
		}

		txn->meth = find_http_meth(ci_head(chn), msg->sl.rq.m_l);
		if (txn->meth == HTTP_METH_GET || txn->meth == HTTP_METH_HEAD)
			s->flags |= SF_REDIRECTABLE;

		if (unlikely(msg->sl.rq.v_l == 0) && !http_upgrade_v09_to_v10(txn))
			return 0;
	}

  end:
	/* everything's OK */
	smp->data.u.sint = 1;
	return 1;
}

/* This function fetches the method of current HTTP request and stores
 * it in the global pattern struct as a chunk. There are two possibilities :
 *   - if the method is known (not HTTP_METH_OTHER), its identifier is stored
 *     in <len> and <ptr> is NULL ;
 *   - if the method is unknown (HTTP_METH_OTHER), <ptr> points to the text and
 *     <len> to its length.
 * This is intended to be used with pat_match_meth() only.
 */
static int smp_fetch_meth(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	int meth;
	struct http_txn *txn;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 0);

		if (!htx)
			return 0;

		txn = smp->strm->txn;
		meth = txn->meth;
		smp->data.type = SMP_T_METH;
		smp->data.u.meth.meth = meth;
		if (meth == HTTP_METH_OTHER) {
			struct htx_sl *sl;

			if ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) {
				/* ensure the indexes are not affected */
				return 0;
			}
			sl = http_get_stline(htx);
			smp->flags |= SMP_F_CONST;
			smp->data.u.meth.str.area = HTX_SL_REQ_MPTR(sl);
			smp->data.u.meth.str.data = HTX_SL_REQ_MLEN(sl);
		}
		smp->flags |= SMP_F_VOL_1ST;
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST_PERM(chn);

		txn = smp->strm->txn;
		meth = txn->meth;
		smp->data.type = SMP_T_METH;
		smp->data.u.meth.meth = meth;
		if (meth == HTTP_METH_OTHER) {
			if ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_RES) {
				/* ensure the indexes are not affected */
				return 0;
			}
			smp->flags |= SMP_F_CONST;
			smp->data.u.meth.str.data = txn->req.sl.rq.m_l;
			smp->data.u.meth.str.area = ci_head(txn->req.chn);
		}
		smp->flags |= SMP_F_VOL_1ST;
	}
	return 1;
}

static int smp_fetch_rqver(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;
	char *ptr;
	int len;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;

		sl = http_get_stline(htx);
		len = HTX_SL_REQ_VLEN(sl);
		ptr = HTX_SL_REQ_VPTR(sl);
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		len = txn->req.sl.rq.v_l;
		ptr = ci_head(chn) + txn->req.sl.rq.v;
	}

	while ((len-- > 0) && (*ptr++ != '/'));
	if (len <= 0)
		return 0;

	smp->data.type = SMP_T_STR;
	smp->data.u.str.area = ptr;
	smp->data.u.str.data = len;

	smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	return 1;
}

static int smp_fetch_stver(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_RES_CHN(smp);
	struct http_txn *txn;
	char *ptr;
	int len;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;

		sl = http_get_stline(htx);
		len = HTX_SL_RES_VLEN(sl);
		ptr = HTX_SL_RES_VPTR(sl);
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		len = txn->rsp.sl.st.v_l;
		ptr = ci_head(chn);
	}

	while ((len-- > 0) && (*ptr++ != '/'));
	if (len <= 0)
		return 0;

	smp->data.type = SMP_T_STR;
	smp->data.u.str.area = ptr;
	smp->data.u.str.data = len;

	smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	return 1;
}

/* 3. Check on Status Code. We manipulate integers here. */
static int smp_fetch_stcode(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_RES_CHN(smp);
	struct http_txn *txn;
	char *ptr;
	int len;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;

		sl = http_get_stline(htx);
		len = HTX_SL_RES_CLEN(sl);
		ptr = HTX_SL_RES_CPTR(sl);
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		len = txn->rsp.sl.st.c_l;
		ptr = ci_head(chn) + txn->rsp.sl.st.c;
	}

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = __strl2ui(ptr, len);
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

static int smp_fetch_uniqueid(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (LIST_ISEMPTY(&smp->sess->fe->format_unique_id))
		return 0;

	if (!smp->strm->unique_id) {
		if ((smp->strm->unique_id = pool_alloc(pool_head_uniqueid)) == NULL)
			return 0;
		smp->strm->unique_id[0] = '\0';
	}
	smp->data.u.str.data = build_logline(smp->strm, smp->strm->unique_id,
	                                    UNIQUEID_LEN, &smp->sess->fe->format_unique_id);

	smp->data.type = SMP_T_STR;
	smp->data.u.str.area = smp->strm->unique_id;
	smp->flags = SMP_F_CONST;
	return 1;
}

/* Returns a string block containing all headers including the
 * empty line which separes headers from the body. This is useful
 * for some headers analysis.
 */
static int smp_fetch_hdrs(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct buffer *temp;
		int32_t pos;

		if (!htx)
			return 0;
		temp = get_trash_chunk();
		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);

			if (type == HTX_BLK_HDR) {
				struct ist n = htx_get_blk_name(htx, blk);
				struct ist v = htx_get_blk_value(htx, blk);

				if (!htx_hdr_to_h1(n, v, temp))
					return 0;
			}
			else if (type == HTX_BLK_EOH) {
				if (!chunk_memcat(temp, "\r\n", 2))
					return 0;
				break;
			}
		}
		smp->data.type = SMP_T_STR;
		smp->data.u.str = *temp;

	}
	else {
		/* LEGACY version */
		struct http_msg *msg;
		struct hdr_idx *idx;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		idx = &txn->hdr_idx;
		msg = &txn->req;

		smp->data.type = SMP_T_STR;
		smp->data.u.str.area = ci_head(chn) + hdr_idx_first_pos(idx);
		smp->data.u.str.data = msg->eoh - hdr_idx_first_pos(idx) + 1 +
			(ci_head(chn)[msg->eoh] == '\r');
	}
	return 1;
}

/* Returns the header request in a length/value encoded format.
 * This is useful for exchanges with the SPOE.
 *
 * A "length value" is a multibyte code encoding numbers. It uses the
 * SPOE format. The encoding is the following:
 *
 * Each couple "header name" / "header value" is composed
 * like this:
 *    "length value" "header name bytes"
 *    "length value" "header value bytes"
 * When the last header is reached, the header name and the header
 * value are empty. Their length are 0
 */
static int smp_fetch_hdrs_bin(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;
	struct buffer *temp;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct buffer *temp;
		char *p, *end;
		int32_t pos;
		int ret;

		if (!htx)
			return 0;
		temp = get_trash_chunk();
		p = temp->area;
		end = temp->area + temp->size;
		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);
			struct ist n, v;

			if (type == HTX_BLK_HDR) {
				n = htx_get_blk_name(htx,blk);
				v = htx_get_blk_value(htx, blk);

				/* encode the header name. */
				ret = encode_varint(n.len, &p, end);
				if (ret == -1)
					return 0;
				if (p + n.len > end)
					return 0;
				memcpy(p, n.ptr, n.len);
				p += n.len;

				/* encode the header value. */
				ret = encode_varint(v.len, &p, end);
				if (ret == -1)
					return 0;
				if (p + v.len > end)
					return 0;
				memcpy(p, v.ptr, v.len);
				p += v.len;

			}
			else if (type == HTX_BLK_EOH) {
				/* encode the end of the header list with empty
				 * header name and header value.
				 */
				ret = encode_varint(0, &p, end);
				if (ret == -1)
					return 0;
				ret = encode_varint(0, &p, end);
				if (ret == -1)
					return 0;
				break;
			}
		}

		/* Initialise sample data which will be filled. */
		smp->data.type = SMP_T_BIN;
		smp->data.u.str.area = temp->area;
		smp->data.u.str.data = p - temp->area;
		smp->data.u.str.size = temp->size;
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		const char *cur_ptr, *cur_next, *p;
		int old_idx, cur_idx;
		struct hdr_idx_elem *cur_hdr;
		const char *hn, *hv;
		int hnl, hvl;
		int ret;
		char *buf;
		char *end;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		temp = get_trash_chunk();
		buf = temp->area;
		end = temp->area + temp->size;

		txn = smp->strm->txn;
		idx = &txn->hdr_idx;

		/* Build array of headers. */
		old_idx = 0;
		cur_next = ci_head(chn) + hdr_idx_first_pos(idx);
		while (1) {
			cur_idx = idx->v[old_idx].next;
			if (!cur_idx)
				break;
			old_idx = cur_idx;

			cur_hdr  = &idx->v[cur_idx];
			cur_ptr  = cur_next;
			cur_next = cur_ptr + cur_hdr->len + cur_hdr->cr + 1;

			/* Now we have one full header at cur_ptr of len cur_hdr->len,
			 * and the next header starts at cur_next. We'll check
			 * this header in the list as well as against the default
			 * rule.
			 */

			/* look for ': *'. */
			hn = cur_ptr;
			for (p = cur_ptr; p < cur_ptr + cur_hdr->len && *p != ':'; p++);
			if (p >= cur_ptr+cur_hdr->len)
				continue;
			hnl = p - hn;
			p++;
			while (p < cur_ptr + cur_hdr->len && (*p == ' ' || *p == '\t'))
				p++;
			if (p >= cur_ptr + cur_hdr->len)
				continue;
			hv = p;
			hvl = cur_ptr + cur_hdr->len-p;

			/* encode the header name. */
			ret = encode_varint(hnl, &buf, end);
			if (ret == -1)
				return 0;
			if (buf + hnl > end)
				return 0;
			memcpy(buf, hn, hnl);
			buf += hnl;

			/* encode and copy the value. */
			ret = encode_varint(hvl, &buf, end);
			if (ret == -1)
				return 0;
			if (buf + hvl > end)
				return 0;
			memcpy(buf, hv, hvl);
			buf += hvl;
		}

		/* encode the end of the header list with empty
		 * header name and header value.
		 */
		ret = encode_varint(0, &buf, end);
		if (ret == -1)
			return 0;
		ret = encode_varint(0, &buf, end);
		if (ret == -1)
			return 0;

		/* Initialise sample data which will be filled. */
		smp->data.type = SMP_T_BIN;
		smp->data.u.str.area = temp->area;
		smp->data.u.str.data = buf - temp->area;
		smp->data.u.str.size = temp->size;
	}
	return 1;
}

/* returns the longest available part of the body. This requires that the body
 * has been waited for using http-buffer-request.
 */
static int smp_fetch_body(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct buffer *temp;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		int32_t pos;

		if (!htx)
			return 0;

		temp = get_trash_chunk();
		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);

			if (type == HTX_BLK_EOM || type == HTX_BLK_TLR || type == HTX_BLK_EOT)
				break;
			if (type == HTX_BLK_DATA) {
				if (!htx_data_to_h1(htx_get_blk_value(htx, blk), temp, 0))
					return 0;
			}
		}

		smp->data.type = SMP_T_BIN;
		smp->data.u.str = *temp;
		smp->flags = SMP_F_VOL_TEST;
	}
	else {
		/* LEGACY version */
		struct http_msg *msg;
		unsigned long len;
		unsigned long block1;
		char *body;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		msg = &smp->strm->txn->req;
		len  = http_body_bytes(msg);
		body = c_ptr(chn, -http_data_rewind(msg));

		block1 = len;
		if (block1 > b_wrap(&chn->buf) - body)
			block1 = b_wrap(&chn->buf) - body;

		if (block1 == len) {
			/* buffer is not wrapped (or empty) */
			smp->data.type = SMP_T_BIN;
			smp->data.u.str.area = body;
			smp->data.u.str.data = len;
			smp->flags = SMP_F_VOL_TEST | SMP_F_CONST;
		}
		else {
			/* buffer is wrapped, we need to defragment it */
			temp = get_trash_chunk();
			memcpy(temp->area, body, block1);
			memcpy(temp->area + block1, b_orig(&chn->buf), len - block1);
			smp->data.type = SMP_T_BIN;
			smp->data.u.str.area = temp->area;
			smp->data.u.str.data = len;
			smp->flags = SMP_F_VOL_TEST;
		}
	}
	return 1;
}


/* returns the available length of the body. This requires that the body
 * has been waited for using http-buffer-request.
 */
static int smp_fetch_body_len(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		int32_t pos;
		unsigned long long len = 0;

		if (!htx)
			return 0;

		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);

			if (type == HTX_BLK_EOM || type == HTX_BLK_TLR || type == HTX_BLK_EOT)
				break;
			if (type == HTX_BLK_DATA)
				len += htx_get_blksz(blk);
		}

		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = len;

		smp->flags = SMP_F_VOL_TEST;
	}
	else {
		/* LEGACY version */
		struct http_msg *msg;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		msg = &smp->strm->txn->req;
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = http_body_bytes(msg);

		smp->flags = SMP_F_VOL_TEST;
	}
	return 1;
}


/* returns the advertised length of the body, or the advertised size of the
 * chunks available in the buffer. This requires that the body has been waited
 * for using http-buffer-request.
 */
static int smp_fetch_body_size(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		int32_t pos;
		unsigned long long len = 0;

		if (!htx)
			return 0;

		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);

			if (type == HTX_BLK_EOM || type == HTX_BLK_TLR || type == HTX_BLK_EOT)
				break;
			if (type == HTX_BLK_DATA)
				len += htx_get_blksz(blk);
		}
		if (htx->extra != ULLONG_MAX)
			len += htx->extra;

		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = len;

		smp->flags = SMP_F_VOL_TEST;
	}
	else {
		/* LEGACY version */
		struct http_msg *msg;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		msg = &smp->strm->txn->req;
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = msg->body_len;

		smp->flags = SMP_F_VOL_TEST;
	}
	return 1;
}


/* 4. Check on URL/URI. A pointer to the URI is stored. */
static int smp_fetch_url(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;
		sl = http_get_stline(htx);
		smp->data.type = SMP_T_STR;
		smp->data.u.str.area = HTX_SL_REQ_UPTR(sl);
		smp->data.u.str.data = HTX_SL_REQ_ULEN(sl);
		smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		smp->data.type = SMP_T_STR;
		smp->data.u.str.data = txn->req.sl.rq.u_l;
		smp->data.u.str.area = ci_head(chn) + txn->req.sl.rq.u;
		smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	}
	return 1;
}

static int smp_fetch_url_ip(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;
	struct sockaddr_storage addr;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;
		sl = http_get_stline(htx);
		url2sa(HTX_SL_REQ_UPTR(sl), HTX_SL_REQ_ULEN(sl), &addr, NULL);
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		url2sa(ci_head(chn) + txn->req.sl.rq.u, txn->req.sl.rq.u_l, &addr, NULL);
	}

	if (((struct sockaddr_in *)&addr)->sin_family != AF_INET)
		return 0;

	smp->data.type = SMP_T_IPV4;
	smp->data.u.ipv4 = ((struct sockaddr_in *)&addr)->sin_addr;
	smp->flags = 0;
	return 1;
}

static int smp_fetch_url_port(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct http_txn *txn;
	struct sockaddr_storage addr;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;
		sl = http_get_stline(htx);
		url2sa(HTX_SL_REQ_UPTR(sl), HTX_SL_REQ_ULEN(sl), &addr, NULL);
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		url2sa(ci_head(chn) + txn->req.sl.rq.u, txn->req.sl.rq.u_l, &addr, NULL);
	}
	if (((struct sockaddr_in *)&addr)->sin_family != AF_INET)
		return 0;

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = ntohs(((struct sockaddr_in *)&addr)->sin_port);
	smp->flags = 0;
	return 1;
}

/* Fetch an HTTP header. A pointer to the beginning of the value is returned.
 * Accepts an optional argument of type string containing the header field name,
 * and an optional argument of type signed or unsigned integer to request an
 * explicit occurrence of the header. Note that in the event of a missing name,
 * headers are considered from the first one. It does not stop on commas and
 * returns full lines instead (useful for User-Agent or Date for example).
 */
static int smp_fetch_fhdr(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.fhdr, res.fhdr */
	struct channel *chn = ((kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	int occ = 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx *ctx = smp->ctx.a[0];
		struct ist name;

		if (!ctx) {
			/* first call */
			ctx = &static_http_hdr_ctx;
			ctx->blk = NULL;
			smp->ctx.a[0] = ctx;
		}

		if (args) {
			if (args[0].type != ARGT_STR)
				return 0;
			name.ptr = args[0].data.str.area;
			name.len = args[0].data.str.data;

			if (args[1].type == ARGT_SINT)
				occ = args[1].data.sint;
		}

		if (!htx)
			return 0;

		if (ctx && !(smp->flags & SMP_F_NOT_LAST))
			/* search for header from the beginning */
			ctx->blk = NULL;

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last header by default */
			occ = -1;

		if (!occ)
			/* prepare to report multiple occurrences for ACL fetches */
			smp->flags |= SMP_F_NOT_LAST;

		smp->data.type = SMP_T_STR;
		smp->flags |= SMP_F_VOL_HDR | SMP_F_CONST;
		if (http_get_htx_fhdr(htx, name, occ, ctx, &smp->data.u.str.area, &smp->data.u.str.data))
			return 1;
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx *ctx = smp->ctx.a[0];
		const struct http_msg *msg;
		const char *name_str = NULL;
		int name_len = 0;

		if (!ctx) {
			/* first call */
			ctx = &static_hdr_ctx;
			ctx->idx = 0;
			smp->ctx.a[0] = ctx;
		}

		if (args) {
			if (args[0].type != ARGT_STR)
				return 0;
			name_str = args[0].data.str.area;
			name_len = args[0].data.str.data;

			if (args[1].type == ARGT_SINT)
				occ = args[1].data.sint;
		}

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		msg = (!(chn->flags & CF_ISRESP) ? &smp->strm->txn->req : &smp->strm->txn->rsp);

		if (ctx && !(smp->flags & SMP_F_NOT_LAST))
			/* search for header from the beginning */
			ctx->idx = 0;

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last header by default */
			occ = -1;

		if (!occ)
			/* prepare to report multiple occurrences for ACL fetches */
			smp->flags |= SMP_F_NOT_LAST;

		smp->data.type = SMP_T_STR;
		smp->flags |= SMP_F_VOL_HDR | SMP_F_CONST;
		if (http_get_fhdr(msg, name_str, name_len, idx, occ, ctx, &smp->data.u.str.area, &smp->data.u.str.data))
			return 1;
	}
	smp->flags &= ~SMP_F_NOT_LAST;
	return 0;
}

/* 6. Check on HTTP header count. The number of occurrences is returned.
 * Accepts exactly 1 argument of type string. It does not stop on commas and
 * returns full lines instead (useful for User-Agent or Date for example).
 */
static int smp_fetch_fhdr_cnt(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.fhdr_cnt, res.fhdr_cnt */
	struct channel *chn = ((kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	int cnt;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx ctx;
		struct ist name;

		if (!htx)
			return 0;

		if (args && args->type == ARGT_STR) {
			name.ptr = args->data.str.area;
			name.len = args->data.str.data;
		} else {
			name.ptr = NULL;
			name.len = 0;
		}

		ctx.blk = NULL;
		cnt = 0;
		while (http_find_header(htx, name, &ctx, 1))
			cnt++;
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx ctx;
		const struct http_msg *msg;
		const char *name = NULL;
		int len = 0;

		if (args && args->type == ARGT_STR) {
			name = args->data.str.area;
			len = args->data.str.data;
		}

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		msg = (!(chn->flags & CF_ISRESP) ? &smp->strm->txn->req : &smp->strm->txn->rsp);

		ctx.idx = 0;
		cnt = 0;
		while (http_find_full_header2(name, len, ci_head(msg->chn), idx, &ctx))
			cnt++;
	}

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = cnt;
	smp->flags = SMP_F_VOL_HDR;
	return 1;
}

static int smp_fetch_hdr_names(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.hdr_names, res.hdr_names */
	struct channel *chn = ((kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	struct buffer *temp;
	char del = ',';

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		int32_t pos;

		if (!htx)
			return 0;

		if (args && args->type == ARGT_STR)
			del = *args[0].data.str.area;

		temp = get_trash_chunk();
		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);
			struct ist n;

			if (type == HTX_BLK_EOH)
				break;
			if (type != HTX_BLK_HDR)
				continue;
			n = htx_get_blk_name(htx, blk);

			if (temp->data)
				temp->area[temp->data++] = del;
			chunk_memcat(temp, n.ptr, n.len);
		}
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx ctx;
		const struct http_msg *msg;

		if (args && args->type == ARGT_STR)
			del = *args[0].data.str.area;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		msg = (!(chn->flags & CF_ISRESP) ? &smp->strm->txn->req : &smp->strm->txn->rsp);

		temp = get_trash_chunk();

		ctx.idx = 0;
		while (http_find_next_header(ci_head(msg->chn), idx, &ctx)) {
			if (temp->data)
				temp->area[temp->data++] = del;
			memcpy(temp->area + temp->data, ctx.line, ctx.del);
			temp->data += ctx.del;
		}
	}

	smp->data.type = SMP_T_STR;
	smp->data.u.str = *temp;
	smp->flags = SMP_F_VOL_HDR;
	return 1;
}

/* Fetch an HTTP header. A pointer to the beginning of the value is returned.
 * Accepts an optional argument of type string containing the header field name,
 * and an optional argument of type signed or unsigned integer to request an
 * explicit occurrence of the header. Note that in the event of a missing name,
 * headers are considered from the first one.
 */
static int smp_fetch_hdr(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.hdr / hdr, res.hdr / shdr */
	struct channel *chn = ((kw[0] == 'h' || kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	int occ = 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx *ctx = smp->ctx.a[0];
		struct ist name;

		if (!ctx) {
			/* first call */
			ctx = &static_http_hdr_ctx;
			ctx->blk = NULL;
			smp->ctx.a[0] = ctx;
		}

		if (args) {
			if (args[0].type != ARGT_STR)
				return 0;
			name.ptr = args[0].data.str.area;
			name.len = args[0].data.str.data;

			if (args[1].type == ARGT_SINT)
				occ = args[1].data.sint;
		}

		if (!htx)
			return 0;

		if (ctx && !(smp->flags & SMP_F_NOT_LAST))
			/* search for header from the beginning */
			ctx->blk = NULL;

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last header by default */
			occ = -1;

		if (!occ)
			/* prepare to report multiple occurrences for ACL fetches */
			smp->flags |= SMP_F_NOT_LAST;

		smp->data.type = SMP_T_STR;
		smp->flags |= SMP_F_VOL_HDR | SMP_F_CONST;
		if (http_get_htx_hdr(htx, name, occ, ctx, &smp->data.u.str.area, &smp->data.u.str.data))
			return 1;
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx *ctx = smp->ctx.a[0];
		const struct http_msg *msg;
		const char *name_str = NULL;
		int name_len = 0;

		if (!ctx) {
			/* first call */
			ctx = &static_hdr_ctx;
			ctx->idx = 0;
			smp->ctx.a[0] = ctx;
		}

		if (args) {
			if (args[0].type != ARGT_STR)
				return 0;
			name_str = args[0].data.str.area;
			name_len = args[0].data.str.data;

			if (args[1].type == ARGT_SINT)
				occ = args[1].data.sint;
		}

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		msg = (!(chn->flags & CF_ISRESP) ? &smp->strm->txn->req : &smp->strm->txn->rsp);

		if (ctx && !(smp->flags & SMP_F_NOT_LAST))
			/* search for header from the beginning */
			ctx->idx = 0;

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last header by default */
			occ = -1;

		if (!occ)
			/* prepare to report multiple occurrences for ACL fetches */
			smp->flags |= SMP_F_NOT_LAST;

		smp->data.type = SMP_T_STR;
		smp->flags |= SMP_F_VOL_HDR | SMP_F_CONST;
		if (http_get_hdr(msg, name_str, name_len, idx, occ, ctx, &smp->data.u.str.area, &smp->data.u.str.data))
			return 1;
	}

	smp->flags &= ~SMP_F_NOT_LAST;
	return 0;
}

/* Same than smp_fetch_hdr() but only relies on the sample direction to choose
 * the right channel. So instead of duplicating the code, we just change the
 * keyword and then fallback on smp_fetch_hdr().
 */
static int smp_fetch_chn_hdr(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	kw = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ ? "req.hdr" : "res.hdr");
	return smp_fetch_hdr(args, smp, kw, private);
}

/* 6. Check on HTTP header count. The number of occurrences is returned.
 * Accepts exactly 1 argument of type string.
 */
static int smp_fetch_hdr_cnt(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.hdr_cnt / hdr_cnt, res.hdr_cnt / shdr_cnt */
	struct channel *chn = ((kw[0] == 'h' || kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	int cnt;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx ctx;
		struct ist name;

		if (!htx)
			return 0;

		if (args && args->type == ARGT_STR) {
			name.ptr = args->data.str.area;
			name.len = args->data.str.data;
		} else {
			name.ptr = NULL;
			name.len = 0;
		}

		ctx.blk = NULL;
		cnt = 0;
		while (http_find_header(htx, name, &ctx, 0))
			cnt++;
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx ctx;
		const struct http_msg *msg;
		const char *name = NULL;
		int len = 0;

		if (args && args->type == ARGT_STR) {
			name = args->data.str.area;
			len = args->data.str.data;
		}

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		msg = (!(chn->flags & CF_ISRESP) ? &smp->strm->txn->req : &smp->strm->txn->rsp);

		ctx.idx = 0;
		cnt = 0;
		while (http_find_header2(name, len, ci_head(msg->chn), idx, &ctx))
			cnt++;
	}

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = cnt;
	smp->flags = SMP_F_VOL_HDR;
	return 1;
}

/* Fetch an HTTP header's integer value. The integer value is returned. It
 * takes a mandatory argument of type string and an optional one of type int
 * to designate a specific occurrence. It returns an unsigned integer, which
 * may or may not be appropriate for everything.
 */
static int smp_fetch_hdr_val(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int ret = smp_fetch_hdr(args, smp, kw, private);

	if (ret > 0) {
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = strl2ic(smp->data.u.str.area,
					   smp->data.u.str.data);
	}

	return ret;
}

/* Fetch an HTTP header's IP value. takes a mandatory argument of type string
 * and an optional one of type int to designate a specific occurrence.
 * It returns an IPv4 or IPv6 address.
 */
static int smp_fetch_hdr_ip(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int ret;

	while ((ret = smp_fetch_hdr(args, smp, kw, private)) > 0) {
		if (url2ipv4((char *) smp->data.u.str.area, &smp->data.u.ipv4)) {
			smp->data.type = SMP_T_IPV4;
			break;
		} else {
			struct buffer *temp = get_trash_chunk();
			if (smp->data.u.str.data < temp->size - 1) {
				memcpy(temp->area, smp->data.u.str.area,
				       smp->data.u.str.data);
				temp->area[smp->data.u.str.data] = '\0';
				if (inet_pton(AF_INET6, temp->area, &smp->data.u.ipv6)) {
					smp->data.type = SMP_T_IPV6;
					break;
				}
			}
		}

		/* if the header doesn't match an IP address, fetch next one */
		if (!(smp->flags & SMP_F_NOT_LAST))
			return 0;
	}
	return ret;
}

/* 8. Check on URI PATH. A pointer to the PATH is stored. The path starts at
 * the first '/' after the possible hostname, and ends before the possible '?'.
 */
static int smp_fetch_path(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;
		struct ist path;
		size_t len;

		if (!htx)
			return 0;

		sl = http_get_stline(htx);
		path = http_get_path(htx_sl_req_uri(sl));
		if (!path.ptr)
			return 0;

		for (len = 0; len < path.len && *(path.ptr + len) != '?'; len++)
			;

		/* OK, we got the '/' ! */
		smp->data.type = SMP_T_STR;
		smp->data.u.str.area = path.ptr;
		smp->data.u.str.data = len;
		smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	}
	else {
		struct http_txn *txn;
		char *ptr, *end;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		end = ci_head(chn) + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
		ptr = http_txn_get_path(txn);
		if (!ptr)
			return 0;

		/* OK, we got the '/' ! */
		smp->data.type = SMP_T_STR;
		smp->data.u.str.area = ptr;

		while (ptr < end && *ptr != '?')
			ptr++;

		smp->data.u.str.data = ptr - smp->data.u.str.area;
		smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	}
	return 1;
}

/* This produces a concatenation of the first occurrence of the Host header
 * followed by the path component if it begins with a slash ('/'). This means
 * that '*' will not be added, resulting in exactly the first Host entry.
 * If no Host header is found, then the path is returned as-is. The returned
 * value is stored in the trash so it does not need to be marked constant.
 * The returned sample is of type string.
 */
static int smp_fetch_base(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	struct buffer *temp;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;
		struct http_hdr_ctx ctx;
		struct ist path;

		if (!htx)
			return 0;

		ctx.blk = NULL;
		if (!http_find_header(htx, ist("Host"), &ctx, 0) || !ctx.value.len)
			return smp_fetch_path(args, smp, kw, private);

		/* OK we have the header value in ctx.value */
		temp = get_trash_chunk();
		chunk_memcat(temp, ctx.value.ptr, ctx.value.len);

		/* now retrieve the path */
		sl = http_get_stline(htx);
		path = http_get_path(htx_sl_req_uri(sl));
		if (path.ptr) {
			size_t len;

			for (len = 0; len < path.len && *(path.ptr + len) != '?'; len++)
				;

			if (len && *(path.ptr) == '/')
				chunk_memcat(temp, path.ptr, len);
		}

		smp->data.type = SMP_T_STR;
		smp->data.u.str = *temp;
	}
	else {
		/* LEGACY version */
		struct http_txn *txn;
		char *ptr, *end, *beg;
		struct hdr_ctx ctx;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		ctx.idx = 0;
		if (!http_find_header2("Host", 4, ci_head(chn), &txn->hdr_idx, &ctx) || !ctx.vlen)
			return smp_fetch_path(args, smp, kw, private);

		/* OK we have the header value in ctx.line+ctx.val for ctx.vlen bytes */
		temp = get_trash_chunk();
		memcpy(temp->area, ctx.line + ctx.val, ctx.vlen);
		smp->data.type = SMP_T_STR;
		smp->data.u.str.area = temp->area;
		smp->data.u.str.data = ctx.vlen;

		/* now retrieve the path */
		end = ci_head(chn) + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
		beg = http_txn_get_path(txn);
		if (!beg)
			beg = end;

		for (ptr = beg; ptr < end && *ptr != '?'; ptr++);

		if (beg < ptr && *beg == '/') {
			memcpy(smp->data.u.str.area + smp->data.u.str.data, beg,
			       ptr - beg);
			smp->data.u.str.data += ptr - beg;
		}
	}

	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This produces a 32-bit hash of the concatenation of the first occurrence of
 * the Host header followed by the path component if it begins with a slash ('/').
 * This means that '*' will not be added, resulting in exactly the first Host
 * entry. If no Host header is found, then the path is used. The resulting value
 * is hashed using the path hash followed by a full avalanche hash and provides a
 * 32-bit integer value. This fetch is useful for tracking per-path activity on
 * high-traffic sites without having to store whole paths.
 */
static int smp_fetch_base32(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	unsigned int hash = 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;
		struct http_hdr_ctx ctx;
		struct ist path;

		if (!htx)
			return 0;

		ctx.blk = NULL;
		if (http_find_header(htx, ist("Host"), &ctx, 0)) {
			/* OK we have the header value in ctx.value */
			while (ctx.value.len--)
				hash = *(ctx.value.ptr++) + (hash << 6) + (hash << 16) - hash;
		}

		/* now retrieve the path */
		sl = http_get_stline(htx);
		path = http_get_path(htx_sl_req_uri(sl));
		if (path.ptr) {
			size_t len;

			for (len = 0; len < path.len && *(path.ptr + len) != '?'; len++)
				;

			if (len && *(path.ptr) == '/') {
				while (len--)
					hash = *(path.ptr++) + (hash << 6) + (hash << 16) - hash;
			}
		}
	}
	else {
		/* LEGACY version */
		struct http_txn *txn;
		struct hdr_ctx ctx;
		char *ptr, *beg, *end;
		int len;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		ctx.idx = 0;
		if (http_find_header2("Host", 4, ci_head(chn), &txn->hdr_idx, &ctx)) {
			/* OK we have the header value in ctx.line+ctx.val for ctx.vlen bytes */
			ptr = ctx.line + ctx.val;
			len = ctx.vlen;
			while (len--)
				hash = *(ptr++) + (hash << 6) + (hash << 16) - hash;
		}

		/* now retrieve the path */
		end = ci_head(chn) + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
		beg = http_txn_get_path(txn);
		if (!beg)
			beg = end;

		for (ptr = beg; ptr < end && *ptr != '?'; ptr++);

		if (beg < ptr && *beg == '/') {
			while (beg < ptr)
				hash = *(beg++) + (hash << 6) + (hash << 16) - hash;
		}
	}

	hash = full_hash(hash);

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = hash;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This concatenates the source address with the 32-bit hash of the Host and
 * path as returned by smp_fetch_base32(). The idea is to have per-source and
 * per-path counters. The result is a binary block from 8 to 20 bytes depending
 * on the source address length. The path hash is stored before the address so
 * that in environments where IPv6 is insignificant, truncating the output to
 * 8 bytes would still work.
 */
static int smp_fetch_base32_src(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct buffer *temp;
	struct connection *cli_conn = objt_conn(smp->sess->origin);

	if (!cli_conn)
		return 0;

	if (!smp_fetch_base32(args, smp, kw, private))
		return 0;

	temp = get_trash_chunk();
	*(unsigned int *) temp->area = htonl(smp->data.u.sint);
	temp->data += sizeof(unsigned int);

	switch (cli_conn->addr.from.ss_family) {
	case AF_INET:
		memcpy(temp->area + temp->data,
		       &((struct sockaddr_in *)&cli_conn->addr.from)->sin_addr,
		       4);
		temp->data += 4;
		break;
	case AF_INET6:
		memcpy(temp->area + temp->data,
		       &((struct sockaddr_in6 *)&cli_conn->addr.from)->sin6_addr,
		       16);
		temp->data += 16;
		break;
	default:
		return 0;
	}

	smp->data.u.str = *temp;
	smp->data.type = SMP_T_BIN;
	return 1;
}

/* Extracts the query string, which comes after the question mark '?'. If no
 * question mark is found, nothing is returned. Otherwise it returns a sample
 * of type string carrying the whole query string.
 */
static int smp_fetch_query(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	char *ptr, *end;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct htx_sl *sl;

		if (!htx)
			return 0;

		sl = http_get_stline(htx);
		ptr = HTX_SL_REQ_UPTR(sl);
		end = HTX_SL_REQ_UPTR(sl) + HTX_SL_REQ_ULEN(sl);
	}
	else {
		/* LEGACY version */
		struct http_txn *txn;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		ptr = ci_head(chn) + txn->req.sl.rq.u;
		end = ptr + txn->req.sl.rq.u_l;
	}

	/* look up the '?' */
	do {
		if (ptr == end)
			return 0;
	} while (*ptr++ != '?');

	smp->data.type = SMP_T_STR;
	smp->data.u.str.area = ptr;
	smp->data.u.str.data = end - ptr;
	smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	return 1;
}

static int smp_fetch_proto_http(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 0);

		if (!htx)
			return 0;
	}
	else {
		/* LEGACY version */

		/* Note: hdr_idx.v cannot be NULL in this ACL because the ACL is tagged
		 * as a layer7 ACL, which involves automatic allocation of hdr_idx.
		 */
		CHECK_HTTP_MESSAGE_FIRST_PERM(chn);
	}
		smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = 1;
	return 1;
}

/* return a valid test if the current request is the first one on the connection */
static int smp_fetch_http_first_req(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = !(smp->strm->txn->flags & TX_NOT_FIRST);
	return 1;
}

/* Accepts exactly 1 argument of type userlist */
static int smp_fetch_http_auth(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (!args || args->type != ARGT_USR)
		return 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);

		if (!htx)
			return 0;
		if (!get_http_auth(smp, htx))
			return 0;
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);
		if (!get_http_auth(smp, NULL))
			return 0;
	}

	smp->data.type = SMP_T_BOOL;
	smp->data.u.sint = check_user(args->data.usr, smp->strm->txn->auth.user,
				      smp->strm->txn->auth.pass);
	return 1;
}

/* Accepts exactly 1 argument of type userlist */
static int smp_fetch_http_auth_grp(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);

	if (!args || args->type != ARGT_USR)
		return 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);

		if (!htx)
			return 0;
		if (!get_http_auth(smp, htx))
			return 0;
	}
	else {
		/* LEGACY version */
		CHECK_HTTP_MESSAGE_FIRST(chn);
		if (!get_http_auth(smp, NULL))
			return 0;
	}

	/* if the user does not belong to the userlist or has a wrong password,
	 * report that it unconditionally does not match. Otherwise we return
	 * a string containing the username.
	 */
	if (!check_user(args->data.usr, smp->strm->txn->auth.user,
	                smp->strm->txn->auth.pass))
		return 0;

	/* pat_match_auth() will need the user list */
	smp->ctx.a[0] = args->data.usr;

	smp->data.type = SMP_T_STR;
	smp->flags = SMP_F_CONST;
	smp->data.u.str.area = smp->strm->txn->auth.user;
	smp->data.u.str.data = strlen(smp->strm->txn->auth.user);

	return 1;
}

/* Fetch a captured HTTP request header. The index is the position of
 * the "capture" option in the configuration file
 */
static int smp_fetch_capture_req_hdr(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct proxy *fe = strm_fe(smp->strm);
	int idx;

	if (!args || args->type != ARGT_SINT)
		return 0;

	idx = args->data.sint;

	if (idx > (fe->nb_req_cap - 1) || smp->strm->req_cap == NULL || smp->strm->req_cap[idx] == NULL)
		return 0;

	smp->data.type = SMP_T_STR;
	smp->flags |= SMP_F_CONST;
	smp->data.u.str.area = smp->strm->req_cap[idx];
	smp->data.u.str.data = strlen(smp->strm->req_cap[idx]);

	return 1;
}

/* Fetch a captured HTTP response header. The index is the position of
 * the "capture" option in the configuration file
 */
static int smp_fetch_capture_res_hdr(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct proxy *fe = strm_fe(smp->strm);
	int idx;

	if (!args || args->type != ARGT_SINT)
		return 0;

	idx = args->data.sint;

	if (idx > (fe->nb_rsp_cap - 1) || smp->strm->res_cap == NULL || smp->strm->res_cap[idx] == NULL)
		return 0;

	smp->data.type = SMP_T_STR;
	smp->flags |= SMP_F_CONST;
	smp->data.u.str.area = smp->strm->res_cap[idx];
	smp->data.u.str.data = strlen(smp->strm->res_cap[idx]);

	return 1;
}

/* Extracts the METHOD in the HTTP request, the txn->uri should be filled before the call */
static int smp_fetch_capture_req_method(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct buffer *temp;
	struct http_txn *txn = smp->strm->txn;
	char *ptr;

	if (!txn || !txn->uri)
		return 0;

	ptr = txn->uri;

	while (*ptr != ' ' && *ptr != '\0')  /* find first space */
		ptr++;

	temp = get_trash_chunk();
	temp->area = txn->uri;
	temp->data = ptr - txn->uri;
	smp->data.u.str = *temp;
	smp->data.type = SMP_T_STR;
	smp->flags = SMP_F_CONST;

	return 1;

}

/* Extracts the path in the HTTP request, the txn->uri should be filled before the call  */
static int smp_fetch_capture_req_uri(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct http_txn *txn = smp->strm->txn;
	struct ist path;
	const char *ptr;

	if (!txn || !txn->uri)
		return 0;

	ptr = txn->uri;

	while (*ptr != ' ' && *ptr != '\0')  /* find first space */
		ptr++;

	if (!*ptr)
		return 0;

	/* skip the first space and find space after URI */
	path = ist2(++ptr, 0);
	while (*ptr != ' ' && *ptr != '\0')
		ptr++;
	path.len = ptr - path.ptr;

	path = http_get_path(path);
	if (!path.ptr)
		return 0;

	smp->data.u.str.area = path.ptr;
	smp->data.u.str.data = path.len;
	smp->data.type = SMP_T_STR;
	smp->flags = SMP_F_CONST;

	return 1;
}

/* Retrieves the HTTP version from the request (either 1.0 or 1.1) and emits it
 * as a string (either "HTTP/1.0" or "HTTP/1.1").
 */
static int smp_fetch_capture_req_ver(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct http_txn *txn = smp->strm->txn;

	if (!txn || txn->req.msg_state < HTTP_MSG_HDR_FIRST)
		return 0;

	if (txn->req.flags & HTTP_MSGF_VER_11)
		smp->data.u.str.area = "HTTP/1.1";
	else
		smp->data.u.str.area = "HTTP/1.0";

	smp->data.u.str.data = 8;
	smp->data.type  = SMP_T_STR;
	smp->flags = SMP_F_CONST;
	return 1;

}

/* Retrieves the HTTP version from the response (either 1.0 or 1.1) and emits it
 * as a string (either "HTTP/1.0" or "HTTP/1.1").
 */
static int smp_fetch_capture_res_ver(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct http_txn *txn = smp->strm->txn;

	if (!txn || txn->rsp.msg_state < HTTP_MSG_HDR_FIRST)
		return 0;

	if (txn->rsp.flags & HTTP_MSGF_VER_11)
		smp->data.u.str.area = "HTTP/1.1";
	else
		smp->data.u.str.area = "HTTP/1.0";

	smp->data.u.str.data = 8;
	smp->data.type  = SMP_T_STR;
	smp->flags = SMP_F_CONST;
	return 1;

}

/* Iterate over all cookies present in a message. The context is stored in
 * smp->ctx.a[0] for the in-header position, smp->ctx.a[1] for the
 * end-of-header-value, and smp->ctx.a[2] for the hdr_ctx. Depending on
 * the direction, multiple cookies may be parsed on the same line or not.
 * The cookie name is in args and the name length in args->data.str.len.
 * Accepts exactly 1 argument of type string. If the input options indicate
 * that no iterating is desired, then only last value is fetched if any.
 * The returned sample is of type CSTR. Can be used to parse cookies in other
 * files.
 */
static int smp_fetch_cookie(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.cookie / cookie / cook, res.cookie / scook / set-cookie */
	struct channel *chn = ((kw[0] == 'c' || kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	int occ = 0;
	int found = 0;

	if (!args || args->type != ARGT_STR)
		return 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx *ctx = smp->ctx.a[2];
		struct ist hdr;

		if (!ctx) {
			/* first call */
			ctx = &static_http_hdr_ctx;
			ctx->blk = NULL;
			smp->ctx.a[2] = ctx;
		}

		if (!htx)
			return 0;

		hdr = (!(chn->flags & CF_ISRESP) ? ist("Cookie") : ist("Set-Cookie"));

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last cookie by default */
			occ = -1;

		/* OK so basically here, either we want only one value and it's the
		 * last one, or we want to iterate over all of them and we fetch the
		 * next one.
		 */

		if (!(smp->flags & SMP_F_NOT_LAST)) {
			/* search for the header from the beginning, we must first initialize
			 * the search parameters.
			 */
			smp->ctx.a[0] = NULL;
			ctx->blk = NULL;
		}

		smp->flags |= SMP_F_VOL_HDR;
		while (1) {
			/* Note: smp->ctx.a[0] == NULL every time we need to fetch a new header */
			if (!smp->ctx.a[0]) {
				if (!http_find_header(htx, hdr, ctx, 0))
					goto out;

				if (ctx->value.len < args->data.str.data + 1)
					continue;

				smp->ctx.a[0] = ctx->value.ptr;
				smp->ctx.a[1] = smp->ctx.a[0] + ctx->value.len;
			}

			smp->data.type = SMP_T_STR;
			smp->flags |= SMP_F_CONST;
			smp->ctx.a[0] = http_extract_cookie_value(smp->ctx.a[0], smp->ctx.a[1],
								  args->data.str.area, args->data.str.data,
								  (smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
								  &smp->data.u.str.area,
								  &smp->data.u.str.data);
			if (smp->ctx.a[0]) {
				found = 1;
				if (occ >= 0) {
					/* one value was returned into smp->data.u.str.{str,len} */
					smp->flags |= SMP_F_NOT_LAST;
					return 1;
				}
			}
			/* if we're looking for last occurrence, let's loop */
		}
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx *ctx = smp->ctx.a[2];
		const char *hdr_name;
		int hdr_name_len;
		char *sol;

		if (!ctx) {
			/* first call */
			ctx = &static_hdr_ctx;
			ctx->idx = 0;
			smp->ctx.a[2] = ctx;
		}

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		if (!(chn->flags & CF_ISRESP)) {
			hdr_name = "Cookie";
			hdr_name_len = 6;
		} else {
			hdr_name = "Set-Cookie";
			hdr_name_len = 10;
		}

		if (!occ && !(smp->opt & SMP_OPT_ITERATE))
			/* no explicit occurrence and single fetch => last cookie by default */
			occ = -1;

		/* OK so basically here, either we want only one value and it's the
		 * last one, or we want to iterate over all of them and we fetch the
		 * next one.
		 */

		sol = ci_head(chn);
		if (!(smp->flags & SMP_F_NOT_LAST)) {
			/* search for the header from the beginning, we must first initialize
			 * the search parameters.
			 */
			smp->ctx.a[0] = NULL;
			ctx->idx = 0;
		}

		smp->flags |= SMP_F_VOL_HDR;

		while (1) {
			/* Note: smp->ctx.a[0] == NULL every time we need to fetch a new header */
			if (!smp->ctx.a[0]) {
				if (!http_find_header2(hdr_name, hdr_name_len, sol, idx, ctx))
					goto out;

				if (ctx->vlen < args->data.str.data + 1)
					continue;

				smp->ctx.a[0] = ctx->line + ctx->val;
				smp->ctx.a[1] = smp->ctx.a[0] + ctx->vlen;
			}

			smp->data.type = SMP_T_STR;
			smp->flags |= SMP_F_CONST;
			smp->ctx.a[0] = http_extract_cookie_value(smp->ctx.a[0], smp->ctx.a[1],
								  args->data.str.area, args->data.str.data,
								  (smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
								  &smp->data.u.str.area, &smp->data.u.str.data);
			if (smp->ctx.a[0]) {
				found = 1;
				if (occ >= 0) {
					/* one value was returned into smp->data.u.str.{str,len} */
					smp->flags |= SMP_F_NOT_LAST;
					return 1;
				}
			}
			/* if we're looking for last occurrence, let's loop */
		}
	}
	/* all cookie headers and values were scanned. If we're looking for the
	 * last occurrence, we may return it now.
	 */
  out:
	smp->flags &= ~SMP_F_NOT_LAST;
	return found;
}

/* Same than smp_fetch_cookie() but only relies on the sample direction to
 * choose the right channel. So instead of duplicating the code, we just change
 * the keyword and then fallback on smp_fetch_cookie().
 */
static int smp_fetch_chn_cookie(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	kw = ((smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ ? "req.cook" : "res.cook");
	return smp_fetch_cookie(args, smp, kw, private);
}

/* Iterate over all cookies present in a request to count how many occurrences
 * match the name in args and args->data.str.len. If <multi> is non-null, then
 * multiple cookies may be parsed on the same line. The returned sample is of
 * type UINT. Accepts exactly 1 argument of type string.
 */
static int smp_fetch_cookie_cnt(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	/* possible keywords: req.cook_cnt / cook_cnt, res.cook_cnt / scook_cnt */
	struct channel *chn = ((kw[0] == 'c' || kw[2] == 'q') ? SMP_REQ_CHN(smp) : SMP_RES_CHN(smp));
	char *val_beg, *val_end;
	int cnt;

	if (!args || args->type != ARGT_STR)
		return 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx ctx;
		struct ist hdr;

		if (!htx)
			return 0;

		hdr = (!(chn->flags & CF_ISRESP) ? ist("Cookie") : ist("Set-Cookie"));

		val_end = val_beg = NULL;
		ctx.blk = NULL;
		cnt = 0;
		while (1) {
			/* Note: val_beg == NULL every time we need to fetch a new header */
			if (!val_beg) {
				if (!http_find_header(htx, hdr, &ctx, 0))
					break;

				if (ctx.value.len < args->data.str.data + 1)
					continue;

				val_beg = ctx.value.ptr;
				val_end = val_beg + ctx.value.len;
			}

			smp->data.type = SMP_T_STR;
			smp->flags |= SMP_F_CONST;
			while ((val_beg = http_extract_cookie_value(val_beg, val_end,
								    args->data.str.area, args->data.str.data,
								    (smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
								    &smp->data.u.str.area,
								    &smp->data.u.str.data))) {
				cnt++;
			}
		}
	}
	else {
		/* LEGACY version */
		struct hdr_idx *idx;
		struct hdr_ctx ctx;
		const char *hdr_name;
		int hdr_name_len;
		char *sol;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		idx = &smp->strm->txn->hdr_idx;
		if (!(chn->flags & CF_ISRESP)) {
			hdr_name = "Cookie";
			hdr_name_len = 6;
		} else {
			hdr_name = "Set-Cookie";
			hdr_name_len = 10;
		}

		sol = ci_head(chn);
		val_end = val_beg = NULL;
		ctx.idx = 0;
		cnt = 0;

		while (1) {
			/* Note: val_beg == NULL every time we need to fetch a new header */
			if (!val_beg) {
				if (!http_find_header2(hdr_name, hdr_name_len, sol, idx, &ctx))
					break;

				if (ctx.vlen < args->data.str.data + 1)
					continue;

				val_beg = ctx.line + ctx.val;
				val_end = val_beg + ctx.vlen;
			}

			smp->data.type = SMP_T_STR;
			smp->flags |= SMP_F_CONST;
			while ((val_beg = http_extract_cookie_value(val_beg, val_end,
								    args->data.str.area, args->data.str.data,
								    (smp->opt & SMP_OPT_DIR) == SMP_OPT_DIR_REQ,
								    &smp->data.u.str.area, &smp->data.u.str.data))) {
				cnt++;
			}
		}
	}

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = cnt;
	smp->flags |= SMP_F_VOL_HDR;
	return 1;
}

/* Fetch an cookie's integer value. The integer value is returned. It
 * takes a mandatory argument of type string. It relies on smp_fetch_cookie().
 */
static int smp_fetch_cookie_val(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int ret = smp_fetch_cookie(args, smp, kw, private);

	if (ret > 0) {
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = strl2ic(smp->data.u.str.area,
					   smp->data.u.str.data);
	}

	return ret;
}

/************************************************************************/
/*           The code below is dedicated to sample fetches              */
/************************************************************************/

/* This scans a URL-encoded query string. It takes an optionally wrapping
 * string whose first contigous chunk has its beginning in ctx->a[0] and end
 * in ctx->a[1], and the optional second part in (ctx->a[2]..ctx->a[3]). The
 * pointers are updated for next iteration before leaving.
 */
static int smp_fetch_param(char delim, const char *name, int name_len, const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	const char *vstart, *vend;
	struct buffer *temp;
	const char **chunks = (const char **)smp->ctx.a;

	if (!http_find_next_url_param(chunks, name, name_len,
	                         &vstart, &vend, delim))
		return 0;

	/* Create sample. If the value is contiguous, return the pointer as CONST,
	 * if the value is wrapped, copy-it in a buffer.
	 */
	smp->data.type = SMP_T_STR;
	if (chunks[2] &&
	    vstart >= chunks[0] && vstart <= chunks[1] &&
	    vend >= chunks[2] && vend <= chunks[3]) {
		/* Wrapped case. */
		temp = get_trash_chunk();
		memcpy(temp->area, vstart, chunks[1] - vstart);
		memcpy(temp->area + ( chunks[1] - vstart ), chunks[2],
		       vend - chunks[2]);
		smp->data.u.str.area = temp->area;
		smp->data.u.str.data = ( chunks[1] - vstart ) + ( vend - chunks[2] );
	} else {
		/* Contiguous case. */
		smp->data.u.str.area = (char *)vstart;
		smp->data.u.str.data = vend - vstart;
		smp->flags = SMP_F_VOL_1ST | SMP_F_CONST;
	}

	/* Update context, check wrapping. */
	chunks[0] = vend;
	if (chunks[2] && vend >= chunks[2] && vend <= chunks[3]) {
		chunks[1] = chunks[3];
		chunks[2] = NULL;
	}

	if (chunks[0] < chunks[1])
		smp->flags |= SMP_F_NOT_LAST;

	return 1;
}

/* This function iterates over each parameter of the query string. It uses
 * ctx->a[0] and ctx->a[1] to store the beginning and end of the current
 * parameter. Since it uses smp_fetch_param(), ctx->a[2..3] are both NULL.
 * An optional parameter name is passed in args[0], otherwise any parameter is
 * considered. It supports an optional delimiter argument for the beginning of
 * the string in args[1], which defaults to "?".
 */
static int smp_fetch_url_param(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	char delim = '?';
	const char *name;
	int name_len;

	if (!args ||
	    (args[0].type && args[0].type != ARGT_STR) ||
	    (args[1].type && args[1].type != ARGT_STR))
		return 0;

	name = "";
	name_len = 0;
	if (args->type == ARGT_STR) {
		name     = args->data.str.area;
		name_len = args->data.str.data;
	}

	if (args[1].type)
		delim = *args[1].data.str.area;

	if (!smp->ctx.a[0]) { // first call, find the query string
		if (smp->px->options2 & PR_O2_USE_HTX) {
			/* HTX version */
			struct htx *htx = smp_prefetch_htx(smp, chn, 1);
			struct htx_sl *sl;

			if (!htx)
				return 0;

			sl = http_get_stline(htx);
			smp->ctx.a[0] = http_find_param_list(HTX_SL_REQ_UPTR(sl), HTX_SL_REQ_ULEN(sl), delim);
			if (!smp->ctx.a[0])
				return 0;

			smp->ctx.a[1] = HTX_SL_REQ_UPTR(sl) + HTX_SL_REQ_ULEN(sl);
		}
		else {
			/* LEGACY version */
			struct http_msg *msg;

			CHECK_HTTP_MESSAGE_FIRST(chn);

			msg = &smp->strm->txn->req;

			smp->ctx.a[0] = http_find_param_list(ci_head(chn) + msg->sl.rq.u,
							     msg->sl.rq.u_l, delim);
			if (!smp->ctx.a[0])
				return 0;

			smp->ctx.a[1] = ci_head(chn) + msg->sl.rq.u + msg->sl.rq.u_l;
		}

		/* Assume that the context is filled with NULL pointer
		 * before the first call.
		 * smp->ctx.a[2] = NULL;
		 * smp->ctx.a[3] = NULL;
		 */
	}

	return smp_fetch_param(delim, name, name_len, args, smp, kw, private);
}

/* This function iterates over each parameter of the body. This requires
 * that the body has been waited for using http-buffer-request. It uses
 * ctx->a[0] and ctx->a[1] to store the beginning and end of the first
 * contigous part of the body, and optionally ctx->a[2..3] to reference the
 * optional second part if the body wraps at the end of the buffer. An optional
 * parameter name is passed in args[0], otherwise any parameter is considered.
 */
static int smp_fetch_body_param(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	const char *name;
	int name_len;

	if (!args || (args[0].type && args[0].type != ARGT_STR))
		return 0;

	name = "";
	name_len = 0;
	if (args[0].type == ARGT_STR) {
		name     = args[0].data.str.area;
		name_len = args[0].data.str.data;
	}

	if (!smp->ctx.a[0]) { // first call, find the query string
		if (smp->px->options2 & PR_O2_USE_HTX) {
			/* HTX version */
			struct htx *htx = smp_prefetch_htx(smp, chn, 1);
			struct buffer *temp;
			int32_t pos;

			if (!htx)
				return 0;

			temp   = get_trash_chunk();
			for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
				struct htx_blk   *blk  = htx_get_blk(htx, pos);
				enum htx_blk_type type = htx_get_blk_type(blk);

				if (type == HTX_BLK_EOM || type == HTX_BLK_TLR || type == HTX_BLK_EOT)
					break;
				if (type == HTX_BLK_DATA) {
					if (!htx_data_to_h1(htx_get_blk_value(htx, blk), temp, 0))
						return 0;
				}
			}

			smp->ctx.a[0] = temp->area;
			smp->ctx.a[1] = temp->area + temp->data;

			/* Assume that the context is filled with NULL pointer
			 * before the first call.
			 * smp->ctx.a[2] = NULL;
			 * smp->ctx.a[3] = NULL;
			 */
		}
		else {
			/* LEGACY version */
			struct http_msg *msg;
			unsigned long len;
			unsigned long block1;
			char *body;

			CHECK_HTTP_MESSAGE_FIRST(chn);

			msg = &smp->strm->txn->req;
			len  = http_body_bytes(msg);
			body = c_ptr(chn, -http_data_rewind(msg));

			block1 = len;
			if (block1 > b_wrap(&chn->buf) - body)
				block1 = b_wrap(&chn->buf) - body;

			if (block1 == len) {
				/* buffer is not wrapped (or empty) */
				smp->ctx.a[0] = body;
				smp->ctx.a[1] = body + len;

				/* Assume that the context is filled with NULL pointer
				 * before the first call.
				 * smp->ctx.a[2] = NULL;
				 * smp->ctx.a[3] = NULL;
				 */
			}
			else {
				/* buffer is wrapped, we need to defragment it */
				smp->ctx.a[0] = body;
				smp->ctx.a[1] = body + block1;
				smp->ctx.a[2] = b_orig(&chn->buf);
				smp->ctx.a[3] = b_orig(&chn->buf) + ( len - block1 );
			}
		}
	}

	return smp_fetch_param('&', name, name_len, args, smp, kw, private);
}

/* Return the signed integer value for the specified url parameter (see url_param
 * above).
 */
static int smp_fetch_url_param_val(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	int ret = smp_fetch_url_param(args, smp, kw, private);

	if (ret > 0) {
		smp->data.type = SMP_T_SINT;
		smp->data.u.sint = strl2ic(smp->data.u.str.area,
					   smp->data.u.str.data);
	}

	return ret;
}

/* This produces a 32-bit hash of the concatenation of the first occurrence of
 * the Host header followed by the path component if it begins with a slash ('/').
 * This means that '*' will not be added, resulting in exactly the first Host
 * entry. If no Host header is found, then the path is used. The resulting value
 * is hashed using the url hash followed by a full avalanche hash and provides a
 * 32-bit integer value. This fetch is useful for tracking per-URL activity on
 * high-traffic sites without having to store whole paths.
 * this differs from the base32 functions in that it includes the url parameters
 * as well as the path
 */
static int smp_fetch_url32(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct channel *chn = SMP_REQ_CHN(smp);
	unsigned int hash = 0;

	if (smp->px->options2 & PR_O2_USE_HTX) {
		/* HTX version */
		struct htx *htx = smp_prefetch_htx(smp, chn, 1);
		struct http_hdr_ctx ctx;
		struct htx_sl *sl;
		struct ist path;

		if (!htx)
			return 0;

		ctx.blk = NULL;
		if (http_find_header(htx, ist("Host"), &ctx, 1)) {
			/* OK we have the header value in ctx.value */
			while (ctx.value.len--)
				hash = *(ctx.value.ptr++) + (hash << 6) + (hash << 16) - hash;
		}

		/* now retrieve the path */
		sl = http_get_stline(htx);
		path = http_get_path(htx_sl_req_uri(sl));
		while (path.len > 0 && *(path.ptr) != '?') {
			path.ptr++;
			path.len--;
		}
		if (path.len && *(path.ptr) == '/') {
			while (path.len--)
				hash = *(path.ptr++) + (hash << 6) + (hash << 16) - hash;
		}
	}
	else {
		/* LEGACY version */
		struct http_txn *txn;
		struct hdr_ctx ctx;
		char *ptr, *beg, *end;
		int len;

		CHECK_HTTP_MESSAGE_FIRST(chn);

		txn = smp->strm->txn;
		ctx.idx = 0;
		if (http_find_header2("Host", 4, ci_head(chn), &txn->hdr_idx, &ctx)) {
			/* OK we have the header value in ctx.line+ctx.val for ctx.vlen bytes */
			ptr = ctx.line + ctx.val;
			len = ctx.vlen;
			while (len--)
				hash = *(ptr++) + (hash << 6) + (hash << 16) - hash;
		}

		/* now retrieve the path */
		end = ci_head(chn) + txn->req.sl.rq.u + txn->req.sl.rq.u_l;
		beg = http_txn_get_path(txn);
		if (!beg)
			beg = end;

		for (ptr = beg; ptr < end ; ptr++);

		if (beg < ptr && *beg == '/') {
			while (beg < ptr)
				hash = *(beg++) + (hash << 6) + (hash << 16) - hash;
		}
	}

	hash = full_hash(hash);

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = hash;
	smp->flags = SMP_F_VOL_1ST;
	return 1;
}

/* This concatenates the source address with the 32-bit hash of the Host and
 * URL as returned by smp_fetch_base32(). The idea is to have per-source and
 * per-url counters. The result is a binary block from 8 to 20 bytes depending
 * on the source address length. The URL hash is stored before the address so
 * that in environments where IPv6 is insignificant, truncating the output to
 * 8 bytes would still work.
 */
static int smp_fetch_url32_src(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	struct buffer *temp;
	struct connection *cli_conn = objt_conn(smp->sess->origin);

	if (!cli_conn)
		return 0;

	if (!smp_fetch_url32(args, smp, kw, private))
		return 0;

	temp = get_trash_chunk();
	*(unsigned int *) temp->area = htonl(smp->data.u.sint);
	temp->data += sizeof(unsigned int);

	switch (cli_conn->addr.from.ss_family) {
	case AF_INET:
		memcpy(temp->area + temp->data,
		       &((struct sockaddr_in *)&cli_conn->addr.from)->sin_addr,
		       4);
		temp->data += 4;
		break;
	case AF_INET6:
		memcpy(temp->area + temp->data,
		       &((struct sockaddr_in6 *)&cli_conn->addr.from)->sin6_addr,
		       16);
		temp->data += 16;
		break;
	default:
		return 0;
	}

	smp->data.u.str = *temp;
	smp->data.type = SMP_T_BIN;
	return 1;
}

/************************************************************************/
/*                          Other utility functions                     */
/************************************************************************/

/* This function is used to validate the arguments passed to any "hdr" fetch
 * keyword. These keywords support an optional positive or negative occurrence
 * number. We must ensure that the number is greater than -MAX_HDR_HISTORY. It
 * is assumed that the types are already the correct ones. Returns 0 on error,
 * non-zero if OK. If <err> is not NULL, it will be filled with a pointer to an
 * error message in case of error, that the caller is responsible for freeing.
 * The initial location must either be freeable or NULL.
 * Note: this function's pointer is checked from Lua.
 */
int val_hdr(struct arg *arg, char **err_msg)
{
	if (arg && arg[1].type == ARGT_SINT && arg[1].data.sint < -MAX_HDR_HISTORY) {
		memprintf(err_msg, "header occurrence must be >= %d", -MAX_HDR_HISTORY);
		return 0;
	}
	return 1;
}

/************************************************************************/
/*      All supported sample fetch keywords must be declared here.      */
/************************************************************************/

/* Note: must not be declared <const> as its list will be overwritten */
static struct sample_fetch_kw_list sample_fetch_keywords = {ILH, {
	{ "base",               smp_fetch_base,               0,                NULL,   SMP_T_STR,  SMP_USE_HRQHV },
	{ "base32",             smp_fetch_base32,             0,                NULL,   SMP_T_SINT, SMP_USE_HRQHV },
	{ "base32+src",         smp_fetch_base32_src,         0,                NULL,   SMP_T_BIN,  SMP_USE_HRQHV },

	/* capture are allocated and are permanent in the stream */
	{ "capture.req.hdr",    smp_fetch_capture_req_hdr,    ARG1(1,SINT),     NULL,   SMP_T_STR,  SMP_USE_HRQHP },

	/* retrieve these captures from the HTTP logs */
	{ "capture.req.method", smp_fetch_capture_req_method, 0,                NULL,   SMP_T_STR,  SMP_USE_HRQHP },
	{ "capture.req.uri",    smp_fetch_capture_req_uri,    0,                NULL,   SMP_T_STR,  SMP_USE_HRQHP },
	{ "capture.req.ver",    smp_fetch_capture_req_ver,    0,                NULL,   SMP_T_STR,  SMP_USE_HRQHP },

	{ "capture.res.hdr",    smp_fetch_capture_res_hdr,    ARG1(1,SINT),     NULL,   SMP_T_STR,  SMP_USE_HRSHP },
	{ "capture.res.ver",    smp_fetch_capture_res_ver,    0,                NULL,   SMP_T_STR,  SMP_USE_HRQHP },

	/* cookie is valid in both directions (eg: for "stick ...") but cook*
	 * are only here to match the ACL's name, are request-only and are used
	 * for ACL compatibility only.
	 */
	{ "cook",               smp_fetch_cookie,             ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "cookie",             smp_fetch_chn_cookie,         ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRQHV|SMP_USE_HRSHV },
	{ "cook_cnt",           smp_fetch_cookie_cnt,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "cook_val",           smp_fetch_cookie_val,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },

	/* hdr is valid in both directions (eg: for "stick ...") but hdr_* are
	 * only here to match the ACL's name, are request-only and are used for
	 * ACL compatibility only.
	 */
	{ "hdr",                smp_fetch_chn_hdr,            ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRQHV|SMP_USE_HRSHV },
	{ "hdr_cnt",            smp_fetch_hdr_cnt,            ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "hdr_ip",             smp_fetch_hdr_ip,             ARG2(0,STR,SINT), val_hdr, SMP_T_IPV4, SMP_USE_HRQHV },
	{ "hdr_val",            smp_fetch_hdr_val,            ARG2(0,STR,SINT), val_hdr, SMP_T_SINT, SMP_USE_HRQHV },

	{ "http_auth",          smp_fetch_http_auth,          ARG1(1,USR),      NULL,    SMP_T_BOOL, SMP_USE_HRQHV },
	{ "http_auth_group",    smp_fetch_http_auth_grp,      ARG1(1,USR),      NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "http_first_req",     smp_fetch_http_first_req,     0,                NULL,    SMP_T_BOOL, SMP_USE_HRQHP },
	{ "method",             smp_fetch_meth,               0,                NULL,    SMP_T_METH, SMP_USE_HRQHP },
	{ "path",               smp_fetch_path,               0,                NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "query",              smp_fetch_query,              0,                NULL,    SMP_T_STR,  SMP_USE_HRQHV },

	/* HTTP protocol on the request path */
	{ "req.proto_http",     smp_fetch_proto_http,         0,                NULL,    SMP_T_BOOL, SMP_USE_HRQHP },
	{ "req_proto_http",     smp_fetch_proto_http,         0,                NULL,    SMP_T_BOOL, SMP_USE_HRQHP },

	/* HTTP version on the request path */
	{ "req.ver",            smp_fetch_rqver,              0,                NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "req_ver",            smp_fetch_rqver,              0,                NULL,    SMP_T_STR,  SMP_USE_HRQHV },

	{ "req.body",           smp_fetch_body,               0,                NULL,    SMP_T_BIN,  SMP_USE_HRQHV },
	{ "req.body_len",       smp_fetch_body_len,           0,                NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "req.body_size",      smp_fetch_body_size,          0,                NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "req.body_param",     smp_fetch_body_param,         ARG1(0,STR),      NULL,    SMP_T_BIN,  SMP_USE_HRQHV },

	{ "req.hdrs",           smp_fetch_hdrs,               0,                NULL,    SMP_T_BIN,  SMP_USE_HRQHV },
	{ "req.hdrs_bin",       smp_fetch_hdrs_bin,           0,                NULL,    SMP_T_BIN,  SMP_USE_HRQHV },

	/* HTTP version on the response path */
	{ "res.ver",            smp_fetch_stver,              0,                NULL,    SMP_T_STR,  SMP_USE_HRSHV },
	{ "resp_ver",           smp_fetch_stver,              0,                NULL,    SMP_T_STR,  SMP_USE_HRSHV },

	/* explicit req.{cook,hdr} are used to force the fetch direction to be request-only */
	{ "req.cook",           smp_fetch_cookie,             ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "req.cook_cnt",       smp_fetch_cookie_cnt,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "req.cook_val",       smp_fetch_cookie_val,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },

	{ "req.fhdr",           smp_fetch_fhdr,               ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRQHV },
	{ "req.fhdr_cnt",       smp_fetch_fhdr_cnt,           ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "req.hdr",            smp_fetch_hdr,                ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRQHV },
	{ "req.hdr_cnt",        smp_fetch_hdr_cnt,            ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "req.hdr_ip",         smp_fetch_hdr_ip,             ARG2(0,STR,SINT), val_hdr, SMP_T_IPV4, SMP_USE_HRQHV },
	{ "req.hdr_names",      smp_fetch_hdr_names,          ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "req.hdr_val",        smp_fetch_hdr_val,            ARG2(0,STR,SINT), val_hdr, SMP_T_SINT, SMP_USE_HRQHV },

	/* explicit req.{cook,hdr} are used to force the fetch direction to be response-only */
	{ "res.cook",           smp_fetch_cookie,             ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRSHV },
	{ "res.cook_cnt",       smp_fetch_cookie_cnt,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "res.cook_val",       smp_fetch_cookie_val,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },

	{ "res.fhdr",           smp_fetch_fhdr,               ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRSHV },
	{ "res.fhdr_cnt",       smp_fetch_fhdr_cnt,           ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "res.hdr",            smp_fetch_hdr,                ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRSHV },
	{ "res.hdr_cnt",        smp_fetch_hdr_cnt,            ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "res.hdr_ip",         smp_fetch_hdr_ip,             ARG2(0,STR,SINT), val_hdr, SMP_T_IPV4, SMP_USE_HRSHV },
	{ "res.hdr_names",      smp_fetch_hdr_names,          ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRSHV },
	{ "res.hdr_val",        smp_fetch_hdr_val,            ARG2(0,STR,SINT), val_hdr, SMP_T_SINT, SMP_USE_HRSHV },

	/* scook is valid only on the response and is used for ACL compatibility */
	{ "scook",              smp_fetch_cookie,             ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRSHV },
	{ "scook_cnt",          smp_fetch_cookie_cnt,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "scook_val",          smp_fetch_cookie_val,         ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "set-cookie",         smp_fetch_cookie,             ARG1(0,STR),      NULL,    SMP_T_STR,  SMP_USE_HRSHV }, /* deprecated */

	/* shdr is valid only on the response and is used for ACL compatibility */
	{ "shdr",               smp_fetch_hdr,                ARG2(0,STR,SINT), val_hdr, SMP_T_STR,  SMP_USE_HRSHV },
	{ "shdr_cnt",           smp_fetch_hdr_cnt,            ARG1(0,STR),      NULL,    SMP_T_SINT, SMP_USE_HRSHV },
	{ "shdr_ip",            smp_fetch_hdr_ip,             ARG2(0,STR,SINT), val_hdr, SMP_T_IPV4, SMP_USE_HRSHV },
	{ "shdr_val",           smp_fetch_hdr_val,            ARG2(0,STR,SINT), val_hdr, SMP_T_SINT, SMP_USE_HRSHV },

	{ "status",             smp_fetch_stcode,             0,                NULL,    SMP_T_SINT, SMP_USE_HRSHP },
	{ "unique-id",          smp_fetch_uniqueid,           0,                NULL,    SMP_T_STR,  SMP_SRC_L4SRV },
	{ "url",                smp_fetch_url,                0,                NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "url32",              smp_fetch_url32,              0,                NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "url32+src",          smp_fetch_url32_src,          0,                NULL,    SMP_T_BIN,  SMP_USE_HRQHV },
	{ "url_ip",             smp_fetch_url_ip,             0,                NULL,    SMP_T_IPV4, SMP_USE_HRQHV },
	{ "url_port",           smp_fetch_url_port,           0,                NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ "url_param",          smp_fetch_url_param,          ARG2(0,STR,STR),  NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "urlp"     ,          smp_fetch_url_param,          ARG2(0,STR,STR),  NULL,    SMP_T_STR,  SMP_USE_HRQHV },
	{ "urlp_val",           smp_fetch_url_param_val,      ARG2(0,STR,STR),  NULL,    SMP_T_SINT, SMP_USE_HRQHV },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &sample_fetch_keywords);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
