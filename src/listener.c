/*
 * Listener management functions.
 *
 * Copyright 2000-2013 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <common/accept4.h>
#include <common/cfgparse.h>
#include <common/config.h>
#include <common/errors.h>
#include <common/initcall.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/time.h>

#include <types/global.h>
#include <types/protocol.h>

#include <proto/acl.h>
#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/log.h>
#include <proto/listener.h>
#include <proto/protocol.h>
#include <proto/proto_sockpair.h>
#include <proto/sample.h>
#include <proto/stream.h>
#include <proto/task.h>

/* List head of all known bind keywords */
static struct bind_kw_list bind_keywords = {
	.list = LIST_HEAD_INIT(bind_keywords.list)
};

struct xfer_sock_list *xfer_sock_list = NULL;

/* there is one listener queue per thread so that a thread unblocking the
 * global queue can wake up listeners bound only to foreing threads by
 * moving them to the remote queues and waking up the associated task.
 */
static struct work_list *local_listener_queue;

/* This function adds the specified listener's file descriptor to the polling
 * lists if it is in the LI_LISTEN state. The listener enters LI_READY or
 * LI_FULL state depending on its number of connections. In deamon mode, we
 * also support binding only the relevant processes to their respective
 * listeners. We don't do that in debug mode however.
 */
static void enable_listener(struct listener *listener)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &listener->lock);
	if (listener->state == LI_LISTEN) {
		if ((global.mode & (MODE_DAEMON | MODE_MWORKER)) &&
		    listener->bind_conf->bind_proc &&
		    !(listener->bind_conf->bind_proc & pid_bit)) {
			/* we don't want to enable this listener and don't
			 * want any fd event to reach it.
			 */
			if (!(global.tune.options & GTUNE_SOCKET_TRANSFER))
				do_unbind_listener(listener, 1);
			else {
				do_unbind_listener(listener, 0);
				listener->state = LI_LISTEN;
			}
		}
		else if (listener->nbconn < listener->maxconn) {
			fd_want_recv(listener->fd);
			listener->state = LI_READY;
		}
		else {
			listener->state = LI_FULL;
		}
	}
	/* if this listener is supposed to be only in the master, close it in the workers */
	if ((global.mode & MODE_MWORKER) &&
	    (listener->options & LI_O_MWORKER) &&
	    master == 0) {
		do_unbind_listener(listener, 1);
	}
	HA_SPIN_UNLOCK(LISTENER_LOCK, &listener->lock);
}

/* This function removes the specified listener's file descriptor from the
 * polling lists if it is in the LI_READY or in the LI_FULL state. The listener
 * enters LI_LISTEN.
 */
static void disable_listener(struct listener *listener)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &listener->lock);
	if (listener->state < LI_READY)
		goto end;
	if (listener->state == LI_READY)
		fd_stop_recv(listener->fd);
	LIST_DEL_LOCKED(&listener->wait_queue);
	listener->state = LI_LISTEN;
  end:
	HA_SPIN_UNLOCK(LISTENER_LOCK, &listener->lock);
}

/* This function tries to temporarily disable a listener, depending on the OS
 * capabilities. Linux unbinds the listen socket after a SHUT_RD, and ignores
 * SHUT_WR. Solaris refuses either shutdown(). OpenBSD ignores SHUT_RD but
 * closes upon SHUT_WR and refuses to rebind. So a common validation path
 * involves SHUT_WR && listen && SHUT_RD. In case of success, the FD's polling
 * is disabled. It normally returns non-zero, unless an error is reported.
 */
int pause_listener(struct listener *l)
{
	int ret = 1;

	HA_SPIN_LOCK(LISTENER_LOCK, &l->lock);

	if (l->state <= LI_ZOMBIE)
		goto end;

	if (l->proto->pause) {
		/* Returns < 0 in case of failure, 0 if the listener
		 * was totally stopped, or > 0 if correctly paused.
		 */
		int ret = l->proto->pause(l);

		if (ret < 0) {
			ret = 0;
			goto end;
		}
		else if (ret == 0)
			goto end;
	}

	LIST_DEL_LOCKED(&l->wait_queue);

	fd_stop_recv(l->fd);
	l->state = LI_PAUSED;
  end:
	HA_SPIN_UNLOCK(LISTENER_LOCK, &l->lock);
	return ret;
}

/* This function tries to resume a temporarily disabled listener. Paused, full,
 * limited and disabled listeners are handled, which means that this function
 * may replace enable_listener(). The resulting state will either be LI_READY
 * or LI_FULL. 0 is returned in case of failure to resume (eg: dead socket).
 * Listeners bound to a different process are not woken up unless we're in
 * foreground mode, and are ignored. If the listener was only in the assigned
 * state, it's totally rebound. This can happen if a pause() has completely
 * stopped it. If the resume fails, 0 is returned and an error might be
 * displayed.
 */
