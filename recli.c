#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <sys/stat.h>
#include "recli.h"

static int in_string = 0;
static size_t string_start = 0;
static cli_syntax_t *syntax = NULL;
static cli_syntax_t *syntax_help = NULL;

static size_t ctx_buflen = 0;
static char ctx_buffer[1024];
static char ctx_mybuf[1024];

static int ctx2argv(char *buf, size_t len, int max_argc, char *argv[])
{
	if (!ctx_buflen) {
		return str2argv(buf, len, max_argc, argv);
	}	

	memcpy(ctx_mybuf, ctx_buffer, ctx_buflen);
	memcpy(ctx_mybuf + ctx_buflen, buf, len + 1);

	return str2argv(ctx_mybuf, ctx_buflen + len, max_argc, argv);
}


#ifndef NO_COMPLETION
void completion(const char *buf, linenoiseCompletions *lc)
{
	int i, num;
	char *tabs[256];

	if (in_string) return;
	
	num = syntax_tab_complete(syntax, buf, strlen(buf), 256, tabs);
	if (num == 0) return;
	
	for (i = 0; i < num; i++) {
		linenoiseAddCompletion(lc, tabs[i]);
		free(tabs[i]);
	}
}
#endif

int foundspace(const char *buf, size_t len, char c)
{
	if (in_string) return 0;
	
	if (len == 0) return 1;
	
	if (buf[len -1] == c) return 1;
	
	return 0;
}

int escapedquote(const char *start)
{
	while (*start) {
		if (*start == '\\') {
			if (!start[1]) return 1;
			start += 2;
		}
		start++;
	}
	return 0;
}


int foundquote(const char *buf, size_t len, char c)
{
	if (!in_string) {
		in_string = 1;
		string_start = len;
		return 0;
	}
	
	if (buf[string_start] != c) return 0;
	
	if (escapedquote(buf + string_start)) return 0;
	
	in_string = 0;
	string_start = 0;
	
	return 0;
}

int foundhelp(const char *buf, size_t len, char c)
{
	int argc;
	char *argv[256];
	const char *help;
	cli_syntax_t *match;
	char mybuf[1024];
	
	c = c;			/* -Wunused */
	
	if (in_string) return 0;

	if (len == 0) {
		printf("\r\n");
		syntax_print_lines(syntax);
		return 1;
	}
	
	memcpy(mybuf, buf, len + 1);
	argc = ctx2argv(mybuf, len, 256, argv);
	printf("?\r\n");
	if (argc == 0) {
		syntax_print_lines(syntax);

	} else {
		match = syntax_match_max(syntax, argc, argv);
		if (!match) {
			printf("NO MATCH\t\n");
			
		} else {
			syntax_printf(match);
			syntax_free(match);
			help = syntax_show_help(syntax_help, argc, argv);
			if (!help) {
				printf("\r\n");
			} else {
				printf(":\r\n%s", help);
			}
		}
	}
	
	return 1;
}

static void runcmd(const char *rundir, int argc, char *argv[])
{
	size_t out;
	char *p, buffer[8192];
	struct stat sbuf;

	if (!rundir || (argc == 0)) return;

	out = snprintf(buffer, sizeof(buffer), "%s", rundir);

	if (stat(buffer, &sbuf) < 0) {
		fprintf(stderr, "Error reading rundir '%s': %s\n",
			rundir, strerror(errno));
		return;
	}

	p = buffer + out;
	while (argc && ((sbuf.st_mode & S_IFDIR) != 0)) {
		out = snprintf(p, buffer + sizeof(buffer) - p, "/%s", argv[0]);
		p += out;
		argc--;
		argv++;

		if (stat(buffer, &sbuf) < 0) {
			fprintf(stderr, "Error reading rundir '%s': %s\n",
				buffer, strerror(errno));
			return;
		}
	}

	if (((sbuf.st_mode & S_IFDIR) != 0)) {
		fprintf(stderr, "Incompletely defined '%s'\n", buffer);
		return;
	}

	fprintf(stderr, "RUNNING CMD %s %d\n", buffer, argc);

	argv[argc] = NULL;
	memmove(argv + 1, argv, sizeof(argv[0]) * argc + 1);
	argv[0] = buffer;

	if (fork() == 0) {
		execvp(buffer, argv);
	}

	/* FIXME: set line buf stdout normal, wait for child */
}

