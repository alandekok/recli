#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>
#include <pwd.h>
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

static char *prompt_full = "";
static char *prompt_ctx = "";
static char *history_file = NULL;

/*
 *	Admins can type a partial command, in which case it's put on
 *	the stack, and the prompt changes to include the partial
 *	command.  We want to track these partial commands, but also
 *	limit the size of the stack.
 */
typedef struct ctx_stack_t {
	char		*prompt;

	char		*buf;
	size_t		len;
	size_t		bufsize;

	char		*argv_buf;
	size_t		argv_bufsize;

	int		argc;
	char		**argv;

	int		total_argc;
	int		max_argc;

	cli_syntax_t	*syntax;
	cli_syntax_t	*help;
} ctx_stack_t;


static char ctx_line_buf[8192];	/* full line of whatever the user entered */
static char ctx_argv_buf[8192];	/* copy of the above, split into argv */
static char *ctx_argv[256];	/* where the argvs are */

#define CTX_STACK_MAX (32)

static int ctx_stack_index = 0;
static ctx_stack_t ctx_stack_array[CTX_STACK_MAX];
static ctx_stack_t *ctx_stack = NULL;

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

	if (in_string) return;

	num = syntax_tab_complete(ctx_stack->syntax, buf, strlen(buf), 256, tabs);
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
static int foundhelp(const char *line, size_t len, UNUSED char c)
{
	int argc;
	char *argv[256];
	char buffer[1024];

	/*
	 *	In a quoted string, don't do anything.
	 */
	if (in_string) return 0;

	printf("?\r\n");

	if ((len == 0) || !ctx_stack->help) {
	do_print:
		syntax_print_lines(ctx_stack->syntax);
		return 1;
	}
	
	memcpy(buffer, line, len + 1);
	argc = str2argv(buffer, len, 256, argv);

	if (argc <= 0) goto do_print;

	syntax_print_context_help(ctx_stack->help, argc, argv);
	syntax_print_context_help_subcommands(ctx_stack->help, argc, argv);

	return 1;
}

static int do_help(int argc, char **argv)
{
	int rcode;
	char const *help;
	char const *fail;

	/*
	 *	Show the current syntax
	 */
	if ((argc >= 1) && (strcmp(argv[0], "syntax") == 0)) {
		syntax_print_lines(ctx_stack->syntax);
		return 1;
	}

	rcode = syntax_check(ctx_stack->syntax, argc, argv, &fail);
	if (rcode < 0) {
		if (!fail) {
			fprintf(stderr, "Invalid input\n");
		} else {
			fprintf(stderr, "Invalid input in word %d - '%s'\n", -rcode, fail);
		}

		return 0;
	}

	/*
	 *	Print short help text first
	 */
	syntax_print_context_help(ctx_stack->help, argc, argv);

	help = syntax_show_help(ctx_stack->help, argc, argv);
	if (!help) {
		recli_fprintf(recli_stdout, "\r\n");
	} else {
		recli_fprintf_words(recli_stdout, "%s", help);
	}

	return 1;
}


static void ctx_stack_pop(void)
{
	if (ctx_stack_index == 0) return;

	assert(ctx_stack->syntax != NULL);
	syntax_free(ctx_stack->syntax);
	if (ctx_stack->help) syntax_free(ctx_stack->help);

	ctx_stack_index--;
	ctx_stack--;

	/*
	 *	Reset buffers, etc. for the previous context.
	 */
	ctx_stack->buf[0] = '\0';
	ctx_stack->argv_buf[0] = '\0';
	ctx_stack->argv[0] = NULL;

	ctx_stack->max_argc += ctx_stack->argc;
	ctx_stack->total_argc -= ctx_stack->argc;
	ctx_stack->argc = 0;
}

/*
 *	The user-entered string is already in ctx_stack->buf
 */
