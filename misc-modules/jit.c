/*
 * jit.c -- the just-in-time module
 *
 * Copyright (C) 2001,2003 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001,2003 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: jit.c,v 1.16 2004/09/26 07:02:43 gregkh Exp $
 */

#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include <asm/hardirq.h>
/*
 * This module is a silly one: it only embeds short code fragments
 * that show how time delays can be handled in the kernel.
 */

int delay = HZ; /* the default delay, expressed in jiffies */
int max_timer_nr = 4096;
int p_cnt = 0;

module_param(delay, int, 0);
module_param(max_timer_nr, int, 0);

MODULE_AUTHOR("Dan Nakahara");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * This file, on the other hand, returns the current time forever
 */
int currentime_show(struct seq_file *file, void *v)
{
	struct timeval tv1;
	struct timespec tv2;
	unsigned long j1;
	u64 j2;

	/* get them four */
	j1 = jiffies;
	j2 = get_jiffies_64();
	do_gettimeofday(&tv1);
	tv2 = current_kernel_time();

	/* print */
	seq_printf(file,"0x%08lx 0x%016Lx %10i.%06i\n"
		       "%40i.%09i\n",
		       j1, j2,
		       (int) tv1.tv_sec, (int) tv1.tv_usec,
		       (int) tv2.tv_sec, (int) tv2.tv_nsec);
	return 0;
}

/*
 * These function prints one line of data, after sleeping one second.
 * It can sleep in different ways.
 */
int jitbusy_show(struct seq_file *file, void *v)
{
	unsigned long j0, j1; /* jiffies */
	j0 = jiffies;
	j1 = j0 + delay;

    while (time_before(jiffies, j1))
        cpu_relax();

	j1 = jiffies; /* actual value after we delayed */

    seq_printf(file, "%9li %9li\n", j0, j1);
	return 0;
}
int jitsched_show(struct seq_file *file, void *v)
{
	unsigned long j0, j1; /* jiffies */
	j0 = jiffies;
	j1 = j0 + delay;

    while (time_before(jiffies, j1)) {
        schedule();
    }

	j1 = jiffies; /* actual value after we delayed */

    seq_printf(file, "%9li %9li\n", j0, j1);
	return 0;
}
int jitqueue_show(struct seq_file *file, void *v)
{
	unsigned long j0, j1; /* jiffies */
	wait_queue_head_t wait;

	init_waitqueue_head (&wait);
	j0 = jiffies;
	j1 = j0 + delay;

    wait_event_interruptible_timeout(wait, 0, delay);

	j1 = jiffies; /* actual value after we delayed */

    seq_printf(file, "%9li %9li\n", j0, j1);
	return 0;
}

int jitschedto_show(struct seq_file *file, void *v)
{
	unsigned long j0, j1; /* jiffies */
	j0 = jiffies;
	j1 = j0 + delay;

    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout (delay);

	j1 = jiffies; /* actual value after we delayed */

    seq_printf(file, "%9li %9li\n", j0, j1);
	return 0;
}


/*
 * The timer example follows
 */

int tdelay = 10;
module_param(tdelay, int, 0);

