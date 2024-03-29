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

#define XSTRINGIFY(x) #x
#define STRINGIFY(x) XSTRINGIFY(x)

#ifndef CONFIG_DIR
#define CONFIG_DIR "config"
#endif

static recli_config_t config = {
	.dir = CONFIG_DIR,
	.prompt = NULL,
	.banner = NULL,
	.syntax = NULL,
	.short_help = NULL,
	.long_help = NULL,
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
	cli_syntax_t	*short_help;
	cli_syntax_t	*long_help;
} ctx_stack_t;


static char ctx_line_buf[8192];	/* full line of whatever the user entered */
static char ctx_argv_buf[8192];	/* copy of the above, split into argv */
static char *ctx_argv[256];	/* where the argvs are */

#define CTX_STACK_MAX (32)

static int ctx_stack_index = 0;
static ctx_stack_t ctx_stack_array[CTX_STACK_MAX];
static ctx_stack_t *ctx_stack = NULL;

extern pid_t child_pid;

typedef void (*builtin_func_t)(int , char **);

typedef struct builtin_t {
	char const	*name;
	builtin_func_t	function;
} builtin_t;

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
static int short_help(const char *line, size_t len, UNUSED char c)
{
	int argc;
	char *argv[256];
	char buffer[1024];

	/*
	 *	In a quoted string, don't do anything.
	 */
	if (in_string) return 0;

	recli_fprintf(recli_stdout, "?\r\n");

	if (!ctx_stack->short_help) {
	do_print:
		syntax_print_lines(ctx_stack->syntax);
		return 1;
	}
	
	memcpy(buffer, line, len + 1);
	argc = str2argv(buffer, len, 256, argv);

	if (argc < 0) goto do_print;

	if (ctx_stack_index > 0) {
		ctx_stack_t *c = &ctx_stack_array[ctx_stack_index - 1];

		recli_fprintf(recli_stdout, "%s - ", c->argv[c->argc - 1]);
	}
	syntax_print_context_help(ctx_stack->short_help, argc, argv);
	syntax_print_context_help_subcommands(ctx_stack->syntax, ctx_stack->short_help, argc, argv);

	return 1;
}


static const char *history_callback(const char *buffer)
{
	int i, j;
	const char *p;

	if (ctx_stack_index == 0) return buffer;

	p = buffer;

	for (i = 0; i < ctx_stack_index; i++) {
		ctx_stack_t *c;

		c = &ctx_stack_array[i];

		for (j = 0; j < c->argc; j++) {
			size_t len;

			len = strlen(c->argv[j]);

			if ((strncmp(c->argv[j], p, len) == 0) && isspace((int) p[len])) {
				p += len + 1;
				continue;
			}

			return p;
		}
	}


	return p;
}


/*
 *	Stack functions
 */
static void ctx_stack_pop(void)
{
	if (ctx_stack_index == 0) return;

	assert(ctx_stack->syntax != NULL);
	syntax_free(ctx_stack->syntax);
	if (ctx_stack->long_help) syntax_free(ctx_stack->long_help);
	if (ctx_stack->short_help) syntax_free(ctx_stack->short_help);

	ctx_stack_index--;
	ctx_stack--;

	/*
	 *	Reset buffers, etc. for the previous context.
	 */
	ctx_stack->buf[0] = '\0';
	ctx_stack->argv_buf[0] = '\0';
	ctx_stack->argv[0] = NULL;
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

	if (ctx_stack->short_help) {
		match = syntax_match_max(ctx_stack->short_help, argc, ctx_stack->argv);
		if (match) {
			next->short_help = syntax_skip_prefix(match, argc);
			syntax_free(match);
		} else {
			next->short_help = NULL;
		}
	} else {
		next->short_help = NULL;
	}

	if (ctx_stack->long_help) {
		match = syntax_match_max(ctx_stack->long_help, argc, ctx_stack->argv);
		if (match) {
			next->long_help = syntax_skip_prefix(match, argc);
			syntax_free(match);
		} else {
			next->long_help = NULL;
		}
	} else {
		next->long_help = NULL;
	}

	len = strlen(ctx_stack->buf);

	ctx_stack->buf[len++] = ' ';
	ctx_stack->buf[len] = '\0';
	ctx_stack->argc = argc;

	ctx_stack->len = len;

	next->buf = ctx_stack->buf + len;
	next->bufsize = ctx_stack->bufsize - len;

	next->argv_buf = &ctx_argv_buf[0] + (next->buf - &ctx_line_buf[0]);
	next->argv_bufsize = next->bufsize;

	next->argv = ctx_stack->argv + argc;
	next->argv[0] = NULL;

	next->max_argc = ctx_stack->max_argc - argc;
	next->argc = 0;
	next->total_argc = ctx_stack->total_argc + argc;

	next->prompt = prompt_ctx;

	ctx_stack_index++;
	ctx_stack = next;
}


