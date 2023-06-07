/*
 * Driver for the Texas Instruments INA226 Power Monitor
 * Datasheet:
 * https://www.ti.com/product/INA237-Q1
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/delay.h>

/* register definitions */
#define INA237_CONFIG				0x00
#define INA237_ADCCONFIG_2			0X01
#define INA237_CURRLSBCALC_3		0x02
#define INA237_VSHUNT				0x04
#define INA237_VBUS					0x05
#define INA237_DIETEMP				0x06
#define INA237_CURRENT				0x07
#define INA237_POWER				0x08
#define INA237_DIAG_ALRT			0x0B
#define INA237_SOVL					0x0C
#define INA237_SUVL					0x0D
#define INA237_BOVL					0x0E
#define INA237_BUVL					0x0F
#define INA237_TEMP_LIMIT			0x10
#define INA237_PWR_LIMIT			0x11
#define INA237_MANUFACTURER_ID		0x3E
#define INA237_DEVICE_ID			0x3F

#define INA237_RSHUNT_DEFAULT		2000

#define INA237_MAX_EXPECT_CURRENT_UA_DEFAULT	(16*1000*1000)
#define INA237_CURRENT_LSB_DIVISOR				(32768) // 2^15

struct ina237_data {
	struct i2c_client *client;
	struct mutex update_lock;
	u32 shunt_resistor_uohms;
	u32 max_expect_current_ua;
};

static ssize_t ina237_set_value(struct device *dev,
				struct device_attribute *da,
				const char *buf,
				size_t count)
{
	struct ina237_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int reg = attr->index;
	long val;
	int ret;

	if (IS_ERR(data))
		return PTR_ERR(data);

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	mutex_lock(&data->update_lock);

	switch (attr->index) {
	case INA237_BOVL:
	case INA237_BUVL:
		{
			u16 bovl_reg = DIV_ROUND_CLOSEST((val * 1000), 3125);
			i2c_smbus_write_word_swapped(data->client, reg, bovl_reg);
		}
		break;

	case INA237_PWR_LIMIT:
		{
			u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
			u32 pwr_limit_lsb = 256 * 200 * current_lsb_uA;
			u16 pwr_limit_reg;

			dev_dbg(dev, "val = %ld\n", val);
			dev_dbg(dev, "current_lsb_uA = %d\n", current_lsb_uA);

			pwr_limit_lsb = DIV_ROUND_CLOSEST(pwr_limit_lsb, 1000);
			dev_dbg(dev, "pwr_limit_lsb = %d\n", pwr_limit_lsb);

			val = (val * 1000 * 1000);
			pwr_limit_reg = DIV_ROUND_CLOSEST(val, pwr_limit_lsb);
			i2c_smbus_write_word_swapped(data->client, reg, pwr_limit_reg);
		}
		break;
	}

	mutex_unlock(&data->update_lock);
	return count;
}

static int ina237_calc_comp_val(u16 comp)
{
	int val = 0;
	u16 sign_mask = 0x8000;
	int neg_val = -32768;

	if(comp & sign_mask)
		val += neg_val;

	val += comp & ~(sign_mask);
	return val;
}

static long ina237_get_vbus(struct device *dev)
{
	long val;
	struct ina237_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	u16 vbus_reg = i2c_smbus_read_word_swapped(client, INA237_VBUS);

	dev_dbg(dev, "INA237_VBUS = %d, 0x%04X\n", vbus_reg, vbus_reg);

	val = ina237_calc_comp_val(vbus_reg) * 3125;
	val = DIV_ROUND_CLOSEST(val, 1000);

	return val;
}

static long ina237_get_current(struct device *dev)
{
	long val;
	struct ina237_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
	u16 current_reg = i2c_smbus_read_word_swapped(client, INA237_CURRENT);

	dev_dbg(dev, "current_lsb_uA = %d\n", current_lsb_uA);
	dev_dbg(dev, "current_reg = %d, 0x%04X\n", current_reg, current_reg);

	val = ina237_calc_comp_val(current_reg) * current_lsb_uA;
	val = DIV_ROUND_CLOSEST(val, 1000);
	return val;
}

