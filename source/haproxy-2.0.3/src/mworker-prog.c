/*
 * Master Worker - program
 *
 * Copyright HAProxy Technologies - William Lallemand <wlallemand@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <common/cfgparse.h>
#include <common/errors.h>
#include <common/initcall.h>

#include <proto/log.h>
#include <proto/mworker.h>

static int use_program = 0; /* do we use the program section ? */

/*
 * Launch every programs
 */
int mworker_ext_launch_all()
{
	int ret;
	struct mworker_proc *child;
	struct mworker_proc *tmp;
	int reexec = 0;

	if (!use_program)
		return 0;

	reexec = getenv("HAPROXY_MWORKER_REEXEC") ? 1 : 0;

	/* find the right mworker_proc */
	list_for_each_entry_safe(child, tmp, &proc_list, list) {
		if (child->reloads == 0 && (child->options & PROC_O_TYPE_PROG)) {

			if (reexec && (!(child->options & PROC_O_START_RELOAD))) {
				struct mworker_proc *old_child;

				/*
				 * This is a reload and we don't want to fork a
				 * new program so have to remove the entry in
				 * the list.
				 *
				 * But before that, we need to mark the
				 * previous program as not leaving, if we find one.
				 */

				list_for_each_entry(old_child, &proc_list, list) {
					if (!(old_child->options & PROC_O_TYPE_PROG) || (!(old_child->options & PROC_O_LEAVING)))
						continue;

					if (!strcmp(old_child->id, child->id))
						old_child->options &= ~PROC_O_LEAVING;
				}


				LIST_DEL(&child->list);
				mworker_free_child(child);
				child = NULL;

				continue;
			}

			child->timestamp = now.tv_sec;

			ret = fork();
			if (ret < 0) {
				ha_alert("Cannot fork program '%s'.\n", child->id);
				exit(EXIT_FAILURE); /* there has been an error */
			} else if (ret > 0) { /* parent */
				child->pid = ret;
				ha_notice("New program '%s' (%d) forked\n", child->id, ret);
				continue;
			} else if (ret == 0) {
				/* In child */
				mworker_unblock_signals();
				mworker_cleanlisteners();
				mworker_cleantasks();

				execvp(child->command[0], child->command);

				ha_alert("Cannot execute %s: %s\n", child->command[0], strerror(errno));
				exit(EXIT_FAILURE);
			}
		}
	}

	return 0;

}


/* Configuration */

