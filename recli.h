#include "linenoise.h"
#include <stdarg.h>
#include <sys/stat.h>
#include "datatypes.h"

extern ssize_t strquotelen(const char *str);
extern int str2argv(char *buf, size_t len, int max_argc, char *argv[]);
extern void print_argv(int argc, char *argv[]);
extern void print_argv_string(int argc, char *argv[]);
extern int recli_fprintf_words(void *ctx, const char *fmt, ...);
int recli_fprintf_wrapper(void *ctx, const char *fmt, ...);
typedef int (*recli_fprintf_t)(void *ctx, const char *fmt, ...);
extern void *recli_stdout;
extern void *recli_stderr;
extern recli_fprintf_t recli_fprintf;

typedef struct cli_permission_t cli_permission_t;

extern int permission_enforce(cli_permission_t *head, int argc, char *argv[]);
extern int permission_parse_file(const char *filename, cli_permission_t **result);
extern void permission_free(cli_permission_t *head);

typedef struct cli_syntax_t cli_syntax_t;

extern int syntax_merge(cli_syntax_t **phead, char *str);
extern int syntax_parse_file(const char *filename, cli_syntax_t **);
extern void syntax_free(cli_syntax_t *);

typedef int (*cli_syntax_parse_t)(const char *);

extern int syntax_parse(const char *buffer, cli_syntax_t **out);
extern int syntax_parse_add(const char *name, cli_syntax_parse_t callback);
extern int syntax_check(cli_syntax_t *syntax, int argc, char *argv[],
			const char **fail);
extern cli_syntax_t *syntax_match_max(cli_syntax_t *head, int argc, char *argv[]);
extern void syntax_printf(const cli_syntax_t *syntax);
extern void syntax_print_lines(const cli_syntax_t *this);
extern int syntax_tab_complete(cli_syntax_t *head, const char *in, size_t len,
			       int max_tabs, char *tabs[]);
extern int syntax_parse_help(const char *filename, cli_syntax_t **phead);
extern const char *syntax_show_help(cli_syntax_t *head, int argc, char *argv[], int flag);
extern int syntax_print_context_help(cli_syntax_t *head, int argc, char *argv[]);

typedef int (*recli_datatype_parse_t)(const char*);

typedef struct recli_datatype_t {
	const char		*name;
	recli_datatype_parse_t  parse;
} recli_datatype_t;

extern recli_datatype_t recli_datatypes[];
extern int recli_datatypes_init(void);

typedef struct recli_config_t {
	const char *dir;
	const char *prompt;
	const char *banner;
	char	   *envp[128];
	cli_syntax_t *syntax;
	ino_t		syntax_inode;
	cli_syntax_t *help;
	cli_permission_t *permissions;
} recli_config_t;

extern int recli_bootstrap(recli_config_t *config);
int recli_exec_syntax(cli_syntax_t **phead, const char *dir, char *program,
		      char *const envp[]);
extern int recli_exec(const char *rundir, int argc, char *argv[],
		      char *const envp[]);
