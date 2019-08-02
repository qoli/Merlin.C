/*
 * Functions dedicated to statistics output and the stats socket
 *
 * Copyright 2000-2012 Willy Tarreau <w@1wt.eu>
 * Copyright 2007-2009 Krzysztof Piotr Oledzki <ole@ans.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <net/if.h>

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/initcall.h>
#include <common/memory.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/uri_auth.h>
#include <common/version.h>
#include <common/base64.h>

#include <types/applet.h>
#include <types/cli.h>
#include <types/global.h>
#include <types/dns.h>
#include <types/stats.h>

#include <proto/activity.h>
#include <proto/backend.h>
#include <proto/channel.h>
#include <proto/checks.h>
#include <proto/compression.h>
#include <proto/stats.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/pattern.h>
#include <proto/pipe.h>
#include <proto/protocol.h>
#include <proto/listener.h>
#include <proto/map.h>
#include <proto/proxy.h>
#include <proto/sample.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/server.h>
#include <proto/ssl_sock.h>
#include <proto/stream_interface.h>
#include <proto/task.h>
#include <proto/proto_udp.h>

#define PAYLOAD_PATTERN "<<"

static struct applet cli_applet;

static const char stats_sock_usage_msg[] =
	"Unknown command. Please enter one of the following commands only :\n"
	"  help           : this message\n"
	"  prompt         : toggle interactive mode with prompt\n"
	"  quit           : disconnect\n"
	"";

static const char stats_permission_denied_msg[] =
	"Permission denied\n"
	"";


static THREAD_LOCAL char *dynamic_usage_msg = NULL;

/* List head of cli keywords */
static struct cli_kw_list cli_keywords = {
	.list = LIST_HEAD_INIT(cli_keywords.list)
};

extern const char *stat_status_codes[];

static struct proxy *mworker_proxy; /* CLI proxy of the master */

static char *cli_gen_usage_msg(struct appctx *appctx)
{
	struct cli_kw_list *kw_list;
	struct cli_kw *kw;
	struct buffer *tmp = get_trash_chunk();
	struct buffer out;

	free(dynamic_usage_msg);
	dynamic_usage_msg = NULL;

	if (LIST_ISEMPTY(&cli_keywords.list))
		goto end;

	chunk_reset(tmp);
	chunk_strcat(tmp, stats_sock_usage_msg);
	list_for_each_entry(kw_list, &cli_keywords.list, list) {
		kw = &kw_list->kw[0];
		while (kw->str_kw[0]) {

			/* in a worker or normal process, don't display master only commands */
			if (master == 0 && (kw->level & ACCESS_MASTER_ONLY))
				goto next_kw;

			/* in master don't displays if we don't have the master bits */
			if (master == 1 && !(kw->level & (ACCESS_MASTER_ONLY|ACCESS_MASTER)))
				goto next_kw;

			if (kw->usage)
				chunk_appendf(tmp, "  %s\n", kw->usage);

next_kw:

			kw++;
		}
	}
	chunk_init(&out, NULL, 0);
	chunk_dup(&out, tmp);
	dynamic_usage_msg = out.area;

end:
	if (dynamic_usage_msg) {
		appctx->ctx.cli.severity = LOG_INFO;
		appctx->ctx.cli.msg = dynamic_usage_msg;
	}
	else {
		appctx->ctx.cli.severity = LOG_INFO;
		appctx->ctx.cli.msg = stats_sock_usage_msg;
	}
	appctx->st0 = CLI_ST_PRINT;

	return dynamic_usage_msg;
}

struct cli_kw* cli_find_kw(char **args)
{
	struct cli_kw_list *kw_list;
	struct cli_kw *kw;/* current cli_kw */
	char **tmp_args;
	const char **tmp_str_kw;
	int found = 0;

	if (LIST_ISEMPTY(&cli_keywords.list))
		return NULL;

	list_for_each_entry(kw_list, &cli_keywords.list, list) {
		kw = &kw_list->kw[0];
		while (*kw->str_kw) {
			tmp_args = args;
			tmp_str_kw = kw->str_kw;
			while (*tmp_str_kw) {
				if (strcmp(*tmp_str_kw, *tmp_args) == 0) {
					found = 1;
				} else {
					found = 0;
					break;
				}
				tmp_args++;
				tmp_str_kw++;
			}
			if (found)
				return (kw);
			kw++;
		}
	}
	return NULL;
}

void cli_register_kw(struct cli_kw_list *kw_list)
{
	LIST_ADDQ(&cli_keywords.list, &kw_list->list);
}


/* allocate a new stats frontend named <name>, and return it
 * (or NULL in case of lack of memory).
 */
static struct proxy *alloc_stats_fe(const char *name, const char *file, int line)
{
	struct proxy *fe;

	fe = calloc(1, sizeof(*fe));
	if (!fe)
		return NULL;

	init_new_proxy(fe);
	fe->next = proxies_list;
	proxies_list = fe;
	fe->last_change = now.tv_sec;
	fe->id = strdup("GLOBAL");
	fe->cap = PR_CAP_FE;
	fe->maxconn = 10;                 /* default to 10 concurrent connections */
	fe->timeout.client = MS_TO_TICKS(10000); /* default timeout of 10 seconds */
	fe->conf.file = strdup(file);
	fe->conf.line = line;
	fe->accept = frontend_accept;
	fe->default_target = &cli_applet.obj_type;

	/* the stats frontend is the only one able to assign ID #0 */
	fe->conf.id.key = fe->uuid = 0;
	eb32_insert(&used_proxy_id, &fe->conf.id);
	return fe;
}

/* This function parses a "stats" statement in the "global" section. It returns
 * -1 if there is any error, otherwise zero. If it returns -1, it will write an
 * error message into the <err> buffer which will be preallocated. The trailing
 * '\n' must not be written. The function must be called with <args> pointing to
 * the first word after "stats".
 */
static int stats_parse_global(char **args, int section_type, struct proxy *curpx,
                              struct proxy *defpx, const char *file, int line,
                              char **err)
{
	struct bind_conf *bind_conf;
	struct listener *l;

	if (!strcmp(args[1], "socket")) {
		int cur_arg;

		if (*args[2] == 0) {
			memprintf(err, "'%s %s' in global section expects an address or a path to a UNIX socket", args[0], args[1]);
			return -1;
		}

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}

		bind_conf = bind_conf_alloc(global.stats_fe, file, line, args[2], xprt_get(XPRT_RAW));
		bind_conf->level &= ~ACCESS_LVL_MASK;
		bind_conf->level |= ACCESS_LVL_OPER; /* default access level */

		if (!str2listener(args[2], global.stats_fe, bind_conf, file, line, err)) {
			memprintf(err, "parsing [%s:%d] : '%s %s' : %s\n",
			          file, line, args[0], args[1], err && *err ? *err : "error");
			return -1;
		}

		cur_arg = 3;
		while (*args[cur_arg]) {
			static int bind_dumped;
			struct bind_kw *kw;

			kw = bind_find_kw(args[cur_arg]);
			if (kw) {
				if (!kw->parse) {
					memprintf(err, "'%s %s' : '%s' option is not implemented in this version (check build options).",
						  args[0], args[1], args[cur_arg]);
					return -1;
				}

				if (kw->parse(args, cur_arg, global.stats_fe, bind_conf, err) != 0) {
					if (err && *err)
						memprintf(err, "'%s %s' : '%s'", args[0], args[1], *err);
					else
						memprintf(err, "'%s %s' : error encountered while processing '%s'",
						          args[0], args[1], args[cur_arg]);
					return -1;
				}

				cur_arg += 1 + kw->skip;
				continue;
			}

			if (!bind_dumped) {
				bind_dump_kws(err);
				indent_msg(err, 4);
				bind_dumped = 1;
			}

			memprintf(err, "'%s %s' : unknown keyword '%s'.%s%s",
			          args[0], args[1], args[cur_arg],
			          err && *err ? " Registered keywords :" : "", err && *err ? *err : "");
			return -1;
		}

		list_for_each_entry(l, &bind_conf->listeners, by_bind) {
			l->accept = session_accept_fd;
			l->default_target = global.stats_fe->default_target;
			l->options |= LI_O_UNLIMITED; /* don't make the peers subject to global limits */
			l->nice = -64;  /* we want to boost priority for local stats */
			global.maxsock++; /* for the listening socket */
		}
	}
	else if (!strcmp(args[1], "timeout")) {
		unsigned timeout;
		const char *res = parse_time_err(args[2], &timeout, TIME_UNIT_MS);

		if (res == PARSE_TIME_OVER) {
			memprintf(err, "timer overflow in argument '%s' to '%s %s' (maximum value is 2147483647 ms or ~24.8 days)",
				 args[2], args[0], args[1]);
			return -1;
		}
		else if (res == PARSE_TIME_UNDER) {
			memprintf(err, "timer underflow in argument '%s' to '%s %s' (minimum non-null value is 1 ms)",
				 args[2], args[0], args[1]);
			return -1;
		}
		else if (res) {
			memprintf(err, "'%s %s' : unexpected character '%c'", args[0], args[1], *res);
			return -1;
		}

		if (!timeout) {
			memprintf(err, "'%s %s' expects a positive value", args[0], args[1]);
			return -1;
		}
		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}
		global.stats_fe->timeout.client = MS_TO_TICKS(timeout);
	}
	else if (!strcmp(args[1], "maxconn")) {
		int maxconn = atol(args[2]);

		if (maxconn <= 0) {
			memprintf(err, "'%s %s' expects a positive value", args[0], args[1]);
			return -1;
		}

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}
		global.stats_fe->maxconn = maxconn;
	}
	else if (!strcmp(args[1], "bind-process")) {  /* enable the socket only on some processes */
		int cur_arg = 2;
		unsigned long set = 0;

		if (!global.stats_fe) {
			if ((global.stats_fe = alloc_stats_fe("GLOBAL", file, line)) == NULL) {
				memprintf(err, "'%s %s' : out of memory trying to allocate a frontend", args[0], args[1]);
				return -1;
			}
		}

		while (*args[cur_arg]) {
			if (strcmp(args[cur_arg], "all") == 0) {
				set = 0;
				break;
			}
			if (parse_process_number(args[cur_arg], &set, MAX_PROCS, NULL, err)) {
				memprintf(err, "'%s %s' : %s", args[0], args[1], *err);
				return -1;
			}
			cur_arg++;
		}
		global.stats_fe->bind_proc = set;
	}
	else {
		memprintf(err, "'%s' only supports 'socket', 'maxconn', 'bind-process' and 'timeout' (got '%s')", args[0], args[1]);
		return -1;
	}
	return 0;
}

/*
 * This function exports the bound addresses of a <frontend> in the environment
 * variable <varname>. Those addresses are separated by semicolons and prefixed
 * with their type (abns@, unix@, sockpair@ etc)
 * Return -1 upon error, 0 otherwise
 */
