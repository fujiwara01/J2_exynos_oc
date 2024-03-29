/*
 * drivers/input/touchscreen/sweep2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2017, Lukas Addon <LukasAddon@gmail.com>
 *
 * v1.0 - 2013, Dennis Rassmann
 *
 * v1.1 - fix bugs and remove powersuspend.c support, 2017, Lukas Addon
 *
 * v1.2 - add wakelock support, 2017, Lukas Addon
 *
 * v1.3 - do not enable  wakelock where s2s and s2w disabled, 2017, Lukas Addon
 *
 * v1.4 - disable system event, use direct function (you must call it from touch driver and screen driver)
 *
 * v1.5 - clean code, now file smaller and faster
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/sweep2wake.h>
#include <linux/input/doubletap2wake.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/input.h>
#include <linux/wakelock.h>

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "LukasAddon <LukasAddon@gmail.com>"
#define DRIVER_DESCRIPTION "Sweep2wake for almost any device"
#define DRIVER_VERSION "1.5"
#define LOGTAG "[sweep2wake]: "

/* Tuneables */
#ifndef CONFIG_TOUCHSCREEN_SWEEP2WAKE_DEBUG
#define S2W_DEBUG		1
#else
#define S2W_DEBUG		0
#endif
#define S2W_DEFAULT		0
#define S2W_PWRKEY_DUR          60

/* Screen size */
#define DEFAULT_S2W_Y_MAX               2560
#define DEFAULT_S2W_Y_LIMIT             DEFAULT_S2W_Y_MAX-160
#define DEFAULT_S2W_X_MAX		1440

/* 0
 * |
 * |
 * |
 * |
 * 2560 - 160
 * 0<-B0-B3-B1--|--B4-B2-B5->1440
 */

/* Sweep2sleep right to left */
#define DEFAULT_S2W_X_B0		250
#define DEFAULT_S2W_X_B1		DEFAULT_S2W_X_B0+150
#define DEFAULT_S2W_X_B2		DEFAULT_S2W_X_B0+450

/* Sweep2sleep left to right */
#define DEFAULT_S2W_X_B3		DEFAULT_S2W_X_B0+130
#define DEFAULT_S2W_X_B4		DEFAULT_S2W_X_MAX-400
#define DEFAULT_S2W_X_B5		DEFAULT_S2W_X_MAX-DEFAULT_S2W_X_B0

/* Resources */
int s2w_switch = S2W_DEFAULT, s2w = S2W_DEFAULT;
int s2w_wakeup = S2W_DEFAULT;

static struct wake_lock s2w_wakelock;

static int touch_x = 0, touch_y = 0;
static bool touch_x_called = false, touch_y_called = false;
static bool  exec_count = true;
static bool scr_on_touch = false, barrier[2] = {false, false};
static bool reverse_barrier[2] = {false, false};
static struct input_dev * sweep2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
//static struct workqueue_struct *s2w_input_wq;


/* Read cmdline for s2w */
static int __init read_s2w_cmdline(char *s2w)
{
	if (strcmp(s2w, "1") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake enabled. \
				| s2w='%s'\n", s2w);
		s2w_switch = 1;
	} else if (strcmp(s2w, "2") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. \
				| s2w='%s'\n", s2w);
		s2w_switch = 2;
	} else if (strcmp(s2w, "0") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. \
				| s2w='%s'\n", s2w);
		s2w_switch = 0;
	} else {
		pr_info("[cmdline_s2w]: No valid input found. \
				Going with default: | s2w='%u'\n", s2w_switch);
	}
	return 1;
}
__setup("s2w=", read_s2w_cmdline);

