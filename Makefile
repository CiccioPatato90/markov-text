# Detect host OS to select the right vendored DuckDB library
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	LIBDIR := $(CURDIR)/libs/macos
else
	LIBDIR := $(CURDIR)/libs/linux
endif

CFLAGS := -Wall -Wextra -O2 -I$(LIBDIR)
LDFLAGS := -L$(LIBDIR) -lduckdb -Wl,-rpath,$(LIBDIR)
BIN := build/markov

all: $(BIN)

$(BIN): markov.c
	mkdir -p build
	gcc $(CFLAGS) -o $(BIN) markov.c $(LDFLAGS)

run: $(BIN)
	./$(BIN)

demo: $(BIN)
	rm -f data/db/demo.db data/db/demo.db.wal
	./$(BIN) data/db/demo.db < demo.txt

debug:
	mkdir -p build
	gcc -g $(CFLAGS) -o $(BIN) markov.c $(LDFLAGS) && lldb ./$(BIN)

clean:
	rm -rf build