int listeners_setenv(struct proxy *frontend, const char *varname)
{
	struct buffer *trash = get_trash_chunk();
	struct bind_conf *bind_conf;

	if (frontend) {
		list_for_each_entry(bind_conf, &frontend->conf.bind, by_fe) {
			struct listener *l;

			list_for_each_entry(l, &bind_conf->listeners, by_bind) {
				char addr[46];
				char port[6];

				/* separate listener by semicolons */
				if (trash->data)
					chunk_appendf(trash, ";");

				if (l->addr.ss_family == AF_UNIX) {
					const struct sockaddr_un *un;

					un = (struct sockaddr_un *)&l->addr;
					if (un->sun_path[0] == '\0') {
						chunk_appendf(trash, "abns@%s", un->sun_path+1);
					} else {
						chunk_appendf(trash, "unix@%s", un->sun_path);
					}
				} else if (l->addr.ss_family == AF_INET) {
					addr_to_str(&l->addr, addr, sizeof(addr));
					port_to_str(&l->addr, port, sizeof(port));
					chunk_appendf(trash, "ipv4@%s:%s", addr, port);
				} else if (l->addr.ss_family == AF_INET6) {
					addr_to_str(&l->addr, addr, sizeof(addr));
					port_to_str(&l->addr, port, sizeof(port));
					chunk_appendf(trash, "ipv6@[%s]:%s", addr, port);
				} else if (l->addr.ss_family == AF_CUST_SOCKPAIR) {
					chunk_appendf(trash, "sockpair@%d", ((struct sockaddr_in *)&l->addr)->sin_addr.s_addr);
				}
			}
		}
		trash->area[trash->data++] = '\0';
		if (setenv(varname, trash->area, 1) < 0)
			return -1;
	}

	return 0;
}

int cli_socket_setenv()
{
	if (listeners_setenv(global.stats_fe, "HAPROXY_CLI") < 0)
		return -1;
	if (listeners_setenv(mworker_proxy, "HAPROXY_MASTER_CLI") < 0)
		return -1;

	return 0;
}

REGISTER_CONFIG_POSTPARSER("cli", cli_socket_setenv);

/* Verifies that the CLI at least has a level at least as high as <level>
 * (typically ACCESS_LVL_ADMIN). Returns 1 if OK, otherwise 0. In case of
 * failure, an error message is prepared and the appctx's state is adjusted
 * to print it so that a return 1 is enough to abort any processing.
 */
int cli_has_level(struct appctx *appctx, int level)
{

	if ((appctx->cli_level & ACCESS_LVL_MASK) < level) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = stats_permission_denied_msg;
		appctx->st0 = CLI_ST_PRINT;
		return 0;
	}
	return 1;
}

/* same as cli_has_level but for the CLI proxy and without error message */
int pcli_has_level(struct stream *s, int level)
{
	if ((s->pcli_flags & ACCESS_LVL_MASK) < level) {
		return 0;
	}
	return 1;
}

/* Returns severity_output for the current session if set, or default for the socket */
static int cli_get_severity_output(struct appctx *appctx)
{
	if (appctx->cli_severity_output)
		return appctx->cli_severity_output;
	return strm_li(si_strm(appctx->owner))->bind_conf->severity_output;
}

/* Processes the CLI interpreter on the stats socket. This function is called
 * from the CLI's IO handler running in an appctx context. The function returns 1
 * if the request was understood, otherwise zero. It is called with appctx->st0
 * set to CLI_ST_GETREQ and presets ->st2 to 0 so that parsers don't have to do
 * it. It will possilbly leave st0 to CLI_ST_CALLBACK if the keyword needs to
 * have its own I/O handler called again. Most of the time, parsers will only
 * set st0 to CLI_ST_PRINT and put their message to be displayed into cli.msg.
 * If a keyword parser is NULL and an I/O handler is declared, the I/O handler
 * will automatically be used.
 */
static int cli_parse_request(struct appctx *appctx)
{
	char *args[MAX_STATS_ARGS + 1], *p, *end, *payload = NULL;
	int i = 0;
	struct cli_kw *kw;

	appctx->st2 = 0;
	memset(&appctx->ctx.cli, 0, sizeof(appctx->ctx.cli));

	p = appctx->chunk->area;
	end = p + appctx->chunk->data;

	/*
	 * Get the payload start if there is one.
	 * For the sake of simplicity, the payload pattern is looked up
	 * everywhere from the start of the input but it can only be found
	 * at the end of the first line if APPCTX_CLI_ST1_PAYLOAD is set.
	 *
	 * The input string was zero terminated so it is safe to use
	 * the str*() functions throughout the parsing
	 */
	if (appctx->st1 & APPCTX_CLI_ST1_PAYLOAD) {
		payload = strstr(p, PAYLOAD_PATTERN);
		end = payload;
		/* skip the pattern */
		payload += strlen(PAYLOAD_PATTERN);
	}

	/*
	 * Get pointers on words.
	 * One extra slot is reserved to store a pointer on a null byte.
	 */
	while (i < MAX_STATS_ARGS && p < end) {
		int j, k;

		/* skip leading spaces/tabs */
		p += strspn(p, " \t");
		if (!*p)
			break;

		args[i] = p;
		p += strcspn(p, " \t");
		*p++ = 0;

		/* unescape backslashes (\) */
		for (j = 0, k = 0; args[i][k]; k++) {
			if (args[i][k] == '\\') {
				if (args[i][k + 1] == '\\')
					k++;
				else
					continue;
			}
			args[i][j] = args[i][k];
			j++;
		}
		args[i][j] = 0;

		i++;
	}
	/* fill unused slots */
	p = appctx->chunk->area + appctx->chunk->data;
	for (; i < MAX_STATS_ARGS + 1; i++)
		args[i] = p;

	kw = cli_find_kw(args);
	if (!kw)
		return 0;

	/* in a worker or normal process, don't display master only commands */
	if (master == 0 && (kw->level & ACCESS_MASTER_ONLY))
		return 0;

	/* in master don't displays if we don't have the master bits */
	if (master == 1 && !(kw->level & (ACCESS_MASTER_ONLY|ACCESS_MASTER)))
		return 0;

	appctx->io_handler = kw->io_handler;
	appctx->io_release = kw->io_release;
	/* kw->parse could set its own io_handler or ip_release handler */
	if ((!kw->parse || kw->parse(args, payload, appctx, kw->private) == 0) && appctx->io_handler) {
		appctx->st0 = CLI_ST_CALLBACK;
	}
	return 1;
}

/* prepends then outputs the argument msg with a syslog-type severity depending on severity_output value */
static int cli_output_msg(struct channel *chn, const char *msg, int severity, int severity_output)
{
	struct buffer *tmp;

	if (likely(severity_output == CLI_SEVERITY_NONE))
		return ci_putblk(chn, msg, strlen(msg));

	tmp = get_trash_chunk();
	chunk_reset(tmp);

	if (severity < 0 || severity > 7) {
		ha_warning("socket command feedback with invalid severity %d", severity);
		chunk_printf(tmp, "[%d]: ", severity);
	}
	else {
		switch (severity_output) {
			case CLI_SEVERITY_NUMBER:
				chunk_printf(tmp, "[%d]: ", severity);
				break;
			case CLI_SEVERITY_STRING:
				chunk_printf(tmp, "[%s]: ", log_levels[severity]);
				break;
			default:
				ha_warning("Unrecognized severity output %d", severity_output);
		}
	}
	chunk_appendf(tmp, "%s", msg);

	return ci_putblk(chn, tmp->area, strlen(tmp->area));
}

/* This I/O handler runs as an applet embedded in a stream interface. It is
 * used to processes I/O from/to the stats unix socket. The system relies on a
 * state machine handling requests and various responses. We read a request,
 * then we process it and send the response, and we possibly display a prompt.
 * Then we can read again. The state is stored in appctx->st0 and is one of the
 * CLI_ST_* constants. appctx->st1 is used to indicate whether prompt is enabled
 * or not.
 */
