CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -fopenmp -Wall -Wextra -ffile-prefix-map=$(CURDIR)=.
LDLIBS  ?= -lm

gc2607d: gc2607d.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f gc2607d

.PHONY: clean
