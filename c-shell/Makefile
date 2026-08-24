CC := gcc

# The assignment mandates -std=c23. GCC only spells it that way from 14
# onwards, so fall back to the identical -std=c2x on older toolchains.
STD := $(shell $(CC) -std=c23 -E -x c /dev/null > /dev/null 2>&1 \
                 && echo c23 || echo c2x)

CFLAGS := -std=$(STD) -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
          -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm \
          -Iinclude
LDFLAGS :=

SRC_DIR := src
BUILD_DIR := build
TARGET := shell.out

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
