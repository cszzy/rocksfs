#pragma once

#include "common.h"
#include <mutex>

namespace metafs {



struct bitmap
{
	unsigned long cnt, free_cnt, siz;
	unsigned long data[0];
	std::mutex mutex_;
};

static inline struct bitmap *create_bitmap(unsigned long cnt)
{
	struct bitmap *bp;
	unsigned long siz;
	siz = ALIGN_UP(cnt, 64);
	bp = (struct bitmap *)safe_align(sizeof(bitmap) + (siz / 64) * sizeof(unsigned long), CL_SIZE, true);
	bp->cnt = cnt;
	bp->free_cnt = cnt;
	for (unsigned long i = cnt; i < siz; i++)
		bp->data[i >> 6] |= 1UL << (i & 63);
	bp->siz = siz;
	return bp;
}

static inline bool bitmap_full(struct bitmap *bp)
{
	std::lock_guard<std::mutex> lock(bp->mutex_);
	return bp->free_cnt == 0;
}

static inline int get_free(struct bitmap *bp)
{
	std::lock_guard<std::mutex> lock(bp->mutex_);
	unsigned long tot, i, ii, j;
	unsigned long old_free_cnt, old_val;
	do
	{
		old_free_cnt = bp->free_cnt;
		if (unlikely(old_free_cnt == 0))
			return -1;
	} while (unlikely(!cmpxchg(&bp->free_cnt, old_free_cnt, old_free_cnt - 1)));

	tot = bp->siz / 64;
	for (i = 0; i < tot; i++)
	{
		for (;;)
		{
			old_val = bp->data[i];
			if (old_val == (unsigned long)-1)
				break;
			j = __builtin_ffsl(old_val + 1) - 1;
			if (cmpxchg(&bp->data[i], old_val, old_val | (1UL << j)))
				return (i << 6) | j;
		}
	}
	p_assert(0, "no way");
	return 0;
}

static inline void put_back(struct bitmap *bp, int bk)
{
	std::lock_guard<std::mutex> lock(bp->mutex_);
	unsigned long old_val;
	p_assert((bp->data[bk >> 6] >> (bk & 63)) & 1, "wrong");
	do
	{
		old_val = bp->data[bk >> 6];
	} while (unlikely(!cmpxchg(&bp->data[bk >> 6], old_val, old_val ^ (1UL << (bk & 63)))));
	atomic_inc(&bp->free_cnt);
}

}