/*
 *	Builtin commands
 */
static void builtin_help(int argc, char **argv)
{
	int rcode;
	char const *help;
	char const *error;

	/*
	 *	Show the current syntax
	 */
	if ((argc >= 1) && (strcmp(argv[0], "syntax") == 0)) {
		syntax_print_lines(ctx_stack->syntax);
		return;
	}

	rcode = syntax_check(ctx_stack->syntax, argc, argv, &error, NULL);
	if (rcode < 0) {
		if (!error) {
			fprintf(stderr, "Invalid input\n");
		} else {
			fprintf(stderr, "Invalid input in word %d - '%s'\n", -rcode, error);
		}

		return;
	}

	if (!ctx_stack->long_help) return;

	/*
	 *	Print short help text first
	 */
	syntax_print_context_help(ctx_stack->long_help, argc, argv);

	help = syntax_show_help(ctx_stack->long_help, argc, argv);
	if (!help) {
		recli_fprintf(recli_stdout, "\r\n");
	} else {
		recli_fprintf_words(recli_stdout, "%s", help);
	}

	return;
}

/*
 *	The built-in commands don't bother checking for too
 *	much input.  They also take priority over user
 *	commands.
 */
static void builtin_end(UNUSED int argc, UNUSED char *argv[])
{
	while (ctx_stack_index > 0) ctx_stack_pop();
}

static void builtin_exit(UNUSED int argc, UNUSED char *argv[])
{
	if (ctx_stack_index == 0) {
		exit(0);
	}

	ctx_stack_pop();

	recli_fprintf(recli_stdout, "%s\n", ctx_line_buf);
}

static void builtin_quit(UNUSED int argc, UNUSED char *argv[])
{
	exit(0);
}

static builtin_t builtin_commands[] = {
	{ "end", builtin_end },
	{ "exit", builtin_exit },
	{ "help", builtin_help },
	{ "logout", builtin_quit },
	{ "quit", builtin_quit },
	{ NULL, NULL }
};

static char const *spaces = "                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                ";

static void process(int tty, char *line)
{
	int i, c, argc;
	int runit = 1;
	int needs_tty = 0;
	size_t len = strlen(line);
	const char *error;
	char **argv;

	if (!len) return;

	if (len >= ctx_stack->bufsize) {
		fprintf(stderr, "line too long\r\n");
		return;
	}

	/*
	 *	Copy the text to the two buffers.
	 */
	memcpy(ctx_stack->buf, line, len + 1);
	memcpy(ctx_stack->argv_buf, line, len + 1);

	argv = ctx_stack->argv;

	argc = str2argv(ctx_stack->argv_buf, len, ctx_stack->max_argc, argv);
	if (!argc) return;

	if (argc < 0) {
		fprintf(stderr, "%s\n", ctx_stack->buf);
		fprintf(stderr, "%.*s^", -argc, spaces);
		fprintf(stderr, " Parse error.\n");
		return;
	}

	for (i = 0; builtin_commands[i].name != NULL; i++) {
		if (strcmp(argv[0], builtin_commands[i].name) == 0) {
			builtin_commands[i].function(argc - 1, argv + 1);
			return;
		}
	}

	/*
	 *	c < 0 - error in argument -C
	 *	c == argc, parsed it completely
	 *	c > argc, add new context
	 */
	c = syntax_check(ctx_stack->syntax, argc, argv, &error, &needs_tty);

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
			fprintf(stderr, "%.*s^", (int) (ctx_stack->argv[-c - 1] - ctx_stack->argv_buf), spaces);
		}

		if (!error) error = "Parse error";

		fprintf(stderr, " %s.\n", error);
		
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
	 *	Note that we check the permissions on the FULL arguments, because that's how it works.
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
		return;
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
		
