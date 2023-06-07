/*
 * Power Supply driver for G1320
 *
 * Copyright (C) 2021 Ubiquiti Inc.
 * Author: Matt Hsu <matt.hsu@ui.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published bythe Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/power/g1320_power.h>

#define G1320_LDF_EXP_BITS    (5)
#define G1320_LDF_EXP_SIGN    (BIT(G1320_LDF_EXP_BITS - 1))
#define G1320_LDF_EXP_MASK    (G1320_LDF_EXP_SIGN - 1)
#define G1320_LDF_MANT_BITS   (11)
#define G1320_LDF_MANT_SIGN   (BIT(G1320_LDF_MANT_BITS  - 1))
#define G1320_LDF_MANT_MASK	  (G1320_LDF_MANT_SIGN - 1)

static const u32
two_exp_post_pow_table[15] = {
	1,
	2,
	4,
	8,
	16,
	32,
	64,
	128,
	256,
	512,
	1024,
	2048,
	4096,
	8192,
	16384
};

static int g1320_convert2s_complement(u32 value, u32 bitlen)
{
    if (!bitlen || bitlen >= 32)
        return (int)value;

    if (value & (u32)(1 << (bitlen - 1)))
        return (int)(GENMASK(31, bitlen) | value);
    else
        return (int)(GENMASK(bitlen - 1, 0) & value);
}

static int g1320_psu_ldf_conv(struct i2c_client *client,
				int raw_data, enum power_supply_property psp)
{
	int mant, exp;

	mant = (raw_data & G1320_LDF_MANT_MASK);
	exp = (raw_data >> G1320_LDF_MANT_BITS);

	dev_dbg(&client->dev, "mant 0x%02x, exp 0x%02x\n", mant, exp);

	if (mant & G1320_LDF_MANT_SIGN)
		mant = g1320_convert2s_complement(mant, G1320_LDF_MANT_BITS);

	if (exp & G1320_LDF_EXP_SIGN) {
		exp = g1320_convert2s_complement(exp, G1320_LDF_EXP_BITS);
		if (psp == POWER_SUPPLY_PROP_CURRENT_NOW)
			mant = (mant * 1000) / two_exp_post_pow_table[abs(exp)];
		else
			mant = mant / two_exp_post_pow_table[abs(exp)];
	} else {
		if (psp == POWER_SUPPLY_PROP_CURRENT_NOW)
			mant = (mant * 1000) * two_exp_post_pow_table[abs(exp)];
		else
			mant = mant * two_exp_post_pow_table[exp];
	}

	return mant;
}

static int g1320_psu_get_psu_prop(struct i2c_client *client,
		enum power_supply_property psp)
{
	struct g1320_psu *psu = i2c_get_clientdata(client);
	int ret, reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		reg = G1320_REG_READ_IOUT;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		reg = G1320_REG_READ_POUT;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		reg = G1320_REG_READ_TEMP1;
		break;
	case POWER_SUPPLY_PROP_FAN_SPEED:
		reg = G1320_REG_FAN_SPEED1;
		break;
	default:
		return -EINVAL;
	}

	regmap_read(psu->regmap, reg, &ret);

	return g1320_psu_ldf_conv(client, ret, psp);
}

static int g1320_psu_get_volt_prop(struct i2c_client *client)
{
	struct g1320_psu *psu = i2c_get_clientdata(client);
	int ret = 0;

	regmap_read(psu->regmap, G1320_REG_READ_VOUT, &ret);
	return (ret = ret/512);
}

static int g1320_psu_get_present_prop(struct i2c_client *client)
{
	struct g1320_psu *psu = i2c_get_clientdata(client);
	int vout_mode;

	regmap_read(psu->regmap, G1320_REG_VOUT_MODE, &vout_mode);

	/* Let's use VOUT_MODE to tell PSU is existed or not */
	if ((vout_mode & 0xff) == 0x17)
		return 1;
	else
		return 0;
}

static int g1320_psu_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct g1320_psu *psu = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_POWER_NOW:
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_FAN_SPEED:
		val->intval = g1320_psu_get_psu_prop(psu->client, psp);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = g1320_psu_get_volt_prop(psu->client);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = g1320_psu_get_present_prop(psu->client);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property g1320_psu_props[] = {
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_FAN_SPEED,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_PRESENT,
};

static const struct regmap_config g1320_psu_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 16,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
	.max_register	= G1320_REG_END,
};

static const struct power_supply_desc g1320_psu_desc[] = {
	{
		.name		= "g1320-psu0",
		.type		= POWER_SUPPLY_TYPE_MAINS,
		.get_property	= g1320_psu_get_property,
		.properties	= g1320_psu_props,
		.num_properties	= ARRAY_SIZE(g1320_psu_props),
	},
	{
		.name		= "g1320-psu1",
		.type		= POWER_SUPPLY_TYPE_MAINS,
		.get_property	= g1320_psu_get_property,
		.properties	= g1320_psu_props,
		.num_properties	= ARRAY_SIZE(g1320_psu_props),
	},
};

#ifdef CONFIG_OF
static int g1320_dt_init(struct device *dev, struct g1320_psu *psu)
{
	struct device_node *np = dev->of_node;
	static u32 tmp_unit;

	if (!np) {
		dev_err(dev, "no charger OF node\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "g1320,unit", &tmp_unit)) {
		tmp_unit++;
	}

	psu->unit = tmp_unit;
	return 0;
}
#else /* CONFIG_OF */
static int g1320_dt_init(struct device *dev, struct g1320_psu *psu)
{
	return 0;
}
#endif /* CONFIG_OF */

static int g1320_psu_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct g1320_psu *psu;
	u32 ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	psu = devm_kzalloc(&client->dev, sizeof(*psu), GFP_KERNEL);
	if (!psu)
		return -EINVAL;

	psu->client = client;
	i2c_set_clientdata(client, psu);
	psu->regmap = devm_regmap_init_i2c(client,
			&g1320_psu_regmap_config);
	if (IS_ERR(psu->regmap)) {
		dev_err(&client->dev, "Failed to initialize regmap\n");
		return -EINVAL;
	}

	psy_cfg.drv_data = psu;

	g1320_dt_init(&client->dev, psu);

	psu->psy = devm_power_supply_register(&client->dev,
					     &g1320_psu_desc[psu->unit], &psy_cfg);
	if (IS_ERR(psu->psy)) {
		dev_err(&client->dev, "Failed to register power supply\n");
		ret = PTR_ERR(psu->psy);
		return ret;
	}

	return 0;
}

static int g1320_psu_remove(struct i2c_client *client)
{
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id g1320_dt_ids[] = {
	{ .compatible = "g1320,g1320-psu" },
	{ },
};
MODULE_DEVICE_TABLE(of, g1320_dt_ids);
#endif

static const struct i2c_device_id g1320_psu_id[] = {
	{ "g1320-psu", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, g1320_psu_id);

static struct i2c_driver g1320_psu_driver = {
	.driver = {
		.name = "g1320-psu",
	},
	.probe = g1320_psu_probe,
	.remove = g1320_psu_remove,
	.id_table = g1320_psu_id,
};
module_i2c_driver(g1320_psu_driver);

MODULE_DESCRIPTION("G1320 Power supply driver");
MODULE_AUTHOR("Matt Hsu <matt.hsu@ui.com>");
MODULE_LICENSE("GPL");
