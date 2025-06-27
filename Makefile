# Makefile for good-blocks
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?= -lm
TARGET  ?= good-blocks
SRC     := main.c
OBJ     := $(SRC:.c=.o)

.PHONY: all clean
.DEFAULT_GOAL := all

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(TARGET) $(OBJ)
