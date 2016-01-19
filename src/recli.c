#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include "recli.h"

static int in_string = 0;
static size_t string_start = 0;

static recli_config_t config = {
	.dir = NULL,
	.prompt = NULL,
	.banner = NULL,
	.syntax = NULL,
	.help = NULL,
	.permissions = NULL
};


/*
 *	Admins can type a partial command, in which case it's put on
 *	the stack, and the prompt changes to include the partial
 *	command.  We want to track these partial commands, but also
 *	limit the size of the stack.
 */
typedef struct ctx_stack_t {
	size_t len;
	char   buffer[256];
} ctx_stack_t;

#define CTX_STACK_MAX (32)

static char ctx_line_full[8192];
static char *ctx_line_end = ctx_line_full;

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

extern pid_t child_pid;

static void catch_sigquit(int sig)
{
	if (child_pid > 1) {
		kill(child_pid, sig);
	}

	/*
	 *	Else ignore it.
	 */
}

static int set_signal(int sig, sig_t func)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	act.sa_handler = func;

	return sigaction(sig, &act, NULL);
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

/*
 *	Callback from linenoise when '?' is pressed.
 */
static int foundhelp(const char *buf, size_t len, UNUSED char c)
{
	int argc;
	char *argv[256];
	cli_syntax_t *match, *suffix;
	char mybuf[1024];
	
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

		/*
		 *	skip the prefix, or print it as something special?
		 */
		suffix = syntax_skip_prefix(match, argc);
		if (!suffix) exit(1);

		syntax_print_lines(suffix);
		syntax_free(suffix);
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
		if (match) {
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
		char const *fail = NULL;
		cli_syntax_t *match;

		my_argc = ctx2argv(buffer + 4, len - 4, 128, my_argv);
		if (my_argc < 0) return -1;

		if ((my_argc > 0) &&
		    (syntax_check(config.syntax, my_argc, my_argv, &fail) < 0)) {
			if (!fail) {
				fprintf(stderr, "Invalid input\n");
			} else {
				fprintf(stderr, "Invalid input '%s'\n", fail);
			}
			return 1;
		}

		/*
		 *	Print short help text first
		 */
		match = syntax_match_max(config.syntax, my_argc, my_argv);
		if (match) {
			syntax_free(match);
			syntax_print_context_help(config.help, my_argc, my_argv);
		}

		help = syntax_show_help(config.help,
					my_argc, my_argv);
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
	char *prompt_full = "";
	char *prompt_ctx = "";
	char const *progname;
	char *history_file = NULL;
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

	/*
	 *	TODO: configurably load the history.
	 *	TODO: limit the size of the history.
	 *	TODO: rename it to ~/.recli_history?
	 */
	linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
	linenoiseSetCharacterCallback(foundspace, ' ');
	linenoiseSetCharacterCallback(foundquote, '"');
	linenoiseSetCharacterCallback(foundquote, '\'');
	linenoiseSetCharacterCallback(foundhelp, '?');

	recli_stdout = stdout;
	recli_stderr = stderr;

	progname = strrchr(argv[0], '/');
	if (progname) {
		progname++;
	} else {
		progname = argv[0];
	}

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
			fprintf(stderr, "Usage: recli [-d config_dir] [-H help.md] -p [permission.txt] [-P] [-s syntax] [-X syntax]\n");
			exit(1);
		}

	argc -= (optind - 1);
	argv += (optind - 1);

	if (!isatty(STDIN_FILENO)) {
		config.prompt = prompt = "";
		context = 0;
		tty = 0;
	}

	if (tty) {
		/*
		 *	The default prompt is the program name.
		 */
		if (!config.prompt) config.prompt = progname;

		prompt_full = malloc(256);
		snprintf(prompt_full, 256, "%s> ", config.prompt);

		prompt_ctx = malloc(256);
		snprintf(prompt_ctx, 256, "%s ...> ", config.prompt);

		prompt = prompt_full;
	}

	/*
	 *	No config dir and we're NOT named "recli".
	 *	Look in /etc/recli/FOO for our configuration.
	 */
	if (!config.dir && (strcmp(progname, "recli") != 0)) {
		line = malloc(2048);
		snprintf(line, 2048, "/etc/recli/%s", progname);
		config.dir = line;
	}

	history_file = malloc(2048);
	snprintf(history_file, 2048, "%s_history.txt", progname);

	if (config.dir) {
		if (recli_bootstrap(&config) < 0) {
			exit(1);
		}
	}

	if (debug_syntax) {
		syntax_printf(config.syntax);printf("\r\n");
	}

	if (!config.dir && !config.banner && tty) {
		recli_fprintf(recli_stdout, "Welcome to ReCLI\nCopyright (C) 2016 Alan DeKok\n\nType \"help\" for help, or use '?' for context-sensitive help.\n");
	}	

	if (quit) goto done;

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	set_signal(SIGINT, catch_sigquit);
	set_signal(SIGQUIT, catch_sigquit);

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
						prompt = prompt_full;
						ctx_line_end = ctx_line_full;
					} else {
						int i;
						char *p;

						p = ctx_line_full;
						for (i = 0; i < ctx_stack_ptr; i++) {
							memcpy(p, ctx_stack[i].buffer, ctx_stack[i].len);
							p += ctx_stack[i].len;
						}
						ctx_line_end = p;
					}
					goto next_line;
				}

				if (strcmp(line, "end") == 0) {
					ctx_stack_ptr = 0;
					ctx_line_end = ctx_line_full;
					prompt = prompt_full;
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

				memcpy(ctx_line_end, line, ctx_stack[ctx_stack_ptr].len);
				ctx_line_end += ctx_stack[ctx_stack_ptr].len;

				ctx_stack_ptr++;

				prompt = prompt_ctx;
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
			/*
			 *	Save the FULL text in the history.
			 */
			strcpy(ctx_line_end, line);
			linenoiseHistoryAdd(ctx_line_full);

			linenoiseHistorySave(history_file); /* Save every new entry */

			if (runit && config.dir) {
				char buffer[8192];

				snprintf(buffer, sizeof(buffer), "%s/bin/",
					 config.dir);

				recli_exec(buffer, my_argc, my_argv,
					   config.envp);
				recli_load_syntax(&config);
				fflush(stdout);
				fflush(stderr);
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
