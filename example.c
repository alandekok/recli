#include <stdio.h>
#include <stdlib.h>
#include "linenoise.h"


void completion(const char *buf, size_t len, linenoiseCompletions *lc) {
    if (buf[0] == 'h') {
        linenoiseAddCompletion(lc,"hello");
        linenoiseAddCompletion(lc,"hello there");
    }
}

int foundspace(const char *buf, size_t len, char c) {
  printf("\r\nSPACE!\r\n");
  return 0;
}

int main(void) {
    char *line;

    linenoiseSetCompletionCallback(completion);
    linenoiseHistoryLoad("history.txt"); /* Load the history at startup */
    linenoiseSetCharacterCallback(foundspace, ' ');

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