int resume_listener(struct listener *l)
{
	int ret = 1;
	unsigned long thr_mask;

	HA_SPIN_LOCK(LISTENER_LOCK, &l->lock);

	/* check that another thread didn't to the job in parallel (e.g. at the
	 * end of listen_accept() while we'd come from dequeue_all_listeners().
	 */
	if (!LIST_ISEMPTY(&l->wait_queue))
		goto end;

	if ((global.mode & (MODE_DAEMON | MODE_MWORKER)) &&
	    l->bind_conf->bind_proc &&
	    !(l->bind_conf->bind_proc & pid_bit))
		goto end;

	if (l->state == LI_ASSIGNED) {
		char msg[100];
		int err;

		err = l->proto->bind(l, msg, sizeof(msg));
		if (err & ERR_ALERT)
			ha_alert("Resuming listener: %s\n", msg);
		else if (err & ERR_WARN)
			ha_warning("Resuming listener: %s\n", msg);

		if (err & (ERR_FATAL | ERR_ABORT)) {
			ret = 0;
			goto end;
		}
	}

	if (l->state < LI_PAUSED || l->state == LI_ZOMBIE) {
		ret = 0;
		goto end;
	}

	if (l->proto->sock_prot == IPPROTO_TCP &&
	    l->state == LI_PAUSED &&
	    listen(l->fd, l->backlog ? l->backlog : l->maxconn) != 0) {
		ret = 0;
		goto end;
	}

	if (l->state == LI_READY)
		goto end;

	LIST_DEL_LOCKED(&l->wait_queue);

	if (l->nbconn >= l->maxconn) {
		l->state = LI_FULL;
		goto end;
	}

	thr_mask = l->bind_conf->bind_thread[relative_pid-1] ?
	           l->bind_conf->bind_thread[relative_pid-1] : MAX_THREADS_MASK;

	if (!(thr_mask & tid_bit)) {
		/* we're not allowed to touch this listener's FD, let's requeue
		 * the listener into one of its owning thread's queue instead.
		 */
		int last_thread = my_ffsl(thr_mask) - 1;
		work_list_add(&local_listener_queue[last_thread], &l->wait_queue);
		goto end;
	}

	fd_want_recv(l->fd);
	l->state = LI_READY;
  end:
	HA_SPIN_UNLOCK(LISTENER_LOCK, &l->lock);
	return ret;
}

/* Marks a ready listener as full so that the stream code tries to re-enable
 * it upon next close() using resume_listener().
 */
static void listener_full(struct listener *l)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &l->lock);
	if (l->state >= LI_READY) {
		LIST_DEL_LOCKED(&l->wait_queue);
		if (l->state != LI_FULL) {
			fd_stop_recv(l->fd);
			l->state = LI_FULL;
		}
	}
	HA_SPIN_UNLOCK(LISTENER_LOCK, &l->lock);
}

/* Marks a ready listener as limited so that we only try to re-enable it when
 * resources are free again. It will be queued into the specified queue.
 */
static void limit_listener(struct listener *l, struct list *list)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &l->lock);
	if (l->state == LI_READY) {
		LIST_ADDQ_LOCKED(list, &l->wait_queue);
		fd_stop_recv(l->fd);
		l->state = LI_LIMITED;
	}
	HA_SPIN_UNLOCK(LISTENER_LOCK, &l->lock);
}

/* This function adds all of the protocol's listener's file descriptors to the
 * polling lists when they are in the LI_LISTEN state. It is intended to be
 * used as a protocol's generic enable_all() primitive, for use after the
 * fork(). It puts the listeners into LI_READY or LI_FULL states depending on
 * their number of connections. It always returns ERR_NONE.
 *
 * Must be called with proto_lock held.
 *
 */
int enable_all_listeners(struct protocol *proto)
{
	struct listener *listener;

	list_for_each_entry(listener, &proto->listeners, proto_list)
		enable_listener(listener);
	return ERR_NONE;
}

/* This function removes all of the protocol's listener's file descriptors from
 * the polling lists when they are in the LI_READY or LI_FULL states. It is
 * intended to be used as a protocol's generic disable_all() primitive. It puts
 * the listeners into LI_LISTEN, and always returns ERR_NONE.
 *
 * Must be called with proto_lock held.
 *
 */
int disable_all_listeners(struct protocol *proto)
{
	struct listener *listener;

	list_for_each_entry(listener, &proto->listeners, proto_list)
		disable_listener(listener);
	return ERR_NONE;
}

