#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "recli.h"


ssize_t strquotelen(const char *str)
{
	char c = *str;
	const char *p = str;

	p++;
	while (*p) {
		if (*p == '\\') {
			if (!p[1]) return -1;
			p += 2;
			continue;
		}
		if (*p == c) break;
		
		p++;
	}
	
	if (*p != c) return -1;
	p++;

	return p - str;
}

int str2argv(char *buf, size_t len, int max_argc, char *argv[])
{
	int argc;
	char *p;

	if ((len == 0) || (max_argc == 0)) return 0;

	while (isspace((int) *buf)) buf++;

	if (!*buf || (*buf == ';') || (*buf == '#')) return 0;

	argc = 0;
	p = buf;

	while (*p && (argc < max_argc)) {
		while (isspace((int) *p)) if (argv) *(p++) = '\0';

		if (!*p) return argc;

		if ((*p == ';') || (*p == '#')) return argc;

		/*
		 *	String: treat it as one block
		 */
		if ((*p == '"') || (*p == '\'')) {
			ssize_t quote;

			quote = strquotelen(p);
			if (quote < 0) return -1;

			if (p[quote] && !isspace((int) p[quote])) {
				return -1;
			}

			if (argv) {
				argv[argc++] = p;
				p += quote;

				if (!*p) return argc;
				*(p++) = '\0';
				continue;
			}

			argc++;
			continue;
		}

		/*
		 *	Anything else: must be a word all by itself.
		 */

		if (argv) argv[argc] = p;
		argc++;

		while (*p && (*p != '"') && (*p != '\'') &&
		       !isspace((int) *p)) {
			p++;
		}

		if (!*p) return argc;

		if (!isspace((int) *p)) return -1;

	}

	return -1;
}

void print_argv(int argc, char *argv[])
{
	int i;

	if (argc == 0) return;
	
	for (i = 0; i < argc; i++) {
		printf("[%d] '%s'\r\n", i, argv[i]);
	}
}

void print_argv_string(int argc, char *argv[])
{
	int i;

	if (argc == 0) return;
	
	for (i = 0; i < argc; i++) {
		printf("%s ", argv[i]);
	}
}

static size_t linelen(const char *buffer, int cols)
{
	const char *p;

	p = buffer;

	/*
	 *	Step 1: go up "cols" characters
	 */
	while (*p) {
		if ((p - buffer) >= cols) break;
		p++;
	}

	if (!*p) return p - buffer; /* short */

	/*
	 *	Step 2: back up to the previous space
	 */
	while ((*p != ' ') && (p >= buffer)) p--;

	if (p > buffer) return p - buffer;

	/*
	 *	No previous space, print the entire word.
	 */
	while (*p && (*p != ' ')) p++;

	return p - buffer;
}

int recli_fprintf_words(void *ctx, const char *fmt, ...)
{
	int cols = linenoiseCols();
	size_t len = 0;
	va_list args;
	char *p, buffer[8192];

	va_start(args, fmt);

	if (cols <= 0) cols = 80;

	vsnprintf(buffer, sizeof(buffer), fmt, args);

	p = buffer;
	while (*p) {
		len = linelen(p, cols - 1);
		syntax_fprintf(ctx, "%.*s\r\n", len, p);
		p += len;
		while (*p == ' ') p++;
	}

	va_end(args);

	return len;
}
