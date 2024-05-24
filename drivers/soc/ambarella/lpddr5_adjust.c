/*
 * Copyright (C) 2017-2029, Ambarella, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <soc/ambarella/misc.h>

extern int ambarella_scm_lp5_adjust_islp5(void);
extern int ambarella_scm_lp5_adjust_init(void);
extern int ambarella_scm_lp5_adjust_run(void);
extern int ambarella_scm_lp5_adjust_show_switch(void);
extern int ambarella_scm_lp5_adjust_set_pvalue(uint32_t pval);
extern int ambarella_scm_lp5_adjust_get_pvalue(void);
extern int ambarella_scm_lp5_adjust_set_nvalue(uint32_t nval);
extern int ambarella_scm_lp5_adjust_get_nvalue(void);
extern int ambarella_scm_lp5_adjust_set_wck2dqi_timer(void);

static struct workqueue_struct *adjust_lp5_wq;
static struct delayed_work adjust_lp5_dwork;
static int lp5_adjust_period;

static void ambarella_lpddr5_adjust_run(struct work_struct *work)
{
	if(!lp5_adjust_period)
		return;
	ambarella_scm_lp5_adjust_set_wck2dqi_timer();
	msleep(5);
	ambarella_scm_lp5_adjust_run();
	schedule_delayed_work(&adjust_lp5_dwork, msecs_to_jiffies(lp5_adjust_period * 1000));
}

static int ambarella_lpddr5_adjust_proc_show(struct seq_file *m, void *v)
{
	char tmp[11];

	seq_printf(m, "usage:\n");
	seq_printf(m, "\techo n > /proc/ambarella/lp5adj\n");
	seq_printf(m, "\twhich n > 0 means adjust every n seconds, n = 0 means disable adjust and ");
	seq_printf(m, "n = -1 means switch on/off print result\n\n");
	seq_printf(m, "current status: ");
	if(lp5_adjust_period) {
		snprintf(tmp, sizeof(tmp), "%d", lp5_adjust_period);
		seq_printf(m, "adjust period is %ss\n", tmp);
	} else
		seq_printf(m, "adjust is disabled\n");

	return 0;
}

static ssize_t ambarella_lpddr5_adjust_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	int period, ret;

	ret = kstrtoint_from_user(buffer, count, 0, &period);
	if (ret)
		return ret;

	/* 0-disable adjust, n>0-adjust ervery n second, -1-switch on/off print result */
	if(period == 0) {
		lp5_adjust_period = 0;
	} else if(period > 0){
		ambarella_scm_lp5_adjust_init();
		lp5_adjust_period = period;
		ambarella_lpddr5_adjust_run(&adjust_lp5_dwork.work);
	} else if(period == -1){
		ambarella_scm_lp5_adjust_show_switch();
	}else {
		pr_err("Invalid argument!\n");
		return -EINVAL;
	}

	return count;
}

static int ambarella_lpddr5_adjust_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_lpddr5_adjust_proc_show, PDE_DATA(inode));
}

static const struct proc_ops proc_lpddr5_adjust_fops = {
	.proc_open = ambarella_lpddr5_adjust_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambarella_lpddr5_adjust_proc_write,
	.proc_release = single_release,
};

static int ambarella_lpddr5_adjust_pval_proc_show(struct seq_file *m, void *v)
{
	int val;

	seq_printf(m, "usage:\n");
	seq_printf(m, "\techo pval > /proc/ambarella/lp5pval means set postive adjust value to pval\n");
	val = ambarella_scm_lp5_adjust_get_pvalue();

	seq_printf(m, "positive adjust value is %d fs\n", val);

	return 0;
}

static int ambarella_lpddr5_adjust_pval_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_lpddr5_adjust_pval_proc_show, PDE_DATA(inode));
}

static ssize_t ambarella_lpddr5_adjust_pval_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	int val, ret;

	ret = kstrtouint_from_user(buffer, count, 0, &val);
	if (ret)
		return ret;

	ambarella_scm_lp5_adjust_set_pvalue(val);

	return count;
}

static const struct proc_ops proc_lpddr5_adjust_pval_fops = {
	.proc_open = ambarella_lpddr5_adjust_pval_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambarella_lpddr5_adjust_pval_proc_write,
	.proc_release = single_release,
};

static int ambarella_lpddr5_adjust_nval_proc_show(struct seq_file *m, void *v)
{
	int val;

	seq_printf(m, "usage:\n");
	seq_printf(m, "\techo nval > /proc/ambarella/lp5nval means set negative adjust value to nval\n");
	val = ambarella_scm_lp5_adjust_get_nvalue();

	seq_printf(m, "negative adjust value is %d fs\n", val);

	return 0;
}

static int ambarella_lpddr5_adjust_nval_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_lpddr5_adjust_nval_proc_show, PDE_DATA(inode));
}

static ssize_t ambarella_lpddr5_adjust_nval_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	int val, ret;

	ret = kstrtouint_from_user(buffer, count, 0, &val);
	if (ret)
		return ret;

	ambarella_scm_lp5_adjust_set_nvalue(val);

	return count;
}

static const struct proc_ops proc_lpddr5_adjust_nval_fops = {
	.proc_open = ambarella_lpddr5_adjust_nval_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambarella_lpddr5_adjust_nval_proc_write,
	.proc_release = single_release,
};

static int __init ambarella_lpddr5_adjust_init(void)
{
	if(!ambarella_scm_lp5_adjust_islp5())
		return 0;

	proc_create_data("lp5adj", S_IRUGO|S_IWUSR,
			ambarella_procfs_dir(), &proc_lpddr5_adjust_fops, NULL);

	proc_create_data("lp5pval", S_IRUGO|S_IWUSR,
			ambarella_procfs_dir(), &proc_lpddr5_adjust_pval_fops, NULL);

	proc_create_data("lp5nval", S_IRUGO|S_IWUSR,
			ambarella_procfs_dir(), &proc_lpddr5_adjust_nval_fops, NULL);

	adjust_lp5_wq = alloc_workqueue("adjust_lp5_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);

	if (!adjust_lp5_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&adjust_lp5_dwork, ambarella_lpddr5_adjust_run);
	ambarella_scm_lp5_adjust_init();

	return 0;
}

static void __exit ambarella_lpddr5_adjust_exit(void)
{
	cancel_delayed_work_sync(&adjust_lp5_dwork);
	destroy_workqueue(adjust_lp5_wq);
}

module_init(ambarella_lpddr5_adjust_init);
module_exit(ambarella_lpddr5_adjust_exit);

