// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2015 - 2019 Intel Corporation. All rights reserved.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/slab.h>

#include <linux/rpmb.h>
#include "rpmb-cdev.h"

static DEFINE_IDA(rpmb_ida);

/**
 * rpmb_dev_get() - increase rpmb device ref counter
 * @rdev: rpmb device
 */
struct rpmb_dev *rpmb_dev_get(struct rpmb_dev *rdev)
{
	return get_device(&rdev->dev) ? rdev : NULL;
}
EXPORT_SYMBOL_GPL(rpmb_dev_get);

/**
 * rpmb_dev_put() - decrease rpmb device ref counter
 * @rdev: rpmb device
 */
void rpmb_dev_put(struct rpmb_dev *rdev)
{
	put_device(&rdev->dev);
}
EXPORT_SYMBOL_GPL(rpmb_dev_put);

/**
 * rpmb_cmd_fixup() - fixup rpmb command
 * @rdev: rpmb device
 * @cmds: rpmb command list
 * @ncmds: number of commands
 */
static void rpmb_cmd_fixup(struct rpmb_dev *rdev,
			   struct rpmb_cmd *cmds, u32 ncmds)
{
	int i;

	if (RPMB_TYPE_HW(rdev->ops->type) != RPMB_TYPE_EMMC)
		return;

	/* Fixup RPMB_READ_DATA specific to eMMC
	 * The block count of the RPMB read operation is not indicated
	 * in the original RPMB Data Read Request packet.
	 * This is different then implementation for other protocol
	 * standards.
	 */
	for (i = 0; i < ncmds; i++) {
		struct rpmb_frame_jdec *frame = cmds[i].frames;

		if (frame->req_resp == cpu_to_be16(RPMB_READ_DATA)) {
			dev_dbg(&rdev->dev, "Fixing up READ_DATA frame to block_count=0\n");
			frame->block_count = 0;
		}
	}
}

/**
 * rpmb_cmd_seq() - send RPMB command sequence
 * @rdev: rpmb device
 * @cmds: rpmb command list
 * @ncmds: number of commands
 *
 * Return:
 * *        0 on success
 * *        -EINVAL on wrong parameters
 * *        -EOPNOTSUPP if device doesn't support the requested operation
 * *        < 0 if the operation fails
 */
int rpmb_cmd_seq(struct rpmb_dev *rdev, struct rpmb_cmd *cmds, u32 ncmds)
{
	int err;

	if (!rdev || !cmds || !ncmds)
		return -EINVAL;

	mutex_lock(&rdev->lock);
	err = -EOPNOTSUPP;
	if (rdev->ops && rdev->ops->cmd_seq) {
		rpmb_cmd_fixup(rdev, cmds, ncmds);
		err = rdev->ops->cmd_seq(rdev->dev.parent, rdev->target,
					 cmds, ncmds);
	}
	mutex_unlock(&rdev->lock);

	return err;
}
EXPORT_SYMBOL_GPL(rpmb_cmd_seq);

/**
 * rpmb_get_capacity() - returns the capacity of the rpmb device
 * @rdev: rpmb device
 *
 * Return:
 * *        capacity of the device in units of 128K, on success
 * *        -EINVAL on wrong parameters
 * *        -EOPNOTSUPP if device doesn't support the requested operation
 * *        < 0 if the operation fails
 */
int rpmb_get_capacity(struct rpmb_dev *rdev)
{
	int err;

	if (!rdev)
		return -EINVAL;

	mutex_lock(&rdev->lock);
	err = -EOPNOTSUPP;
	if (rdev->ops && rdev->ops->get_capacity)
		err = rdev->ops->get_capacity(rdev->dev.parent, rdev->target);
	mutex_unlock(&rdev->lock);

	return err;
}
EXPORT_SYMBOL_GPL(rpmb_get_capacity);

static void rpmb_dev_release(struct device *dev)
{
	struct rpmb_dev *rdev = to_rpmb_dev(dev);

	ida_simple_remove(&rpmb_ida, rdev->id);
	kfree(rdev);
}

struct class rpmb_class = {
	.name = "rpmb",
	.owner = THIS_MODULE,
	.dev_release = rpmb_dev_release,
};
EXPORT_SYMBOL(rpmb_class);

/**
 * rpmb_dev_find_device() - return first matching rpmb device
 * @data: data for the match function
 * @match: the matching function
 *
 * Return: matching rpmb device or NULL on failure
 */
