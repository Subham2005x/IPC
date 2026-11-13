CC = gcc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread

TARGET := ipc_system
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c include/ipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJECTS)
