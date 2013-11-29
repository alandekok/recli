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
#include <fcntl.h>
#include <dirent.h>
#include <assert.h>

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include "recli.h"

static int load_envp(const char *dir, recli_config_t *config)
{
	int argc;
	FILE *fp;
	char buffer[8192];

	snprintf(buffer, sizeof(buffer), "%s/ENV", dir);
	fp = fopen(buffer, "r");
	if (!fp) {
		if (errno == ENOENT) return 0;
		return -1;
	}

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

	snprintf(buffer, sizeof(buffer), "RECLI_DIR=%s", dir);
	config->envp[argc++] = strdup(buffer);
	config->envp[argc] = NULL;


	fclose(fp);
	return 0;  
}


typedef struct rbuf_t {
	char buffer[8192];
	char *start, *append;
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
	rcode = vsnprintf(b->append, b->buffer + sizeof(b->buffer) - b->append,
			  fmt, args);
	va_end(args);

	/*
	 *	If we're called for stderr, dump it to the caller.
	 */
	if (b == &buf_err) {
		return b->old_fprintf(b->old_ctx, "%s", b->buffer);
	}

	for (p = b->append; *p != '\0'; p++) {
		if (*p < ' ') {
			*p = '\0';
			break;
		}

		if (!isspace((int) *p)) break;
	}

	if (*p == '-') *p = '\0';


	/*
	 *	Errors go to the caller, not to us.
	 */
	recli_fprintf = b->old_fprintf;
	recli_stdout = buf_out.old_ctx;
	recli_stderr = buf_err.old_ctx;

	if (syntax_merge(b->phead, b->start) < 0) {
		b->rcode = -1;
	}

	recli_fprintf = recli_fprintf_syntax;
	recli_stdout = &buf_out;
	recli_stderr = &buf_err;

	return rcode;
}


int recli_exec_syntax(cli_syntax_t **phead, const char *dir, char *program,
		      char *const envp[])
{
	int rcode = 0;
	char *p;
	char *argv[4];

	argv[0] = program;
	argv[1] = "--config";
	argv[2] = "syntax";
	argv[3] = NULL;

	buf_out.old_fprintf = recli_fprintf;
	buf_out.old_ctx = recli_stdout;
	buf_out.rcode = 0;
	buf_out.phead = phead;
	buf_out.append = buf_out.buffer;

	buf_err.old_fprintf = recli_fprintf;
	buf_err.old_ctx = recli_stderr;
	buf_err.rcode = 0;
	buf_err.phead = NULL;
	buf_err.append = buf_err.buffer;

	recli_fprintf = recli_fprintf_syntax;
	recli_stdout = &buf_out;
	recli_stderr = &buf_err;

	strlcpy(buf_out.buffer, program, sizeof(buf_out.buffer));
	for (p = buf_out.buffer; *p; p++) {
		if (*p == '/') {
			*p = ' ';
		}
	}
	p[0] = ' ';
	p[1] = '\0';

	buf_out.append = p + 1;

	buf_out.start = buf_out.buffer;
	if (strncmp(buf_out.buffer, "DEFAULT ", 8) == 0) {
		buf_out.start += 8;
	}

	/*
	 *	Now that we have the command prefix parsed out, ensure
	 *	that the executable we run doesn't have '/' in it.
	 */
	p = strchr(argv[0], '/');
	if (p) argv[0] = p + 1;

	rcode = recli_exec(dir, 3, argv, envp);

	recli_fprintf = buf_out.old_fprintf;
	recli_stdout = buf_out.old_ctx;
	recli_stderr = buf_err.old_ctx;

	return rcode;
}


int recli_load_dirs(cli_syntax_t **phead, const char *name, size_t skip,
		    char *const envp[])
{
	struct dirent *dp;
	DIR *dir;
	struct stat s;
	char *p;
	char buffer[8192];

	dir = opendir(name);
	if (!dir) {
		recli_fprintf(recli_stderr, "Failed opening %s: %s\n",
			      name, strerror(errno));
		return -1;
	}

	while ((dp = readdir(dir)) != NULL) {
		if (dp->d_name[0] == '.') continue;

		snprintf(buffer, sizeof(buffer), "%s/%s", name, dp->d_name);
		
		if (stat(buffer, &s) != 0) continue;

		if (S_ISDIR(s.st_mode)) {
			recli_load_dirs(phead, buffer, skip, envp);
			continue;
		}

		if (!(S_IFREG & s.st_mode) ||
		    !(S_IXUSR & s.st_mode)) continue;

		p = strchr(dp->d_name, '~');
		if (p) continue;

		recli_exec_syntax(phead, name, buffer + skip + 1,
				  envp); /* ignore errors */
	}

	closedir(dir);
	return 0;
}