static void ctx_stack_push(int argc)
{
	size_t len;
	ctx_stack_t *next;
	cli_syntax_t *match;

	if (ctx_stack_index >= (CTX_STACK_MAX - 1)) return;

	next = ctx_stack + 1;

	match = syntax_match_max(ctx_stack->syntax, argc, ctx_stack->argv);
	assert(match != NULL);

	next->syntax = syntax_skip_prefix(match, argc);
	assert(next->syntax != NULL);
	syntax_free(match);

	if (ctx_stack->help) {
		match = syntax_match_max(ctx_stack->help, argc, ctx_stack->argv);
		if (match) {
			next->help = syntax_skip_prefix(match, argc);
			syntax_free(match);
		} else {
			next->help = NULL;
		}
	} else {
		next->help = NULL;
	}

	len = strlen(ctx_stack->buf);

	ctx_stack->buf[len++] = ' ';
	ctx_stack->buf[len] = '\0';

	ctx_stack->len = len;

	next->buf = ctx_stack->buf + len;
	next->bufsize = ctx_stack->bufsize - len;

	next->argv_buf = &ctx_argv_buf[0] + (next->buf - &ctx_line_buf[0]);

	next->argv_buf = ctx_stack->argv[argc - 1] + 1;
	next->argv_bufsize = next->bufsize;

	next->argv = ctx_stack->argv + argc;
	next->argv[0] = NULL;

	next->max_argc = ctx_stack->max_argc - argc;
	next->argc = argc;
	next->total_argc = ctx_stack->total_argc + argc;

	next->prompt = prompt_ctx;

	ctx_stack_index++;
	ctx_stack = next;
}

static char const *spaces = "                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                ";

static void process(int tty, char *line)
{
	int argc, c;
	int runit = 1;
	size_t len = strlen(line);
	const char *fail;
	char **argv;

	if (!len) goto done;

	if (len >= ctx_stack->bufsize) {
		fprintf(stderr, "line too long\r\n");
		goto done;
	}

	/*
	 *	Copy the text to the two buffers.
	 */
	memcpy(ctx_stack->buf, line, len + 1);
	memcpy(ctx_stack->argv_buf, line, len + 1);

	argv = ctx_stack->argv;

	argc = str2argv(ctx_stack->argv_buf, len, ctx_stack->max_argc, argv);
	if (!argc) goto done;

	if (argc < 0) {
		fprintf(stderr, "%s\n", ctx_stack->buf);
		fprintf(stderr, "%.*s^", -argc, spaces);
		fprintf(stderr, " Parse error.\n");
		goto done;
	}

	/*
	 *	The built-in commands don't bother checking for too
	 *	much input.  They also take priority over user
	 *	commands.
	 */
	if (strcmp(argv[0], "exit") == 0) {
		if (ctx_stack_index == 0) {
			exit(0);
		}

		ctx_stack_pop();
		printf("%s\n", ctx_line_buf);
		goto done;
	}

	if (strcmp(argv[0], "end") == 0) {
		while (ctx_stack_index > 0) ctx_stack_pop();
		goto done;
	}

	if ((strcmp(argv[0], "quit") == 0) ||
	    (strcmp(argv[0], "logout") == 0)) {
		exit(0);
	}

	if (strcmp(argv[0], "help") == 0) {
		do_help(argc - 1, argv + 1);
		goto done;
	}

	/*
	 *	c < 0 - error in argument -C
	 *	c == argc, parsed it completely
	 *	c > argc, add new context
	 */
	c = syntax_check(ctx_stack->syntax, argc, argv, &fail);

	if (c < 0) {
		/*
		 *	FIXME: check against argc
		 *
		 *	FIXME: check against stack
		 *
		 *	if we have "x y" on the stack
		 *	and type in an erroneous "z",
		 *	the c here will be -3, not -1.
		 */
		fprintf(stderr, "%s\n", ctx_stack->buf);
		if (-c == argc) {
			fprintf(stderr, "%.*s^", (int) (ctx_stack->argv[argc - 1] - ctx_stack->argv_buf), spaces);

		} else if (-c > argc) {
			fprintf(stderr, "%.*s^", (int) strlen(ctx_stack->buf), spaces);

		} else {
			fprintf(stderr, "%.*s^", (int) (ctx_stack->argv[-c] - ctx_stack->argv_buf), spaces);
		}
		fprintf(stderr, " Parse error.\n");
		
		runit = 0;
		goto add_line;
	}

	/*
	 *	We reached the end of the syntax before the end of the input
	 */
	if (c < argc) {
		fprintf(stderr, "%s\n", ctx_stack->buf);
		fprintf(stderr, "%.*s^", (int) (ctx_stack->argv[c] - ctx_stack->argv_buf), spaces);

		fprintf(stderr, " Unexpected text.\n");
		runit = 0;
		goto add_line;
	}

	/*
	 *	FIXME: figure out which thing we didn't have permission for.
	 *
	 *	Note that we check the permissions on the FULL arguments, because that's how it works.q
	 */
	if (!permission_enforce(config.permissions, ctx_stack->total_argc + argc,
				ctx_argv)) {
		fprintf(stderr, "%s\n", line);
		fprintf(stderr, "^ - No permission\n");
		runit = 0;
		goto add_line;
	}

	/*
	 *	Got N commands, wanted M > N in order to do anything.
	 */
	if (c > argc) {
		runit = 0;

		if (ctx_stack_index >= (CTX_STACK_MAX - 1)) {
			c = -1;
			goto add_line;
		}

		ctx_stack_push(argc);
		goto done;
	}

	runit = 1;

add_line:
	if (tty) {
		/*
		 *	Save the FULL text in the history.
		 */
		linenoiseHistoryAdd(ctx_line_buf);

		if (history_file) linenoiseHistorySave(history_file);
	}

	if (runit && config.dir) {
		char buffer[8192];

		snprintf(buffer, sizeof(buffer), "%s/bin/",
			 config.dir);
		
		recli_exec(buffer, ctx_stack->total_argc + argc,
			   ctx_argv, config.envp);
		recli_load_syntax(&config);
		fflush(stdout);
		fflush(stderr);
	}

done:
	free(line);
}


