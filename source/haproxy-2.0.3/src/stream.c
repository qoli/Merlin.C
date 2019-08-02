/*
 * Stream management functions.
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <common/cfgparse.h>
#include <common/config.h>
#include <common/buffer.h>
#include <common/debug.h>
#include <common/hathreads.h>
#include <common/htx.h>
#include <common/initcall.h>
#include <common/memory.h>

#include <types/applet.h>
#include <types/capture.h>
#include <types/cli.h>
#include <types/filters.h>
#include <types/global.h>
#include <types/stats.h>

#include <proto/acl.h>
#include <proto/action.h>
#include <proto/activity.h>
#include <proto/arg.h>
#include <proto/backend.h>
#include <proto/channel.h>
#include <proto/checks.h>
#include <proto/cli.h>
#include <proto/connection.h>
#include <proto/dict.h>
#include <proto/dns.h>
#include <proto/stats.h>
#include <proto/fd.h>
#include <proto/filters.h>
#include <proto/freq_ctr.h>
#include <proto/frontend.h>
#include <proto/hdr_idx.h>
#include <proto/hlua.h>
#include <proto/http_rules.h>
#include <proto/listener.h>
#include <proto/log.h>
#include <proto/raw_sock.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/pipe.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/queue.h>
#include <proto/server.h>
#include <proto/sample.h>
#include <proto/stick_table.h>
#include <proto/stream_interface.h>
#include <proto/task.h>
#include <proto/tcp_rules.h>
#include <proto/vars.h>

DECLARE_POOL(pool_head_stream, "stream", sizeof(struct stream));

struct list streams = LIST_HEAD_INIT(streams);
__decl_spinlock(streams_lock);

/* List of all use-service keywords. */
static struct list service_keywords = LIST_HEAD_INIT(service_keywords);


/* Create a new stream for connection <conn>. Return < 0 on error. This is only
 * valid right after the handshake, before the connection's data layer is
 * initialized, because it relies on the session to be in conn->owner.
 */
int stream_create_from_cs(struct conn_stream *cs)
{
	struct stream *strm;

	strm = stream_new(cs->conn->owner, &cs->obj_type);
	if (strm == NULL)
		return -1;

	task_wakeup(strm->task, TASK_WOKEN_INIT);
	return 0;
}

/* Callback used to wake up a stream when an input buffer is available. The
 * stream <s>'s stream interfaces are checked for a failed buffer allocation
 * as indicated by the presence of the SI_FL_RXBLK_ROOM flag and the lack of a
 * buffer, and and input buffer is assigned there (at most one). The function
 * returns 1 and wakes the stream up if a buffer was taken, otherwise zero.
 * It's designed to be called from __offer_buffer().
 */
int stream_buf_available(void *arg)
{
	struct stream *s = arg;

	if (!s->req.buf.size && !s->req.pipe && (s->si[0].flags & SI_FL_RXBLK_BUFF) &&
	    b_alloc_margin(&s->req.buf, global.tune.reserved_bufs))
		si_rx_buff_rdy(&s->si[0]);
	else if (!s->res.buf.size && !s->res.pipe && (s->si[1].flags & SI_FL_RXBLK_BUFF) &&
		 b_alloc_margin(&s->res.buf, 0))
		si_rx_buff_rdy(&s->si[1]);
	else
		return 0;

	task_wakeup(s->task, TASK_WOKEN_RES);
	return 1;

}

/* This function is called from the session handler which detects the end of
 * handshake, in order to complete initialization of a valid stream. It must be
 * called with a completely initialized session. It returns the pointer to
 * the newly created stream, or NULL in case of fatal error. The client-facing
 * end point is assigned to <origin>, which must be valid. The stream's task
 * is configured with a nice value inherited from the listener's nice if any.
 * The task's context is set to the new stream, and its function is set to
 * process_stream(). Target and analysers are null.
 */
struct stream *stream_new(struct session *sess, enum obj_type *origin)
{
	struct stream *s;
	struct task *t;
	struct conn_stream *cs  = objt_cs(origin);
	struct appctx *appctx   = objt_appctx(origin);
	const struct cs_info *csinfo;

	if (unlikely((s = pool_alloc(pool_head_stream)) == NULL))
		goto out_fail_alloc;

	/* minimum stream initialization required for an embryonic stream is
	 * fairly low. We need very little to execute L4 ACLs, then we need a
	 * task to make the client-side connection live on its own.
	 *  - flags
	 *  - stick-entry tracking
	 */
	s->flags = 0;
	s->logs.logwait = sess->fe->to_log;
	s->logs.level = 0;
	tv_zero(&s->logs.tv_request);
	s->logs.t_queue = -1;
	s->logs.t_connect = -1;
	s->logs.t_data = -1;
	s->logs.t_close = 0;
	s->logs.bytes_in = s->logs.bytes_out = 0;
	s->logs.prx_queue_pos = 0;  /* we get the number of pending conns before us */
	s->logs.srv_queue_pos = 0; /* we will get this number soon */
	s->obj_type = OBJ_TYPE_STREAM;

	csinfo = si_get_cs_info(cs);
	if (csinfo) {
		s->logs.accept_date = csinfo->create_date;
		s->logs.tv_accept = csinfo->tv_create;
		s->logs.t_handshake = csinfo->t_handshake;
		s->logs.t_idle = csinfo->t_idle;
	}
	else {
		s->logs.accept_date = sess->accept_date;
		s->logs.tv_accept = sess->tv_accept;
		s->logs.t_handshake = sess->t_handshake;
		s->logs.t_idle = -1;
	}

	/* default logging function */
	s->do_log = strm_log;

	/* default error reporting function, may be changed by analysers */
	s->srv_error = default_srv_error;

	/* Initialise the current rule list pointer to NULL. We are sure that
	 * any rulelist match the NULL pointer.
	 */
	s->current_rule_list = NULL;
	s->current_rule = NULL;

	/* Copy SC counters for the stream. We don't touch refcounts because
	 * any reference we have is inherited from the session. Since the stream
	 * doesn't exist without the session, the session's existence guarantees
	 * we don't lose the entry. During the store operation, the stream won't
	 * touch these ones.
	 */
	memcpy(s->stkctr, sess->stkctr, sizeof(s->stkctr));

	s->sess = sess;
	s->si[0].flags = SI_FL_NONE;
	s->si[1].flags = SI_FL_ISBACK;

	s->uniq_id = _HA_ATOMIC_XADD(&global.req_count, 1);

	/* OK, we're keeping the stream, so let's properly initialize the stream */
	LIST_INIT(&s->back_refs);

	LIST_INIT(&s->buffer_wait.list);
	s->buffer_wait.target = s;
	s->buffer_wait.wakeup_cb = stream_buf_available;

	s->call_rate.curr_sec = s->call_rate.curr_ctr = s->call_rate.prev_ctr = 0;
	s->pcli_next_pid = 0;
	s->pcli_flags = 0;
	s->unique_id = NULL;

	if ((t = task_new(tid_bit)) == NULL)
		goto out_fail_alloc;

	s->task = t;
	s->pending_events = 0;
	t->process = process_stream;
	t->context = s;
	t->expire = TICK_ETERNITY;
	if (sess->listener)
		t->nice = sess->listener->nice;

	/* Note: initially, the stream's backend points to the frontend.
	 * This changes later when switching rules are executed or
	 * when the default backend is assigned.
	 */
	s->be  = sess->fe;
	s->req.buf = BUF_NULL;
	s->res.buf = BUF_NULL;
	s->req_cap = NULL;
	s->res_cap = NULL;

	/* Initialise all the variables contexts even if not used.
	 * This permits to prune these contexts without errors.
	 */
	vars_init(&s->vars_txn,    SCOPE_TXN);
	vars_init(&s->vars_reqres, SCOPE_REQ);

	/* this part should be common with other protocols */
	if (si_reset(&s->si[0]) < 0)
		goto out_fail_alloc;
	si_set_state(&s->si[0], SI_ST_EST);
	s->si[0].hcto = sess->fe->timeout.clientfin;

	if (cs && cs->conn->mux) {
		if (cs->conn->mux->flags & MX_FL_CLEAN_ABRT)
			s->si[0].flags |= SI_FL_CLEAN_ABRT;
		if (cs->conn->mux->flags & MX_FL_HTX)
			s->flags |= SF_HTX;
	}
	/* Set SF_HTX flag for HTX frontends. */
	if (sess->fe->mode == PR_MODE_HTTP && sess->fe->options2 & PR_O2_USE_HTX)
		s->flags |= SF_HTX;

	/* attach the incoming connection to the stream interface now. */
	if (cs)
		si_attach_cs(&s->si[0], cs);
	else if (appctx)
		si_attach_appctx(&s->si[0], appctx);

	if (likely(sess->fe->options2 & PR_O2_INDEPSTR))
		s->si[0].flags |= SI_FL_INDEP_STR;

	/* pre-initialize the other side's stream interface to an INIT state. The
	 * callbacks will be initialized before attempting to connect.
	 */
	if (si_reset(&s->si[1]) < 0)
		goto out_fail_alloc_si1;
	s->si[1].hcto = TICK_ETERNITY;

	if (likely(sess->fe->options2 & PR_O2_INDEPSTR))
		s->si[1].flags |= SI_FL_INDEP_STR;

	stream_init_srv_conn(s);
	s->target = sess->listener ? sess->listener->default_target : NULL;

	s->pend_pos = NULL;
	s->priority_class = 0;
	s->priority_offset = 0;

	/* init store persistence */
	s->store_count = 0;

	channel_init(&s->req);
	s->req.flags |= CF_READ_ATTACHED; /* the producer is already connected */
	s->req.analysers = sess->listener ? sess->listener->analysers : 0;

	if (!sess->fe->fe_req_ana) {
		channel_auto_connect(&s->req);  /* don't wait to establish connection */
		channel_auto_close(&s->req);    /* let the producer forward close requests */
	}

	s->req.rto = sess->fe->timeout.client;
	s->req.wto = TICK_ETERNITY;
	s->req.rex = TICK_ETERNITY;
	s->req.wex = TICK_ETERNITY;
	s->req.analyse_exp = TICK_ETERNITY;

	channel_init(&s->res);
	s->res.flags |= CF_ISRESP;
	s->res.analysers = 0;

	if (sess->fe->options2 & PR_O2_NODELAY) {
		s->req.flags |= CF_NEVER_WAIT;
		s->res.flags |= CF_NEVER_WAIT;
	}

	s->res.wto = sess->fe->timeout.client;
	s->res.rto = TICK_ETERNITY;
	s->res.rex = TICK_ETERNITY;
	s->res.wex = TICK_ETERNITY;
	s->res.analyse_exp = TICK_ETERNITY;

	s->txn = NULL;
	s->hlua = NULL;

	s->dns_ctx.dns_requester = NULL;
	s->dns_ctx.hostname_dn = NULL;
	s->dns_ctx.hostname_dn_len = 0;
	s->dns_ctx.parent = NULL;

	HA_SPIN_LOCK(STRMS_LOCK, &streams_lock);
	LIST_ADDQ(&streams, &s->list);
	HA_SPIN_UNLOCK(STRMS_LOCK, &streams_lock);

	if (flt_stream_init(s) < 0 || flt_stream_start(s) < 0)
		goto out_fail_accept;

	s->si[1].l7_buffer = BUF_NULL;
	/* finish initialization of the accepted file descriptor */
	if (appctx)
		si_want_get(&s->si[0]);

	if (sess->fe->accept && sess->fe->accept(s) < 0)
		goto out_fail_accept;

	/* it is important not to call the wakeup function directly but to
	 * pass through task_wakeup(), because this one knows how to apply
	 * priorities to tasks. Using multi thread we must be sure that
	 * stream is fully initialized before calling task_wakeup. So
	 * the caller must handle the task_wakeup
	 */
	return s;

	/* Error unrolling */
 out_fail_accept:
	flt_stream_release(s, 0);
	task_destroy(t);
	tasklet_free(s->si[1].wait_event.tasklet);
	LIST_DEL(&s->list);
out_fail_alloc_si1:
	tasklet_free(s->si[0].wait_event.tasklet);
 out_fail_alloc:
	pool_free(pool_head_stream, s);
	return NULL;
}

/*
 * frees  the context associated to a stream. It must have been removed first.
 */
static void stream_free(struct stream *s)
{
	struct session *sess = strm_sess(s);
	struct proxy *fe = sess->fe;
	struct bref *bref, *back;
	struct conn_stream *cli_cs = objt_cs(s->si[0].end);
	int must_free_sess;
	int i;

	/* detach the stream from its own task before even releasing it so
	 * that walking over a task list never exhibits a dying stream.
	 */
	s->task->context = NULL;
	__ha_barrier_store();

	pendconn_free(s);

	if (objt_server(s->target)) { /* there may be requests left pending in queue */
		if (s->flags & SF_CURR_SESS) {
			s->flags &= ~SF_CURR_SESS;
			_HA_ATOMIC_SUB(&__objt_server(s->target)->cur_sess, 1);
		}
		if (may_dequeue_tasks(objt_server(s->target), s->be))
			process_srv_queue(objt_server(s->target));
	}

	if (unlikely(s->srv_conn)) {
		/* the stream still has a reserved slot on a server, but
		 * it should normally be only the same as the one above,
		 * so this should not happen in fact.
		 */
		sess_change_server(s, NULL);
	}

	if (s->req.pipe)
		put_pipe(s->req.pipe);

	if (s->res.pipe)
		put_pipe(s->res.pipe);

	/* We may still be present in the buffer wait queue */
	if (!LIST_ISEMPTY(&s->buffer_wait.list)) {
		HA_SPIN_LOCK(BUF_WQ_LOCK, &buffer_wq_lock);
		LIST_DEL(&s->buffer_wait.list);
		LIST_INIT(&s->buffer_wait.list);
		HA_SPIN_UNLOCK(BUF_WQ_LOCK, &buffer_wq_lock);
	}
	if (s->req.buf.size || s->res.buf.size) {
		b_drop(&s->req.buf);
		b_drop(&s->res.buf);
		offer_buffers(NULL, tasks_run_queue);
	}

	pool_free(pool_head_uniqueid, s->unique_id);
	s->unique_id = NULL;

	hlua_ctx_destroy(s->hlua);
	s->hlua = NULL;
	if (s->txn)
		http_end_txn(s);

	/* ensure the client-side transport layer is destroyed */
	if (cli_cs)
		cs_close(cli_cs);

	for (i = 0; i < s->store_count; i++) {
		if (!s->store[i].ts)
			continue;
		stksess_free(s->store[i].table, s->store[i].ts);
		s->store[i].ts = NULL;
	}

	if (s->txn) {
		pool_free(pool_head_hdr_idx, s->txn->hdr_idx.v);
		pool_free(pool_head_http_txn, s->txn);
		s->txn = NULL;
	}

	if (s->dns_ctx.dns_requester) {
		free(s->dns_ctx.hostname_dn); s->dns_ctx.hostname_dn = NULL;
		s->dns_ctx.hostname_dn_len = 0;
		dns_unlink_resolution(s->dns_ctx.dns_requester);

		pool_free(dns_requester_pool, s->dns_ctx.dns_requester);
		s->dns_ctx.dns_requester = NULL;
	}

	flt_stream_stop(s);
	flt_stream_release(s, 0);

	if (fe) {
		pool_free(fe->rsp_cap_pool, s->res_cap);
		pool_free(fe->req_cap_pool, s->req_cap);
	}

	/* Cleanup all variable contexts. */
	if (!LIST_ISEMPTY(&s->vars_txn.head))
		vars_prune(&s->vars_txn, s->sess, s);
	if (!LIST_ISEMPTY(&s->vars_reqres.head))
		vars_prune(&s->vars_reqres, s->sess, s);

	stream_store_counters(s);

	HA_SPIN_LOCK(STRMS_LOCK, &streams_lock);
	list_for_each_entry_safe(bref, back, &s->back_refs, users) {
		/* we have to unlink all watchers. We must not relink them if
		 * this stream was the last one in the list.
		 */
		LIST_DEL(&bref->users);
		LIST_INIT(&bref->users);
		if (s->list.n != &streams)
			LIST_ADDQ(&LIST_ELEM(s->list.n, struct stream *, list)->back_refs, &bref->users);
		bref->ref = s->list.n;
	}
	LIST_DEL(&s->list);
	HA_SPIN_UNLOCK(STRMS_LOCK, &streams_lock);

	/* applets do not release session yet */
	must_free_sess = objt_appctx(sess->origin) && sess->origin == s->si[0].end;


	si_release_endpoint(&s->si[1]);
	si_release_endpoint(&s->si[0]);

	tasklet_free(s->si[0].wait_event.tasklet);
	tasklet_free(s->si[1].wait_event.tasklet);

	b_free(&s->si[1].l7_buffer);
	if (must_free_sess) {
		sess->origin = NULL;
		session_free(sess);
	}

	pool_free(pool_head_stream, s);

	/* We may want to free the maximum amount of pools if the proxy is stopping */
	if (fe && unlikely(fe->state == PR_STSTOPPED)) {
		pool_flush(pool_head_buffer);
		pool_flush(pool_head_http_txn);
		pool_flush(pool_head_hdr_idx);
		pool_flush(pool_head_requri);
		pool_flush(pool_head_capture);
		pool_flush(pool_head_stream);
		pool_flush(pool_head_session);
		pool_flush(pool_head_connection);
		pool_flush(pool_head_pendconn);
		pool_flush(fe->req_cap_pool);
		pool_flush(fe->rsp_cap_pool);
	}
}


