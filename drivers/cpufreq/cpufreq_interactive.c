/*
 * drivers/cpufreq/cpufreq_interactive.c
 *
 * Copyright (C) 2010 Google, Inc.
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
 * Author: Mike Chan (mike@android.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <asm/cputime.h>
#include <linux/hotplug.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_interactive.h>

static int active_count;

struct cpufreq_interactive_cpuinfo {
	struct timer_list cpu_timer;
	struct timer_list cpu_slack_timer;
	spinlock_t load_lock; /* protects the next 4 fields */
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int target_freq;
	unsigned int floor_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time;
	struct rw_semaphore enable_sem;
	int governor_enabled;
};

static DEFINE_PER_CPU(struct cpufreq_interactive_cpuinfo, cpuinfo);

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

static struct interactive_vaules {
	/* Hi speed to bump to from lo speed when load burst (default max) */
	unsigned int hispeed_freq;

	/* Go to hi speed when CPU load at or above this value. */
	unsigned long go_hispeed_load;

	/*
	 * The minimum amount of time to spend at a frequency before we can ramp down.
	 */
	unsigned long min_sample_time;

	/*
	 * The sample rate of the timer used to increase frequency
	 */
	unsigned long timer_rate;

	/*
	 * Wait this long before raising speed above hispeed, by default a single
	 * timer interval.
	 */
	unsigned long above_hispeed_delay_val;

