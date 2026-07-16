// SPDX-License-Identifier: GPL-2.0-only
/* Minimal AS33970 microphone-array DSP driver for Lenovo Q706F. */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AS33970_CMD_GET(x) ((x) | 0x100)
#define AS33970_ID(a, b, c, d) ((((a) - 0x20) << 8) | (((b) - 0x20) << 14) | \
				 (((c) - 0x20) << 20) | (((d) - 0x20) << 26))
#define AS33970_ARM_ID AS33970_ID('M', 'C', 'U', ' ')
#define AS33970_CTRL_ID AS33970_ID('C', 'T', 'R', 'L')
#define AS33970_CMD_WORDS 13
#define SYS_CMD_VERSION 1
#define SYS_CMD_LOADER_VERSION 3
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
#define AS33970_FIRMWARE "as33970/partition_boot2flash.img"
#define AS33970_FW_SIZE 0x6b000
#define AS33970_BOOT_OFFSET 0x100
#define AS33970_BOOT_SIZE 0x2000
#define AS33970_SYSTEM_OFFSET 0x10000
#define AS33970_SYSTEM_SIZE 0x5b000
#define AS33970_BLOCK_SIZE 0x1000
#define AS33970_I2C_DATA_MAX 120
#define AS33970_LOADER_HEADER_SIZE 8
#define AS33970_LOADER_REPLY_MAX 64

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
	u8 loader_seq;
};

static u32 as33970_loader_hash(const void *buffer, size_t length)
{
	const __le16 *words = buffer;
	u32 sum1 = 0xffff, sum2 = 0xffff;
	size_t count = length / sizeof(*words);

	while (count) {
		size_t block = min_t(size_t, count, 359);
		size_t i;

		count -= block;
		for (i = 0; i < block; i++) {
			sum1 += le16_to_cpu(*words++);
			sum2 += sum1;
		}
		sum1 = (sum1 & 0xffff) + (sum1 >> 16);
		sum2 = (sum2 & 0xffff) + (sum2 >> 16);
	}

	sum1 = (sum1 & 0xffff) + (sum1 >> 16);
	sum2 = (sum2 & 0xffff) + (sum2 >> 16);
	return (sum2 << 16) | sum1;
}

static int as33970_raw_write(struct i2c_client *client, u16 address,
			     const void *data, size_t length)
{
	const u8 *src = data;
	u8 buffer[AS33970_I2C_DATA_MAX + 2];
	int ret;

	while (length) {
		size_t chunk = min_t(size_t, length, AS33970_I2C_DATA_MAX);

		buffer[0] = address >> 8;
		buffer[1] = address;
		memcpy(buffer + 2, src, chunk);
		ret = i2c_master_send(client, buffer, chunk + 2);
		if (ret != chunk + 2)
			return ret < 0 ? ret : -EIO;
		address += chunk;
		src += chunk;
		length -= chunk;
	}

	return 0;
}

static int as33970_raw_read(struct i2c_client *client, u16 address,
			    void *data, size_t length)
{
	u8 reg[2] = { address >> 8, address };
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = sizeof(reg), .buf = reg },
		{ .addr = client->addr, .flags = I2C_M_RD,
		  .len = length, .buf = data },
	};
	int ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));

	return ret == ARRAY_SIZE(messages) ? 0 : ret < 0 ? ret : -EIO;
}