/* Dequeues all of the listeners waiting for a resource in wait queue <queue>. */
void dequeue_all_listeners(struct list *list)
{
	struct listener *listener;

	while ((listener = LIST_POP_LOCKED(list, struct listener *, wait_queue))) {
		/* This cannot fail because the listeners are by definition in
		 * the LI_LIMITED state.
		 */
		resume_listener(listener);
	}
}

/* Must be called with the lock held. Depending on <do_close> value, it does
 * what unbind_listener or unbind_listener_no_close should do.
 */
void do_unbind_listener(struct listener *listener, int do_close)
{
	if (listener->state == LI_READY && fd_updt)
		fd_stop_recv(listener->fd);

	LIST_DEL_LOCKED(&listener->wait_queue);

	if (listener->state >= LI_PAUSED) {
		if (do_close) {
			fd_delete(listener->fd);
			listener->fd = -1;
		}
		else
			fd_remove(listener->fd);
		listener->state = LI_ASSIGNED;
	}
}

/* This function closes the listening socket for the specified listener,
 * provided that it's already in a listening state. The listener enters the
 * LI_ASSIGNED state. This function is intended to be used as a generic
 * function for standard protocols.
 */
void unbind_listener(struct listener *listener)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &listener->lock);
	do_unbind_listener(listener, 1);
	HA_SPIN_UNLOCK(LISTENER_LOCK, &listener->lock);
}

/* This function pretends the listener is dead, but keeps the FD opened, so
 * that we can provide it, for conf reloading.
 */
void unbind_listener_no_close(struct listener *listener)
{
	HA_SPIN_LOCK(LISTENER_LOCK, &listener->lock);
	do_unbind_listener(listener, 0);
	HA_SPIN_UNLOCK(LISTENER_LOCK, &listener->lock);
}

/* This function closes all listening sockets bound to the protocol <proto>,
 * and the listeners end in LI_ASSIGNED state if they were higher. It does not
 * detach them from the protocol. It always returns ERR_NONE.
 *
 * Must be called with proto_lock held.
 *
 */
int unbind_all_listeners(struct protocol *proto)
{
	struct listener *listener;

	list_for_each_entry(listener, &proto->listeners, proto_list)
		unbind_listener(listener);
	return ERR_NONE;
}

/* creates one or multiple listeners for bind_conf <bc> on sockaddr <ss> on port
 * range <portl> to <porth>, and possibly attached to fd <fd> (or -1 for auto
 * allocation). The address family is taken from ss->ss_family. The number of
 * jobs and listeners is automatically increased by the number of listeners
 * created. If the <inherited> argument is set to 1, it specifies that the FD
 * was obtained from a parent process.
 * It returns non-zero on success, zero on error with the error message
 * set in <err>.
 */
int create_listeners(struct bind_conf *bc, const struct sockaddr_storage *ss,
                     int portl, int porth, int fd, int inherited, char **err)
{
	struct protocol *proto = protocol_by_family(ss->ss_family);
	struct listener *l;
	int port;

	if (!proto) {
		memprintf(err, "unsupported protocol family %d", ss->ss_family);
		return 0;
	}

	for (port = portl; port <= porth; port++) {
		l = calloc(1, sizeof(*l));
		if (!l) {
			memprintf(err, "out of memory");
			return 0;
		}
		l->obj_type = OBJ_TYPE_LISTENER;
		LIST_ADDQ(&bc->frontend->conf.listeners, &l->by_fe);
		LIST_ADDQ(&bc->listeners, &l->by_bind);
		l->bind_conf = bc;

		l->fd = fd;
		memcpy(&l->addr, ss, sizeof(*ss));
		LIST_INIT(&l->wait_queue);
		l->state = LI_INIT;

		proto->add(l, port);

		if (inherited)
			l->options |= LI_O_INHERITED;

		HA_SPIN_INIT(&l->lock);
		HA_ATOMIC_ADD(&jobs, 1);
		HA_ATOMIC_ADD(&listeners, 1);
	}
	return 1;
}

/* Delete a listener from its protocol's list of listeners. The listener's
 * state is automatically updated from LI_ASSIGNED to LI_INIT. The protocol's
 * number of listeners is updated, as well as the global number of listeners
 * and jobs. Note that the listener must have previously been unbound. This
 * is the generic function to use to remove a listener.
 *
 * Will grab the proto_lock.
 *
 */
