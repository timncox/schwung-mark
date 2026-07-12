CC ?= cc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude

.PHONY: test arm clean

test: build/host_sim
	./build/host_sim

build/host_sim: src/mark_core.c src/mark_core.h test/host_sim.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/mark_core.c test/host_sim.c -o $@ -lm

arm:
	./scripts/build.sh

clean:
	rm -rf build
