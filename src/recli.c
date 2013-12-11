#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <unistd.h>
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

typedef struct ctx_stack_t {
	size_t len;
	char   buffer[256];
} ctx_stack_t;

#define CTX_STACK_MAX (32)

static int ctx_stack_ptr = 0;
static ctx_stack_t ctx_stack[CTX_STACK_MAX];

static char ctx_mybuf[8192];

static int ctx2argv(char *buf, size_t len, int max_argc, char *argv[])
{
	int i;
	char *p;

	if (!ctx_stack_ptr) {
		return str2argv(buf, len, max_argc, argv);
	}	

	p = ctx_mybuf;
	for (i = 0; i < ctx_stack_ptr; i++) {
		memcpy(p, ctx_stack[i].buffer, ctx_stack[i].len);
		p += ctx_stack[i].len;
	}

	memcpy(p, buf, len + 1);
	p += len;

	return str2argv(ctx_mybuf, p - ctx_mybuf, max_argc, argv);
}


#ifndef NO_COMPLETION
void completion(const char *buf, linenoiseCompletions *lc)
{
	int i, num;
	char *tabs[256];
	size_t offset = 0;
	char buffer[1024];

	if (in_string) return;

	if (ctx_stack_ptr > 0) {
		char *p = buffer;

		for (i = 0; i < ctx_stack_ptr; i++) {
			strlcpy(p, ctx_stack[i].buffer,
				sizeof(buffer) - (p - buffer));
			p += strlen(p);
		}

		strlcpy(p, buf, sizeof(buffer) - (p - buffer));
		offset = p - buffer;
		buf = buffer;
	}
	
	num = syntax_tab_complete(config.syntax, buf, strlen(buf), 256, tabs);
	if (num == 0) return;
	
	for (i = 0; i < num; i++) {
		linenoiseAddCompletion(lc, tabs[i] + offset);
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
	cli_syntax_t *match;
	char mybuf[1024];
	
	c = c;			/* -Wunused */
	
	if (in_string) return 0;

	if (len == 0) {
		printf("?\r\n");
		if (ctx_stack_ptr == 0) {
			syntax_print_lines(config.syntax);
			return 1;
		}

		memcpy(mybuf, buf, len + 1);
		argc = ctx2argv(mybuf, len, 256, argv);

		match = syntax_match_max(config.syntax, argc, argv);
		if (!match) return 1;

		syntax_print_lines(match);
		syntax_free(match);
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

static int do_help(char *buffer, size_t len)
{
	int my_argc;
	char *my_argv[128];

	if (strcmp(buffer, "help syntax") == 0) {
		cli_syntax_t *match;

		my_argc = ctx2argv(buffer, len, 128, my_argv);
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
			recli_fprintf_words(recli_stdout, "%s", help);
		}
		return 1;
	}

	return 0;		/* not help */
}


static const char *spaces = "                                                                                                                                                                                                                                                                ";

int main(int argc, char **argv)
{
	int c, rcode, my_argc;
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

	recli_stdout = stdout;
	recli_stderr = stderr;

	while ((c = getopt(argc, argv, "d:H:p:qr:s:P:X:")) != EOF) switch(c) {
		case 'd':
			config.dir = optarg;
			break;

		case 'H':
			if (syntax_parse_help(optarg, &config.help) < 0) exit(1);
			break;

		case 'p':
			rcode = permission_parse_file(optarg, &config.permissions);
			if (rcode < 0) exit(1);
			if (rcode == 0) exit(0);
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
			fprintf(stderr, "Usage: recli [-s syntax] [-X syntax]\n");
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

			if (context)  {
				if (strcmp(line, "exit") == 0) {
					if (ctx_stack_ptr == 0) {
						exit(0);
					}

					ctx_stack_ptr--;
					if (ctx_stack_ptr == 0) {
						prompt = config.prompt;
					}
					goto next_line;
				}

				if (strcmp(line, "end") == 0) {
					ctx_stack_ptr = 0;
					prompt = config.prompt;
					goto next_line;
				}

				if ((strcmp(line, "quit") == 0) ||
				    (strcmp(line, "logout") == 0)) {
					exit(0);
				}
			}

			if (mylen >= sizeof(mybuf)) {
				fprintf(stderr, "line too long\r\n");
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
			if ((c < 0) && (ctx_stack_ptr > 0)) {
				int argc2;
				char *argv2[128];
				const char *fail2;
				char buf2[4096];

				memcpy(buf2, line, mylen + 1);
				argc2 = str2argv(buf2, mylen, 128, argv2);
				c = syntax_check(config.syntax, argc2, argv2,
						 &fail2);
				if (c <= 0) {
					c = -1;
					goto show_error;
				}
				memcpy(my_argv, argv2, sizeof(argv2[0]) * argc2);
				my_argc = argc2;
				c = 0;
				goto show_error;
			}


			if ((c == 0) && !context) c = -1;
			
			if (c == 0) {
				if (ctx_stack_ptr == CTX_STACK_MAX) {
					runit = 0;
					c = -1;
					goto add_line;
				}

				if (!permission_enforce(config.permissions, my_argc, my_argv)) {
					fprintf(stderr, "%s\n", line);
					fprintf(stderr, "^ - No permission\n");
					runit = 0;
					goto add_line;
				}

				ctx_stack[ctx_stack_ptr].len = strlen(line);
				if (ctx_stack[ctx_stack_ptr].len + 2 >=
				    sizeof(ctx_stack[ctx_stack_ptr].buffer)) {
					runit = 0;
					c = -1;
					goto add_line;
				}

				memcpy(ctx_stack[ctx_stack_ptr].buffer, line,
				       ctx_stack[ctx_stack_ptr].len);

				memcpy(ctx_stack[ctx_stack_ptr].buffer + 
				       ctx_stack[ctx_stack_ptr].len, " ", 2);
				ctx_stack[ctx_stack_ptr].len++;
				prompt = "recli ...> ";
				ctx_stack_ptr++;
				goto next_line;
			}
			
			/*
			 *	We return the error based on
			 *	what the user entered, not on
			 *	what we synthesized from the
			 *	context.
			 */
			if (ctx_stack_ptr) {
				int i;

				for (i = 0; i < ctx_stack_ptr; i++) {
					fail += ctx_stack[i].len;
				}
			}
			
		show_error:			
			if (c < 0) {
				fprintf(stderr, "%s\n", line);
				if (fail &&
				    ((size_t) (fail - my_argv[0]) < sizeof(spaces))) {
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

			if (!config.dir) {
				int i;

				for (i = 0; i < ctx_stack_ptr; i++) {
					printf("%s", ctx_stack[i].buffer);
				}
				printf("%s\n", line);
			}

		add_line:
			linenoiseHistoryAdd(line);
			linenoiseHistorySave("history.txt"); /* Save every new entry */

			if (runit && config.dir) {
				char buffer[8192];

				snprintf(buffer, sizeof(buffer), "%s/bin/",
					 config.dir);

				recli_exec(buffer, my_argc, my_argv,
					   config.envp);
				recli_load_syntax(&config);
			}
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
