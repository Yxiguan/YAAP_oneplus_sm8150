/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
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
 */
#define pr_fmt(fmt)		KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/irq.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of_gpio.h>
#include <linux/oneplus/boot_mode.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <net/netlink.h>
#include <net/sock.h>
#include "gf_spi.h"
#include "../fingerprint_detect/fingerprint_detect.h"

#define VER_MAJOR   1
#define VER_MINOR   2
#define PATCH_LEVEL 8
#define GF_SPIDEV_NAME     "goodix,fingerprint"
/*device name after register in charater*/
#define GF_DEV_NAME            "goodix_fp"
#define	GF_INPUT_NAME	    "gf_input"	/*"goodix_fp" */
#define	CHRD_DRIVER_NAME	"goodix_fp_spi"
#define	CLASS_NAME		    "goodix_fp"
#define N_SPI_MINORS		32	/* ... up to 256 */

static int SPIDEV_MAJOR;
static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);
static struct wakeup_source fp_wakelock;
static struct gf_dev gf;
static int pid = -1;
struct sock *gf_nl_sk = NULL;

static inline void sendnlmsg(char *msg)
{
	struct sk_buff *skb = alloc_skb(1, GFP_KERNEL);
	struct nlmsghdr *nlh = nlmsg_put(skb, 0, 0, 0, 1, 0);

	*((char *)NLMSG_DATA(nlh)) = *msg;
	netlink_unicast(gf_nl_sk, skb, pid, MSG_DONTWAIT);
}

static inline void sendnlmsg_tp(struct fp_underscreen_info *msg)
{
	int len = sizeof(msg);
	struct sk_buff *skb_1 = alloc_skb(len, GFP_KERNEL);
	struct nlmsghdr *nlh = nlmsg_put(skb_1, 0, 0, 0, len, 0);

	*((struct fp_underscreen_info *)NLMSG_DATA(nlh)) = *msg;
	netlink_unicast(gf_nl_sk, skb_1, pid, MSG_DONTWAIT);
}

static inline void nl_data_ready(struct sk_buff *__skb)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	char str[100];
	skb = skb_get (__skb);
	if(skb->len >= NLMSG_SPACE(0))
	{
		nlh = nlmsg_hdr(skb);
		memcpy(str, NLMSG_DATA(nlh), sizeof(str));
		pid = nlh->nlmsg_pid;
		kfree_skb(skb);
	}
}

static inline int netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg = {0, };
	netlink_cfg.groups = netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	gf_nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST,
			&netlink_cfg);
	return 0;
}

static inline void netlink_exit(void)
{
	if(gf_nl_sk != NULL){
		netlink_kernel_release(gf_nl_sk);
		gf_nl_sk = NULL;
	}
}

static inline int gf_pinctrl_init(struct gf_dev* gf_dev)
{
	int ret = 0;
	struct device *dev = &gf_dev->spi->dev;

	gf_dev->gf_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(gf_dev->gf_pinctrl)) {
		ret = PTR_ERR(gf_dev->gf_pinctrl);
		goto err;
	}
	gf_dev->gpio_state_enable =
		pinctrl_lookup_state(gf_dev->gf_pinctrl, "fp_en_init");
	if (IS_ERR_OR_NULL(gf_dev->gpio_state_enable)) {
		ret = PTR_ERR(gf_dev->gpio_state_enable);
		goto err;
	}
	gf_dev->gpio_state_disable =
		pinctrl_lookup_state(gf_dev->gf_pinctrl, "fp_dis_init");
	if (IS_ERR_OR_NULL(gf_dev->gpio_state_disable)) {
		ret = PTR_ERR(gf_dev->gpio_state_disable);
		goto err;
	}

	return 0;
err:
	gf_dev->gf_pinctrl = NULL;
	gf_dev->gpio_state_enable = NULL;
	gf_dev->gpio_state_disable = NULL;
	return ret;
}

static inline int gf_parse_dts(struct gf_dev* gf_dev)
{
	int rc = 0;
	struct device *dev = &gf_dev->spi->dev;
	struct device_node *np = dev->of_node;

	gf_dev->reset_gpio = of_get_named_gpio(np, "fp-gpio-reset", 0);
	if (gf_dev->reset_gpio < 0) {
		return gf_dev->reset_gpio;
	}

	rc = devm_gpio_request(dev, gf_dev->reset_gpio, "goodix_reset");
	if (rc) {
		goto err_reset;
	}
	gpio_direction_output(gf_dev->reset_gpio, 0);

	gf_dev->irq_gpio = of_get_named_gpio(np, "fp-gpio-irq", 0);
	if (gf_dev->irq_gpio < 0) {
		return gf_dev->irq_gpio;
	}

	rc = devm_gpio_request(dev, gf_dev->irq_gpio, "goodix_irq");
	if (rc) {
		goto err_irq;
	}
	gpio_direction_input(gf_dev->irq_gpio);

	return rc;
err_irq:
	devm_gpio_free(dev, gf_dev->irq_gpio);
err_reset:
	devm_gpio_free(dev, gf_dev->reset_gpio);
	return rc;
}

