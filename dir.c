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

		if (name) {
			snprintf(buffer, sizeof(buffer), "%s/permission/%s.txt",
				 config->dir, name);
			if ((stat(buffer, &statbuf) >= 0) &&
			    (permission_parse_file(buffer, &config->permissions) < 0)) {
				return -1;
			}
		} /* else ignore any permissions */
	}

	return 0;
}
