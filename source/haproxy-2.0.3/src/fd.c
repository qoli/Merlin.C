/*
 * File descriptors management functions.
 *
 * Copyright 2000-2014 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This code implements an events cache for file descriptors. It remembers the
 * readiness of a file descriptor after a return from poll() and the fact that
 * an I/O attempt failed on EAGAIN. Events in the cache which are still marked
 * ready and active are processed just as if they were reported by poll().
 *
 * This serves multiple purposes. First, it significantly improves performance
 * by avoiding to subscribe to polling unless absolutely necessary, so most
 * events are processed without polling at all, especially send() which
 * benefits from the socket buffers. Second, it is the only way to support
 * edge-triggered pollers (eg: EPOLL_ET). And third, it enables I/O operations
 * that are backed by invisible buffers. For example, SSL is able to read a
 * whole socket buffer and not deliver it to the application buffer because
 * it's full. Unfortunately, it won't be reported by a poller anymore until
 * some new activity happens. The only way to call it again thus is to keep
 * this readiness information in the cache and to access it without polling
 * once the FD is enabled again.
 *
 * One interesting feature of the cache is that it maintains the principle
 * of speculative I/O introduced in haproxy 1.3 : the first time an event is
 * enabled, the FD is considered as ready so that the I/O attempt is performed
 * via the cache without polling. And the polling happens only when EAGAIN is
 * first met. This avoids polling for HTTP requests, especially when the
 * defer-accept mode is used. It also avoids polling for sending short data
 * such as requests to servers or short responses to clients.
 *
 * The cache consists in a list of active events and a list of updates.
 * Active events are events that are expected to come and that we must report
 * to the application until it asks to stop or asks to poll. Updates are new
 * requests for changing an FD state. Updates are the only way to create new
 * events. This is important because it means that the number of cached events
 * cannot increase between updates and will only grow one at a time while
 * processing updates. All updates must always be processed, though events
 * might be processed by small batches if required.
 *
 * There is no direct link between the FD and the updates list. There is only a
 * bit in the fdtab[] to indicate than a file descriptor is already present in
 * the updates list. Once an fd is present in the updates list, it will have to
 * be considered even if its changes are reverted in the middle or if the fd is
 * replaced.
 *
 * It is important to understand that as long as all expected events are
 * processed, they might starve the polled events, especially because polled
 * I/O starvation quickly induces more cached I/O. One solution to this
 * consists in only processing a part of the events at once, but one drawback
 * is that unhandled events will still wake the poller up. Using an edge-
 * triggered poller such as EPOLL_ET will solve this issue though.
 *
 * Since we do not want to scan all the FD list to find cached I/O events,
 * we store them in a list consisting in a linear array holding only the FD
 * indexes right now. Note that a closed FD cannot exist in the cache, because
 * it is closed by fd_delete() which in turn calls fd_release_cache_entry()
 * which always removes it from the list.
 *
 * The FD array has to hold a back reference to the cache. This reference is
 * always valid unless the FD is not in the cache and is not updated, in which
 * case the reference points to index 0.
 *
 * The event state for an FD, as found in fdtab[].state, is maintained for each
 * direction. The state field is built this way, with R bits in the low nibble
 * and W bits in the high nibble for ease of access and debugging :
 *
 *               7    6    5    4   3    2    1    0
 *             [ 0 | PW | RW | AW | 0 | PR | RR | AR ]
 *
 *                   A* = active     *R = read
 *                   P* = polled     *W = write
 *                   R* = ready
 *
 * An FD is marked "active" when there is a desire to use it.
 * An FD is marked "polled" when it is registered in the polling.
 * An FD is marked "ready" when it has not faced a new EAGAIN since last wake-up
 * (it is a cache of the last EAGAIN regardless of polling changes).
 *
 * We have 8 possible states for each direction based on these 3 flags :
 *
 *   +---+---+---+----------+---------------------------------------------+
 *   | P | R | A | State    | Description				  |
 *   +---+---+---+----------+---------------------------------------------+
 *   | 0 | 0 | 0 | DISABLED | No activity desired, not ready.		  |
 *   | 0 | 0 | 1 | MUSTPOLL | Activity desired via polling.		  |
 *   | 0 | 1 | 0 | STOPPED  | End of activity without polling.		  |
 *   | 0 | 1 | 1 | ACTIVE   | Activity desired without polling.		  |
 *   | 1 | 0 | 0 | ABORT    | Aborted poll(). Not frequently seen.	  |
 *   | 1 | 0 | 1 | POLLED   | FD is being polled.			  |
 *   | 1 | 1 | 0 | PAUSED   | FD was paused while ready (eg: buffer full) |
 *   | 1 | 1 | 1 | READY    | FD was marked ready by poll()		  |
 *   +---+---+---+----------+---------------------------------------------+
 *
 * The transitions are pretty simple :
 *   - fd_want_*() : set flag A
 *   - fd_stop_*() : clear flag A
 *   - fd_cant_*() : clear flag R (when facing EAGAIN)
 *   - fd_may_*()  : set flag R (upon return from poll())
 *   - sync()      : if (A) { if (!R) P := 1 } else { P := 0 }
 *
 * The PAUSED, ABORT and MUSTPOLL states are transient for level-trigerred
 * pollers and are fixed by the sync() which happens at the beginning of the
 * poller. For event-triggered pollers, only the MUSTPOLL state will be
 * transient and ABORT will lead to PAUSED. The ACTIVE state is the only stable
 * one which has P != A.
 *
 * The READY state is a bit special as activity on the FD might be notified
 * both by the poller or by the cache. But it is needed for some multi-layer
 * protocols (eg: SSL) where connection activity is not 100% linked to FD
 * activity. Also some pollers might prefer to implement it as ACTIVE if
 * enabling/disabling the FD is cheap. The READY and ACTIVE states are the
 * two states for which a cache entry is allocated.
 *
 * The state transitions look like the diagram below. Only the 4 right states
 * have polling enabled :
 *
 *          (POLLED=0)          (POLLED=1)
 *
 *          +----------+  sync  +-------+
 *          | DISABLED | <----- | ABORT |         (READY=0, ACTIVE=0)
 *          +----------+        +-------+
 *         clr |  ^           set |  ^
 *             |  |               |  |
 *             v  | set           v  | clr
 *          +----------+  sync  +--------+
 *          | MUSTPOLL | -----> | POLLED |        (READY=0, ACTIVE=1)
 *          +----------+        +--------+
 *                ^          poll |  ^
 *                |               |  |
 *                | EAGAIN        v  | EAGAIN
 *           +--------+         +-------+
 *           | ACTIVE |         | READY |         (READY=1, ACTIVE=1)
 *           +--------+         +-------+
 *         clr |  ^           set |  ^
 *             |  |               |  |
 *             v  | set           v  | clr
 *          +---------+   sync  +--------+
 *          | STOPPED | <------ | PAUSED |        (READY=1, ACTIVE=0)
 *          +---------+         +--------+
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/resource.h>

#if defined(USE_POLL)
#include <poll.h>
#include <errno.h>
#endif

#include <common/compat.h>
#include <common/config.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/log.h>
#include <proto/port_range.h>

struct fdtab *fdtab = NULL;     /* array of all the file descriptors */
unsigned long *polled_mask = NULL; /* Array for the polled_mask of each fd */
struct fdinfo *fdinfo = NULL;   /* less-often used infos for file descriptors */
int totalconn;                  /* total # of terminated sessions */
int actconn;                    /* # of active sessions */