void delete_listener(struct listener *listener)
{
	HA_SPIN_LOCK(PROTO_LOCK, &proto_lock);
	HA_SPIN_LOCK(LISTENER_LOCK, &listener->lock);
	if (listener->state == LI_ASSIGNED) {
		listener->state = LI_INIT;
		LIST_DEL(&listener->proto_list);
		listener->proto->nb_listeners--;
		HA_ATOMIC_SUB(&jobs, 1);
		HA_ATOMIC_SUB(&listeners, 1);
	}
	HA_SPIN_UNLOCK(LISTENER_LOCK, &listener->lock);
	HA_SPIN_UNLOCK(PROTO_LOCK, &proto_lock);
}

/* This function is called on a read event from a listening socket, corresponding
 * to an accept. It tries to accept as many connections as possible, and for each
 * calls the listener's accept handler (generally the frontend's accept handler).
 */
void listener_accept(int fd)
{
	struct listener *l = fdtab[fd].owner;
	struct proxy *p;
	unsigned int max_accept;
	int next_conn = 0;
	int next_feconn = 0;
	int next_actconn = 0;
	int expire;
	int cfd;
	int ret;
#ifdef USE_ACCEPT4
	static int accept4_broken;
#endif

	if (!l)
		return;
	p = l->bind_conf->frontend;

	/* if l->maxaccept is -1, then max_accept is UINT_MAX. It is not really
	 * illimited, but it is probably enough.
	 */
	max_accept = l->maxaccept ? l->maxaccept : 1;

	if (!(l->options & LI_O_UNLIMITED) && global.sps_lim) {
		int max = freq_ctr_remain(&global.sess_per_sec, global.sps_lim, 0);

		if (unlikely(!max)) {
			/* frontend accept rate limit was reached */
			expire = tick_add(now_ms, next_event_delay(&global.sess_per_sec, global.sps_lim, 0));
			goto wait_expire;
		}

		if (max_accept > max)
			max_accept = max;
	}

	if (!(l->options & LI_O_UNLIMITED) && global.cps_lim) {
		int max = freq_ctr_remain(&global.conn_per_sec, global.cps_lim, 0);

		if (unlikely(!max)) {
			/* frontend accept rate limit was reached */
			expire = tick_add(now_ms, next_event_delay(&global.conn_per_sec, global.cps_lim, 0));
			goto wait_expire;
		}

		if (max_accept > max)
			max_accept = max;
	}
#ifdef USE_OPENSSL
	if (!(l->options & LI_O_UNLIMITED) && global.ssl_lim && l->bind_conf && l->bind_conf->is_ssl) {
		int max = freq_ctr_remain(&global.ssl_per_sec, global.ssl_lim, 0);

		if (unlikely(!max)) {
			/* frontend accept rate limit was reached */
			expire = tick_add(now_ms, next_event_delay(&global.ssl_per_sec, global.ssl_lim, 0));
			goto wait_expire;
		}

		if (max_accept > max)
			max_accept = max;
	}
#endif
	if (p && p->fe_sps_lim) {
		int max = freq_ctr_remain(&p->fe_sess_per_sec, p->fe_sps_lim, 0);

		if (unlikely(!max)) {
			/* frontend accept rate limit was reached */
			limit_listener(l, &p->listener_queue);
			task_schedule(p->task, tick_add(now_ms, next_event_delay(&p->fe_sess_per_sec, p->fe_sps_lim, 0)));
			goto end;
		}

		if (max_accept > max)
			max_accept = max;
	}

	/* Note: if we fail to allocate a connection because of configured
	 * limits, we'll schedule a new attempt worst 1 second later in the
	 * worst case. If we fail due to system limits or temporary resource
	 * shortage, we try again 100ms later in the worst case.
	 */
	for (; max_accept; next_conn = next_feconn = next_actconn = 0, max_accept--) {
		struct sockaddr_storage addr;
		socklen_t laddr = sizeof(addr);
		unsigned int count;

		/* pre-increase the number of connections without going too far.
		 * We process the listener, then the proxy, then the process.
		 * We know which ones to unroll based on the next_xxx value.
		 */
		do {
			count = l->nbconn;
			if (count >= l->maxconn) {
				/* the listener was marked full or another
				 * thread is going to do it.
				 */
				next_conn = 0;
				goto end;
			}
			next_conn = count + 1;
		} while (!HA_ATOMIC_CAS(&l->nbconn, (int *)&count, next_conn));

		if (next_conn == l->maxconn) {
			/* we filled it, mark it full */
			listener_full(l);
		}

		if (p) {
			do {
				count = p->feconn;
				if (count >= p->maxconn) {
					/* the frontend was marked full or another
					 * thread is going to do it.
					 */
					next_feconn = 0;
					goto end;
				}
				next_feconn = count + 1;
			} while (!HA_ATOMIC_CAS(&p->feconn, &count, next_feconn));

			if (unlikely(next_feconn == p->maxconn)) {
				/* we just filled it */
				limit_listener(l, &p->listener_queue);
			}
		}

		if (!(l->options & LI_O_UNLIMITED)) {
			do {
				count = actconn;
				if (count >= global.maxconn) {
					/* the process was marked full or another
					 * thread is going to do it.
					 */
					next_actconn = 0;
					goto end;
				}
				next_actconn = count + 1;
			} while (!HA_ATOMIC_CAS(&actconn, (int *)&count, next_actconn));

			if (unlikely(next_actconn == global.maxconn)) {
				limit_listener(l, &global_listener_queue);
				task_schedule(global_listener_queue_task, tick_add(now_ms, 1000)); /* try again in 1 second */
			}
		}

		/* with sockpair@ we don't want to do an accept */
		if (unlikely(l->addr.ss_family == AF_CUST_SOCKPAIR)) {
			if ((cfd = recv_fd_uxst(fd)) != -1)
				fcntl(cfd, F_SETFL, O_NONBLOCK);
			/* just like with UNIX sockets, only the family is filled */
			addr.ss_family = AF_UNIX;
			laddr = sizeof(addr.ss_family);
		} else

#ifdef USE_ACCEPT4
		/* only call accept4() if it's known to be safe, otherwise
		 * fallback to the legacy accept() + fcntl().
		 */
		if (unlikely(accept4_broken ||
			((cfd = accept4(fd, (struct sockaddr *)&addr, &laddr, SOCK_NONBLOCK)) == -1 &&
			(errno == ENOSYS || errno == EINVAL || errno == EBADF) &&
			(accept4_broken = 1))))
#endif
			if ((cfd = accept(fd, (struct sockaddr *)&addr, &laddr)) != -1)
				fcntl(cfd, F_SETFL, O_NONBLOCK);

		if (unlikely(cfd == -1)) {
			switch (errno) {
			case EAGAIN:
				if (fdtab[fd].ev & FD_POLL_HUP) {
					/* the listening socket might have been disabled in a shared
					 * process and we're a collateral victim. We'll just pause for
					 * a while in case it comes back. In the mean time, we need to
					 * clear this sticky flag.
					 */
					fdtab[fd].ev &= ~FD_POLL_HUP;
					goto transient_error;
				}
				fd_cant_recv(fd);
				goto end; /* nothing more to accept */
			case EINVAL:
				/* might be trying to accept on a shut fd (eg: soft stop) */
				goto transient_error;
			case EINTR:
			case ECONNABORTED:
				HA_ATOMIC_SUB(&l->nbconn, 1);
				if (p)
					HA_ATOMIC_SUB(&p->feconn, 1);
				if (!(l->options & LI_O_UNLIMITED))
					HA_ATOMIC_SUB(&actconn, 1);
				continue;
			case ENFILE:
				if (p)
					send_log(p, LOG_EMERG,
						 "Proxy %s reached system FD limit (maxsock=%d). Please check system tunables.\n",
						 p->id, global.maxsock);
				goto transient_error;
			case EMFILE:
				if (p)
					send_log(p, LOG_EMERG,
						 "Proxy %s reached process FD limit (maxsock=%d). Please check 'ulimit-n' and restart.\n",
						 p->id, global.maxsock);
				goto transient_error;
			case ENOBUFS:
			case ENOMEM:
				if (p)
					send_log(p, LOG_EMERG,
						 "Proxy %s reached system memory limit (maxsock=%d). Please check system tunables.\n",
						 p->id, global.maxsock);
				goto transient_error;
			default:
				/* unexpected result, let's give up and let other tasks run */
				goto stop;
			}
		}

		/* we don't want to leak the FD upon reload if it's in the master */
		if (unlikely(master == 1))
			fcntl(cfd, F_SETFD, FD_CLOEXEC);

		/* The connection was accepted, it must be counted as such */
		if (l->counters)
			HA_ATOMIC_UPDATE_MAX(&l->counters->conn_max, next_conn);

		if (p)
			HA_ATOMIC_UPDATE_MAX(&p->fe_counters.conn_max, next_feconn);

		proxy_inc_fe_conn_ctr(l, p);

		if (!(l->options & LI_O_UNLIMITED)) {
			count = update_freq_ctr(&global.conn_per_sec, 1);
			HA_ATOMIC_UPDATE_MAX(&global.cps_max, count);
		}

		if (unlikely(cfd >= global.maxsock)) {
			send_log(p, LOG_EMERG,
				 "Proxy %s reached the configured maximum connection limit. Please check the global 'maxconn' value.\n",
				 p->id);
			close(cfd);
			limit_listener(l, &global_listener_queue);
			task_schedule(global_listener_queue_task, tick_add(now_ms, 1000)); /* try again in 1 second */
			goto end;
		}

		/* past this point, l->accept() will automatically decrement
		 * l->nbconn, feconn and actconn once done. Setting next_*conn=0
		 * allows the error path not to rollback on nbconn. It's more
		 * convenient than duplicating all exit labels.
		 */
		next_conn = 0;
		next_feconn = 0;
		next_actconn = 0;

		ret = l->accept(l, cfd, &addr);
		if (unlikely(ret <= 0)) {
			/* The connection was closed by stream_accept(). Either
			 * we just have to ignore it (ret == 0) or it's a critical
			 * error due to a resource shortage, and we must stop the
			 * listener (ret < 0).
			 */
			if (ret == 0) /* successful termination */
				continue;

			goto transient_error;
		}

		/* increase the per-process number of cumulated sessions, this
		 * may only be done once l->accept() has accepted the connection.
		 */
		if (!(l->options & LI_O_UNLIMITED)) {
			count = update_freq_ctr(&global.sess_per_sec, 1);
			HA_ATOMIC_UPDATE_MAX(&global.sps_max, count);
		}
#ifdef USE_OPENSSL
		if (!(l->options & LI_O_UNLIMITED) && l->bind_conf && l->bind_conf->is_ssl) {
			count = update_freq_ctr(&global.ssl_per_sec, 1);
			HA_ATOMIC_UPDATE_MAX(&global.ssl_max, count);
		}
#endif

	} /* end of for (max_accept--) */

	/* we've exhausted max_accept, so there is no need to poll again */
 stop:
	fd_done_recv(fd);
	goto end;

 transient_error:
	/* pause the listener and try again in 100 ms */
	expire = tick_add(now_ms, 100);

 wait_expire:
	limit_listener(l, &global_listener_queue);
	task_schedule(global_listener_queue_task, tick_first(expire, global_listener_queue_task->expire));
 end:
	if (next_conn)
		HA_ATOMIC_SUB(&l->nbconn, 1);

	if (p && next_feconn)
		HA_ATOMIC_SUB(&p->feconn, 1);

	if (next_actconn)
		HA_ATOMIC_SUB(&actconn, 1);

	if ((l->state == LI_FULL && l->nbconn < l->maxconn) ||
	    (l->state == LI_LIMITED && ((!p || p->feconn < p->maxconn) && (actconn < global.maxconn)))) {
		/* at least one thread has to this when quitting */
		resume_listener(l);

		/* Dequeues all of the listeners waiting for a resource */
		if (!LIST_ISEMPTY(&global_listener_queue))
			dequeue_all_listeners(&global_listener_queue);

		if (p && !LIST_ISEMPTY(&p->listener_queue) &&
		    (!p->fe_sps_lim || freq_ctr_remain(&p->fe_sess_per_sec, p->fe_sps_lim, 0) > 0))
			dequeue_all_listeners(&p->listener_queue);
	}
}