/* Allocates a work buffer for stream <s>. It is meant to be called inside
 * process_stream(). It will only allocate the side needed for the function
 * to work fine, which is the response buffer so that an error message may be
 * built and returned. Response buffers may be allocated from the reserve, this
 * is critical to ensure that a response may always flow and will never block a
 * server from releasing a connection. Returns 0 in case of failure, non-zero
 * otherwise.
 */
static int stream_alloc_work_buffer(struct stream *s)
{
	if (!LIST_ISEMPTY(&s->buffer_wait.list)) {
		HA_SPIN_LOCK(BUF_WQ_LOCK, &buffer_wq_lock);
		LIST_DEL(&s->buffer_wait.list);
		LIST_INIT(&s->buffer_wait.list);
		HA_SPIN_UNLOCK(BUF_WQ_LOCK, &buffer_wq_lock);
	}

	if (b_alloc_margin(&s->res.buf, 0))
		return 1;

	HA_SPIN_LOCK(BUF_WQ_LOCK, &buffer_wq_lock);
	LIST_ADDQ(&buffer_wq, &s->buffer_wait.list);
	HA_SPIN_UNLOCK(BUF_WQ_LOCK, &buffer_wq_lock);
	return 0;
}

/* releases unused buffers after processing. Typically used at the end of the
 * update() functions. It will try to wake up as many tasks/applets as the
 * number of buffers that it releases. In practice, most often streams are
 * blocked on a single buffer, so it makes sense to try to wake two up when two
 * buffers are released at once.
 */
void stream_release_buffers(struct stream *s)
{
	int offer = 0;

	if (c_size(&s->req) && c_empty(&s->req)) {
		offer = 1;
		b_free(&s->req.buf);
	}
	if (c_size(&s->res) && c_empty(&s->res)) {
		offer = 1;
		b_free(&s->res.buf);
	}

	/* if we're certain to have at least 1 buffer available, and there is
	 * someone waiting, we can wake up a waiter and offer them.
	 */
	if (offer)
		offer_buffers(s, tasks_run_queue);
}

void stream_process_counters(struct stream *s)
{
	struct session *sess = s->sess;
	unsigned long long bytes;
	void *ptr1,*ptr2;
	struct stksess *ts;
	int i;

	bytes = s->req.total - s->logs.bytes_in;
	s->logs.bytes_in = s->req.total;
	if (bytes) {
		_HA_ATOMIC_ADD(&sess->fe->fe_counters.bytes_in, bytes);
		_HA_ATOMIC_ADD(&s->be->be_counters.bytes_in,    bytes);

		if (objt_server(s->target))
			_HA_ATOMIC_ADD(&objt_server(s->target)->counters.bytes_in, bytes);

		if (sess->listener && sess->listener->counters)
			_HA_ATOMIC_ADD(&sess->listener->counters->bytes_in, bytes);

		for (i = 0; i < MAX_SESS_STKCTR; i++) {
			struct stkctr *stkctr = &s->stkctr[i];

			ts = stkctr_entry(stkctr);
			if (!ts) {
				stkctr = &sess->stkctr[i];
				ts = stkctr_entry(stkctr);
				if (!ts)
					continue;
			}

			HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);
			ptr1 = stktable_data_ptr(stkctr->table, ts, STKTABLE_DT_BYTES_IN_CNT);
			if (ptr1)
				stktable_data_cast(ptr1, bytes_in_cnt) += bytes;

			ptr2 = stktable_data_ptr(stkctr->table, ts, STKTABLE_DT_BYTES_IN_RATE);
			if (ptr2)
				update_freq_ctr_period(&stktable_data_cast(ptr2, bytes_in_rate),
						       stkctr->table->data_arg[STKTABLE_DT_BYTES_IN_RATE].u, bytes);
			HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

			/* If data was modified, we need to touch to re-schedule sync */
			if (ptr1 || ptr2)
				stktable_touch_local(stkctr->table, ts, 0);
		}
	}

	bytes = s->res.total - s->logs.bytes_out;
	s->logs.bytes_out = s->res.total;
	if (bytes) {
		_HA_ATOMIC_ADD(&sess->fe->fe_counters.bytes_out, bytes);
		_HA_ATOMIC_ADD(&s->be->be_counters.bytes_out,    bytes);

		if (objt_server(s->target))
			_HA_ATOMIC_ADD(&objt_server(s->target)->counters.bytes_out, bytes);

		if (sess->listener && sess->listener->counters)
			_HA_ATOMIC_ADD(&sess->listener->counters->bytes_out, bytes);

		for (i = 0; i < MAX_SESS_STKCTR; i++) {
			struct stkctr *stkctr = &s->stkctr[i];

			ts = stkctr_entry(stkctr);
			if (!ts) {
				stkctr = &sess->stkctr[i];
				ts = stkctr_entry(stkctr);
				if (!ts)
					continue;
			}

			HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);
			ptr1 = stktable_data_ptr(stkctr->table, ts, STKTABLE_DT_BYTES_OUT_CNT);
			if (ptr1)
				stktable_data_cast(ptr1, bytes_out_cnt) += bytes;

			ptr2 = stktable_data_ptr(stkctr->table, ts, STKTABLE_DT_BYTES_OUT_RATE);
			if (ptr2)
				update_freq_ctr_period(&stktable_data_cast(ptr2, bytes_out_rate),
						       stkctr->table->data_arg[STKTABLE_DT_BYTES_OUT_RATE].u, bytes);
			HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

			/* If data was modified, we need to touch to re-schedule sync */
			if (ptr1 || ptr2)
				stktable_touch_local(stkctr->table, stkctr_entry(stkctr), 0);
		}
	}
}

/* This function is called with (si->state == SI_ST_CON) meaning that a
 * connection was attempted and that the file descriptor is already allocated.
 * We must check for timeout, error and abort. Possible output states are
 * SI_ST_CER (error), SI_ST_DIS (abort), and SI_ST_CON (no change). This only
 * works with connection-based streams. We know that there were no I/O event
 * when reaching this function. Timeouts and errors are *not* cleared.
 */
static void sess_update_st_con_tcp(struct stream *s)
{
	struct stream_interface *si = &s->si[1];
	struct channel *req = &s->req;
	struct channel *rep = &s->res;
	struct conn_stream *srv_cs = objt_cs(si->end);
	struct connection *conn = srv_cs ? srv_cs->conn : objt_conn(si->end);

	/* the client might want to abort */
	if ((rep->flags & CF_SHUTW) ||
	    ((req->flags & CF_SHUTW_NOW) &&
	     (channel_is_empty(req) || (s->be->options & PR_O_ABRT_CLOSE)))) {
		si->flags |= SI_FL_NOLINGER;
		si_shutw(si);
		si->err_type |= SI_ET_CONN_ABRT;
		if (s->srv_error)
			s->srv_error(s, si);
		/* Note: state = SI_ST_DIS now */
		return;
	}

	/* retryable error ? */
	if (si->flags & (SI_FL_EXP|SI_FL_ERR)) {
		if (!(s->flags & SF_SRV_REUSED) && conn) {
			conn_stop_tracking(conn);
			conn_full_close(conn);
		}

		if (!si->err_type) {
			if (si->flags & SI_FL_ERR)
				si->err_type = SI_ET_CONN_ERR;
			else
				si->err_type = SI_ET_CONN_TO;
		}

		si->state  = SI_ST_CER;
		return;
	}
}

/* This function is called with (si->state == SI_ST_CER) meaning that a
 * previous connection attempt has failed and that the file descriptor
 * has already been released. Possible causes include asynchronous error
 * notification and time out. Possible output states are SI_ST_CLO when
 * retries are exhausted, SI_ST_TAR when a delay is wanted before a new
 * connection attempt, SI_ST_ASS when it's wise to retry on the same server,
 * and SI_ST_REQ when an immediate redispatch is wanted. The buffers are
 * marked as in error state. Timeouts and errors are cleared before retrying.
 */
static void sess_update_st_cer(struct stream *s)
{
	struct stream_interface *si = &s->si[1];
	struct conn_stream *cs = objt_cs(si->end);
	struct connection *conn = cs_conn(cs);

	si->exp    = TICK_ETERNITY;
	si->flags &= ~SI_FL_EXP;

	/* we probably have to release last stream from the server */
	if (objt_server(s->target)) {
		health_adjust(objt_server(s->target), HANA_STATUS_L4_ERR);

		if (s->flags & SF_CURR_SESS) {
			s->flags &= ~SF_CURR_SESS;
			_HA_ATOMIC_SUB(&__objt_server(s->target)->cur_sess, 1);
		}

		if ((si->flags & SI_FL_ERR) &&
		    conn && conn->err_code == CO_ER_SSL_MISMATCH_SNI) {
			/* We tried to connect to a server which is configured
			 * with "verify required" and which doesn't have the
			 * "verifyhost" directive. The server presented a wrong
			 * certificate (a certificate for an unexpected name),
			 * which implies that we have used SNI in the handshake,
			 * and that the server doesn't have the associated cert
			 * and presented a default one.
			 *
			 * This is a serious enough issue not to retry. It's
			 * especially important because this wrong name might
			 * either be the result of a configuration error, and
			 * retrying will only hammer the server, or is caused
			 * by the use of a wrong SNI value, most likely
			 * provided by the client and we don't want to let the
			 * client provoke retries.
			 */
			si->conn_retries = 0;
		}
	}

	/* ensure that we have enough retries left */
	si->conn_retries--;
	if (si->conn_retries < 0 || !(s->be->retry_type & PR_RE_CONN_FAILED)) {
		if (!si->err_type) {
			si->err_type = SI_ET_CONN_ERR;
		}

		if (objt_server(s->target))
			_HA_ATOMIC_ADD(&objt_server(s->target)->counters.failed_conns, 1);
		_HA_ATOMIC_ADD(&s->be->be_counters.failed_conns, 1);
		sess_change_server(s, NULL);
		if (may_dequeue_tasks(objt_server(s->target), s->be))
			process_srv_queue(objt_server(s->target));

		/* shutw is enough so stop a connecting socket */
		si_shutw(si);
		s->req.flags |= CF_WRITE_ERROR;
		s->res.flags |= CF_READ_ERROR;

		si->state = SI_ST_CLO;
		if (s->srv_error)
			s->srv_error(s, si);
		return;
	}

	stream_choose_redispatch(s);

	if (si->flags & SI_FL_ERR) {
		/* The error was an asynchronous connection error, and we will
		 * likely have to retry connecting to the same server, most
		 * likely leading to the same result. To avoid this, we wait
		 * MIN(one second, connect timeout) before retrying. We don't
		 * do it when the failure happened on a reused connection
		 * though.
		 */

		int delay = 1000;

		if (s->be->timeout.connect && s->be->timeout.connect < delay)
			delay = s->be->timeout.connect;

		if (!si->err_type)
			si->err_type = SI_ET_CONN_ERR;

		/* only wait when we're retrying on the same server */
		if ((si->state == SI_ST_ASS ||
		     (s->be->lbprm.algo & BE_LB_KIND) != BE_LB_KIND_RR ||
		     (s->be->srv_act <= 1)) && !(s->flags & SF_SRV_REUSED)) {
			si->state = SI_ST_TAR;
			si->exp = tick_add(now_ms, MS_TO_TICKS(delay));
		}
		si->flags &= ~SI_FL_ERR;
	}
}

/* This function is called with (si->state == SI_ST_RDY) meaning that a
 * connection was attempted, that the file descriptor is already allocated,
 * and that it has succeeded. We must still check for errors and aborts.
 * Possible output states are SI_ST_EST (established), SI_ST_CER (error),
 * and SI_ST_DIS (abort). This only works with connection-based streams.
 * Timeouts and errors are *not* cleared.
 */
static void sess_update_st_rdy_tcp(struct stream *s)
{
	struct stream_interface *si = &s->si[1];
	struct channel *req = &s->req;
	struct channel *rep = &s->res;
	struct conn_stream *srv_cs = objt_cs(si->end);
	struct connection *conn = srv_cs ? srv_cs->conn : objt_conn(si->end);

	/* We know the connection at least succeeded, though it could have
	 * since met an error for any other reason. At least it didn't time out
	 * eventhough the timeout might have been reported right after success.
	 * We need to take care of various situations here :
	 *   - everything might be OK. We have to switch to established.
	 *   - an I/O error might have been reported after a successful transfer,
	 *     which is not retryable and needs to be logged correctly, and needs
	 *     established as well
	 *   - SI_ST_CON implies !CF_WROTE_DATA but not conversely as we could
	 *     have validated a connection with incoming data (e.g. TCP with a
	 *     banner protocol), or just a successful connect() probe.
	 *   - the client might have requested a connection abort, this needs to
	 *     be checked before we decide to retry anything.
	 */

	/* it's still possible to handle client aborts or connection retries
	 * before any data were sent.
	 */
	if (!(req->flags & CF_WROTE_DATA)) {
		/* client abort ? */
		if ((rep->flags & CF_SHUTW) ||
		    ((req->flags & CF_SHUTW_NOW) &&
		     (channel_is_empty(req) || (s->be->options & PR_O_ABRT_CLOSE)))) {
			/* give up */
			si->flags |= SI_FL_NOLINGER;
			si_shutw(si);
			si->err_type |= SI_ET_CONN_ABRT;
			if (s->srv_error)
				s->srv_error(s, si);
			return;
		}

		/* retryable error ? */
		if (si->flags & SI_FL_ERR) {
			if (!(s->flags & SF_SRV_REUSED) && conn) {
				conn_stop_tracking(conn);
				conn_full_close(conn);
			}

			if (!si->err_type)
				si->err_type = SI_ET_CONN_ERR;
			si->state = SI_ST_CER;
			return;
		}
	}

	/* data were sent and/or we had no error, sess_establish() will
	 * now take over.
	 */
	si->err_type = SI_ET_NONE;
	si->state    = SI_ST_EST;
}

/*
 * This function handles the transition between the SI_ST_CON state and the
 * SI_ST_EST state. It must only be called after switching from SI_ST_CON (or
 * SI_ST_INI or SI_ST_RDY) to SI_ST_EST, but only when a ->proto is defined.
 * Note that it will switch the interface to SI_ST_DIS if we already have
 * the CF_SHUTR flag, it means we were able to forward the request, and
 * receive the response, before process_stream() had the opportunity to
 * make the switch from SI_ST_CON to SI_ST_EST. When that happens, we want
 * to go through sess_establish() anyway, to make sure the analysers run.
 * Timeouts are cleared. Error are reported on the channel so that analysers
 * can handle them.
 */
static void sess_establish(struct stream *s)
{
	struct stream_interface *si = &s->si[1];
	struct conn_stream *srv_cs = objt_cs(si->end);
	struct connection *conn = srv_cs ? srv_cs->conn : objt_conn(si->end);
	struct channel *req = &s->req;
	struct channel *rep = &s->res;

	/* First, centralize the timers information, and clear any irrelevant
	 * timeout.
	 */
	s->logs.t_connect = tv_ms_elapsed(&s->logs.tv_accept, &now);
	si->exp = TICK_ETERNITY;
	si->flags &= ~SI_FL_EXP;

	/* errors faced after sending data need to be reported */
	if (si->flags & SI_FL_ERR && req->flags & CF_WROTE_DATA) {
		/* Don't add CF_WRITE_ERROR if we're here because
		 * early data were rejected by the server, or
		 * http_wait_for_response() will never be called
		 * to send a 425.
		 */
		if (conn && conn->err_code != CO_ER_SSL_EARLY_FAILED)
			req->flags |= CF_WRITE_ERROR;
		rep->flags |= CF_READ_ERROR;
		si->err_type = SI_ET_DATA_ERR;
	}

	/* If the request channel is waiting for the connect(), we mark the read
	 * side as attached on the response channel and we wake up it once. So
	 * it will have a chance to forward data now.
	 */
	if (req->flags & CF_WAKE_CONNECT) {
		req->flags |= CF_WAKE_ONCE;
		req->flags &= ~CF_WAKE_CONNECT;
	}

	if (objt_server(s->target))
		health_adjust(objt_server(s->target), HANA_STATUS_L4_OK);

	if (s->be->mode == PR_MODE_TCP) { /* let's allow immediate data connection in this case */
		/* if the user wants to log as soon as possible, without counting
		 * bytes from the server, then this is the right moment. */
		if (!LIST_ISEMPTY(&strm_fe(s)->logformat) && !(s->logs.logwait & LW_BYTES)) {
			/* note: no pend_pos here, session is established */
			s->logs.t_close = s->logs.t_connect; /* to get a valid end date */
			s->do_log(s);
		}
	}
	else {
		rep->flags |= CF_READ_DONTWAIT; /* a single read is enough to get response headers */
	}

	rep->analysers |= strm_fe(s)->fe_rsp_ana | s->be->be_rsp_ana;

	/* Be sure to filter response headers if the backend is an HTTP proxy
	 * and if there are filters attached to the stream. */
	if (s->be->mode == PR_MODE_HTTP && HAS_FILTERS(s))
		rep->analysers |= AN_RES_FLT_HTTP_HDRS;

	si_rx_endp_more(si);
	rep->flags |= CF_READ_ATTACHED; /* producer is now attached */
	if (objt_cs(si->end)) {
		/* real connections have timeouts */
		req->wto = s->be->timeout.server;
		rep->rto = s->be->timeout.server;
		/* The connection is now established, try to read data from the
		 * underlying layer, and subscribe to recv events. We use a
		 * delayed recv here to give a chance to the data to flow back
		 * by the time we process other tasks.
		 */
		si_chk_rcv(si);
	}
	req->wex = TICK_ETERNITY;
	/* If we managed to get the whole response, switch to SI_ST_DIS now. */
	if (rep->flags & CF_SHUTR)
		si->state = SI_ST_DIS;
}

