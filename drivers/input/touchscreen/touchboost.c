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
u64 time_stamp;

static u64 touch_time_stamp;

static void boost_input_event(struct input_handle *handle,
                unsigned int type, unsigned int code, int value)
{
	u64 now = ktime_to_ms(ktime_get());

	if (now - touch_time_stamp < MIM_TIME_INTERVAL_MS)
		return;

	if (boostpulse_duration_val > 50)
		idle_counter = 0;

	touch_time_stamp = now;
	boostpulse_endtime = now + boostpulse_duration_val;
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
	input_register_handler(&boost_input_handler);
	return 0;
}
late_initcall(init);