int main(int argc, char **argv)
{
	int c, rcode;
	char const *progname;
	int quit = 0;
	char *line;
	int tty = 1;
	int debug_syntax = 0;

#ifndef NO_COMPLETION
	linenoiseSetCompletionCallback(completion);
#endif

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
			config.prompt = optarg;
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
		config.prompt = "";
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

	if (tty) {
		 char *home;

		 home = getenv("HOME");
		 if (!home) home = getpwuid(getuid())->pw_dir;

		 printf("HOME DIR %s\n", home);

		if (home) {
			history_file = malloc(8192);

			snprintf(history_file, 8192, "%s/.recli", home);
			if ((mkdir(history_file, 0700) < 0) &&
			    (errno != EEXIST)) {
				free(history_file);
				history_file = NULL;
			}

			snprintf(history_file, 8192, "%s/.recli/%s_history.txt", home, progname);

			linenoiseHistoryLoad(history_file); /* Load the history at startup */
		}
	}

	linenoiseSetCharacterCallback(foundspace, ' ');
	linenoiseSetCharacterCallback(foundquote, '"');
	linenoiseSetCharacterCallback(foundquote, '\'');
	linenoiseSetCharacterCallback(foundhelp, '?');

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

	/*
	 *	Set up the stack.
	 */
	ctx_stack_index = 0;
	ctx_stack = &ctx_stack_array[0];

	ctx_stack->argv = ctx_argv;
	ctx_stack->argc = 0;
	ctx_stack->total_argc = 0;
	ctx_stack->max_argc = sizeof(ctx_argv) / sizeof(ctx_argv[0]);
	
	ctx_stack->buf = ctx_line_buf;
	ctx_stack->bufsize = sizeof(ctx_line_buf);

	ctx_stack->argv_buf = ctx_argv_buf;
	ctx_stack->argv_bufsize = sizeof(ctx_argv_buf);

	ctx_stack->syntax = config.syntax;
	ctx_stack->help = config.help;

	ctx_stack->prompt = prompt_full;

	while ((line = linenoise(ctx_stack->prompt)) != NULL) {
		process(tty, line);
	}

done:
	while (ctx_stack_index > 0) ctx_stack_pop();

	if (config.help) syntax_free(config.help);
	syntax_free(config.syntax);
	permission_free(config.permissions);

	syntax_free(NULL);

	return 0;
}
