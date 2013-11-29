all:
	@$(MAKE) -C src

clean:
	@rm -rf *~
	@$(MAKE) -C tests clean

test: src/recli
	@$(MAKE) -C tests

src/recli: $(wildcard src/*.[ch])
	@$(MAKE) -C src recli

push: test
	@git push