/* Check if the connection request is in such a state that it can be aborted. */
static int check_req_may_abort(struct channel *req, struct stream *s)
{
	return ((req->flags & (CF_READ_ERROR)) ||
	        ((req->flags & (CF_SHUTW_NOW|CF_SHUTW)) &&  /* empty and client aborted */
	         (channel_is_empty(req) || (s->be->options & PR_O_ABRT_CLOSE))));
}

/* Update back stream interface status for input states SI_ST_ASS, SI_ST_QUE,
 * SI_ST_TAR. Other input states are simply ignored.
 * Possible output states are SI_ST_CLO, SI_ST_TAR, SI_ST_ASS, SI_ST_REQ, SI_ST_CON
 * and SI_ST_EST. Flags must have previously been updated for timeouts and other
 * conditions.
 */
static void sess_update_stream_int(struct stream *s)
{
	struct server *srv = objt_server(s->target);
	struct stream_interface *si = &s->si[1];
	struct channel *req = &s->req;

	DPRINTF(stderr,"[%u] %s: sess=%p rq=%p, rp=%p, exp(r,w)=%u,%u rqf=%08x rpf=%08x rqh=%lu rqt=%lu rph=%lu rpt=%lu cs=%d ss=%d\n",
		now_ms, __FUNCTION__,
		s,
		req, &s->res,
		req->rex, s->res.wex,
		req->flags, s->res.flags,
		ci_data(req), co_data(req), ci_data(&s->res), co_data(&s->res), s->si[0].state, s->si[1].state);

	if (si->state == SI_ST_ASS) {
		/* Server assigned to connection request, we have to try to connect now */
		int conn_err;

		/* Before we try to initiate the connection, see if the
		 * request may be aborted instead.
		 */
		if (check_req_may_abort(req, s)) {
			si->err_type |= SI_ET_CONN_ABRT;
			goto abort_connection;
		}

		conn_err = connect_server(s);
		srv = objt_server(s->target);

		if (conn_err == SF_ERR_NONE) {
			/* state = SI_ST_CON or SI_ST_EST now */
			if (srv)
				srv_inc_sess_ctr(srv);
			if (srv)
				srv_set_sess_last(srv);
			return;
		}

		/* We have received a synchronous error. We might have to
		 * abort, retry immediately or redispatch.
		 */
		if (conn_err == SF_ERR_INTERNAL) {
			if (!si->err_type) {
				si->err_type = SI_ET_CONN_OTHER;
			}

			if (srv)
				srv_inc_sess_ctr(srv);
			if (srv)
				srv_set_sess_last(srv);
			if (srv)
				_HA_ATOMIC_ADD(&srv->counters.failed_conns, 1);
			_HA_ATOMIC_ADD(&s->be->be_counters.failed_conns, 1);

			/* release other streams waiting for this server */
			sess_change_server(s, NULL);
			if (may_dequeue_tasks(srv, s->be))
				process_srv_queue(srv);

			/* Failed and not retryable. */
			si_shutr(si);
			si_shutw(si);
			req->flags |= CF_WRITE_ERROR;

			s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);

			/* we may need to know the position in the queue for logging */
			pendconn_cond_unlink(s->pend_pos);

			/* no stream was ever accounted for this server */
			si->state = SI_ST_CLO;
			if (s->srv_error)
				s->srv_error(s, si);
			return;
		}

		/* We are facing a retryable error, but we don't want to run a
		 * turn-around now, as the problem is likely a source port
		 * allocation problem, so we want to retry now.
		 */
		si->state = SI_ST_CER;
		si->flags &= ~SI_FL_ERR;
		sess_update_st_cer(s);
		/* now si->state is one of SI_ST_CLO, SI_ST_TAR, SI_ST_ASS, SI_ST_REQ */
		return;
	}
	else if (si->state == SI_ST_QUE) {
		/* connection request was queued, check for any update */
		if (!pendconn_dequeue(s)) {
			/* The connection is not in the queue anymore. Either
			 * we have a server connection slot available and we
			 * go directly to the assigned state, or we need to
			 * load-balance first and go to the INI state.
			 */
			si->exp = TICK_ETERNITY;
			if (unlikely(!(s->flags & SF_ASSIGNED)))
				si->state = SI_ST_REQ;
			else {
				s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);
				si->state = SI_ST_ASS;
			}
			return;
		}

		/* Connection request still in queue... */
		if (si->flags & SI_FL_EXP) {
			/* ... and timeout expired */
			si->exp = TICK_ETERNITY;
			si->flags &= ~SI_FL_EXP;
			s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);

			/* we may need to know the position in the queue for logging */
			pendconn_cond_unlink(s->pend_pos);

			if (srv)
				_HA_ATOMIC_ADD(&srv->counters.failed_conns, 1);
			_HA_ATOMIC_ADD(&s->be->be_counters.failed_conns, 1);
			si_shutr(si);
			si_shutw(si);
			req->flags |= CF_WRITE_TIMEOUT;
			if (!si->err_type)
				si->err_type = SI_ET_QUEUE_TO;
			si->state = SI_ST_CLO;
			if (s->srv_error)
				s->srv_error(s, si);
			return;
		}

		/* Connection remains in queue, check if we have to abort it */
		if (check_req_may_abort(req, s)) {
			s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);

			/* we may need to know the position in the queue for logging */
			pendconn_cond_unlink(s->pend_pos);

			si->err_type |= SI_ET_QUEUE_ABRT;
			goto abort_connection;
		}

		/* Nothing changed */
		return;
	}
	else if (si->state == SI_ST_TAR) {
		/* Connection request might be aborted */
		if (check_req_may_abort(req, s)) {
			si->err_type |= SI_ET_CONN_ABRT;
			goto abort_connection;
		}

		if (!(si->flags & SI_FL_EXP))
			return;  /* still in turn-around */

		si->flags &= ~SI_FL_EXP;
		si->exp = TICK_ETERNITY;

		/* we keep trying on the same server as long as the stream is
		 * marked "assigned".
		 * FIXME: Should we force a redispatch attempt when the server is down ?
		 */
		if (s->flags & SF_ASSIGNED)
			si->state = SI_ST_ASS;
		else
			si->state = SI_ST_REQ;
		return;
	}
	return;

abort_connection:
	/* give up */
	si->exp = TICK_ETERNITY;
	si->flags &= ~SI_FL_EXP;
	si_shutr(si);
	si_shutw(si);
	si->state = SI_ST_CLO;
	if (s->srv_error)
		s->srv_error(s, si);
	return;
}

/* Set correct stream termination flags in case no analyser has done it. It
 * also counts a failed request if the server state has not reached the request
 * stage.
 */
static void sess_set_term_flags(struct stream *s)
{
	if (!(s->flags & SF_FINST_MASK)) {
		if (s->si[1].state == SI_ST_INI) {
			/* anything before REQ in fact */
			_HA_ATOMIC_ADD(&strm_fe(s)->fe_counters.failed_req, 1);
			if (strm_li(s) && strm_li(s)->counters)
				_HA_ATOMIC_ADD(&strm_li(s)->counters->failed_req, 1);

			s->flags |= SF_FINST_R;
		}
		else if (s->si[1].state == SI_ST_QUE)
			s->flags |= SF_FINST_Q;
		else if (si_state_in(s->si[1].state, SI_SB_REQ|SI_SB_TAR|SI_SB_ASS|SI_SB_CON|SI_SB_CER|SI_SB_RDY))
			s->flags |= SF_FINST_C;
		else if (s->si[1].state == SI_ST_EST || s->si[1].prev_state == SI_ST_EST)
			s->flags |= SF_FINST_D;
		else
			s->flags |= SF_FINST_L;
	}
}

/* This function initiates a server connection request on a stream interface
 * already in SI_ST_REQ state. Upon success, the state goes to SI_ST_ASS for
 * a real connection to a server, indicating that a server has been assigned,
 * or SI_ST_EST for a successful connection to an applet. It may also return
 * SI_ST_QUE, or SI_ST_CLO upon error.
 */
static void sess_prepare_conn_req(struct stream *s)
{
	struct stream_interface *si = &s->si[1];

	DPRINTF(stderr,"[%u] %s: sess=%p rq=%p, rp=%p, exp(r,w)=%u,%u rqf=%08x rpf=%08x rqh=%lu rqt=%lu rph=%lu rpt=%lu cs=%d ss=%d\n",
		now_ms, __FUNCTION__,
		s,
		&s->req, &s->res,
		s->req.rex, s->res.wex,
		s->req.flags, s->res.flags,
		ci_data(&s->req), co_data(&s->req), ci_data(&s->res), co_data(&s->res), s->si[0].state, s->si[1].state);

	if (si->state != SI_ST_REQ)
		return;

	if (unlikely(obj_type(s->target) == OBJ_TYPE_APPLET)) {
		/* the applet directly goes to the EST state */
		struct appctx *appctx = objt_appctx(si->end);

		if (!appctx || appctx->applet != __objt_applet(s->target))
			appctx = si_register_handler(si, objt_applet(s->target));

		if (!appctx) {
			/* No more memory, let's immediately abort. Force the
			 * error code to ignore the ERR_LOCAL which is not a
			 * real error.
			 */
			s->flags &= ~(SF_ERR_MASK | SF_FINST_MASK);

			si_shutr(si);
			si_shutw(si);
			s->req.flags |= CF_WRITE_ERROR;
			si->err_type = SI_ET_CONN_RES;
			si->state = SI_ST_CLO;
			if (s->srv_error)
				s->srv_error(s, si);
			return;
		}

		if (tv_iszero(&s->logs.tv_request))
			s->logs.tv_request = now;
		s->logs.t_queue   = tv_ms_elapsed(&s->logs.tv_accept, &now);
		si->state         = SI_ST_EST;
		si->err_type      = SI_ET_NONE;
		be_set_sess_last(s->be);
		/* let sess_establish() finish the job */
		return;
	}

	/* Try to assign a server */
	if (srv_redispatch_connect(s) != 0) {
		/* We did not get a server. Either we queued the
		 * connection request, or we encountered an error.
		 */
		if (si->state == SI_ST_QUE)
			return;

		/* we did not get any server, let's check the cause */
		si_shutr(si);
		si_shutw(si);
		s->req.flags |= CF_WRITE_ERROR;
		if (!si->err_type)
			si->err_type = SI_ET_CONN_OTHER;
		si->state = SI_ST_CLO;
		if (s->srv_error)
			s->srv_error(s, si);
		return;
	}

	/* The server is assigned */
	s->logs.t_queue = tv_ms_elapsed(&s->logs.tv_accept, &now);
	si->state = SI_ST_ASS;
	be_set_sess_last(s->be);
}

/* This function parses the use-service action ruleset. It executes
 * the associated ACL and set an applet as a stream or txn final node.
 * it returns ACT_RET_ERR if an error occurs, the proxy left in
 * consistent state. It returns ACT_RET_STOP in succes case because
 * use-service must be a terminal action. Returns ACT_RET_YIELD
 * if the initialisation function require more data.
 */
enum act_return process_use_service(struct act_rule *rule, struct proxy *px,
                                    struct session *sess, struct stream *s, int flags)

{
	struct appctx *appctx;

	/* Initialises the applet if it is required. */
	if (flags & ACT_FLAG_FIRST) {
		/* Register applet. this function schedules the applet. */
		s->target = &rule->applet.obj_type;
		if (unlikely(!si_register_handler(&s->si[1], objt_applet(s->target))))
			return ACT_RET_ERR;

		/* Initialise the context. */
		appctx = si_appctx(&s->si[1]);
		memset(&appctx->ctx, 0, sizeof(appctx->ctx));
		appctx->rule = rule;
	}
	else
		appctx = si_appctx(&s->si[1]);

	/* Stops the applet sheduling, in case of the init function miss
	 * some data.
	 */
	si_stop_get(&s->si[1]);

	/* Call initialisation. */
	if (rule->applet.init)
		switch (rule->applet.init(appctx, px, s)) {
		case 0: return ACT_RET_ERR;
		case 1: break;
		default: return ACT_RET_YIELD;
	}

	if (rule->from != ACT_F_HTTP_REQ) {
		if (sess->fe == s->be) /* report it if the request was intercepted by the frontend */
			_HA_ATOMIC_ADD(&sess->fe->fe_counters.intercepted_req, 1);

		/* The flag SF_ASSIGNED prevent from server assignment. */
		s->flags |= SF_ASSIGNED;
	}

	/* Now we can schedule the applet. */
	si_cant_get(&s->si[1]);
	appctx_wakeup(appctx);
	return ACT_RET_STOP;
}

/* This stream analyser checks the switching rules and changes the backend
 * if appropriate. The default_backend rule is also considered, then the
 * target backend's forced persistence rules are also evaluated last if any.
 * It returns 1 if the processing can continue on next analysers, or zero if it
 * either needs more data or wants to immediately abort the request.
 */
