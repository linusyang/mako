/*
 * Copyright (c) 2013, Francisco Franco. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#include <linux/hotplug.h>

#define MIM_TIME_INTERVAL_MS 10

/* extern */
bool is_touching;
unsigned long freq_boosted_time;
unsigned long time_stamp;

void touchboost_func(void);
unsigned int get_input_boost_freq(void);

static struct workqueue_struct *input_boost_wq;
static struct work_struct input_boost_work;

static void do_input_boost(struct work_struct *work)
{
	touchboost_func();
}

static void boost_input_event(struct input_handle *handle,
                unsigned int type, unsigned int code, int value)
{
	unsigned long now = ktime_to_ms(ktime_get());

	if (now - freq_boosted_time < MIM_TIME_INTERVAL_MS)
		return;

	if (work_pending(&input_boost_work))
		return;
	
	if (!is_touching)
	{
		queue_work_on(0, input_boost_wq, &input_boost_work);
		gpu_idle = false;
		is_touching = true;
	}
	idle_counter = -10;
	freq_boosted_time = time_stamp = now;
}

static int boost_input_connect(struct input_handler *handler,
                struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id boost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
				INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
				BIT_MASK(ABS_MT_POSITION_X) |
				BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler boost_input_handler = {
	.event          = boost_input_event,
	.connect        = boost_input_connect,
	.disconnect     = boost_input_disconnect,
	.name           = "input-boost",
	.id_table       = boost_ids,
};

#pragma GCC diagnostic ignored "-Wunused-result"
static int init(void)
{
	input_boost_wq = alloc_workqueue("input_boost_wq", WQ_FREEZABLE | WQ_HIGHPRI, 0);

	if (!input_boost_wq)
		return -EFAULT;

	INIT_WORK(&input_boost_work, do_input_boost);

	input_register_handler(&boost_input_handler);
	return 0;
}
late_initcall(init);