	/*
	 * Max additional time to wait in idle, beyond timer_rate, at speeds above
	 * minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
	int timer_slack_val;

	/* Target load. Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int default_target_loads[11];
	unsigned int *target_loads;
	int ntarget_loads;
} 
boost_values = {
	.default_target_loads = {30, 702000, 40, 1026000, 50, 1350000, 65},
	.target_loads = boost_values.default_target_loads,
	.ntarget_loads = ARRAY_SIZE(boost_values.default_target_loads)
}, busy_values = {
	.hispeed_freq = 1350000,
	.go_hispeed_load = 93,
	.min_sample_time = (60 * USEC_PER_MSEC),
	.timer_rate = (20 * USEC_PER_MSEC),
	.above_hispeed_delay_val = (30 * USEC_PER_MSEC),
	.timer_slack_val = (40 * USEC_PER_MSEC),
	.default_target_loads = {40, 702000, 50, 1026000, 60, 1350000, 70},
	.target_loads = busy_values.default_target_loads,
	.ntarget_loads = ARRAY_SIZE(busy_values.default_target_loads)
}, idle_values = {
	.hispeed_freq = 702000,
	.go_hispeed_load = 99,
	.min_sample_time = (20 * USEC_PER_MSEC),
	.timer_rate = (30 * USEC_PER_MSEC),
	.above_hispeed_delay_val = (150 * USEC_PER_MSEC),
	.timer_slack_val = -1,
	.default_target_loads = {60, 702000, 70, 1026000, 80, 1350000, 90},
	.target_loads = idle_values.default_target_loads,
	.ntarget_loads = ARRAY_SIZE(idle_values.default_target_loads)
};

/* Duration of a boot pulse in usecs */
u64 boostpulse_duration_val = 1500;

/* End time of boost pulse */
u64 boostpulse_endtime;

static int cpufreq_governor_interactive(struct cpufreq_policy *policy,
		unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE
static
#endif
struct cpufreq_governor cpufreq_gov_interactive = {
	.name = "interactive",
	.governor = cpufreq_governor_interactive,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static void cpufreq_interactive_timer_resched(
	struct cpufreq_interactive_cpuinfo *pcpu)
{
	unsigned long expires;
	unsigned long flags;
	struct interactive_vaules *values;

	values = gpu_idle ? &idle_values : &busy_values;
	
	expires = jiffies + usecs_to_jiffies(values->timer_rate);
		
	mod_timer_pinned(&pcpu->cpu_timer, expires);
	if (values->timer_slack_val >= 0 && pcpu->target_freq > pcpu->policy->min) {
		expires += usecs_to_jiffies(values->timer_slack_val);
		mod_timer_pinned(&pcpu->cpu_slack_timer, expires);
	}

	spin_lock_irqsave(&pcpu->load_lock, flags);
	pcpu->time_in_idle =
		get_cpu_idle_time(smp_processor_id(),
				     &pcpu->time_in_idle_timestamp
					, gpu_idle ? 0 : 1);
	pcpu->cputime_speedadj = 0;
	pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);
}

static unsigned int freq_to_targetload(unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;
	struct interactive_vaules *values;

	if (gpu_idle)
		values = &idle_values;
	else if (boostpulse_endtime > ktime_to_ms(ktime_get()))
		values = &boost_values;
	else
		values = &busy_values;

	spin_lock_irqsave(&(values->target_loads_lock), flags);

	for (i = 0; i < values->ntarget_loads - 1 && freq >= values->target_loads[i+1]
		&& values->target_loads[i+2] != 0; i += 2);

	ret = values->target_loads[i];
	spin_unlock_irqrestore(&(values->target_loads_lock), flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */

static unsigned int choose_freq(
	struct cpufreq_interactive_cpuinfo *pcpu, unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		cpufreq_frequency_table_target(
			pcpu->policy, pcpu->freq_table, loadadjfreq / tl,
			CPUFREQ_RELATION_L, &index);
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				cpufreq_frequency_table_target(
					pcpu->policy, pcpu->freq_table,
					freqmax - 1, CPUFREQ_RELATION_H,
					&index);
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				cpufreq_frequency_table_target(
					pcpu->policy, pcpu->freq_table,
					freqmin + 1, CPUFREQ_RELATION_L,
					&index);
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}

static u64 update_load(int cpu)
{
	struct cpufreq_interactive_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	u64 now;
	u64 now_idle;
	unsigned int delta_idle;
	unsigned int delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, gpu_idle ? 0 : 1);
	delta_idle = (unsigned int)(now_idle - pcpu->time_in_idle);
	delta_time = (unsigned int)(now - pcpu->time_in_idle_timestamp);
	active_time = delta_time - delta_idle;
	pcpu->cputime_speedadj += active_time * pcpu->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static void cpufreq_interactive_timer(unsigned long data)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, data);
	unsigned int new_freq;
	unsigned int loadadjfreq;
	unsigned int index;
	unsigned long flags;
	struct interactive_vaules *values;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled)
		goto exit;

	spin_lock_irqsave(&pcpu->load_lock, flags);
	now = update_load(data);
	delta_time = (unsigned int)(now - pcpu->cputime_speedadj_timestamp);
	cputime_speedadj = pcpu->cputime_speedadj;
	spin_unlock_irqrestore(&pcpu->load_lock, flags);

	if (WARN_ON_ONCE(!delta_time))
		goto rearm;

	values = gpu_idle ? &idle_values : &busy_values;

	do_div(cputime_speedadj, delta_time);
	loadadjfreq = (unsigned int)cputime_speedadj * 100;
	cpu_load = loadadjfreq / pcpu->target_freq;

	if (cpu_load >= values->go_hispeed_load) {
		if (pcpu->target_freq < values->hispeed_freq) {
			new_freq = values->hispeed_freq;
		} else {
			new_freq = choose_freq(pcpu, loadadjfreq);

			if (new_freq < values->hispeed_freq)
				new_freq = values->hispeed_freq;
		}
	} else {
		new_freq = choose_freq(pcpu, loadadjfreq);
	}

	if (pcpu->target_freq >= values->hispeed_freq &&
	    new_freq > pcpu->target_freq &&
	    now - pcpu->hispeed_validate_time <
		values->above_hispeed_delay_val) {
		trace_cpufreq_interactive_notyet(
			data, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		goto rearm;
	}

	pcpu->hispeed_validate_time = now;

	if (cpufreq_frequency_table_target(pcpu->policy, pcpu->freq_table,
					   new_freq, CPUFREQ_RELATION_L,
					   &index)) {
		pr_warn_once("timer %d: cpufreq_frequency_table_target error\n",
			     (int) data);
		goto rearm;
	}

	new_freq = pcpu->freq_table[index].frequency;

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if (new_freq < pcpu->floor_freq) {
		if (now - pcpu->floor_validate_time < values->min_sample_time) {
			trace_cpufreq_interactive_notyet(
				data, cpu_load, pcpu->target_freq,
				pcpu->policy->cur, new_freq);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to hispeed_freq.  If boosted to hispeed_freq then we
	 * allow the speed to drop as soon as the boostpulse duration expires
	 * (or the indefinite boost is turned off).
	 */

	if (new_freq > values->hispeed_freq) {
		pcpu->floor_freq = new_freq;
		pcpu->floor_validate_time = now;
	}

	if (pcpu->target_freq == new_freq) {
		trace_cpufreq_interactive_already(
			data, cpu_load, pcpu->target_freq,
			pcpu->policy->cur, new_freq);
		goto rearm_if_notmax;
	}

	trace_cpufreq_interactive_target(data, cpu_load, pcpu->target_freq,
					 pcpu->policy->cur, new_freq);

	pcpu->target_freq = new_freq;
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(data, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process(speedchange_task);

rearm_if_notmax:
	/*
	 * Already set max speed and don't see a need to change that,
	 * wait until next idle to re-evaluate, don't need timer.
	 */
	if (pcpu->target_freq == pcpu->policy->max)
		goto exit;

rearm:
	if (!timer_pending(&pcpu->cpu_timer))
		cpufreq_interactive_timer_resched(pcpu);

exit:
	up_read(&pcpu->enable_sem);
	return;
}

static void cpufreq_interactive_idle_start(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());
	int pending;

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	pending = timer_pending(&pcpu->cpu_timer);

	if (pcpu->target_freq != pcpu->policy->min) {
		/*
		 * Entering idle while not at lowest speed.  On some
		 * platforms this can hold the other CPU(s) at that speed
		 * even though the CPU is idle. Set a timer to re-evaluate
		 * speed so this idle CPU doesn't hold the other CPUs above
		 * min indefinitely.  This should probably be a quirk of
		 * the CPUFreq driver.
		 */
		if (!pending)
			cpufreq_interactive_timer_resched(pcpu);
	}

	up_read(&pcpu->enable_sem);
}

static void cpufreq_interactive_idle_end(void)
{
	struct cpufreq_interactive_cpuinfo *pcpu =
		&per_cpu(cpuinfo, smp_processor_id());

	if (!down_read_trylock(&pcpu->enable_sem))
		return;
	if (!pcpu->governor_enabled) {
		up_read(&pcpu->enable_sem);
		return;
	}

	/* Arm the timer for 1-2 ticks later if not already. */
	if (!timer_pending(&pcpu->cpu_timer)) {
		cpufreq_interactive_timer_resched(pcpu);
	} else if (time_after_eq(jiffies, pcpu->cpu_timer.expires)) {
		del_timer(&pcpu->cpu_timer);
		del_timer(&pcpu->cpu_slack_timer);
		cpufreq_interactive_timer(smp_processor_id());
	}

	up_read(&pcpu->enable_sem);
}

static int cpufreq_interactive_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			unsigned int j;
			unsigned int max_freq = 0;

			pcpu = &per_cpu(cpuinfo, cpu);
			if (!down_read_trylock(&pcpu->enable_sem))
				continue;
			if (!pcpu->governor_enabled) {
				up_read(&pcpu->enable_sem);
				continue;
			}

			for_each_cpu(j, pcpu->policy->cpus) {
				struct cpufreq_interactive_cpuinfo *pjcpu =
					&per_cpu(cpuinfo, j);

				if (pjcpu->target_freq > max_freq)
					max_freq = pjcpu->target_freq;
			}

			if (max_freq != pcpu->policy->cur)
				__cpufreq_driver_target(pcpu->policy,
							max_freq,
							CPUFREQ_RELATION_H);
			trace_cpufreq_interactive_setspeed(cpu,
						     pcpu->target_freq,
						     pcpu->policy->cur);

			up_read(&pcpu->enable_sem);
		}
	}

	return 0;
}

/*static void cpufreq_interactive_boost(void)
{
	int i;
	int anyboost = 0;
	unsigned long flags;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct interactive_vaules *values;

	values = gpu_idle ? &idle_values : &busy_values;

	spin_lock_irqsave(&speedchange_cpumask_lock, flags);

	for_each_online_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);

		if (pcpu->target_freq < values->hispeed_freq) {
			pcpu->target_freq = values->hispeed_freq;
			cpumask_set_cpu(i, &speedchange_cpumask);
			pcpu->hispeed_validate_time =
				ktime_to_us(ktime_get());
			anyboost = 1;
		}

		*
		 * Set floor freq and (re)start timer for when last
		 * validated.
		 *

		pcpu->floor_freq = values->hispeed_freq;
		pcpu->floor_validate_time = ktime_to_us(ktime_get());
	}

	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

	if (anyboost)
		wake_up_process(speedchange_task);
}*/

static int cpufreq_interactive_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_interactive_cpuinfo *pcpu;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_POSTCHANGE) {
		pcpu = &per_cpu(cpuinfo, freq->cpu);
		if (!down_read_trylock(&pcpu->enable_sem))
			return 0;
		if (!pcpu->governor_enabled) {
			up_read(&pcpu->enable_sem);
			return 0;
		}

		for_each_cpu(cpu, pcpu->policy->cpus) {
			struct cpufreq_interactive_cpuinfo *pjcpu =
				&per_cpu(cpuinfo, cpu);
			spin_lock_irqsave(&pjcpu->load_lock, flags);
			update_load(cpu);
			spin_unlock_irqrestore(&pjcpu->load_lock, flags);
		}

		up_read(&pcpu->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_interactive_notifier,
};

static ssize_t show_target_loads(struct interactive_vaules *values, 
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&(values->target_loads_lock), flags);

	for (i = 0; i < values->ntarget_loads &&
			values->target_loads[i] != 0; i++)
		ret += sprintf(buf + ret, "%u%s", values->target_loads[i],
			       i & 0x1 ? ":" : " ");

	ret += sprintf(buf + ret, "\n");
	spin_unlock_irqrestore(&(values->target_loads_lock), flags);
	return ret;
}

