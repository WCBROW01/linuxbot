CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lcurl

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

linuxbot: $(OBJ) /usr/local/lib/libdiscord.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

.PHONY: clean
clean: rm -f $(OBJ) linuxbot
