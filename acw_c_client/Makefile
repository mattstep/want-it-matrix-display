CFILES=main.c
PROJECTS=fade white epilepsy bounce chill strobe chase sweep
DEPS=matrix.h

all: $(PROJECTS)

clean:
	rm -rf $(PROJECTS) *~ *.dSYM

$(PROJECTS): %: %.c $(CFILES) $(DEPS)
	g++ -Wall -O2 -g -o $@ $< $(CFILES)

test: acw_client
	./acw_client