int cfg_parse_program(const char *file, int linenum, char **args, int kwm)
{
	static struct mworker_proc *ext_child = NULL;
	struct mworker_proc *child;
	int err_code = 0;

	if (!strcmp(args[0], "program")) {
		if (alertif_too_many_args(1, file, linenum, args, &err_code)) {
			err_code |= ERR_ABORT;
			goto error;
		}

		if (!*args[1]) {
			ha_alert("parsing [%s:%d] : '%s' expects an <id> argument\n",
				 file, linenum, args[0]);
			err_code |= ERR_ALERT | ERR_ABORT;
			goto error;
		}

		ext_child = calloc(1, sizeof(*ext_child));
		if (!ext_child) {
			ha_alert("parsing [%s:%d] : out of memory.\n", file, linenum);
			err_code |= ERR_ALERT | ERR_ABORT;
			goto error;
		}

		ext_child->options |= PROC_O_TYPE_PROG; /* external process */
		ext_child->command = NULL;
		ext_child->path = NULL;
		ext_child->id = NULL;
		ext_child->pid = -1;
		ext_child->relative_pid = -1;
		ext_child->reloads = 0;
		ext_child->timestamp = -1;
		ext_child->ipc_fd[0] = -1;
		ext_child->ipc_fd[1] = -1;
		ext_child->options |= PROC_O_START_RELOAD; /* restart the programs by default */
		LIST_INIT(&ext_child->list);

		list_for_each_entry(child, &proc_list, list) {
			if (child->reloads == 0 && (child->options & PROC_O_TYPE_PROG)) {
				if (!strcmp(args[1], child->id)) {
					ha_alert("parsing [%s:%d]: '%s' program section already exists in the configuration.\n", file, linenum, args[1]);
					err_code |= ERR_ALERT | ERR_ABORT;
					goto error;
				}
			}
		}

		ext_child->id = strdup(args[1]);
		if (!ext_child->id) {
			ha_alert("parsing [%s:%d] : out of memory.\n", file, linenum);
			err_code |= ERR_ALERT | ERR_ABORT;
			goto error;
		}

		LIST_ADDQ(&proc_list, &ext_child->list);

	} else if (!strcmp(args[0], "command")) {
		int arg_nb = 0;
		int i = 0;

		if (*(args[1]) == 0) {
			ha_alert("parsing [%s:%d]: '%s' expects a command with optional arguments separated in words.\n", file, linenum, args[0]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto error;
		}

		while (*args[arg_nb+1])
			arg_nb++;

		ext_child->command = calloc(arg_nb+1, sizeof(*ext_child->command));

		if (!ext_child->command) {
			ha_alert("parsing [%s:%d] : out of memory.\n", file, linenum);
			err_code |= ERR_ALERT | ERR_ABORT;
			goto error;
		}

		while (i < arg_nb) {
			ext_child->command[i] = strdup(args[i+1]);
			if (!ext_child->command[i]) {
				ha_alert("parsing [%s:%d] : out of memory.\n", file, linenum);
				err_code |= ERR_ALERT | ERR_ABORT;
				goto error;
			}
			i++;
		}
		ext_child->command[i] = NULL;

	} else if (!strcmp(args[0], "option")) {

		if (*(args[1]) == '\0') {
			ha_alert("parsing [%s:%d]: '%s' expects an option name.\n",
				 file, linenum, args[0]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto error;
		}

		if (strcmp(args[1], "start-on-reload") == 0) {
			if (alertif_too_many_args_idx(0, 1, file, linenum, args, &err_code))
				goto error;
			if (kwm == KWM_STD)
				ext_child->options |= PROC_O_START_RELOAD;
			else if (kwm == KWM_NO)
				ext_child->options &= ~PROC_O_START_RELOAD;
			goto out;

		} else {
			ha_alert("parsing [%s:%d] : unknown option '%s'.\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto error;
		}
	} else {
		ha_alert("parsing [%s:%d] : unknown keyword '%s' in '%s' section\n", file, linenum, args[0], "program");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto error;
	}

	use_program = 1;

	return err_code;

error:
	if (ext_child) {
		LIST_DEL(&ext_child->list);
		if (ext_child->command) {
			int i;

			for (i = 0; ext_child->command[i]; i++) {
				if (ext_child->command[i]) {
					free(ext_child->command[i]);
					ext_child->command[i] = NULL;
				}
			}
			free(ext_child->command);
			ext_child->command = NULL;
		}
		if (ext_child->id) {
			free(ext_child->id);
			ext_child->id = NULL;
		}
	}

	free(ext_child);
	ext_child = NULL;

out:
	return err_code;

}

int cfg_program_postparser()
{
	int err_code = 0;
	struct mworker_proc *child;

	list_for_each_entry(child, &proc_list, list) {
		if (child->reloads == 0 && (child->options & PROC_O_TYPE_PROG)) {
			if (child->command == NULL) {
				ha_alert("The program section '%s' lacks a command to launch.\n", child->id);
				err_code |= ERR_ALERT | ERR_FATAL;
			}
		}
	}

	if (use_program && !(global.mode & MODE_MWORKER)) {
		ha_alert("Can't use a 'program' section without master worker mode.\n");
		err_code |= ERR_ALERT | ERR_FATAL;
	}

	return err_code;
}


REGISTER_CONFIG_SECTION("program", cfg_parse_program, NULL);
REGISTER_CONFIG_POSTPARSER("program", cfg_program_postparser);
