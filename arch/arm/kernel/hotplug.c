/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Simple hot[un]plug driver for SMP
 *
 * rewritten by Patrick Dittrich <patrick90vhm@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/hotplug.h>
 
#include <mach/cpufreq.h>

#define DEFAULT_FIRST_LEVEL 60
#define DEFAULT_THIRD_LEVEL 30
#define DEFAULT_SUSPEND_FREQ 702000
#define DEFAULT_CORES_ON_TOUCH 2
#define DEFAULT_COUNTER 50
#define BOOST_TIME 3000

//#define DEBUG

#define GPU_STATE 2
#define ACTIVE_CORES 4
#define TUNABLES 2

/* DEFAULT_THIRD_LEVEL, DEFAULT_FIRST_LEVEL */
static unsigned int hotplug_val[GPU_STATE][ACTIVE_CORES][TUNABLES] =
{{	
	/* gpu idle */
	{0, 80},
	{40, 85},
	{50, 90},
	{60, 100} 
	},{
	/* gpu busy */
	{0, 60},
	{30, 60},
	{30, 65},
	{40, 100} 
}};

struct cpu_load_data {
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static unsigned int cores_on_touch = DEFAULT_CORES_ON_TOUCH;
static u64 now, coreboost_endtime;
static short first_counter = 0;
static short third_counter = 0;

static struct workqueue_struct *wq;
static struct workqueue_struct *pm_wq;
static struct delayed_work decide_hotplug;
static struct work_struct resume;
static struct work_struct suspend;

unsigned int get_cur_max(unsigned int cpu);

static inline int get_cpu_load(unsigned int cpu)
{
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	struct cpufreq_policy policy;
	u64 cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;
	unsigned int cur_load;
	unsigned int cur_max, max_freq, cur_freq;

	cpufreq_get_policy(&policy, cpu);
	
	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time,
						gpu_idle ? 0 : 1);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	/* get the correct max frequency and current freqency */
	cur_max = get_cur_max(policy.cpu);

	if (cur_max >= policy.max)
	{
		max_freq = policy.max;
		cur_freq = policy.cur;
	}
	else
	{
		max_freq = cur_max;
		cur_freq = policy.cur > cur_max ? cur_max : policy.cur;
	}

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	return (cur_load * cur_freq) / max_freq;
}

static void __ref online_core(unsigned short cpus_num)
{
	unsigned int cpu;
	
	if (cpus_num > 3)
		return;
		
	for_each_possible_cpu(cpu) 
	{
		if (!cpu_online(cpu)) 
		{
			cpu_up(cpu);
			break;
		}
	}
	
	coreboost_endtime = now + BOOST_TIME;
	first_counter = 0;
	third_counter = -DEFAULT_COUNTER;
	
	return;
}

static void __ref offline_core(unsigned short cpus_num)
{   
	unsigned int cpu;

	if (cpus_num == 1 || (cpus_num == cores_on_touch
				&& coreboost_endtime > now))
		return;
	
	for (cpu = 3; cpu; cpu--)
	{
		if (cpu_online(cpu)) 
		{
			cpu_down(cpu);
			break;
		}
	}
	
	coreboost_endtime = now + BOOST_TIME;
	first_counter = 0;
	third_counter = 0;
	
	return;
}

unsigned int scale_first_level(unsigned int online_cpus)
{
	return hotplug_val[(gpu_idle)?0:1][online_cpus-1][1];
}

unsigned int scale_third_level(unsigned int online_cpus)
{
	return hotplug_val[(gpu_idle)?0:1][online_cpus-1][0];
}

