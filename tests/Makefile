TESTS	:= hostname ipaddr ipv4addr ipv6addr integer string fish aorb maybea comments many merge prefix \
		varargs

all: ../src/recli
	@rm -f .failed
	@for x in $(TESTS); do \
		./testcli.sh $$x; \
	done
	@if [ -f .failed ]; then \
		echo "FAILED :" `cat .failed`; \
		exit 1; \
	fi

../src/recli: $(wildcard ../src/*.[ch])
	@$(MAKE) -C ../src recli

clean:
	@rm -f *~ *.tmp *diff .failed