struct poller pollers[MAX_POLLERS];
struct poller cur_poller;
int nbpollers = 0;

volatile struct fdlist fd_cache ; // FD events cache
volatile struct fdlist fd_cache_local[MAX_THREADS]; // FD events local for each thread
volatile struct fdlist update_list; // Global update list

unsigned long fd_cache_mask = 0; // Mask of threads with events in the cache

THREAD_LOCAL int *fd_updt  = NULL;  // FD updates list
THREAD_LOCAL int  fd_nbupdt = 0;   // number of updates in the list
THREAD_LOCAL int poller_rd_pipe = -1; // Pipe to wake the thread
int poller_wr_pipe[MAX_THREADS]; // Pipe to wake the threads

volatile int ha_used_fds = 0; // Number of FD we're currently using

#define _GET_NEXT(fd, off) ((struct fdlist_entry *)(void *)((char *)(&fdtab[fd]) + off))->next
#define _GET_PREV(fd, off) ((struct fdlist_entry *)(void *)((char *)(&fdtab[fd]) + off))->prev
/* adds fd <fd> to fd list <list> if it was not yet in it */
void fd_add_to_fd_list(volatile struct fdlist *list, int fd, int off)
{
	int next;
	int new;
	int old;
	int last;

redo_next:
	next = _GET_NEXT(fd, off);
	/* Check that we're not already in the cache, and if not, lock us. */
	if (next >= -2)
		goto done;
	if (!_HA_ATOMIC_CAS(&_GET_NEXT(fd, off), &next, -2))
		goto redo_next;
	__ha_barrier_atomic_store();

	new = fd;
redo_last:
	/* First, insert in the linked list */
	last = list->last;
	old = -1;

	_GET_PREV(fd, off) = -2;
	/* Make sure the "prev" store is visible before we update the last entry */
	__ha_barrier_store();

	if (unlikely(last == -1)) {
		/* list is empty, try to add ourselves alone so that list->last=fd */
		if (unlikely(!_HA_ATOMIC_CAS(&list->last, &old, new)))
			    goto redo_last;

		/* list->first was necessary -1, we're guaranteed to be alone here */
		list->first = fd;
	} else {
		/* adding ourselves past the last element
		 * The CAS will only succeed if its next is -1,
		 * which means it's in the cache, and the last element.
		 */
		if (unlikely(!_HA_ATOMIC_CAS(&_GET_NEXT(last, off), &old, new)))
			goto redo_last;

		/* Then, update the last entry */
		list->last = fd;
	}
	__ha_barrier_store();
	/* since we're alone at the end of the list and still locked(-2),
	 * we know noone tried to add past us. Mark the end of list.
	 */
	_GET_PREV(fd, off) = last;
	_GET_NEXT(fd, off) = -1;
	__ha_barrier_store();
done:
	return;
}