static ssize_t show_boost_target_loads(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return show_target_loads(&boost_values, buf);
}

static ssize_t show_busy_target_loads(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return show_target_loads(&busy_values, buf);
}

static ssize_t show_idle_target_loads(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return show_target_loads(&idle_values, buf);
}

static ssize_t store_target_loads(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	const char *cp;
	unsigned int *new_target_loads = NULL;
	int ntokens = 1;
	int i;
	unsigned long flags;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err_inval;

	new_target_loads = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!new_target_loads) {
		ret = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &new_target_loads[i++]) != 1)
			goto err_inval;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_inval;

	spin_lock_irqsave(&(values->target_loads_lock), flags);
	if (values->target_loads != values->default_target_loads)
		kfree(values->target_loads);
	values->target_loads = new_target_loads;
	values->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&(values->target_loads_lock), flags);
	return count;

err_inval:
	ret = -EINVAL;
err:
	kfree(new_target_loads);
	return ret;
}

static ssize_t store_boost_target_loads(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	return store_target_loads(&boost_values, buf, count);
}

static ssize_t store_busy_target_loads(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	return store_target_loads(&busy_values, buf, count);
}

static ssize_t store_idle_target_loads(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	return store_target_loads(&idle_values, buf, count);
}

static struct global_attr boost_target_loads_attr =
	__ATTR(boost_target_loads, S_IRUGO | S_IWUSR,
		show_boost_target_loads, store_boost_target_loads);