/* Notify the listener that a connection initiated from it was released. This
 * is used to keep the connection count consistent and to possibly re-open
 * listening when it was limited.
 */
void listener_release(struct listener *l)
{
	struct proxy *fe = l->bind_conf->frontend;

	if (!(l->options & LI_O_UNLIMITED))
		HA_ATOMIC_SUB(&actconn, 1);
	if (fe)
		HA_ATOMIC_SUB(&fe->feconn, 1);
	HA_ATOMIC_SUB(&l->nbconn, 1);

	if (l->state == LI_FULL || l->state == LI_LIMITED)
		resume_listener(l);

	/* Dequeues all of the listeners waiting for a resource */
	if (!LIST_ISEMPTY(&global_listener_queue))
		dequeue_all_listeners(&global_listener_queue);

	if (!LIST_ISEMPTY(&fe->listener_queue) &&
	    (!fe->fe_sps_lim || freq_ctr_remain(&fe->fe_sess_per_sec, fe->fe_sps_lim, 0) > 0))
		dequeue_all_listeners(&fe->listener_queue);
}

/* resume listeners waiting in the local listener queue. They are still in LI_LIMITED state */
static struct task *listener_queue_process(struct task *t, void *context, unsigned short state)
{
	struct work_list *wl = context;
	struct listener *l;