static int process_switching_rules(struct stream *s, struct channel *req, int an_bit)
{
	struct persist_rule *prst_rule;
	struct session *sess = s->sess;
	struct proxy *fe = sess->fe;

	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;

	DPRINTF(stderr,"[%u] %s: stream=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%lu analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		ci_data(req),
		req->analysers);

	/* now check whether we have some switching rules for this request */
	if (!(s->flags & SF_BE_ASSIGNED)) {
		struct switching_rule *rule;

		list_for_each_entry(rule, &fe->switching_rules, list) {
			int ret = 1;

			if (rule->cond) {
				ret = acl_exec_cond(rule->cond, fe, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
				ret = acl_pass(ret);
				if (rule->cond->pol == ACL_COND_UNLESS)
					ret = !ret;
			}

			if (ret) {
				/* If the backend name is dynamic, try to resolve the name.
				 * If we can't resolve the name, or if any error occurs, break
				 * the loop and fallback to the default backend.
				 */
				struct proxy *backend = NULL;

				if (rule->dynamic) {
					struct buffer *tmp;

					tmp = alloc_trash_chunk();
					if (!tmp)
						goto sw_failed;

					if (build_logline(s, tmp->area, tmp->size, &rule->be.expr))
						backend = proxy_be_by_name(tmp->area);

					free_trash_chunk(tmp);
					tmp = NULL;

					if (!backend)
						break;
				}
				else
					backend = rule->be.backend;

				if (!stream_set_backend(s, backend))
					goto sw_failed;
				break;
			}
		}

		/* To ensure correct connection accounting on the backend, we
		 * have to assign one if it was not set (eg: a listen). This
		 * measure also takes care of correctly setting the default
		 * backend if any.
		 */
		if (!(s->flags & SF_BE_ASSIGNED))
			if (!stream_set_backend(s, fe->defbe.be ? fe->defbe.be : s->be))
				goto sw_failed;
	}

	/* we don't want to run the TCP or HTTP filters again if the backend has not changed */
	if (fe == s->be) {
		s->req.analysers &= ~AN_REQ_INSPECT_BE;
		s->req.analysers &= ~AN_REQ_HTTP_PROCESS_BE;
		s->req.analysers &= ~AN_REQ_FLT_START_BE;
	}

	/* as soon as we know the backend, we must check if we have a matching forced or ignored
	 * persistence rule, and report that in the stream.
	 */
	list_for_each_entry(prst_rule, &s->be->persist_rules, list) {
		int ret = 1;

		if (prst_rule->cond) {
	                ret = acl_exec_cond(prst_rule->cond, s->be, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (prst_rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			/* no rule, or the rule matches */
			if (prst_rule->type == PERSIST_TYPE_FORCE) {
				s->flags |= SF_FORCE_PRST;
			} else {
				s->flags |= SF_IGNORE_PRST;
			}
			break;
		}
	}

	return 1;

 sw_failed:
	/* immediately abort this request in case of allocation failure */
	channel_abort(&s->req);
	channel_abort(&s->res);

	if (!(s->flags & SF_ERR_MASK))
		s->flags |= SF_ERR_RESOURCE;
	if (!(s->flags & SF_FINST_MASK))
		s->flags |= SF_FINST_R;

	if (s->txn)
		s->txn->status = 500;
	s->req.analysers &= AN_REQ_FLT_END;
	s->req.analyse_exp = TICK_ETERNITY;
	return 0;
}

/* This stream analyser works on a request. It applies all use-server rules on
 * it then returns 1. The data must already be present in the buffer otherwise
 * they won't match. It always returns 1.
 */
static int process_server_rules(struct stream *s, struct channel *req, int an_bit)
{
	struct proxy *px = s->be;
	struct session *sess = s->sess;
	struct server_rule *rule;

	DPRINTF(stderr,"[%u] %s: stream=%p b=%p, exp(r,w)=%u,%u bf=%08x bl=%lu analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		c_data(req),
		req->analysers);

	if (!(s->flags & SF_ASSIGNED)) {
		list_for_each_entry(rule, &px->server_rules, list) {
			int ret;

			ret = acl_exec_cond(rule->cond, s->be, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;

			if (ret) {
				struct server *srv = rule->srv.ptr;

				if ((srv->cur_state != SRV_ST_STOPPED) ||
				    (px->options & PR_O_PERSIST) ||
				    (s->flags & SF_FORCE_PRST)) {
					s->flags |= SF_DIRECT | SF_ASSIGNED;
					s->target = &srv->obj_type;
					break;
				}
				/* if the server is not UP, let's go on with next rules
				 * just in case another one is suited.
				 */
			}
		}
	}

	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;
	return 1;
}

static inline void sticking_rule_find_target(struct stream *s,
                                             struct stktable *t, struct stksess *ts)
{
	struct proxy *px = s->be;
	struct eb32_node *node;
	struct dict_entry *de;
	void *ptr;
	struct server *srv;

	/* Look for the server name previously stored in <t> stick-table */
	HA_RWLOCK_RDLOCK(STK_SESS_LOCK, &ts->lock);
	ptr = __stktable_data_ptr(t, ts, STKTABLE_DT_SERVER_NAME);
	de = stktable_data_cast(ptr, server_name);
	HA_RWLOCK_RDUNLOCK(STK_SESS_LOCK, &ts->lock);

	if (de) {
		struct ebpt_node *name;

		name = ebis_lookup(&px->conf.used_server_name, de->value.key);
		if (name) {
			srv = container_of(name, struct server, conf.name);
			goto found;
		}
	}

	/* Look for the server ID */
	HA_RWLOCK_RDLOCK(STK_SESS_LOCK, &ts->lock);
	ptr = __stktable_data_ptr(t, ts, STKTABLE_DT_SERVER_ID);
	node = eb32_lookup(&px->conf.used_server_id, stktable_data_cast(ptr, server_id));
	HA_RWLOCK_RDUNLOCK(STK_SESS_LOCK, &ts->lock);

	if (!node)
		return;

	srv = container_of(node, struct server, conf.id);
 found:
	if ((srv->cur_state != SRV_ST_STOPPED) ||
	    (px->options & PR_O_PERSIST) || (s->flags & SF_FORCE_PRST)) {
		s->flags |= SF_DIRECT | SF_ASSIGNED;
		s->target = &srv->obj_type;
	}
}

/* This stream analyser works on a request. It applies all sticking rules on
 * it then returns 1. The data must already be present in the buffer otherwise
 * they won't match. It always returns 1.
 */
static int process_sticking_rules(struct stream *s, struct channel *req, int an_bit)
{
	struct proxy    *px   = s->be;
	struct session *sess  = s->sess;
	struct sticking_rule  *rule;

	DPRINTF(stderr,"[%u] %s: stream=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%lu analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		req,
		req->rex, req->wex,
		req->flags,
		ci_data(req),
		req->analysers);

	list_for_each_entry(rule, &px->sticking_rules, list) {
		int ret = 1 ;
		int i;

		/* Only the first stick store-request of each table is applied
		 * and other ones are ignored. The purpose is to allow complex
		 * configurations which look for multiple entries by decreasing
		 * order of precision and to stop at the first which matches.
		 * An example could be a store of the IP address from an HTTP
		 * header first, then from the source if not found.
		 */
		for (i = 0; i < s->store_count; i++) {
			if (rule->table.t == s->store[i].table)
				break;
		}

		if (i !=  s->store_count)
			continue;

		if (rule->cond) {
	                ret = acl_exec_cond(rule->cond, px, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
			ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			struct stktable_key *key;

			key = stktable_fetch_key(rule->table.t, px, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->expr, NULL);
			if (!key)
				continue;

			if (rule->flags & STK_IS_MATCH) {
				struct stksess *ts;

				if ((ts = stktable_lookup_key(rule->table.t, key)) != NULL) {
					if (!(s->flags & SF_ASSIGNED))
						sticking_rule_find_target(s, rule->table.t, ts);
					stktable_touch_local(rule->table.t, ts, 1);
				}
			}
			if (rule->flags & STK_IS_STORE) {
				if (s->store_count < (sizeof(s->store) / sizeof(s->store[0]))) {
					struct stksess *ts;

					ts = stksess_new(rule->table.t, key);
					if (ts) {
						s->store[s->store_count].table = rule->table.t;
						s->store[s->store_count++].ts = ts;
					}
				}
			}
		}
	}

	req->analysers &= ~an_bit;
	req->analyse_exp = TICK_ETERNITY;
	return 1;
}

/* This stream analyser works on a response. It applies all store rules on it
 * then returns 1. The data must already be present in the buffer otherwise
 * they won't match. It always returns 1.
 */
static int process_store_rules(struct stream *s, struct channel *rep, int an_bit)
{
	struct proxy    *px   = s->be;
	struct session *sess  = s->sess;
	struct sticking_rule  *rule;
	int i;
	int nbreq = s->store_count;

	DPRINTF(stderr,"[%u] %s: stream=%p b=%p, exp(r,w)=%u,%u bf=%08x bh=%lu analysers=%02x\n",
		now_ms, __FUNCTION__,
		s,
		rep,
		rep->rex, rep->wex,
		rep->flags,
		ci_data(rep),
		rep->analysers);

	list_for_each_entry(rule, &px->storersp_rules, list) {
		int ret = 1 ;

		/* Only the first stick store-response of each table is applied
		 * and other ones are ignored. The purpose is to allow complex
		 * configurations which look for multiple entries by decreasing
		 * order of precision and to stop at the first which matches.
		 * An example could be a store of a set-cookie value, with a
		 * fallback to a parameter found in a 302 redirect.
		 *
		 * The store-response rules are not allowed to override the
		 * store-request rules for the same table, but they may coexist.
		 * Thus we can have up to one store-request entry and one store-
		 * response entry for the same table at any time.
		 */
		for (i = nbreq; i < s->store_count; i++) {
			if (rule->table.t == s->store[i].table)
				break;
		}

		/* skip existing entries for this table */
		if (i < s->store_count)
			continue;

		if (rule->cond) {
	                ret = acl_exec_cond(rule->cond, px, sess, s, SMP_OPT_DIR_RES|SMP_OPT_FINAL);
	                ret = acl_pass(ret);
			if (rule->cond->pol == ACL_COND_UNLESS)
				ret = !ret;
		}

		if (ret) {
			struct stktable_key *key;

			key = stktable_fetch_key(rule->table.t, px, sess, s, SMP_OPT_DIR_RES|SMP_OPT_FINAL, rule->expr, NULL);
			if (!key)
				continue;

			if (s->store_count < (sizeof(s->store) / sizeof(s->store[0]))) {
				struct stksess *ts;

				ts = stksess_new(rule->table.t, key);
				if (ts) {
					s->store[s->store_count].table = rule->table.t;
					s->store[s->store_count++].ts = ts;
				}
			}
		}
	}

	/* process store request and store response */
	for (i = 0; i < s->store_count; i++) {
		struct stksess *ts;
		void *ptr;
		struct dict_entry *de;

		if (objt_server(s->target) && objt_server(s->target)->flags & SRV_F_NON_STICK) {
			stksess_free(s->store[i].table, s->store[i].ts);
			s->store[i].ts = NULL;
			continue;
		}

		ts = stktable_set_entry(s->store[i].table, s->store[i].ts);
		if (ts != s->store[i].ts) {
			/* the entry already existed, we can free ours */
			stksess_free(s->store[i].table, s->store[i].ts);
		}
		s->store[i].ts = NULL;

		HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);
		ptr = __stktable_data_ptr(s->store[i].table, ts, STKTABLE_DT_SERVER_ID);
		stktable_data_cast(ptr, server_id) = __objt_server(s->target)->puid;
		HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

		HA_RWLOCK_WRLOCK(STK_SESS_LOCK, &ts->lock);
		de = dict_insert(&server_name_dict, __objt_server(s->target)->id);
		if (de) {
			ptr = __stktable_data_ptr(s->store[i].table, ts, STKTABLE_DT_SERVER_NAME);
			stktable_data_cast(ptr, server_name) = de;
		}
		HA_RWLOCK_WRUNLOCK(STK_SESS_LOCK, &ts->lock);

		stktable_touch_local(s->store[i].table, ts, 1);
	}
	s->store_count = 0; /* everything is stored */

	rep->analysers &= ~an_bit;
	rep->analyse_exp = TICK_ETERNITY;
	return 1;
}

/* This macro is very specific to the function below. See the comments in
 * process_stream() below to understand the logic and the tests.
 */
#define UPDATE_ANALYSERS(real, list, back, flag) {			\
		list = (((list) & ~(flag)) | ~(back)) & (real);		\
		back = real;						\
		if (!(list))						\
			break;						\
		if (((list) ^ ((list) & ((list) - 1))) < (flag))	\
			continue;					\
}

/* These 2 following macros call an analayzer for the specified channel if the
 * right flag is set. The first one is used for "filterable" analyzers. If a
 * stream has some registered filters, pre and post analyaze callbacks are
 * called. The second are used for other analyzers (AN_REQ/RES_FLT_* and
 * AN_REQ/RES_HTTP_XFER_BODY) */
#define FLT_ANALYZE(strm, chn, fun, list, back, flag, ...)			\
	{									\
		if ((list) & (flag)) {						\
			if (HAS_FILTERS(strm)) {			        \
				if (!flt_pre_analyze((strm), (chn), (flag)))    \
					break;				        \
				if (!fun((strm), (chn), (flag), ##__VA_ARGS__))	\
					break;					\
				if (!flt_post_analyze((strm), (chn), (flag)))	\
					break;					\
			}							\
			else {							\
				if (!fun((strm), (chn), (flag), ##__VA_ARGS__))	\
					break;					\
			}							\
			UPDATE_ANALYSERS((chn)->analysers, (list),		\
					 (back), (flag));			\
		}								\
	}

#define ANALYZE(strm, chn, fun, list, back, flag, ...)			\
	{								\
		if ((list) & (flag)) {					\
			if (!fun((strm), (chn), (flag), ##__VA_ARGS__))	\
				break;					\
			UPDATE_ANALYSERS((chn)->analysers, (list),	\
					 (back), (flag));		\
		}							\
	}

/* Processes the client, server, request and response jobs of a stream task,
 * then puts it back to the wait queue in a clean state, or cleans up its
 * resources if it must be deleted. Returns in <next> the date the task wants
 * to be woken up, or TICK_ETERNITY. In order not to call all functions for
 * nothing too many times, the request and response buffers flags are monitored
 * and each function is called only if at least another function has changed at
 * least one flag it is interested in.
 */
struct task *process_stream(struct task *t, void *context, unsigned short state)
{
	struct server *srv;
	struct stream *s = context;
	struct session *sess = s->sess;
	unsigned int rqf_last, rpf_last;
	unsigned int rq_prod_last, rq_cons_last;
	unsigned int rp_cons_last, rp_prod_last;
	unsigned int req_ana_back;
	struct channel *req, *res;
	struct stream_interface *si_f, *si_b;
	unsigned int rate;

	activity[tid].stream++;

	req = &s->req;
	res = &s->res;

	si_f = &s->si[0];
	si_b = &s->si[1];

	/* First, attempt to receive pending data from I/O layers */
	si_sync_recv(si_f);
	si_sync_recv(si_b);

	rate = update_freq_ctr(&s->call_rate, 1);
	if (rate >= 100000 && s->call_rate.prev_ctr) { // make sure to wait at least a full second
		stream_dump_and_crash(&s->obj_type, read_freq_ctr(&s->call_rate));
	}

	//DPRINTF(stderr, "%s:%d: cs=%d ss=%d(%d) rqf=0x%08x rpf=0x%08x\n", __FUNCTION__, __LINE__,
	//        si_f->state, si_b->state, si_b->err_type, req->flags, res->flags);

	/* this data may be no longer valid, clear it */
	if (s->txn)
		memset(&s->txn->auth, 0, sizeof(s->txn->auth));

	/* This flag must explicitly be set every time */
	req->flags &= ~(CF_READ_NOEXP|CF_WAKE_WRITE);
	res->flags &= ~(CF_READ_NOEXP|CF_WAKE_WRITE);

	/* Keep a copy of req/rep flags so that we can detect shutdowns */
	rqf_last = req->flags & ~CF_MASK_ANALYSER;
	rpf_last = res->flags & ~CF_MASK_ANALYSER;

	/* we don't want the stream interface functions to recursively wake us up */
	si_f->flags |= SI_FL_DONT_WAKE;
	si_b->flags |= SI_FL_DONT_WAKE;

	/* update pending events */
	s->pending_events |= (state & TASK_WOKEN_ANY);

	/* 1a: Check for low level timeouts if needed. We just set a flag on
	 * stream interfaces when their timeouts have expired.
	 */
	if (unlikely(s->pending_events & TASK_WOKEN_TIMER)) {
		si_check_timeouts(si_f);
		si_check_timeouts(si_b);

		/* check channel timeouts, and close the corresponding stream interfaces
		 * for future reads or writes. Note: this will also concern upper layers
		 * but we do not touch any other flag. We must be careful and correctly
		 * detect state changes when calling them.
		 */

		channel_check_timeouts(req);

		if (unlikely((req->flags & (CF_SHUTW|CF_WRITE_TIMEOUT)) == CF_WRITE_TIMEOUT)) {
			si_b->flags |= SI_FL_NOLINGER;
			si_shutw(si_b);
		}

		if (unlikely((req->flags & (CF_SHUTR|CF_READ_TIMEOUT)) == CF_READ_TIMEOUT)) {
			if (si_f->flags & SI_FL_NOHALF)
				si_f->flags |= SI_FL_NOLINGER;
			si_shutr(si_f);
		}

		channel_check_timeouts(res);

		if (unlikely((res->flags & (CF_SHUTW|CF_WRITE_TIMEOUT)) == CF_WRITE_TIMEOUT)) {
			si_f->flags |= SI_FL_NOLINGER;
			si_shutw(si_f);
		}

		if (unlikely((res->flags & (CF_SHUTR|CF_READ_TIMEOUT)) == CF_READ_TIMEOUT)) {
			if (si_b->flags & SI_FL_NOHALF)
				si_b->flags |= SI_FL_NOLINGER;
			si_shutr(si_b);
		}

		if (HAS_FILTERS(s))
			flt_stream_check_timeouts(s);

		/* Once in a while we're woken up because the task expires. But
		 * this does not necessarily mean that a timeout has been reached.
		 * So let's not run a whole stream processing if only an expiration
		 * timeout needs to be refreshed.
		 */
		if (!((req->flags | res->flags) &
		      (CF_SHUTR|CF_READ_ACTIVITY|CF_READ_TIMEOUT|CF_SHUTW|
		       CF_WRITE_ACTIVITY|CF_WRITE_TIMEOUT|CF_ANA_TIMEOUT)) &&
		    !((si_f->flags | si_b->flags) & (SI_FL_EXP|SI_FL_ERR)) &&
		    ((s->pending_events & TASK_WOKEN_ANY) == TASK_WOKEN_TIMER)) {
			si_f->flags &= ~SI_FL_DONT_WAKE;
			si_b->flags &= ~SI_FL_DONT_WAKE;
			goto update_exp_and_leave;
		}
	}

 resync_stream_interface:
	/* below we may emit error messages so we have to ensure that we have
	 * our buffers properly allocated.
	 */
	if (!stream_alloc_work_buffer(s)) {
		/* No buffer available, we've been subscribed to the list of
		 * buffer waiters, let's wait for our turn.
		 */
		si_f->flags &= ~SI_FL_DONT_WAKE;
		si_b->flags &= ~SI_FL_DONT_WAKE;
		goto update_exp_and_leave;
	}

	/* 1b: check for low-level errors reported at the stream interface.
	 * First we check if it's a retryable error (in which case we don't
	 * want to tell the buffer). Otherwise we report the error one level
	 * upper by setting flags into the buffers. Note that the side towards
	 * the client cannot have connect (hence retryable) errors. Also, the
	 * connection setup code must be able to deal with any type of abort.
	 */
	srv = objt_server(s->target);
	if (unlikely(si_f->flags & SI_FL_ERR)) {
		if (si_state_in(si_f->state, SI_SB_EST|SI_SB_DIS)) {
			si_shutr(si_f);
			si_shutw(si_f);
			si_report_error(si_f);
			if (!(req->analysers) && !(res->analysers)) {
				_HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				if (!(s->flags & SF_ERR_MASK))
					s->flags |= SF_ERR_CLICL;
				if (!(s->flags & SF_FINST_MASK))
					s->flags |= SF_FINST_D;
			}
		}
	}

	if (unlikely(si_b->flags & SI_FL_ERR)) {
		if (si_state_in(si_b->state, SI_SB_EST|SI_SB_DIS)) {
			si_shutr(si_b);
			si_shutw(si_b);
			si_report_error(si_b);
			_HA_ATOMIC_ADD(&s->be->be_counters.failed_resp, 1);
			if (srv)
				_HA_ATOMIC_ADD(&srv->counters.failed_resp, 1);
			if (!(req->analysers) && !(res->analysers)) {
				_HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				if (!(s->flags & SF_ERR_MASK))
					s->flags |= SF_ERR_SRVCL;
				if (!(s->flags & SF_FINST_MASK))
					s->flags |= SF_FINST_D;
			}
		}
		/* note: maybe we should process connection errors here ? */
	}

	if (si_state_in(si_b->state, SI_SB_CON|SI_SB_RDY)) {
		/* we were trying to establish a connection on the server side,
		 * maybe it succeeded, maybe it failed, maybe we timed out, ...
		 */
		if (si_b->state == SI_ST_RDY)
			sess_update_st_rdy_tcp(s);
		else if (si_b->state == SI_ST_CON)
			sess_update_st_con_tcp(s);

		if (si_b->state == SI_ST_CER)
			sess_update_st_cer(s);
		else if (si_b->state == SI_ST_EST)
			sess_establish(s);

		/* state is now one of SI_ST_CON (still in progress), SI_ST_EST
		 * (established), SI_ST_DIS (abort), SI_ST_CLO (last error),
		 * SI_ST_ASS/SI_ST_TAR/SI_ST_REQ for retryable errors.
		 */
	}

	rq_prod_last = si_f->state;
	rq_cons_last = si_b->state;
	rp_cons_last = si_f->state;
	rp_prod_last = si_b->state;

	/* Check for connection closure */

	DPRINTF(stderr,
		"[%u] %s:%d: task=%p s=%p, sfl=0x%08x, rq=%p, rp=%p, exp(r,w)=%u,%u rqf=%08x rpf=%08x rqh=%lu rqt=%lu rph=%lu rpt=%lu cs=%d ss=%d, cet=0x%x set=0x%x retr=%d\n",
		now_ms, __FUNCTION__, __LINE__,
		t,
		s, s->flags,
		req, res,
		req->rex, res->wex,
		req->flags, res->flags,
		ci_data(req), co_data(req), ci_data(res), co_data(res), si_f->state, si_b->state,
		si_f->err_type, si_b->err_type,
		si_b->conn_retries);

	/* nothing special to be done on client side */
	if (unlikely(si_f->state == SI_ST_DIS))
		si_f->state = SI_ST_CLO;

	/* When a server-side connection is released, we have to count it and
	 * check for pending connections on this server.
	 */
	if (unlikely(si_b->state == SI_ST_DIS)) {
		si_b->state = SI_ST_CLO;
		srv = objt_server(s->target);
		if (srv) {
			if (s->flags & SF_CURR_SESS) {
				s->flags &= ~SF_CURR_SESS;
				_HA_ATOMIC_SUB(&srv->cur_sess, 1);
			}
			sess_change_server(s, NULL);
			if (may_dequeue_tasks(srv, s->be))
				process_srv_queue(srv);
		}
	}

	/*
	 * Note: of the transient states (REQ, CER, DIS), only REQ may remain
	 * at this point.
	 */

 resync_request:
	/* Analyse request */
	if (((req->flags & ~rqf_last) & CF_MASK_ANALYSER) ||
	    ((req->flags ^ rqf_last) & CF_MASK_STATIC) ||
	    (req->analysers && (req->flags & CF_SHUTW)) ||
	    si_f->state != rq_prod_last ||
	    si_b->state != rq_cons_last ||
	    s->pending_events & TASK_WOKEN_MSG) {
		unsigned int flags = req->flags;

		if (si_state_in(si_f->state, SI_SB_EST|SI_SB_DIS|SI_SB_CLO)) {
			int max_loops = global.tune.maxpollevents;
			unsigned int ana_list;
			unsigned int ana_back;

			/* it's up to the analysers to stop new connections,
			 * disable reading or closing. Note: if an analyser
			 * disables any of these bits, it is responsible for
			 * enabling them again when it disables itself, so
			 * that other analysers are called in similar conditions.
			 */
			channel_auto_read(req);
			channel_auto_connect(req);
			channel_auto_close(req);

			/* We will call all analysers for which a bit is set in
			 * req->analysers, following the bit order from LSB
			 * to MSB. The analysers must remove themselves from
			 * the list when not needed. Any analyser may return 0
			 * to break out of the loop, either because of missing
			 * data to take a decision, or because it decides to
			 * kill the stream. We loop at least once through each
			 * analyser, and we may loop again if other analysers
			 * are added in the middle.
			 *
			 * We build a list of analysers to run. We evaluate all
			 * of these analysers in the order of the lower bit to
			 * the higher bit. This ordering is very important.
			 * An analyser will often add/remove other analysers,
			 * including itself. Any changes to itself have no effect
			 * on the loop. If it removes any other analysers, we
			 * want those analysers not to be called anymore during
			 * this loop. If it adds an analyser that is located
			 * after itself, we want it to be scheduled for being
			 * processed during the loop. If it adds an analyser
			 * which is located before it, we want it to switch to
			 * it immediately, even if it has already been called
			 * once but removed since.
			 *
			 * In order to achieve this, we compare the analyser
			 * list after the call with a copy of it before the
			 * call. The work list is fed with analyser bits that
			 * appeared during the call. Then we compare previous
			 * work list with the new one, and check the bits that
			 * appeared. If the lowest of these bits is lower than
			 * the current bit, it means we have enabled a previous
			 * analyser and must immediately loop again.
			 */

			ana_list = ana_back = req->analysers;
			while (ana_list && max_loops--) {
				/* Warning! ensure that analysers are always placed in ascending order! */
				ANALYZE    (s, req, flt_start_analyze,          ana_list, ana_back, AN_REQ_FLT_START_FE);
				FLT_ANALYZE(s, req, tcp_inspect_request,        ana_list, ana_back, AN_REQ_INSPECT_FE);
				FLT_ANALYZE(s, req, http_wait_for_request,      ana_list, ana_back, AN_REQ_WAIT_HTTP);
				FLT_ANALYZE(s, req, http_wait_for_request_body, ana_list, ana_back, AN_REQ_HTTP_BODY);
				FLT_ANALYZE(s, req, http_process_req_common,    ana_list, ana_back, AN_REQ_HTTP_PROCESS_FE, sess->fe);
				FLT_ANALYZE(s, req, process_switching_rules,    ana_list, ana_back, AN_REQ_SWITCHING_RULES);
				ANALYZE    (s, req, flt_start_analyze,          ana_list, ana_back, AN_REQ_FLT_START_BE);
				FLT_ANALYZE(s, req, tcp_inspect_request,        ana_list, ana_back, AN_REQ_INSPECT_BE);
				FLT_ANALYZE(s, req, http_process_req_common,    ana_list, ana_back, AN_REQ_HTTP_PROCESS_BE, s->be);
				FLT_ANALYZE(s, req, http_process_tarpit,        ana_list, ana_back, AN_REQ_HTTP_TARPIT);
				FLT_ANALYZE(s, req, process_server_rules,       ana_list, ana_back, AN_REQ_SRV_RULES);
				FLT_ANALYZE(s, req, http_process_request,       ana_list, ana_back, AN_REQ_HTTP_INNER);
				FLT_ANALYZE(s, req, tcp_persist_rdp_cookie,     ana_list, ana_back, AN_REQ_PRST_RDP_COOKIE);
				FLT_ANALYZE(s, req, process_sticking_rules,     ana_list, ana_back, AN_REQ_STICKING_RULES);
				ANALYZE    (s, req, flt_analyze_http_headers,   ana_list, ana_back, AN_REQ_FLT_HTTP_HDRS);
				ANALYZE    (s, req, http_request_forward_body,  ana_list, ana_back, AN_REQ_HTTP_XFER_BODY);
				ANALYZE    (s, req, pcli_wait_for_request,      ana_list, ana_back, AN_REQ_WAIT_CLI);
				ANALYZE    (s, req, flt_xfer_data,              ana_list, ana_back, AN_REQ_FLT_XFER_DATA);
				ANALYZE    (s, req, flt_end_analyze,            ana_list, ana_back, AN_REQ_FLT_END);
				break;
			}
		}

		rq_prod_last = si_f->state;
		rq_cons_last = si_b->state;
		req->flags &= ~CF_WAKE_ONCE;
		rqf_last = req->flags;

		if ((req->flags ^ flags) & (CF_SHUTR|CF_SHUTW))
			goto resync_request;
	}

	/* we'll monitor the request analysers while parsing the response,
	 * because some response analysers may indirectly enable new request
	 * analysers (eg: HTTP keep-alive).
	 */
	req_ana_back = req->analysers;

 resync_response:
	/* Analyse response */

	if (((res->flags & ~rpf_last) & CF_MASK_ANALYSER) ||
		 (res->flags ^ rpf_last) & CF_MASK_STATIC ||
		 (res->analysers && (res->flags & CF_SHUTW)) ||
		 si_f->state != rp_cons_last ||
		 si_b->state != rp_prod_last ||
		 s->pending_events & TASK_WOKEN_MSG) {
		unsigned int flags = res->flags;

		if (si_state_in(si_b->state, SI_SB_EST|SI_SB_DIS|SI_SB_CLO)) {
			int max_loops = global.tune.maxpollevents;
			unsigned int ana_list;
			unsigned int ana_back;

			/* it's up to the analysers to stop disable reading or
			 * closing. Note: if an analyser disables any of these
			 * bits, it is responsible for enabling them again when
			 * it disables itself, so that other analysers are called
			 * in similar conditions.
			 */
			channel_auto_read(res);
			channel_auto_close(res);

			/* We will call all analysers for which a bit is set in
			 * res->analysers, following the bit order from LSB
			 * to MSB. The analysers must remove themselves from
			 * the list when not needed. Any analyser may return 0
			 * to break out of the loop, either because of missing
			 * data to take a decision, or because it decides to
			 * kill the stream. We loop at least once through each
			 * analyser, and we may loop again if other analysers
			 * are added in the middle.
			 */

			ana_list = ana_back = res->analysers;
			while (ana_list && max_loops--) {
				/* Warning! ensure that analysers are always placed in ascending order! */
				ANALYZE    (s, res, flt_start_analyze,          ana_list, ana_back, AN_RES_FLT_START_FE);
				ANALYZE    (s, res, flt_start_analyze,          ana_list, ana_back, AN_RES_FLT_START_BE);
				FLT_ANALYZE(s, res, tcp_inspect_response,       ana_list, ana_back, AN_RES_INSPECT);
				FLT_ANALYZE(s, res, http_wait_for_response,     ana_list, ana_back, AN_RES_WAIT_HTTP);
				FLT_ANALYZE(s, res, process_store_rules,        ana_list, ana_back, AN_RES_STORE_RULES);
				FLT_ANALYZE(s, res, http_process_res_common,    ana_list, ana_back, AN_RES_HTTP_PROCESS_BE, s->be);
				ANALYZE    (s, res, flt_analyze_http_headers,   ana_list, ana_back, AN_RES_FLT_HTTP_HDRS);
				ANALYZE    (s, res, http_response_forward_body, ana_list, ana_back, AN_RES_HTTP_XFER_BODY);
				ANALYZE    (s, res, pcli_wait_for_response,     ana_list, ana_back, AN_RES_WAIT_CLI);
				ANALYZE    (s, res, flt_xfer_data,              ana_list, ana_back, AN_RES_FLT_XFER_DATA);
				ANALYZE    (s, res, flt_end_analyze,            ana_list, ana_back, AN_RES_FLT_END);
				break;
			}
		}

		rp_cons_last = si_f->state;
		rp_prod_last = si_b->state;
		res->flags &= ~CF_WAKE_ONCE;
		rpf_last = res->flags;

		if ((res->flags ^ flags) & (CF_SHUTR|CF_SHUTW))
			goto resync_response;
	}

	/* maybe someone has added some request analysers, so we must check and loop */
	if (req->analysers & ~req_ana_back)
		goto resync_request;

	if ((req->flags & ~rqf_last) & CF_MASK_ANALYSER)
		goto resync_request;

	/* FIXME: here we should call protocol handlers which rely on
	 * both buffers.
	 */


	/*
	 * Now we propagate unhandled errors to the stream. Normally
	 * we're just in a data phase here since it means we have not
	 * seen any analyser who could set an error status.
	 */
	srv = objt_server(s->target);
	if (unlikely(!(s->flags & SF_ERR_MASK))) {
		if (req->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) {
			/* Report it if the client got an error or a read timeout expired */
			req->analysers = 0;
			if (req->flags & CF_READ_ERROR) {
				_HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLICL;
			}
			else if (req->flags & CF_READ_TIMEOUT) {
				_HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLITO;
			}
			else if (req->flags & CF_WRITE_ERROR) {
				_HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVCL;
			}
			else {
				_HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVTO;
			}
			sess_set_term_flags(s);

			/* Abort the request if a client error occurred while
			 * the backend stream-interface is in the SI_ST_INI
			 * state. It is switched into the SI_ST_CLO state and
			 * the request channel is erased. */
			if (si_b->state == SI_ST_INI) {
				si_b->state = SI_ST_CLO;
				channel_abort(req);
				if (IS_HTX_STRM(s))
					channel_htx_erase(req, htxbuf(&req->buf));
				else
					channel_erase(req);
			}
		}
		else if (res->flags & (CF_READ_ERROR|CF_READ_TIMEOUT|CF_WRITE_ERROR|CF_WRITE_TIMEOUT)) {
			/* Report it if the server got an error or a read timeout expired */
			res->analysers = 0;
			if (res->flags & CF_READ_ERROR) {
				_HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVCL;
			}
			else if (res->flags & CF_READ_TIMEOUT) {
				_HA_ATOMIC_ADD(&s->be->be_counters.srv_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.srv_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.srv_aborts, 1);
				s->flags |= SF_ERR_SRVTO;
			}
			else if (res->flags & CF_WRITE_ERROR) {
				_HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLICL;
			}
			else {
				_HA_ATOMIC_ADD(&s->be->be_counters.cli_aborts, 1);
				_HA_ATOMIC_ADD(&sess->fe->fe_counters.cli_aborts, 1);
				if (srv)
					_HA_ATOMIC_ADD(&srv->counters.cli_aborts, 1);
				s->flags |= SF_ERR_CLITO;
			}
			sess_set_term_flags(s);
		}
	}

	/*
	 * Here we take care of forwarding unhandled data. This also includes
	 * connection establishments and shutdown requests.
	 */


	/* If noone is interested in analysing data, it's time to forward
	 * everything. We configure the buffer to forward indefinitely.
	 * Note that we're checking CF_SHUTR_NOW as an indication of a possible
	 * recent call to channel_abort().
	 */
	if (unlikely((!req->analysers || (req->analysers == AN_REQ_FLT_END && !(req->flags & CF_FLT_ANALYZE))) &&
	    !(req->flags & (CF_SHUTW|CF_SHUTR_NOW)) &&
	    (si_state_in(si_f->state, SI_SB_EST|SI_SB_DIS|SI_SB_CLO)) &&
	    (req->to_forward != CHN_INFINITE_FORWARD))) {
		/* This buffer is freewheeling, there's no analyser
		 * attached to it. If any data are left in, we'll permit them to
		 * move.
		 */
		channel_auto_read(req);
		channel_auto_connect(req);
		channel_auto_close(req);

		if (IS_HTX_STRM(s)) {
			struct htx *htx = htxbuf(&req->buf);

			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			co_set_data(req, htx->data);
			if (!(req->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_htx_forward_forever(req, htx);
		}
		else {
			/* We'll let data flow between the producer (if still connected)
			 * to the consumer (which might possibly not be connected yet).
			 */
			c_adv(req, ci_data(req));
			if (!(req->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_forward_forever(req);

			/* Just in order to support fetching HTTP contents after start
			 * of forwarding when the HTTP forwarding analyser is not used,
			 * we simply reset msg->sov so that HTTP rewinding points to the
			 * headers.
			 */
			if (s->txn)
				s->txn->req.sov = s->txn->req.eoh + s->txn->req.eol - co_data(req);
		}
	}

	/* check if it is wise to enable kernel splicing to forward request data */
	if (!(req->flags & (CF_KERN_SPLICING|CF_SHUTR)) &&
	    req->to_forward &&
	    (global.tune.options & GTUNE_USE_SPLICE) &&
	    (objt_cs(si_f->end) && __objt_cs(si_f->end)->conn->xprt && __objt_cs(si_f->end)->conn->xprt->rcv_pipe) &&
	    (objt_cs(si_b->end) && __objt_cs(si_b->end)->conn->xprt && __objt_cs(si_b->end)->conn->xprt->snd_pipe) &&
	    (pipes_used < global.maxpipes) &&
	    (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_REQ) ||
	     (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_AUT) &&
	      (req->flags & CF_STREAMER_FAST)))) {
		req->flags |= CF_KERN_SPLICING;
	}

	/* reflect what the L7 analysers have seen last */
	rqf_last = req->flags;

	/* it's possible that an upper layer has requested a connection setup or abort.
	 * There are 2 situations where we decide to establish a new connection :
	 *  - there are data scheduled for emission in the buffer
	 *  - the CF_AUTO_CONNECT flag is set (active connection)
	 */
	if (si_b->state == SI_ST_INI) {
		if (!(req->flags & CF_SHUTW)) {
			if ((req->flags & CF_AUTO_CONNECT) || !channel_is_empty(req)) {
				/* If we have an appctx, there is no connect method, so we
				 * immediately switch to the connected state, otherwise we
				 * perform a connection request.
				 */
				si_b->state = SI_ST_REQ; /* new connection requested */
				si_b->conn_retries = s->be->conn_retries;
				if ((s->be->retry_type &~ PR_RE_CONN_FAILED) &&
				    !(si_b->flags & SI_FL_D_L7_RETRY))
					si_b->flags |= SI_FL_L7_RETRY;
			}
		}
		else {
			si_release_endpoint(si_b);
			si_b->state = SI_ST_CLO; /* shutw+ini = abort */
			channel_shutw_now(req);        /* fix buffer flags upon abort */
			channel_shutr_now(res);
		}
	}


	/* we may have a pending connection request, or a connection waiting
	 * for completion.
	 */
	if (si_state_in(si_b->state, SI_SB_REQ|SI_SB_QUE|SI_SB_TAR|SI_SB_ASS)) {
		/* prune the request variables and swap to the response variables. */
		if (s->vars_reqres.scope != SCOPE_RES) {
			if (!LIST_ISEMPTY(&s->vars_reqres.head)) {
				vars_prune(&s->vars_reqres, s->sess, s);
				vars_init(&s->vars_reqres, SCOPE_RES);
			}
		}

		do {
			/* nb: step 1 might switch from QUE to ASS, but we first want
			 * to give a chance to step 2 to perform a redirect if needed.
			 */
			if (si_b->state != SI_ST_REQ)
				sess_update_stream_int(s);
			if (si_b->state == SI_ST_REQ)
				sess_prepare_conn_req(s);

			/* applets directly go to the ESTABLISHED state. Similarly,
			 * servers experience the same fate when their connection
			 * is reused.
			 */
			if (unlikely(si_b->state == SI_ST_EST))
				sess_establish(s);

			/* Now we can add the server name to a header (if requested) */
			/* check for HTTP mode and proxy server_name_hdr_name != NULL */
			if (si_state_in(si_b->state, SI_SB_CON|SI_SB_RDY|SI_SB_EST) &&
			    (s->be->server_id_hdr_name != NULL) &&
			    (s->be->mode == PR_MODE_HTTP) &&
			    objt_server(s->target)) {
				http_send_name_header(s, s->be, objt_server(s->target)->id);
			}

			srv = objt_server(s->target);
			if (si_b->state == SI_ST_ASS && srv && srv->rdr_len && (s->flags & SF_REDIRECTABLE))
				http_perform_server_redirect(s, si_b);
		} while (si_b->state == SI_ST_ASS);
	}

	/* Let's see if we can send the pending request now */
	si_sync_send(si_b);

	/*
	 * Now forward all shutdown requests between both sides of the request buffer
	 */

	/* first, let's check if the request buffer needs to shutdown(write), which may
	 * happen either because the input is closed or because we want to force a close
	 * once the server has begun to respond. If a half-closed timeout is set, we adjust
	 * the other side's timeout as well.
	 */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CLOSE|CF_SHUTR)) ==
		     (CF_AUTO_CLOSE|CF_SHUTR))) {
		channel_shutw_now(req);
	}

	/* shutdown(write) pending */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW &&
		     channel_is_empty(req))) {
		if (req->flags & CF_READ_ERROR)
			si_b->flags |= SI_FL_NOLINGER;
		si_shutw(si_b);
	}

	/* shutdown(write) done on server side, we must stop the client too */
	if (unlikely((req->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTW &&
		     !req->analysers))
		channel_shutr_now(req);

	/* shutdown(read) pending */
	if (unlikely((req->flags & (CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTR_NOW)) {
		if (si_f->flags & SI_FL_NOHALF)
			si_f->flags |= SI_FL_NOLINGER;
		si_shutr(si_f);
	}

	/* Benchmarks have shown that it's optimal to do a full resync now */
	if (si_f->state == SI_ST_DIS ||
	    si_state_in(si_b->state, SI_SB_RDY|SI_SB_DIS) ||
	    (si_f->flags & SI_FL_ERR && si_f->state != SI_ST_CLO) ||
	    (si_b->flags & SI_FL_ERR && si_b->state != SI_ST_CLO))
		goto resync_stream_interface;

	/* otherwise we want to check if we need to resync the req buffer or not */
	if ((req->flags ^ rqf_last) & (CF_SHUTR|CF_SHUTW))
		goto resync_request;

	/* perform output updates to the response buffer */

	/* If noone is interested in analysing data, it's time to forward
	 * everything. We configure the buffer to forward indefinitely.
	 * Note that we're checking CF_SHUTR_NOW as an indication of a possible
	 * recent call to channel_abort().
	 */
	if (unlikely((!res->analysers || (res->analysers == AN_RES_FLT_END && !(res->flags & CF_FLT_ANALYZE))) &&
	    !(res->flags & (CF_SHUTW|CF_SHUTR_NOW)) &&
	    si_state_in(si_b->state, SI_SB_EST|SI_SB_DIS|SI_SB_CLO) &&
	    (res->to_forward != CHN_INFINITE_FORWARD))) {
		/* This buffer is freewheeling, there's no analyser
		 * attached to it. If any data are left in, we'll permit them to
		 * move.
		 */
		channel_auto_read(res);
		channel_auto_close(res);

		if (IS_HTX_STRM(s)) {
			struct htx *htx = htxbuf(&res->buf);

			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			co_set_data(res, htx->data);
			if (!(res->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_htx_forward_forever(res, htx);
		}
		else {
			/* We'll let data flow between the producer (if still connected)
			 * to the consumer.
			 */
			c_adv(res, ci_data(res));
			if (!(res->flags & (CF_SHUTR|CF_SHUTW_NOW)))
				channel_forward_forever(res);

			/* Just in order to support fetching HTTP contents after start
			 * of forwarding when the HTTP forwarding analyser is not used,
			 * we simply reset msg->sov so that HTTP rewinding points to the
			 * headers.
			 */
			if (s->txn)
				s->txn->rsp.sov = s->txn->rsp.eoh + s->txn->rsp.eol - co_data(res);
		}

		/* if we have no analyser anymore in any direction and have a
		 * tunnel timeout set, use it now. Note that we must respect
		 * the half-closed timeouts as well.
		 */
		if (!req->analysers && s->be->timeout.tunnel) {
			req->rto = req->wto = res->rto = res->wto =
				s->be->timeout.tunnel;

			if ((req->flags & CF_SHUTR) && tick_isset(sess->fe->timeout.clientfin))
				res->wto = sess->fe->timeout.clientfin;
			if ((req->flags & CF_SHUTW) && tick_isset(s->be->timeout.serverfin))
				res->rto = s->be->timeout.serverfin;
			if ((res->flags & CF_SHUTR) && tick_isset(s->be->timeout.serverfin))
				req->wto = s->be->timeout.serverfin;
			if ((res->flags & CF_SHUTW) && tick_isset(sess->fe->timeout.clientfin))
				req->rto = sess->fe->timeout.clientfin;

			req->rex = tick_add(now_ms, req->rto);
			req->wex = tick_add(now_ms, req->wto);
			res->rex = tick_add(now_ms, res->rto);
			res->wex = tick_add(now_ms, res->wto);
		}
	}

	/* check if it is wise to enable kernel splicing to forward response data */
	if (!(res->flags & (CF_KERN_SPLICING|CF_SHUTR)) &&
	    res->to_forward &&
	    (global.tune.options & GTUNE_USE_SPLICE) &&
	    (objt_cs(si_f->end) && __objt_cs(si_f->end)->conn->xprt && __objt_cs(si_f->end)->conn->xprt->snd_pipe) &&
	    (objt_cs(si_b->end) && __objt_cs(si_b->end)->conn->xprt && __objt_cs(si_b->end)->conn->xprt->rcv_pipe) &&
	    (pipes_used < global.maxpipes) &&
	    (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_RTR) ||
	     (((sess->fe->options2|s->be->options2) & PR_O2_SPLIC_AUT) &&
	      (res->flags & CF_STREAMER_FAST)))) {
		res->flags |= CF_KERN_SPLICING;
	}

	/* reflect what the L7 analysers have seen last */
	rpf_last = res->flags;

	/* Let's see if we can send the pending response now */
	si_sync_send(si_f);

	/*
	 * Now forward all shutdown requests between both sides of the buffer
	 */

	/*
	 * FIXME: this is probably where we should produce error responses.
	 */

	/* first, let's check if the response buffer needs to shutdown(write) */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CLOSE|CF_SHUTR)) ==
		     (CF_AUTO_CLOSE|CF_SHUTR))) {
		channel_shutw_now(res);
	}

	/* shutdown(write) pending */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTW_NOW)) == CF_SHUTW_NOW &&
		     channel_is_empty(res))) {
		si_shutw(si_f);
	}

	/* shutdown(write) done on the client side, we must stop the server too */
	if (unlikely((res->flags & (CF_SHUTW|CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTW) &&
	    !res->analysers)
		channel_shutr_now(res);

	/* shutdown(read) pending */
	if (unlikely((res->flags & (CF_SHUTR|CF_SHUTR_NOW)) == CF_SHUTR_NOW)) {
		if (si_b->flags & SI_FL_NOHALF)
			si_b->flags |= SI_FL_NOLINGER;
		si_shutr(si_b);
	}

	if (si_f->state == SI_ST_DIS ||
	    si_state_in(si_b->state, SI_SB_RDY|SI_SB_DIS) ||
	    (si_f->flags & SI_FL_ERR && si_f->state != SI_ST_CLO) ||
	    (si_b->flags & SI_FL_ERR && si_b->state != SI_ST_CLO))
		goto resync_stream_interface;

	if ((req->flags & ~rqf_last) & CF_MASK_ANALYSER)
		goto resync_request;

	if ((res->flags ^ rpf_last) & CF_MASK_STATIC)
		goto resync_response;

	if (((req->flags ^ rqf_last) | (res->flags ^ rpf_last)) & CF_MASK_ANALYSER)
		goto resync_request;

	/* we're interested in getting wakeups again */
	si_f->flags &= ~SI_FL_DONT_WAKE;
	si_b->flags &= ~SI_FL_DONT_WAKE;

	/* This is needed only when debugging is enabled, to indicate
	 * client-side or server-side close. Please note that in the unlikely
	 * event where both sides would close at once, the sequence is reported
	 * on the server side first.
	 */
	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) ||
		      (global.mode & MODE_VERBOSE)))) {
		if (si_b->state == SI_ST_CLO &&
		    si_b->prev_state == SI_ST_EST) {
			chunk_printf(&trash, "%08x:%s.srvcls[%04x:%04x]\n",
				      s->uniq_id, s->be->id,
			              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
			              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
			shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
		}

		if (si_f->state == SI_ST_CLO &&
		    si_f->prev_state == SI_ST_EST) {
			chunk_printf(&trash, "%08x:%s.clicls[%04x:%04x]\n",
				      s->uniq_id, s->be->id,
			              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
			              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
			shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
		}
	}

	if (likely((si_f->state != SI_ST_CLO) || !si_state_in(si_b->state, SI_SB_INI|SI_SB_CLO))) {
		if ((sess->fe->options & PR_O_CONTSTATS) && (s->flags & SF_BE_ASSIGNED))
			stream_process_counters(s);

		si_update_both(si_f, si_b);

		/* Trick: if a request is being waiting for the server to respond,
		 * and if we know the server can timeout, we don't want the timeout
		 * to expire on the client side first, but we're still interested
		 * in passing data from the client to the server (eg: POST). Thus,
		 * we can cancel the client's request timeout if the server's
		 * request timeout is set and the server has not yet sent a response.
		 */

		if ((res->flags & (CF_AUTO_CLOSE|CF_SHUTR)) == 0 &&
		    (tick_isset(req->wex) || tick_isset(res->rex))) {
			req->flags |= CF_READ_NOEXP;
			req->rex = TICK_ETERNITY;
		}

		/* Reset pending events now */
		s->pending_events = 0;

	update_exp_and_leave:
		/* Note: please ensure that if you branch here you disable SI_FL_DONT_WAKE */
		t->expire = tick_first((tick_is_expired(t->expire, now_ms) ? 0 : t->expire),
				       tick_first(tick_first(req->rex, req->wex),
						  tick_first(res->rex, res->wex)));
		if (!req->analysers)
			req->analyse_exp = TICK_ETERNITY;

		if ((sess->fe->options & PR_O_CONTSTATS) && (s->flags & SF_BE_ASSIGNED) &&
		          (!tick_isset(req->analyse_exp) || tick_is_expired(req->analyse_exp, now_ms)))
			req->analyse_exp = tick_add(now_ms, 5000);

		t->expire = tick_first(t->expire, req->analyse_exp);

		t->expire = tick_first(t->expire, res->analyse_exp);

		if (si_f->exp)
			t->expire = tick_first(t->expire, si_f->exp);

		if (si_b->exp)
			t->expire = tick_first(t->expire, si_b->exp);

		DPRINTF(stderr,
			"[%u] queuing with exp=%u req->rex=%u req->wex=%u req->ana_exp=%u"
			" rep->rex=%u rep->wex=%u, si[0].exp=%u, si[1].exp=%u, cs=%d, ss=%d\n",
			now_ms, t->expire, req->rex, req->wex, req->analyse_exp,
			res->rex, res->wex, si_f->exp, si_b->exp, si_f->state, si_b->state);

		s->pending_events &= ~(TASK_WOKEN_TIMER | TASK_WOKEN_RES);
		stream_release_buffers(s);
		return t; /* nothing more to do */
	}

	if (s->flags & SF_BE_ASSIGNED)
		_HA_ATOMIC_SUB(&s->be->beconn, 1);

	if (unlikely((global.mode & MODE_DEBUG) &&
		     (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)))) {
		chunk_printf(&trash, "%08x:%s.closed[%04x:%04x]\n",
			      s->uniq_id, s->be->id,
		              objt_cs(si_f->end) ? (unsigned short)objt_cs(si_f->end)->conn->handle.fd : -1,
		              objt_cs(si_b->end) ? (unsigned short)objt_cs(si_b->end)->conn->handle.fd : -1);
		shut_your_big_mouth_gcc(write(1, trash.area, trash.data));
	}

	s->logs.t_close = tv_ms_elapsed(&s->logs.tv_accept, &now);
	stream_process_counters(s);

	if (s->txn && s->txn->status) {
		int n;

		n = s->txn->status / 100;
		if (n < 1 || n > 5)
			n = 0;

		if (sess->fe->mode == PR_MODE_HTTP) {
			_HA_ATOMIC_ADD(&sess->fe->fe_counters.p.http.rsp[n], 1);
		}
		if ((s->flags & SF_BE_ASSIGNED) &&
		    (s->be->mode == PR_MODE_HTTP)) {
			_HA_ATOMIC_ADD(&s->be->be_counters.p.http.rsp[n], 1);
			_HA_ATOMIC_ADD(&s->be->be_counters.p.http.cum_req, 1);
		}
	}

	/* let's do a final log if we need it */
	if (!LIST_ISEMPTY(&sess->fe->logformat) && s->logs.logwait &&
	    !(s->flags & SF_MONITOR) &&
	    (!(sess->fe->options & PR_O_NULLNOLOG) || req->total)) {
		/* we may need to know the position in the queue */
		pendconn_free(s);
		s->do_log(s);
	}

	/* update time stats for this stream */
	stream_update_time_stats(s);

	/* the task MUST not be in the run queue anymore */
	stream_free(s);
	task_destroy(t);
	return NULL;
}

