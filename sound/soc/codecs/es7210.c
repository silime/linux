// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo Q706F ES7210 microphone ADC initialization. */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

struct es7210_priv {
	struct regmap *regmap;
};

static int es7210_suspend(struct device *dev)
{
	struct es7210_priv *es7210 = dev_get_drvdata(dev);
	static const struct reg_sequence sleep_regs[] = {
		{ 0x14, 0x03 }, { 0x15, 0x03 }, { 0x06, 0x00 },
		{ 0x4b, 0xff }, { 0x4c, 0xff }, { 0x0b, 0xd0 },
		{ 0x40, 0x80 }, { 0x01, 0x7f }, { 0x06, 0x07 },
	};

	return regmap_multi_reg_write(es7210->regmap, sleep_regs,
				      ARRAY_SIZE(sleep_regs));
}

static int es7210_resume(struct device *dev)
{
	struct es7210_priv *es7210 = dev_get_drvdata(dev);
	static const struct reg_sequence wake_regs[] = {
		{ 0x14, 0x03 }, { 0x15, 0x03 }, { 0x06, 0x00 },
		{ 0x01, 0x20 }, { 0x40, 0x42 }, { 0x0b, 0x02 },
		{ 0x4b, 0x00 }, { 0x4c, 0x00 }, { 0x14, 0x00 },
		{ 0x15, 0x00 },
	};

	return regmap_multi_reg_write(es7210->regmap, wake_regs,
				      ARRAY_SIZE(wake_regs));
}

static DEFINE_SIMPLE_DEV_PM_OPS(es7210_pm_ops, es7210_suspend, es7210_resume);

static const struct reg_sequence es7210_init_regs[] = {
	{ 0x00, 0xff }, { 0x00, 0x32 }, { 0x0d, 0x09 },
	{ 0x09, 0x30 }, { 0x0a, 0x30 }, { 0x23, 0x2a },
	{ 0x22, 0x0a }, { 0x21, 0x2a }, { 0x20, 0x0a },
	{ 0x08, 0x14 }, { 0x11, 0x60 }, { 0x12, 0x00 },
	{ 0x40, 0xc3 }, { 0x41, 0x70 }, { 0x42, 0x70 },
	{ 0x1b, 0xbf }, { 0x1c, 0xbf }, { 0x1d, 0xbf },
	{ 0x1e, 0xbf }, { 0x43, 0x10 }, { 0x44, 0x10 },
	{ 0x45, 0x10 }, { 0x46, 0x10 }, { 0x47, 0x08 },
	{ 0x48, 0x08 }, { 0x49, 0x08 }, { 0x4a, 0x08 },
	{ 0x07, 0x20 }, { 0x02, 0x41 }, { 0x06, 0x00 },
	{ 0x4b, 0x0f }, { 0x4c, 0x0f }, { 0x00, 0x71 },
	{ 0x00, 0x41 },
};

static const struct regmap_config es7210_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static int es7210_probe(struct i2c_client *client)
{
	struct es7210_priv *es7210;
	int ret;

	es7210 = devm_kzalloc(&client->dev, sizeof(*es7210), GFP_KERNEL);
	if (!es7210)
		return -ENOMEM;
	ret = devm_regulator_get_enable(&client->dev, "vdd");
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to enable VDD\n");
	msleep(20);

	es7210->regmap = devm_regmap_init_i2c(client, &es7210_regmap_config);
	if (IS_ERR(es7210->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(es7210->regmap),
				     "failed to create regmap\n");

	/* The BSP requires 50 ms between reset and the setup sequence. */
	ret = regmap_write(es7210->regmap, 0x00, 0xff);
	if (ret)
		return dev_err_probe(&client->dev, ret, "reset failed\n");
	msleep(50);
	ret = regmap_multi_reg_write(es7210->regmap, &es7210_init_regs[1],
				     ARRAY_SIZE(es7210_init_regs) - 1);
	if (ret)
		return dev_err_probe(&client->dev, ret, "initialization failed\n");

	i2c_set_clientdata(client, es7210);
	dev_info(&client->dev, "four-channel microphone ADC initialized\n");
	return 0;
}

static const struct of_device_id es7210_of_match[] = {
	{ .compatible = "everest,es7210" },
	{ }
};
MODULE_DEVICE_TABLE(of, es7210_of_match);

static struct i2c_driver es7210_driver = {
	.driver = {
		.name = "es7210",
		.of_match_table = es7210_of_match,
		.pm = pm_sleep_ptr(&es7210_pm_ops),
	},
	.probe = es7210_probe,
};
module_i2c_driver(es7210_driver);

MODULE_DESCRIPTION("Everest Semi ES7210 microphone ADC initializer");
MODULE_LICENSE("GPL");