	while ((l = LIST_POP_LOCKED(&wl->head, struct listener *, wait_queue))) {
		/* The listeners are still in the LI_LIMITED state */
		resume_listener(l);
	}
	return t;
}

/* Initializes the listener queues. Returns 0 on success, otherwise ERR_* flags */
static int listener_queue_init()
{
	local_listener_queue = work_list_create(global.nbthread, listener_queue_process, NULL);
	if (!local_listener_queue) {
		ha_alert("Out of memory while initializing listener queues.\n");
		return ERR_FATAL|ERR_ABORT;
	}
	return 0;
}

static void listener_queue_deinit()
{
	work_list_destroy(local_listener_queue, global.nbthread);
}

REGISTER_CONFIG_POSTPARSER("multi-threaded listener queue", listener_queue_init);
REGISTER_POST_DEINIT(listener_queue_deinit);

/*
 * Registers the bind keyword list <kwl> as a list of valid keywords for next
 * parsing sessions.
 */
void bind_register_keywords(struct bind_kw_list *kwl)
{
	LIST_ADDQ(&bind_keywords.list, &kwl->list);
}

/* Return a pointer to the bind keyword <kw>, or NULL if not found. If the
 * keyword is found with a NULL ->parse() function, then an attempt is made to
 * find one with a valid ->parse() function. This way it is possible to declare
 * platform-dependant, known keywords as NULL, then only declare them as valid
 * if some options are met. Note that if the requested keyword contains an
 * opening parenthesis, everything from this point is ignored.
 */