/* Update the stream's backend and server time stats */
void stream_update_time_stats(struct stream *s)
{
	int t_request;
	int t_queue;
	int t_connect;
	int t_data;
	int t_close;
	struct server *srv;

	t_request = 0;
	t_queue   = s->logs.t_queue;
	t_connect = s->logs.t_connect;
	t_close   = s->logs.t_close;
	t_data    = s->logs.t_data;

	if (s->be->mode != PR_MODE_HTTP)
		t_data = t_connect;

	if (t_connect < 0 || t_data < 0)
		return;

	if (tv_isge(&s->logs.tv_request, &s->logs.tv_accept))
		t_request = tv_ms_elapsed(&s->logs.tv_accept, &s->logs.tv_request);

	t_data    -= t_connect;
	t_connect -= t_queue;
	t_queue   -= t_request;

	srv = objt_server(s->target);
	if (srv) {
		swrate_add(&srv->counters.q_time, TIME_STATS_SAMPLES, t_queue);
		swrate_add(&srv->counters.c_time, TIME_STATS_SAMPLES, t_connect);
		swrate_add(&srv->counters.d_time, TIME_STATS_SAMPLES, t_data);
		swrate_add(&srv->counters.t_time, TIME_STATS_SAMPLES, t_close);
	}
	HA_SPIN_LOCK(PROXY_LOCK, &s->be->lock);
	swrate_add(&s->be->be_counters.q_time, TIME_STATS_SAMPLES, t_queue);
	swrate_add(&s->be->be_counters.c_time, TIME_STATS_SAMPLES, t_connect);
	swrate_add(&s->be->be_counters.d_time, TIME_STATS_SAMPLES, t_data);
	swrate_add(&s->be->be_counters.t_time, TIME_STATS_SAMPLES, t_close);
	HA_SPIN_UNLOCK(PROXY_LOCK, &s->be->lock);
}