static
struct rpmb_dev *rpmb_dev_find_device(const void *data,
				      int (*match)(struct device *dev,
						   const void *data))
{
	struct device *dev;

	dev = class_find_device(&rpmb_class, NULL, data, match);

	return dev ? to_rpmb_dev(dev) : NULL;
}

static int match_by_type(struct device *dev, const void *data)
{
	struct rpmb_dev *rdev = to_rpmb_dev(dev);
	const u32 *type = data;

	return (*type == RPMB_TYPE_ANY || rdev->ops->type == *type);
}

/**
 * rpmb_dev_get_by_type() - return first registered rpmb device
 *      with matching type.
 * @type: rpbm underlying device type
 *
 * If run with RPMB_TYPE_ANY the first an probably only
 * device is returned
 *
 * Return: matching rpmb device or NULL/ERR_PTR on failure
 */
struct rpmb_dev *rpmb_dev_get_by_type(u32 type)
{
	if (type > RPMB_TYPE_MAX)
		return ERR_PTR(-EINVAL);

	return rpmb_dev_find_device(&type, match_by_type);
}
EXPORT_SYMBOL_GPL(rpmb_dev_get_by_type);

struct device_with_target {
	const struct device *dev;
	u8 target;
};

static int match_by_parent(struct device *dev, const void *data)
{
	const struct device_with_target *d = data;
	struct rpmb_dev *rdev = to_rpmb_dev(dev);

	return (d->dev && dev->parent == d->dev && rdev->target == d->target);
}

/**
 * rpmb_dev_find_by_device() - retrieve rpmb device from the parent device
 * @parent: parent device of the rpmb device
 * @target: RPMB target/region within the physical device
 *
 * Return: NULL if there is no rpmb device associated with the parent device
 */
struct rpmb_dev *rpmb_dev_find_by_device(struct device *parent, u8 target)
{
	struct device_with_target t;

	if (!parent)
		return NULL;

	t.dev = parent;
	t.target = target;

	return rpmb_dev_find_device(&t, match_by_parent);
}
EXPORT_SYMBOL_GPL(rpmb_dev_find_by_device);

static ssize_t type_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct rpmb_dev *rdev = to_rpmb_dev(dev);
	const char *sim;
	ssize_t ret;

	sim = (rdev->ops->type & RPMB_TYPE_SIM) ? ":SIM" : "";
	switch (RPMB_TYPE_HW(rdev->ops->type)) {
	case RPMB_TYPE_EMMC:
		ret = scnprintf(buf, PAGE_SIZE, "EMMC%s\n", sim);
		break;
	case RPMB_TYPE_UFS:
		ret = scnprintf(buf, PAGE_SIZE, "UFS%s\n", sim);
		break;
	case RPMB_TYPE_NVME:
		ret = scnprintf(buf, PAGE_SIZE, "NVMe%s\n", sim);
		break;
	default:
		ret = scnprintf(buf, PAGE_SIZE, "UNKNOWN\n");
		break;
	}

	return ret;
}
static DEVICE_ATTR_RO(type);

static ssize_t id_read(struct file *file, struct kobject *kobj,
		       struct bin_attribute *attr, char *buf,
		       loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct rpmb_dev *rdev = to_rpmb_dev(dev);
	size_t sz = min_t(size_t, rdev->ops->dev_id_len, PAGE_SIZE);

	if (!rdev->ops->dev_id)
		return 0;

	return memory_read_from_buffer(buf, count, &off, rdev->ops->dev_id, sz);
}
static BIN_ATTR_RO(id, 0);

static ssize_t wr_cnt_max_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct rpmb_dev *rdev = to_rpmb_dev(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", rdev->ops->wr_cnt_max);
}
static DEVICE_ATTR_RO(wr_cnt_max);

static ssize_t rd_cnt_max_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct rpmb_dev *rdev = to_rpmb_dev(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", rdev->ops->rd_cnt_max);
}
static DEVICE_ATTR_RO(rd_cnt_max);

static struct attribute *rpmb_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_wr_cnt_max.attr,
	&dev_attr_rd_cnt_max.attr,
	NULL,
};

static struct bin_attribute *rpmb_bin_attributes[] = {
	&bin_attr_id,
	NULL,
};

static struct attribute_group rpmb_attr_group = {
	.attrs = rpmb_attrs,
	.bin_attrs = rpmb_bin_attributes,
};

static const struct attribute_group *rpmb_attr_groups[] = {
	&rpmb_attr_group,
	NULL
};

