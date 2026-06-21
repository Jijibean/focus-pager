/**
 * notif_store.c — Persistent per-sender notification thread store
 *
 * Threads are kept in RAM (s_threads[]) and written to NVS on every change.
 * NVS key scheme (all keys ≤ 15 chars):
 *   "ns_n"         — int32: thread count
 *   "ns_<i>_s"     — str:   sender for thread i
 *   "ns_<i>_c"     — str:   category for thread i
 *   "ns_<i>_n"     — int32: message count for thread i
 *   "ns_<i>_<j>_t" — str:   timestamp for message j of thread i
 *   "ns_<i>_<j>_m" — str:   text for message j of thread i
 */

#include "notif_store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG    "NS"
#define NVS_NS "notif_store"

static ns_thread_t s_threads[NS_MAX_THREADS];
static int         s_count = 0;

/* ── NVS key builders ─────────────────────────────────────────────────────── */

static void key_thread(char *buf, int ti, const char *suf)
{
    snprintf(buf, 16, "ns_%d_%s", ti, suf);
}

static void key_msg(char *buf, int ti, int mi, const char *suf)
{
    snprintf(buf, 16, "ns_%d_%d_%s", ti, mi, suf);
}

/* ── NVS persistence ──────────────────────────────────────────────────────── */

static void save_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed");
        return;
    }

    nvs_set_i32(h, "ns_n", (int32_t)s_count);

    for (int i = 0; i < s_count; i++) {
        char key[16];

        key_thread(key, i, "s");
        nvs_set_str(h, key, s_threads[i].sender);

        key_thread(key, i, "c");
        nvs_set_str(h, key, s_threads[i].cat);

        key_thread(key, i, "n");
        nvs_set_i32(h, key, (int32_t)s_threads[i].count);

        for (int j = 0; j < s_threads[i].count; j++) {
            key_msg(key, i, j, "t");
            nvs_set_str(h, key, s_threads[i].msgs[j].ts);

            key_msg(key, i, j, "m");
            nvs_set_str(h, key, s_threads[i].msgs[j].text);
        }
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGD(TAG, "Saved %d threads", s_count);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void notif_store_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No persisted store — starting fresh");
        return;
    }

    int32_t cnt = 0;
    if (nvs_get_i32(h, "ns_n", &cnt) != ESP_OK || cnt <= 0) {
        nvs_close(h);
        return;
    }
    if (cnt > NS_MAX_THREADS) cnt = NS_MAX_THREADS;
    s_count = (int)cnt;

    for (int i = 0; i < s_count; i++) {
        char key[16];
        size_t sz;

        sz = NS_SENDER_LEN;
        key_thread(key, i, "s");
        nvs_get_str(h, key, s_threads[i].sender, &sz);

        sz = NS_CAT_LEN;
        key_thread(key, i, "c");
        nvs_get_str(h, key, s_threads[i].cat, &sz);

        int32_t mc = 0;
        key_thread(key, i, "n");
        nvs_get_i32(h, key, &mc);
        if (mc > NS_MAX_MSG_PER_THREAD) mc = NS_MAX_MSG_PER_THREAD;
        s_threads[i].count = (int)mc;

        for (int j = 0; j < s_threads[i].count; j++) {
            sz = NS_TS_LEN;
            key_msg(key, i, j, "t");
            nvs_get_str(h, key, s_threads[i].msgs[j].ts, &sz);

            sz = NS_MSG_LEN;
            key_msg(key, i, j, "m");
            nvs_get_str(h, key, s_threads[i].msgs[j].text, &sz);
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d threads from NVS", s_count);
}

void notif_store_add(const char *cat, const char *sender,
                     const char *msg,  const char *timestamp)
{
    if (!sender)    sender    = "";
    if (!cat)       cat       = "";
    if (!msg)       msg       = "";
    if (!timestamp) timestamp = "";

    /* Find existing thread for this sender */
    int found = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_threads[i].sender, sender) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        /* New sender — drop oldest thread if store is full */
        if (s_count >= NS_MAX_THREADS)
            s_count = NS_MAX_THREADS - 1;   /* last slot will be overwritten */

        /* Shift existing threads right to free index 0 */
        memmove(&s_threads[1], &s_threads[0],
                (size_t)s_count * sizeof(ns_thread_t));
        memset(&s_threads[0], 0, sizeof(ns_thread_t));
        strncpy(s_threads[0].sender, sender, NS_SENDER_LEN - 1);
        strncpy(s_threads[0].cat,    cat,    NS_CAT_LEN    - 1);
        s_threads[0].count = 0;
        s_count++;
        found = 0;
    } else if (found > 0) {
        /* Move existing thread to front */
        ns_thread_t tmp = s_threads[found];
        memmove(&s_threads[1], &s_threads[0],
                (size_t)found * sizeof(ns_thread_t));
        s_threads[0] = tmp;
        found = 0;
    }

    /* Prepend message to thread (newest at index 0) */
    ns_thread_t *t = &s_threads[0];
    int newcount = (t->count < NS_MAX_MSG_PER_THREAD)
                   ? t->count + 1 : NS_MAX_MSG_PER_THREAD;
    memmove(&t->msgs[1], &t->msgs[0],
            (size_t)(newcount - 1) * sizeof(ns_msg_t));
    t->count = newcount;
    strncpy(t->msgs[0].ts,   timestamp, NS_TS_LEN  - 1);
    strncpy(t->msgs[0].text, msg,       NS_MSG_LEN - 1);
    t->msgs[0].ts[NS_TS_LEN   - 1] = '\0';
    t->msgs[0].text[NS_MSG_LEN - 1] = '\0';

    /* Update category to latest */
    strncpy(t->cat, cat, NS_CAT_LEN - 1);
    t->cat[NS_CAT_LEN - 1] = '\0';

    ESP_LOGI(TAG, "Added msg to '%s' (thread %d, %d msgs)", t->sender, found, t->count);
    save_all();
}

int notif_store_count(void)
{
    return s_count;
}

const ns_thread_t *notif_store_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_threads[idx];
}