static struct global_attr idle_target_loads_attr =
	__ATTR(idle_target_loads, S_IRUGO | S_IWUSR,
		show_idle_target_loads, store_idle_target_loads);

static struct global_attr busy_target_loads_attr =
	__ATTR(busy_target_loads, S_IRUGO | S_IWUSR,
		show_busy_target_loads, store_busy_target_loads);

static ssize_t show_busy_hispeed_freq(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", busy_values.hispeed_freq);
}

static ssize_t show_idle_hispeed_freq(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", idle_values.hispeed_freq);
}

static ssize_t store_hispeed_freq(struct interactive_vaules *values,
	const char *buf, size_t count)
{
	int ret;
	long unsigned int val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	values->hispeed_freq = val;
	return count;
}

static ssize_t store_busy_hispeed_freq(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	return store_hispeed_freq(&busy_values, buf, count);
}

static ssize_t store_idle_hispeed_freq(struct kobject *kobj,
				  struct attribute *attr, const char *buf,
				  size_t count)
{
	return store_hispeed_freq(&idle_values, buf, count);
}

static struct global_attr busy_hispeed_freq_attr = __ATTR(busy_hispeed_freq,
		0644, show_busy_hispeed_freq, store_busy_hispeed_freq);

static struct global_attr idle_hispeed_freq_attr = __ATTR(idle_hispeed_freq,
		0644, show_idle_hispeed_freq, store_idle_hispeed_freq);