/* PowerKey work func */
static void sweep2wake_presspwr(struct work_struct *sweep2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
                return;
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
        mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* PowerKey trigger */
static void sweep2wake_pwrswitch(void) {
	schedule_work(&sweep2wake_presspwr_work);
        return;
}

/* reset on finger release */
static void sweep2wake_reset(void) {
	if (wake_lock_active(&s2w_wakelock))
		wake_unlock(&s2w_wakelock);
	exec_count = true;
	barrier[0] = false;
	barrier[1] = false;
	reverse_barrier[0] = false;
	reverse_barrier[1] = false;
	scr_on_touch = false;
}

/* Sweep2wake main function */
static void detect_sweep2wake(int sweep_coord, int sweep_height, bool st)
{
	int prev_coord = 0, next_coord = 0;
	int reverse_prev_coord = 0, reverse_next_coord = 0;
	bool single_touch = st;

	// swipe to sleep check
	if ((single_touch) && (flg_screen_report == true) && (s2w_switch > 0)) {
		scr_on_touch = true;
		/* s2s: right->left */
		prev_coord = DEFAULT_S2W_X_B5;
		next_coord = DEFAULT_S2W_X_B2;
		if ((barrier[0] == true) ||
				((sweep_coord < prev_coord) &&
				(sweep_coord > next_coord) &&
				(sweep_height > DEFAULT_S2W_Y_LIMIT))) {	
			prev_coord = next_coord;
			next_coord = DEFAULT_S2W_X_B1;
			barrier[0] = true;
			wake_lock_timeout(&s2w_wakelock, HZ*2);
			if ((barrier[1] == true) ||
					((sweep_coord < prev_coord) &&
					(sweep_coord > next_coord) &&
					(sweep_height >
					DEFAULT_S2W_Y_LIMIT))) {
		#if S2W_DEBUG
       		pr_info(LOGTAG"x,y(%4d,%4d) right->left single:%s\n",
				sweep_coord, sweep_height,
				(single_touch) ? "true" : "false");
		#endif				
				prev_coord = next_coord;
				barrier[1] = true;
				if ((sweep_coord < prev_coord) &&
						(sweep_height >
						DEFAULT_S2W_Y_LIMIT)) {
					if (sweep_coord <
							DEFAULT_S2W_X_B0) {
						if (exec_count) {
							pr_info(LOGTAG"OFF\n");
							sweep2wake_pwrswitch();
							exec_count = false;
						}
					}
				}
			}
		}
		/* s2s: left->right */
		reverse_prev_coord = DEFAULT_S2W_X_B0;
		reverse_next_coord = DEFAULT_S2W_X_B3;
		if ((reverse_barrier[0] == true) ||
				((sweep_coord > reverse_prev_coord) &&
				(sweep_coord < reverse_next_coord) &&
				(sweep_height > DEFAULT_S2W_Y_LIMIT))) {
			reverse_prev_coord = reverse_next_coord;
			reverse_next_coord = DEFAULT_S2W_X_B4;
			reverse_barrier[0] = true;
			wake_lock_timeout(&s2w_wakelock, HZ*2);
			if ((reverse_barrier[1] == true) ||
					((sweep_coord > reverse_prev_coord) &&
					(sweep_coord < reverse_next_coord) &&
					(sweep_height >
					DEFAULT_S2W_Y_LIMIT))) {
		#if S2W_DEBUG
       		pr_info(LOGTAG"x,y(%4d,%4d) left->right single:%s\n",
				sweep_coord, sweep_height,
				(single_touch) ? "true" : "false");
		#endif					
				reverse_prev_coord = reverse_next_coord;
				reverse_barrier[1] = true;
				if ((sweep_coord > reverse_prev_coord) &&
						(sweep_height >
						DEFAULT_S2W_Y_LIMIT)) {
					if (sweep_coord > DEFAULT_S2W_X_B5) {
						if (exec_count) {
							pr_info(LOGTAG"OFF\n");
							sweep2wake_pwrswitch();
							exec_count = false;
						}
					}
				}
			}
		}
	}

	// swipe to wakeup check
	if ((single_touch) && (flg_screen_report == false) && (s2w_wakeup > 0)) {
		scr_on_touch = true;
		/* s2s: right->left */
		prev_coord = DEFAULT_S2W_X_B5;
		next_coord = DEFAULT_S2W_X_B2;
		if ((barrier[0] == true) ||
				((sweep_coord < prev_coord) &&
				(sweep_coord > next_coord) &&
				(sweep_height > DEFAULT_S2W_Y_LIMIT))) {	
			prev_coord = next_coord;
			next_coord = DEFAULT_S2W_X_B1;
			barrier[0] = true;
			wake_lock_timeout(&s2w_wakelock, HZ*2);
			if ((barrier[1] == true) ||
					((sweep_coord < prev_coord) &&
					(sweep_coord > next_coord) &&
					(sweep_height >
					DEFAULT_S2W_Y_LIMIT))) {
		#if S2W_DEBUG
       		pr_info(LOGTAG"x,y(%4d,%4d) right->left single:%s\n",
				sweep_coord, sweep_height,
				(single_touch) ? "true" : "false");
		#endif				
				prev_coord = next_coord;
				barrier[1] = true;
				if ((sweep_coord < prev_coord) &&
						(sweep_height >
						DEFAULT_S2W_Y_LIMIT)) {
					if (sweep_coord <
							DEFAULT_S2W_X_B0) {
						if (exec_count) {
							pr_info(LOGTAG"OFF\n");
							sweep2wake_pwrswitch();
							exec_count = false;
						}
					}
				}
			}
		}
		/* s2s: left->right */
		reverse_prev_coord = DEFAULT_S2W_X_B0;
		reverse_next_coord = DEFAULT_S2W_X_B3;
		if ((reverse_barrier[0] == true) ||
				((sweep_coord > reverse_prev_coord) &&
				(sweep_coord < reverse_next_coord) &&
				(sweep_height > DEFAULT_S2W_Y_LIMIT))) {
			reverse_prev_coord = reverse_next_coord;
			reverse_next_coord = DEFAULT_S2W_X_B4;
			reverse_barrier[0] = true;
			wake_lock_timeout(&s2w_wakelock, HZ*2);
			if ((reverse_barrier[1] == true) ||
					((sweep_coord > reverse_prev_coord) &&
					(sweep_coord < reverse_next_coord) &&
					(sweep_height >
					DEFAULT_S2W_Y_LIMIT))) {
		#if S2W_DEBUG
       		pr_info(LOGTAG"x,y(%4d,%4d) left->right single:%s\n",
				sweep_coord, sweep_height,
				(single_touch) ? "true" : "false");
		#endif					
				reverse_prev_coord = reverse_next_coord;
				reverse_barrier[1] = true;
				if ((sweep_coord > reverse_prev_coord) &&
						(sweep_height >
						DEFAULT_S2W_Y_LIMIT)) {
					if (sweep_coord > DEFAULT_S2W_X_B5) {
						if (exec_count) {
							pr_info(LOGTAG"OFF\n");
							sweep2wake_pwrswitch();
							exec_count = false;
						}
					}
				}
			}
		}
	}

}

//static void s2w_input_event(struct input_handle *handle, unsigned int type,
void s2w_input_event(
				unsigned int code, int value) {
/*#if S2W_DEBUG
	pr_info("sweep2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		"undef"), code, value);
#endif*/
	if (!s2w_switch && !s2w_wakeup) {
		return;
	}

	if (code == ABS_MT_SLOT && value > 0) {
		sweep2wake_reset();
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		sweep2wake_reset();
		return;
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y) {
		touch_y = value;
		touch_y_called = true;
	}

	if (touch_x_called && touch_y_called) {
		touch_x_called = false;
		touch_y_called = false;

		detect_sweep2wake(touch_x, touch_y, true);
		//queue_work_on(0, s2w_input_wq, &s2w_input_work);
	}
}
EXPORT_SYMBOL(s2w_input_event);

//static int input_dev_filter(struct input_dev *dev) {
//	if (strstr(dev->name, "touch") ||
//	    strstr(dev->name, "lge_touch_core")) {
//		return 0;
//	} else {
//		return 1;
//	}
//}
//
//static int s2w_input_connect(struct input_handler *handler,
//				struct input_dev *dev,
//				const struct input_device_id *id) {
//	struct input_handle *handle;
//	int error;
//
//	if (input_dev_filter(dev))
//		return -ENODEV;
//
//	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
//	if (!handle)
//		return -ENOMEM;
//
//	handle->dev = dev;
//	handle->handler = handler;
//	handle->name = "s2w";
//
//	error = input_register_handle(handle);
//	if (error)
//		goto err2;
//
//	error = input_open_device(handle);
//	if (error)
//		goto err1;
//
//	return 0;
//err1:
//	input_unregister_handle(handle);
//err2:
//	kfree(handle);
//	return error;
//}
//
//static void s2w_input_disconnect(struct input_handle *handle) {
//	input_close_device(handle);
//	input_unregister_handle(handle);
//	kfree(handle);
//}
//
//static const struct input_device_id s2w_ids[] = {
//	{ .driver_info = 1 },
//	{ },
//};
//
//static struct input_handler s2w_input_handler = {
//	.event		= s2w_input_event,
//	.connect	= s2w_input_connect,
//	.disconnect	= s2w_input_disconnect,
//	.name		= "s2w_inputreq",
//	.id_table	= s2w_ids,
//};

/*
 * SYSFS stuff below here
 */
//s2w_wakeup
static ssize_t s2w_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_wakeup);

	return count;
}

