CC      ?= gcc
CFLAGS  ?= -O2 -march=native -Wall -Wextra -fopenmp
LDLIBS  ?= -lm

gc2607d: gc2607d.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f gc2607d

.PHONY: clean