/*
 *	Load a (possibly cached) syntax.  If the cache exists, use it
 *	in preference to anything else.
 *
 *	We remember the INODE of the cached file instead of the
 *	modification timestamp.  This is because there may be multiple
 *	people using the same CLI.  If one updates the syntax, we want
 *	the other one to see only the finished new version, and not
 *	any intermediate version.  This requirement means that
 *	updating the syntax has to be done as an atomic operation, i.e.
 *
 *		$ ./bin/rehash > ./cache/syntax.txt.new
 *		$ mv ./cache/syntax.txt.new ./cache/syntax.txt
 */
int recli_load_syntax(recli_config_t *config)
{
	struct stat statbuf;
	cli_syntax_t *head = NULL;
	char buffer[8192];

	snprintf(buffer, sizeof(buffer), "%s/cache/syntax.txt", config->dir);
	if (stat(buffer, &statbuf) == 0) {
		if (config->syntax_inode == statbuf.st_ino) return 0;

		if (syntax_parse_file(buffer, &head) < 0) return -1;

		config->syntax_inode = statbuf.st_ino;
	} else {
		snprintf(buffer, sizeof(buffer), "%s/bin/", config->dir);

		if (recli_load_dirs(&head, buffer, strlen(buffer),
			    config->envp) < 0) return -1;

		/*
		 *	FIXME: dump syntax to syntax.cache file, and
		 *	update cached inode.
		 */
	}

	if (config->syntax) syntax_free(config->syntax);
	config->syntax = head;

	return 0;
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

	config->envp[0] = NULL;
	if (load_envp(config->dir, config) < 0) {
		return -1;
	}

	recli_datatypes_init();

	if (recli_load_syntax(config) < 0) return -1;

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
	char *my_argv[256];

	if (!rundir || (argc == 0)) return 0;

	out = snprintf(buffer, sizeof(buffer), "%s/", rundir);

	if (stat(buffer, &sbuf) < 0) {
		recli_fprintf(recli_stderr, "Error reading '%s': %s\n",
			buffer, strerror(errno));
		return -1;
	}

	p = q =  buffer + out;
	while (argc && ((sbuf.st_mode & S_IFDIR) != 0)) {
		out = snprintf(p, buffer + sizeof(buffer) - p, "/%s",
			       argv[index]);
		p += out;
		index++;

		if (stat(buffer, &sbuf) < 0) {
			snprintf(q, sizeof(buffer) - (q - buffer), "/DEFAULT");
			if (stat(buffer, &sbuf) < 0) {
				for (index = 0; index < argc; index++) {
					recli_fprintf(recli_stdout, "%s ",
						      argv[index]);
				}
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
	my_argv[0] = buffer;
	memcpy(&my_argv[1], &argv[index], sizeof(argv[0]) * (argc - index));
	my_argv[argc - index + 1] = NULL;

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
			execvp(buffer, my_argv);
		} else {
			execve(buffer, my_argv, envp);
		}
		fprintf(stderr, "Failed running %s: %s\n",
			buffer, strerror(errno));
		exit(1);	/* if exec faild, exit. */
	}

	assert(pd[1] >= 0);
	assert(epd[1] >= 0);
	close(pd[1]);
	close(epd[1]);

	if (pid < 0) {
		assert(pd[0] >= 0);
		assert(epd[0] >= 0);
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
				assert(pd[0] >= 0);
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
				assert(epd[0] >= 0);
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

	index = -1;
	if (WEXITSTATUS(status) == 0) {
		index = 0;
	}

	if (pd[0] >= 0) close(pd[0]);
	if (pd[0] >= 0)  close(epd[0]);

	return index;
}