static void cli_io_handler(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct channel *req = si_oc(si);
	struct channel *res = si_ic(si);
	struct bind_conf *bind_conf = strm_li(si_strm(si))->bind_conf;
	int reql;
	int len;

	if (unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO))
		goto out;

	/* Check if the input buffer is available. */
	if (res->buf.size == 0) {
		/* buf.size==0 means we failed to get a buffer and were
		 * already subscribed to a wait list to get a buffer.
		 */
		goto out;
	}

	while (1) {
		if (appctx->st0 == CLI_ST_INIT) {
			/* Stats output not initialized yet */
			memset(&appctx->ctx.stats, 0, sizeof(appctx->ctx.stats));
			/* reset severity to default at init */
			appctx->cli_severity_output = bind_conf->severity_output;
			appctx->st0 = CLI_ST_GETREQ;
			appctx->cli_level = bind_conf->level;
		}
		else if (appctx->st0 == CLI_ST_END) {
			/* Let's close for real now. We just close the request
			 * side, the conditions below will complete if needed.
			 */
			si_shutw(si);
			free_trash_chunk(appctx->chunk);
			break;
		}
		else if (appctx->st0 == CLI_ST_GETREQ) {
			char *str;

			/* use a trash chunk to store received data */
			if (!appctx->chunk) {
				appctx->chunk = alloc_trash_chunk();
				if (!appctx->chunk) {
					appctx->st0 = CLI_ST_END;
					continue;
				}
			}

			str = appctx->chunk->area + appctx->chunk->data;

			/* ensure we have some output room left in the event we
			 * would want to return some info right after parsing.
			 */
			if (buffer_almost_full(si_ib(si))) {
				si_rx_room_blk(si);
				break;
			}

			/* '- 1' is to ensure a null byte can always be inserted at the end */
			reql = co_getline(si_oc(si), str,
					  appctx->chunk->size - appctx->chunk->data - 1);
			if (reql <= 0) { /* closed or EOL not found */
				if (reql == 0)
					break;
				appctx->st0 = CLI_ST_END;
				continue;
			}

			if (!(appctx->st1 & APPCTX_CLI_ST1_PAYLOAD)) {
				/* seek for a possible unescaped semi-colon. If we find
				 * one, we replace it with an LF and skip only this part.
				 */
				for (len = 0; len < reql; len++) {
					if (str[len] == '\\') {
						len++;
						continue;
					}
					if (str[len] == ';') {
						str[len] = '\n';
						reql = len + 1;
						break;
					}
				}
			}

			/* now it is time to check that we have a full line,
			 * remove the trailing \n and possibly \r, then cut the
			 * line.
			 */
			len = reql - 1;
			if (str[len] != '\n') {
				appctx->st0 = CLI_ST_END;
				continue;
			}

			if (len && str[len-1] == '\r')
				len--;

			str[len] = '\0';
			appctx->chunk->data += len;

			if (appctx->st1 & APPCTX_CLI_ST1_PAYLOAD) {
				appctx->chunk->area[appctx->chunk->data] = '\n';
				appctx->chunk->area[appctx->chunk->data + 1] = 0;
				appctx->chunk->data++;
			}

			appctx->st0 = CLI_ST_PROMPT;

			if (appctx->st1 & APPCTX_CLI_ST1_PAYLOAD) {
				/* empty line */
				if (!len) {
					/* remove the last two \n */
					appctx->chunk->data -= 2;
					appctx->chunk->area[appctx->chunk->data] = 0;

					if (!cli_parse_request(appctx))
						cli_gen_usage_msg(appctx);

					chunk_reset(appctx->chunk);
					/* NB: cli_sock_parse_request() may have put
					 * another CLI_ST_O_* into appctx->st0.
					 */

					appctx->st1 &= ~APPCTX_CLI_ST1_PAYLOAD;
				}
			}
			else {
				/*
				 * Look for the "payload start" pattern at the end of a line
				 * Its location is not remembered here, this is just to switch
				 * to a gathering mode.
				 */
				if (!strcmp(appctx->chunk->area + appctx->chunk->data - strlen(PAYLOAD_PATTERN), PAYLOAD_PATTERN))
					appctx->st1 |= APPCTX_CLI_ST1_PAYLOAD;
				else {
					/* no payload, the command is complete: parse the request */
					if (!cli_parse_request(appctx))
						cli_gen_usage_msg(appctx);

					chunk_reset(appctx->chunk);
				}
			}

			/* re-adjust req buffer */
			co_skip(si_oc(si), reql);
			req->flags |= CF_READ_DONTWAIT; /* we plan to read small requests */
		}
		else {	/* output functions */
			switch (appctx->st0) {
			case CLI_ST_PROMPT:
				break;
			case CLI_ST_PRINT:
				if (cli_output_msg(res, appctx->ctx.cli.msg, appctx->ctx.cli.severity,
							cli_get_severity_output(appctx)) != -1)
					appctx->st0 = CLI_ST_PROMPT;
				else
					si_rx_room_blk(si);
				break;
			case CLI_ST_PRINT_FREE: {
				const char *msg = appctx->ctx.cli.err;

				if (!msg)
					msg = "Out of memory.\n";

				if (cli_output_msg(res, msg, LOG_ERR, cli_get_severity_output(appctx)) != -1) {
					free(appctx->ctx.cli.err);
					appctx->st0 = CLI_ST_PROMPT;
				}
				else
					si_rx_room_blk(si);
				break;
			}
			case CLI_ST_CALLBACK: /* use custom pointer */
				if (appctx->io_handler)
					if (appctx->io_handler(appctx)) {
						appctx->st0 = CLI_ST_PROMPT;
						if (appctx->io_release) {
							appctx->io_release(appctx);
							appctx->io_release = NULL;
						}
					}
				break;
			default: /* abnormal state */
				si->flags |= SI_FL_ERR;
				break;
			}

			/* The post-command prompt is either LF alone or LF + '> ' in interactive mode */
			if (appctx->st0 == CLI_ST_PROMPT) {
				const char *prompt = "";

				if (appctx->st1 & APPCTX_CLI_ST1_PROMPT) {
					/*
					 * when entering a payload with interactive mode, change the prompt
					 * to emphasize that more data can still be sent
					 */
					if (appctx->chunk->data && appctx->st1 & APPCTX_CLI_ST1_PAYLOAD)
						prompt = "+ ";
					else
						prompt = "\n> ";
				}
				else {
					if (!(appctx->st1 & (APPCTX_CLI_ST1_PAYLOAD|APPCTX_CLI_ST1_NOLF)))
						prompt = "\n";
				}

				if (ci_putstr(si_ic(si), prompt) != -1)
					appctx->st0 = CLI_ST_GETREQ;
				else
					si_rx_room_blk(si);
			}

			/* If the output functions are still there, it means they require more room. */
			if (appctx->st0 >= CLI_ST_OUTPUT)
				break;

			/* Now we close the output if one of the writers did so,
			 * or if we're not in interactive mode and the request
			 * buffer is empty. This still allows pipelined requests
			 * to be sent in non-interactive mode.
			 */
			if (((res->flags & (CF_SHUTW|CF_SHUTW_NOW))) ||
			   (!(appctx->st1 & APPCTX_CLI_ST1_PROMPT) && !co_data(req) && (!(appctx->st1 & APPCTX_CLI_ST1_PAYLOAD)))) {
				appctx->st0 = CLI_ST_END;
				continue;
			}

			/* switch state back to GETREQ to read next requests */
			appctx->st0 = CLI_ST_GETREQ;
			/* reactivate the \n at the end of the response for the next command */
			appctx->st1 &= ~APPCTX_CLI_ST1_NOLF;
		}
	}

	if ((res->flags & CF_SHUTR) && (si->state == SI_ST_EST)) {
		DPRINTF(stderr, "%s@%d: si to buf closed. req=%08x, res=%08x, st=%d\n",
			__FUNCTION__, __LINE__, req->flags, res->flags, si->state);
		/* Other side has closed, let's abort if we have no more processing to do
		 * and nothing more to consume. This is comparable to a broken pipe, so
		 * we forward the close to the request side so that it flows upstream to
		 * the client.
		 */
		si_shutw(si);
	}

	if ((req->flags & CF_SHUTW) && (si->state == SI_ST_EST) && (appctx->st0 < CLI_ST_OUTPUT)) {
		DPRINTF(stderr, "%s@%d: buf to si closed. req=%08x, res=%08x, st=%d\n",
			__FUNCTION__, __LINE__, req->flags, res->flags, si->state);
		/* We have no more processing to do, and nothing more to send, and
		 * the client side has closed. So we'll forward this state downstream
		 * on the response buffer.
		 */
		si_shutr(si);
		res->flags |= CF_READ_NULL;
	}

 out:
	DPRINTF(stderr, "%s@%d: st=%d, rqf=%x, rpf=%x, rqh=%lu, rqs=%lu, rh=%lu, rs=%lu\n",
		__FUNCTION__, __LINE__,
		si->state, req->flags, res->flags, ci_data(req), co_data(req), ci_data(res), co_data(res));
}

/* This is called when the stream interface is closed. For instance, upon an
 * external abort, we won't call the i/o handler anymore so we may need to
 * remove back references to the stream currently being dumped.
 */
static void cli_release_handler(struct appctx *appctx)
{
	if (appctx->io_release) {
		appctx->io_release(appctx);
		appctx->io_release = NULL;
	}
	else if (appctx->st0 == CLI_ST_PRINT_FREE) {
		free(appctx->ctx.cli.err);
		appctx->ctx.cli.err = NULL;
	}
}

/* This function dumps all environmnent variables to the buffer. It returns 0
 * if the output buffer is full and it needs to be called again, otherwise
 * non-zero. Dumps only one entry if st2 == STAT_ST_END. It uses cli.p0 as the
 * pointer to the current variable.
 */
static int cli_io_handler_show_env(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	char **var = appctx->ctx.cli.p0;

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW)))
		return 1;

	chunk_reset(&trash);

	/* we have two inner loops here, one for the proxy, the other one for
	 * the buffer.
	 */
	while (*var) {
		chunk_printf(&trash, "%s\n", *var);

		if (ci_putchk(si_ic(si), &trash) == -1) {
			si_rx_room_blk(si);
			return 0;
		}
		if (appctx->st2 == STAT_ST_END)
			break;
		var++;
		appctx->ctx.cli.p0 = var;
	}

	/* dump complete */
	return 1;
}

/* This function dumps all file descriptors states (or the requested one) to
 * the buffer. It returns 0 if the output buffer is full and it needs to be
 * called again, otherwise non-zero. Dumps only one entry if st2 == STAT_ST_END.
 * It uses cli.i0 as the fd number to restart from.
 */
static int cli_io_handler_show_fd(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	int fd = appctx->ctx.cli.i0;
	int ret = 1;

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW)))
		goto end;

	chunk_reset(&trash);

	/* isolate the threads once per round. We're limited to a buffer worth
	 * of output anyway, it cannot last very long.
	 */
	thread_isolate();

	/* we have two inner loops here, one for the proxy, the other one for
	 * the buffer.
	 */
	while (fd >= 0 && fd < global.maxsock) {
		struct fdtab fdt;
		struct listener *li = NULL;
		struct server *sv = NULL;
		struct proxy *px = NULL;
		const struct mux_ops *mux = NULL;
		void *ctx = NULL;
		uint32_t conn_flags = 0;
		int is_back = 0;

		fdt = fdtab[fd];

		if (!fdt.owner)
			goto skip; // closed

		if (fdt.iocb == conn_fd_handler) {
			conn_flags = ((struct connection *)fdt.owner)->flags;
			mux = ((struct connection *)fdt.owner)->mux;
			ctx = ((struct connection *)fdt.owner)->ctx;
			li = objt_listener(((struct connection *)fdt.owner)->target);
			sv = objt_server(((struct connection *)fdt.owner)->target);
			px = objt_proxy(((struct connection *)fdt.owner)->target);
			is_back = conn_is_back((struct connection *)fdt.owner);
		}
		else if (fdt.iocb == listener_accept)
			li = fdt.owner;

		chunk_printf(&trash,
			     "  %5d : st=0x%02x(R:%c%c%c W:%c%c%c) ev=0x%02x(%c%c%c%c%c) [%c%c] cnext=%d cprev=%d tmask=0x%lx umask=0x%lx owner=%p iocb=%p(%s)",
			     fd,
			     fdt.state,
			     (fdt.state & FD_EV_POLLED_R) ? 'P' : 'p',
			     (fdt.state & FD_EV_READY_R)  ? 'R' : 'r',
			     (fdt.state & FD_EV_ACTIVE_R) ? 'A' : 'a',
			     (fdt.state & FD_EV_POLLED_W) ? 'P' : 'p',
			     (fdt.state & FD_EV_READY_W)  ? 'R' : 'r',
			     (fdt.state & FD_EV_ACTIVE_W) ? 'A' : 'a',
			     fdt.ev,
			     (fdt.ev & FD_POLL_HUP) ? 'H' : 'h',
			     (fdt.ev & FD_POLL_ERR) ? 'E' : 'e',
			     (fdt.ev & FD_POLL_OUT) ? 'O' : 'o',
			     (fdt.ev & FD_POLL_PRI) ? 'P' : 'p',
			     (fdt.ev & FD_POLL_IN)  ? 'I' : 'i',
			     fdt.linger_risk ? 'L' : 'l',
			     fdt.cloned ? 'C' : 'c',
			     fdt.cache.next,
			     fdt.cache.prev,
			     fdt.thread_mask, fdt.update_mask,
			     fdt.owner,
			     fdt.iocb,
			     (fdt.iocb == conn_fd_handler)  ? "conn_fd_handler" :
			     (fdt.iocb == dgram_fd_handler) ? "dgram_fd_handler" :
			     (fdt.iocb == listener_accept)  ? "listener_accept" :
			     (fdt.iocb == poller_pipe_io_handler) ? "poller_pipe_io_handler" :
			     (fdt.iocb == mworker_accept_wrapper) ? "mworker_accept_wrapper" :
#ifdef USE_OPENSSL
#if (HA_OPENSSL_VERSION_NUMBER >= 0x1010000fL) && !defined(OPENSSL_NO_ASYNC)
			     (fdt.iocb == ssl_async_fd_free) ? "ssl_async_fd_free" :
			     (fdt.iocb == ssl_async_fd_handler) ? "ssl_async_fd_handler" :
#endif
#endif
			     "unknown");

		if (fdt.iocb == conn_fd_handler) {
			chunk_appendf(&trash, " back=%d cflg=0x%08x", is_back, conn_flags);
			if (px)
				chunk_appendf(&trash, " px=%s", px->id);
			else if (sv)
				chunk_appendf(&trash, " sv=%s/%s", sv->id, sv->proxy->id);
			else if (li)
				chunk_appendf(&trash, " fe=%s", li->bind_conf->frontend->id);

			if (mux) {
				chunk_appendf(&trash, " mux=%s ctx=%p", mux->name, ctx);
				if (mux->show_fd)
					mux->show_fd(&trash, fdt.owner);
			}
			else
				chunk_appendf(&trash, " nomux");
		}
		else if (fdt.iocb == listener_accept) {
			chunk_appendf(&trash, " l.st=%s fe=%s",
			              listener_state_str(li),
			              li->bind_conf->frontend->id);
		}

		chunk_appendf(&trash, "\n");

		if (ci_putchk(si_ic(si), &trash) == -1) {
			si_rx_room_blk(si);
			appctx->ctx.cli.i0 = fd;
			ret = 0;
			break;
		}
	skip:
		if (appctx->st2 == STAT_ST_END)
			break;

		fd++;
	}

 end:
	/* dump complete */

	thread_release();
	return ret;
}

