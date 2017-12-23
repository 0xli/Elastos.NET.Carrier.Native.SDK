#ifndef __THISTORY_H__
#define __THISTORY_H__

#include <string.h>
#include <assert.h>

#include <linkedhashtable.h>

#include "ela_carrier.h"

typedef struct HistoryItem {
    HashEntry he;
    char key[ELA_MAX_ID_LEN + ELA_MAX_EXTENSION_NAME_LEN + 4];
    int64_t tid;
    void *user_data;
} HistoryItem;

static
int hid_compare(const void *key1, size_t len1, const void *key2, size_t len2)
{
    assert(key1 && strlen(key1) == len1);
    assert(key2 && strlen(key2) == len2);

    if (len1 > len2)
        return 1;

    if (len1 < len2)
        return -1;

    return strcmp(key1, key2);
}

static inline
Hashtable *transaction_history_create(int capacity)
{
    return hashtable_create(capacity, 1, NULL, hid_compare);
}

/* Invite request history from other users */
static inline
void tansaction_history_put_invite(Hashtable *thistory,
                                   const char *userid, int64_t tid)
{
    HistoryItem *item;

    assert(thistory && userid && tid);

    item = rc_alloc(sizeof(HistoryItem), NULL);
    sprintf(item->key, "ir_%s", userid);
    item->tid = tid;

    item->he.data = item;
    item->he.key = item->key;
    item->he.keylen = strlen(item->key);

    hashtable_put(thistory, &item->he);
    deref(item);
}

static inline
int64_t transaction_history_get_invite(Hashtable *thistory, const char *userid)
{
    int64_t val = 0;
    HistoryItem *item;
    char key[ELA_MAX_ID_LEN + ELA_MAX_EXTENSION_NAME_LEN + 4];

    assert(thistory && userid);

    sprintf(key, "ir_%s", userid);
    item = hashtable_get(thistory, key, strlen(key));
    if (item) {
        val = item->tid;
        deref(item);
    }

    return val;
}

static inline
void transaction_history_remove_invite(Hashtable *thistory, const char *userid)
{
    char key[ELA_MAX_ID_LEN  + ELA_MAX_EXTENSION_NAME_LEN + 4];

    assert(thistory && userid);

    sprintf(key, "ir_%s", userid);
    deref(hashtable_remove(thistory, key, strlen(key)));
}

#endif /* __THISTORY_H__ */