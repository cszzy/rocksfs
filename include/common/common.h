#pragma once

#include <stdint.h>
#include <stddef.h> //offsetof
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <malloc.h>
#include <errno.h>

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2 * !!(condition)]))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define UNUSED(x) ((void)(x))
#define p_info(fmt, ...) printf("MetaFS info: " fmt "\n", ##__VA_ARGS__)
#define p_err(fmt, ...) fprintf(stderr, "%s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define p_assert(b, fmt, ...)                                              \
	if (unlikely(!(b)))                                                      \
	{                                                                        \
		fprintf(stderr, "%s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
		*(long *)0 = 0;                                                        \
		exit(100);                                                             \
	}
#define barrier() asm volatile("" \
															 :  \
															 :  \
															 : "memory")
#define dump_on(x)     \
	{                    \
		if (unlikely(x))   \
		{                  \
			*((int *)0) = 1; \
		}                  \
	}
#define setoffval(ptr, off, type) (*(type *)((char *)(ptr) + (int)(off)))

#define rmb() asm volatile("lfence" :: \
															 : "memory")
#define wmb() asm volatile("sfence" :: \
															 : "memory")
#define mb() asm volatile("mfence" :: \
															: "memory")
#define cpu_relax() asm volatile("pause\n" \
																 :         \
																 :         \
																 : "memory")

#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))
#define cmpxchg(P, O, N) __sync_bool_compare_and_swap((P), (O), (N))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
#define atomic_dec(P) __sync_add_and_fetch((P), -1)
#define atomic_add(P, V) __sync_add_and_fetch((P), (V))
#define atomic_set_bit(P, V) __sync_or_and_fetch((P), 1 << (V))
#define atomic_clear_bit(P, V) __sync_and_and_fetch((P), ~(1 << (V)))

#define ALIGN_UP(a, siz) (((a) + (siz)-1) & (~((siz)-1)))
#define ALIGN_DOWN(a, siz) ((a) & (~((siz)-1)))

#define FN_LEN 256
#define CL_SFT 6
#define CL_SIZE (1 << CL_SFT)
#define CL_MASK (CL_SIZE - 1)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

namespace metafs {

static inline unsigned th_rand(unsigned *seed)
{
	return *seed = *seed * 1103515245L + 12345L;
}

static inline void *safe_alloc(size_t siz, bool clear)
{
	void *mem;
	mem = malloc(siz);
	dump_on(mem == NULL);
	if (clear)
		memset(mem, 0, siz);
	return mem;
}
static inline void *safe_align(size_t siz, size_t ali, bool clear)
{
	void *mem;
	mem = memalign(ali, siz);
	dump_on(mem == NULL);
	if (clear)
		memset(mem, 0, siz);
	return mem;
}
static inline pthread_t safe_thread_create(void *(*routine)(void *), void *p)
{
	pthread_t thd;
	if (routine == NULL || pthread_create(&thd, NULL, routine, p))
	{
		p_assert(0, "thread create wrong");
	}
	return thd;
}

static inline void *safe_get_hugepage(char *fn, size_t siz)
{
	void *addr;
	int fd = open(fn, O_CREAT | O_RDWR, 0600);
	p_info("get hugepage %s", fn);
	p_assert(fd > 0, "get hugepage wrong %d", errno);
	addr = mmap(NULL, siz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	p_assert(addr != MAP_FAILED, "");
	return addr;
}

static inline char *strdump(const char *s)
{
	size_t len = 0;
	char *copy = NULL;
	if (s == NULL)
		return NULL;
	len = strlen(s) + sizeof("");
	copy = (char *)safe_alloc(len, false);
	memcpy(copy, s, len);
	return copy;
}
static inline unsigned long str_hash(const char *s, int len)
{
	register unsigned long res = 0;
	register int i;
	for (i = 0; i < len; i++)
	{
		res = res * 131 + s[i];
	}
	return res;
}
static inline unsigned long long_hash(unsigned long v)
{
	return (v << 53) | (v >> 11);
}

static inline void bind_cpu(unsigned lcoreid)
{
	int ret;
	cpu_set_t cpu_mask;
	CPU_ZERO(&cpu_mask);
	CPU_SET(lcoreid, &cpu_mask);
	ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_mask);
	p_assert(ret == 0, "");
}

static inline unsigned long rdtsc(void)
{
	union
	{
		unsigned long tsc_64;
		struct
		{
			unsigned lo_32;
			unsigned hi_32;
		};
	} tsc;

	asm volatile("rdtsc"
							 : "=a"(tsc.lo_32),
								 "=d"(tsc.hi_32));
	return tsc.tsc_64;
}

} // namespace indexfs