/* removes fd <fd> from fd list <list> */
void fd_rm_from_fd_list(volatile struct fdlist *list, int fd, int off)
{
#if defined(HA_HAVE_CAS_DW) || defined(HA_CAS_IS_8B)
	volatile struct fdlist_entry cur_list, next_list;
#endif
	int old;
	int new = -2;
	int prev;
	int next;
	int last;
lock_self:
#if (defined(HA_CAS_IS_8B) || defined(HA_HAVE_CAS_DW))
	next_list.next = next_list.prev = -2;
	cur_list = *(volatile struct fdlist_entry *)(((char *)&fdtab[fd]) + off);
	/* First, attempt to lock our own entries */
	do {
		/* The FD is not in the FD cache, give up */
		if (unlikely(cur_list.next <= -3))
			return;
		if (unlikely(cur_list.prev == -2 || cur_list.next == -2))
			goto lock_self;
	} while (
#ifdef HA_CAS_IS_8B
	    unlikely(!_HA_ATOMIC_CAS(((void **)(void *)&_GET_NEXT(fd, off)), ((void **)(void *)&cur_list), (*(void **)(void *)&next_list))))
#else
	    unlikely(!_HA_ATOMIC_DWCAS(((void *)&_GET_NEXT(fd, off)), ((void *)&cur_list), ((void *)&next_list))))
#endif
	    ;
	next = cur_list.next;
	prev = cur_list.prev;

#else
lock_self_next:
	next = ({ volatile int *next = &_GET_NEXT(fd, off); *next; });
	if (next == -2)
		goto lock_self_next;
	if (next <= -3)
		goto done;
	if (unlikely(!_HA_ATOMIC_CAS(&_GET_NEXT(fd, off), &next, -2)))
		goto lock_self_next;
lock_self_prev:
	prev = ({ volatile int *prev = &_GET_PREV(fd, off); *prev; });
	if (prev == -2)
		goto lock_self_prev;
	if (unlikely(!_HA_ATOMIC_CAS(&_GET_PREV(fd, off), &prev, -2)))
		goto lock_self_prev;
