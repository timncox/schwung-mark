#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_fx_api_v2.h"

typedef struct { int gain; } fake_fx_t;

static void *fake_create(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    fake_fx_t *fx = calloc(1, sizeof(*fx));
    if (fx) fx->gain = 50;
    return fx;
}

static void fake_destroy(void *instance) { free(instance); }

static void fake_process(void *instance, int16_t *audio, int frames) {
    fake_fx_t *fx = (fake_fx_t *)instance;
    for (int i = 0; i < frames * 2; i++)
        audio[i] = (int16_t)((int)audio[i] * fx->gain / 100);
}

static void fake_set(void *instance, const char *key, const char *val) {
    fake_fx_t *fx = (fake_fx_t *)instance;
    if (!strcmp(key, "gain")) {
        int v = atoi(val);
        fx->gain = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else if (!strcmp(key, "state")) {
        const char *p = strstr(val, "\"gain\":");
        if (p) {
            int v = atoi(p + 7);
            fx->gain = v < 0 ? 0 : (v > 100 ? 100 : v);
        }
    }
}

static int fake_get(void *instance, const char *key, char *buf, int len) {
    fake_fx_t *fx = (fake_fx_t *)instance;
    /* Keep the call open briefly so the host test can exercise an unload
     * racing a control-thread reader. */
    usleep(250);
    if (!strcmp(key, "gain")) return snprintf(buf, (size_t)len, "%d", fx->gain);
    if (!strcmp(key, "state"))
        return snprintf(buf, (size_t)len, "{\"gain\":%d}", fx->gain);
    return -1;
}

static void fake_midi(void *instance, const uint8_t *msg, int len, int source) {
    fake_fx_t *fx = (fake_fx_t *)instance;
    (void)msg;
    (void)len;
    (void)source;
    usleep(250);
    if (fx->gain < 0) fx->gain = 0;
}

static audio_fx_api_v2_t api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .create_instance = fake_create,
    .destroy_instance = fake_destroy,
    .process_block = fake_process,
    .set_param = fake_set,
    .get_param = fake_get,
    .on_midi = fake_midi,
};

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &api;
}
