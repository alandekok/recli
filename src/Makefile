all:  recli linenoise_example linenoise_utf8_example linenoise_cpp_example ../recli

linenoise_example: linenoise.h linenoise.c example.c
	$(CC) -Wall -W -Os -g -o $@ linenoise.c example.c

linenoise_utf8_example: linenoise.c utf8.c example.c
	$(CC) -DNO_COMPLETION -DUSE_UTF8 -Wall -W -Os -g -o $@ linenoise.c utf8.c example.c

linenoise_cpp_example: linenoise.h linenoise.c
	g++ -Wall -W -Os -g -o $@ linenoise.c example.c

clean:
	@rm -f linenoise_example linenoise_utf8_example linenoise_cpp_example recli
	@rm -rf *.o *~ *.dSYM

push: check
	@git push

RECLI_SRCS := linenoise.c recli.c util.c syntax.c permission.c datatypes.c \
	dir.c strlcpy.c

RECLI_OBJS := $(RECLI_SRCS:.c=.o)

$(RECLI_OBJS): linenoise.h recli.h datatypes.h 

%.o: %.c
	$(CC) -Wall -W -g -c $<

recli: $(RECLI_OBJS)

../recli: recli
	@cp $< $@