static int as33970_wait_byte(struct i2c_client *client, u8 expected,
			     unsigned int timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u8 status[4];
	int ret;

	do {
		ret = as33970_raw_read(client, 0, status, sizeof(status));
		if (!ret && status[0] == expected)
			return 0;
		usleep_range(500, 1000);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static int as33970_loader_command(struct as33970_priv *as33970, u8 command,
				  const void *arguments, size_t argument_size,
				  const void *payload, size_t payload_size,
				  void *reply_data, size_t reply_data_size)
{
	struct i2c_client *client = to_i2c_client(as33970->dev);
	u8 request[12] = { command, as33970->loader_seq };
	u8 reply[AS33970_LOADER_REPLY_MAX];
	u32 checksum;
	u16 reply_size;
	unsigned long timeout;
	int ret, error;

	if (argument_size > sizeof(request) - AS33970_LOADER_HEADER_SIZE)
		return -EINVAL;
	put_unaligned_le16(AS33970_LOADER_HEADER_SIZE + argument_size,
			   request + 2);
	memcpy(request + AS33970_LOADER_HEADER_SIZE, arguments, argument_size);
	checksum = as33970_loader_hash(request,
				       AS33970_LOADER_HEADER_SIZE + argument_size);
	put_unaligned_le32(checksum, request + 4);

	if (payload_size) {
		ret = as33970_raw_write(client,
					 AS33970_LOADER_HEADER_SIZE + argument_size,
					 payload, payload_size);
		if (ret)
			return ret;
	}
	ret = as33970_raw_write(client, 4, request + 4,
				AS33970_LOADER_HEADER_SIZE + argument_size - 4);
	if (ret)
		return ret;
	ret = as33970_raw_write(client, 0, request, 4);
	if (ret || command == 14)
		return ret;

	timeout = jiffies + msecs_to_jiffies(2000);
	do {
		ret = as33970_raw_read(client, 0, reply, 4);
		if (ret)
			return ret;
		if (reply[0] & BIT(7))
			break;
		usleep_range(500, 1000);
	} while (time_before(jiffies, timeout));
	if (!(reply[0] & BIT(7)))
		return -ETIMEDOUT;
	as33970->loader_seq++;
	if (reply[0] & BIT(6)) {
		error = sign_extend32(reply[0] & GENMASK(5, 0), 5);
		return error ?: -EIO;
	}

	reply_size = get_unaligned_le16(reply + 2);
	if (reply_size < AS33970_LOADER_HEADER_SIZE ||
	    reply_size > sizeof(reply))
		return -EPROTO;
	ret = as33970_raw_read(client, 0, reply, reply_size);
	if (ret)
		return ret;
	if (reply_data && reply_data_size)
		memcpy(reply_data, reply + AS33970_LOADER_HEADER_SIZE,
		       min_t(size_t, reply_data_size,
			     reply_size - AS33970_LOADER_HEADER_SIZE));
	return 0;
}

static int as33970_bootloader_block(struct i2c_client *client,
				    const u8 *data, u32 block)
{
	__le32 command[4];
	u32 sum = 0x52 + block + AS33970_BLOCK_SIZE / sizeof(u32);
	unsigned int i;
	int ret;

	for (i = 0; i < AS33970_BLOCK_SIZE; i += sizeof(u32))
		sum += get_unaligned_le32(data + i);
	command[0] = cpu_to_le32(0x52);
	command[1] = cpu_to_le32(block);
	command[2] = cpu_to_le32(~sum);
	command[3] = cpu_to_le32(AS33970_BLOCK_SIZE / sizeof(u32));

	ret = as33970_raw_write(client, 0x10, data, AS33970_BLOCK_SIZE);
	if (ret)
		return ret;
	ret = as33970_raw_write(client, 4, &command[1], 12);
	if (ret)
		return ret;
	ret = as33970_raw_write(client, 0, &command[0], 4);
	if (ret)
		return ret;
	return as33970_wait_byte(client, 0x53, 2000);
}

static size_t as33970_effective_block_size(const u8 *data)
{
	size_t i;

	for (i = AS33970_BLOCK_SIZE; i; i -= sizeof(u32))
		if (get_unaligned_le32(data + i - sizeof(u32)) != 0xffffffff)
			return i;

	/* The vendor loader sends a completely erased block at full size. */
	return AS33970_BLOCK_SIZE;
}

static int as33970_load_firmware(struct as33970_priv *as33970,
				 const struct firmware *firmware)
{
	struct i2c_client *client = to_i2c_client(as33970->dev);
	u8 reply[20];
	__le32 zero = 0;
	unsigned int block;
	int ret;

	if (firmware->size != AS33970_FW_SIZE ||
	    memcmp(firmware->data, "EFLC", 4) ||
	    memcmp(firmware->data + AS33970_SYSTEM_OFFSET, "EFSC", 4))
		return -EINVAL;

	gpiod_set_value_cansleep(as33970->reset, 0);
	msleep(50);
	gpiod_set_value_cansleep(as33970->reset, 1);
	ret = as33970_wait_byte(client, 0x43, 2000);
	if (ret)
		return ret;
	ret = as33970_raw_write(client, 0, &zero, sizeof(zero));
	if (ret)
		return ret;
	ret = as33970_wait_byte(client, 0x53, 2000);
	if (ret)
		return ret;

	for (block = 0; block < AS33970_BOOT_SIZE / AS33970_BLOCK_SIZE; block++) {
		ret = as33970_bootloader_block(client,
			firmware->data + AS33970_BOOT_OFFSET +
			block * AS33970_BLOCK_SIZE, block);
		if (ret)
			return ret;
	}

	zero = cpu_to_le32(0x44);
	ret = as33970_raw_write(client, 0, &zero, sizeof(zero));
	if (ret)
		return ret;
	ret = as33970_wait_byte(client, 0x80, 1000);
	if (ret)
		return ret;
	as33970->loader_seq = 0;
	ret = as33970_loader_command(as33970, 0, NULL, 0, NULL, 0,
				     reply, sizeof(reply));
	if (ret)
		return ret;
	zero = 0;
	ret = as33970_loader_command(as33970, 2, &zero, sizeof(zero),
				     NULL, 0, reply, sizeof(reply));
	if (ret)
		return ret;
	ret = as33970_loader_command(as33970, 1, NULL, 0, NULL, 0,
				     reply, sizeof(reply));
	if (ret)
		return ret;

	for (block = 0; block < AS33970_SYSTEM_SIZE / AS33970_BLOCK_SIZE;
	     block++) {
		const u8 *data = firmware->data + AS33970_SYSTEM_OFFSET +
				 block * AS33970_BLOCK_SIZE;
		size_t length = as33970_effective_block_size(data);
		__le16 arguments[2] = {
			cpu_to_le16(block), cpu_to_le16(length),
		};

		ret = as33970_loader_command(as33970, 8, arguments,
					     sizeof(arguments), data, length,
					     NULL, 0);
		if (ret) {
			dev_err(as33970->dev, "firmware block %u failed: %d\n",
				block, ret);
			return ret;
		}
	}

	/* Let WRITE_VERIFY commit the final block before rebooting the loader. */
	msleep(500);
	ret = as33970_loader_command(as33970, 14, NULL, 0, NULL, 0,
				     NULL, 0);
	if (ret)
		return ret;
	msleep(1000);
	return 0;
}

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
	if (id & AS33970_CMD_GET(0))
		cmd.num_words = AS33970_CMD_WORDS;
	ret = regmap_bulk_write(as33970->regmap, 4, &raw[1],
				cmd.num_words + 1);
	if (ret)
		return ret;
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
	const struct firmware *firmware;
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
	if (ret < 4) {
		dev_info(&client->dev, "loading %s\n", AS33970_FIRMWARE);
		ret = request_firmware(&firmware, AS33970_FIRMWARE, &client->dev);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to request firmware\n");
		ret = as33970_load_firmware(as33970, firmware);
		release_firmware(firmware);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "firmware download failed\n");
		/* The vendor loader performs this handshake after reboot. */
		ret = as33970_command(as33970,
				       AS33970_CMD_GET(SYS_CMD_LOADER_VERSION),
				       AS33970_CTRL_ID, 0, NULL, &version);
		if (ret < 4)
			return dev_err_probe(&client->dev, ret < 0 ? ret : -EIO,
					     "firmware startup handshake failed\n");
		ret = as33970_command(as33970, AS33970_CMD_GET(SYS_CMD_VERSION),
				       AS33970_ARM_ID, 0, NULL, &version);
	}
	if (ret >= 4) {
		as33970->fw_status = 1;
		dev_info(&client->dev, "firmware %u.%u.%u.%u\n", version.data[0],
			 version.data[1], version.data[2], version.data[3]);
	} else {
		return dev_err_probe(&client->dev, ret < 0 ? ret : -EIO,
				     "firmware did not start\n");
	}

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