static ssize_t show_busy_go_hispeed_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", busy_values.go_hispeed_load);
}

static ssize_t show_idle_go_hispeed_load(struct kobject *kobj,
				     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", idle_values.go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	values->go_hispeed_load = val;
	return count;
}

static ssize_t store_busy_go_hispeed_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_go_hispeed_load(&busy_values, buf, count);
}

static ssize_t store_idle_go_hispeed_load(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_go_hispeed_load(&idle_values, buf, count);
}

static struct global_attr busy_go_hispeed_load_attr = __ATTR(busy_go_hispeed_load,
		0644, show_busy_go_hispeed_load, store_busy_go_hispeed_load);

static struct global_attr idle_go_hispeed_load_attr = __ATTR(idle_go_hispeed_load,
		0644, show_idle_go_hispeed_load, store_idle_go_hispeed_load);

static ssize_t show_busy_min_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", busy_values.min_sample_time);
}

static ssize_t show_idle_min_sample_time(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", idle_values.min_sample_time);
}

static ssize_t store_min_sample_time(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	values->min_sample_time = val;
	return count;
}

static ssize_t store_idle_min_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_min_sample_time(&busy_values, buf, count);
}

static ssize_t store_busy_min_sample_time(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_min_sample_time(&idle_values, buf, count);
}

static struct global_attr busy_min_sample_time_attr = __ATTR(busy_min_sample_time,
		0644, show_busy_min_sample_time, store_busy_min_sample_time);

static struct global_attr idle_min_sample_time_attr = __ATTR(idle_min_sample_time,
		0644, show_idle_min_sample_time, store_idle_min_sample_time);

static ssize_t show_busy_above_hispeed_delay(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", busy_values.above_hispeed_delay_val);
}

static ssize_t show_idle_above_hispeed_delay(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", idle_values.above_hispeed_delay_val);
}

static ssize_t store_above_hispeed_delay(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	values->above_hispeed_delay_val = val;
	return count;
}

static ssize_t store_busy_above_hispeed_delay(struct kobject *kobj,
					 struct attribute *attr,
					 const char *buf, size_t count)
{
	return store_above_hispeed_delay(&busy_values, buf, count);
}

