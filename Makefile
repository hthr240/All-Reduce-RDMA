CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += $(shell pkg-config --cflags libibverbs 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libibverbs 2>/dev/null || echo -libverbs)

TARGET := ring_allreduce
SRC := ring_allreduce.c
OBJ := $(SRC:.c=.o)
TEST_SOURCES := $(wildcard tests/test_phase*.c)
TEST_TARGETS := $(TEST_SOURCES:.c=)
TEST_PHASE_NUMBERS := $(patsubst tests/test_phase%,%,$(TEST_TARGETS))
TEST_PHASE_TARGETS := $(addprefix test-phase,$(TEST_PHASE_NUMBERS))

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/test_phase%: tests/test_phase%.c ring_allreduce.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

test: $(TEST_TARGETS)
	@set -e; for test_target in $(TEST_TARGETS); do \
		./$$test_target; \
	done; \
	printf 'All tests passed\n'

$(TEST_PHASE_TARGETS): test-phase%: tests/test_phase%
	@./$<
	@printf 'All tests passed for phase %s\n' "$*"

clean:
	rm -f $(OBJ) $(TARGET) $(TEST_TARGETS)

.PHONY: all test $(TEST_PHASE_TARGETS) clean
