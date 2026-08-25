CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += $(shell pkg-config --cflags libibverbs 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libibverbs 2>/dev/null || echo -libverbs)

TARGET := ring_allreduce
SRC := ring_allreduce.c
OBJ := $(SRC:.c=.o)
TEST_TARGET := tests/test_phase1

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TEST_TARGET): tests/test_phase1.c ring_allreduce.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_phase1.c $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJ) $(TARGET) $(TEST_TARGET)

.PHONY: all test clean
