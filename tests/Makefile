TESTS	:= ipaddr integer string fish aorb maybea comments many merge prefix

all: ../cli
	@rm -f .failed
	@for x in $(TESTS); do \
		./testcli.sh $$x; \
	done
	@if [ -f .failed ]; then \
		echo "FAILED :" `cat .failed`; \
		exit 1; \
	fi

../cli: $(wildcard ../*.[ch])
	@$(MAKE) -C .. cli

clean:
	@rm -f *~ *.tmp *diff .failed
