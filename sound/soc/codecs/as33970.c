// SPDX-License-Identifier: GPL-2.0-only
/* Minimal AS33970 microphone-array DSP driver for Lenovo Q706F. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AS33970_CMD_GET(x) ((x) | 0x100)
#define AS33970_ID(a, b, c, d) ((((a) - 0x20) << 8) | (((b) - 0x20) << 14) | \
				 (((c) - 0x20) << 20) | (((d) - 0x20) << 26))
#define AS33970_ARM_ID AS33970_ID('M', 'C', 'U', ' ')
#define AS33970_CMD_WORDS 13
#define SYS_CMD_VERSION 1
#define SYS_CMD_PARAMETER_VALUE 14
#define SYS_CMD_EVENT_PARAM 15
#define EVENT_USB_RECORD_STARTSTOP BIT(1)
#define EVENT_USB_PLAYBACK_STARTSTOP BIT(5)
#define EVENT_PAR_RATE_MAIN_INPUT 32
#define EVENT_PAR_RATE_HOST_RECORD 38
#define EVENT_PAR_USB_RECORD_STATE 43
#define EVENT_PAR_USB_PLAYBACK_STATE 50
#define PAR_INDEX_I2S_RX_WIDTH 14
#define PAR_INDEX_I2S_RX_NUM_OF_BITS 18
#define PAR_INDEX_I2S_TX_WIDTH 19
#define PAR_INDEX_I2S_TX_NUM_OF_BITS 23

struct as33970_cmd {
	int num_words:16;
	u32 command_id:15;
	u32 reply:1;
	u32 module_id;
	u32 data[AS33970_CMD_WORDS];
};

struct as33970_priv {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct mutex lock;
	u32 rate;
	u32 width;
	u32 frame_bits;
	u32 fw_status;
};

static int as33970_command(struct as33970_priv *as33970, u16 id,
			   u32 module, unsigned int count, const u32 *data,
			   struct as33970_cmd *reply);

static ssize_t fw_status_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct as33970_priv *as33970 = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", as33970->fw_status);
}

static ssize_t fw_status_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct as33970_priv *as33970 = dev_get_drvdata(dev);
	unsigned int status;
	int ret;

	ret = kstrtouint(buf, 0, &status);
	if (ret)
		return ret;
	as33970->fw_status = !!status;
	return count;
}
static DEVICE_ATTR_RW(fw_status);

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct as33970_priv *as33970 = dev_get_drvdata(dev);
	struct as33970_cmd version;
	int ret;

	mutex_lock(&as33970->lock);
	ret = as33970_command(as33970, AS33970_CMD_GET(SYS_CMD_VERSION),
			      AS33970_ARM_ID, 0, NULL, &version);
	mutex_unlock(&as33970->lock);
	if (ret < 4)
		return ret < 0 ? ret : -EIO;

	return sysfs_emit(buf, "%u.%u.%u.%u\n", version.data[0],
			  version.data[1], version.data[2], version.data[3]);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t reset_dsp_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct as33970_priv *as33970 = dev_get_drvdata(dev);
	bool reset;
	int ret;

	ret = kstrtobool(buf, &reset);
	if (ret)
		return ret;
	if (!reset || !as33970->reset)
		return -EINVAL;

	mutex_lock(&as33970->lock);
	gpiod_set_value_cansleep(as33970->reset, 0);
	msleep(50);
	gpiod_set_value_cansleep(as33970->reset, 1);
	msleep(100);
	as33970->fw_status = 0;
	mutex_unlock(&as33970->lock);
	return count;
}
static DEVICE_ATTR_WO(reset_dsp);

static struct attribute *as33970_attrs[] = {
	&dev_attr_fw_status.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_reset_dsp.attr,
	NULL,
};

static const struct attribute_group as33970_attr_group = {
	.attrs = as33970_attrs,
};

static const struct regmap_config as33970_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x2000,
	.cache_type = REGCACHE_NONE,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
};

static int as33970_command(struct as33970_priv *as33970, u16 id,
			   u32 module, unsigned int count, const u32 *data,
			   struct as33970_cmd *reply)
{
	struct as33970_cmd cmd = { .num_words = count, .command_id = id,
				   .module_id = module };
	u32 *raw = (u32 *)&cmd;
	unsigned long timeout;
	int ret;

	if (count > AS33970_CMD_WORDS)
		return -EINVAL;
	memcpy(cmd.data, data, count * sizeof(*data));
	ret = regmap_bulk_write(as33970->regmap, 4, &raw[1], count + 1);
	if (ret)
		return ret;
	if (id & AS33970_CMD_GET(0))
		cmd.num_words = AS33970_CMD_WORDS;
	ret = regmap_bulk_write(as33970->regmap, 0, raw, 1);
	if (ret)
		return ret;

	timeout = jiffies + msecs_to_jiffies(2000);
	do {
		ret = regmap_bulk_read(as33970->regmap, 0, raw, 1);
		if (ret)
			return ret;
		if (cmd.reply)
			break;
		usleep_range(5000, 6000);
	} while (time_before(jiffies, timeout));
	if (!cmd.reply)
		return -ETIMEDOUT;
	if (cmd.num_words < 0)
		return cmd.num_words;
	if (cmd.num_words > AS33970_CMD_WORDS)
		return -EOVERFLOW;
	if (cmd.num_words) {
		ret = regmap_bulk_read(as33970->regmap, 8, cmd.data, cmd.num_words);
		if (ret)
			return ret;
	}
	if (reply)
		*reply = cmd;
	return cmd.num_words;
}

static int as33970_set(struct as33970_priv *as33970, u16 id,
			u32 module, unsigned int count, ...)
{
	u32 data[AS33970_CMD_WORDS];
	va_list args;
	unsigned int i;

	va_start(args, count);
	for (i = 0; i < count; i++)
		data[i] = va_arg(args, unsigned int);
	va_end(args);
	return as33970_command(as33970, id, module, count, data, NULL);
}

static int as33970_record(struct as33970_priv *as33970, bool enable)
{
	int ret;

	if (!enable)
		return as33970_set(as33970, SYS_CMD_EVENT_PARAM, AS33970_ARM_ID, 3,
			EVENT_USB_RECORD_STARTSTOP, EVENT_PAR_USB_RECORD_STATE, 0);

	ret = as33970_set(as33970, SYS_CMD_PARAMETER_VALUE, AS33970_ARM_ID, 2,
			   EVENT_PAR_RATE_MAIN_INPUT, as33970->rate);
	if (ret < 0)
		return ret;
	ret = as33970_set(as33970, SYS_CMD_PARAMETER_VALUE, AS33970_ARM_ID, 2,
			   PAR_INDEX_I2S_TX_WIDTH, as33970->width);
	if (ret < 0)
		return ret;
	ret = as33970_set(as33970, SYS_CMD_PARAMETER_VALUE, AS33970_ARM_ID, 2,
			   PAR_INDEX_I2S_TX_NUM_OF_BITS, as33970->frame_bits);
	if (ret < 0)
		return ret;
	return as33970_set(as33970, SYS_CMD_EVENT_PARAM, AS33970_ARM_ID, 9,
		EVENT_USB_RECORD_STARTSTOP, EVENT_PAR_USB_RECORD_STATE, 1,
		EVENT_PAR_RATE_HOST_RECORD, as33970->rate,
		PAR_INDEX_I2S_RX_WIDTH, as33970->width,
		PAR_INDEX_I2S_RX_NUM_OF_BITS, as33970->frame_bits);
}

static int as33970_playback(struct as33970_priv *as33970, bool enable)
{
	return as33970_set(as33970, SYS_CMD_EVENT_PARAM, AS33970_ARM_ID, 3,
		EVENT_USB_PLAYBACK_STARTSTOP, EVENT_PAR_USB_PLAYBACK_STATE,
		enable);
}

static int as33970_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct as33970_priv *as33970 = snd_soc_component_get_drvdata(dai->component);
	int bits = snd_soc_params_to_frame_size(params);

	if (bits < 0)
		return bits;
	as33970->rate = params_rate(params);
	as33970->width = params_width(params) == 24 ? 2 : 1;
	as33970->frame_bits = bits;
	return 0;
}

static int as33970_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct as33970_priv *as33970 = snd_soc_component_get_drvdata(dai->component);
	int ret;

	mutex_lock(&as33970->lock);
	/* Match the downstream driver: establish the AS33970 stream during
	 * startup, before the Q6AFE DAI is prepared and started. */
	ret = as33970_playback(as33970, true);
	if (ret >= 0)
		ret = as33970_record(as33970, true);
	if (ret < 0)
		as33970_playback(as33970, false);
	mutex_unlock(&as33970->lock);
	if (ret < 0)
		dev_err(as33970->dev, "failed to start recording: %d\n", ret);
	return ret < 0 ? ret : 0;
}

