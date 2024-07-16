// SPDX-License-Identifier: GPL-2.0-only
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
//   Copyright (c) 2024 Caleb Connolly <caleb@postmarketos.org>
#define DEBUG

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct samsung_amsa26zp01 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
};

static inline
struct samsung_amsa26zp01 *to_samsung_amsa26zp01(struct drm_panel *panel)
{
	return container_of(panel, struct samsung_amsa26zp01, panel);
}

static void samsung_amsa26zp01_reset(struct samsung_amsa26zp01 *amb655x)
{
	gpiod_set_value_cansleep(amb655x->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(amb655x->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(amb655x->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int samsung_amsa26zp01_on(struct samsung_amsa26zp01 *amb655x)
{
	struct drm_dsc_picture_parameter_set pps;
	struct mipi_dsi_device *dsi = amb655x->dsi;
	struct mipi_dsi_multi_context ctx = { .dsi = dsi };
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	drm_dsc_pps_payload_pack(&pps, &amb655x->dsc);
	// print_hex_dump_bytes("pps: ", DUMP_PREFIX_OFFSET, &pps, sizeof(pps));
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xfc, 0x5a, 0x5a);
    // mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	// mipi_dsi_msleep(&ctx, 11);
	/*
	 * This panel uses PPS selectors with offset:
	 * PPS 1 if pps_identifier is 0
	 * PPS 2 if pps_identifier is 1
	 */
	mipi_dsi_compression_mode_ext_multi(&ctx, true,
				      MIPI_DSI_COMPRESSION_DSC, 1);
    /*
    send pps
     */
	mipi_dsi_dcs_write_buffer_multi(&ctx, &pps, sizeof(pps));
    // mipi_dsi_dcs_write_long_multi(&ctx, 0x9e,
	// 		       0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x06, 0x40,
	// 		       0x0a, 0x00, 0x00, 0x64, 0x05, 0x00, 0x05, 0x00,
	// 		       0x02, 0x00, 0x03, 0x81, 0x00, 0x20, 0x0d, 0xbd,
	// 		       0x00, 0x11, 0x00, 0x0c, 0x00, 0xf9, 0x00, 0x6d,
	// 		       0x18, 0x00, 0x10, 0xd0, 0x03, 0x0c, 0x20, 0x00,
	// 		       0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
	// 		       0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b,
	// 		       0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	// 		       0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19, 0xf8,
	// 		       0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a, 0xf6,
	// 		       0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x63, 0xf4);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf8, 0x58, 0x00, 0x10, 0x54);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf9, 0x04, 0x85);
    /* 60 Hz */
    // mipi_dsi_dcs_write_long_multi(&ctx, 0x60, 0x20);
    /* 120 Hz */
	mipi_dsi_dcs_write_long_multi(&ctx, 0x60, 0x20);
	msleep(50);
	mipi_dsi_dcs_write_long_multi(&ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x28);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf8, 0x58, 0x00, 0x30, 0x35);
    mipi_dsi_msleep(&ctx, 110);
	mipi_dsi_dcs_write_long_multi(&ctx, 0xf9, 0xc0, 0x2b);
    mipi_dsi_msleep(&ctx, 110);
	mipi_dsi_dcs_set_display_on_multi(&ctx);

	// mipi_dsi_dcs_write_long_multi(&ctx, 0x9F, 0x5A, 0x5A);
	// mipi_dsi_dcs_write_seq_multi(&ctx, MIPI_DCS_ENTER_NORMAL_MODE);
	// mipi_dsi_dcs_write_long_multi(&ctx, 0x9F, 0xA5, 0xA5);

	return ctx.accum_err;
}

static int samsung_amsa26zp01_off(struct samsung_amsa26zp01 *amb655x)
{
	struct mipi_dsi_device *dsi = amb655x->dsi;
	struct mipi_dsi_multi_context ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	// mipi_dsi_dcs_write_long_multi(&ctx, 0x9f, 0x5a, 0x5a);

	mipi_dsi_dcs_set_display_on_multi(&ctx);
	mipi_dsi_msleep(&ctx, 20);

	mipi_dsi_dcs_set_display_off_multi(&ctx);
	mipi_dsi_msleep(&ctx, 20);

	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);
	// mipi_dsi_dcs_write_long_multi(&ctx, 0x9f, 0xa5, 0xa5);

	mipi_dsi_msleep(&ctx, 150);

	return ctx.accum_err;
}

static int samsung_amsa26zp01_prepare(struct drm_panel *panel)
{
	struct samsung_amsa26zp01 *ctx = to_samsung_amsa26zp01(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	/*
	 * During the first call to prepare, the regulators are already enabled
	 * since they're boot-on. Avoid enabling them twice so we keep the refcounts
	 * balanced.
	 */
	if (!regulator_is_enabled(ctx->supplies[0].consumer)) {
		ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		if (ret) {
			dev_err(dev, "Failed to enable regulators: %d\n", ret);
			return ret;
		}
	}

	samsung_amsa26zp01_reset(ctx);

	ret = samsung_amsa26zp01_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	msleep(28);

	return 0;
}

static int samsung_amsa26zp01_unprepare(struct drm_panel *panel)
{
	struct samsung_amsa26zp01 *amb655x = to_samsung_amsa26zp01(panel);
	struct device *dev = &amb655x->dsi->dev;
	int ret;

	ret = samsung_amsa26zp01_off(amb655x);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(amb655x->reset_gpio, 1);

	ret = regulator_bulk_disable(ARRAY_SIZE(amb655x->supplies), amb655x->supplies);
	if (ret) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	return 0;
}

// static const struct drm_display_mode samsung_2k_dsc_60_mode = {
// 	.clock = (2560 + 12 + 12 + 20) * (1600 + 12 + 2 + 16) * 60 / 1000,
// 	.hdisplay = 2560,
// 	.hsync_start = 2560 + 12,
// 	.hsync_end = 2560 + 12 + 12,
// 	.htotal = 2560 + 12 + 12 + 20,
// 	.vdisplay = 1600,
// 	.vsync_start = 1600 + 12,
// 	.vsync_end = 1600 + 12 + 2,
// 	.vtotal = 1600 + 12 + 2 + 16,
// 	.width_mm = 271,
// 	.height_mm = 170,
// };
static const struct drm_display_mode samsung_2k_dsc_120_mode = {
	.clock = (2560 + 20 + 12 + 20) * (1600 + 8 + 2 + 16) * 120 / 1000,
	.hdisplay = 2560,
	.hsync_start = 2560 + 20,
	.hsync_end = 2560 + 20 + 12,
	.htotal = 2560 + 20 + 12 + 20,
	.vdisplay = 1600,
	.vsync_start = 1600 + 8,
	.vsync_end = 1600 + 8 + 2,
	.vtotal = 1600 + 8 + 2 + 16,
	.width_mm = 271,
	.height_mm = 170,
};

static int samsung_amsa26zp01_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &samsung_2k_dsc_120_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs samsung_amsa26zp01_panel_funcs = {
	.prepare = samsung_amsa26zp01_prepare,
	.unprepare = samsung_amsa26zp01_unprepare,
	.get_modes = samsung_amsa26zp01_get_modes,
};

static int samsung_amsa26zp01_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = (u16)backlight_get_brightness(bl);
	
	int ret;
	struct device *dev = &dsi->dev;
	dev_info(dev, "brightness: %d\n", brightness);
	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	return 0;
}

// static int samsung_2k_dsc_bl_get_brightness(struct backlight_device *bl)
// {
// 	struct mipi_dsi_device *dsi = bl_get_data(bl);
// 	u16 brightness;
// 	int ret;

// 	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

// 	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
// 	if (ret < 0)
// 		return ret;

// 	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

// 	return brightness;
// }

static const struct backlight_ops samsung_amsa26zp01_bl_ops = {
	.update_status = samsung_amsa26zp01_bl_update_status,
	// .get_brightness = samsung_2k_dsc_bl_get_brightness,
};
static struct backlight_device *
samsung_amsa26zp01_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2047,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &samsung_amsa26zp01_bl_ops, &props);
}