struct bind_kw *bind_find_kw(const char *kw)
{
	int index;
	const char *kwend;
	struct bind_kw_list *kwl;
	struct bind_kw *ret = NULL;

	kwend = strchr(kw, '(');
	if (!kwend)
		kwend = kw + strlen(kw);

	list_for_each_entry(kwl, &bind_keywords.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if ((strncmp(kwl->kw[index].kw, kw, kwend - kw) == 0) &&
			    kwl->kw[index].kw[kwend-kw] == 0) {
				if (kwl->kw[index].parse)
					return &kwl->kw[index]; /* found it !*/
				else
					ret = &kwl->kw[index];  /* may be OK */
			}
		}
	}
	return ret;
}

/* Dumps all registered "bind" keywords to the <out> string pointer. The
 * unsupported keywords are only dumped if their supported form was not
 * found.
 */
void bind_dump_kws(char **out)
{
	struct bind_kw_list *kwl;
	int index;

	*out = NULL;
	list_for_each_entry(kwl, &bind_keywords.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if (kwl->kw[index].parse ||
			    bind_find_kw(kwl->kw[index].kw) == &kwl->kw[index]) {
				memprintf(out, "%s[%4s] %s%s%s\n", *out ? *out : "",
				          kwl->scope,
				          kwl->kw[index].kw,
				          kwl->kw[index].skip ? " <arg>" : "",
				          kwl->kw[index].parse ? "" : " (not supported)");
			}
		}
	}
}

/************************************************************************/
/*      All supported sample and ACL keywords must be declared here.    */
/************************************************************************/

/* set temp integer to the number of connexions to the same listening socket */
static int
smp_fetch_dconn(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->sess->listener->nbconn;
	return 1;
}

/* set temp integer to the id of the socket (listener) */
static int
smp_fetch_so_id(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->sess->listener->luid;
	return 1;
}

/* parse the "accept-proxy" bind keyword */
static int bind_parse_accept_proxy(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->options |= LI_O_ACC_PROXY;

	return 0;
}

/* parse the "accept-netscaler-cip" bind keyword */
static int bind_parse_accept_netscaler_cip(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;
	uint32_t val;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	val = atol(args[cur_arg + 1]);
	if (val <= 0) {
		memprintf(err, "'%s' : invalid value %d, must be > 0", args[cur_arg], val);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind) {
		l->options |= LI_O_ACC_CIP;
		conf->ns_cip_magic = val;
	}

	return 0;
}