static void __ref decide_hotplug_func(struct work_struct *work)
{
	unsigned int cpu, load, av_load = 0;
	unsigned short online_cpus, up_val, down_val;

#ifdef DEBUG
	short load_array[4] = {};
	int cpu_debug = 0;
	struct cpufreq_policy policy;
#endif

	now = ktime_to_ms(ktime_get());
	online_cpus = num_online_cpus();

	for_each_online_cpu(cpu) 
	{
		load = get_cpu_load(cpu);
		av_load += load;
		
#ifdef DEBUG
		load_array[cpu] = load;
#endif		
	}

	av_load = av_load / online_cpus;
	
	if (gpu_idle)
	{
		up_val = 3;
		down_val = 6;
	}
	else if (boostpulse_endtime > now 
		&& online_cpus < cores_on_touch)
	{
		up_val = 15;
		down_val = 7;		
	}
	else
	{
		up_val = 10;
		down_val = 5;		
	}

	if (av_load >= scale_first_level(online_cpus))
	{
		coreboost_endtime = now + BOOST_TIME;

		if (first_counter < DEFAULT_COUNTER)
			first_counter += up_val;
		
		if (third_counter > 0)
			third_counter -= up_val;
			
		if (first_counter >= DEFAULT_COUNTER)
			online_core(online_cpus);	
	}
	else if (av_load <= scale_third_level(online_cpus))
	{
		if (third_counter < DEFAULT_COUNTER)
			third_counter += down_val;
		
		if (first_counter > 0)
			first_counter -= down_val;
			
		if (third_counter >= DEFAULT_COUNTER)
			offline_core(online_cpus);	
	}
	else
	{
		if (now + (BOOST_TIME / 2) > coreboost_endtime)
			coreboost_endtime = now + BOOST_TIME / 2; 

		if (first_counter > 0)
			first_counter -= down_val;
		
		if (third_counter > 0)
			third_counter -= down_val; 
	}

#ifdef DEBUG
	cpu = 0;
	pr_info("------HOTPLUG DEBUG INFO------\n");
	pr_info("Cores on:\t%d", online_cpus);
	pr_info("Core0:\t\t%d", load_array[0]);
	pr_info("Core1:\t\t%d", load_array[1]);
	pr_info("Core2:\t\t%d", load_array[2]);
	pr_info("Core3:\t\t%d", load_array[3]);
	pr_info("Av Load:\t\t%d", av_load);
	pr_info("-------------------------------");
	pr_info("Up count:\t%d\n",first_counter);
	pr_info("Dw count:\t%d\n",third_counter);

	if (gpu_idle)
		pr_info("Gpu Idle:\ttrue");
	else
		pr_info("Gpu Idle:\tfalse");
	if (boostpulse_endtime > now)
		pr_info("Touch:\t\ttrue");
	else
		pr_info("Touch:\t\tfalse");
	
	for_each_possible_cpu(cpu_debug)
	{
		if (cpu_online(cpu_debug))
		{
			cpufreq_get_policy(&policy, cpu_debug);
			pr_info("cpu%d:\t\t%d MHz",
					cpu_debug,policy.cur/1000);
		}
		else
			pr_info("cpu%d:\t\toff",cpu_debug);
	}
	pr_info("First level:\t%d", scale_first_level(online_cpus));
	pr_info("Third level:\t%d", scale_third_level(online_cpus));
	pr_info("-----------------------------------------");
#endif

	queue_delayed_work(wq, &decide_hotplug, msecs_to_jiffies(30));
}

static void suspend_func(struct work_struct *work)
{
	int cpu;

	/* cancel the hotplug work when the screen is off and flush the WQ */
	flush_workqueue(wq);
	cancel_delayed_work_sync(&decide_hotplug);

	pr_info("Early Suspend stopping Hotplug work...\n");
	
	for_each_possible_cpu(cpu) 
	{
		if (cpu)
		{
			cpu_down(cpu);
		}
		
	}

	first_counter = 0;
	third_counter = 0;
}

static void __ref resume_func(struct work_struct *work)
{
	int cpu, onlined = 0;
	u64 now = ktime_to_ms(ktime_get());

	idle_counter = 0;
	gpu_idle = false;

	coreboost_endtime = now + BOOST_TIME;
	boostpulse_endtime = now + boostpulse_duration_val;

	for_each_possible_cpu(cpu) 
	{
		if (cpu) 
		{
			cpu_up(cpu);
			if (++onlined == 2)
				break;
		}
	}
	
	pr_info("Late Resume starting Hotplug work...\n");
	queue_delayed_work(wq, &decide_hotplug, HZ);	
}

static void hotplug_early_suspend(struct early_suspend *handler)
{	 
	queue_work_on(0, pm_wq, &suspend);
}

static void hotplug_early_resume(struct early_suspend *handler)
{  
	queue_work_on(0, pm_wq, &resume);
}

static struct early_suspend hotplug_suspend =
{
	.suspend = hotplug_early_suspend,
	.resume = hotplug_early_resume,
};

int __init hotplug_init(void)
{
	pr_info("Hotplug driver started.\n");

	wq = alloc_ordered_workqueue("hotplug_workqueue", 0);
	
	if (!wq)
		return -ENOMEM;

	pm_wq = alloc_workqueue("pm_workqueue", 0, 1);
	
	if (!pm_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	INIT_WORK(&resume, resume_func);
	INIT_WORK(&suspend, suspend_func);
	queue_delayed_work(wq, &decide_hotplug, HZ*25);
	
	register_early_suspend(&hotplug_suspend);
	
	return 0;
}
late_initcall(hotplug_init);