/* This function dumps some activity counters used by developers and support to
 * rule out some hypothesis during bug reports. It returns 0 if the output
 * buffer is full and it needs to be called again, otherwise non-zero. It dumps
 * everything at once in the buffer and is not designed to do it in multiple
 * passes.
 */
static int cli_io_handler_show_activity(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	int thr;

	if (unlikely(si_ic(si)->flags & (CF_WRITE_ERROR|CF_SHUTW)))
		return 1;

	chunk_reset(&trash);

#undef SHOW_TOT
#define SHOW_TOT(t, x)							\
	do {								\
		unsigned int _v[MAX_THREADS];				\
		unsigned int _tot;					\
		const unsigned int _nbt = global.nbthread;		\
		for (_tot = t = 0; t < _nbt; t++)			\
			_tot += _v[t] = (x);				\
		if (_nbt == 1) {					\
			chunk_appendf(&trash, " %u\n", _tot);		\
			break;						\
		}							\
		chunk_appendf(&trash, " %u [", _tot);			\
		for (t = 0; t < _nbt; t++)				\
			chunk_appendf(&trash, " %u", _v[t]);		\
		chunk_appendf(&trash, " ]\n");				\
	} while (0)

#undef SHOW_AVG
#define SHOW_AVG(t, x)							\
	do {								\
		unsigned int _v[MAX_THREADS];				\
		unsigned int _tot;					\
		const unsigned int _nbt = global.nbthread;		\
		for (_tot = t = 0; t < _nbt; t++)			\
			_tot += _v[t] = (x);				\
		if (_nbt == 1) {					\
			chunk_appendf(&trash, " %u\n", _tot);		\
			break;						\
		}							\
		chunk_appendf(&trash, " %u [", (_tot + _nbt/2) / _nbt); \
		for (t = 0; t < _nbt; t++)				\
			chunk_appendf(&trash, " %u", _v[t]);		\
		chunk_appendf(&trash, " ]\n");				\
	} while (0)

	chunk_appendf(&trash, "thread_id: %u (%u..%u)\n", tid + 1, 1, global.nbthread);
	chunk_appendf(&trash, "date_now: %lu.%06lu\n", (long)now.tv_sec, (long)now.tv_usec);
	chunk_appendf(&trash, "loops:");        SHOW_TOT(thr, activity[thr].loops);
	chunk_appendf(&trash, "wake_cache:");   SHOW_TOT(thr, activity[thr].wake_cache);
	chunk_appendf(&trash, "wake_tasks:");   SHOW_TOT(thr, activity[thr].wake_tasks);
	chunk_appendf(&trash, "wake_signal:");  SHOW_TOT(thr, activity[thr].wake_signal);
	chunk_appendf(&trash, "poll_exp:");     SHOW_TOT(thr, activity[thr].poll_exp);
	chunk_appendf(&trash, "poll_drop:");    SHOW_TOT(thr, activity[thr].poll_drop);
	chunk_appendf(&trash, "poll_dead:");    SHOW_TOT(thr, activity[thr].poll_dead);
	chunk_appendf(&trash, "poll_skip:");    SHOW_TOT(thr, activity[thr].poll_skip);
	chunk_appendf(&trash, "fd_lock:");      SHOW_TOT(thr, activity[thr].fd_lock);
	chunk_appendf(&trash, "conn_dead:");    SHOW_TOT(thr, activity[thr].conn_dead);
	chunk_appendf(&trash, "stream:");       SHOW_TOT(thr, activity[thr].stream);
	chunk_appendf(&trash, "pool_fail:");    SHOW_TOT(thr, activity[thr].pool_fail);
	chunk_appendf(&trash, "buf_wait:");     SHOW_TOT(thr, activity[thr].buf_wait);
	chunk_appendf(&trash, "empty_rq:");     SHOW_TOT(thr, activity[thr].empty_rq);
	chunk_appendf(&trash, "long_rq:");      SHOW_TOT(thr, activity[thr].long_rq);
	chunk_appendf(&trash, "ctxsw:");        SHOW_TOT(thr, activity[thr].ctxsw);
	chunk_appendf(&trash, "tasksw:");       SHOW_TOT(thr, activity[thr].tasksw);
	chunk_appendf(&trash, "cpust_ms_tot:"); SHOW_TOT(thr, activity[thr].cpust_total / 2);
	chunk_appendf(&trash, "cpust_ms_1s:");  SHOW_TOT(thr, read_freq_ctr(&activity[thr].cpust_1s) / 2);
	chunk_appendf(&trash, "cpust_ms_15s:"); SHOW_TOT(thr, read_freq_ctr_period(&activity[thr].cpust_15s, 15000) / 2);
	chunk_appendf(&trash, "avg_loop_us:");  SHOW_AVG(thr, swrate_avg(activity[thr].avg_loop_us, TIME_STATS_SAMPLES));
	chunk_appendf(&trash, "accepted:");     SHOW_TOT(thr, activity[thr].accepted);
	chunk_appendf(&trash, "accq_pushed:");  SHOW_TOT(thr, activity[thr].accq_pushed);
	chunk_appendf(&trash, "accq_full:");    SHOW_TOT(thr, activity[thr].accq_full);
#ifdef USE_THREAD
	chunk_appendf(&trash, "accq_ring:");    SHOW_TOT(thr, (accept_queue_rings[thr].tail - accept_queue_rings[thr].head + ACCEPT_QUEUE_SIZE) % ACCEPT_QUEUE_SIZE);
#endif

#if defined(DEBUG_DEV)
	/* keep these ones at the end */
	chunk_appendf(&trash, "ctr0:");         SHOW_TOT(thr, activity[thr].ctr0);
	chunk_appendf(&trash, "ctr1:");         SHOW_TOT(thr, activity[thr].ctr1);
	chunk_appendf(&trash, "ctr2:");         SHOW_TOT(thr, activity[thr].ctr2);
#endif

	if (ci_putchk(si_ic(si), &trash) == -1) {
		chunk_reset(&trash);
		chunk_printf(&trash, "[output too large, cannot dump]\n");
		si_rx_room_blk(si);
	}

#undef SHOW_AVG
#undef SHOW_TOT
	/* dump complete */
	return 1;
}

/*
 * CLI IO handler for `show cli sockets`.
 * Uses ctx.cli.p0 to store the restart pointer.
 */
static int cli_io_handler_show_cli_sock(struct appctx *appctx)
{
	struct bind_conf *bind_conf;
	struct stream_interface *si = appctx->owner;

	chunk_reset(&trash);

	switch (appctx->st2) {
		case STAT_ST_INIT:
			chunk_printf(&trash, "# socket lvl processes\n");
			if (ci_putchk(si_ic(si), &trash) == -1) {
				si_rx_room_blk(si);
				return 0;
			}
			appctx->st2 = STAT_ST_LIST;

		case STAT_ST_LIST:
			if (global.stats_fe) {
				list_for_each_entry(bind_conf, &global.stats_fe->conf.bind, by_fe) {
					struct listener *l;

					/*
					 * get the latest dumped node in appctx->ctx.cli.p0
					 * if the current node is the first of the list
					 */

					if (appctx->ctx.cli.p0  &&
					    &bind_conf->by_fe == (&global.stats_fe->conf.bind)->n) {
						/* change the current node to the latest dumped and continue the loop */
						bind_conf = LIST_ELEM(appctx->ctx.cli.p0, typeof(bind_conf), by_fe);
						continue;
					}

					list_for_each_entry(l, &bind_conf->listeners, by_bind) {

						char addr[46];
						char port[6];

						if (l->addr.ss_family == AF_UNIX) {
							const struct sockaddr_un *un;

							un = (struct sockaddr_un *)&l->addr;
							if (un->sun_path[0] == '\0') {
								chunk_appendf(&trash, "abns@%s ", un->sun_path+1);
							} else {
								chunk_appendf(&trash, "unix@%s ", un->sun_path);
							}
						} else if (l->addr.ss_family == AF_INET) {
							addr_to_str(&l->addr, addr, sizeof(addr));
							port_to_str(&l->addr, port, sizeof(port));
							chunk_appendf(&trash, "ipv4@%s:%s ", addr, port);
						} else if (l->addr.ss_family == AF_INET6) {
							addr_to_str(&l->addr, addr, sizeof(addr));
							port_to_str(&l->addr, port, sizeof(port));
							chunk_appendf(&trash, "ipv6@[%s]:%s ", addr, port);
						} else if (l->addr.ss_family == AF_CUST_SOCKPAIR) {
							chunk_appendf(&trash, "sockpair@%d ", ((struct sockaddr_in *)&l->addr)->sin_addr.s_addr);
						} else
							chunk_appendf(&trash, "unknown ");

						if ((bind_conf->level & ACCESS_LVL_MASK) == ACCESS_LVL_ADMIN)
							chunk_appendf(&trash, "admin ");
						else if ((bind_conf->level & ACCESS_LVL_MASK) == ACCESS_LVL_OPER)
							chunk_appendf(&trash, "operator ");
						else if ((bind_conf->level & ACCESS_LVL_MASK) == ACCESS_LVL_USER)
							chunk_appendf(&trash, "user ");
						else
							chunk_appendf(&trash, "  ");

						if (bind_conf->bind_proc != 0) {
							int pos;
							for (pos = 0; pos < 8 * sizeof(bind_conf->bind_proc); pos++) {
								if (bind_conf->bind_proc & (1UL << pos)) {
									chunk_appendf(&trash, "%d,", pos+1);
								}
							}
							/* replace the latest comma by a newline */
							trash.area[trash.data-1] = '\n';

						} else {
							chunk_appendf(&trash, "all\n");
						}

						if (ci_putchk(si_ic(si), &trash) == -1) {
							si_rx_room_blk(si);
							return 0;
						}
					}
					appctx->ctx.cli.p0 = &bind_conf->by_fe; /* store the latest list node dumped */
				}
			}
		default:
			appctx->st2 = STAT_ST_FIN;
			return 1;
	}
}


/* parse a "show env" CLI request. Returns 0 if it needs to continue, 1 if it
 * wants to stop here. It puts the variable to be dumped into cli.p0 if a single
 * variable is requested otherwise puts environ there.
 */
static int cli_parse_show_env(char **args, char *payload, struct appctx *appctx, void *private)
{
	extern char **environ;
	char **var;

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	var = environ;

	if (*args[2]) {
		int len = strlen(args[2]);

		for (; *var; var++) {
			if (strncmp(*var, args[2], len) == 0 &&
			    (*var)[len] == '=')
				break;
		}
		if (!*var) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "Variable not found\n";
			appctx->st0 = CLI_ST_PRINT;
			return 1;
		}
		appctx->st2 = STAT_ST_END;
	}
	appctx->ctx.cli.p0 = var;
	return 0;
}

/* parse a "show fd" CLI request. Returns 0 if it needs to continue, 1 if it
 * wants to stop here. It puts the FD number into cli.i0 if a specific FD is
 * requested and sets st2 to STAT_ST_END, otherwise leaves 0 in i0.
 */
static int cli_parse_show_fd(char **args, char *payload, struct appctx *appctx, void *private)
{
	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	appctx->ctx.cli.i0 = 0;

	if (*args[2]) {
		appctx->ctx.cli.i0 = atoi(args[2]);
		appctx->st2 = STAT_ST_END;
	}
	return 0;
}

