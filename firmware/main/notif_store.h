#pragma once

/**
 * notif_store.h — Persistent per-sender notification thread store
 *
 * Messages are grouped by sender (title field from ANCS).
 * Each sender has a thread holding up to NS_MAX_MSG_PER_THREAD messages,
 * newest at index 0.  The store is persisted to NVS so history survives
 * reboots.  Threads are ordered most-recently-active first (index 0).
 */

#define NS_MAX_THREADS        6
#define NS_MAX_MSG_PER_THREAD 8
#define NS_SENDER_LEN        48
#define NS_CAT_LEN           20
#define NS_MSG_LEN           100
#define NS_TS_LEN            12

typedef struct {
    char ts[NS_TS_LEN];
    char text[NS_MSG_LEN];
} ns_msg_t;

typedef struct {
    char     sender[NS_SENDER_LEN];
    char     cat[NS_CAT_LEN];
    int      count;                          /* number of messages stored  */
    int      unread;                         /* messages not yet viewed    */
    ns_msg_t msgs[NS_MAX_MSG_PER_THREAD];   /* msgs[0] = newest           */
} ns_thread_t;

/**
 * Load persisted threads from NVS.
 * Call once after nvs_flash_init(), before any other notif_store call.
 */
void notif_store_init(void);

/**
 * Add a message.  If sender already has a thread it moves to index 0 and
 * the message is prepended.  If sender is new a thread is created; if the
 * store is full the oldest thread (highest index) is dropped.
 */
void notif_store_add(const char *cat, const char *sender,
                     const char *msg,  const char *timestamp);

/** Number of threads currently stored (0..NS_MAX_THREADS). */
int notif_store_count(void);

/** Return thread at idx (0 = most recently active).  NULL if out of range. */
const ns_thread_t *notif_store_get(int idx);

/** Mark all messages in thread idx as read (unread → 0).  Persists to NVS. */
void notif_store_mark_read(int idx);

/** Sum of unread counts across all threads. */
int notif_store_total_unread(void);