static inline int gf_hw_reset(struct gf_dev *gf_dev, unsigned int delay_ms)
{
	if(gf_dev == NULL) {
		return -1;
	}
	gpio_direction_output(gf_dev->reset_gpio, 1);
	gpio_set_value(gf_dev->reset_gpio, 0);
	mdelay(3);
	gpio_set_value(gf_dev->reset_gpio, 1);
	mdelay(delay_ms);
	return 0;
}

static inline void gf_enable_irq(struct gf_dev *gf_dev)
{
	if (!(gf_dev->irq_enabled)) {
		enable_irq(gf_dev->irq);
		gf_dev->irq_enabled = 1;
	}
}

static inline void gf_disable_irq(struct gf_dev *gf_dev)
{
	if (gf_dev->irq_enabled) {
		gf_dev->irq_enabled = 0;
		disable_irq(gf_dev->irq);
	}
}

static inline irqreturn_t gf_irq(int irq, void *handle)
{
	__pm_wakeup_event(&fp_wakelock, 400);
	sendnlmsg((char[]){1});
	return IRQ_HANDLED;
}

static inline int irq_setup(struct gf_dev *gf_dev)
{
	int status;
	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
	status = request_irq(gf_dev->irq, gf_irq,
			IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND | IRQF_PERCPU,
			"gf", gf_dev);

	if (status) {
		return status;
	}
	enable_irq_wake(gf_dev->irq);
	gf_dev->irq_enabled = 1;

	return status;
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = &gf;
	int retval = 0;
	u8 netlink_route = NETLINK_TEST;
	struct gf_ioc_chip_info info;

	if (_IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;

	if (_IOC_DIR(cmd) & _IOC_READ)
		retval = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		retval = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (retval)
		return -EFAULT;

	switch (cmd) {
	case GF_IOC_INIT:
		if (copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8))) {
			retval = -EFAULT;
			break;
		}
		break;
	case GF_IOC_EXIT:
		break;
	case GF_IOC_DISABLE_IRQ:
		gf_disable_irq(gf_dev);
		break;
	case GF_IOC_ENABLE_IRQ:
		gf_enable_irq(gf_dev);
		break;
	case GF_IOC_RESET:
		gf_hw_reset(gf_dev, 0);
		break;
	case GF_IOC_ENTER_SLEEP_MODE:
		break;
	case GF_IOC_GET_FW_INFO:
		break;
	case GF_IOC_REMOVE:
		break;

	case GF_IOC_CHIP_INFO:
		if (copy_from_user(&info, (struct gf_ioc_chip_info *)arg, sizeof(struct gf_ioc_chip_info))) {
			retval = -EFAULT;
			break;
		}
		break;
	default:
		break;
	}

	return retval;
}

#ifdef CONFIG_COMPAT
static inline long gf_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /*CONFIG_COMPAT*/


static inline int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = &gf;

	mutex_lock(&device_list_lock);
	filp->private_data = gf_dev;
	nonseekable_open(inode, filp);
	mutex_unlock(&device_list_lock);
	return 0;
}

static inline int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = &gf;

	mutex_lock(&device_list_lock);
	gf_dev = filp->private_data;
	filp->private_data = NULL;
	mutex_unlock(&device_list_lock);
	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gf_ioctl,
	.compat_ioctl = gf_compat_ioctl,
	.open = gf_open,
	.release = gf_release,
};

static inline ssize_t screen_state_get(struct device *device,
			     struct device_attribute *attribute,
			     char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%i\n", 1);
}

static inline ssize_t udfps_pressed_get(struct device *device,
			     struct device_attribute *attribute,
			     char *buffer)
{
	struct gf_dev *gfDev = dev_get_drvdata(device);
	return scnprintf(buffer, PAGE_SIZE, "%i\n", gfDev->udfps_pressed);
}

static DEVICE_ATTR(screen_state, S_IRUGO, screen_state_get, NULL);
static DEVICE_ATTR(udfps_pressed, S_IRUGO, udfps_pressed_get, NULL);

static struct attribute *gf_attributes[] = {
	&dev_attr_screen_state.attr,
	&dev_attr_udfps_pressed.attr,
	NULL
};

static const struct attribute_group gf_attribute_group = {
	.attrs = gf_attributes,
};

static struct fp_underscreen_info fp_tpinfo ={0};
int __always_inline opticalfp_irq_handler(struct fp_underscreen_info* tp_info)
{
	struct gf_dev *gf_dev = &gf;

	if (gf_dev->spi == NULL)
		return 0;

	fp_tpinfo = *tp_info;
	__pm_wakeup_event(&fp_wakelock, 2000);

	gf_dev->udfps_pressed = fp_tpinfo.touch_state;
	sysfs_notify(&gf_dev->spi->dev.kobj, NULL, dev_attr_udfps_pressed.attr.name);
	fp_tpinfo.touch_state = (uint8_t){(fp_tpinfo.touch_state == 1) ? 4 : 5};
	sendnlmsg_tp(&fp_tpinfo);

	return 0;
}
EXPORT_SYMBOL(opticalfp_irq_handler);

