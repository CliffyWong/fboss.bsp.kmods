// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * A slot ID driver based on the PCA9535/PCA9534
 * The slot ids read from the four cable cartridges (CCs) are mapped to
 * register 0 and 1 such as:
 * 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
 * ----------- ----------- ----------- -----------
 *  CC-D[0:3]   CC-C[0:3]   CC-B[0:3]   CC-A[0:3]
 * Notes:
 * 1. Change the endianess of the bits before parsing;
 * 2. All four CCs should return the same value;
 * 3. The polarity of the pins could be changed, need to check before
 *    reading;
 *
 * Mapping from the reg value (after reversing) to the slot id:
 * 1110 -> slot 0
 * 1101 -> slot 1
 * 1100 -> slot 2
 * 1011 -> slot 3
 * 1010 -> slot 4
 * 1001 -> slot 5
 * 0000 -> test fixture
 */

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/bitrev.h>
#include <linux/sysfs.h>
#include <linux/hwmon-sysfs.h>

#define DRIVER_NAME "anacapa_slot_id"
#define MASK_LOWER 0x0F
#define MASK_UPPER 0xF0

enum io_exp_type {
	pca9534,
	pca9535,
};

struct slot_id_config {
	u32 val_offset;
	u32 pol_offset;
	u8 val_mask;
	u8 pol_mask;
};

struct pca953x_chip_desc {
	size_t num_slot_id;
	const struct slot_id_config *slot_id_configs;
};

struct pca953x_slot_id_data {
	struct device *dev;
	struct i2c_client *client;
	const struct pca953x_chip_desc *chip;
};

static const struct slot_id_config pca9534_configs[] = {
	{
		.val_offset = 0x00,
		.pol_offset = 0x02,
		.val_mask = MASK_LOWER,
		.pol_mask = MASK_LOWER,
	},
	{
		.val_offset = 0x00,
		.pol_offset = 0x02,
		.val_mask = MASK_UPPER,
		.pol_mask = MASK_UPPER,
	},
};

static const struct slot_id_config pca9535_configs[] = {
	{
		.val_offset = 0x00,
		.pol_offset = 0x04,
		.val_mask = MASK_LOWER,
		.pol_mask = MASK_LOWER,
	},
	{
		.val_offset = 0x00,
		.pol_offset = 0x04,
		.val_mask = MASK_UPPER,
		.pol_mask = MASK_UPPER,
	},
	{
		.val_offset = 0x01,
		.pol_offset = 0x05,
		.val_mask = MASK_LOWER,
		.pol_mask = MASK_LOWER,
	},
	{
		.val_offset = 0x01,
		.pol_offset = 0x05,
		.val_mask = MASK_UPPER,
		.pol_mask = MASK_UPPER,
	},
};

static const struct pca953x_chip_desc pca953x_chips[] = {
	[pca9534] = {
		.num_slot_id = ARRAY_SIZE(pca9534_configs),
		.slot_id_configs = pca9534_configs,
	},
	[pca9535] = {
		.num_slot_id = ARRAY_SIZE(pca9535_configs),
		.slot_id_configs = pca9535_configs,
	},
};

static const struct i2c_device_id slot_device_id[] = {
	{"cc_slot_pca9534", pca9534},
	{"cc_slot_pca9535", pca9535},
	{ },
};

MODULE_DEVICE_TABLE(i2c, slot_device_id);

static u8 reverse_nibble(u8 n)
{
	return bitrev8(n) >> 4;
}