#endif
	__ha_barrier_atomic_store();

	/* Now, lock the entries of our neighbours */
	if (likely(prev != -1)) {
redo_prev:
		old = fd;

		if (unlikely(!_HA_ATOMIC_CAS(&_GET_NEXT(prev, off), &old, new))) {
			if (unlikely(old == -2)) {
				/* Neighbour already locked, give up and
				 * retry again once he's done
				 */
				_GET_PREV(fd, off) = prev;
				__ha_barrier_store();
				_GET_NEXT(fd, off) = next;
				__ha_barrier_store();
				goto lock_self;
			}
			goto redo_prev;
		}
	}
	if (likely(next != -1)) {
redo_next:
		old = fd;
		if (unlikely(!_HA_ATOMIC_CAS(&_GET_PREV(next, off), &old, new))) {
			if (unlikely(old == -2)) {
				/* Neighbour already locked, give up and
				 * retry again once he's done
				 */
				if (prev != -1) {
					_GET_NEXT(prev, off) = fd;
					__ha_barrier_store();
				}
				_GET_PREV(fd, off) = prev;
				__ha_barrier_store();
				_GET_NEXT(fd, off) = next;
				__ha_barrier_store();
				goto lock_self;
			}
			goto redo_next;
		}
	}
	if (list->first == fd)
		list->first = next;
	__ha_barrier_store();
	last = list->last;
	while (unlikely(last == fd && (!_HA_ATOMIC_CAS(&list->last, &last, prev))))
		__ha_compiler_barrier();
	/* Make sure we let other threads know we're no longer in cache,
	 * before releasing our neighbours.
	 */
	__ha_barrier_store();
	if (likely(prev != -1))
		_GET_NEXT(prev, off) = next;
	__ha_barrier_store();
	if (likely(next != -1))
		_GET_PREV(next, off) = prev;
	__ha_barrier_store();
	/* Ok, now we're out of the fd cache */
	_GET_NEXT(fd, off) = -(next + 4);
	__ha_barrier_store();
done:
	return;
}

#undef _GET_NEXT
#undef _GET_PREV

/* Deletes an FD from the fdsets.
 * The file descriptor is also closed.
 */
static void fd_dodelete(int fd, int do_close)
{
	unsigned long locked = atleast2(fdtab[fd].thread_mask);

	if (locked)
		HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fdtab[fd].linger_risk) {
		/* this is generally set when connecting to servers */
		setsockopt(fd, SOL_SOCKET, SO_LINGER,
			   (struct linger *) &nolinger, sizeof(struct linger));
	}
	if (cur_poller.clo)
		cur_poller.clo(fd);

	fd_release_cache_entry(fd);
	fdtab[fd].state = 0;

	port_range_release_port(fdinfo[fd].port_range, fdinfo[fd].local_port);
	fdinfo[fd].port_range = NULL;
	fdtab[fd].owner = NULL;
	fdtab[fd].thread_mask = 0;
	if (do_close) {
		polled_mask[fd] = 0;
		close(fd);
		_HA_ATOMIC_SUB(&ha_used_fds, 1);
	}
	if (locked)
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Deletes an FD from the fdsets.
 * The file descriptor is also closed.
 */
void fd_delete(int fd)
{
	fd_dodelete(fd, 1);
}

/* Deletes an FD from the fdsets.
 * The file descriptor is kept open.
 */
void fd_remove(int fd)
{
	fd_dodelete(fd, 0);
}

static inline void fdlist_process_cached_events(volatile struct fdlist *fdlist)
{
	int fd, old_fd, e;
	unsigned long locked;

	for (old_fd = fd = fdlist->first; fd != -1; fd = fdtab[fd].cache.next) {
		if (fd == -2) {
			fd = old_fd;
			continue;
		} else if (fd <= -3)
			fd = -fd - 4;
		if (fd == -1)
			break;
		old_fd = fd;
		if (!(fdtab[fd].thread_mask & tid_bit))
			continue;
		if (fdtab[fd].cache.next < -3)
			continue;

		_HA_ATOMIC_OR(&fd_cache_mask, tid_bit);
		locked = atleast2(fdtab[fd].thread_mask);
		if (locked && HA_SPIN_TRYLOCK(FD_LOCK, &fdtab[fd].lock)) {
			activity[tid].fd_lock++;
			continue;
		}

		e = fdtab[fd].state;
		fdtab[fd].ev &= FD_POLL_STICKY;

		if ((e & (FD_EV_READY_R | FD_EV_ACTIVE_R)) == (FD_EV_READY_R | FD_EV_ACTIVE_R))
			fdtab[fd].ev |= FD_POLL_IN;

		if ((e & (FD_EV_READY_W | FD_EV_ACTIVE_W)) == (FD_EV_READY_W | FD_EV_ACTIVE_W))
			fdtab[fd].ev |= FD_POLL_OUT;

		if (fdtab[fd].iocb && fdtab[fd].owner && fdtab[fd].ev) {
			if (locked)
				HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
			fdtab[fd].iocb(fd);
		}
		else {
			fd_release_cache_entry(fd);
			if (locked)
				HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		}
	}
}

/* Scan and process the cached events. This should be called right after
 * the poller. The loop may cause new entries to be created, for example
 * if a listener causes an accept() to initiate a new incoming connection
 * wanting to attempt an recv().
 */
