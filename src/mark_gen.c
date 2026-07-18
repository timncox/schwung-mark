/*
 * Mark sound_generator/overtake build — standalone 5-track live looper.
 *
 * Declares capabilities.audio_in and reads the hardware input directly
 * from the host mailbox (host->mapped_memory + host->audio_in_offset),
 * like the in-tree linein module. Input routing follows whatever input
 * was last selected in stock Move (mic / line / USB-C via the XMOS mux).
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "mark_core.h"

static const host_api_v1_t *g_host;

static void *gen_create(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    mark_t *m = mark_create_in_dir(g_host, module_dir);
    /* this build reads the hardware input directly — the UI keys its
     * feedback guard off this flag */
    if (m) {
        mark_set_param(m, "hw_input", "1");
        /* sessions live OUTSIDE the module dir so reinstalls keep them */
        mark_set_param(m, "session_dir",
                       "/data/UserData/schwung/mark-sessions");
    }
    return m;
}

static void gen_destroy(void *inst) { mark_destroy((mark_t *)inst); }

static void gen_render(void *inst, int16_t *out_lr, int frames) {
    const int16_t *in = NULL;
    if (g_host && g_host->mapped_memory)
        in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    if (!in) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    mark_process((mark_t *)inst, in, out_lr, frames);
}

static void gen_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    mark_on_midi((mark_t *)inst, msg, len, source);
}

static void gen_set_param(void *inst, const char *key, const char *val) {
    mark_set_param((mark_t *)inst, key, val);
}

static int gen_get_param(void *inst, const char *key, char *buf, int buf_len) {
    /* schwung-manager discovers the active overtake tool by probing
     * overtake_dsp:module_id — the shim forwards it to the DSP, so the
     * plugin must answer (smack lesson: silence = "no tool loaded"). */
    if (!strcmp(key, "module_id"))
        return snprintf(buf, (size_t)buf_len, "mark");
    return mark_get_param((mark_t *)inst, key, buf, buf_len);
}

static int gen_get_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len;
    return 0;
}

static plugin_api_v2_t api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = gen_create,
    .destroy_instance = gen_destroy,
    .on_midi          = gen_on_midi,
    .set_param        = gen_set_param,
    .get_param        = gen_get_param,
    .get_error        = gen_get_error,
    .render_block     = gen_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
