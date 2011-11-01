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
#include <assert.h>

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include "recli.h"

int recli_bootstrap(recli_config_t *config)
{
	int rcode;
	struct stat statbuf;
	char buffer[8192];

	if (!config || !config->dir) {
		fprintf(stderr, "No configuration directory\n");
		return -1;
	}

	if (!config->syntax) {
		snprintf(buffer, sizeof(buffer), "%s/syntax.txt", config->dir);
		if (syntax_parse_file(buffer, &(config->syntax)) < 0) {
			return -1;
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
			fprintf(stderr, "Failed opening %s: %s\n",
				buffer, strerror(errno));
			return -1;
		}

		while (fgets(buffer, sizeof(buffer), fp)) {
			printf("%s", buffer);
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

int recli_exec(const char *rundir, int argc, char *argv[])
{
	int index = 0;
	size_t out;
	char *p, *q, buffer[8192];
	struct stat sbuf;

	if (!rundir || (argc == 0)) return 0;

	out = snprintf(buffer, sizeof(buffer), "%s", rundir);

	if (stat(buffer, &sbuf) < 0) {
		fprintf(stderr, "Error reading rundir '%s': %s\n",
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
					printf("%s ", argv[index]);
				}
				printf("\r\n");
				return -1;
			}

			index = 0;
			goto run;
		}
	}

	if (((sbuf.st_mode & S_IFDIR) != 0)) {
		fprintf(stderr, "Incompletely defined '%s'\n", buffer);
		return -1;
	}

run:
	argc -= index;
	argv += index;
	
	argv[argc] = NULL;
	memmove(argv + 1, argv, sizeof(argv[0]) * argc + 1);
	argv[0] = buffer;

	printf("\r");

	if (fork() == 0) {
		execvp(buffer, argv);
	}

	waitpid(-1, NULL, 0);
	printf("\r");

	return 0;
}

