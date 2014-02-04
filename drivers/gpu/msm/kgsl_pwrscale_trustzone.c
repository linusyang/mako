/* Copyright (c) 2010-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <mach/socinfo.h>
#include <mach/scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

static void tz_wake(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	if (device->state != KGSL_STATE_NAP)
		kgsl_pwrctrl_pwrlevel_change(device,
					device->pwrctrl.default_pwrlevel);
}

/*** extern var ***/
bool gpu_idle;
short idle_counter;

//#define DEBUG

#define SAMPLE_TIME_MS 20

#define HISTORY_SIZE 6
#define GPU_IDLE_THRESHOLD 10

static void gpu_idle_detection(struct kgsl_device *device, int load)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	if ((pwr->active_pwrlevel >= pwr->min_pwrlevel - 1) 
				&& (GPU_IDLE_THRESHOLD >= load))
	{
		if (idle_counter < HISTORY_SIZE)
			idle_counter += 1;
	}
	else if (idle_counter > 0)
		idle_counter -= 2;
	
	if (idle_counter >= HISTORY_SIZE)
		gpu_idle = true;
	else if (idle_counter <= 0)
		gpu_idle = false;
}

#define GO_HIGHSPEED_LOAD 90

static unsigned int interactive_load[4][2] = {
	{100,30},
	{60,25},
	{50,20},
	{40,0}};

unsigned int up_threshold(int gpu_state){
	return interactive_load[gpu_state][0]; }
unsigned int down_threshold(int gpu_state){
	return interactive_load[gpu_state][1]; }

static int interactive_governor(struct kgsl_device *device, int load)
{
	int val = 0;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	if (load >= GO_HIGHSPEED_LOAD)
	{
		if (pwr->active_pwrlevel > pwr->max_pwrlevel)
		{
			val = -(pwr->active_pwrlevel);
		}
	}
	else if (load >= up_threshold(pwr->active_pwrlevel))
	{
		if (pwr->active_pwrlevel > pwr->max_pwrlevel)
			val = -1;
	}
	else if (load < down_threshold(pwr->active_pwrlevel))
	{
		if (pwr->active_pwrlevel < pwr->min_pwrlevel)
			val = 1;
	}

#ifdef DEBUG
	pr_info("------------------------------------------------");
	pr_info("GPU frequency:\t\t%d\n", 
		pwr->pwrlevels[pwr->active_pwrlevel].gpu_freq/1000000);
	pr_info("load:\t\t\t%d",load);
	pr_info("up_threshold:\t\t%u",
		up_threshold(pwr->active_pwrlevel));
	pr_info("down_threshold:\t\t%u",
		down_threshold(pwr->active_pwrlevel));
	pr_info("pwr->active_pwrlevel:\t%d",pwr->active_pwrlevel);
	pr_info("------------------------------------------------");
	if(gpu_idle){pr_info("GPU IDLE");}
	else{pr_info("GPU BUSY");}
	pr_info("Idle counter:\t\t%d",idle_counter);
	pr_info("------------------------------------------------");
#endif

	return val;
}

static u64 time_stamp;
static unsigned long sum_total_time, sum_busy_time;

static void tz_idle(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct kgsl_power_stats stats;
	unsigned long load = 0;
	int val = 0;
	bool idle_calc_ready = false;
	u64 now = ktime_to_ms(ktime_get());

	device->ftbl->power_stats(device, &stats);
	sum_total_time += (unsigned long)stats.total_time;
	sum_busy_time += (unsigned long)stats.busy_time;

	if (time_stamp < now)
	{
		if (sum_busy_time > 0 && sum_total_time > 0)
			load = (100 * sum_busy_time) / sum_total_time;
		else
			load = 0;
		idle_calc_ready = true;
		sum_total_time = sum_busy_time = 0;
		time_stamp = now + SAMPLE_TIME_MS;
		gpu_idle_detection(device, load);
	}

	if (idle_calc_ready)
		val = interactive_governor(device, load);

	if (val)
		kgsl_pwrctrl_pwrlevel_change(device,
					     pwr->active_pwrlevel + val);
}

static void tz_busy(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void tz_sleep(struct kgsl_device *device,
	struct kgsl_pwrscale *pwrscale)
{
	time_stamp = ktime_to_ms(ktime_get()) + SAMPLE_TIME_MS;

	gpu_idle = true;
	idle_counter = HISTORY_SIZE;
}

#ifdef CONFIG_MSM_SCM
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return 0;
}
#else
static int tz_init(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
	return -EINVAL;
}
#endif /* CONFIG_MSM_SCM */

static void tz_close(struct kgsl_device *device, struct kgsl_pwrscale *pwrscale)
{
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_tz = {
	.name = "trustzone",
	.init = tz_init,
	.busy = tz_busy,
	.idle = tz_idle,
	.sleep = tz_sleep,
	.wake = tz_wake,
	.close = tz_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_tz);