int __always_inline gf_opticalfp_irq_handler(int event)
{
	struct gf_dev *gf_dev = &gf;

	if (gf_dev->spi == NULL)
		return 0;

	__pm_wakeup_event(&fp_wakelock, 2000);

	gf_dev->udfps_pressed = event;
	sysfs_notify(&gf_dev->spi->dev.kobj, NULL, dev_attr_udfps_pressed.attr.name);

	sendnlmsg((char[]){(event == 1) ? 4 : 5});

	return 0;
}
EXPORT_SYMBOL(gf_opticalfp_irq_handler);

void __always_inline gf_opticalfp_ready(int ready)
{
	sendnlmsg((char[]){(ready == 1) ? 6 : 7});
}

static struct class *gf_class;
static int gf_probe(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;
	int status = -EINVAL;
	unsigned long minor;
	INIT_LIST_HEAD(&gf_dev->device_entry);
	gf_dev->spi = pdev;
	gf_dev->irq_gpio = -EINVAL;
	gf_dev->reset_gpio = -EINVAL;
	gf_dev->pwr_gpio = -EINVAL;
	gf_dev->fb_black = 0;

	/* If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;
		gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt,
				gf_dev, GF_DEV_NAME);
		status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	} else {
		status = -ENODEV;
		mutex_unlock(&device_list_lock);
		goto error_hw;
	}

	if (status == 0) {
		set_bit(minor, minors);
		list_add(&gf_dev->device_entry, &device_list);
	} else {
		gf_dev->devt = 0;
	}
	mutex_unlock(&device_list_lock);
	status = gf_parse_dts(gf_dev);
	if (status)
		goto err_parse_dt;
	/*
	 * liuyan mv wakelock here
	 * it should before irq
	 */
	wakeup_source_init(&fp_wakelock, "fp_wakelock");
	status = irq_setup(gf_dev);
	if (status)
		goto err_irq;

	status = gf_pinctrl_init(gf_dev);
	if (status)
		goto err_irq;
	if (get_boot_mode() !=  MSM_BOOT_MODE__FACTORY) {
		status = pinctrl_select_state(gf_dev->gf_pinctrl,
			gf_dev->gpio_state_enable);
		if (status) {
			goto error_hw;
		}
	} else {
		status = pinctrl_select_state(gf_dev->gf_pinctrl,
			gf_dev->gpio_state_disable);
		if (status) {
			goto error_hw;
		}
	}
	if (status == 0) {
		gf_dev->input = input_allocate_device();
		if (gf_dev->input == NULL) {
			status = -ENOMEM;
			goto error_dev;
		}
		gf_dev->input->name = GF_INPUT_NAME;
		status = input_register_device(gf_dev->input);
		if (status) {
			goto error_input;
		}
	}

	platform_set_drvdata(pdev, gf_dev);
	status = sysfs_create_group(&gf_dev->spi->dev.kobj,
			&gf_attribute_group);
	if (status) {
		goto error_input;
	}
	return status;

error_input:
	if (gf_dev->input != NULL)
		input_free_device(gf_dev->input);
error_dev:
	if (gf_dev->devt != 0) {
		mutex_lock(&device_list_lock);
		list_del(&gf_dev->device_entry);
		device_destroy(gf_class, gf_dev->devt);
		clear_bit(MINOR(gf_dev->devt), minors);
		mutex_unlock(&device_list_lock);
	}
err_irq:
	gf_cleanup(gf_dev);
err_parse_dt:
error_hw:
	return status;
}

static inline int gf_remove(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;

	wakeup_source_trash(&fp_wakelock);
	if (gf_dev->input)
		input_unregister_device(gf_dev->input);
	input_free_device(gf_dev->input);
	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);
	mutex_unlock(&device_list_lock);
	return 0;
}

static struct of_device_id gx_match_table[] = {
	{ .compatible = GF_SPIDEV_NAME },
	{},
};

static struct platform_driver gf_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = gx_match_table,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};

static inline int __init gf_init(void)
{
	int status;

	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */
	if ((fp_version != 0x03) && (fp_version != 0x04) && (fp_version != 0x07))
		return 0;
	BUILD_BUG_ON(N_SPI_MINORS > 256);
	status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	if (status < 0) {
		return status;
	}
	SPIDEV_MAJOR = status;
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		return PTR_ERR(gf_class);
	}
	status = platform_driver_register(&gf_driver);
	if (status < 0) {
		class_destroy(gf_class);
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
	}
	netlink_init();
	return 0;
}
module_init(gf_init);

static inline void __exit gf_exit(void)
{
	netlink_exit();
	platform_driver_unregister(&gf_driver);
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");