void fd_process_cached_events()
{
	_HA_ATOMIC_AND(&fd_cache_mask, ~tid_bit);
	fdlist_process_cached_events(&fd_cache_local[tid]);
	fdlist_process_cached_events(&fd_cache);
}

#if defined(USE_CLOSEFROM)
void my_closefrom(int start)
{
	closefrom(start);
}

#elif defined(USE_POLL)
/* This is a portable implementation of closefrom(). It closes all open file
 * descriptors starting at <start> and above. It relies on the fact that poll()
 * will return POLLNVAL for each invalid (hence close) file descriptor passed
 * in argument in order to skip them. It acts with batches of FDs and will
 * typically perform one poll() call per 1024 FDs so the overhead is low in
 * case all FDs have to be closed.
 */
void my_closefrom(int start)
{
	struct pollfd poll_events[1024];
	struct rlimit limit;
	int nbfds, fd, ret, idx;
	int step, next;

	if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
		step = nbfds = limit.rlim_cur;
	else
		step = nbfds = 0;

	if (nbfds <= 0) {
		/* set safe limit */
		nbfds = 1024;
		step = 256;
	}

	if (step > sizeof(poll_events) / sizeof(poll_events[0]))
		step = sizeof(poll_events) / sizeof(poll_events[0]);

	while (start < nbfds) {
		next = (start / step + 1) * step;

		for (fd = start; fd < next && fd < nbfds; fd++) {
			poll_events[fd - start].fd = fd;
			poll_events[fd - start].events = 0;
		}

		do {
			ret = poll(poll_events, fd - start, 0);
			if (ret >= 0)
				break;
		} while (errno == EAGAIN || errno == EINTR || errno == ENOMEM);

		if (ret)
			ret = fd - start;

		for (idx = 0; idx < ret; idx++) {
			if (poll_events[idx].revents & POLLNVAL)
				continue; /* already closed */

			fd = poll_events[idx].fd;
			close(fd);
		}
		start = next;
	}
}

#else // defined(USE_POLL)

/* This is a portable implementation of closefrom(). It closes all open file
 * descriptors starting at <start> and above. This is a naive version for use
 * when the operating system provides no alternative.
 */
void my_closefrom(int start)
{
	struct rlimit limit;
	int nbfds;

	if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
		nbfds = limit.rlim_cur;
	else
		nbfds = 0;

	if (nbfds <= 0)
		nbfds = 1024; /* safe limit */

	while (start < nbfds)
		close(start++);
}
#endif // defined(USE_POLL)

/* disable the specified poller */
void disable_poller(const char *poller_name)
{
	int p;

	for (p = 0; p < nbpollers; p++)
		if (strcmp(pollers[p].name, poller_name) == 0)
			pollers[p].pref = 0;
}

void poller_pipe_io_handler(int fd)
{
	char buf[1024];
	/* Flush the pipe */
	while (read(fd, buf, sizeof(buf)) > 0);
	fd_cant_recv(fd);
}

/* allocate the per-thread fd_updt thus needs to be called early after
 * thread creation.
 */
static int alloc_pollers_per_thread()
{
	fd_updt = calloc(global.maxsock, sizeof(*fd_updt));
	return fd_updt != NULL;
}

/* Initialize the pollers per thread.*/
static int init_pollers_per_thread()
{
	int mypipe[2];

	if (pipe(mypipe) < 0)
		return 0;

	poller_rd_pipe = mypipe[0];
	poller_wr_pipe[tid] = mypipe[1];
	fcntl(poller_rd_pipe, F_SETFL, O_NONBLOCK);
	fd_insert(poller_rd_pipe, poller_pipe_io_handler, poller_pipe_io_handler,
	    tid_bit);
	fd_want_recv(poller_rd_pipe);
	return 1;
}

/* Deinitialize the pollers per thread */
static void deinit_pollers_per_thread()
{
	/* rd and wr are init at the same place, but only rd is init to -1, so
	  we rely to rd to close.   */
	if (poller_rd_pipe > -1) {
		close(poller_rd_pipe);
		poller_rd_pipe = -1;
		close(poller_wr_pipe[tid]);
		poller_wr_pipe[tid] = -1;
	}
}

/* Release the pollers per thread, to be called late */
static void free_pollers_per_thread()
{
	free(fd_updt);
	fd_updt = NULL;
}

/*
 * Initialize the pollers till the best one is found.
 * If none works, returns 0, otherwise 1.
 */