#if 0
		printf("RUNNING %s\n", buffer);
		for (int i = 0; i <  ctx_stack->total_argc + argc; i++) {
			printf("\t%d %s\n", i, ctx_argv[i]);
		}
#endif

		recli_exec(buffer, needs_tty, ctx_stack->total_argc + argc,
			   ctx_argv, config.envp);
		recli_load_syntax(&config);

		/* If the config was reloaded, update the stack */
		if (config.syntax != ctx_stack->syntax) {
			while (ctx_stack_index > 0) ctx_stack_pop();
			ctx_stack->syntax = config.syntax;
		}

		fflush(stdout);
		fflush(stderr);
	}
}

static void usage(char const *name, int rcode)
{
	FILE *out = stderr;

	if (rcode == 0) out = stdout;

	fprintf(out, "Usage: %s [-d config_dir]\n", name);
	fprintf(out, "  -d <config_dir>	Configuration file directory, defaults to '%s'\n", config.dir);
	fprintf(out, "\n");
	fprintf(out, "  Additional options which should be used only for testing,\n");
	fprintf(out, "  as they will ignore the configuration directory\n");
	fprintf(out, "  When testing, no commands will be executed.\n");
	fprintf(out, "\n");
	fprintf(out, "  -H help.txt     Load 'help.txt' as the help text file.\n");
	fprintf(out, "  -s syntax.txt   Load syntax from 'syntax.txt'\n");
	fprintf(out, "  -p perm.txt     Load permissions from 'perm.txt'\n");
	fprintf(out, "  -X <flag>       Add debugging.  Valid flags are 'syntax'\n");
	exit(rcode);
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

	while ((c = getopt(argc, argv, "d:hH:p:qr:s:P:X:")) != EOF) switch(c) {
		case 'd':
			config.dir = optarg;
			break;

		case 'h':
			usage(progname, 0);
			break;

		case 'H':
			if (syntax_parse_help(optarg, &config.long_help, &config.short_help) < 0) exit(1);
			config.dir = NULL;
			break;

		case 'p':
			rcode = permission_parse_file(optarg, &config.permissions);
			if (rcode < 0) exit(1);
			if (rcode == 0) exit(0);
			config.dir = NULL;
			break;

		case 'q':
			quit = 1;
			break;

		case 's':
			if (syntax_parse_file(optarg, &config.syntax) < 0) exit(1);
			config.dir = NULL;
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
			usage(progname, 1);
			break;
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

		 linenoiseSetHistoryCallback(history_callback);
	}

	linenoiseSetCharacterCallback(foundspace, ' ');
	linenoiseSetCharacterCallback(foundquote, '"');
	linenoiseSetCharacterCallback(foundquote, '\'');
	linenoiseSetCharacterCallback(short_help, '?');

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
	ctx_stack->long_help = config.long_help;
	ctx_stack->short_help = config.short_help;

	ctx_stack->prompt = prompt_full;

	while ((line = linenoise(ctx_stack->prompt)) != NULL) {
		process(tty, line);
		free(line);
	}

done:
	while (ctx_stack_index > 0) ctx_stack_pop();

	if (config.short_help) syntax_free(config.short_help);
	if (config.long_help) syntax_free(config.long_help);
	syntax_free(config.syntax);
	permission_free(config.permissions);

	syntax_free(NULL);

	return 0;
}
