/*
 * Handle directory traversal
 *
 * Copyright (c) 2011, Alan DeKok <aland at freeradius dot org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include "recli.h"

static int read_envp(const char *filename, recli_config_t *config)
{
	int argc;
	FILE *fp;
	char buffer[1024];

	fp = fopen(filename, "r");
	if (!fp) return -1;

	argc = 0;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char *p;

		for (p = buffer; *p != '\0'; p++) {
			if ((*p == '\n') || (*p == '\r')) {
				*p = '\0';
				break;
			}
		}

		if (p == (buffer + sizeof(buffer) - 1)) {
			return -1; /* line too long */
		}

		if (p == buffer) continue;

		config->envp[argc++] = strdup(buffer);
		if (argc >= 127) return -1;
	}

	config->envp[argc] = NULL;


	fclose(fp);
	return 0;  
}


typedef struct rbuf_t {
	char buffer[8192];
	char *start;
	cli_syntax_t **phead;
	recli_fprintf_t old_fprintf;
	void *old_ctx;
	int rcode;
} rbuf_t;


static rbuf_t buf_out, buf_err;

static int recli_fprintf_syntax(void *ctx, const char *fmt, ...)
{
	int rcode;
	char *p;
	va_list args;
	rbuf_t *b = ctx;

	va_start(args, fmt);
	rcode = vsnprintf(b->start, b->buffer + sizeof(b->buffer) - b->start,
			  fmt, args);
	va_end(args);

	/*
	 *	If we're called for stderr, dump it to the caller.
	 */
	if (b == &buf_err) {
		return b->old_fprintf(b->old_ctx, "%s", b->buffer);
	}

	for (p = b->start; *p != '\0'; p++) {
		if (!isspace((int) *p)) break;
	}

	if (!*p || isspace((int) *p)) return 0;


	/*
	 *	Errors go to the caller, not to us.
	 */
	recli_fprintf = b->old_fprintf;
	recli_stdout = buf_out.old_ctx;
	recli_stderr = buf_err.old_ctx;

	if (syntax_merge(b->phead, b->buffer) < 0) {
		b->rcode = -1;
	}

	recli_fprintf = recli_fprintf_syntax;
	recli_stdout = &buf_out;
	recli_stderr = &buf_err;

	return rcode;
}


int recli_exec_syntax(cli_syntax_t **phead, char *program)
{
	int rcode = 0;
	char *p;
	char *argv[3];

	argv[0] = program;
	argv[1] = "--config";
	argv[2] = "syntax";

	buf_out.old_fprintf = recli_fprintf;
	buf_out.old_ctx = recli_stdout;
	buf_out.rcode = 0;
	buf_out.phead = phead;
	buf_out.start = buf_out.buffer;

	buf_err.old_fprintf = recli_fprintf;
	buf_err.old_ctx = recli_stderr;
	buf_err.rcode = 0;
	buf_err.phead = NULL;
	buf_err.start = buf_err.buffer;

	recli_fprintf = recli_fprintf_syntax;
	recli_stdout = &buf_out;
	recli_stderr = &buf_err;

	strlcpy(buf_out.buffer, program, sizeof(buf_out.buffer));
	for (p = buf_out.buffer; *p != '\0'; p++) {
		if (*p == '/') {
			*p = ' ';
		}
		p++;
	}
	p[0] = ' ';
	p[1] = '\0';
	buf_out.start = p + 1;

	rcode = recli_exec("init", 3, argv, NULL);

	recli_fprintf = buf_out.old_fprintf ;
	recli_stdout = buf_out.old_ctx;
	recli_stderr = buf_err.old_ctx;

	return rcode;
}