/* parse a "set timeout" CLI request. It always returns 1. */
static int cli_parse_set_timeout(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);

	if (strcmp(args[2], "cli") == 0) {
		unsigned timeout;
		const char *res;

		if (!*args[3]) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "Expects an integer value.\n";
			appctx->st0 = CLI_ST_PRINT;
			return 1;
		}

		res = parse_time_err(args[3], &timeout, TIME_UNIT_S);
		if (res || timeout < 1) {
			appctx->ctx.cli.severity = LOG_ERR;
			appctx->ctx.cli.msg = "Invalid timeout value.\n";
			appctx->st0 = CLI_ST_PRINT;
			return 1;
		}

		s->req.rto = s->res.wto = 1 + MS_TO_TICKS(timeout*1000);
		task_wakeup(s->task, TASK_WOKEN_MSG); // recompute timeouts
		return 1;
	}
	else {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "'set timeout' only supports 'cli'.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}
}

/* parse a "set maxconn global" command. It always returns 1. */
static int cli_parse_set_maxconn_global(char **args, char *payload, struct appctx *appctx, void *private)
{
	int v;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	if (!*args[3]) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Expects an integer value.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	v = atoi(args[3]);
	if (v > global.hardmaxconn) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Value out of range.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	/* check for unlimited values */
	if (v <= 0)
		v = global.hardmaxconn;

	global.maxconn = v;

	/* Dequeues all of the listeners waiting for a resource */
	if (!LIST_ISEMPTY(&global_listener_queue))
		dequeue_all_listeners(&global_listener_queue);

	return 1;
}

static int set_severity_output(int *target, char *argument)
{
	if (!strcmp(argument, "none")) {
		*target = CLI_SEVERITY_NONE;
		return 1;
	}
	else if (!strcmp(argument, "number")) {
		*target = CLI_SEVERITY_NUMBER;
		return 1;
	}
	else if (!strcmp(argument, "string")) {
		*target = CLI_SEVERITY_STRING;
		return 1;
	}
	return 0;
}

/* parse a "set severity-output" command. */
static int cli_parse_set_severity_output(char **args, char *payload, struct appctx *appctx, void *private)
{
	if (*args[2] && set_severity_output(&appctx->cli_severity_output, args[2]))
		return 0;

	appctx->ctx.cli.severity = LOG_ERR;
	appctx->ctx.cli.msg = "one of 'none', 'number', 'string' is a required argument\n";
	appctx->st0 = CLI_ST_PRINT;
	return 1;
}


/* show the level of the current CLI session */
static int cli_parse_show_lvl(char **args, char *payload, struct appctx *appctx, void *private)
{

	appctx->ctx.cli.severity = LOG_INFO;
	if ((appctx->cli_level & ACCESS_LVL_MASK) == ACCESS_LVL_ADMIN)
		appctx->ctx.cli.msg = "admin\n";
	else if ((appctx->cli_level & ACCESS_LVL_MASK) == ACCESS_LVL_OPER)
		appctx->ctx.cli.msg = "operator\n";
	else if ((appctx->cli_level & ACCESS_LVL_MASK) == ACCESS_LVL_USER)
		appctx->ctx.cli.msg = "user\n";
	else
		appctx->ctx.cli.msg = "unknown\n";

	appctx->st0 = CLI_ST_PRINT;
	return 1;

}

/* parse and set the CLI level dynamically */
static int cli_parse_set_lvl(char **args, char *payload, struct appctx *appctx, void *private)
{
	/* this will ask the applet to not output a \n after the command */
	if (!strcmp(args[1], "-"))
	    appctx->st1 |= APPCTX_CLI_ST1_NOLF;

	if (!strcmp(args[0], "operator")) {
		if (!cli_has_level(appctx, ACCESS_LVL_OPER)) {
			return 1;
		}
		appctx->cli_level &= ~ACCESS_LVL_MASK;
		appctx->cli_level |= ACCESS_LVL_OPER;

	} else if (!strcmp(args[0], "user")) {
		if (!cli_has_level(appctx, ACCESS_LVL_USER)) {
			return 1;
		}
		appctx->cli_level &= ~ACCESS_LVL_MASK;
		appctx->cli_level |= ACCESS_LVL_USER;
	}
	return 1;
}


int cli_parse_default(char **args, char *payload, struct appctx *appctx, void *private)
{
	return 0;
}

