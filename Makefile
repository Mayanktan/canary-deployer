# make         build everything into bin/
# make test    run the end-to-end test
# make tsan    run the same test against a ThreadSanitizer build of the controller
# make clean   remove bin/

CC     ?= cc
OPT    ?= -O2 -g
CFLAGS  = $(OPT) -std=gnu11 -Wall -Wextra -pthread

COMMON     = $(wildcard src/common/*.c)
CONTROLLER = $(wildcard src/controller/*.c)
HEADERS    = $(wildcard src/*/*.h)

all: bin/controller bin/demo-service bin/deployctl bin/loadgen

bin/controller: $(CONTROLLER) $(COMMON) $(HEADERS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(CONTROLLER) $(COMMON)

# demo-service, deployctl and loadgen are each a single main.c plus the common code.
bin/%: src/%/main.c $(COMMON) $(HEADERS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< $(COMMON)

test: all
	./tests/e2e_test.sh

# ThreadSanitizer watches every memory access at runtime and reports a data
# race (two threads using the same memory without a lock) as a test failure.
tsan: all
	@mkdir -p bin/tsan
	$(CC) -O1 -g -std=gnu11 -Wall -Wextra -pthread -fsanitize=thread -o bin/tsan/controller $(CONTROLLER) $(COMMON)
	CONTROLLER=bin/tsan/controller ./tests/e2e_test.sh

clean:
	rm -rf bin

.PHONY: all test tsan clean
