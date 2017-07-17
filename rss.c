#define _GNU_SOURCE
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "table.h"

static inline uint32_t raw_ctz(uint32_t n) { return __builtin_ctz(n); }
static inline uint32_t zero_rightmost_1bit(uint32_t x) { return x & (x - 1); }

static inline uint32_t base_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	return input_tuple[0] ^ input_tuple[1] ^ input_tuple[2];
}

static inline uint32_t sup_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint8_t *input = (uint8_t *)input_tuple;
	uint32_t len = input_len * (sizeof(uint32_t)/sizeof(uint8_t));
	uint32_t result = 0;
	for (uint32_t idx = 0; idx < len; ++idx) {
		uint8_t value = input[idx];
		if ((value & 0X80)) { result ^= session_get_fast_mask(idx, 0); }
		if ((value & 0X40)) { result ^= session_get_fast_mask(idx, 1); }
		if ((value & 0X20)) { result ^= session_get_fast_mask(idx, 2); }
		if ((value & 0X10)) { result ^= session_get_fast_mask(idx, 3); }
		if ((value & 0X08)) { result ^= session_get_fast_mask(idx, 4); }
		if ((value & 0X04)) { result ^= session_get_fast_mask(idx, 5); }
		if ((value & 0X02)) { result ^= session_get_fast_mask(idx, 6); }
		if ((value & 0X01)) { result ^= session_get_fast_mask(idx, 7); }
	}
	return result;
}

static inline uint32_t sup2_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint8_t *input = (uint8_t *)input_tuple;
	uint32_t len = input_len * (sizeof(uint32_t)/sizeof(uint8_t));
	uint32_t result = 0;
	for (uint32_t idx = 0; idx < len; ++idx) {
		for (uint8_t j, map = input[idx];
			map && ((j = raw_ctz(map)), true);
			map = zero_rightmost_1bit(map)) {
			result ^= session_get_fast_mask(idx, 7 - j);
		}
	}
	return result;
}

static inline uint32_t sup3_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint8_t *input = (uint8_t *)input_tuple;
	uint32_t len = input_len * (sizeof(uint32_t)/sizeof(uint8_t));
	uint32_t result = 0;
	for (uint32_t idx = 0; idx < len; ++idx) {
		for (uint8_t j, map = input[idx];
			map && ((j = raw_ctz(map)), true);
			map = zero_rightmost_1bit(map)) {
			result ^= session_get_fast_mask2(idx, 7 - j);
		}
	}
	return result;
}

static inline uint32_t dpdk_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint32_t i, j, ret = 0;
	for (j = 0; j < input_len; j++) {
		for (i = 0; i < 32; i++) {
			if (input_tuple[j] & (1 << (31 - i))) {
				ret ^= ((const uint32_t *)default_rss_key)[j] << i |
					(uint32_t)((uint64_t)(((const uint32_t *)default_rss_key)[j + 1]) >> (32 - i));
			}
		}
	}
	return ret;
}

static inline uint32_t dpdk2_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint32_t i, map, j, ret = 0;
	for (j = 0; j < input_len; j++) {
    	for (uint32_t map__ = input_tuple[j]; 
			map__ && ((i = raw_ctz(map__)), true); 
			map__ = zero_rightmost_1bit(map__)) {
			ret ^= ((const uint32_t *)default_rss_key)[j] << (31 - i) |
				(uint32_t)((uint64_t)(((const uint32_t *)default_rss_key)[j + 1]) >> (i + 1));
		}
	}
	return ret;
}

static inline uint32_t dpdk3_rss(const uint32_t *input_tuple, uint32_t input_len)
{
	uint32_t i, map, ret = 0;
#define DPDK3_RSS(round) \
   	for (uint32_t map__ = input_tuple[round]; \
		map__ && ((i = raw_ctz(map__)), true); \
		map__ = zero_rightmost_1bit(map__)) { \
		ret ^= ((const uint32_t *)default_rss_key)[round] << (31 - i) | \
			(uint32_t)((uint64_t)(((const uint32_t *)default_rss_key)[round + 1]) >> (i + 1)); \
	}

	DPDK3_RSS(0)
	DPDK3_RSS(1)
	DPDK3_RSS(2)
	return ret;
}

uint32_t sse_rss();

static uint32_t verify_tuple[TUPLE_LEN];
static uint32_t tuples[TUPLE_NUM][TUPLE_LEN];

static inline void get_tuple(uint32_t *tuple, uint32_t len)
{
	for (uint32_t i = 0; i < len; ++i) {
		tuple[i] = rand();
	}
}

static inline void init_tuples()
{
	srand(0);
	for (uint32_t i = 0; i < TUPLE_NUM; ++i) {
		get_tuple(tuples[i], TUPLE_LEN);
	}
}

static inline void prefetch(const void *p)
{
	asm volatile("prefetcht0 %[p]" : [p] "+m" (*(volatile char *)p));
}

static inline uint64_t rdtsc(void)
{
	union {
		uint64_t tsc_64;
		struct {
			uint32_t lo_32;
			uint32_t hi_32;
		};
	} tsc;
	asm volatile("rdtsc" :
		     "=a" (tsc.lo_32),
		     "=d" (tsc.hi_32));
	return tsc.tsc_64;
}

typedef uint32_t (*rss_fun_t)(uint32_t *input_tuple, ...);

typedef struct {
	char name[32];
	rss_fun_t func;
} rss_t;