/* parse a "set rate-limit" command. It always returns 1. */
static int cli_parse_set_ratelimit(char **args, char *payload, struct appctx *appctx, void *private)
{
	int v;
	int *res;
	int mul = 1;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	if (strcmp(args[2], "connections") == 0 && strcmp(args[3], "global") == 0)
		res = &global.cps_lim;
	else if (strcmp(args[2], "sessions") == 0 && strcmp(args[3], "global") == 0)
		res = &global.sps_lim;
#ifdef USE_OPENSSL
	else if (strcmp(args[2], "ssl-sessions") == 0 && strcmp(args[3], "global") == 0)
		res = &global.ssl_lim;
#endif
	else if (strcmp(args[2], "http-compression") == 0 && strcmp(args[3], "global") == 0) {
		res = &global.comp_rate_lim;
		mul = 1024;
	}
	else {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg =
			"'set rate-limit' only supports :\n"
			"   - 'connections global' to set the per-process maximum connection rate\n"
			"   - 'sessions global' to set the per-process maximum session rate\n"
#ifdef USE_OPENSSL
			"   - 'ssl-sessions global' to set the per-process maximum SSL session rate\n"
#endif
			"   - 'http-compression global' to set the per-process maximum compression speed in kB/s\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	if (!*args[4]) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Expects an integer value.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	v = atoi(args[4]);
	if (v < 0) {
		appctx->ctx.cli.severity = LOG_ERR;
		appctx->ctx.cli.msg = "Value out of range.\n";
		appctx->st0 = CLI_ST_PRINT;
		return 1;
	}

	*res = v * mul;

	/* Dequeues all of the listeners waiting for a resource */
	if (!LIST_ISEMPTY(&global_listener_queue))
		dequeue_all_listeners(&global_listener_queue);

	return 1;
}

/* parse the "expose-fd" argument on the bind lines */
static int bind_parse_expose_fd(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing fd type", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}
	if (!strcmp(args[cur_arg+1], "listeners")) {
		conf->level |= ACCESS_FD_LISTENERS;
	} else {
		memprintf(err, "'%s' only supports 'listeners' (got '%s')",
			  args[cur_arg], args[cur_arg+1]);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

/* parse the "level" argument on the bind lines */
static int bind_parse_level(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing level", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (!strcmp(args[cur_arg+1], "user")) {
		conf->level &= ~ACCESS_LVL_MASK;
		conf->level |= ACCESS_LVL_USER;
	} else if (!strcmp(args[cur_arg+1], "operator")) {
		conf->level &= ~ACCESS_LVL_MASK;
		conf->level |= ACCESS_LVL_OPER;
	} else if (!strcmp(args[cur_arg+1], "admin")) {
		conf->level &= ~ACCESS_LVL_MASK;
		conf->level |= ACCESS_LVL_ADMIN;
	} else {
		memprintf(err, "'%s' only supports 'user', 'operator', and 'admin' (got '%s')",
			  args[cur_arg], args[cur_arg+1]);
		return ERR_ALERT | ERR_FATAL;
	}

	return 0;
}

static int bind_parse_severity_output(char **args, int cur_arg, struct proxy *px, struct bind_conf *conf, char **err)
{
	if (!*args[cur_arg + 1]) {
		memprintf(err, "'%s' : missing severity format", args[cur_arg]);
		return ERR_ALERT | ERR_FATAL;
	}

	if (set_severity_output(&conf->severity_output, args[cur_arg+1]))
		return 0;
	else {
		memprintf(err, "'%s' only supports 'none', 'number', and 'string' (got '%s')",
				args[cur_arg], args[cur_arg+1]);
		return ERR_ALERT | ERR_FATAL;
	}
}



/* Send all the bound sockets, always returns 1 */
static int _getsocks(char **args, char *payload, struct appctx *appctx, void *private)
{
	char *cmsgbuf = NULL;
	unsigned char *tmpbuf = NULL;
	struct cmsghdr *cmsg;
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct connection *remote = cs_conn(objt_cs(si_opposite(si)->end));
	struct msghdr msghdr;
	struct iovec iov;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	int *tmpfd;
	int tot_fd_nb = 0;
	struct proxy *px;
	int i = 0;
	int fd = -1;
	int curoff = 0;
	int old_fcntl = -1;
	int ret;

	if (!remote) {
		ha_warning("Only works on real connections\n");
		goto out;
	}

	fd = remote->handle.fd;

	/* Temporary set the FD in blocking mode, that will make our life easier */
	old_fcntl = fcntl(fd, F_GETFL);
	if (old_fcntl < 0) {
		ha_warning("Couldn't get the flags for the unix socket\n");
		goto out;
	}
	cmsgbuf = malloc(CMSG_SPACE(sizeof(int) * MAX_SEND_FD));
	if (!cmsgbuf) {
		ha_warning("Failed to allocate memory to send sockets\n");
		goto out;
	}
	if (fcntl(fd, F_SETFL, old_fcntl &~ O_NONBLOCK) == -1) {
		ha_warning("Cannot make the unix socket blocking\n");
		goto out;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
	iov.iov_base = &tot_fd_nb;
	iov.iov_len = sizeof(tot_fd_nb);
	if (!(strm_li(s)->bind_conf->level & ACCESS_FD_LISTENERS))
		goto out;
	memset(&msghdr, 0, sizeof(msghdr));
	/*
	 * First, calculates the total number of FD, so that we can let
	 * the caller know how much he should expects.
	 */
	px = proxies_list;
	while (px) {
		struct listener *l;

		list_for_each_entry(l, &px->conf.listeners, by_fe) {
			/* Only transfer IPv4/IPv6/UNIX sockets */
			if (l->state >= LI_ZOMBIE &&
			    (l->proto->sock_family == AF_INET ||
			    l->proto->sock_family == AF_INET6 ||
			    l->proto->sock_family == AF_UNIX))
				tot_fd_nb++;
		}
		px = px->next;
	}
	if (tot_fd_nb == 0)
		goto out;

	/* First send the total number of file descriptors, so that the
	 * receiving end knows what to expect.
	 */
	msghdr.msg_iov = &iov;
	msghdr.msg_iovlen = 1;
	ret = sendmsg(fd, &msghdr, 0);
	if (ret != sizeof(tot_fd_nb)) {
		ha_warning("Failed to send the number of sockets to send\n");
		goto out;
	}

	/* Now send the fds */
	msghdr.msg_control = cmsgbuf;
	msghdr.msg_controllen = CMSG_SPACE(sizeof(int) * MAX_SEND_FD);
	cmsg = CMSG_FIRSTHDR(&msghdr);
	cmsg->cmsg_len = CMSG_LEN(MAX_SEND_FD * sizeof(int));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	tmpfd = (int *)CMSG_DATA(cmsg);

	px = proxies_list;
	/* For each socket, e message is sent, containing the following :
	 *  Size of the namespace name (or 0 if none), as an unsigned char.
	 *  The namespace name, if any
	 *  Size of the interface name (or 0 if none), as an unsigned char
	 *  The interface name, if any
	 *  Listener options, as an int.
	 */
	/* We will send sockets MAX_SEND_FD per MAX_SEND_FD, allocate a
	 * buffer big enough to store the socket informations.
	 */
	tmpbuf = malloc(MAX_SEND_FD * (1 + MAXPATHLEN + 1 + IFNAMSIZ + sizeof(int)));
	if (tmpbuf == NULL) {
		ha_warning("Failed to allocate memory to transfer socket informations\n");
		goto out;
	}
	iov.iov_base = tmpbuf;
	while (px) {
		struct listener *l;

		list_for_each_entry(l, &px->conf.listeners, by_fe) {
			int ret;
			/* Only transfer IPv4/IPv6 sockets */
			if (l->state >= LI_ZOMBIE &&
			    (l->proto->sock_family == AF_INET ||
			    l->proto->sock_family == AF_INET6 ||
			    l->proto->sock_family == AF_UNIX)) {
				memcpy(&tmpfd[i % MAX_SEND_FD], &l->fd, sizeof(l->fd));
				if (!l->netns)
					tmpbuf[curoff++] = 0;
#ifdef USE_NS
				else {
					char *name = l->netns->node.key;
					unsigned char len = l->netns->name_len;
					tmpbuf[curoff++] = len;
					memcpy(tmpbuf + curoff, name, len);
					curoff += len;
				}
#endif
				if (l->interface) {
					unsigned char len = strlen(l->interface);
					tmpbuf[curoff++] = len;
					memcpy(tmpbuf + curoff, l->interface, len);
				curoff += len;
				} else
					tmpbuf[curoff++] = 0;
				memcpy(tmpbuf + curoff, &l->options,
				    sizeof(l->options));
				curoff += sizeof(l->options);


				i++;
			} else
				continue;
			if ((!(i % MAX_SEND_FD))) {
				iov.iov_len = curoff;
				if (sendmsg(fd, &msghdr, 0) != curoff) {
					ha_warning("Failed to transfer sockets\n");
					goto out;
				}
				/* Wait for an ack */
				do {
					ret = recv(fd, &tot_fd_nb,
					    sizeof(tot_fd_nb), 0);
				} while (ret == -1 && errno == EINTR);
				if (ret <= 0) {
					ha_warning("Unexpected error while transferring sockets\n");
					goto out;
				}
				curoff = 0;
			}

		}
		px = px->next;
	}
	if (i % MAX_SEND_FD) {
		iov.iov_len = curoff;
		cmsg->cmsg_len = CMSG_LEN((i % MAX_SEND_FD) * sizeof(int));
		msghdr.msg_controllen = CMSG_SPACE(sizeof(int) *  (i % MAX_SEND_FD));
		if (sendmsg(fd, &msghdr, 0) != curoff) {
			ha_warning("Failed to transfer sockets\n");
			goto out;
		}
	}

out:
	if (fd >= 0 && old_fcntl >= 0 && fcntl(fd, F_SETFL, old_fcntl) == -1) {
		ha_warning("Cannot make the unix socket non-blocking\n");
		goto out;
	}
	appctx->st0 = CLI_ST_END;
	free(cmsgbuf);
	free(tmpbuf);
	return 1;
}

static int cli_parse_simple(char **args, char *payload, struct appctx *appctx, void *private)
{
	if (*args[0] == 'h')
		/* help */
		cli_gen_usage_msg(appctx);
	else if (*args[0] == 'p')
		/* prompt */
		appctx->st1 ^= APPCTX_CLI_ST1_PROMPT;
	else if (*args[0] == 'q')
		/* quit */
		appctx->st0 = CLI_ST_END;

	return 1;
}

void pcli_write_prompt(struct stream *s)
{
	struct buffer *msg = get_trash_chunk();
	struct channel *oc = si_oc(&s->si[0]);

	if (!(s->pcli_flags & PCLI_F_PROMPT))
		return;

	if (s->pcli_flags & PCLI_F_PAYLOAD) {
		chunk_appendf(msg, "+ ");
	} else {
		if (s->pcli_next_pid == 0)
			chunk_appendf(msg, "master%s> ",
			              (global.mode & MODE_MWORKER_WAIT) ? "[ReloadFailed]" : "");
		else
			chunk_appendf(msg, "%d> ", s->pcli_next_pid);
	}
	co_inject(oc, msg->area, msg->data);
}


/* The pcli_* functions are used for the CLI proxy in the master */

void pcli_reply_and_close(struct stream *s, const char *msg)
{
	struct buffer *buf = get_trash_chunk();

	chunk_initstr(buf, msg);
	si_retnclose(&s->si[0], buf);
}

static enum obj_type *pcli_pid_to_server(int proc_pid)
{
	struct mworker_proc *child;

	/* return the CLI applet of the master */
	if (proc_pid == 0)
		return &cli_applet.obj_type;

	list_for_each_entry(child, &proc_list, list) {
		if (child->pid == proc_pid){
			return &child->srv->obj_type;
		}
	}
	return NULL;
}

/* Take a CLI prefix in argument (eg: @!1234 @master @1)
 *  Return:
 *     0: master
 *   > 0: pid of a worker
 *   < 0: didn't find a worker
 */
static int pcli_prefix_to_pid(const char *prefix)
{
	int proc_pid;
	struct mworker_proc *child;
	char *errtol = NULL;

	if (*prefix != '@') /* not a prefix, should not happen */
		return -1;

	prefix++;
	if (!*prefix)    /* sent @ alone, return the master */
		return 0;

	if (strcmp("master", prefix) == 0) {
		return 0;
	} else if (*prefix == '!') {
		prefix++;
		if (!*prefix)
			return -1;

		proc_pid = strtol(prefix, &errtol, 10);
		if (*errtol != '\0')
			return -1;
		list_for_each_entry(child, &proc_list, list) {
			if (!(child->options & PROC_O_TYPE_WORKER))
				continue;
			if (child->pid == proc_pid){
				return child->pid;
			}
		}
	} else {
		struct mworker_proc *chosen = NULL;
		/* this is a relative pid */

		proc_pid = strtol(prefix, &errtol, 10);
		if (*errtol != '\0')
			return -1;

		if (proc_pid == 0) /* return the master */
			return 0;

		/* chose the right process, the current one is the one with the
		 least number of reloads */
		list_for_each_entry(child, &proc_list, list) {
			if (!(child->options & PROC_O_TYPE_WORKER))
				continue;
			if (child->relative_pid == proc_pid){
				if (child->reloads == 0)
					return child->pid;
				else if (chosen == NULL || child->reloads < chosen->reloads)
					chosen = child;
			}
		}
		if (chosen)
			return chosen->pid;
	}
	return -1;
}

/* Return::
 *  >= 0 : number of words to escape
 *  = -1 : error
 */

int pcli_find_and_exec_kw(struct stream *s, char **args, int argl, char **errmsg, int *next_pid)
{
	if (argl < 1)
		return 0;

	/* there is a prefix */
	if (args[0][0] == '@') {
		int target_pid = pcli_prefix_to_pid(args[0]);

		if (target_pid == -1) {
			memprintf(errmsg, "Can't find the target PID matching the prefix '%s'\n", args[0]);
			return -1;
		}

		/* if the prefix is alone, define a default target */
		if (argl == 1)
			s->pcli_next_pid = target_pid;
		else
			*next_pid = target_pid;
		return 1;
	} else if (!strcmp("prompt", args[0])) {
		s->pcli_flags ^= PCLI_F_PROMPT;
		return argl; /* return the number of elements in the array */

	} else if (!strcmp("quit", args[0])) {
		channel_shutr_now(&s->req);
		channel_shutw_now(&s->res);
		return argl; /* return the number of elements in the array */
	} else if (!strcmp(args[0], "operator")) {
		if (!pcli_has_level(s, ACCESS_LVL_OPER)) {
			memprintf(errmsg, "Permission denied!\n");
			return -1;
		}
		s->pcli_flags &= ~ACCESS_LVL_MASK;
		s->pcli_flags |= ACCESS_LVL_OPER;
		return argl;

	} else if (!strcmp(args[0], "user")) {
		if (!pcli_has_level(s, ACCESS_LVL_USER)) {
			memprintf(errmsg, "Permission denied!\n");
			return -1;
		}
		s->pcli_flags &= ~ACCESS_LVL_MASK;
		s->pcli_flags |= ACCESS_LVL_USER;
		return argl;
	}

	return 0;
}

/*
 * Parse the CLI request:
 *  - It does basically the same as the cli_io_handler, but as a proxy
 *  - It can exec a command and strip non forwardable commands
 *
 *  Return:
 *  - the number of characters to forward or
 *  - 1 if there is an error or not enough data
 */
int pcli_parse_request(struct stream *s, struct channel *req, char **errmsg, int *next_pid)
{
	char *str = (char *)ci_head(req);
	char *end = (char *)ci_stop(req);
	char *args[MAX_STATS_ARGS + 1]; /* +1 for storing a NULL */
	int argl; /* number of args */
	char *p;
	char *trim = NULL;
	char *payload = NULL;
	int wtrim = 0; /* number of words to trim */
	int reql = 0;
	int ret;
	int i = 0;

	p = str;

	if (!(s->pcli_flags & PCLI_F_PAYLOAD)) {

		/* Looks for the end of one command */
		while (p+reql < end) {
			/* handle escaping */
			if (p[reql] == '\\') {
				reql++;
				continue;
			}
			if (p[reql] == ';' || p[reql] == '\n') {
				/* found the end of the command */
				p[reql] = '\n';
				reql++;
				break;
			}
			reql++;
		}
	} else {
		while (p+reql < end) {
			if (p[reql] == '\n') {
				/* found the end of the line */
				reql++;
				break;
			}
			reql++;
		}
	}

	/* set end to first byte after the end of the command */
	end = p + reql;

	/* there is no end to this command, need more to parse ! */
	if (*(end-1) != '\n') {
		return -1;
	}

	if (s->pcli_flags & PCLI_F_PAYLOAD) {
		if (reql == 1) /* last line of the payload */
			s->pcli_flags &= ~PCLI_F_PAYLOAD;
		return reql;
	}

	*(end-1) = '\0';

	/* splits the command in words */
	while (i < MAX_STATS_ARGS && p < end) {
		int j, k;

		/* skip leading spaces/tabs */
		p += strspn(p, " \t");
		if (!*p)
			break;

		args[i] = p;
		p += strcspn(p, " \t");
		*p++ = 0;

		/* unescape backslashes (\) */
		for (j = 0, k = 0; args[i][k]; k++) {
			if (args[i][k] == '\\') {
				if (args[i][k + 1] == '\\')
					k++;
				else
					continue;
			}
			args[i][j] = args[i][k];
			j++;
		}
		args[i][j] = 0;
		i++;
	}

	argl = i;

	for (; i < MAX_STATS_ARGS + 1; i++)
		args[i] = NULL;

	wtrim = pcli_find_and_exec_kw(s, args, argl, errmsg, next_pid);

	/* End of words are ending by \0, we need to replace the \0s by spaces
1	   before forwarding them */
	p = str;
	while (p < end-1) {
		if (*p == '\0')
			*p = ' ';
		p++;
	}

	payload = strstr(str, PAYLOAD_PATTERN);
	if ((end - 1) == (payload + strlen(PAYLOAD_PATTERN))) {
		/* if the payload pattern is at the end */
		s->pcli_flags |= PCLI_F_PAYLOAD;
		ret = reql;
	}

	*(end-1) = '\n';

	if (wtrim > 0) {
		trim = &args[wtrim][0];
		if (trim == NULL) /* if this was the last word in the table */
			trim = end;

		b_del(&req->buf, trim - str);

		ret = end - trim;
	} else if (wtrim < 0) {
		/* parsing error */
		return -1;
	} else {
		/* the whole string */
		ret = end - str;
	}

	if (ret > 1) {
		if (pcli_has_level(s, ACCESS_LVL_ADMIN)) {
			goto end;
		} else if (pcli_has_level(s, ACCESS_LVL_OPER)) {
			ci_insert_line2(req, 0, "operator -", strlen("operator -"));
			ret += strlen("operator -") + 2;
		} else if (pcli_has_level(s, ACCESS_LVL_USER)) {
			ci_insert_line2(req, 0, "user -", strlen("user -"));
			ret += strlen("user -") + 2;
		}
	}
end:

	return ret;
}

int pcli_wait_for_request(struct stream *s, struct channel *req, int an_bit)
{
	int next_pid = -1;
	int to_forward;
	char *errmsg = NULL;

	if ((s->pcli_flags & ACCESS_LVL_MASK) == ACCESS_LVL_NONE)
		s->pcli_flags |= strm_li(s)->bind_conf->level & ACCESS_LVL_MASK;

read_again:
	/* if the channel is closed for read, we won't receive any more data
	   from the client, but we don't want to forward this close to the
	   server */
	channel_dont_close(req);

	/* We don't know yet to which server we will connect */
	channel_dont_connect(req);


	/* we are not waiting for a response, there is no more request and we
	 * receive a close from the client, we can leave */
	if (!(ci_data(req)) && req->flags & CF_SHUTR) {
		channel_shutw_now(&s->res);
		s->req.analysers &= ~AN_REQ_WAIT_CLI;
		return 1;
	}

	req->flags |= CF_READ_DONTWAIT;

	/* need more data */
	if (!ci_data(req))
		return 0;

	/* If there is data available for analysis, log the end of the idle time. */
	if (c_data(req) && s->logs.t_idle == -1)
		s->logs.t_idle = tv_ms_elapsed(&s->logs.tv_accept, &now) - s->logs.t_handshake;

	to_forward = pcli_parse_request(s, req, &errmsg, &next_pid);
	if (to_forward > 0) {
		int target_pid;
		/* enough data */

		/* forward only 1 command */
		channel_forward(req, to_forward);

		if (!(s->pcli_flags & PCLI_F_PAYLOAD)) {
			/* we send only 1 command per request, and we write close after it */
			channel_shutw_now(req);
		} else {
			pcli_write_prompt(s);
		}

		s->res.flags |= CF_WAKE_ONCE; /* need to be called again */

		/* remove the XFER_DATA analysers, which forwards all
		 * the data, we don't want to forward the next requests
		 * We need to add CF_FLT_ANALYZE to abort the forward too.
		 */
		req->analysers &= ~(AN_REQ_FLT_XFER_DATA|AN_REQ_WAIT_CLI);
		req->analysers |= AN_REQ_FLT_END|CF_FLT_ANALYZE;
		s->res.analysers |= AN_RES_WAIT_CLI;

		if (!(s->flags & SF_ASSIGNED)) {
			if (next_pid > -1)
				target_pid = next_pid;
			else
				target_pid = s->pcli_next_pid;
			/* we can connect now */
			s->target = pcli_pid_to_server(target_pid);

			s->flags |= (SF_DIRECT | SF_ASSIGNED);
			channel_auto_connect(req);
		}

	} else if (to_forward == 0) {
		/* we trimmed things but we might have other commands to consume */
		pcli_write_prompt(s);
		goto read_again;
	} else if (to_forward == -1 && errmsg) {
		/* there was an error during the parsing */
			pcli_reply_and_close(s, errmsg);
			return 0;
	} else if (to_forward == -1 && channel_full(req, global.tune.maxrewrite)) {
		/* buffer is full and we didn't catch the end of a command */
		goto send_help;
	}

	return 0;

send_help:
	b_reset(&req->buf);
	b_putblk(&req->buf, "help\n", 5);
	goto read_again;
}

int pcli_wait_for_response(struct stream *s, struct channel *rep, int an_bit)
{
	struct proxy *fe = strm_fe(s);
	struct proxy *be = s->be;

	if (rep->flags & CF_READ_ERROR) {
		pcli_reply_and_close(s, "Can't connect to the target CLI!\n");
		s->res.analysers &= ~AN_RES_WAIT_CLI;
		return 0;
	}
	rep->flags |= CF_READ_DONTWAIT; /* try to get back here ASAP */
	rep->flags |= CF_NEVER_WAIT;

	/* don't forward the close */
	channel_dont_close(&s->res);
	channel_dont_close(&s->req);

	if (s->pcli_flags & PCLI_F_PAYLOAD) {
		s->req.analysers |= AN_REQ_WAIT_CLI;
		s->res.analysers &= ~AN_RES_WAIT_CLI;
		s->req.flags |= CF_WAKE_ONCE; /* need to be called again if there is some command left in the request */
		return 0;
	}

	/* forward the data */
	if (ci_data(rep)) {
		c_adv(rep, ci_data(rep));
		return 0;
	}

	if ((rep->flags & (CF_SHUTR|CF_READ_NULL))) {
		/* stream cleanup */

		pcli_write_prompt(s);

		s->si[1].flags |= SI_FL_NOLINGER | SI_FL_NOHALF;
		si_shutr(&s->si[1]);
		si_shutw(&s->si[1]);

		/*
		 * starting from there this the same code as
		 * http_end_txn_clean_session().
		 *
		 * It allows to do frontend keepalive while reconnecting to a
		 * new server for each request.
		 */

		if (s->flags & SF_BE_ASSIGNED) {
			HA_ATOMIC_SUB(&be->beconn, 1);
			if (unlikely(s->srv_conn))
				sess_change_server(s, NULL);
		}

		s->logs.t_close = tv_ms_elapsed(&s->logs.tv_accept, &now);
		stream_process_counters(s);

		/* don't count other requests' data */
		s->logs.bytes_in  -= ci_data(&s->req);
		s->logs.bytes_out -= ci_data(&s->res);

		/* we may need to know the position in the queue */
		pendconn_free(s);

		/* let's do a final log if we need it */
		if (!LIST_ISEMPTY(&fe->logformat) && s->logs.logwait &&
		    !(s->flags & SF_MONITOR) &&
		    (!(fe->options & PR_O_NULLNOLOG) || s->req.total)) {
			s->do_log(s);
		}

		/* stop tracking content-based counters */
		stream_stop_content_counters(s);
		stream_update_time_stats(s);

		s->logs.accept_date = date; /* user-visible date for logging */
		s->logs.tv_accept = now;  /* corrected date for internal use */
		s->logs.t_handshake = 0; /* There are no handshake in keep alive connection. */
		s->logs.t_idle = -1;
		tv_zero(&s->logs.tv_request);
		s->logs.t_queue = -1;
		s->logs.t_connect = -1;
		s->logs.t_data = -1;
		s->logs.t_close = 0;
		s->logs.prx_queue_pos = 0;  /* we get the number of pending conns before us */
		s->logs.srv_queue_pos = 0; /* we will get this number soon */

		s->logs.bytes_in = s->req.total = ci_data(&s->req);
		s->logs.bytes_out = s->res.total = ci_data(&s->res);

		stream_del_srv_conn(s);
		if (objt_server(s->target)) {
			if (s->flags & SF_CURR_SESS) {
				s->flags &= ~SF_CURR_SESS;
				HA_ATOMIC_SUB(&__objt_server(s->target)->cur_sess, 1);
			}
			if (may_dequeue_tasks(__objt_server(s->target), be))
				process_srv_queue(__objt_server(s->target));
		}

		s->target = NULL;

		/* only release our endpoint if we don't intend to reuse the
		 * connection.
		 */
		if (!si_conn_ready(&s->si[1])) {
			si_release_endpoint(&s->si[1]);
			s->srv_conn = NULL;
		}

		s->si[1].state     = s->si[1].prev_state = SI_ST_INI;
		s->si[1].err_type  = SI_ET_NONE;
		s->si[1].conn_retries = 0;  /* used for logging too */
		s->si[1].exp       = TICK_ETERNITY;
		s->si[1].flags    &= SI_FL_ISBACK | SI_FL_DONT_WAKE; /* we're in the context of process_stream */
		s->req.flags &= ~(CF_SHUTW|CF_SHUTW_NOW|CF_AUTO_CONNECT|CF_WRITE_ERROR|CF_STREAMER|CF_STREAMER_FAST|CF_NEVER_WAIT|CF_WAKE_CONNECT|CF_WROTE_DATA);
		s->res.flags &= ~(CF_SHUTR|CF_SHUTR_NOW|CF_READ_ATTACHED|CF_READ_ERROR|CF_READ_NOEXP|CF_STREAMER|CF_STREAMER_FAST|CF_WRITE_PARTIAL|CF_NEVER_WAIT|CF_WROTE_DATA|CF_READ_NULL);
		s->flags &= ~(SF_DIRECT|SF_ASSIGNED|SF_ADDR_SET|SF_BE_ASSIGNED|SF_FORCE_PRST|SF_IGNORE_PRST);
		s->flags &= ~(SF_CURR_SESS|SF_REDIRECTABLE|SF_SRV_REUSED);
		s->flags &= ~(SF_ERR_MASK|SF_FINST_MASK|SF_REDISP);
		/* reinitialise the current rule list pointer to NULL. We are sure that
		 * any rulelist match the NULL pointer.
		 */
		s->current_rule_list = NULL;

		s->be = strm_fe(s);
		s->logs.logwait = strm_fe(s)->to_log;
		s->logs.level = 0;
		stream_del_srv_conn(s);
		s->target = NULL;
		/* re-init store persistence */
		s->store_count = 0;
		s->uniq_id = global.req_count++;

		s->req.flags |= CF_READ_DONTWAIT; /* one read is usually enough */

		s->req.flags |= CF_WAKE_ONCE; /* need to be called again if there is some command left in the request */

		s->req.analysers |= AN_REQ_WAIT_CLI;
		s->res.analysers &= ~AN_RES_WAIT_CLI;

		/* We must trim any excess data from the response buffer, because we
		 * may have blocked an invalid response from a server that we don't
		 * want to accidently forward once we disable the analysers, nor do
		 * we want those data to come along with next response. A typical
		 * example of such data would be from a buggy server responding to
		 * a HEAD with some data, or sending more than the advertised
		 * content-length.
		 */
		if (unlikely(ci_data(&s->res)))
			b_set_data(&s->res.buf, co_data(&s->res));

		/* Now we can realign the response buffer */
		c_realign_if_empty(&s->res);

		s->req.rto = strm_fe(s)->timeout.client;
		s->req.wto = TICK_ETERNITY;

		s->res.rto = TICK_ETERNITY;
		s->res.wto = strm_fe(s)->timeout.client;

		s->req.rex = TICK_ETERNITY;
		s->req.wex = TICK_ETERNITY;
		s->req.analyse_exp = TICK_ETERNITY;
		s->res.rex = TICK_ETERNITY;
		s->res.wex = TICK_ETERNITY;
		s->res.analyse_exp = TICK_ETERNITY;
		s->si[1].hcto = TICK_ETERNITY;

		/* we're removing the analysers, we MUST re-enable events detection.
		 * We don't enable close on the response channel since it's either
		 * already closed, or in keep-alive with an idle connection handler.
		 */
		channel_auto_read(&s->req);
		channel_auto_close(&s->req);
		channel_auto_read(&s->res);


		return 1;
	}
	return 0;
}

/*
 * The mworker functions are used to initialize the CLI in the master process
 */

 /*
 * Stop the mworker proxy
 */
void mworker_cli_proxy_stop()
{
	if (mworker_proxy)
		stop_proxy(mworker_proxy);
}

/*
 * Create the mworker CLI proxy
 */
int mworker_cli_proxy_create()
{
	struct mworker_proc *child;
	char *msg = NULL;
	char *errmsg = NULL;

	mworker_proxy = calloc(1, sizeof(*mworker_proxy));
	if (!mworker_proxy)
		return -1;

	init_new_proxy(mworker_proxy);
	mworker_proxy->next = proxies_list;
	proxies_list = mworker_proxy;
	mworker_proxy->id = strdup("MASTER");
	mworker_proxy->mode = PR_MODE_CLI;
	mworker_proxy->state = PR_STNEW;
	mworker_proxy->last_change = now.tv_sec;
	mworker_proxy->cap = PR_CAP_LISTEN; /* this is a listen section */
	mworker_proxy->maxconn = 10;                 /* default to 10 concurrent connections */
	mworker_proxy->timeout.client = 0; /* no timeout */
	mworker_proxy->conf.file = strdup("MASTER");
	mworker_proxy->conf.line = 0;
	mworker_proxy->accept = frontend_accept;
	mworker_proxy-> lbprm.algo = BE_LB_ALGO_NONE;

	/* Does not init the default target the CLI applet, but must be done in
	 * the request parsing code */
	mworker_proxy->default_target = NULL;

	/* the check_config_validity() will get an ID for the proxy */
	mworker_proxy->uuid = -1;

	proxy_store_name(mworker_proxy);

	/* create all servers using the mworker_proc list */
	list_for_each_entry(child, &proc_list, list) {
		struct server *newsrv = NULL;
		struct sockaddr_storage *sk;
		int port1, port2, port;
		struct protocol *proto;

		newsrv = new_server(mworker_proxy);
		if (!newsrv)
			goto error;

		/* we don't know the new pid yet */
		if (child->pid == -1)
			memprintf(&msg, "cur-%d", child->relative_pid);
		else
			memprintf(&msg, "old-%d", child->pid);

		newsrv->next = mworker_proxy->srv;
		mworker_proxy->srv = newsrv;
		newsrv->conf.file = strdup(msg);
		newsrv->id = strdup(msg);
		newsrv->conf.line = 0;

		memprintf(&msg, "sockpair@%d", child->ipc_fd[0]);
		if ((sk = str2sa_range(msg, &port, &port1, &port2, &errmsg, NULL, NULL, 0)) == 0) {
			goto error;
		}
		free(msg);
		msg = NULL;

		proto = protocol_by_family(sk->ss_family);
		if (!proto || !proto->connect) {
			goto error;
		}

		/* no port specified */
		newsrv->flags |= SRV_F_MAPPORTS;
		newsrv->addr = *sk;
		/* don't let the server participate to load balancing */
		newsrv->iweight = 0;
		newsrv->uweight = 0;
		srv_lb_commit_status(newsrv);

		child->srv = newsrv;
	}
	return 0;

error:
	ha_alert("%s\n", errmsg);

	list_for_each_entry(child, &proc_list, list) {
		free((char *)child->srv->conf.file); /* cast because of const char *  */
		free(child->srv->id);
		free(child->srv);
		child->srv = NULL;
	}
	free(mworker_proxy->id);
	free(mworker_proxy->conf.file);
	free(mworker_proxy);
	mworker_proxy = NULL;
	free(errmsg);
	free(msg);

	return -1;
}

/*
 * Create a new listener for the master CLI proxy
 */
int mworker_cli_proxy_new_listener(char *line)
{
	struct bind_conf *bind_conf;
	struct listener *l;
	char *err = NULL;
	char *args[MAX_LINE_ARGS + 1];
	int arg;
	int cur_arg;

	arg = 0;
	args[0] = line;

	/* args is a bind configuration with spaces replaced by commas */
	while (*line && arg < MAX_LINE_ARGS) {

		if (*line == ',') {
			*line++ = '\0';
			while (*line == ',')
				line++;
			args[++arg] = line;
		}
		line++;
	}

	args[++arg] = "\0";

	bind_conf = bind_conf_alloc(mworker_proxy, "master-socket", 0, "", xprt_get(XPRT_RAW));
	if (!bind_conf)
		goto err;

	bind_conf->level &= ~ACCESS_LVL_MASK;
	bind_conf->level |= ACCESS_LVL_ADMIN;

	if (!str2listener(args[0], mworker_proxy, bind_conf, "master-socket", 0, &err)) {
		ha_alert("Cannot create the listener of the master CLI\n");
		goto err;
	}

	cur_arg = 1;

	while (*args[cur_arg]) {
			static int bind_dumped;
			struct bind_kw *kw;

			kw = bind_find_kw(args[cur_arg]);
			if (kw) {
				if (!kw->parse) {
					memprintf(&err, "'%s %s' : '%s' option is not implemented in this version (check build options).",
						  args[0], args[1], args[cur_arg]);
					goto err;
				}

				if (kw->parse(args, cur_arg, global.stats_fe, bind_conf, &err) != 0) {
					if (err)
						memprintf(&err, "'%s %s' : '%s'", args[0], args[1], err);
					else
						memprintf(&err, "'%s %s' : error encountered while processing '%s'",
						          args[0], args[1], args[cur_arg]);
					goto err;
				}

				cur_arg += 1 + kw->skip;
				continue;
			}

			if (!bind_dumped) {
				bind_dump_kws(&err);
				indent_msg(&err, 4);
				bind_dumped = 1;
			}

			memprintf(&err, "'%s %s' : unknown keyword '%s'.%s%s",
			          args[0], args[1], args[cur_arg],
			          err ? " Registered keywords :" : "", err ? err : "");
			goto err;
	}


	list_for_each_entry(l, &bind_conf->listeners, by_bind) {
		l->accept = session_accept_fd;
		l->default_target = mworker_proxy->default_target;
		/* don't make the peers subject to global limits and don't close it in the master */
		l->options |= (LI_O_UNLIMITED|LI_O_MWORKER); /* we are keeping this FD in the master */
		l->nice = -64;  /* we want to boost priority for local stats */
		global.maxsock++; /* for the listening socket */
	}
	global.maxsock += mworker_proxy->maxconn;

	return 0;

err:
	ha_alert("%s\n", err);
	free(err);
	free(bind_conf);
	return -1;

}

/*
 * Create a new CLI socket using a socketpair for a worker process
 * <mworker_proc> is the process structure, and <proc> is the process number
 */
int mworker_cli_sockpair_new(struct mworker_proc *mworker_proc, int proc)
{
	struct bind_conf *bind_conf;
	struct listener *l;
	char *path = NULL;
	char *err = NULL;

	/* master pipe to ensure the master is still alive  */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, mworker_proc->ipc_fd) < 0) {
		ha_alert("Cannot create worker socketpair.\n");
		return -1;
	}

	/* XXX: we might want to use a separate frontend at some point */
	if (!global.stats_fe) {
		if ((global.stats_fe = alloc_stats_fe("GLOBAL", "master-socket", 0)) == NULL) {
			ha_alert("out of memory trying to allocate the stats frontend");
			goto error;
		}
	}

	bind_conf = bind_conf_alloc(global.stats_fe, "master-socket", 0, "", xprt_get(XPRT_RAW));
	if (!bind_conf)
		goto error;

	bind_conf->level &= ~ACCESS_LVL_MASK;
	bind_conf->level |= ACCESS_LVL_ADMIN; /* TODO: need to lower the rights with a CLI keyword*/

	bind_conf->bind_proc = 1UL << proc;
	global.stats_fe->bind_proc = 0; /* XXX: we should be careful with that, it can be removed by configuration */

	if (!memprintf(&path, "sockpair@%d", mworker_proc->ipc_fd[1])) {
		ha_alert("Cannot allocate listener.\n");
		goto error;
	}

	if (!str2listener(path, global.stats_fe, bind_conf, "master-socket", 0, &err)) {
		free(path);
		ha_alert("Cannot create a CLI sockpair listener for process #%d\n", proc);
		goto error;
	}
	free(path);
	path = NULL;

	list_for_each_entry(l, &bind_conf->listeners, by_bind) {
		l->accept = session_accept_fd;
		l->default_target = global.stats_fe->default_target;
		l->options |= (LI_O_UNLIMITED | LI_O_NOSTOP);
		/* it's a sockpair but we don't want to keep the fd in the master */
		l->options &= ~LI_O_INHERITED;
		l->nice = -64;  /* we want to boost priority for local stats */
		global.maxsock++; /* for the listening socket */
	}

	return 0;

error:
	close(mworker_proc->ipc_fd[0]);
	close(mworker_proc->ipc_fd[1]);
	free(err);

	return -1;
}

static struct applet cli_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<CLI>", /* used for logging */
	.fct = cli_io_handler,
	.release = cli_release_handler,
};