static ssize_t store_idle_above_hispeed_delay(struct kobject *kobj,
					 struct attribute *attr,
					 const char *buf, size_t count)
{
	return store_above_hispeed_delay(&idle_values, buf, count);
}

define_one_global_rw(idle_above_hispeed_delay);
define_one_global_rw(busy_above_hispeed_delay);

static ssize_t show_busy_timer_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", busy_values.timer_rate);
}

static ssize_t show_idle_timer_rate(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", idle_values.timer_rate);
}

static ssize_t store_timer_rate(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	values->timer_rate = val;
	return count;
}

static ssize_t store_busy_timer_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_timer_rate(&busy_values, buf, count);
}

static ssize_t store_idle_timer_rate(struct kobject *kobj,
			struct attribute *attr, const char *buf, size_t count)
{
	return store_timer_rate(&idle_values, buf, count);
}

static struct global_attr busy_timer_rate_attr = __ATTR(busy_timer_rate,
		0644, show_busy_timer_rate, store_busy_timer_rate);

static struct global_attr idle_timer_rate_attr = __ATTR(idle_timer_rate,
		0644, show_idle_timer_rate, store_idle_timer_rate);

static ssize_t show_busy_timer_slack(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", busy_values.timer_slack_val);
}

static ssize_t show_idle_timer_slack(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", idle_values.timer_slack_val);
}

static ssize_t store_timer_slack(struct interactive_vaules *values, 
	const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	values->timer_slack_val = val;
	return count;
}

static ssize_t store_busy_timer_slack(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	return store_timer_slack(&busy_values, buf, count);
}

static ssize_t store_idle_timer_slack(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	return store_timer_slack(&idle_values, buf, count);
}

define_one_global_rw(busy_timer_slack);
define_one_global_rw(idle_timer_slack);

static ssize_t show_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sprintf(buf, "%10lld\n", (long long)boostpulse_duration_val);
}

static ssize_t store_boostpulse_duration(
	struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	boostpulse_duration_val = val;
	return count;
}

define_one_global_rw(boostpulse_duration);

static struct attribute *interactive_attributes[] = {
	&boost_target_loads_attr.attr,
	&busy_target_loads_attr.attr,
	&busy_hispeed_freq_attr.attr,
	&busy_go_hispeed_load_attr.attr,
	&busy_above_hispeed_delay.attr,
	&busy_min_sample_time_attr.attr,
	&busy_timer_rate_attr.attr,
	&busy_timer_slack.attr,
	&idle_target_loads_attr.attr,
	&idle_hispeed_freq_attr.attr,
	&idle_go_hispeed_load_attr.attr,
	&idle_above_hispeed_delay.attr,
	&idle_min_sample_time_attr.attr,
	&idle_timer_rate_attr.attr,
	&idle_timer_slack.attr,
	&boostpulse_duration.attr,
	NULL,
};

static struct attribute_group interactive_attr_group = {
	.attrs = interactive_attributes,
	.name = "interactive",
};

static int cpufreq_interactive_idle_notifier(struct notifier_block *nb,
					     unsigned long val,
					     void *data)
{
	switch (val) {
	case IDLE_START:
		cpufreq_interactive_idle_start();
		break;
	case IDLE_END:
		cpufreq_interactive_idle_end();
		break;
	}

	return 0;
}

static struct notifier_block cpufreq_interactive_idle_nb = {
	.notifier_call = cpufreq_interactive_idle_notifier,
};