/* This data structure used as "data" for the timer and tasklet functions */
struct jit_data {
	struct timer_list timer;
	struct tasklet_struct tlet;
	int hi; /* tasklet or tasklet_hi */
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	struct seq_file *sq_file;
	int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	seq_printf(data->sq_file, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
int jitimer_show(struct seq_file *file, void *v)
{
	struct jit_data *data;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_timer(&data->timer);
	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	seq_printf(file, "   time   delta  inirq    pid   cpu command\n");
	seq_printf(file, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our timer function */
	data->prevjiffies = j;
	data->sq_file = file;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the timer */
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay; /* parameter */
	add_timer(&data->timer);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current))
		return -ERESTARTSYS;
	kfree(data);
	return 0;
}

void jit_taskletprio_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	seq_printf(data->sq_file, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->prevjiffies = j;
		if (data->hi)
			tasklet_hi_schedule(&data->tlet);
		else
			tasklet_schedule(&data->tlet);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
int jitasklet_show(struct seq_file *file, void *v)
{
	struct jit_data *data;
	unsigned long j = jiffies;
	long hi = 0L;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	seq_printf(file, "   time   delta  inirq    pid   cpu command\n");
	seq_printf(file, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our tasklet function */
	data->prevjiffies = j;
	data->sq_file = file;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the tasklet */
	tasklet_init(&data->tlet, jit_taskletprio_fn, (unsigned long)data);
	data->hi = hi;
    tasklet_schedule(&data->tlet);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	kfree(data);
	return 0;
}

int jitasklethi_show(struct seq_file *file, void *v)
{
	struct jit_data *data;
	unsigned long j = jiffies;
	long hi = 1L;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	seq_printf(file, "   time   delta  inirq    pid   cpu command\n");
	seq_printf(file, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our tasklet function */
	data->prevjiffies = j;
	data->sq_file = file;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the tasklet */
	tasklet_init(&data->tlet, jit_taskletprio_fn, (unsigned long)data);
	data->hi = hi;
    tasklet_hi_schedule(&data->tlet);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	kfree(data);
	return 0;
}

static void *jit_proc_seq_start (struct seq_file *s, loff_t * pos)
{
    loff_t *spos;
    if(p_cnt > max_timer_nr)
        return NULL;

    printk(KERN_ALERT "start: %i\n", p_cnt);
    ++p_cnt;
    spos = kmalloc (sizeof (loff_t), GFP_KERNEL);
    if (!spos)
        return NULL;
    *spos = *pos;
    return spos;
}

static void *jit_proc_seq_next (struct seq_file *s, void *v, loff_t * pos)
{
    loff_t *spos;
    if(p_cnt >=max_timer_nr)
        return NULL;
    printk(KERN_ALERT "next: %i\n", p_cnt);
    ++p_cnt;

    spos = (loff_t *) v;
    *pos = ++(*spos);
    return spos;
}

static void jit_proc_seq_stop (struct seq_file *s, void *v)
{
    kfree (v);
}
#define BUILD_JIT_PROC_SEQ_OPS(type) \
    static struct seq_operations type##_proc_seq_ops = {\
        .start = jit_proc_seq_start,        \
        .next = jit_proc_seq_next,      \
        .stop = jit_proc_seq_stop,      \
        .show = type##_show,    \
    };

BUILD_JIT_PROC_SEQ_OPS(jitbusy);
BUILD_JIT_PROC_SEQ_OPS(jitsched);
BUILD_JIT_PROC_SEQ_OPS(jitqueue);
BUILD_JIT_PROC_SEQ_OPS(jitschedto);


#define BUILD_JIT_PROC_OPEN(type) \
    static int type##_proc_open(struct inode *inode, struct file *file) \
    {   \
        return seq_open(file, &type##_proc_seq_ops);    \
    }

#define BUILD_JIT_PROC_SINGLE_OPEN(type) \
    static int type##_proc_single_open(struct inode *inode, struct file *file)\
    {   \
        return single_open(file, &type##_show, NULL);   \
    }

/*
 * Now to implement the /proc file we need only make an open
 * method which sets up the sequence operators.
 */
BUILD_JIT_PROC_OPEN(jitbusy)
BUILD_JIT_PROC_OPEN(jitsched)
BUILD_JIT_PROC_OPEN(jitqueue)
BUILD_JIT_PROC_OPEN(jitschedto)

BUILD_JIT_PROC_SINGLE_OPEN(currentime)
BUILD_JIT_PROC_SINGLE_OPEN(jitimer)
BUILD_JIT_PROC_SINGLE_OPEN(jitasklet)
BUILD_JIT_PROC_SINGLE_OPEN(jitasklethi)

static int jit_seq_release(struct inode *inode, struct file *file){
    p_cnt = 0;
    return seq_release(inode, file);
}


#define BUILD_JIT_PROC_OPS(type) \
    static struct file_operations type##_proc_ops = {\
        .owner   = THIS_MODULE,         \
        .open    = type##_proc_open,    \
        .read    = seq_read,            \
        .llseek  = seq_lseek,           \
        .release = jit_seq_release,      \
    };

#define BUILD_JIT_PROC_SINGLE_OPS(type) \
    static struct file_operations type##_proc_single_ops = {\
        .owner   = THIS_MODULE,     \
        .open    = type##_proc_single_open,     \
        .read    = seq_read,        \
        .llseek  = seq_lseek,       \
        .release = single_release,      \
    };


/*
 * Create a set of file operations for our proc file.
 */
BUILD_JIT_PROC_OPS(jitbusy)
BUILD_JIT_PROC_OPS(jitsched)
BUILD_JIT_PROC_OPS(jitqueue)
BUILD_JIT_PROC_OPS(jitschedto)

BUILD_JIT_PROC_SINGLE_OPS(currentime)
BUILD_JIT_PROC_SINGLE_OPS(jitimer)
BUILD_JIT_PROC_SINGLE_OPS(jitasklet)
BUILD_JIT_PROC_SINGLE_OPS(jitasklethi)


int __init jit_init(void)
{
    p_cnt = 0;
	proc_create("jitbusy", 0, NULL, &jitbusy_proc_ops);
	proc_create("jitsched", 0, NULL, &jitsched_proc_ops);
	proc_create("jitqueue", 0, NULL, &jitqueue_proc_ops);
	proc_create("jitschedto", 0, NULL, &jitschedto_proc_ops);

	proc_create("currentime", 0, NULL, &currentime_proc_single_ops);
	proc_create("jitimer", 0, NULL, &jitimer_proc_single_ops);
    proc_create("jitasklet", 0, NULL, &jitasklet_proc_single_ops);
    proc_create("jitasklethi", 0, NULL, &jitasklethi_proc_single_ops);

	return 0; /* success */
}

void __exit jit_cleanup(void)
{
	remove_proc_entry("jitbusy", NULL);
	remove_proc_entry("jitsched", NULL);
	remove_proc_entry("jitqueue", NULL);
	remove_proc_entry("jitschedto", NULL);

	remove_proc_entry("currentime", NULL);
	remove_proc_entry("jitimer", NULL);
	remove_proc_entry("jitasklet", NULL);
	remove_proc_entry("jitasklethi", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
