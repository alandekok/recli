all:
	@$(MAKE) -C src

clean:
	@rm -rf *~ recli
	@$(MAKE) -C src clean
	@$(MAKE) -C tests clean

test: recli
	@$(MAKE) -C tests

recli: $(wildcard src/*.[ch])
	@$(MAKE) -C src recli

push: test
	@git push

install: recli
	@test -d $(DESTDIR)/usr/bin || install -d -o root -g root -m 0755 $(DESTDIR)/usr/bin
	@install -o root -g root -m 0755 recli $(DESTDIR)/usr/bin/recli