/*
 * This function adjusts sess->srv_conn and maintains the previous and new
 * server's served stream counts. Setting newsrv to NULL is enough to release
 * current connection slot. This function also notifies any LB algo which might
 * expect to be informed about any change in the number of active streams on a
 * server.
 */
void sess_change_server(struct stream *sess, struct server *newsrv)
{
	if (sess->srv_conn == newsrv)
		return;

	if (sess->srv_conn) {
		_HA_ATOMIC_SUB(&sess->srv_conn->served, 1);
		_HA_ATOMIC_SUB(&sess->srv_conn->proxy->served, 1);
		__ha_barrier_atomic_store();
		if (sess->srv_conn->proxy->lbprm.server_drop_conn)
			sess->srv_conn->proxy->lbprm.server_drop_conn(sess->srv_conn);
		stream_del_srv_conn(sess);
	}

	if (newsrv) {
		_HA_ATOMIC_ADD(&newsrv->served, 1);
		_HA_ATOMIC_ADD(&newsrv->proxy->served, 1);
		__ha_barrier_atomic_store();
		if (newsrv->proxy->lbprm.server_take_conn)
			newsrv->proxy->lbprm.server_take_conn(newsrv);
		stream_add_srv_conn(sess, newsrv);
	}
}

/* Handle server-side errors for default protocols. It is called whenever a a
 * connection setup is aborted or a request is aborted in queue. It sets the
 * stream termination flags so that the caller does not have to worry about
 * them. It's installed as ->srv_error for the server-side stream_interface.
 */