static ssize_t slot_id_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct pca953x_slot_id_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = to_sensor_dev_attr(attr);
	const struct slot_id_config *config;
	int ret;
	u8 val, pol_val, reversed_val;
	int slot_id;

	/* is_visible should prevent this from being called for invalid indices */
	if (sattr->index >= data->chip->num_slot_id)
		return -EINVAL;

	config = &data->chip->slot_id_configs[sattr->index];

	/*
	 * Read the polarity register. If a bit is set, the corresponding
	 * input pin's value is inverted.
	 */
	ret = i2c_smbus_read_byte_data(data->client, config->pol_offset);
	if (ret < 0)
		return ret;
	pol_val = (u8)ret;

	ret = i2c_smbus_read_byte_data(data->client, config->val_offset);
	if (ret < 0)
		return ret;
	val = (u8)ret;

	/* Apply polarity inversion to get the true pin state */
	val ^= pol_val;

	val &= config->val_mask;

	if (config->val_mask == MASK_UPPER)
		val >>= 4;

	reversed_val = reverse_nibble(val);

	switch (reversed_val) {
	case 0b1110:
		slot_id = 0;
		break;
	case 0b1101:
		slot_id = 1;
		break;
	case 0b1100:
		slot_id = 2;
		break;
	case 0b1011:
		slot_id = 3;
		break;
	case 0b1010:
		slot_id = 4;
		break;
	case 0b1001:
		slot_id = 5;
		break;
	case 0:
		slot_id = -1; /* test fixture */
		break;
	case 0b1000:
		slot_id = -2; /* ladakh(25%) test rack */
		break;
	case 0b0100:
		slot_id = -3; /* leh800b(87.5%) test rack */
		break;
	case 0b1111:
		slot_id = -4; /* evt blade */
		break;
	default:
		return sprintf(buf, "unknown\n");
	}

	return sprintf(buf, "%d\n", slot_id);
}

static SENSOR_DEVICE_ATTR_RO(cc_slot_id_0, slot_id, 0);
static SENSOR_DEVICE_ATTR_RO(cc_slot_id_1, slot_id, 1);
static SENSOR_DEVICE_ATTR_RO(cc_slot_id_2, slot_id, 2);
static SENSOR_DEVICE_ATTR_RO(cc_slot_id_3, slot_id, 3);

static struct attribute *slot_id_attrs[] = {
	&sensor_dev_attr_cc_slot_id_0.dev_attr.attr,
	&sensor_dev_attr_cc_slot_id_1.dev_attr.attr,
	&sensor_dev_attr_cc_slot_id_2.dev_attr.attr,
	&sensor_dev_attr_cc_slot_id_3.dev_attr.attr,
	NULL,
};

static umode_t slot_id_is_visible(struct kobject *kobj, struct attribute *attr,
				  int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pca953x_slot_id_data *data = dev_get_drvdata(dev);

	if (n < data->chip->num_slot_id)
		return attr->mode;

	return 0;
}

static const struct attribute_group slot_id_group = {
	.attrs = slot_id_attrs,
	.is_visible = slot_id_is_visible,
};

static const struct attribute_group *slot_id_groups[] = {
	&slot_id_group,
	NULL,
};

static void slot_id_remove(struct i2c_client *client)
{
	/*
	 * Resources are managed by devm or the driver core.
	 * The sysfs group is handled by .dev_groups and is automatically
	 * removed when the driver is unbound from the device.
	 */
}

static int slot_id_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct pca953x_slot_id_data *data;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);

	data->dev = &client->dev;
	data->client = client;
	data->chip = &pca953x_chips[id->driver_data];

	/*
	 * Polarity is not configured at probe time. Instead, it is read
	 * and applied dynamically in the slot_id_show() function to ensure
	 * the value is always correctly interpreted, regardless of the
	 * hardware's polarity setting.
	 */

	return 0;
}

static struct i2c_driver slot_id_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.dev_groups = slot_id_groups,
	},
	.probe = slot_id_probe,
	.remove = slot_id_remove,
	.id_table = slot_device_id,
};

module_i2c_driver(slot_id_driver);

MODULE_AUTHOR("Evan Zong <ezong@celestica.com>");
MODULE_DESCRIPTION("Anacapa Slot ID driver for PCA953x");
MODULE_LICENSE("GPL");
