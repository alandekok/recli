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

static recli_config_t config = {
	.dir = NULL,
	.prompt = "recli> ",
	.banner = NULL,
	.syntax = NULL,
	.help = NULL,
	.permissions = NULL
};
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
	
	num = syntax_tab_complete(config.syntax, buf, strlen(buf), 256, tabs);
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
		syntax_print_lines(config.syntax);
		return 1;
	}
	
	memcpy(mybuf, buf, len + 1);
	argc = ctx2argv(mybuf, len, 256, argv);
	printf("?\r\n");
	if (argc == 0) {
		syntax_print_lines(config.syntax);

	} else {

		match = syntax_match_max(config.syntax, argc, argv);
		if (!match) {
			printf("NO MATCH\t\n");
			
		} else {
			syntax_free(match);
			syntax_print_context_help(config.help, argc, argv);
		}
	}
	
	return 1;
}

static void runcmd(const char *rundir, int argc, char *argv[])
{
	int index = 0;
	size_t out;
	char *p, *q, buffer[8192];
	struct stat sbuf;

	if (!rundir || (argc == 0)) return;

	out = snprintf(buffer, sizeof(buffer), "%s", rundir);

	if (stat(buffer, &sbuf) < 0) {
		fprintf(stderr, "Error reading rundir '%s': %s\n",
			rundir, strerror(errno));
		return;
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
				return;
			}

			index = 0;
			goto run;
		}
	}

	if (((sbuf.st_mode & S_IFDIR) != 0)) {
		fprintf(stderr, "Incompletely defined '%s'\n", buffer);
		return;
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
}

static int do_help(char *buffer, size_t len)
{
	int my_argc;
	char *my_argv[128];

	if (strcmp(buffer, "help syntax") == 0) {
		cli_syntax_t *match;

		my_argc = str2argv(ctx_buffer, ctx_buflen, 128, my_argv);
		if (my_argc < 0) return -1;

		match = syntax_match_max(config.syntax,
					 my_argc, my_argv);
		if (match) {
			syntax_print_lines(match);
			syntax_free(match);
		}
		return 1;
	}

	if ((strcmp(buffer, "help") == 0) ||
	    (strncmp(buffer, "help ", 5) == 0)) {
		const char *help = NULL;

		my_argc = ctx2argv(buffer + 4, len - 4, 128, my_argv);
		if (my_argc < 0) return -1;
				
		help = syntax_show_help(config.help,
					my_argc, my_argv, 1);
		if (!help) {
			printf("\r\n");
		} else {
			printf("%s", help);
		}
		return 1;
	}

	return 0;		/* not help */
}


static const char *spaces = "                                                                                                                                                                                                                                                                ";

int main(int argc, char **argv)
{
	int c, my_argc;
	const char *prompt = config.prompt;
	int quit = 0;
	int context = 1;
	char *line;
	char *my_argv[128];
	int tty = 1;
	int debug_syntax = 0;
	char mybuf[1024];

#ifndef NO_COMPLETION
	linenoiseSetCompletionCallback(completion);
#endif
	linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
	linenoiseSetCharacterCallback(foundspace, ' ');
	linenoiseSetCharacterCallback(foundquote, '"');
	linenoiseSetCharacterCallback(foundquote, '\'');
	linenoiseSetCharacterCallback(foundhelp, '?');

	while ((c = getopt(argc, argv, "d:H:p:qr:s:P:X:")) != EOF) switch(c) {
		case 'd':
			config.dir = optarg;
			break;

		case 'H':
			if (syntax_parse_help(optarg, &config.help) < 0) exit(1);
			break;

		case 'p':
			if (permission_parse_file(optarg, &config.permissions) < 0) exit(1);
			break;

		case 'q':
			quit = 1;
			break;

		case 's':
			if (syntax_parse_file(optarg, &config.syntax) < 0) exit(1);
			break;

		case 'P':
			config.prompt = prompt = optarg;
			break;
		    
		case 'X':
			if (strcmp(optarg, "syntax") == 0) {
				debug_syntax = 1;
			}
			break;
		    
		default:
			fprintf(stderr, "Usage: cli [-s syntax] [-X syntax]\n");
			exit(1);
		}

	argc -= (optind - 1);
	argv += (optind - 1);

	if (!isatty(STDIN_FILENO)) {
		config.prompt = prompt = "";
		context = 0;
		tty = 0;
	}

	if (config.dir) {
		if (recli_bootstrap(&config) < 0) {
			exit(1);
		}
	}

	if (debug_syntax) {
		syntax_printf(config.syntax);printf("\r\n");
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
				prompt = config.prompt;
				goto next_line;
			}

			memcpy(mybuf, line, mylen + 1);

			/*
			 *	Look for "help" BEFORE splitting the
			 *	line.  This is so that we can do help
			 *	when the user has a context.
			 */
			if (config.help) {
				c = do_help(mybuf, mylen);
				if (c < 0) goto show_error;
				if (c == 1) {
					runit = 0;
					goto add_line;
				}
			}

			my_argc = ctx2argv(mybuf, mylen, 128, my_argv);
			if (my_argc < 0) {
				c = -1;
				goto show_error;
			}

			c = syntax_check(config.syntax, my_argc, my_argv, &fail);
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
			
		show_error:			
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

			if (!permission_enforce(config.permissions, my_argc, my_argv)) {
				fprintf(stderr, "%s\n", line);
				fprintf(stderr, "^ - No permission\n");
				runit = 0;
				goto add_line;
			}

			if (!config.dir) printf("%s%s\n", ctx_buffer, line);

		add_line:
			linenoiseHistoryAdd(line);
			linenoiseHistorySave("history.txt"); /* Save every new entry */

			if (runit && config.dir) runcmd(config.dir, my_argc, my_argv);
		}
	next_line:
		free(line);
	}

done:
	if (config.help) syntax_free(config.help);
	syntax_free(config.syntax);
	permission_free(config.permissions);

	syntax_free(NULL);

	return 0;
}
