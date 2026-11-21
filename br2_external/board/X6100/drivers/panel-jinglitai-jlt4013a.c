// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Jinglitai JLT4013A LCD Panel.
 *
 * Copyright (C) Rui Oliveira 2022
 * Copyright (C) Oleg Belousov 2022
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <drm/drm_device.h>
#include <drm/drm_connector.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/version.h>
#include <linux/sysfs.h>

#define ST7701S_SWRESET 0x01
#define ST7701S_SLPOUT 0x11
#define ST7701S_DISPOFF 0x28
#define ST7701S_DISPON 0x29
#define ST7701S_COLMOD 0x3A

#define ST7701S_CN2BKxSEL 0xFF

/* BK0 */

#define ST7701S_LNESET 0xC0
#define ST7701S_PORCTRL 0xC1
#define ST7701S_INVSET 0xC2
#define ST7701S_PVGAMCTRL 0xB0
#define ST7701S_NVGAMCTRL 0xB1

/* BK1 */

#define ST7701S_VRHS 0xB0
#define ST7701S_VCOM 0xB1
#define ST7701S_VGHSS 0xB2
#define ST7701S_TESTCMD 0xB3
#define ST7701S_VGLS 0xB5
#define ST7701S_PWCTRL1 0xB7
#define ST7701S_PWCTRL2 0xB8
#define ST7701S_PWCTRL3 0xB9
#define ST7701S_SPD1 0xC1
#define ST7701S_SPD2 0xC2
#define ST7701S_MIPISET1 0xD0

#define ST7701S_TRY(val, func)                                                          \
	do {                                                                            \
		if ((val = (func))) {                                                   \
			pr_warn("Jinglitai JLT4013A: SPI write failed with error %d\n", \
				val);                                                   \
			goto error;                                                     \
		}                                                                       \
	} while (0)