static rss_t rsss[] = {
	{"base", (rss_fun_t)base_rss}, 
	{"sup", (rss_fun_t)sup_rss}, 
	{"sup2", (rss_fun_t)sup2_rss}, 
	{"sup3", (rss_fun_t)sup3_rss}, 
	{"dpdk", (rss_fun_t)dpdk_rss}, 
	{"dpdk2", (rss_fun_t)dpdk2_rss}, 
	{"dpdk3", (rss_fun_t)dpdk3_rss}, 
};

int main(int argc, char *argv[])
{
	init_tuples();
	if (argc < 2) {
		printf("invalid parameter.\n");
		return -1;
	}
	cpu_set_t cs;
	CPU_ZERO(&cs);
	CPU_SET(1, &cs);
	sched_setaffinity(getpid(), CPU_COUNT(&cs), &cs);

	printf("base bench:\n");
	uint64_t count = 0, start = rdtsc(), end;
	for (size_t i = 0; i < TUPLE_NUM; i+=6) {
		prefetch(tuples[i]);
	}
	end = rdtsc();
	printf("\tpretch: cycles: %lu, average = %lu\n", (end - start), (end - start)/(TUPLE_NUM/6));

	printf("round1 normal test:\n");
	for (size_t i = 1; i < argc; ++i) {
		uint64_t sum = 0, start = rdtsc(), end;
		for (size_t j = 0; j < sizeof(rsss)/sizeof(rss_t); ++j) {
			if (!strcmp(argv[i], rsss[j].name)) {
				for (size_t k = 0; k < TUPLE_NUM; ++k) {
					sum += rsss[j].func(tuples[k], TUPLE_LEN);
				}
			}
		}
		end = rdtsc();
		printf("\t%s: sum = %lu, cycles = %lu, average = %lu\n", argv[i], sum, (end - start), (end - start)/TUPLE_NUM);
	}

	printf("round2 normal+loop-expand test:\n");
	for (size_t i = 1; i < argc; ++i) {
		uint64_t sum = 0, start = rdtsc(), end;
		for (size_t j = 0; j < sizeof(rsss)/sizeof(rss_t); ++j) {
			if (!strcmp(argv[i], rsss[j].name)) {
				for (size_t k = 0; k < TUPLE_NUM; k+=10) {
					sum += rsss[j].func(tuples[k], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+1], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+2], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+3], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+4], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+5], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+6], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+7], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+8], TUPLE_LEN);
					sum += rsss[j].func(tuples[k+9], TUPLE_LEN);
				}
			}
		}
		end = rdtsc();
		printf("\t%s: sum = %lu, cycles = %lu, average = %lu\n", argv[i], sum, (end - start), (end - start)/TUPLE_NUM);
	}

#define RSS_TEST(name) \
do { \
	uint64_t sum = 0, start = rdtsc(), end; \
	for (size_t k = 0; k < TUPLE_NUM; ++k) { \
		sum += name##_rss(tuples[k], TUPLE_LEN); \
	} \
	end = rdtsc(); \
	printf("\t%s: sum = %lu, cycles = %lu, average = %lu\n", #name, sum, (end - start), (end - start)/TUPLE_NUM); \
} while (0)

	printf("round3 inline test:\n");
	RSS_TEST(base);
	RSS_TEST(sup);
	RSS_TEST(sup2);
	RSS_TEST(sup3);
	RSS_TEST(dpdk);
	RSS_TEST(dpdk2);
	RSS_TEST(dpdk3);

#define RSS_TEST2(name) \
do { \
	uint64_t sum = 0, start = rdtsc(), end; \
	for (size_t k = 0; k < TUPLE_NUM; k+=0x10) { \
		sum += name##_rss(tuples[k], TUPLE_LEN); \
		sum += name##_rss(tuples[k+1], TUPLE_LEN); \
		sum += name##_rss(tuples[k+2], TUPLE_LEN); \
		sum += name##_rss(tuples[k+3], TUPLE_LEN); \
		sum += name##_rss(tuples[k+4], TUPLE_LEN); \
		sum += name##_rss(tuples[k+5], TUPLE_LEN); \
		sum += name##_rss(tuples[k+6], TUPLE_LEN); \
		sum += name##_rss(tuples[k+7], TUPLE_LEN); \
		sum += name##_rss(tuples[k+8], TUPLE_LEN); \
		sum += name##_rss(tuples[k+9], TUPLE_LEN); \
		sum += name##_rss(tuples[k+10], TUPLE_LEN); \
		sum += name##_rss(tuples[k+11], TUPLE_LEN); \
		sum += name##_rss(tuples[k+12], TUPLE_LEN); \
		sum += name##_rss(tuples[k+13], TUPLE_LEN); \
		sum += name##_rss(tuples[k+14], TUPLE_LEN); \
		sum += name##_rss(tuples[k+15], TUPLE_LEN); \
	} \
	end = rdtsc(); \
	printf("\t%s: sum = %lu, cycles = %lu, average = %lu\n", #name, sum, (end - start), (end - start)/TUPLE_NUM); \
} while (0)

	printf("round4 inline+loop-expand test:\n");
	RSS_TEST2(base);
	RSS_TEST2(sup);
	RSS_TEST2(sup2);
	RSS_TEST2(sup3);
	RSS_TEST2(dpdk);
	RSS_TEST2(dpdk2);
	RSS_TEST2(dpdk3);
	return 0;
}
