/*
 * HTTP/2 protocol processing
 *
 * Copyright 2017 Willy Tarreau <w@1wt.eu>
 * Copyright (C) 2017 HAProxy Technologies
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <common/config.h>
#include <common/h2.h>
#include <common/http-hdr.h>
#include <common/ist.h>

struct h2_frame_definition h2_frame_definition[H2_FT_ENTRIES] =	{
	 [H2_FT_DATA         ] = { .dir = 3, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 0, .max_len = H2_MAX_FRAME_LEN, },
	 [H2_FT_HEADERS      ] = { .dir = 3, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 1, .max_len = H2_MAX_FRAME_LEN, },
	 [H2_FT_PRIORITY     ] = { .dir = 3, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 5, .max_len = 5,                },
	 [H2_FT_RST_STREAM   ] = { .dir = 3, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 4, .max_len = 4,                },
	 [H2_FT_SETTINGS     ] = { .dir = 3, .min_id = 0, .max_id = 0,                .min_len = 0, .max_len = H2_MAX_FRAME_LEN, },
	 [H2_FT_PUSH_PROMISE ] = { .dir = 0, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 4, .max_len = H2_MAX_FRAME_LEN, },
	 [H2_FT_PING         ] = { .dir = 3, .min_id = 0, .max_id = 0,                .min_len = 8, .max_len = 8,                },
	 [H2_FT_GOAWAY       ] = { .dir = 3, .min_id = 0, .max_id = 0,                .min_len = 8, .max_len = H2_MAX_FRAME_LEN, },
	 [H2_FT_WINDOW_UPDATE] = { .dir = 3, .min_id = 0, .max_id = H2_MAX_STREAM_ID, .min_len = 4, .max_len = 4,                },
	 [H2_FT_CONTINUATION ] = { .dir = 3, .min_id = 1, .max_id = H2_MAX_STREAM_ID, .min_len = 0, .max_len = H2_MAX_FRAME_LEN, },
};

/* Prepare the request line into <*ptr> (stopping at <end>) from pseudo headers
 * stored in <phdr[]>. <fields> indicates what was found so far. This should be
 * called once at the detection of the first general header field or at the end
 * of the request if no general header field was found yet. Returns 0 on success
 * or a negative error code on failure. Upon success, <msgf> is updated with a
 * few H2_MSGF_* flags indicating what was found while parsing.
 */
static int h2_prepare_h1_reqline(uint32_t fields, struct ist *phdr, char **ptr, char *end, unsigned int *msgf)
{
	char *out = *ptr;
	int uri_idx = H2_PHDR_IDX_PATH;

	if ((fields & H2_PHDR_FND_METH) && isteq(phdr[H2_PHDR_IDX_METH], ist("CONNECT"))) {
		/* RFC 7540 #8.2.6 regarding CONNECT: ":scheme" and ":path"
		 * MUST be omitted ; ":authority" contains the host and port
		 * to connect to.
		 */
		if (fields & H2_PHDR_FND_SCHM) {
			/* scheme not allowed */
			goto fail;
		}
		else if (fields & H2_PHDR_FND_PATH) {
			/* path not allowed */
			goto fail;
		}
		else if (!(fields & H2_PHDR_FND_AUTH)) {
			/* missing authority */
			goto fail;
		}
		// otherwise OK ; let's use the authority instead of the URI
		uri_idx = H2_PHDR_IDX_AUTH;
		*msgf |= H2_MSGF_BODY_TUNNEL;
	}
	else if ((fields & (H2_PHDR_FND_METH|H2_PHDR_FND_SCHM|H2_PHDR_FND_PATH)) !=
	         (H2_PHDR_FND_METH|H2_PHDR_FND_SCHM|H2_PHDR_FND_PATH)) {
		/* RFC 7540 #8.1.2.3 : all requests MUST include exactly one
		 * valid value for the ":method", ":scheme" and ":path" phdr
		 * unless it is a CONNECT request.
		 */
		if (!(fields & H2_PHDR_FND_METH)) {
			/* missing method */
			goto fail;
		}
		else if (!(fields & H2_PHDR_FND_SCHM)) {
			/* missing scheme */
			goto fail;
		}
		else {
			/* missing path */
			goto fail;
		}
	}

	/* 7540#8.1.2.3: :path must not be empty */
	if (!phdr[uri_idx].len)
		goto fail;

	if (out + phdr[H2_PHDR_IDX_METH].len + 1 + phdr[uri_idx].len + 11 > end) {
		/* too large */
		goto fail;
	}

	memcpy(out, phdr[H2_PHDR_IDX_METH].ptr, phdr[H2_PHDR_IDX_METH].len);
	out += phdr[H2_PHDR_IDX_METH].len;
	*(out++) = ' ';

	memcpy(out, phdr[uri_idx].ptr, phdr[uri_idx].len);
	out += phdr[uri_idx].len;
	memcpy(out, " HTTP/1.1\r\n", 11);
	out += 11;

	*ptr = out;
	return 0;
 fail:
	return -1;
}

