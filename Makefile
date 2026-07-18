CC ?= cc
CFLAGS = -O2 -g -Wall -Wextra -Iinclude
TEST_FX_DIR = build/test-modules/audio_fx/testfx

ifeq ($(shell uname -s),Darwin)
SHARED_FLAGS = -dynamiclib
else
SHARED_FLAGS = -shared
endif

.PHONY: test arm clean

test: build/host_sim
	./build/host_sim

build/host_sim: src/mark_core.c src/mark_core.h test/host_sim.c $(TEST_FX_DIR)/testfx.so
	@mkdir -p build
	$(CC) $(CFLAGS) src/mark_core.c test/host_sim.c -o $@ -lm -lpthread

$(TEST_FX_DIR)/testfx.so: test/fake_fx.c test/fake_fx_module.json include/audio_fx_api_v2.h
	@mkdir -p $(TEST_FX_DIR) build/test-modules/overtake/mark
	$(CC) $(CFLAGS) -fPIC $(SHARED_FLAGS) test/fake_fx.c -o $@
	cp test/fake_fx_module.json $(TEST_FX_DIR)/module.json

arm:
	./scripts/build.sh

clean:
	rm -rf build