static ssize_t s2w_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n')
                if (s2w_wakeup != buf[0] - '0')
		        s2w_wakeup = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
	s2w_sweep2wake_show, s2w_sweep2wake_dump);

static ssize_t s2w_sweep2sleep_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t s2w_sweep2sleep_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n')
                if (s2w_switch != buf[0] - '0')
		        s2w_switch = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(sweep2sleep, (S_IWUSR|S_IRUGO),
	s2w_sweep2sleep_show, s2w_sweep2sleep_dump);

static ssize_t s2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t s2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(sweep2wake_version, (S_IWUSR|S_IRUGO),
	s2w_version_show, s2w_version_dump);

/*
 * INIT / EXIT stuff below here
 */
struct kobject *sweep2sleep_kobj;
EXPORT_SYMBOL_GPL(sweep2sleep_kobj);

static int __init sweep2wake_init(void)
{
	int rc = 0;

	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	wake_lock_init(&s2w_wakelock, WAKE_LOCK_SUSPEND, "s2w_wakelock");

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);
	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

//	s2w_input_wq = create_workqueue("s2wiwq");
//	if (!s2w_input_wq) {
//		pr_err("%s: Failed to create s2wiwq workqueue\n", __func__);
//		return -EFAULT;
//	}

//	INIT_WORK(&s2w_input_work, s2w_input_callback);
//	rc = input_register_handler(&s2w_input_handler);
//	if (rc)
//		pr_err("%s: Failed to register s2w_input_handler\n", __func__);

	sweep2sleep_kobj = kobject_create_and_add("sweep2sleep", NULL) ;
	if (sweep2sleep_kobj == NULL) {
		pr_warn("%s: sweep2sleep_kobj create_and_add failed\n",
				__func__);
	}
	rc = sysfs_create_file(sweep2sleep_kobj, &dev_attr_sweep2sleep.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sleep\n",
				__func__);
	}
	rc = sysfs_create_file(sweep2sleep_kobj, &dev_attr_sweep2wake.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake\n",
				__func__);
	}	
	rc = sysfs_create_file(sweep2sleep_kobj,
			&dev_attr_sweep2wake_version.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for \
				sweep2wake_version\n", __func__);
	}

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);

	return 0;
}

static void __exit sweep2wake_exit(void)
{
	kobject_del(sweep2sleep_kobj);
	input_unregister_device(sweep2wake_pwrdev);
	input_free_device(sweep2wake_pwrdev);
	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");
