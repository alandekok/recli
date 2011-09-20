#include <stdio.h>
#include <stdlib.h>
#include "linenoise.h"

static int in_string = 0;
static size_t string_start = 0;

void completion(const char *buf, size_t len, linenoiseCompletions *lc) {
    if (buf[0] == 'h') {
        linenoiseAddCompletion(lc,"hello");
        linenoiseAddCompletion(lc,"hello there");
    }
}

int foundspace(const char *buf, size_t len, char c) {
    if (in_string) return 0;

    if (len == 0) return 1;

    if (buf[len -1] == c) return 1;

    printf("\r\nSPACE!\r\n");
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


int foundquote(const char *buf, size_t len, char c) {
    if (!in_string) {
        in_string = 1;
	string_start = len;
	return 0;
    }

    if (buf[string_start] != c) return 0;

    if (escapedquote(buf + string_start)) return 0;

    in_string = 0;
    printf("\r\nSTRING %s%c\r\n", buf + string_start, buf[string_start]);
    string_start = 0;

    return 0;
}

int main(void) {
    char *line;

    linenoiseSetCompletionCallback(completion);
    linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
    linenoiseSetCharacterCallback(foundspace, ' ');
    linenoiseSetCharacterCallback(foundquote, '"');
    linenoiseSetCharacterCallback(foundquote, '\'');

    while((line = linenoise("hello> ")) != NULL) {
        if (line[0] != '\0') {
            printf("echo: '%s'\n", line);
            linenoiseHistoryAdd(line);
            linenoiseHistorySave("history.txt"); /* Save every new entry */
        }
        free(line);
    }
    return 0;
}
