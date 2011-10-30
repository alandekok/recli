TESTS	:= ipaddr integer string fish aorb maybea comments many merge prefix

all: ../recli
	@rm -f .failed
	@for x in $(TESTS); do \
		./testcli.sh $$x; \
	done
	@if [ -f .failed ]; then \
		echo "FAILED :" `cat .failed`; \
		exit 1; \
	fi

../recli: $(wildcard ../*.[ch])
	@$(MAKE) -C .. recli

clean:
	@rm -f *~ *.tmp *diff .failed