void default_srv_error(struct stream *s, struct stream_interface *si)
{
	int err_type = si->err_type;
	int err = 0, fin = 0;

	if (err_type & SI_ET_QUEUE_ABRT) {
		err = SF_ERR_CLICL;
		fin = SF_FINST_Q;
	}
	else if (err_type & SI_ET_CONN_ABRT) {
		err = SF_ERR_CLICL;
		fin = SF_FINST_C;
	}
	else if (err_type & SI_ET_QUEUE_TO) {
		err = SF_ERR_SRVTO;
		fin = SF_FINST_Q;
	}
	else if (err_type & SI_ET_QUEUE_ERR) {
		err = SF_ERR_SRVCL;
		fin = SF_FINST_Q;
	}
	else if (err_type & SI_ET_CONN_TO) {
		err = SF_ERR_SRVTO;
		fin = SF_FINST_C;
	}
	else if (err_type & SI_ET_CONN_ERR) {
		err = SF_ERR_SRVCL;
		fin = SF_FINST_C;
	}
	else if (err_type & SI_ET_CONN_RES) {
		err = SF_ERR_RESOURCE;
		fin = SF_FINST_C;
	}
	else /* SI_ET_CONN_OTHER and others */ {
		err = SF_ERR_INTERNAL;
		fin = SF_FINST_C;
	}

	if (!(s->flags & SF_ERR_MASK))
		s->flags |= err;
	if (!(s->flags & SF_FINST_MASK))
		s->flags |= fin;
}

/* kill a stream and set the termination flags to <why> (one of SF_ERR_*) */
void stream_shutdown(struct stream *stream, int why)
{
	if (stream->req.flags & (CF_SHUTW|CF_SHUTW_NOW))
		return;

	channel_shutw_now(&stream->req);
	channel_shutr_now(&stream->res);
	stream->task->nice = 1024;
	if (!(stream->flags & SF_ERR_MASK))
		stream->flags |= why;
	task_wakeup(stream->task, TASK_WOKEN_OTHER);
}

/* Appends a dump of the state of stream <s> into buffer <buf> which must have
 * preliminary be prepared by its caller, with each line prepended by prefix
 * <pfx>, and each line terminated by character <eol>.
 */
void stream_dump(struct buffer *buf, const struct stream *s, const char *pfx, char eol)
{
	const struct conn_stream *csf, *csb;
	const struct connection  *cof, *cob;
	const struct appctx      *acf, *acb;
	const struct server      *srv;
	const char *src = "unknown";
	const char *dst = "unknown";
	char pn[INET6_ADDRSTRLEN];
	const struct channel *req, *res;
	const struct stream_interface *si_f, *si_b;

	if (!s) {
		chunk_appendf(buf, "%sstrm=%p%c", pfx, s, eol);
		return;
	}

	if (s->obj_type != OBJ_TYPE_STREAM) {
		chunk_appendf(buf, "%sstrm=%p [invalid type=%d(%s)]%c",
		              pfx, s, s->obj_type, obj_type_name(&s->obj_type), eol);
		return;
	}

	si_f = &s->si[0];
	si_b = &s->si[1];
	req = &s->req;
	res = &s->res;

	csf = objt_cs(si_f->end);
	cof = cs_conn(csf);
	acf = objt_appctx(si_f->end);
	if (cof && addr_to_str(&cof->addr.from, pn, sizeof(pn)) >= 0)
		src = pn;
	else if (acf)
		src = acf->applet->name;

	csb = objt_cs(si_b->end);
	cob = cs_conn(csb);
	acb = objt_appctx(si_b->end);
	srv = objt_server(s->target);
	if (srv)
		dst = srv->id;
	else if (acb)
		dst = acb->applet->name;

	chunk_appendf(buf,
	              "%sstrm=%p src=%s fe=%s be=%s dst=%s%c"
	              "%srqf=%x rqa=%x rpf=%x rpa=%x sif=%s,%x sib=%s,%x%c"
	              "%saf=%p,%u csf=%p,%x%c"
	              "%sab=%p,%u csb=%p,%x%c"
	              "%scof=%p,%x:%s(%p)/%s(%p)/%s(%d)%c"
	              "%scob=%p,%x:%s(%p)/%s(%p)/%s(%d)%c"
	              "",
	              pfx, s, src, s->sess->fe->id, s->be->id, dst, eol,
	              pfx, req->flags, req->analysers, res->flags, res->analysers,
	                   si_state_str(si_f->state), si_f->flags,
	                   si_state_str(si_b->state), si_b->flags, eol,
	              pfx, acf, acf ? acf->st0   : 0, csf, csf ? csf->flags : 0, eol,
	              pfx, acb, acb ? acb->st0   : 0, csb, csb ? csb->flags : 0, eol,
	              pfx, cof, cof ? cof->flags : 0, conn_get_mux_name(cof), cof?cof->ctx:0, conn_get_xprt_name(cof),
	                   cof ? cof->xprt_ctx : 0, conn_get_ctrl_name(cof), cof ? cof->handle.fd : 0, eol,
	              pfx, cob, cob ? cob->flags : 0, conn_get_mux_name(cob), cob?cob->ctx:0, conn_get_xprt_name(cob),
	                   cob ? cob->xprt_ctx : 0, conn_get_ctrl_name(cob), cob ? cob->handle.fd : 0, eol);
}

/* dumps an error message for type <type> at ptr <ptr> related to stream <s>,
 * having reached loop rate <rate>, then aborts hoping to retrieve a core.
 */
void stream_dump_and_crash(enum obj_type *obj, int rate)
{
	const struct stream *s;
	char *msg = NULL;
	const void *ptr;

	ptr = s = objt_stream(obj);
	if (!s) {
		const struct appctx *appctx = objt_appctx(obj);
		if (!appctx)
			return;
		ptr = appctx;
		s = si_strm(appctx->owner);
		if (!s)
			return;
	}

	chunk_reset(&trash);
	stream_dump(&trash, s, "", ' ');
	memprintf(&msg,
	          "A bogus %s [%p] is spinning at %d calls per second and refuses to die, "
	          "aborting now! Please report this error to developers "
	          "[%s]\n",
	          obj_type_name(obj), ptr, rate, trash.area);

	ha_alert("%s", msg);
	send_log(NULL, LOG_EMERG, "%s", msg);
	abort();
}

/************************************************************************/
/*           All supported ACL keywords must be declared here.          */
/************************************************************************/

/* 0=OK, <0=Alert, >0=Warning */
static enum act_parse_ret stream_parse_use_service(const char **args, int *cur_arg,
                                                   struct proxy *px, struct act_rule *rule,
                                                   char **err)
{
	struct action_kw *kw;

	/* Check if the service name exists. */
	if (*(args[*cur_arg]) == 0) {
		memprintf(err, "'%s' expects a service name.", args[0]);
		return ACT_RET_PRS_ERR;
	}

	/* lookup for keyword corresponding to a service. */
	kw = action_lookup(&service_keywords, args[*cur_arg]);
	if (!kw) {
		memprintf(err, "'%s' unknown service name.", args[1]);
		return ACT_RET_PRS_ERR;
	}
	(*cur_arg)++;

	/* executes specific rule parser. */
	rule->kw = kw;
	if (kw->parse((const char **)args, cur_arg, px, rule, err) == ACT_RET_PRS_ERR)
		return ACT_RET_PRS_ERR;

	/* Register processing function. */
	rule->action_ptr = process_use_service;
	rule->action = ACT_CUSTOM;

	return ACT_RET_PRS_OK;
}

void service_keywords_register(struct action_kw_list *kw_list)
{
	LIST_ADDQ(&service_keywords, &kw_list->list);
}

/* Lists the known services on <out> */
void list_services(FILE *out)
{
	struct action_kw_list *kw_list;
	int found = 0;
	int i;

	fprintf(out, "Available services :");
	list_for_each_entry(kw_list, &service_keywords, list) {
		for (i = 0; kw_list->kw[i].kw != NULL; i++) {
			if (!found)
				fputc('\n', out);
			found = 1;
			fprintf(out, "\t%s\n", kw_list->kw[i].kw);
		}
	}
	if (!found)
		fprintf(out, " none\n");
}

/* This function dumps a complete stream state onto the stream interface's
 * read buffer. The stream has to be set in strm. It returns 0 if the output
 * buffer is full and it needs to be called again, otherwise non-zero. It is
 * designed to be called from stats_dump_strm_to_buffer() below.
 */