static const struct of_device_id jlt4013a_of_match[] = {
	{ .compatible = "sitronix,st7701s" },
	{ .compatible = "jinglitai,jlt4013a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jlt4013a_of_match);

/*
 * Xiegu's own mode (stock firmware, kernel 5.8.9, panel-jlt4013a-pss): 14.616 MHz,
 * 525 x 928, ~30 Hz. The 2022 mode (27.5 MHz, 542 x 843, 60 Hz) doubles the picture
 * a few lines apart on newer panels, which take it only at this rate.
 */
static const struct drm_display_mode jlt4013a_default_display_mode = {
	.clock = 14616,
	.hdisplay = 480,
	.hsync_start = 480 + 32,
	.hsync_end = 480 + 32 + 11,
	.htotal = 480 + 32 + 11 + 2,
	.vdisplay = 800,
	.vsync_start = 800 + 54,
	.vsync_end = 800 + 54 + 41,
	.vtotal = 800 + 54 + 41 + 33,
	.width_mm = 52,
	.height_mm = 86,
};

/*
 * Xiegu's ST7701S init, from the stock U-Boot 2017.11 (19 Aug 2024): the stock kernel
 * sends nothing and keeps what U-Boot set. { command, data length, data... }; a length
 * of 0xff is a delay of data[0] ms. RGBCTRL (C3) stays at its default: the panel
 * samples pixel data on the rising edge (bus_flags below).
 */
#define ST7701S_DELAY 0xff
static const u8 jlt4013a_init_seq[][18] = {
	{ ST7701S_SWRESET, 0 },
	{ ST7701S_DELAY, 0xff, 50 },
	{ ST7701S_SLPOUT, 0 },
	{ ST7701S_DELAY, 0xff, 120 },
	/* BK0 */
	{ ST7701S_CN2BKxSEL, 5, 0x77, 0x01, 0x00, 0x00, 0x10 },
	{ ST7701S_LNESET, 2, 0x63, 0x00 },
	{ ST7701S_PORCTRL, 2, 0x11, 0x02 },
	{ ST7701S_INVSET, 2, 0x31, 0x03 },
	{ 0xCC, 1, 0x10 },
	{ ST7701S_PVGAMCTRL, 16, 0x40, 0x01, 0x46, 0x0D, 0x13, 0x09, 0x05, 0x09,
				 0x09, 0x1B, 0x07, 0x15, 0x12, 0x4C, 0x10, 0xC8 },
	{ ST7701S_NVGAMCTRL, 16, 0x40, 0x02, 0x86, 0x0D, 0x13, 0x09, 0x05, 0x09,
				 0x09, 0x1F, 0x07, 0x15, 0x12, 0x15, 0x19, 0x08 },
	/* BK1 */
	{ ST7701S_CN2BKxSEL, 5, 0x77, 0x01, 0x00, 0x00, 0x11 },
	{ ST7701S_VRHS, 1, 0x50 },
	{ ST7701S_VCOM, 1, 0x68 },
	{ ST7701S_VGHSS, 1, 0x07 },
	{ ST7701S_TESTCMD, 1, 0x80 },
	{ ST7701S_VGLS, 1, 0x47 },
	{ ST7701S_PWCTRL1, 1, 0x85 },
	{ ST7701S_PWCTRL2, 1, 0x21 },
	{ ST7701S_PWCTRL3, 1, 0x10 },
	{ ST7701S_SPD1, 2, 0x21, 0x36 },
	{ ST7701S_SPD2, 1, 0x78 },
	{ ST7701S_MIPISET1, 1, 0x88 },
	{ ST7701S_DELAY, 0xff, 100 },
	/* Gate timing (GIP) */
	{ 0xE0, 3, 0x00, 0x00, 0x02 },
	{ 0xE1, 11, 0x08, 0x00, 0x0A, 0x00, 0x07, 0x00, 0x09, 0x00, 0x00, 0x33, 0x33 },
	{ 0xE2, 13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0xE3, 4, 0x00, 0x00, 0x33, 0x33 },
	{ 0xE4, 2, 0x44, 0x44 },
	{ 0xE5, 16, 0x0E, 0x2D, 0xA0, 0xA0, 0x10, 0x2D, 0xA0, 0xA0,
		    0x0A, 0x2D, 0xA0, 0xA0, 0x0C, 0x2D, 0xA0, 0xA0 },
	{ 0xE6, 4, 0x00, 0x00, 0x33, 0x33 },
	{ 0xE7, 2, 0x44, 0x44 },
	{ 0xE8, 16, 0x0D, 0x2D, 0xA0, 0xA0, 0x0F, 0x2D, 0xA0, 0xA0,
		    0x09, 0x2D, 0xA0, 0xA0, 0x0B, 0x2D, 0xA0, 0xA0 },
	{ 0xEB, 7, 0x02, 0x01, 0xE4, 0xE4, 0x44, 0x00, 0x40 },
	{ 0xEC, 2, 0x02, 0x01 },
	{ 0xED, 16, 0xAB, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF, 0xFF,
		    0xFF, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xBA },
	/* back to the command set */
	{ ST7701S_CN2BKxSEL, 5, 0x77, 0x01, 0x00, 0x00, 0x00 },
	{ ST7701S_DELAY, 0xff, 10 },
	{ ST7701S_COLMOD, 1, 0x77 },
	{ ST7701S_DISPON, 0 },
};


struct jlt4013a {
	struct drm_panel panel;
	struct spi_device *spi;
	struct gpio_desc *reset;
	struct gpio_desc *dcx;
	struct regulator *supply;
	struct mutex lock;
};

static int st7701s_spi_write(struct jlt4013a *ctx, u8 data)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;

	spi_message_init(&msg);

	xfer.tx_buf = &data;
	xfer.bits_per_word = 8;
	xfer.len = sizeof(data);

	spi_message_add_tail(&xfer, &msg);
	return spi_sync(ctx->spi, &msg);
}

static int st7701s_write_command(struct jlt4013a *ctx, u8 cmd)
{
	// pr_info("Jinglitai JLT4013A: Writing SPI command with value %x\n", cmd);

	gpiod_set_value(ctx->dcx, 0);

	return st7701s_spi_write(ctx, cmd);
}

static int st7701s_write_data(struct jlt4013a *ctx, u8 cmd)
{
	// pr_info("Jinglitai JLT4013A: Writing SPI data with value %x\n", cmd);

	gpiod_set_value(ctx->dcx, 1);

	return st7701s_spi_write(ctx, cmd);
}

static inline struct jlt4013a *panel_to_jlt4013a(struct drm_panel *panel)
{
	return container_of(panel, struct jlt4013a, panel);
}

static int jlt4013a_prepare(struct drm_panel *panel)
{
	int ret, i, j;
	struct jlt4013a *ctx = panel_to_jlt4013a(panel);

	mutex_lock(&ctx->lock);

	/* Enable power supply */

	pr_info("Jinglitai JLT4013A: Trying to enable power supply\n");
	ret = regulator_enable(ctx->supply);
	if (ret) {
		pr_err("Jinglitai JLT4013A: Failed to enable power supply\n");
		goto error;
	}
	msleep(120);
	pr_info("Jinglitai JLT4013A: Enabled power supply\n");

	/* Reset routine */
	pr_info("Jinglitai JLT4013A: Doing the reset routine\n");
	gpiod_set_value(ctx->reset, 1);
	msleep(120);
	gpiod_set_value(ctx->reset, 0);
	msleep(120); // Sleep mandated by the datasheet
	pr_info("Jinglitai JLT4013A: Panel is reset\n");

	/* Initialization routine: Xiegu's (jlt4013a_init_seq) */
	pr_info("Jinglitai JLT4013A: Doing the initialization routine\n");

	for (i = 0; i < ARRAY_SIZE(jlt4013a_init_seq); i++) {
		const u8 *c = jlt4013a_init_seq[i];

		if (c[0] == ST7701S_DELAY && c[1] == 0xff) {
			msleep(c[2]);
			continue;
		}
		ST7701S_TRY(ret, st7701s_write_command(ctx, c[0]));
		for (j = 0; j < c[1]; j++)
			ST7701S_TRY(ret, st7701s_write_data(ctx, c[2 + j]));
	}

	pr_info("Jinglitai JLT4013A: Panel is initialized\n");

	mutex_unlock(&ctx->lock);
	return 0;

error:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int jlt4013a_unprepare(struct drm_panel *panel)
{
	struct jlt4013a *ctx = panel_to_jlt4013a(panel);

	int ret = regulator_disable(ctx->supply);
	return ret;
}


static int jlt4013a_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	/* Xiegu's: data driven on the falling edge, sampled on the rising one (RGBCTRL default) */
	static const u32 bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;


	struct drm_display_mode *mode = drm_mode_duplicate(
		connector->dev, &jlt4013a_default_display_mode);
	if (mode == NULL) {
		dev_err(panel->dev,
			"Jinglitai JLT4013A: Failed to add default mode\n");
		return -EAGAIN;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bpc = 8;
	connector->display_info.bus_flags = bus_flags;

	drm_mode_probed_add(connector, mode);

	drm_display_info_set_bus_formats(&connector->display_info, &bus_format,
					 1);

	return 1;
}

static int jlt4013a_enable(struct drm_panel *panel)
{
	return 0;
}

static int jlt4013a_disable(struct drm_panel *panel)
{
	return 0;
}

static enum drm_panel_orientation jlt4013a_get_orientation(struct drm_panel *panel)
{
    enum drm_panel_orientation orientation;
    int ret;

    ret = of_drm_get_panel_orientation(panel->dev->of_node, &orientation);
    if (ret < 0)
        return DRM_MODE_PANEL_ORIENTATION_UNKNOWN;

    return orientation;
}

/* --- sysfs immed_msg interface --- */

static ssize_t immed_msg_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
    struct jlt4013a *ctx = dev_get_drvdata(dev);
    u8 bytes[64];
    int nbytes = 0;
    int ret;
    char *workbuf, *token, *next;
    int i;

    if (count == 0 || count > 191)
        return -EINVAL;

    workbuf = kstrdup(buf, GFP_KERNEL);
    if (!workbuf)
        return -ENOMEM;

    next = workbuf;
    while ((token = strsep(&next, " \t\n")) != NULL) {
        if (*token == '\0')   /* skip empty tokens */
            continue;
        if (nbytes >= ARRAY_SIZE(bytes))
            break;
        if (kstrtou8(token, 16, &bytes[nbytes]) < 0) {
            kfree(workbuf);
            return -EINVAL;
        }
        nbytes++;
    }
    kfree(workbuf);

    if (nbytes == 0)
        return -EINVAL;

    mutex_lock(&ctx->lock);

    ret = st7701s_write_command(ctx, bytes[0]);
    if (ret)
        goto out_unlock;

    for (i = 1; i < nbytes; i++) {
        ret = st7701s_write_data(ctx, bytes[i]);
        if (ret)
            goto out_unlock;
    }

out_unlock:
    mutex_unlock(&ctx->lock);
    return ret ? ret : count;
}
static DEVICE_ATTR_WO(immed_msg);

static struct attribute *jlt4013a_attrs[] = {
    &dev_attr_immed_msg.attr,
    NULL,
};
static const struct attribute_group jlt4013a_attr_group = {
    .attrs = jlt4013a_attrs,
};

static const struct drm_panel_funcs jlt4013afuncs = {
	.prepare = jlt4013a_prepare,
	.unprepare = jlt4013a_unprepare,
	.get_modes = jlt4013a_get_modes,
	.enable = jlt4013a_enable,
	.disable = jlt4013a_disable,
	.get_orientation = jlt4013a_get_orientation,
};

static int jlt4013a_probe(struct spi_device *spi)
{
	int err;
	struct device *dev = &spi->dev;
	struct jlt4013a *ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return -EAGAIN;

	ctx->spi = spi;
	spi_set_drvdata(spi, ctx);

	mutex_init(&ctx->lock);

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply)) {
		dev_err(dev,
			"Jinglitai JLT4013A: Failed to get power supply\n");
		return PTR_ERR(ctx->supply);
	}

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(dev, "Jinglitai JLT4013A: Failed to get reset GPIO\n");
		return PTR_ERR(ctx->reset);
	}

	ctx->dcx = devm_gpiod_get(dev, "dcx", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->dcx)) {
		dev_err(dev, "Jinglitai JLT4013A: Failed to get dcx GPIO\n");
		return PTR_ERR(ctx->dcx);
	}

	drm_panel_init(&ctx->panel, dev, &jlt4013afuncs,
		       DRM_MODE_CONNECTOR_DPI);

	err = drm_panel_of_backlight(&ctx->panel);
	if (err)
		return err;

	drm_panel_add(&ctx->panel);

	/* Create sysfs group */
	err = sysfs_create_group(&dev->kobj, &jlt4013a_attr_group);
	if (err)
		dev_warn(dev, "Failed to create sysfs group: %d\n", err);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0)