static const char *spaces = "                                                                                                                                                                                                                                                                ";

int main(int argc, char **argv)
{
	int c, my_argc;
	const char *baseprompt = "recli> ";
	const char *prompt = baseprompt;
	const char *rundir = NULL;
	int quit = 0;
	int context = 1;
	char *line;
	char *my_argv[128];
	int tty = 1;
	char mybuf[1024];

#ifndef NO_COMPLETION
	linenoiseSetCompletionCallback(completion);
#endif
	linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
	linenoiseSetCharacterCallback(foundspace, ' ');
	linenoiseSetCharacterCallback(foundquote, '"');
	linenoiseSetCharacterCallback(foundquote, '\'');
	linenoiseSetCharacterCallback(foundhelp, '?');

	while ((c = getopt(argc, argv, "H:p:qr:s:P:X:")) != EOF) switch(c) {
		case 'H':
			if (syntax_parse_help(optarg, &syntax_help) < 0) exit(1);
			break;

		case 'p':
			if (permission_parse_file(optarg) < 0) exit(1);
			break;

		case 'q':
			quit = 1;
			break;

		case 'r':
			rundir = optarg;
			break;

		case 's':
			if (syntax_parse_file(optarg, &syntax) < 0) exit(1);
			break;

		case 'P':
			baseprompt = prompt = optarg;
			break;
		    
		case 'X':
			if (strcmp(optarg, "syntax") == 0) {
				syntax_printf(syntax);printf("\r\n");
			}
			break;
		    
		default:
			fprintf(stderr, "Usage: cli [-s syntax] [-X syntax]\n");
			exit(1);
		}

	argc -= (optind - 1);
	argv += (optind - 1);

	if (!isatty(STDIN_FILENO)) {
		baseprompt = prompt = "";
		context = 0;
		tty = 0;
	}

	if (quit) goto done;

	while((line = linenoise(prompt)) != NULL) {
		int runit = 1;
		const char *fail;

		if (line[0] != '\0') {
			size_t mylen = strlen(line);

			if (context && (strcmp(line, "end") == 0)) {
				ctx_buflen = 0;
				ctx_buffer[0] = '\0';
				prompt = baseprompt;
				goto next_line;
			}

			memcpy(mybuf, line, mylen + 1);

			my_argc = ctx2argv(mybuf, mylen, 128, my_argv);
			if (my_argc < 0) {
				c = -1;

			} else {
				c = syntax_check(syntax, my_argc, my_argv, &fail);
				if ((c == 0) && !context) c = -1;
				
				if (c == 0) {
					strcat(ctx_buffer, line);
					ctx_buflen = strlen(ctx_buffer);
					ctx_buffer[ctx_buflen] = ' ';
					ctx_buflen++;
					prompt = "recli ...> ";
					goto next_line;
				}

				/*
				 *	We return the error based on
				 *	what the user entered, not on
				 *	what we synthesized from the
				 *	context.
				 */
				if (ctx_buflen) {
					fail += ctx_buflen + 1;
				}

			}
				
			if (c < 0) {
				fprintf(stderr, "%s\n", line);
				if (fail &&
				    (fail - my_argv[0]) < sizeof(spaces)) {
					fprintf(stderr, "%.*s^ - Invalid input\n",
						(int) (fail - my_argv[0]), spaces);
				} else {
					fprintf(stderr, "^ - Invalid input\n");
				}

				runit = 0;
				goto add_line;
			}

			if (!permission_enforce(my_argc, my_argv)) {
				fprintf(stderr, "%s\n", line);
				fprintf(stderr, "^ - No permission\n");
				runit = 0;
				goto add_line;
			}

			printf("%s%s\n", ctx_buffer, line);

		add_line:
			linenoiseHistoryAdd(line);
			linenoiseHistorySave("history.txt"); /* Save every new entry */

			if (runit && rundir) runcmd(rundir, my_argc, my_argv);
		}
	next_line:
		free(line);
	}

done:
	if (syntax_help) syntax_free(syntax_help);
	syntax_free(NULL);

	return 0;
}
