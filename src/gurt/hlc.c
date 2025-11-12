/*
 * (C) Copyright 2019-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * This file is part of GURT. Hybrid Logical Clock (HLC) implementation.
 */
#include <gurt/common.h>	/* for NSEC_PER_SEC */
#include <gurt/atomic.h>
#include <time.h>
#include <stdint.h>

/**
 * HLC timestamp unit (given in the HLC timestamp value for 1 ns) (i.e.,
 * 1/16 ns, offering a 36-year range)
 */
#define D_HLC_NSEC 16ULL

/**
 * HLC start time (given in the Unix time for 2021-01-01 00:00:00 +0000 UTC in
 * seconds) (i.e., together with D_HLC_NSEC, offering a range of [2021, 2057])
 */
#define D_HLC_START_SEC 1609459200ULL

/** Mask for the 18 logical bits */
#define D_HLC_MASK 0x3FFFFULL

static ATOMIC uint64_t d_hlc;

/** See d_hlc_epsilon_set's API doc */
static uint64_t d_hlc_epsilon = 1ULL * NSEC_PER_SEC * D_HLC_NSEC;

/** Get local physical time */
static inline uint64_t d_hlc_localtime_get(void)
{
	struct timespec now;
	uint64_t	pt;
	int		rc;

	rc = clock_gettime(CLOCK_REALTIME, &now);
	D_ASSERTF(rc == 0, "clock_gettime: %d\n", errno);
	D_ASSERT(now.tv_sec > D_HLC_START_SEC);
	//Yuanguo:
	//  t_ns = {current-time} - {2021-01-01 00:00:00} 的 纳秒数
	//  pt = t_ns * D_HLC_NSEC;
	//  为什么乘以 D_HLC_NSEC (16) 呢？
	//  因为现在(2025年) t_ns 是60-bit，前面空4-bit，乘以16即左移4-bit；左移之后，后面的 “计数空间” 更大；
	//
	//           46-bit             18-bit
	//    +-----------------------+----------+
	//    |     pt的高46位        | 计数空间 |
	//    +-----------------------+----------+
	//
	//  感觉没有必要左移4-bit:
	//    - 32年之后(现在是2025年)，t_ns就61-bit了，左移4位，最高位就丢了！
	//    - 前面的注释也提及: offering a 36-year range, ..., offering a range of [2021, 2057], 写注释的时间是2021年；
	pt = ((now.tv_sec - D_HLC_START_SEC) * NSEC_PER_SEC + now.tv_nsec) *
	     D_HLC_NSEC;

	/** Return the most significant 46 bits of time. */
	return pt & ~D_HLC_MASK;
}

//Yuanguo:
//  本函数返回一个HLC (Hybrid Logical Clock) time; 如何转化为真实时间呢？
//    - step-1: 把低18-bit掩掉； 也可以省略这一步！因为这个计数单位是 1/16 ns，计数又不会太大！
//    - step-2: 除以16，得到纳秒；
//    - step-3: 除以1000000000，得到秒；
//    - step-4: 加上1609459200 (2021-01-01 00:00:00)
uint64_t d_hlc_get(void)
{
	uint64_t pt = d_hlc_localtime_get();
	uint64_t hlc, ret;

  //Yuanguo: atomic_compare_exchange(obj, expected, desired)
  //  比较*obj和expected：
  //      - 若相等，则把*obj设置为desired，返回true;
  //        对于本case来说，就是全局变量d_hlc没有被别的线程并发修改；所以本线程把它修改为ret；返回true，do-while结束；
  //      - 若不等，则把*obj load到expected，返回false;
  //        对于本case来说，就是全局变量d_hlc已有被别的线程并发修改；所以本线程load新值到hlc，然后重试；
  //  所以，机器不重启的情况下，本函数返回值肯定是不重复且递增的！
	do {
		hlc = d_hlc;
		ret = (hlc & ~D_HLC_MASK) < pt ? pt : (hlc + 1);
	} while (!atomic_compare_exchange(&d_hlc, hlc, ret));

	return ret;
}

int d_hlc_get_msg(uint64_t msg, uint64_t *hlc_out, uint64_t *offset)
{
	uint64_t pt = d_hlc_localtime_get();
	uint64_t hlc, ret, ml = msg & ~D_HLC_MASK;
	uint64_t off;

	off = ml > pt ? ml - pt : 0;

	if (offset != NULL)
		*offset = off;

	if (off > d_hlc_epsilon)
		return -DER_HLC_SYNC;

	do {
		hlc = d_hlc;
		if ((hlc & ~D_HLC_MASK) < ml)
			ret = ml < pt ? pt : (msg + 1);
		else if ((hlc & ~D_HLC_MASK) < pt)
			ret = pt;
		else if (pt <= ml)
			ret = (hlc < msg ? msg : hlc) + 1;
		else
			ret = hlc + 1;
	} while (!atomic_compare_exchange(&d_hlc, hlc, ret));

	if (hlc_out != NULL)
		*hlc_out = ret;
	return 0;
}

uint64_t d_hlc2nsec(uint64_t hlc)
{
	return hlc / D_HLC_NSEC;
}

uint64_t d_nsec2hlc(uint64_t nsec)
{
	return nsec * D_HLC_NSEC;
}

uint64_t d_hlc2unixnsec(uint64_t hlc)
{
	return hlc / D_HLC_NSEC + D_HLC_START_SEC * NSEC_PER_SEC;
}

int d_hlc2timespec(uint64_t hlc, struct timespec *ts)
{
	uint64_t nsec;

	if (ts == NULL)
		return -DER_INVAL;

	nsec = d_hlc2nsec(hlc);
	ts->tv_sec = nsec / NSEC_PER_SEC + D_HLC_START_SEC;
	ts->tv_nsec = nsec % NSEC_PER_SEC;
	return 0;
}

int d_timespec2hlc(struct timespec ts, uint64_t *hlc)
{
	uint64_t nsec;

	if (hlc == NULL)
		return -DER_INVAL;

	nsec = (ts.tv_sec - D_HLC_START_SEC) * NSEC_PER_SEC + ts.tv_nsec;
	*hlc = d_nsec2hlc(nsec);
	return 0;
}

uint64_t d_unixnsec2hlc(uint64_t unixnsec)
{
	uint64_t start = D_HLC_START_SEC * NSEC_PER_SEC;

	/*
	 * If the time represented by unixnsec is before the time represented
	 * by D_HLC_START_SEC, or after the maximum time representable, then
	 * the conversion is impossible.
	 */
	if (unixnsec < start || unixnsec - start > (uint64_t)-1 / D_HLC_NSEC)
		return 0;

	return (unixnsec - start) * D_HLC_NSEC;
}

void d_hlc_epsilon_set(uint64_t epsilon)
{
	d_hlc_epsilon = (epsilon + D_HLC_MASK) & ~D_HLC_MASK;
	D_INFO("set maximum system clock offset to "DF_U64" ns\n",
	       d_hlc_epsilon);
}

uint64_t d_hlc_epsilon_get(void)
{
	return d_hlc_epsilon;
}

uint64_t d_hlc_epsilon_get_bound(uint64_t hlc)
{
	return (hlc + d_hlc_epsilon) | D_HLC_MASK;
}

uint64_t d_hlc_age2sec(uint64_t hlc)
{
	uint64_t pt = d_hlc_localtime_get();

	if (unlikely(pt <= hlc))
		return 0;

	return d_hlc2sec(pt - hlc);
}