static void jlt4013a_remove(struct spi_device *spi)
{
	struct jlt4013a *ctx = spi_get_drvdata(spi);

	sysfs_remove_group(&spi->dev.kobj, &jlt4013a_attr_group);
	drm_panel_remove(&ctx->panel);
	mutex_destroy(&ctx->lock);
}
#else
static int jlt4013a_remove(struct spi_device *spi)
{
	struct jlt4013a *ctx = spi_get_drvdata(spi);

	sysfs_remove_group(&spi->dev.kobj, &jlt4013a_attr_group);
	drm_panel_remove(&ctx->panel);
	mutex_destroy(&ctx->lock);
	return 0;
}
#endif

static struct spi_driver jlt4013a_driver = {
	.probe		= jlt4013a_probe,
	.remove		= jlt4013a_remove,
	.driver		= {
		.name	= "jlt4013a",
		.of_match_table = jlt4013a_of_match,
	},
};
module_spi_driver(jlt4013a_driver);

MODULE_AUTHOR("Rui Oliveira <ruimail24@gmail.com>");
MODULE_AUTHOR("Oleg Belousov <www.strijar.ru>");
MODULE_DESCRIPTION("Driver for the Jinglitai JLT4013A LCD Panel");
MODULE_LICENSE("GPL v2");