/**
 * rpmb_dev_unregister() - unregister RPMB partition from the RPMB subsystem
 * @rdev: the rpmb device to unregister
 * Return:
 * *        0 on success
 * *        -EINVAL on wrong parameters
 */
int rpmb_dev_unregister(struct rpmb_dev *rdev)
{
	if (!rdev)
		return -EINVAL;

	mutex_lock(&rdev->lock);
	rpmb_cdev_del(rdev);
	device_del(&rdev->dev);
	mutex_unlock(&rdev->lock);

	rpmb_dev_put(rdev);

	return 0;
}
EXPORT_SYMBOL_GPL(rpmb_dev_unregister);

/**
 * rpmb_dev_unregister_by_device() - unregister RPMB partition
 *     from the RPMB subsystem
 * @dev: the parent device of the rpmb device
 * @target: RPMB target/region within the physical device
 * Return:
 * *        0 on success
 * *        -EINVAL on wrong parameters
 * *        -ENODEV if a device cannot be find.
 */
int rpmb_dev_unregister_by_device(struct device *dev, u8 target)
{
	struct rpmb_dev *rdev;

	if (!dev)
		return -EINVAL;

	rdev = rpmb_dev_find_by_device(dev, target);
	if (!rdev) {
		dev_warn(dev, "no disk found %s\n", dev_name(dev->parent));
		return -ENODEV;
	}

	rpmb_dev_put(rdev);

	return rpmb_dev_unregister(rdev);
}
EXPORT_SYMBOL_GPL(rpmb_dev_unregister_by_device);

/**
 * rpmb_dev_get_drvdata() - driver data getter
 * @rdev: rpmb device
 *
 * Return: driver private data
 */
void *rpmb_dev_get_drvdata(const struct rpmb_dev *rdev)
{
	return dev_get_drvdata(&rdev->dev);
}
EXPORT_SYMBOL_GPL(rpmb_dev_get_drvdata);

/**
 * rpmb_dev_set_drvdata() - driver data setter
 * @rdev: rpmb device
 * @data: data to store
 */
void rpmb_dev_set_drvdata(struct rpmb_dev *rdev, void *data)
{
	dev_set_drvdata(&rdev->dev, data);
}
EXPORT_SYMBOL_GPL(rpmb_dev_set_drvdata);

/**
 * rpmb_dev_register - register RPMB partition with the RPMB subsystem
 * @dev: storage device of the rpmb device
 * @target: RPMB target/region within the physical device
 * @ops: device specific operations
 *
 * Return: a pointer to rpmb device
 */
struct rpmb_dev *rpmb_dev_register(struct device *dev, u8 target,
				   const struct rpmb_ops *ops)
{
	struct rpmb_dev *rdev;
	int id;
	int ret;

	if (!dev || !ops)
		return ERR_PTR(-EINVAL);

	if (!ops->cmd_seq)
		return ERR_PTR(-EINVAL);

	if (!ops->get_capacity)
		return ERR_PTR(-EINVAL);

	if (ops->type == RPMB_TYPE_ANY || ops->type > RPMB_TYPE_MAX)
		return ERR_PTR(-EINVAL);

	rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return ERR_PTR(-ENOMEM);

	id = ida_simple_get(&rpmb_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto exit;
	}

	mutex_init(&rdev->lock);
	rdev->ops = ops;
	rdev->id = id;
	rdev->target = target;

	dev_set_name(&rdev->dev, "rpmb%d", id);
	rdev->dev.class = &rpmb_class;
	rdev->dev.parent = dev;
	rdev->dev.groups = rpmb_attr_groups;

	rpmb_cdev_prepare(rdev);

	ret = device_register(&rdev->dev);
	if (ret)
		goto exit;

	rpmb_cdev_add(rdev);

	dev_dbg(&rdev->dev, "registered device\n");

	return rdev;

exit:
	if (id >= 0)
		ida_simple_remove(&rpmb_ida, id);
	kfree(rdev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(rpmb_dev_register);

static int __init rpmb_init(void)
{
	ida_init(&rpmb_ida);
	class_register(&rpmb_class);
	return rpmb_cdev_init();
}

static void __exit rpmb_exit(void)
{
	rpmb_cdev_exit();
	class_unregister(&rpmb_class);
	ida_destroy(&rpmb_ida);
}

subsys_initcall(rpmb_init);
module_exit(rpmb_exit);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("RPMB class");
MODULE_LICENSE("GPL v2");