static void as33970_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct as33970_priv *as33970 = snd_soc_component_get_drvdata(dai->component);

	mutex_lock(&as33970->lock);
	as33970_record(as33970, false);
	as33970_playback(as33970, false);
	mutex_unlock(&as33970->lock);
}

static const struct snd_soc_dai_ops as33970_dai_ops = {
	.startup = as33970_startup,
	.shutdown = as33970_shutdown,
	.hw_params = as33970_hw_params,
};

static struct snd_soc_dai_driver as33970_dai = {
	.name = "as33970-i2s-codec",
	.capture = {
		.stream_name = "AS33970 Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &as33970_dai_ops,
};

static const struct snd_soc_component_driver as33970_component = {
	.name = "as33970",
};

static int as33970_probe(struct i2c_client *client)
{
	static const char * const supplies[] = {
		"as33970-vbat", "as33970-avdd13"
	};
	struct as33970_priv *as33970;
	struct as33970_cmd version;
	int ret;

	as33970 = devm_kzalloc(&client->dev, sizeof(*as33970), GFP_KERNEL);
	if (!as33970)
		return -ENOMEM;
	as33970->dev = &client->dev;
	as33970->rate = 48000;
	as33970->width = 1;
	as33970->frame_bits = 32;
	mutex_init(&as33970->lock);
	ret = devm_regulator_bulk_get_enable(&client->dev,
					     ARRAY_SIZE(supplies), supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to enable supplies\n");
	as33970->reset = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(as33970->reset))
		return dev_err_probe(&client->dev, PTR_ERR(as33970->reset), "reset GPIO\n");
	msleep(50);
	gpiod_set_value_cansleep(as33970->reset, 1);
	msleep(500);
	as33970->regmap = devm_regmap_init_i2c(client, &as33970_regmap_config);
	if (IS_ERR(as33970->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(as33970->regmap), "regmap\n");
	i2c_set_clientdata(client, as33970);
	ret = devm_device_add_group(&client->dev, &as33970_attr_group);
	if (ret)
		return ret;

	ret = as33970_command(as33970, AS33970_CMD_GET(SYS_CMD_VERSION),
			       AS33970_ARM_ID, 0, NULL, &version);
	if (ret >= 4)
		dev_info(&client->dev, "firmware %u.%u.%u.%u\n", version.data[0],
			 version.data[1], version.data[2], version.data[3]);
	else
		dev_warn(&client->dev, "firmware query failed: %d\n", ret);

	return devm_snd_soc_register_component(&client->dev, &as33970_component,
					       &as33970_dai, 1);
}

static const struct of_device_id as33970_of_match[] = {
	{ .compatible = "syna,as33970" },
	{ }
};
MODULE_DEVICE_TABLE(of, as33970_of_match);

static struct i2c_driver as33970_driver = {
	.driver = {
		.name = "as33970",
		.of_match_table = as33970_of_match,
	},
	.probe = as33970_probe,
};
module_i2c_driver(as33970_driver);

MODULE_DESCRIPTION("Synaptics AS33970 microphone-array DSP");
MODULE_LICENSE("GPL");
