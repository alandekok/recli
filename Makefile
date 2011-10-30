all:  recli linenoise_example linenoise_utf8_example linenoise_cpp_example

RECLI_SRCS := linenoise.c recli.c util.c syntax.c permission.c datatypes.c \
	dir.c

recli: linenoise.h recli.h datatypes.h $(RECLI_SRCS)
	$(CC) -Wall -W -g -o $@ $(RECLI_SRCS)

linenoise_example: linenoise.h linenoise.c example.c
	$(CC) -Wall -W -Os -g -o $@ linenoise.c example.c

linenoise_utf8_example: linenoise.c utf8.c example.c
	$(CC) -DNO_COMPLETION -DUSE_UTF8 -Wall -W -Os -g -o $@ linenoise.c utf8.c example.c

linenoise_cpp_example: linenoise.h linenoise.c
	g++ -Wall -W -Os -g -o $@ linenoise.c example.c

clean:
	@rm -f linenoise_example linenoise_utf8_example linenoise_cpp_example recli
	@rm -rf *.o *~ *.dSYM
	@$(MAKE) -C tests clean

check: recli
	@$(MAKE) -C tests

push: check
	@git push