static int cpufreq_governor_interactive(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	unsigned int j;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct cpufreq_frequency_table *freq_table;

	switch (event) {
	case CPUFREQ_GOV_START:
		if (!cpu_online(policy->cpu))
			return -EINVAL;

		mutex_lock(&gov_lock);

		freq_table =
			cpufreq_frequency_get_table(policy->cpu);

		for_each_cpu(j, policy->cpus) {
			unsigned long expires;

			pcpu = &per_cpu(cpuinfo, j);
			pcpu->policy = policy;
			pcpu->target_freq = policy->cur;
			pcpu->freq_table = freq_table;
			pcpu->floor_freq = pcpu->target_freq;
			pcpu->floor_validate_time =
				ktime_to_us(ktime_get());
			pcpu->hispeed_validate_time =
				pcpu->floor_validate_time;
			down_write(&pcpu->enable_sem);
			expires = jiffies +
				usecs_to_jiffies(idle_values.timer_rate);
			pcpu->cpu_timer.expires = expires;
			add_timer_on(&pcpu->cpu_timer, j);
			if (idle_values.timer_slack_val >= 0) {
				expires += usecs_to_jiffies(
					idle_values.timer_slack_val);
				pcpu->cpu_slack_timer.expires = expires;
				add_timer_on(&pcpu->cpu_slack_timer, j);
			}
			pcpu->governor_enabled = 1;
			up_write(&pcpu->enable_sem);
		}

		/*
		 * Do not register the idle hook and create sysfs
		 * entries if we have already done so.
		 */
		if (++active_count > 1) {
			mutex_unlock(&gov_lock);
			return 0;
		}

		rc = sysfs_create_group(cpufreq_global_kobject,
				&interactive_attr_group);
		if (rc) {
			mutex_unlock(&gov_lock);
			return rc;
		}

		idle_notifier_register(&cpufreq_interactive_idle_nb);
		cpufreq_register_notifier(
			&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);
		for_each_cpu(j, policy->cpus) {
			pcpu = &per_cpu(cpuinfo, j);
			down_write(&pcpu->enable_sem);
			pcpu->governor_enabled = 0;
			del_timer_sync(&pcpu->cpu_timer);
			del_timer_sync(&pcpu->cpu_slack_timer);
			up_write(&pcpu->enable_sem);
		}

		if (--active_count > 0) {
			mutex_unlock(&gov_lock);
			return 0;
		}

		cpufreq_unregister_notifier(
			&cpufreq_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
		idle_notifier_unregister(&cpufreq_interactive_idle_nb);
		sysfs_remove_group(cpufreq_global_kobject,
				&interactive_attr_group);
		mutex_unlock(&gov_lock);

		break;

	case CPUFREQ_GOV_LIMITS:
		if (policy->max < policy->cur)
			__cpufreq_driver_target(policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > policy->cur)
			__cpufreq_driver_target(policy,
					policy->min, CPUFREQ_RELATION_L);
		break;
	}
	return 0;
}

static void cpufreq_interactive_nop_timer(unsigned long data)
{
}

static int __init cpufreq_interactive_init(void)
{
	unsigned int i;
	struct cpufreq_interactive_cpuinfo *pcpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	/* Initalize per-cpu timers */
	for_each_possible_cpu(i) {
		pcpu = &per_cpu(cpuinfo, i);
		init_timer_deferrable(&pcpu->cpu_timer);
		pcpu->cpu_timer.function = cpufreq_interactive_timer;
		pcpu->cpu_timer.data = i;
		init_timer(&pcpu->cpu_slack_timer);
		pcpu->cpu_slack_timer.function = cpufreq_interactive_nop_timer;
		spin_lock_init(&pcpu->load_lock);
		init_rwsem(&pcpu->enable_sem);
	}

	spin_lock_init(&(busy_values.target_loads_lock));
	spin_lock_init(&(idle_values.target_loads_lock));
	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	speedchange_task =
		kthread_create(cpufreq_interactive_speedchange_task, NULL,
			       "cfinteractive");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process(speedchange_task);

	return cpufreq_register_governor(&cpufreq_gov_interactive);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE
fs_initcall(cpufreq_interactive_init);
#else
module_init(cpufreq_interactive_init);
#endif

static void __exit cpufreq_interactive_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_interactive);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);
}

module_exit(cpufreq_interactive_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com>");
MODULE_DESCRIPTION("'cpufreq_interactive' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPL");