/* Takes an H2 request present in the headers list <list> terminated by a name
 * being <NULL,0> and emits the equivalent HTTP/1.1 request according to the
 * rules documented in RFC7540 #8.1.2. The output contents are emitted in <out>
 * for a max of <osize> bytes, and the amount of bytes emitted is returned. In
 * case of error, a negative error code is returned.
 *
 * Upon success, <msgf> is filled with a few H2_MSGF_* flags indicating what
 * was found while parsing. The caller must set it to zero in or H2_MSGF_BODY
 * if a body is detected (!ES).
 *
 * The headers list <list> must be composed of :
 *   - n.name != NULL, n.len  > 0 : literal header name
 *   - n.name == NULL, n.len  > 0 : indexed pseudo header name number <n.len>
 *                                  among H2_PHDR_IDX_*
 *   - n.name ignored, n.len == 0 : end of list
 *   - in all cases except the end of list, v.name and v.len must designate a
 *     valid value.
 *
 * The Cookie header will be reassembled at the end, and for this, the <list>
 * will be used to create a linked list, so its contents may be destroyed.
 */
int h2_make_h1_request(struct http_hdr *list, char *out, int osize, unsigned int *msgf, unsigned long long *body_len)
{
	struct ist phdr_val[H2_PHDR_NUM_ENTRIES];
	char *out_end = out + osize;
	uint32_t fields; /* bit mask of H2_PHDR_FND_* */
	uint32_t idx;
	int ck, lck; /* cookie index and last cookie index */
	int phdr;
	int ret;
	int i;

	lck = ck = -1; // no cookie for now
	fields = 0;
	for (idx = 0; list[idx].n.len != 0; idx++) {
		if (!list[idx].n.ptr) {
			/* this is an indexed pseudo-header */
			phdr = list[idx].n.len;
		}
		else {
			/* this can be any type of header */
			/* RFC7540#8.1.2: upper case not allowed in header field names */
			for (i = 0; i < list[idx].n.len; i++)
				if ((uint8_t)(list[idx].n.ptr[i] - 'A') < 'Z' - 'A')
					goto fail;

			phdr = h2_str_to_phdr(list[idx].n);
		}

		if (phdr > 0 && phdr < H2_PHDR_NUM_ENTRIES) {
			/* insert a pseudo header by its index (in phdr) and value (in value) */
			if (fields & ((1 << phdr) | H2_PHDR_FND_NONE)) {
				if (fields & H2_PHDR_FND_NONE) {
					/* pseudo header field after regular headers */
					goto fail;
				}
				else {
					/* repeated pseudo header field */
					goto fail;
				}
			}
			fields |= 1 << phdr;
			phdr_val[phdr] = list[idx].v;
			continue;
		}
		else if (phdr != 0) {
			/* invalid pseudo header -- should never happen here */
			goto fail;
		}

		/* regular header field in (name,value) */
		if (!(fields & H2_PHDR_FND_NONE)) {
			/* no more pseudo-headers, time to build the request line */
			ret = h2_prepare_h1_reqline(fields, phdr_val, &out, out_end, msgf);
			if (ret != 0)
				goto leave;
			fields |= H2_PHDR_FND_NONE;
		}

		if (isteq(list[idx].n, ist("host")))
			fields |= H2_PHDR_FND_HOST;

		if (isteq(list[idx].n, ist("content-length"))) {
			ret = h2_parse_cont_len_header(msgf, &list[idx].v, body_len);
			if (ret < 0)
				goto fail;

			if (ret == 0)
				continue; // skip this duplicate
		}

		/* these ones are forbidden in requests (RFC7540#8.1.2.2) */
		if (isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			goto fail;

		if (isteq(list[idx].n, ist("te")) && !isteq(list[idx].v, ist("trailers")))
			goto fail;

		/* cookie requires special processing at the end */
		if (isteq(list[idx].n, ist("cookie"))) {
			list[idx].n.len = -1;

			if (ck < 0)
				ck = idx;
			else
				list[lck].n.len = idx;

			lck = idx;
			continue;
		}

		if (out + list[idx].n.len + 2 + list[idx].v.len + 2 > out_end) {
			/* too large */
			goto fail;
		}

		/* copy "name: value" */
		memcpy(out, list[idx].n.ptr, list[idx].n.len);
		out += list[idx].n.len;
		*(out++) = ':';
		*(out++) = ' ';

		memcpy(out, list[idx].v.ptr, list[idx].v.len);
		out += list[idx].v.len;
		*(out++) = '\r';
		*(out++) = '\n';
	}

	/* RFC7540#8.1.2.1 mandates to reject response pseudo-headers (:status) */
	if (fields & H2_PHDR_FND_STAT)
		goto fail;

	/* Let's dump the request now if not yet emitted. */
	if (!(fields & H2_PHDR_FND_NONE)) {
		ret = h2_prepare_h1_reqline(fields, phdr_val, &out, out_end, msgf);
		if (ret != 0)
			goto leave;
	}

	/* complete with missing Host if needed */
	if ((fields & (H2_PHDR_FND_HOST|H2_PHDR_FND_AUTH)) == H2_PHDR_FND_AUTH) {
		/* missing Host field, use :authority instead */
		if (out + 6 + phdr_val[H2_PHDR_IDX_AUTH].len + 2 > out_end) {
			/* too large */
			goto fail;
		}

		memcpy(out, "host: ", 6);
		memcpy(out + 6, phdr_val[H2_PHDR_IDX_AUTH].ptr, phdr_val[H2_PHDR_IDX_AUTH].len);
		out += 6 + phdr_val[H2_PHDR_IDX_AUTH].len;
		*(out++) = '\r';
		*(out++) = '\n';
	}

	if ((*msgf & (H2_MSGF_BODY|H2_MSGF_BODY_TUNNEL|H2_MSGF_BODY_CL)) == H2_MSGF_BODY) {
		/* add chunked encoding */
		if (out + 28 > out_end)
			goto fail;
		memcpy(out, "transfer-encoding: chunked\r\n", 28);
		out += 28;
	}

	/* now we may have to build a cookie list. We'll dump the values of all
	 * visited headers.
	 */
	if (ck >= 0) {
		if (out + 8 > out_end) {
			/* too large */
			goto fail;
		}
		memcpy(out, "cookie: ", 8);
		out += 8;

		do {
			if (out + list[ck].v.len + 2 > out_end) {
				/* too large */
				goto fail;
			}
			memcpy(out, list[ck].v.ptr, list[ck].v.len);
			out += list[ck].v.len;
			ck = list[ck].n.len;

			if (ck >= 0) {
				*(out++) = ';';
				*(out++) = ' ';
			}
		} while (ck >= 0);

		if (out + 2 > out_end) {
			/* too large */
			goto fail;
		}
		*(out++) = '\r';
		*(out++) = '\n';
	}

	/* And finish */
	if (out + 2 > out_end) {
		/* too large */
		goto fail;
	}

	*(out++) = '\r';
	*(out++) = '\n';
	ret = out + osize - out_end;
 leave:
	return ret;

 fail:
	return -1;
}

/* Takes an H2 headers list <list> terminated by a name being <NULL,0> and
 * emits the equivalent HTTP/1.1 trailers block not including the empty line.
 * The output contents are emitted in <out> for a max of <osize> bytes, and the
 * amount of bytes emitted is returned. In case of error, a negative error code
 * is returned. The caller must have verified that the message in the buffer is
 * compatible with receipt of trailers.
 *
 * The headers list <list> must be composed of :
 *   - n.name != NULL, n.len  > 0 : literal header name
 *   - n.name == NULL, n.len  > 0 : indexed pseudo header name number <n.len>
 *                                  among H2_PHDR_IDX_* (illegal here)
 *   - n.name ignored, n.len == 0 : end of list
 *   - in all cases except the end of list, v.name and v.len must designate a
 *     valid value.
 */
int h2_make_h1_trailers(struct http_hdr *list, char *out, int osize)
{
	char *out_end = out + osize;
	uint32_t idx;
	int i;

	for (idx = 0; list[idx].n.len != 0; idx++) {
		if (!list[idx].n.ptr) {
			/* This is an indexed pseudo-header (RFC7540#8.1.2.1) */
			goto fail;
		}

		/* RFC7540#8.1.2: upper case not allowed in header field names */
		for (i = 0; i < list[idx].n.len; i++)
			if ((uint8_t)(list[idx].n.ptr[i] - 'A') < 'Z' - 'A')
				goto fail;

		if (h2_str_to_phdr(list[idx].n) != 0) {
			/* This is a pseudo-header (RFC7540#8.1.2.1) */
			goto fail;
		}

		/* these ones are forbidden in trailers (RFC7540#8.1.2.2) */
		if (isteq(list[idx].n, ist("host")) ||
		    isteq(list[idx].n, ist("content-length")) ||
		    isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("te")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			goto fail;

		if (out + list[idx].n.len + 2 + list[idx].v.len + 2 > out_end) {
			/* too large */
			goto fail;
		}

		/* copy "name: value" */
		memcpy(out, list[idx].n.ptr, list[idx].n.len);
		out += list[idx].n.len;
		*(out++) = ':';
		*(out++) = ' ';

		memcpy(out, list[idx].v.ptr, list[idx].v.len);
		out += list[idx].v.len;
		*(out++) = '\r';
		*(out++) = '\n';
	}

	return out + osize - out_end;

 fail:
	return -1;
}

/* Parse the Content-Length header field of an HTTP/2 request. The function
 * checks all possible occurrences of a comma-delimited value, and verifies
 * if any of them doesn't match a previous value. It returns <0 if a value
 * differs, 0 if the whole header can be dropped (i.e. already known), or >0
 * if the value can be indexed (first one). In the last case, the value might
 * be adjusted and the caller must only add the updated value.
 */
int h2_parse_cont_len_header(unsigned int *msgf, struct ist *value, unsigned long long *body_len)
{
	char *e, *n;
	unsigned long long cl;
	int not_first = !!(*msgf & H2_MSGF_BODY_CL);
	struct ist word;

	word.ptr = value->ptr - 1; // -1 for next loop's pre-increment
	e = value->ptr + value->len;

	while (++word.ptr < e) {
		/* skip leading delimitor and blanks */
		if (unlikely(HTTP_IS_LWS(*word.ptr)))
			continue;

		/* digits only now */
		for (cl = 0, n = word.ptr; n < e; n++) {
			unsigned int c = *n - '0';
			if (unlikely(c > 9)) {
				/* non-digit */
				if (unlikely(n == word.ptr)) // spaces only
					goto fail;
				break;
			}
			if (unlikely(cl > ULLONG_MAX / 10ULL))
				goto fail; /* multiply overflow */
			cl = cl * 10ULL;
			if (unlikely(cl + c < cl))
				goto fail; /* addition overflow */
			cl = cl + c;
		}

		/* keep a copy of the exact cleaned value */
		word.len = n - word.ptr;

		/* skip trailing LWS till next comma or EOL */
		for (; n < e; n++) {
			if (!HTTP_IS_LWS(*n)) {
				if (unlikely(*n != ','))
					goto fail;
				break;
			}
		}

		/* if duplicate, must be equal */
		if (*msgf & H2_MSGF_BODY_CL && cl != *body_len)
			goto fail;

		/* OK, store this result as the one to be indexed */
		*msgf |= H2_MSGF_BODY_CL;
		*body_len = cl;
		*value = word;
		word.ptr = n;
	}
	/* here we've reached the end with a single value or a series of
	 * identical values, all matching previous series if any. The last
	 * parsed value was sent back into <value>. We just have to decide
	 * if this occurrence has to be indexed (it's the first one) or
	 * silently skipped (it's not the first one)
	 */
	return !not_first;
 fail:
	return -1;
}

/* Prepare the request line into <htx> from pseudo headers stored in <phdr[]>.
 * <fields> indicates what was found so far. This should be called once at the
 * detection of the first general header field or at the end of the request if
 * no general header field was found yet. Returns the created start line on
 * success, or NULL on failure. Upon success, <msgf> is updated with a few
 * H2_MSGF_* flags indicating what was found while parsing.
 */
static struct htx_sl *h2_prepare_htx_reqline(uint32_t fields, struct ist *phdr, struct htx *htx, unsigned int *msgf)
{
	int uri_idx = H2_PHDR_IDX_PATH;
	unsigned int flags = HTX_SL_F_NONE;
	struct htx_sl *sl;
	size_t i;

	if ((fields & H2_PHDR_FND_METH) && isteq(phdr[H2_PHDR_IDX_METH], ist("CONNECT"))) {
		/* RFC 7540 #8.2.6 regarding CONNECT: ":scheme" and ":path"
		 * MUST be omitted ; ":authority" contains the host and port
		 * to connect to.
		 */
		if (fields & H2_PHDR_FND_SCHM) {
			/* scheme not allowed */
			goto fail;
		}
		else if (fields & H2_PHDR_FND_PATH) {
			/* path not allowed */
			goto fail;
		}
		else if (!(fields & H2_PHDR_FND_AUTH)) {
			/* missing authority */
			goto fail;
		}
		// otherwise OK ; let's use the authority instead of the URI
		uri_idx = H2_PHDR_IDX_AUTH;
		*msgf |= H2_MSGF_BODY_TUNNEL;
	}
	else if ((fields & (H2_PHDR_FND_METH|H2_PHDR_FND_SCHM|H2_PHDR_FND_PATH)) !=
	         (H2_PHDR_FND_METH|H2_PHDR_FND_SCHM|H2_PHDR_FND_PATH)) {
		/* RFC 7540 #8.1.2.3 : all requests MUST include exactly one
		 * valid value for the ":method", ":scheme" and ":path" phdr
		 * unless it is a CONNECT request.
		 */
		if (!(fields & H2_PHDR_FND_METH)) {
			/* missing method */
			goto fail;
		}
		else if (!(fields & H2_PHDR_FND_SCHM)) {
			/* missing scheme */
			goto fail;
		}
		else {
			/* missing path */
			goto fail;
		}
	}

	/* 7540#8.1.2.3: :path must not be empty */
	if (!phdr[uri_idx].len)
		goto fail;

	/* make sure :path doesn't contain LWS nor CTL characters */
	for (i = 0; i < phdr[uri_idx].len; i++) {
		unsigned char c = phdr[uri_idx].ptr[i];
		if (HTTP_IS_LWS(c) || HTTP_IS_CTL(c))
			htx->flags |= HTX_FL_PARSING_ERROR;
	}

	/* Set HTX start-line flags */
	flags |= HTX_SL_F_VER_11;    // V2 in fact
	flags |= HTX_SL_F_XFER_LEN;  // xfer len always known with H2

	sl = htx_add_stline(htx, HTX_BLK_REQ_SL, flags, phdr[H2_PHDR_IDX_METH], phdr[uri_idx], ist("HTTP/2.0"));
	if (!sl)
		goto fail;

	sl->info.req.meth = find_http_meth(phdr[H2_PHDR_IDX_METH].ptr, phdr[H2_PHDR_IDX_METH].len);
	sl->flags |= HTX_SL_F_HAS_SCHM;
	sl->flags |= (isteqi(phdr[H2_PHDR_IDX_SCHM], ist("http")) ? HTX_SL_F_SCHM_HTTP : HTX_SL_F_SCHM_HTTPS);
	return sl;
 fail:
	return NULL;
}

/* Takes an H2 request present in the headers list <list> terminated by a name
 * being <NULL,0> and emits the equivalent HTX request according to the rules
 * documented in RFC7540 #8.1.2. The output contents are emitted in <htx>, and
 * non-zero is returned if some bytes were emitted. In case of error, a
 * negative error code is returned.
 *
 * Upon success, <msgf> is filled with a few H2_MSGF_* flags indicating what
 * was found while parsing. The caller must set it to zero in or H2_MSGF_BODY
 * if a body is detected (!ES).
 *
 * The headers list <list> must be composed of :
 *   - n.name != NULL, n.len  > 0 : literal header name
 *   - n.name == NULL, n.len  > 0 : indexed pseudo header name number <n.len>
 *                                  among H2_PHDR_IDX_*
 *   - n.name ignored, n.len == 0 : end of list
 *   - in all cases except the end of list, v.name and v.len must designate a
 *     valid value.
 *
 * The Cookie header will be reassembled at the end, and for this, the <list>
 * will be used to create a linked list, so its contents may be destroyed.
 */
int h2_make_htx_request(struct http_hdr *list, struct htx *htx, unsigned int *msgf, unsigned long long *body_len)
{
	struct ist phdr_val[H2_PHDR_NUM_ENTRIES];
	uint32_t fields; /* bit mask of H2_PHDR_FND_* */
	uint32_t idx;
	int ck, lck; /* cookie index and last cookie index */
	int phdr;
	int ret;
	int i;
	uint32_t used = htx_used_space(htx);
	struct htx_sl *sl = NULL;
	unsigned int sl_flags = 0;

	lck = ck = -1; // no cookie for now
	fields = 0;
	for (idx = 0; list[idx].n.len != 0; idx++) {
		if (!list[idx].n.ptr) {
			/* this is an indexed pseudo-header */
			phdr = list[idx].n.len;
		}
		else {
			/* this can be any type of header */
			/* RFC7540#8.1.2: upper case not allowed in header field names */
			for (i = 0; i < list[idx].n.len; i++)
				if ((uint8_t)(list[idx].n.ptr[i] - 'A') < 'Z' - 'A')
					goto fail;

			phdr = h2_str_to_phdr(list[idx].n);
		}

		if (phdr > 0 && phdr < H2_PHDR_NUM_ENTRIES) {
			/* insert a pseudo header by its index (in phdr) and value (in value) */
			if (fields & ((1 << phdr) | H2_PHDR_FND_NONE)) {
				if (fields & H2_PHDR_FND_NONE) {
					/* pseudo header field after regular headers */
					goto fail;
				}
				else {
					/* repeated pseudo header field */
					goto fail;
				}
			}
			fields |= 1 << phdr;
			phdr_val[phdr] = list[idx].v;
			continue;
		}
		else if (phdr != 0) {
			/* invalid pseudo header -- should never happen here */
			goto fail;
		}

		/* regular header field in (name,value) */
		if (unlikely(!(fields & H2_PHDR_FND_NONE))) {
			/* no more pseudo-headers, time to build the request line */
			sl = h2_prepare_htx_reqline(fields, phdr_val, htx, msgf);
			if (!sl)
				goto fail;
			fields |= H2_PHDR_FND_NONE;
		}

		if (isteq(list[idx].n, ist("host")))
			fields |= H2_PHDR_FND_HOST;

		if (isteq(list[idx].n, ist("content-length"))) {
			ret = h2_parse_cont_len_header(msgf, &list[idx].v, body_len);
			if (ret < 0)
				goto fail;

			sl_flags |= HTX_SL_F_CLEN;
			if (ret == 0)
				continue; // skip this duplicate
		}

		/* these ones are forbidden in requests (RFC7540#8.1.2.2) */
		if (isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			goto fail;

		if (isteq(list[idx].n, ist("te")) && !isteq(list[idx].v, ist("trailers")))
			goto fail;

		/* cookie requires special processing at the end */
		if (isteq(list[idx].n, ist("cookie"))) {
			list[idx].n.len = -1;

			if (ck < 0)
				ck = idx;
			else
				list[lck].n.len = idx;

			lck = idx;
			continue;
		}

		if (!htx_add_header(htx, list[idx].n, list[idx].v))
			goto fail;
	}

	/* RFC7540#8.1.2.1 mandates to reject response pseudo-headers (:status) */
	if (fields & H2_PHDR_FND_STAT)
		goto fail;

	/* Let's dump the request now if not yet emitted. */
	if (!(fields & H2_PHDR_FND_NONE)) {
		sl = h2_prepare_htx_reqline(fields, phdr_val, htx, msgf);
		if (!sl)
			goto fail;
	}

	if (!(*msgf & H2_MSGF_BODY) || ((*msgf & H2_MSGF_BODY_CL) && *body_len == 0))
		sl_flags |= HTX_SL_F_BODYLESS;

	/* update the start line with last detected header info */
	sl->flags |= sl_flags;

	/* complete with missing Host if needed */
	if ((fields & (H2_PHDR_FND_HOST|H2_PHDR_FND_AUTH)) == H2_PHDR_FND_AUTH) {
		/* missing Host field, use :authority instead */
		if (!htx_add_header(htx, ist("host"), phdr_val[H2_PHDR_IDX_AUTH]))
			goto fail;
	}

	/* now we may have to build a cookie list. We'll dump the values of all
	 * visited headers.
	 */
	if (ck >= 0) {
		uint32_t fs; // free space
		uint32_t bs; // block size
		uint32_t vl; // value len
		uint32_t tl; // total length
		struct htx_blk *blk;

		blk = htx_add_header(htx, ist("cookie"), list[ck].v);
		if (!blk)
			goto fail;

		tl = list[ck].v.len;
		fs = htx_free_data_space(htx);
		bs = htx_get_blksz(blk);

		/* for each extra cookie, we'll extend the cookie's value and
		 * insert "; " before the new value.
		 */
		fs += tl; // first one is already counted
		for (; (ck = list[ck].n.len) >= 0 ; ) {
			vl = list[ck].v.len;
			tl += vl + 2;
			if (tl > fs)
				goto fail;

			htx_change_blk_value_len(htx, blk, tl);
			*(char *)(htx_get_blk_ptr(htx, blk) + bs + 0) = ';';
			*(char *)(htx_get_blk_ptr(htx, blk) + bs + 1) = ' ';
			memcpy(htx_get_blk_ptr(htx, blk) + bs + 2, list[ck].v.ptr, vl);
			bs += vl + 2;
		}

	}

	/* now send the end of headers marker */
	htx_add_endof(htx, HTX_BLK_EOH);

	/* Set bytes used in the HTX mesage for the headers now */
	sl->hdrs_bytes = htx_used_space(htx) - used;

	ret = 1;
	return ret;

 fail:
	return -1;
}

/* Prepare the status line into <htx> from pseudo headers stored in <phdr[]>.
 * <fields> indicates what was found so far. This should be called once at the
 * detection of the first general header field or at the end of the message if
 * no general header field was found yet. Returns the created start line on
 * success, or NULL on failure. Upon success, <msgf> is updated with a few
 * H2_MSGF_* flags indicating what was found while parsing.
 */
static struct htx_sl *h2_prepare_htx_stsline(uint32_t fields, struct ist *phdr, struct htx *htx, unsigned int *msgf)
{
	unsigned int flags = HTX_SL_F_NONE;
	struct htx_sl *sl;
	unsigned char h, t, u;

	/* only :status is allowed as a pseudo header */
	if (!(fields & H2_PHDR_FND_STAT))
		goto fail;

	if (phdr[H2_PHDR_IDX_STAT].len != 3)
		goto fail;

	/* Set HTX start-line flags */
	flags |= HTX_SL_F_VER_11;    // V2 in fact
	flags |= HTX_SL_F_XFER_LEN;  // xfer len always known with H2

	sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags, ist("HTTP/2.0"), phdr[H2_PHDR_IDX_STAT], ist(""));
	if (!sl)
		goto fail;

	h = phdr[H2_PHDR_IDX_STAT].ptr[0] - '0';
	t = phdr[H2_PHDR_IDX_STAT].ptr[1] - '0';
	u = phdr[H2_PHDR_IDX_STAT].ptr[2] - '0';
	if (h > 9 || t > 9 || u > 9)
		goto fail;

	sl->info.res.status = h * 100 + t * 10 + u;

	/* On 1xx responses (except 101) there is no ES on the HEADERS frame but
	 * there is no body. So remove the flag H2_MSGF_BODY and add
	 * H2_MSGF_RSP_1XX to notify the decoder another HEADERS frame is
	 * expected.
	 */
	if (sl->info.res.status < 200 &&
	    (sl->info.res.status == 100 || sl->info.res.status >= 102)) {
		*msgf |= H2_MSGF_RSP_1XX;
		*msgf &= ~H2_MSGF_BODY;
	}

	return sl;
 fail:
	return NULL;
}

/* Takes an H2 response present in the headers list <list> terminated by a name
 * being <NULL,0> and emits the equivalent HTX response according to the rules
 * documented in RFC7540 #8.1.2. The output contents are emitted in <htx>, and
 * a positive value is returned if some bytes were emitted. In case of error, a
 * negative error code is returned.
 *
 * Upon success, <msgf> is filled with a few H2_MSGF_* flags indicating what
 * was found while parsing. The caller must set it to zero in or H2_MSGF_BODY
 * if a body is detected (!ES).
 *
 * The headers list <list> must be composed of :
 *   - n.name != NULL, n.len  > 0 : literal header name
 *   - n.name == NULL, n.len  > 0 : indexed pseudo header name number <n.len>
 *                                  among H2_PHDR_IDX_*
 *   - n.name ignored, n.len == 0 : end of list
 *   - in all cases except the end of list, v.name and v.len must designate a
 *     valid value.
 */
int h2_make_htx_response(struct http_hdr *list, struct htx *htx, unsigned int *msgf, unsigned long long *body_len)
{
	struct ist phdr_val[H2_PHDR_NUM_ENTRIES];
	uint32_t fields; /* bit mask of H2_PHDR_FND_* */
	uint32_t idx;
	int phdr;
	int ret;
	int i;
	uint32_t used = htx_used_space(htx);
	struct htx_sl *sl = NULL;
	unsigned int sl_flags = 0;

	fields = 0;
	for (idx = 0; list[idx].n.len != 0; idx++) {
		if (!list[idx].n.ptr) {
			/* this is an indexed pseudo-header */
			phdr = list[idx].n.len;
		}
		else {
			/* this can be any type of header */
			/* RFC7540#8.1.2: upper case not allowed in header field names */
			for (i = 0; i < list[idx].n.len; i++)
				if ((uint8_t)(list[idx].n.ptr[i] - 'A') < 'Z' - 'A')
					goto fail;

			phdr = h2_str_to_phdr(list[idx].n);
		}

		if (phdr > 0 && phdr < H2_PHDR_NUM_ENTRIES) {
			/* insert a pseudo header by its index (in phdr) and value (in value) */
			if (fields & ((1 << phdr) | H2_PHDR_FND_NONE)) {
				if (fields & H2_PHDR_FND_NONE) {
					/* pseudo header field after regular headers */
					goto fail;
				}
				else {
					/* repeated pseudo header field */
					goto fail;
				}
			}
			fields |= 1 << phdr;
			phdr_val[phdr] = list[idx].v;
			continue;
		}
		else if (phdr != 0) {
			/* invalid pseudo header -- should never happen here */
			goto fail;
		}

		/* regular header field in (name,value) */
		if (!(fields & H2_PHDR_FND_NONE)) {
			/* no more pseudo-headers, time to build the status line */
			sl = h2_prepare_htx_stsline(fields, phdr_val, htx, msgf);
			if (!sl)
				goto fail;
			fields |= H2_PHDR_FND_NONE;
		}

		if (isteq(list[idx].n, ist("content-length"))) {
			ret = h2_parse_cont_len_header(msgf, &list[idx].v, body_len);
			if (ret < 0)
				goto fail;

			sl_flags |= HTX_SL_F_CLEN;
			if (ret == 0)
				continue; // skip this duplicate
		}

		/* these ones are forbidden in responses (RFC7540#8.1.2.2) */
		if (isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			goto fail;

		if (!htx_add_header(htx, list[idx].n, list[idx].v))
			goto fail;
	}

	/* RFC7540#8.1.2.1 mandates to reject request pseudo-headers */
	if (fields & (H2_PHDR_FND_AUTH|H2_PHDR_FND_METH|H2_PHDR_FND_PATH|H2_PHDR_FND_SCHM))
		goto fail;

	/* Let's dump the request now if not yet emitted. */
	if (!(fields & H2_PHDR_FND_NONE)) {
		sl = h2_prepare_htx_stsline(fields, phdr_val, htx, msgf);
		if (!sl)
			goto fail;
	}

	if (!(*msgf & H2_MSGF_BODY) || ((*msgf & H2_MSGF_BODY_CL) && *body_len == 0))
		sl_flags |= HTX_SL_F_BODYLESS;

	/* update the start line with last detected header info */
	sl->flags |= sl_flags;

	if ((*msgf & (H2_MSGF_BODY|H2_MSGF_BODY_TUNNEL|H2_MSGF_BODY_CL)) == H2_MSGF_BODY) {
		/* FIXME: Do we need to signal anything when we have a body and
		 * no content-length, to have the equivalent of H1's chunked
		 * encoding?
		 */
	}

	/* now send the end of headers marker */
	htx_add_endof(htx, HTX_BLK_EOH);

	/* Set bytes used in the HTX mesage for the headers now */
	sl->hdrs_bytes = htx_used_space(htx) - used;

	ret = 1;
	return ret;

 fail:
	return -1;
}

/* Takes an H2 headers list <list> terminated by a name being <NULL,0> and emits
 * the equivalent HTX trailers blocks. The output contents are emitted in <htx>,
 * and a positive value is returned if some bytes were emitted. In case of
 * error, a negative error code is returned. The caller must have verified that
 * the message in the buffer is compatible with receipt of trailers.
 *
 * The headers list <list> must be composed of :
 *   - n.name != NULL, n.len  > 0 : literal header name
 *   - n.name == NULL, n.len  > 0 : indexed pseudo header name number <n.len>
 *                                  among H2_PHDR_IDX_* (illegal here)
 *   - n.name ignored, n.len == 0 : end of list
 *   - in all cases except the end of list, v.name and v.len must designate a
 *     valid value.
 */
int h2_make_htx_trailers(struct http_hdr *list, struct htx *htx)
{
	uint32_t idx;
	int i;

	for (idx = 0; list[idx].n.len != 0; idx++) {
		if (!list[idx].n.ptr) {
			/* This is an indexed pseudo-header (RFC7540#8.1.2.1) */
			goto fail;
		}

		/* RFC7540#8.1.2: upper case not allowed in header field names */
		for (i = 0; i < list[idx].n.len; i++)
			if ((uint8_t)(list[idx].n.ptr[i] - 'A') < 'Z' - 'A')
				goto fail;

		if (h2_str_to_phdr(list[idx].n) != 0) {
			/* This is a pseudo-header (RFC7540#8.1.2.1) */
			goto fail;
		}

		/* these ones are forbidden in trailers (RFC7540#8.1.2.2) */
		if (isteq(list[idx].n, ist("host")) ||
		    isteq(list[idx].n, ist("content-length")) ||
		    isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("te")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			goto fail;

		if (!htx_add_trailer(htx, list[idx].n, list[idx].v))
			goto fail;
	}

	if (!htx_add_endof(htx, HTX_BLK_EOT))
		goto fail;

	return 1;

 fail:
	return -1;
}
