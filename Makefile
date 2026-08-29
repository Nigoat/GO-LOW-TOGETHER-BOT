CONCORD_PATH ?= /usr/local
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -I$(CONCORD_PATH)/include -Iinclude -I/usr/include/postgresql -DCCORD_SIGINTCATCH
LDFLAGS = -L$(CONCORD_PATH)/lib -ldiscord -lcurl -lpthread -lpq

SRC = src/main.c src/config.c src/db.c src/verify.c src/tickets.c src/clans.c src/voice.c
OBJ = $(SRC:.c=.o)
TARGET = GLT

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
