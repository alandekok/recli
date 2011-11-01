/**
 * Permission parsing and validation functions
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
 * See LICENCE for licence details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "recli.h"

struct cli_permission_t {
	int allowed;
	int lineno;
	int argc;
	struct cli_permission_t *next;

	char *input_line;
	const char *argv[1];
};

int permission_enforce(cli_permission_t *head, int argc, char *argv[])
{
	int i;
	cli_permission_t *this;

	if (!head || (argc == 0)) return 1;

	for (this = head; this != NULL; this = this->next) {
		int match = 1;

		for (i = 0; i < this->argc; i++) {
			if (i > argc) {
				break;
			}

			if (strcmp(this->argv[i], "*") == 0) {
				continue;
			}

			if (strcmp(this->argv[i], argv[i]) != 0) {
				match = 0;
				break;
			}
		}

		if (!match) {
			continue;
		}

		if (!this->allowed) {
#if 0
			recli_fprintf(recli_stdout, ": Disallowed by rule %d",
				this->lineno);
			fflush(stdout);
#endif
			return 0;
		}

		return 1;
	}

	return 1;
}


static cli_permission_t *permission_parse_line(const char *buf)
{
	size_t len;
	cli_permission_t *this;
	int argc;
	char *argv[256];
	char *buffer;

	len = strlen(buf);
	if (len == 0) return NULL;

	buffer = malloc(len + 1);
	memcpy(buffer, buf, len + 1);
	argc = str2argv(buffer, len, 256, argv);
	if (argc == 0) {
		free(buffer);
		return NULL;
	}

	this = calloc(sizeof(*this) + (sizeof(this->argv[0]) * (argc - 1)), 1);
	this->allowed = 1;
	this->input_line = buffer;

	if (argv[0][0] == '!') {
		this->allowed = 0;
		argv[0]++;
	}

	this->argc = argc;
	memcpy(this->argv, argv, sizeof(argv[0]) * argc);
	memset(argv, 0, sizeof(argv[0]) * argc);

	return this;
}

int permission_parse_file(const char *filename, cli_permission_t **phead)
{
	int lineno;
	FILE *fp;
	char buffer[1024];
	cli_permission_t *head, *this, **last;

	if (!filename || !phead) return -1;

	fp = fopen(filename, "r");
	if (!fp) {
		recli_fprintf(recli_stderr, "Failed opening %s: %s\n",
			filename, strerror(errno));
		return -1;
	}

	head = NULL;
	last = &head;
	lineno = 0;

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char *p;
		lineno++;

		p = strchr(buffer, '\r');
		if (p) *p = '\0';

		p = strchr(buffer, '\n');
		if (p) *p = '\0';

		this = permission_parse_line(buffer);
		if (!this) continue;
		this->lineno = lineno;
		*last = this;
		last = &this->next;
	}

	fclose(fp);
	*phead = head;

	/*
	 *	Not allowed to do anything.
	 */
	if (!head->next && !head->allowed && (head->argc == 1) &&
	    (strcmp(head->argv[0], "*") == 0)) {
		return 0;
	}

	return 1;
}

void permission_free(cli_permission_t *head)
{
	cli_permission_t *this, *next;

	if (!head) return;

	for (this = head; this != NULL; this = next) {
		next = this->next;

		free(this->input_line);
		memset(this, 0, sizeof(*this));
		free(this);
	}
}