/* parse the "backlog" bind keyword */
static int bind_parse_backlog(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;
	int val;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	val = atol(args[cur_arg + 1]);
	if (val <= 0) {
		memprintf(err, "'%s' : invalid value %d, must be > 0", args[cur_arg], val);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->backlog = val;

	return 0;
}

/* parse the "id" bind keyword */
static int bind_parse_id(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct eb32_node *node;
	struct listener *l, *new;
	char *error;

	if (conf->listeners.n != conf->listeners.p) {
		memprintf(err, "'%s' can only be used with a single socket", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : expects an integer argument", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	new = LIST_NEXT(&conf->listeners, struct listener *, by_bind);
	new->luid = strtol(args[cur_arg + 1], &error, 10);
	if (*error != '\0') {
		memprintf(err, "'%s' : expects an integer argument, found '%s'", args[cur_arg], args[cur_arg + 1]);
		return ERR_ALERT | ERR_FATAL;
	}
	new->conf.id.key = new->luid;

	if (new->luid <= 0) {
		memprintf(err, "'%s' : custom id has to be > 0", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	node = eb32_lookup(&px->conf.used_listener_id, new->luid);
	if (node) {
		l = container_of(node, struct listener, conf.id);
		memprintf(err, "'%s' : custom id %d already used at %s:%d ('bind %s')",
		          args[cur_arg], l->luid, l->bind_conf->file, l->bind_conf->line,
		          l->bind_conf->arg);
		return ERR_ALERT | ERR_FATAL;
	}

	eb32_insert(&px->conf.used_listener_id, &new->conf.id);
	return 0;
}

/* parse the "maxconn" bind keyword */
static int bind_parse_maxconn(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;
	int val;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	val = atol(args[cur_arg + 1]);
	if (val <= 0) {
		memprintf(err, "'%s' : invalid value %d, must be > 0", args[cur_arg], val);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->maxconn = val;

	return 0;
}

/* parse the "name" bind keyword */
static int bind_parse_name(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing name", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->name = strdup(args[cur_arg + 1]);

	return 0;
}

/* parse the "nice" bind keyword */
static int bind_parse_nice(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct listener *l;
	int val;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	val = atol(args[cur_arg + 1]);
	if (val < -1024 || val > 1024) {
		memprintf(err, "'%s' : invalid value %d, allowed range is -1024..1024", args[cur_arg], val);
		return ERR_ALERT | ERR_FATAL;
	}

	list_for_each_entry(l, &conf->listeners, by_bind)
		l->nice = val;

	return 0;
}

/* parse the "process" bind keyword */
static int bind_parse_process(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	char *slash;
	unsigned long proc = 0, thread = 0;
	int i;

	if ((slash = strchr(args[cur_arg + 1], '/')) != NULL)
		*slash = 0;

	if (parse_process_number(args[cur_arg + 1], &proc, NULL, err)) {
		memprintf(err, "'%s' : %s", args[cur_arg], *err);
		return ERR_ALERT | ERR_FATAL;
	}

	if (slash) {
		if (parse_process_number(slash+1, &thread, NULL, err)) {
			memprintf(err, "'%s' : %s", args[cur_arg], *err);
			return ERR_ALERT | ERR_FATAL;
		}
		*slash = '/';
	}

	conf->bind_proc |= proc;
	if (thread) {
		for (i = 0; i < LONGBITS; i++)
			if (!proc || (proc & (1UL << i)))
				conf->bind_thread[i] |= thread;
	}
	return 0;
}

/* parse the "proto" bind keyword */
static int bind_parse_proto(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	struct ist proto;

	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing value", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	proto = ist2(args[cur_arg + 1], strlen(args[cur_arg + 1]));
	conf->mux_proto = get_mux_proto(proto);
	if (!conf->mux_proto) {
		memprintf(err, "'%s' :  unknown MUX protocol '%s'", args[cur_arg], args[cur_arg+1]);
		return ERR_ALERT | ERR_FATAL;
	}
	return 0;
}

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct sample_fetch_kw_list smp_kws = {ILH, {
	{ "dst_conn", smp_fetch_dconn, 0, NULL, SMP_T_SINT, SMP_USE_FTEND, },
	{ "so_id",    smp_fetch_so_id, 0, NULL, SMP_T_SINT, SMP_USE_FTEND, },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &smp_kws);

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted.
 */
static struct acl_kw_list acl_kws = {ILH, {
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, acl_register_keywords, &acl_kws);

/* Note: must not be declared <const> as its list will be overwritten.
 * Please take care of keeping this list alphabetically sorted, doing so helps
 * all code contributors.
 * Optional keywords are also declared with a NULL ->parse() function so that
 * the config parser can report an appropriate error when a known keyword was
 * not enabled.
 */
static struct bind_kw_list bind_kws = { "ALL", { }, {
	{ "accept-netscaler-cip", bind_parse_accept_netscaler_cip, 1 }, /* enable NetScaler Client IP insertion protocol */
	{ "accept-proxy", bind_parse_accept_proxy, 0 }, /* enable PROXY protocol */
	{ "backlog",      bind_parse_backlog,      1 }, /* set backlog of listening socket */
	{ "id",           bind_parse_id,           1 }, /* set id of listening socket */
	{ "maxconn",      bind_parse_maxconn,      1 }, /* set maxconn of listening socket */
	{ "name",         bind_parse_name,         1 }, /* set name of listening socket */
	{ "nice",         bind_parse_nice,         1 }, /* set nice of listening socket */
	{ "process",      bind_parse_process,      1 }, /* set list of allowed process for this socket */
	{ "proto",        bind_parse_proto,        1 }, /* set the proto to use for all incoming connections */
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, bind_register_keywords, &bind_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