int init_pollers()
{
	int p;
	struct poller *bp;

	if ((fdtab = calloc(global.maxsock, sizeof(struct fdtab))) == NULL)
		goto fail_tab;

	if ((polled_mask = calloc(global.maxsock, sizeof(unsigned long))) == NULL)
		goto fail_polledmask;

	if ((fdinfo = calloc(global.maxsock, sizeof(struct fdinfo))) == NULL)
		goto fail_info;

	fd_cache.first = fd_cache.last = -1;
	update_list.first = update_list.last = -1;

	for (p = 0; p < global.maxsock; p++) {
		HA_SPIN_INIT(&fdtab[p].lock);
		/* Mark the fd as out of the fd cache */
		fdtab[p].cache.next = -3;
		fdtab[p].update.next = -3;
	}
	for (p = 0; p < global.nbthread; p++)
		fd_cache_local[p].first = fd_cache_local[p].last = -1;

	do {
		bp = NULL;
		for (p = 0; p < nbpollers; p++)
			if (!bp || (pollers[p].pref > bp->pref))
				bp = &pollers[p];

		if (!bp || bp->pref == 0)
			break;

		if (bp->init(bp)) {
			memcpy(&cur_poller, bp, sizeof(*bp));
			return 1;
		}
	} while (!bp || bp->pref == 0);

	free(fdinfo);
 fail_info:
	free(polled_mask);
 fail_polledmask:
	free(fdtab);
 fail_tab:
	return 0;
}

/*
 * Deinitialize the pollers.
 */
void deinit_pollers() {

	struct poller *bp;
	int p;

	for (p = 0; p < global.maxsock; p++)
		HA_SPIN_DESTROY(&fdtab[p].lock);

	for (p = 0; p < nbpollers; p++) {
		bp = &pollers[p];

		if (bp && bp->pref)
			bp->term(bp);
	}

	free(fdinfo);   fdinfo   = NULL;
	free(fdtab);    fdtab    = NULL;
	free(polled_mask); polled_mask = NULL;
}

/*
 * Lists the known pollers on <out>.
 * Should be performed only before initialization.
 */
int list_pollers(FILE *out)
{
	int p;
	int last, next;
	int usable;
	struct poller *bp;

	fprintf(out, "Available polling systems :\n");

	usable = 0;
	bp = NULL;
	last = next = -1;
	while (1) {
		for (p = 0; p < nbpollers; p++) {
			if ((next < 0 || pollers[p].pref > next)
			    && (last < 0 || pollers[p].pref < last)) {
				next = pollers[p].pref;
				if (!bp || (pollers[p].pref > bp->pref))
					bp = &pollers[p];
			}
		}

		if (next == -1)
			break;

		for (p = 0; p < nbpollers; p++) {
			if (pollers[p].pref == next) {
				fprintf(out, " %10s : ", pollers[p].name);
				if (pollers[p].pref == 0)
					fprintf(out, "disabled, ");
				else
					fprintf(out, "pref=%3d, ", pollers[p].pref);
				if (pollers[p].test(&pollers[p])) {
					fprintf(out, " test result OK");
					if (next > 0)
						usable++;
				} else {
					fprintf(out, " test result FAILED");
					if (bp == &pollers[p])
						bp = NULL;
				}
				fprintf(out, "\n");
			}
		}
		last = next;
		next = -1;
	};
	fprintf(out, "Total: %d (%d usable), will use %s.\n", nbpollers, usable, bp ? bp->name : "none");
	return 0;
}

/*
 * Some pollers may lose their connection after a fork(). It may be necessary
 * to create initialize part of them again. Returns 0 in case of failure,
 * otherwise 1. The fork() function may be NULL if unused. In case of error,
 * the the current poller is destroyed and the caller is responsible for trying
 * another one by calling init_pollers() again.
 */
int fork_poller()
{
	int fd;
	for (fd = 0; fd < global.maxsock; fd++) {
		if (fdtab[fd].owner) {
			fdtab[fd].cloned = 1;
		}
	}

	if (cur_poller.fork) {
		if (cur_poller.fork(&cur_poller))
			return 1;
		cur_poller.term(&cur_poller);
		return 0;
	}
	return 1;
}

REGISTER_PER_THREAD_ALLOC(alloc_pollers_per_thread);
REGISTER_PER_THREAD_INIT(init_pollers_per_thread);
REGISTER_PER_THREAD_DEINIT(deinit_pollers_per_thread);
REGISTER_PER_THREAD_FREE(free_pollers_per_thread);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
