CC = gcc

# aggressive warning + scan flags
CFLAGS = -std=c11 -Iinclude \
         -Wall -Wextra -Wpedantic \
         -Wshadow \
         -Wconversion \
         -Wsign-conversion \
         -Wunused \
         -Wunused-parameter \
         -Wunused-function \
         -Wunused-variable \
         -Wmissing-prototypes \
         -Wstrict-prototypes \
         -Wunreachable-code \
         -Wformat=2 \
         -Wundef \
         -g

LIBS = -lncursesw

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

sysdash: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIBS)
	rm -f src/*.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o sysdash

.PHONY: clean
