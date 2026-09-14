# Exercise 3 - Ring All-Reduce over RDMA Verbs.
#
#   make              build the course driver `test`
#   make check        build and run all local unit tests (no fabric needed)
#   make test-NAME    build and run one test, e.g. `make test-topology`
#   make tar          package the submission: [ID1=... ID2=...] make tar
#   make clean        remove build artifacts

CC       ?= gcc
CFLAGS   ?= -O3 -Wall -Wextra -std=c11
CPPFLAGS += $(shell pkg-config --cflags libibverbs 2>/dev/null)
LDLIBS   += $(shell pkg-config --libs libibverbs 2>/dev/null || echo -libverbs) -lm

ID1 ?= 000000000
ID2 ?= 000000000

LIB_SRC := ring_allreduce.c pg_verbs.c pg_bootstrap.c pg_collective.c
LIB_HDR := pg.h

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:.c=)
TEST_RUN := $(patsubst tests/test_%,test-%,$(TEST_BIN))

.PHONY: all check clean tar $(TEST_RUN)

all: test

test: test.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test.c $(LIB_SRC) $(LDLIBS)

# ---------------------------------------------------------------- tests ----

tests/test_%: tests/test_%.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB_SRC) $(LDLIBS)

check: $(TEST_BIN)
	@set -e; for test_binary in $(TEST_BIN); do ./$$test_binary; done; \
	printf 'All tests passed\n'

$(TEST_RUN): test-%: tests/test_%
	@./$<

# ----------------------------------------------------------- submission ----

tar: clean
	tar -czvf $(ID1)_$(ID2).tgz Makefile pg.h $(LIB_SRC) test.c report.txt

clean:
	rm -f test $(TEST_BIN) $(ID1)_$(ID2).tgz