static int samsung_amsa26zp01_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct samsung_amsa26zp01 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vdd";
	ctx->supplies[2].supply = "avdd";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get vddio regulator\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		dev_err(dev, "Failed to enable regulators: %d\n", ret);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	
	drm_panel_init(&ctx->panel, dev, &samsung_amsa26zp01_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = samsung_amsa26zp01_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 2 */
	ctx->dsc.slice_height = 100;
	ctx->dsc.slice_width = 1280;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(2560 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 2560 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void samsung_amsa26zp01_remove(struct mipi_dsi_device *dsi)
{
	struct samsung_amsa26zp01 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id samsung_amsa26zp01_of_match[] = {
	{ .compatible = "samsung,amsa26zp01" }, //amsa26zp01
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, samsung_amsa26zp01_of_match);

static struct mipi_dsi_driver samsung_amsa26zp01_driver = {
	.probe = samsung_amsa26zp01_probe,
	.remove = samsung_amsa26zp01_remove,
	.driver = {
		.name = "panel-samsung-amsa26zp01",
		.of_match_table = samsung_amsa26zp01_of_match,
	},
};
module_mipi_dsi_driver(samsung_amsa26zp01_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@postmarketos.org>");
MODULE_DESCRIPTION("DRM driver for Samsung AMSA26ZP01 DSC DSI panel");
MODULE_LICENSE("GPL");
