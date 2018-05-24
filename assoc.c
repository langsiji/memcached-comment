/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

// mc的键/值关联模块，使用hash表来管理
// 当hash表的节点数大于hash表桶个数的1.5倍，就会rehash
// 对hash表扩展增加桶个数，桶个数bucket呈指数增长(hashpower)

// 比较关键的点就是mc通过一个单独的线程来监听
// 需要扩展hash表消息，当收到扩展hash表消息时，不是
// 马上加锁，一次性处理整个hash表的rehash工作，而是
// 加锁一次，将hash_bulk_move个hash表桶下的元素进行rehash
// 放到新的hash表中，这么做的原因主要是考虑到效率，
// 防止当桶很多，处理hash表rehash占用cpu且和锁
// 导致其他请求得不到处理，可以看到在下面对
// hash的find/insert/delete操作，都会先判断是否正在扩展
// hash表(expanding表示是否在扩展)
static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;


typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */

/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;

#define hashsize(n) ((ub4)1<<(n))
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
static item** primary_hashtable = 0;

/*
 * Previous hash table. During expansion, we look here for keys that haven't
 * been moved over to the primary yet.
 */
static item** old_hashtable = 0;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

/* Flag: Are we in the middle of expanding now? */
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
static unsigned int expand_bucket = 0; // 已扩展到第几个桶

void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;
    }
    primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize(hashpower) * sizeof(void *);
    STATS_UNLOCK();
}

item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

    item *ret = NULL;
    int depth = 0;
    while (it) {
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */

static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;
	// 当hash表节点数大于bucket*1.5之后,内部会进行扩容,
	// mc使用了一个线程来对hash表进行扩容,expanding表示
	// 正在扩容,expand_bucket表示当前rehash到哪个桶

	// 这条判断语句表示如果正在扩容,其所找的节点
	// 所在的桶还未被扩容到新的hash表primary_hashtable
	// 那么就从旧的hash表old_hashtable从获取
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
    return pos;
}

/* grows the hashtable to the next power of 2. */
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;

    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++; // 指数增长
        expanding = true;
        expand_bucket = 0;
        STATS_LOCK();
        stats.hash_power_level = hashpower;
        stats.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats.hash_is_expanding = 1;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;
        /* Bad news, but we can keep running. */
    }
}

/* 发送expand通知到expand thread*/
static void assoc_start_expand(void) {
    if (started_expanding)
        return;
    started_expanding = true;
    pthread_cond_signal(&maintenance_cond);
}

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
    	// 正在扩展hash表，且节点在旧的hash表上，就对旧的
    	// hash表操作，将节点插入
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }

    hash_items++;
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {
        assoc_start_expand();
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

// 从hash表中删除
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);

    if (*before) {
        item *nxt;
        hash_items--;
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}


static volatile int do_run_maintenance_thread = 1;

#define DEFAULT_HASH_BULK_MOVE 1
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE; /* 扩展hash表的时候，一次轮询扩展多少个桶*/

/* 扩展hash表工作线程，监听expand消息并做处理*/
static void *assoc_maintenance_thread(void *arg) {

    while (do_run_maintenance_thread) {
        int ii = 0;

        /* Lock the cache, and bulk move multiple buckets to the new
         * hash table. */
        item_lock_global();
        mutex_lock(&cache_lock);
		/* 每次只hash hash_bulk_move个桶，而不是一次性hash整个哈希表，
		     防止长期占用cache_lock，导致其他线程阻塞
		*/
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
			// 重新hash    键/值
            for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
                next = it->h_next;
				// 插入新表
                bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);
                it->h_next = primary_hashtable[bucket];
                primary_hashtable[bucket] = it;
            }

            old_hashtable[expand_bucket] = NULL;

            expand_bucket++;
            if (expand_bucket == hashsize(hashpower - 1)) {
                expanding = false;
                free(old_hashtable);
                STATS_LOCK();
                stats.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                stats.hash_is_expanding = 0;
                STATS_UNLOCK();
                if (settings.verbose > 1)
                    fprintf(stderr, "Hash table expansion done\n");
            }
        }

        mutex_unlock(&cache_lock);
        item_unlock_global();

        if (!expanding) {
            /* finished expanding. tell all threads to use fine-grained locks */
			//进入到这里，说明已经不需要迁移数据(停止扩展了) 
            //告诉所有的workers线程，访问item时，切换到段级别的锁
            //会阻塞到所有workers线程都切换到段级别的锁 
            switch_item_lock_type(ITEM_LOCK_GRANULAR);
            slabs_rebalancer_resume(); // pthread_mutex_unlock(&slabs_rebalance_lock)
            /* We are done expanding.. just wait for next invocation */
            mutex_lock(&cache_lock);
            started_expanding = false;
			// 等待expand通知扩展hash
			//直到别的线程插入数据后发现item数量已经到了1.5倍哈希表大小，  
            //此时调用别的线程调用assoc_start_expand函数，该函数会调用pthread_cond_signal  
            //唤醒扩展线程 
            pthread_cond_wait(&maintenance_cond, &cache_lock); 
            /* Before doing anything, tell threads to use a global lock */
            mutex_unlock(&cache_lock);
            slabs_rebalancer_pause(); // pthread_mutex_lock(&slabs_rebalance_lock)
            // 开始扩展哈希表，暂时切换到全局锁
            // 因为在扩展期间，无法通过hash item定位
            // 段锁，所有的workers线程和迁移线程一起，
            // 争抢全局级别的锁
            switch_item_lock_type(ITEM_LOCK_GLOBAL);
            mutex_lock(&cache_lock);
            assoc_expand(); // 对关键变量初始化，开始扩展
            mutex_unlock(&cache_lock);
        }
    }
    return NULL;
}

static pthread_t maintenance_tid;

/* 开启assoc expand线程*/
int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;
        }
    }
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}
/* 关闭assoc expand线程*/
void stop_assoc_maintenance_thread() {
    mutex_lock(&cache_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    mutex_unlock(&cache_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}