/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "help", NULL }, NULL, cli_parse_simple, NULL },
	{ { "prompt", NULL }, NULL, cli_parse_simple, NULL },
	{ { "quit", NULL }, NULL, cli_parse_simple, NULL },
	{ { "set", "maxconn", "global",  NULL }, "set maxconn global : change the per-process maxconn setting", cli_parse_set_maxconn_global, NULL },
	{ { "set", "rate-limit", NULL }, "set rate-limit : change a rate limiting value", cli_parse_set_ratelimit, NULL },
	{ { "set", "severity-output",  NULL }, "set severity-output [none|number|string] : set presence of severity level in feedback information", cli_parse_set_severity_output, NULL, NULL },
	{ { "set", "timeout",  NULL }, "set timeout    : change a timeout setting", cli_parse_set_timeout, NULL, NULL },
	{ { "show", "env",  NULL }, "show env [var] : dump environment variables known to the process", cli_parse_show_env, cli_io_handler_show_env, NULL },
	{ { "show", "cli", "sockets",  NULL }, "show cli sockets : dump list of cli sockets", cli_parse_default, cli_io_handler_show_cli_sock, NULL, NULL, ACCESS_MASTER },
	{ { "show", "cli", "level", NULL },    "show cli level   : display the level of the current CLI session", cli_parse_show_lvl, NULL, NULL, NULL, ACCESS_MASTER},
	{ { "show", "fd", NULL }, "show fd [num] : dump list of file descriptors in use", cli_parse_show_fd, cli_io_handler_show_fd, NULL },
	{ { "show", "activity", NULL }, "show activity : show per-thread activity stats (for support/developers)", cli_parse_default, cli_io_handler_show_activity, NULL },
	{ { "operator", NULL },  "operator       : lower the level of the current CLI session to operator", cli_parse_set_lvl, NULL, NULL, NULL, ACCESS_MASTER},
	{ { "user", NULL },      "user           : lower the level of the current CLI session to user", cli_parse_set_lvl, NULL, NULL, NULL, ACCESS_MASTER},
	{ { "_getsocks", NULL }, NULL,  _getsocks, NULL },
	{{},}
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "stats", stats_parse_global },
	{ 0, NULL, NULL },
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);

static struct bind_kw_list bind_kws = { "STAT", { }, {
	{ "level",     bind_parse_level,    1 }, /* set the unix socket admin level */
	{ "expose-fd", bind_parse_expose_fd, 1 }, /* set the unix socket expose fd rights */
	{ "severity-output", bind_parse_severity_output, 1 }, /* set the severity output format */
	{ NULL, NULL, 0 },
}};

INITCALL1(STG_REGISTER, bind_register_keywords, &bind_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