int recli_bootstrap(recli_config_t *config)
{
	int rcode;
	struct stat statbuf;
	char buffer[8192];

	if (!config || !config->dir) {
		recli_fprintf(recli_stderr, "No configuration directory\n");
		return -1;
	}

	if (!config->syntax) {
		snprintf(buffer, sizeof(buffer), "%s/syntax.txt", config->dir);
		if (syntax_parse_file(buffer, &(config->syntax)) < 0) {
			return -1;
		}

		if (recli_exec_syntax(&config->syntax, "shit") < 0) {
			exit(1);
		}
	}

	if (!config->help) {
		snprintf(buffer, sizeof(buffer), "%s/help.md", config->dir);

		if (stat(buffer, &statbuf) >= 0) {
			if (syntax_parse_help(buffer, &(config->help)) < 0) {
				return -1;
			}
		}
	}

	snprintf(buffer, sizeof(buffer), "%s/banner.txt", config->dir);
	if (stat(buffer, &statbuf) >= 0) {
		FILE *fp = fopen(buffer, "r");
		if (!fp) {
			recli_fprintf(recli_stderr, "Failed opening %s: %s\n",
				buffer, strerror(errno));
			return -1;
		}

		while (fgets(buffer, sizeof(buffer), fp)) {
			recli_fprintf(recli_stdout, "%s", buffer);
		}

		fclose(fp);
	}

	config->envp[0] = NULL;
	snprintf(buffer, sizeof(buffer), "%s/ENV", config->dir);
	if (stat(buffer, &statbuf) >= 0) {
		if (read_envp(buffer, config) < 0) {
			return -1;
		}
	}

	if (!config->permissions) {
		char *name = NULL;
		struct passwd *pwd;

		pwd = getpwuid(getuid());
		if (pwd) name = pwd->pw_name;

		if (!name) name = "DEFAULT";
		snprintf(buffer, sizeof(buffer), "%s/permission/%s.txt",
			 config->dir, name);
		if (stat(buffer, &statbuf) >= 0) {
			rcode =  permission_parse_file(buffer,
						       &config->permissions);
			if (rcode < 0) return -1;
			
			/*
			 *	Not allowed to do anything: exit.
			 */
			if (rcode == 0) exit(0);
		}
	}

	return 0;
}

static void nonblock(int fd)
{
#ifdef O_NONBLOCK
	/*
	 *	Try to set it non-blocking.
	 */
	int flags;
	
	if ((flags = fcntl(fd, F_GETFL, NULL)) < 0)  {
		return;
	}
	
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) {
		return;
	}
#endif
}