static int stats_dump_full_strm_to_buffer(struct stream_interface *si, struct stream *strm)
{
	struct appctx *appctx = __objt_appctx(si->end);
	struct tm tm;
	extern const char *monthname[12];
	char pn[INET6_ADDRSTRLEN];
	struct conn_stream *cs;
	struct connection *conn;
	struct appctx *tmpctx;

	chunk_reset(&trash);

	if (appctx->ctx.sess.section > 0 && appctx->ctx.sess.uid != strm->uniq_id) {
		/* stream changed, no need to go any further */
		chunk_appendf(&trash, "  *** session terminated while we were watching it ***\n");
		if (ci_putchk(si_ic(si), &trash) == -1)
			goto full;
		goto done;
	}

	switch (appctx->ctx.sess.section) {
	case 0: /* main status of the stream */
		appctx->ctx.sess.uid = strm->uniq_id;
		appctx->ctx.sess.section = 1;
		/* fall through */

	case 1:
		get_localtime(strm->logs.accept_date.tv_sec, &tm);
		chunk_appendf(&trash,
			     "%p: [%02d/%s/%04d:%02d:%02d:%02d.%06d] id=%u proto=%s",
			     strm,
			     tm.tm_mday, monthname[tm.tm_mon], tm.tm_year+1900,
			     tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(strm->logs.accept_date.tv_usec),
			     strm->uniq_id,
			     strm_li(strm) ? strm_li(strm)->proto->name : "?");

		conn = objt_conn(strm_orig(strm));
		switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " source=%s:%d\n",
			              pn, get_host_port(&conn->addr.from));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " source=unix:%d\n", strm_li(strm)->luid);
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		chunk_appendf(&trash,
			     "  flags=0x%x, conn_retries=%d, srv_conn=%p, pend_pos=%p waiting=%d\n",
			     strm->flags, strm->si[1].conn_retries, strm->srv_conn, strm->pend_pos,
			     !LIST_ISEMPTY(&strm->buffer_wait.list));

		chunk_appendf(&trash,
			     "  frontend=%s (id=%u mode=%s), listener=%s (id=%u)",
			     strm_fe(strm)->id, strm_fe(strm)->uuid, strm_fe(strm)->mode ? "http" : "tcp",
			     strm_li(strm) ? strm_li(strm)->name ? strm_li(strm)->name : "?" : "?",
			     strm_li(strm) ? strm_li(strm)->luid : 0);

		if (conn)
			conn_get_to_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.to, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.to));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix:%d\n", strm_li(strm)->luid);
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		if (strm->be->cap & PR_CAP_BE)
			chunk_appendf(&trash,
				     "  backend=%s (id=%u mode=%s)",
				     strm->be->id,
				     strm->be->uuid, strm->be->mode ? "http" : "tcp");
		else
			chunk_appendf(&trash, "  backend=<NONE> (id=-1 mode=-)");

		cs = objt_cs(strm->si[1].end);
		conn = cs_conn(cs);

		if (conn)
			conn_get_from_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.from));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix\n");
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		if (strm->be->cap & PR_CAP_BE)
			chunk_appendf(&trash,
				     "  server=%s (id=%u)",
				     objt_server(strm->target) ? objt_server(strm->target)->id : "<none>",
				     objt_server(strm->target) ? objt_server(strm->target)->puid : 0);
		else
			chunk_appendf(&trash, "  server=<NONE> (id=-1)");

		if (conn)
			conn_get_to_addr(conn);

		switch (conn ? addr_to_str(&conn->addr.to, pn, sizeof(pn)) : AF_UNSPEC) {
		case AF_INET:
		case AF_INET6:
			chunk_appendf(&trash, " addr=%s:%d\n",
				     pn, get_host_port(&conn->addr.to));
			break;
		case AF_UNIX:
			chunk_appendf(&trash, " addr=unix\n");
			break;
		default:
			/* no more information to print right now */
			chunk_appendf(&trash, "\n");
			break;
		}

		chunk_appendf(&trash,
			     "  task=%p (state=0x%02x nice=%d calls=%u rate=%u exp=%s tmask=0x%lx%s",
			     strm->task,
			     strm->task->state,
			     strm->task->nice, strm->task->calls, read_freq_ctr(&strm->call_rate),
			     strm->task->expire ?
			             tick_is_expired(strm->task->expire, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(strm->task->expire - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			     strm->task->thread_mask,
			     task_in_rq(strm->task) ? ", running" : "");

		chunk_appendf(&trash,
			     " age=%s)\n",
			     human_time(now.tv_sec - strm->logs.accept_date.tv_sec, 1));

		if (strm->txn)
			chunk_appendf(&trash,
			      "  txn=%p flags=0x%x meth=%d status=%d req.st=%s rsp.st=%s\n"
			      "      req.f=0x%02x blen=%llu chnk=%llu next=%u\n"
			      "      rsp.f=0x%02x blen=%llu chnk=%llu next=%u\n",
			      strm->txn, strm->txn->flags, strm->txn->meth, strm->txn->status,
			      h1_msg_state_str(strm->txn->req.msg_state), h1_msg_state_str(strm->txn->rsp.msg_state),
			      strm->txn->req.flags, strm->txn->req.body_len, strm->txn->req.chunk_len, strm->txn->req.next,
			      strm->txn->rsp.flags, strm->txn->rsp.body_len, strm->txn->rsp.chunk_len, strm->txn->rsp.next);

		chunk_appendf(&trash,
			     "  si[0]=%p (state=%s flags=0x%02x endp0=%s:%p exp=%s et=0x%03x sub=%d)\n",
			     &strm->si[0],
			     si_state_str(strm->si[0].state),
			     strm->si[0].flags,
			     obj_type_name(strm->si[0].end),
			     obj_base_ptr(strm->si[0].end),
			     strm->si[0].exp ?
			             tick_is_expired(strm->si[0].exp, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(strm->si[0].exp - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			      strm->si[0].err_type, strm->si[0].wait_event.events);

		chunk_appendf(&trash,
			     "  si[1]=%p (state=%s flags=0x%02x endp1=%s:%p exp=%s et=0x%03x sub=%d)\n",
			     &strm->si[1],
			     si_state_str(strm->si[1].state),
			     strm->si[1].flags,
			     obj_type_name(strm->si[1].end),
			     obj_base_ptr(strm->si[1].end),
			     strm->si[1].exp ?
			             tick_is_expired(strm->si[1].exp, now_ms) ? "<PAST>" :
			                     human_time(TICKS_TO_MS(strm->si[1].exp - now_ms),
			                     TICKS_TO_MS(1000)) : "<NEVER>",
			     strm->si[1].err_type, strm->si[1].wait_event.events);

		if ((cs = objt_cs(strm->si[0].end)) != NULL) {
			conn = cs->conn;

			chunk_appendf(&trash,
			              "  co0=%p ctrl=%s xprt=%s mux=%s data=%s target=%s:%p\n",
				      conn,
				      conn_get_ctrl_name(conn),
				      conn_get_xprt_name(conn),
				      conn_get_mux_name(conn),
				      cs_get_data_name(cs),
			              obj_type_name(conn->target),
			              obj_base_ptr(conn->target));

			chunk_appendf(&trash,
			              "      flags=0x%08x fd=%d fd.state=%02x fd.cache=%d updt=%d fd.tmask=0x%lx\n",
			              conn->flags,
			              conn->handle.fd,
			              conn->handle.fd >= 0 ? fdtab[conn->handle.fd].state : 0,
			              conn->handle.fd >= 0 ? fdtab[conn->handle.fd].cache.next >= -2 : 0,
			              conn->handle.fd >= 0 ? !!(fdtab[conn->handle.fd].update_mask & tid_bit) : 0,
				      conn->handle.fd >= 0 ? fdtab[conn->handle.fd].thread_mask: 0);

			chunk_appendf(&trash, "      cs=%p csf=0x%08x ctx=%p\n", cs, cs->flags, cs->ctx);
		}
		else if ((tmpctx = objt_appctx(strm->si[0].end)) != NULL) {
			chunk_appendf(&trash,
			              "  app0=%p st0=%d st1=%d st2=%d applet=%s tmask=0x%lx nice=%d calls=%u rate=%u cpu=%llu lat=%llu\n",
				      tmpctx,
				      tmpctx->st0,
				      tmpctx->st1,
				      tmpctx->st2,
			              tmpctx->applet->name,
			              tmpctx->thread_mask,
			              tmpctx->t->nice, tmpctx->t->calls, read_freq_ctr(&tmpctx->call_rate),
			              (unsigned long long)tmpctx->t->cpu_time, (unsigned long long)tmpctx->t->lat_time);
		}

		if ((cs = objt_cs(strm->si[1].end)) != NULL) {
			conn = cs->conn;

			chunk_appendf(&trash,
			              "  co1=%p ctrl=%s xprt=%s mux=%s data=%s target=%s:%p\n",
				      conn,
				      conn_get_ctrl_name(conn),
				      conn_get_xprt_name(conn),
				      conn_get_mux_name(conn),
				      cs_get_data_name(cs),
			              obj_type_name(conn->target),
			              obj_base_ptr(conn->target));

			chunk_appendf(&trash,
			              "      flags=0x%08x fd=%d fd.state=%02x fd.cache=%d updt=%d fd.tmask=0x%lx\n",
			              conn->flags,
			              conn->handle.fd,
			              conn->handle.fd >= 0 ? fdtab[conn->handle.fd].state : 0,
			              conn->handle.fd >= 0 ? fdtab[conn->handle.fd].cache.next >= -2 : 0,
			              conn->handle.fd >= 0 ? !!(fdtab[conn->handle.fd].update_mask & tid_bit) : 0,
				      conn->handle.fd >= 0 ? fdtab[conn->handle.fd].thread_mask: 0);

			chunk_appendf(&trash, "      cs=%p csf=0x%08x ctx=%p\n", cs, cs->flags, cs->ctx);
		}
		else if ((tmpctx = objt_appctx(strm->si[1].end)) != NULL) {
			chunk_appendf(&trash,
			              "  app1=%p st0=%d st1=%d st2=%d applet=%s tmask=0x%lx nice=%d calls=%u rate=%u cpu=%llu lat=%llu\n",
				      tmpctx,
				      tmpctx->st0,
				      tmpctx->st1,
				      tmpctx->st2,
			              tmpctx->applet->name,
			              tmpctx->thread_mask,
			              tmpctx->t->nice, tmpctx->t->calls, read_freq_ctr(&tmpctx->call_rate),
			              (unsigned long long)tmpctx->t->cpu_time, (unsigned long long)tmpctx->t->lat_time);
		}

		chunk_appendf(&trash,
			     "  req=%p (f=0x%06x an=0x%x pipe=%d tofwd=%d total=%lld)\n"
			     "      an_exp=%s",
			     &strm->req,
			     strm->req.flags, strm->req.analysers,
			     strm->req.pipe ? strm->req.pipe->data : 0,
			     strm->req.to_forward, strm->req.total,
			     strm->req.analyse_exp ?
			     human_time(TICKS_TO_MS(strm->req.analyse_exp - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " rex=%s",
			     strm->req.rex ?
			     human_time(TICKS_TO_MS(strm->req.rex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " wex=%s\n"
			     "      buf=%p data=%p o=%u p=%u req.next=%d i=%u size=%u\n",
			     strm->req.wex ?
			     human_time(TICKS_TO_MS(strm->req.wex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>",
			     &strm->req.buf,
		             b_orig(&strm->req.buf), (unsigned int)co_data(&strm->req),
			     (unsigned int)ci_head_ofs(&strm->req),
		             strm->txn ? strm->txn->req.next : 0, (unsigned int)ci_data(&strm->req),
			     (unsigned int)strm->req.buf.size);

		if (IS_HTX_STRM(strm)) {
			struct htx *htx = htxbuf(&strm->req.buf);

			chunk_appendf(&trash,
				      "      htx=%p flags=0x%x size=%u data=%u used=%u wrap=%s extra=%llu\n",
				      htx, htx->flags, htx->size, htx->data, htx->used,
				      (htx->tail >= htx->head) ? "NO" : "YES",
				      (unsigned long long)htx->extra);
		}

		chunk_appendf(&trash,
			     "  res=%p (f=0x%06x an=0x%x pipe=%d tofwd=%d total=%lld)\n"
			     "      an_exp=%s",
			     &strm->res,
			     strm->res.flags, strm->res.analysers,
			     strm->res.pipe ? strm->res.pipe->data : 0,
			     strm->res.to_forward, strm->res.total,
			     strm->res.analyse_exp ?
			     human_time(TICKS_TO_MS(strm->res.analyse_exp - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " rex=%s",
			     strm->res.rex ?
			     human_time(TICKS_TO_MS(strm->res.rex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>");

		chunk_appendf(&trash,
			     " wex=%s\n"
			     "      buf=%p data=%p o=%u p=%u rsp.next=%d i=%u size=%u\n",
			     strm->res.wex ?
			     human_time(TICKS_TO_MS(strm->res.wex - now_ms),
					TICKS_TO_MS(1000)) : "<NEVER>",
			     &strm->res.buf,
		             b_orig(&strm->res.buf), (unsigned int)co_data(&strm->res),
		             (unsigned int)ci_head_ofs(&strm->res),
		             strm->txn ? strm->txn->rsp.next : 0, (unsigned int)ci_data(&strm->res),
			     (unsigned int)strm->res.buf.size);

		if (IS_HTX_STRM(strm)) {
			struct htx *htx = htxbuf(&strm->res.buf);

			chunk_appendf(&trash,
				      "      htx=%p flags=0x%x size=%u data=%u used=%u wrap=%s extra=%llu\n",
				      htx, htx->flags, htx->size, htx->data, htx->used,
				      (htx->tail >= htx->head) ? "NO" : "YES",
				      (unsigned long long)htx->extra);
		}

		if (ci_putchk(si_ic(si), &trash) == -1)
			goto full;

		/* use other states to dump the contents */
	}
	/* end of dump */
 done:
	appctx->ctx.sess.uid = 0;
	appctx->ctx.sess.section = 0;
	return 1;
 full:
	return 0;
}


static int cli_parse_show_sess(char **args, char *payload, struct appctx *appctx, void *private)
{
	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	if (*args[2] && strcmp(args[2], "all") == 0)
		appctx->ctx.sess.target = (void *)-1;
	else if (*args[2])
		appctx->ctx.sess.target = (void *)strtoul(args[2], NULL, 0);
	else
		appctx->ctx.sess.target = NULL;
	appctx->ctx.sess.section = 0; /* start with stream status */
	appctx->ctx.sess.pos = 0;

	return 0;
}

/* This function dumps all streams' states onto the stream interface's
 * read buffer. It returns 0 if the output buffer is full and it needs
 * to be called again, otherwise non-zero. It proceeds in an isolated
 * thread so there is no thread safety issue here.
 */
static int cli_io_handler_dump_sess(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct connection *conn;

	thread_isolate();

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW))) {
		/* If we're forced to shut down, we might have to remove our
		 * reference to the last stream being dumped.
		 */
		if (appctx->st2 == STAT_ST_LIST) {
			if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users)) {
				LIST_DEL(&appctx->ctx.sess.bref.users);
				LIST_INIT(&appctx->ctx.sess.bref.users);
			}
		}
		goto done;
	}

	chunk_reset(&trash);

	switch (appctx->st2) {
	case STAT_ST_INIT:
		/* the function had not been called yet, let's prepare the
		 * buffer for a response. We initialize the current stream
		 * pointer to the first in the global list. When a target
		 * stream is being destroyed, it is responsible for updating
		 * this pointer. We know we have reached the end when this
		 * pointer points back to the head of the streams list.
		 */
		LIST_INIT(&appctx->ctx.sess.bref.users);
		appctx->ctx.sess.bref.ref = streams.n;
		appctx->st2 = STAT_ST_LIST;
		/* fall through */

	case STAT_ST_LIST:
		/* first, let's detach the back-ref from a possible previous stream */
		if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users)) {
			LIST_DEL(&appctx->ctx.sess.bref.users);
			LIST_INIT(&appctx->ctx.sess.bref.users);
		}

		/* and start from where we stopped */
		while (appctx->ctx.sess.bref.ref != &streams) {
			char pn[INET6_ADDRSTRLEN];
			struct stream *curr_strm;

			curr_strm = LIST_ELEM(appctx->ctx.sess.bref.ref, struct stream *, list);

			if (appctx->ctx.sess.target) {
				if (appctx->ctx.sess.target != (void *)-1 && appctx->ctx.sess.target != curr_strm)
					goto next_sess;

				LIST_ADDQ(&curr_strm->back_refs, &appctx->ctx.sess.bref.users);
				/* call the proper dump() function and return if we're missing space */
				if (!stats_dump_full_strm_to_buffer(si, curr_strm))
					goto full;

				/* stream dump complete */
				LIST_DEL(&appctx->ctx.sess.bref.users);
				LIST_INIT(&appctx->ctx.sess.bref.users);
				if (appctx->ctx.sess.target != (void *)-1) {
					appctx->ctx.sess.target = NULL;
					break;
				}
				else
					goto next_sess;
			}

			chunk_appendf(&trash,
				     "%p: proto=%s",
				     curr_strm,
				     strm_li(curr_strm) ? strm_li(curr_strm)->proto->name : "?");

			conn = objt_conn(strm_orig(curr_strm));
			switch (conn ? addr_to_str(&conn->addr.from, pn, sizeof(pn)) : AF_UNSPEC) {
			case AF_INET:
			case AF_INET6:
				chunk_appendf(&trash,
					     " src=%s:%d fe=%s be=%s srv=%s",
					     pn,
					     get_host_port(&conn->addr.from),
					     strm_fe(curr_strm)->id,
					     (curr_strm->be->cap & PR_CAP_BE) ? curr_strm->be->id : "<NONE>",
					     objt_server(curr_strm->target) ? objt_server(curr_strm->target)->id : "<none>"
					     );
				break;
			case AF_UNIX:
				chunk_appendf(&trash,
					     " src=unix:%d fe=%s be=%s srv=%s",
					     strm_li(curr_strm)->luid,
					     strm_fe(curr_strm)->id,
					     (curr_strm->be->cap & PR_CAP_BE) ? curr_strm->be->id : "<NONE>",
					     objt_server(curr_strm->target) ? objt_server(curr_strm->target)->id : "<none>"
					     );
				break;
			}

			chunk_appendf(&trash,
				     " ts=%02x age=%s calls=%u rate=%u cpu=%llu lat=%llu",
				     curr_strm->task->state,
				     human_time(now.tv_sec - curr_strm->logs.tv_accept.tv_sec, 1),
			             curr_strm->task->calls, read_freq_ctr(&curr_strm->call_rate),
			             (unsigned long long)curr_strm->task->cpu_time, (unsigned long long)curr_strm->task->lat_time);

			chunk_appendf(&trash,
				     " rq[f=%06xh,i=%u,an=%02xh,rx=%s",
				     curr_strm->req.flags,
			             (unsigned int)ci_data(&curr_strm->req),
				     curr_strm->req.analysers,
				     curr_strm->req.rex ?
				     human_time(TICKS_TO_MS(curr_strm->req.rex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",wx=%s",
				     curr_strm->req.wex ?
				     human_time(TICKS_TO_MS(curr_strm->req.wex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",ax=%s]",
				     curr_strm->req.analyse_exp ?
				     human_time(TICKS_TO_MS(curr_strm->req.analyse_exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     " rp[f=%06xh,i=%u,an=%02xh,rx=%s",
				     curr_strm->res.flags,
			             (unsigned int)ci_data(&curr_strm->res),
				     curr_strm->res.analysers,
				     curr_strm->res.rex ?
				     human_time(TICKS_TO_MS(curr_strm->res.rex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",wx=%s",
				     curr_strm->res.wex ?
				     human_time(TICKS_TO_MS(curr_strm->res.wex - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     ",ax=%s]",
				     curr_strm->res.analyse_exp ?
				     human_time(TICKS_TO_MS(curr_strm->res.analyse_exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			conn = cs_conn(objt_cs(curr_strm->si[0].end));
			chunk_appendf(&trash,
				     " s0=[%d,%1xh,fd=%d,ex=%s]",
				     curr_strm->si[0].state,
				     curr_strm->si[0].flags,
				     conn ? conn->handle.fd : -1,
				     curr_strm->si[0].exp ?
				     human_time(TICKS_TO_MS(curr_strm->si[0].exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			conn = cs_conn(objt_cs(curr_strm->si[1].end));
			chunk_appendf(&trash,
				     " s1=[%d,%1xh,fd=%d,ex=%s]",
				     curr_strm->si[1].state,
				     curr_strm->si[1].flags,
				     conn ? conn->handle.fd : -1,
				     curr_strm->si[1].exp ?
				     human_time(TICKS_TO_MS(curr_strm->si[1].exp - now_ms),
						TICKS_TO_MS(1000)) : "");

			chunk_appendf(&trash,
				     " exp=%s",
				     curr_strm->task->expire ?
				     human_time(TICKS_TO_MS(curr_strm->task->expire - now_ms),
						TICKS_TO_MS(1000)) : "");
			if (task_in_rq(curr_strm->task))
				chunk_appendf(&trash, " run(nice=%d)", curr_strm->task->nice);

			chunk_appendf(&trash, "\n");

			if (ci_putchk(si_ic(si), &trash) == -1) {
				/* let's try again later from this stream. We add ourselves into
				 * this stream's users so that it can remove us upon termination.
				 */
				LIST_ADDQ(&curr_strm->back_refs, &appctx->ctx.sess.bref.users);
				goto full;
			}

		next_sess:
			appctx->ctx.sess.bref.ref = curr_strm->list.n;
		}

		if (appctx->ctx.sess.target && appctx->ctx.sess.target != (void *)-1) {
			/* specified stream not found */
			if (appctx->ctx.sess.section > 0)
				chunk_appendf(&trash, "  *** session terminated while we were watching it ***\n");
			else
				chunk_appendf(&trash, "Session not found.\n");

			if (ci_putchk(si_ic(si), &trash) == -1)
				goto full;

			appctx->ctx.sess.target = NULL;
			appctx->ctx.sess.uid = 0;
			goto done;
		}
		/* fall through */

	default:
		appctx->st2 = STAT_ST_FIN;
		goto done;
	}
 done:
	thread_release();
	return 1;
 full:
	thread_release();
	si_rx_room_blk(si);
	return 0;
}

static void cli_release_show_sess(struct appctx *appctx)
{
	if (appctx->st2 == STAT_ST_LIST) {
		HA_SPIN_LOCK(STRMS_LOCK, &streams_lock);
		if (!LIST_ISEMPTY(&appctx->ctx.sess.bref.users))
			LIST_DEL(&appctx->ctx.sess.bref.users);
		HA_SPIN_UNLOCK(STRMS_LOCK, &streams_lock);
	}
}

/* Parses the "shutdown session" directive, it always returns 1 */
static int cli_parse_shutdown_session(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct stream *strm, *ptr;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	if (!*args[2]) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Session pointer expected (use 'show sess').\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	ptr = (void *)strtoul(args[2], NULL, 0);

	/* first, look for the requested stream in the stream table */
	list_for_each_entry(strm, &streams, list) {
		if (strm == ptr)
			break;
	}

	/* do we have the stream ? */
	if (strm != ptr) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "No such session (use 'show sess').\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	stream_shutdown(strm, SF_ERR_KILLED);
	return 1;
}

/* Parses the "shutdown session server" directive, it always returns 1 */
static int cli_parse_shutdown_sessions_server(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct server *sv;
	struct stream *strm, *strm_bck;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	sv = cli_find_server(appctx, args[3]);
	if (!sv)
		return 1;

	/* kill all the stream that are on this server */
	HA_SPIN_LOCK(SERVER_LOCK, &sv->lock);
	list_for_each_entry_safe(strm, strm_bck, &sv->actconns, by_srv)
		if (strm->srv_conn == sv)
			stream_shutdown(strm, SF_ERR_KILLED);
	HA_SPIN_UNLOCK(SERVER_LOCK, &sv->lock);
	return 1;
}

/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "show", "sess",  NULL }, "show sess [id] : report the list of current sessions or dump this session", cli_parse_show_sess, cli_io_handler_dump_sess, cli_release_show_sess },
	{ { "shutdown", "session",  NULL }, "shutdown session : kill a specific session", cli_parse_shutdown_session, NULL, NULL },
	{ { "shutdown", "sessions",  "server" }, "shutdown sessions server : kill sessions on a server", cli_parse_shutdown_sessions_server, NULL, NULL },
	{{},}
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

/* main configuration keyword registration. */
static struct action_kw_list stream_tcp_keywords = { ILH, {
	{ "use-service", stream_parse_use_service },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_req_cont_keywords_register, &stream_tcp_keywords);

static struct action_kw_list stream_http_keywords = { ILH, {
	{ "use-service", stream_parse_use_service },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, http_req_keywords_register, &stream_http_keywords);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
