#include "linenoise.h"
#include "datatypes.h"

extern ssize_t strquotelen(const char *str);
extern int str2argv(char *buf, size_t len, int max_argc, char *argv[]);
extern void print_argv(int argc, char *argv[]);
extern void print_argv_string(int argc, char *argv[]);

extern int permission_enforce(int argc, char *argv[]);
extern int permission_parse_file(const char *filename);
extern void permission_free(void);

typedef struct cli_syntax_t cli_syntax_t;

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

typedef struct recli_config_t {
	const char *dir;
	const char *prompt;
	const char *banner;
	cli_syntax_t *syntax;
	cli_syntax_t *help;
} recli_config_t;

extern int recli_bootstrap(recli_config_t *config);