int recli_exec(const char *rundir, int argc, char *argv[], char *const envp[])
{
	int index = 0;
	int status;
	size_t out;
	int pd[2], epd[2];
	char *p, *q, buffer[8192];
	pid_t pid;
	struct stat sbuf;

	if (!rundir || (argc == 0)) return 0;

	out = snprintf(buffer, sizeof(buffer), "%s", rundir);

	if (stat(buffer, &sbuf) < 0) {
		recli_fprintf(recli_stderr, "Error reading rundir '%s': %s\n",
			rundir, strerror(errno));
		return -1;
	}

	p = q =  buffer + out;
	while (argc && ((sbuf.st_mode & S_IFDIR) != 0)) {
		out = snprintf(p, buffer + sizeof(buffer) - p, "/%s",
			       argv[index]);
		p += out;
		index++;

		if (stat(buffer, &sbuf) < 0) {
			snprintf(q, sizeof(buffer) - (q - buffer), "/run");
			if (stat(buffer, &sbuf) < 0) {
				for (index = 0; index < argc; index++) {
					recli_fprintf(recli_stdout, "%s ",
						      argv[index]);
				}
				recli_fprintf(recli_stdout, "\r\n");
				return -1;
			}

			index = 0;
			goto run;
		}
	}

	if (((sbuf.st_mode & S_IFDIR) != 0)) {
		recli_fprintf(recli_stderr, "Incompletely defined '%s'\n", buffer);
		return -1;
	}

run:
	argc -= index;
	argv += index;
	
	argv[argc] = NULL;
	memmove(argv + 1, argv, sizeof(argv[0]) * argc + 1);
	argv[0] = buffer;

	recli_fprintf(recli_stdout, "\r");

	if (pipe(pd) != 0) {
		recli_fprintf(recli_stderr, "Failed opening stdout pipe: %s\n",
			      strerror(errno));
		return -1;
	}

	if (pipe(epd) != 0) {
		close(pd[0]);
		close(pd[1]);
		recli_fprintf(recli_stderr, "Failed opening stderr pipe: %s\n",
			      strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid == 0) {		/* child */
		int devnull;

		devnull = open("/dev/null", O_RDWR);
		if (devnull < 0) {
			recli_fprintf(recli_stderr, "Failed opening /dev/null: %s\n",
				      strerror(errno));
			exit(1);
		}
		dup2(devnull, STDIN_FILENO);

		close(epd[0]);	/* reading FD */
		if (dup2(epd[1], STDERR_FILENO) != STDERR_FILENO) {
			recli_fprintf(recli_stderr, "Failed duping stderr: %s\n",
				      strerror(errno));
			exit(1);
		}

		close(pd[0]);	/* reading FD */
		if (dup2(pd[1], STDOUT_FILENO) != STDOUT_FILENO) {
			recli_fprintf(recli_stderr, "Failed duping stdout: %s\n",
				      strerror(errno));
			exit(1);
		}

		close(devnull);

		/*
		 *	FIXME: closefrom(3)
		 */

		if (!envp || !envp[0]) {
			execvp(buffer, argv);
		} else {
			execve(buffer, argv, envp);
		}
		exit(1);	/* if exec faild, exit. */
	}

	close(pd[1]);
	close(epd[1]);

	if (pid < 0) {
		close(pd[0]);
		close(epd[0]);

		recli_fprintf(recli_stderr, "Failed forking program: %s\n",
			      strerror(errno));
		return -1;
	}

	nonblock(pd[0]);
	nonblock(epd[0]);
      
	/*
	 *	Read from both pipes, printing one to stdout, and the
	 *	other to stderr.
	 */
	while ((pd[0] >= 0) && (epd[0] >= 0)) {
		int maxfd;
		ssize_t num;
		fd_set fds;

		FD_ZERO(&fds);
		if (pd[0] >= 0) FD_SET(pd[0], &fds);
		if (pd[0] >= 0) FD_SET(epd[0], &fds);

		maxfd = pd[0];
		if (maxfd < epd[0]) maxfd = epd[0];
		maxfd++;

		num = select(maxfd,  &fds, NULL, NULL, NULL);
		if (num == 0) break;
		if (num < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if ((pd[0] >= 0) && FD_ISSET(pd[0], &fds)) {
			num = read(pd[0], buffer, sizeof(buffer) - 1);
			if (num == 0) {
			close_stdout:
				close(pd[0]);
				pd[0] = -1;

			} else if (num < 0) {
				if (errno != EINTR) goto close_stdout;
				/* else ignore it */

			} else {
				buffer[num] = '\0';
				recli_fprintf(recli_stdout, "%s", buffer);
			}
		}

		if ((epd[0] >= 0) && FD_ISSET(epd[0], &fds)) {
			num = read(epd[0], buffer, sizeof(buffer) - 1);
			if (num == 0) {
			close_stderr:
				close(epd[0]);
				epd[0] = -1;

			} else if (num < 0) {
				if (errno != EINTR) goto close_stderr;
				/* else ignore it */

			} else {
				buffer[num] = '\0';
				recli_fprintf(recli_stderr, "%s", buffer);
			}
		}
	}

	waitpid(pid, &status, 0);
	recli_fprintf(recli_stdout, "\r");

	index = -1;
	if (WEXITSTATUS(status) == 0) {
		index = 0;
	}

	if (pd[0] >= 0) close(pd[0]);
	if (pd[0] >= 0)  close(epd[0]);

	return index;
}