static ssize_t ina237_show_value(struct device *dev,
				 struct device_attribute *da,
				 char *buf)
{
	struct ina237_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	long val;

	if (IS_ERR(data))
		return PTR_ERR(data);

	switch (attr->index) {
	case INA237_DIETEMP:
		{
			u16 dietemp_reg = i2c_smbus_read_word_swapped(client, attr->index);

			val = (dietemp_reg >> 4) * 125;
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_VSHUNT:
		{
			u16 vshunt_reg = i2c_smbus_read_word_swapped(client, attr->index);
			u16 config_reg = i2c_smbus_read_word_swapped(client, INA237_CONFIG);
			u16 vshunt_lsb = 0;

			if((config_reg & 0x08) == 0x00)
				vshunt_lsb = 5000;
			else
				vshunt_lsb = 1250;


			dev_dbg(dev, "INA237_CONFIG = 0x%04X\n", config_reg);
			dev_dbg(dev, "INA237_VSHUNT = %d, 0x%04X\n", vshunt_reg, vshunt_reg);
			dev_dbg(dev, "vshunt_lsb = %d\n", vshunt_lsb);

			val = ina237_calc_comp_val(vshunt_reg) * vshunt_lsb;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_SOVL:
		{
			u16 sovl_reg = i2c_smbus_read_word_swapped(client, attr->index);
			u16 config_reg = i2c_smbus_read_word_swapped(client, INA237_CONFIG);
			u16 vshunt_lsb = 0;

			if((config_reg & 0x08) == 0x00)
				vshunt_lsb = 5000;
			else
				vshunt_lsb = 1250;

			dev_dbg(dev, "INA237_CONFIG = 0x%04X\n", config_reg);
			dev_dbg(dev, "INA237_SOVL = %d, 0x%04X\n", sovl_reg, sovl_reg);
			dev_dbg(dev, "vshunt_lsb = %d\n", vshunt_lsb);

			val = ina237_calc_comp_val(sovl_reg) * vshunt_lsb;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_SUVL:
		{
			u16 suvl_reg = i2c_smbus_read_word_swapped(client, attr->index);
			u16 config_reg = i2c_smbus_read_word_swapped(client, INA237_CONFIG);
			u16 vshunt_lsb = 0;

			if((config_reg & 0x08) == 0x00)
				vshunt_lsb = 5000;
			else
				vshunt_lsb = 1250;

			dev_dbg(dev, "INA237_CONFIG = 0x%04X\n", config_reg);
			dev_dbg(dev, "INA237_SUVL = %d, 0x%04X\n", suvl_reg, suvl_reg);
			dev_dbg(dev, "vshunt_lsb = %d\n", vshunt_lsb);

			val = ina237_calc_comp_val(suvl_reg) * vshunt_lsb;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_VBUS:
		{
#if 1
			return snprintf(buf, PAGE_SIZE, "%ld\n", ina237_get_vbus(dev));
#else
			u16 vbus_reg = i2c_smbus_read_word_swapped(client, attr->index);

			dev_dbg(dev, "INA237_VBUS = %d, 0x%04X\n", vbus_reg, vbus_reg);

			val = ina237_calc_comp_val(vbus_reg) * 3125;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
#endif
		}
		break;

	case INA237_BOVL:
		{
			u16 bovl_reg = i2c_smbus_read_word_swapped(client, attr->index);

			dev_dbg(dev, "INA237_BOVL = %d, 0x%04X\n", bovl_reg, bovl_reg);

			val = ina237_calc_comp_val(bovl_reg) * 3125;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_BUVL:
		{
			u16 buvl_reg = i2c_smbus_read_word_swapped(client, attr->index);

			dev_dbg(dev, "INA237_BUVL = %d, 0x%04X\n", buvl_reg, buvl_reg);

			val = ina237_calc_comp_val(buvl_reg) * 3125;
			val = DIV_ROUND_CLOSEST(val, 1000);
			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_CURRENT:
		{
#if 1
			return snprintf(buf, PAGE_SIZE, "%ld\n", ina237_get_current(dev));
#else
			u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
			u16 current_reg = i2c_smbus_read_word_swapped(client, attr->index);

			dev_dbg(dev, "current_lsb_uA = %ld\n", current_lsb_uA);
			//printk(">>>>> current_lsb_uA = %ld\n", current_lsb_uA);
			dev_dbg(dev, "current_reg = %d, 0x%04X\n", current_reg, current_reg);
			//printk(">>>>> current_reg = %d, 0x%04X\n", current_reg, current_reg);

			val = ina237_calc_comp_val(current_reg) * current_lsb_uA;
			val = DIV_ROUND_CLOSEST(val, 1000);

			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
#endif
		}
		break;

	case INA237_POWER:
		{
#if 0
			return snprintf(buf, PAGE_SIZE, "%ld\n", (DIV_ROUND_CLOSEST(ina237_get_vbus(dev), 1000) * ina237_get_current(dev)));
#else
			u32 power_reg = 0x00;

			if(i2c_smbus_read_i2c_block_data(client, attr->index, 3, (u8*)&power_reg) == 3) {
				u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
				u32 power_lsb = current_lsb_uA * 200;

				dev_dbg(dev, "current_lsb_uA = %d\n", current_lsb_uA);
				//printk(">>>>> current_lsb_uA = %ld\n", current_lsb_uA);

				power_lsb = DIV_ROUND_CLOSEST(power_lsb, 1000);
				dev_dbg(dev, "power_lsb = %d\n", power_lsb);
				//printk(">>>>> power_lsb = %ld\n", power_lsb);

				dev_dbg(dev, "power_reg = 0x%08X (%d)\n", power_reg, power_reg);
				//printk(">>>>> power_reg = 0x%08X (%d)\n", power_reg, power_reg);
				{
					u32 tmp = (power_reg & 0xff0000) >> 16 |
								(power_reg & 0xff00) |
								((power_reg & 0xff) << 16);
					power_reg = tmp;
				}
				dev_dbg(dev, "swapped power_reg = 0x%08X (%d)\n", power_reg, power_reg);
				//printk(">>>>> swapped power_reg = 0x%08X (%d)\n", power_reg, power_reg);


				val = power_reg * power_lsb;
				val = DIV_ROUND_CLOSEST(val, (1000 * 1000));
				return snprintf(buf, PAGE_SIZE, "%ld\n", val);
			}
#endif
		}
		break;

	case INA237_PWR_LIMIT:
		{
			u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
			u16 pwr_limit_reg = i2c_smbus_read_word_swapped(client, attr->index);
			u32 pwr_limit_lsb = 256 * 200 * current_lsb_uA;

			dev_dbg(dev, "current_lsb_uA = %d\n", current_lsb_uA);

			pwr_limit_lsb = DIV_ROUND_CLOSEST(pwr_limit_lsb, 1000);
			dev_dbg(dev, "pwr_limit_lsb = %d\n", pwr_limit_lsb);
			dev_dbg(dev, "pwr_limit_reg = 0x%08X (%d)\n", pwr_limit_reg, pwr_limit_reg);

			val = pwr_limit_reg * pwr_limit_lsb;
			val = DIV_ROUND_CLOSEST(val, (1000 * 1000));

			return snprintf(buf, PAGE_SIZE, "%ld\n", val);
		}
		break;

	case INA237_MANUFACTURER_ID:
	case INA237_DEVICE_ID:
		{
			u16 id = i2c_smbus_read_word_swapped(client, attr->index);
			return snprintf(buf, PAGE_SIZE, "0x%04X\n", id);
		}
		break;
	}

	return sprintf(buf,"%ld\n", val);
}

static ssize_t ina237_show_shunt_resistor(struct device *dev,
				 struct device_attribute *da,
				 char *buf)
{
	struct ina237_data *data = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", data->shunt_resistor_uohms);
}

/* DIE temperature */
static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, ina237_show_value, NULL, INA237_DIETEMP);
/* Shunt voltage */
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, ina237_show_value, NULL, INA237_VSHUNT);
static SENSOR_DEVICE_ATTR(in0_input_max, S_IRUGO, ina237_show_value, NULL, INA237_SOVL);
static SENSOR_DEVICE_ATTR(in0_input_min, S_IRUGO, ina237_show_value, NULL, INA237_SUVL);
/* Bus voltage */
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, ina237_show_value, NULL, INA237_VBUS);
static SENSOR_DEVICE_ATTR(in1_input_max, S_IRUGO | S_IWUSR, ina237_show_value, ina237_set_value, INA237_BOVL);
static SENSOR_DEVICE_ATTR(in1_input_min, S_IRUGO | S_IWUSR, ina237_show_value, ina237_set_value, INA237_BUVL);
/* Current */
static SENSOR_DEVICE_ATTR(curr1_input, S_IRUGO, ina237_show_value, NULL, INA237_CURRENT);
/* Power */
static SENSOR_DEVICE_ATTR(power1_input, S_IRUGO, ina237_show_value, NULL,  INA237_POWER);
/* Power Limit */
static SENSOR_DEVICE_ATTR(power1_max, S_IRUGO | S_IWUSR, ina237_show_value, ina237_set_value, INA237_PWR_LIMIT);
/* Manufacturer ID */
static SENSOR_DEVICE_ATTR(manufacturer_id, S_IRUGO, ina237_show_value, NULL, INA237_MANUFACTURER_ID);
/* Device ID */
static SENSOR_DEVICE_ATTR(device_id, S_IRUGO, ina237_show_value, NULL, INA237_DEVICE_ID);
/* Shunt Resistor */
static SENSOR_DEVICE_ATTR(shunt_resistor, S_IRUGO, ina237_show_shunt_resistor, NULL, 0);

static struct attribute *ina237_attrs[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in0_input_max.dev_attr.attr,
	&sensor_dev_attr_in0_input_min.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in1_input_max.dev_attr.attr,
	&sensor_dev_attr_in1_input_min.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_power1_input.dev_attr.attr,
	&sensor_dev_attr_power1_max.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_manufacturer_id.dev_attr.attr,
	&sensor_dev_attr_device_id.dev_attr.attr,
	&sensor_dev_attr_shunt_resistor.dev_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ina237);

static int ina237_init_client(struct i2c_client *client,
			      struct ina237_data *data)
{
	return 0;
}

static int ina237_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct i2c_adapter *adapter = client->adapter;
	struct ina237_data *data;
	struct device *hwmon_dev;
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);
	data->client = client;
	mutex_init(&data->update_lock);

	ret = ina237_init_client(client, data);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_groups(&client->dev,
							   client->name,
							   data, ina237_groups);
	if (IS_ERR(hwmon_dev)) {
		ret = PTR_ERR(hwmon_dev);
		return ret;
	}

	if (of_property_read_u32(dev->of_node, "shunt-resistor-uohms", &data->shunt_resistor_uohms) < 0)
		data->shunt_resistor_uohms = INA237_RSHUNT_DEFAULT;

	if (of_property_read_u32(dev->of_node, "max-expect-current-ua", &data->max_expect_current_ua) < 0)
		data->max_expect_current_ua = INA237_MAX_EXPECT_CURRENT_UA_DEFAULT;

	{
		u32 current_lsb_uA = DIV_ROUND_CLOSEST(data->max_expect_current_ua, INA237_CURRENT_LSB_DIVISOR);
		u32 tmp;
		u16 currlsbcalc_reg;

		tmp  =  DIV_ROUND_CLOSEST((current_lsb_uA * 819), 1000);
		currlsbcalc_reg = DIV_ROUND_CLOSEST((tmp * data->shunt_resistor_uohms), 1000);

		dev_dbg(dev, "currlsbcalc_reg = 0x%04X\n", currlsbcalc_reg);
		i2c_smbus_write_word_swapped(data->client, INA237_CURRLSBCALC_3, currlsbcalc_reg);
	}

	dev_info(&client->dev, "%s driver is register\n", client->name);
	return 0;
}

static int ina237_remove(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id ina237_id[] = {
	{ "ina237", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ina237_id);

/* This is the driver that will be inserted */
static struct i2c_driver ina237_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "ina237",
	},
	.probe		= ina237_probe,
	.remove		= ina237_remove,
	.id_table	= ina237_id,
};

module_i2c_driver(ina237_driver);

MODULE_AUTHOR("matthew.chen@ui.com");
MODULE_DESCRIPTION("INA237 driver");
MODULE_LICENSE("GPL");
