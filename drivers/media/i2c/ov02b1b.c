// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV02B1B 2MP camera sensor (the depth camera of the Xiaomi POCO X3 NFC "surya").
 *
 * There is no public datasheet and no public driver. The init table was decoded from the phone's
 * CamX sensor module (com.qti.sensormodule.j20c_ofilm_ov02b1b_depth.bin): the module lists the
 * register addresses in one array and the values in another, in the same order. The chip is a
 * sibling of the OV02A10 (same 0xfd page register, same 0x02/0x03 chip id registers, same
 * trigger register 0x01 on page 1), so its exposure and gain registers follow that family.
 * Registers and values are 8 bit; 0xfd selects the page.
 *
 * 1600x1200 RAW10, one lane, 66.56 MHz pixel clock, 30 fps (line 1792, frame 1236).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV02B1B_REG_CHIP_ID		CCI_REG16(0x02)	/* page 0 */
#define OV02B1B_CHIP_ID			0x002b
#define OV02B1B_REG_PAGE		CCI_REG8(0xfd)
#define OV02B1B_REG_TRIGGER		CCI_REG8(0x01)	/* page 1: latch the exposure and gain */
#define OV02B1B_REG_EXPOSURE		CCI_REG16(0x0e)	/* page 1, in lines */
#define OV02B1B_REG_GAIN		CCI_REG8(0x22)	/* page 1: 0x10 = 1x */

#define OV02B1B_WIDTH			1600
#define OV02B1B_HEIGHT			1200
#define OV02B1B_LINE_LENGTH		1792
#define OV02B1B_FRAME_LENGTH		1236
#define OV02B1B_PIXEL_RATE		66560000ULL
/* RAW10 on one lane, double data rate */
#define OV02B1B_LINK_FREQ		(OV02B1B_PIXEL_RATE * 10 / 2)
#define OV02B1B_MCLK			19200000
#define OV02B1B_EXPOSURE_MARGIN		4

/* V4L2 analogue gain in 1/64 steps, 64 = 1x; the register has 16 steps per 1x */
#define OV02B1B_GAIN_MIN		64
#define OV02B1B_GAIN_MAX		992

/*
 * Start and stop of the output. The CamX module's stream settings could not be decoded, so these
 * are what the sibling parts use (page 3, register 0xc2) and can be changed while bringing the
 * sensor up: /sys/module/ov02b1b/parameters.
 */
static unsigned int stream_page = 3;
module_param(stream_page, uint, 0644);
MODULE_PARM_DESC(stream_page, "page of the stream control register");
static unsigned int stream_reg = 0xc2;
module_param(stream_reg, uint, 0644);
MODULE_PARM_DESC(stream_reg, "stream control register");
static unsigned int stream_on = 0x01;
module_param(stream_on, uint, 0644);
MODULE_PARM_DESC(stream_on, "value that starts the output");
static unsigned int stream_off = 0x00;
module_param(stream_off, uint, 0644);
MODULE_PARM_DESC(stream_off, "value that stops the output");

static const s64 ov02b1b_link_freqs[] = { OV02B1B_LINK_FREQ };

static const char * const ov02b1b_supply_names[] = {
	"dovdd",	/* 1.8 V I/O */
	"avdd",		/* 2.8 V analog */
};

struct ov02b1b {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov02b1b_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
};

static inline struct ov02b1b *to_ov02b1b(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov02b1b, sd);
}

/* 1600x1200 init, 104 writes, from the surya CamX sensor module (ofilm ov02b1b depth, MCLK 19.2 MHz) */
static const struct reg_sequence ov02b1b_init_regs[] = {
	{ CCI_REG8(0xfc), 0x01 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x24), 0x03 },
	{ CCI_REG8(0x25), 0x0c },
	{ CCI_REG8(0x29), 0x02 },
	{ CCI_REG8(0x2a), 0x31 },
	{ CCI_REG8(0x2b), 0x05 },
	{ CCI_REG8(0x1e), 0x17 },
	{ CCI_REG8(0x33), 0x07 },
	{ CCI_REG8(0x35), 0x07 },
	{ CCI_REG8(0x4a), 0x0c },
	{ CCI_REG8(0x3a), 0x05 },
	{ CCI_REG8(0x3b), 0x02 },
	{ CCI_REG8(0x3e), 0x00 },
	{ CCI_REG8(0x46), 0x01 },
	{ CCI_REG8(0x6d), 0x03 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x0e), 0x02 },
	{ CCI_REG8(0x0f), 0x1a },
	{ CCI_REG8(0x14), 0x00 },
	{ CCI_REG8(0x15), 0x10 },
	{ CCI_REG8(0x18), 0x00 },
	{ CCI_REG8(0x22), 0xff },
	{ CCI_REG8(0x23), 0x02 },
	{ CCI_REG8(0x17), 0x2c },
	{ CCI_REG8(0x19), 0x20 },
	{ CCI_REG8(0x1b), 0x06 },
	{ CCI_REG8(0x1c), 0x04 },
	{ CCI_REG8(0x20), 0x03 },
	{ CCI_REG8(0x30), 0x01 },
	{ CCI_REG8(0x33), 0x01 },
	{ CCI_REG8(0x31), 0x0a },
	{ CCI_REG8(0x32), 0x09 },
	{ CCI_REG8(0x38), 0x01 },
	{ CCI_REG8(0x39), 0x01 },
	{ CCI_REG8(0x3a), 0x01 },
	{ CCI_REG8(0x3b), 0x01 },
	{ CCI_REG8(0x4f), 0x04 },
	{ CCI_REG8(0x4e), 0x05 },
	{ CCI_REG8(0x50), 0x01 },
	{ CCI_REG8(0x35), 0x0c },
	{ CCI_REG8(0x45), 0x2a },
	{ CCI_REG8(0x46), 0x2a },
	{ CCI_REG8(0x47), 0x2a },
	{ CCI_REG8(0x48), 0x2a },
	{ CCI_REG8(0x4a), 0x2c },
	{ CCI_REG8(0x4b), 0x2c },
	{ CCI_REG8(0x4c), 0x2c },
	{ CCI_REG8(0x4d), 0x2c },
	{ CCI_REG8(0x56), 0x3a },
	{ CCI_REG8(0x57), 0x0a },
	{ CCI_REG8(0x58), 0x24 },
	{ CCI_REG8(0x59), 0x20 },
	{ CCI_REG8(0x5a), 0x0a },
	{ CCI_REG8(0x5b), 0xff },
	{ CCI_REG8(0x37), 0x0a },
	{ CCI_REG8(0x42), 0x0e },
	{ CCI_REG8(0x68), 0x90 },
	{ CCI_REG8(0x69), 0xcd },
	{ CCI_REG8(0x6a), 0x8f },
	{ CCI_REG8(0x7c), 0x0a },
	{ CCI_REG8(0x7d), 0x0a },
	{ CCI_REG8(0x7e), 0x0a },
	{ CCI_REG8(0x7f), 0x08 },
	{ CCI_REG8(0x83), 0x14 },
	{ CCI_REG8(0x84), 0x14 },
	{ CCI_REG8(0x86), 0x14 },
	{ CCI_REG8(0x87), 0x07 },
	{ CCI_REG8(0x88), 0x0f },
	{ CCI_REG8(0x94), 0x02 },
	{ CCI_REG8(0x98), 0xd1 },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0xfd), 0x03 },
	{ CCI_REG8(0x97), 0x6c },
	{ CCI_REG8(0x98), 0x60 },
	{ CCI_REG8(0x99), 0x60 },
	{ CCI_REG8(0x9a), 0x6c },
	{ CCI_REG8(0xa1), 0x40 },
	{ CCI_REG8(0xaf), 0x04 },
	{ CCI_REG8(0xb1), 0x40 },
	{ CCI_REG8(0xae), 0x0d },
	{ CCI_REG8(0x88), 0x5b },
	{ CCI_REG8(0x89), 0x7c },
	{ CCI_REG8(0xb4), 0x05 },
	{ CCI_REG8(0x8c), 0x40 },
	{ CCI_REG8(0x8e), 0x40 },
	{ CCI_REG8(0x90), 0x40 },
	{ CCI_REG8(0x92), 0x40 },
	{ CCI_REG8(0x9b), 0x46 },
	{ CCI_REG8(0xac), 0x40 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x5a), 0x15 },
	{ CCI_REG8(0x74), 0x01 },
	{ CCI_REG8(0xfd), 0x00 },
	{ CCI_REG8(0x50), 0x40 },
	{ CCI_REG8(0x52), 0xb0 },
	{ CCI_REG8(0xfd), 0x01 },
	{ CCI_REG8(0x03), 0x70 },
	{ CCI_REG8(0x05), 0x10 },
	{ CCI_REG8(0x07), 0x20 },
	{ CCI_REG8(0x09), 0xb0 },
	{ CCI_REG8(0xfd), 0x03 },
	{ CCI_REG8(0xfb), 0x01 },
};

static int ov02b1b_write_page(struct ov02b1b *ov, u8 page, u32 reg, u64 val, int *err)
{
	cci_write(ov->regmap, OV02B1B_REG_PAGE, page, err);
	return cci_write(ov->regmap, reg, val, err);
}

static int ov02b1b_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov02b1b *ov = container_of(ctrl->handler, struct ov02b1b, ctrls);
	int ret = 0;

	/* registers are only written while powered; enable_streams applies the values */
	if (!pm_runtime_get_if_active(ov->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ov02b1b_write_page(ov, 0x01, OV02B1B_REG_EXPOSURE, ctrl->val, &ret);
		cci_write(ov->regmap, OV02B1B_REG_TRIGGER, 0x01, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ov02b1b_write_page(ov, 0x01, OV02B1B_REG_GAIN, ctrl->val >> 2, &ret);
		cci_write(ov->regmap, OV02B1B_REG_TRIGGER, 0x01, &ret);
		break;
	default:
		break;
	}

	pm_runtime_put(ov->dev);
	return ret;
}

static const struct v4l2_ctrl_ops ov02b1b_ctrl_ops = {
	.s_ctrl = ov02b1b_set_ctrl,
};

static int ov02b1b_init_controls(struct ov02b1b *ov)
{
	struct v4l2_ctrl_handler *hdl = &ov->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);

	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0, ov02b1b_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, OV02B1B_PIXEL_RATE, OV02B1B_PIXEL_RATE, 1,
			  OV02B1B_PIXEL_RATE);
	/* the timing is fixed: it comes from the init table */
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_VBLANK, OV02B1B_FRAME_LENGTH - OV02B1B_HEIGHT,
				 OV02B1B_FRAME_LENGTH - OV02B1B_HEIGHT, 1,
				 OV02B1B_FRAME_LENGTH - OV02B1B_HEIGHT);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK, OV02B1B_LINE_LENGTH - OV02B1B_WIDTH,
				 OV02B1B_LINE_LENGTH - OV02B1B_WIDTH, 1,
				 OV02B1B_LINE_LENGTH - OV02B1B_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ov->exposure = v4l2_ctrl_new_std(hdl, &ov02b1b_ctrl_ops, V4L2_CID_EXPOSURE, 1,
					 OV02B1B_FRAME_LENGTH - OV02B1B_EXPOSURE_MARGIN, 1, 538);
	v4l2_ctrl_new_std(hdl, &ov02b1b_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, OV02B1B_GAIN_MIN,
			  OV02B1B_GAIN_MAX, 1, OV02B1B_GAIN_MIN);

	ret = v4l2_fwnode_device_parse(ov->dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_new_fwnode_properties(hdl, &ov02b1b_ctrl_ops, &props);

	if (hdl->error)
		return hdl->error;

	ov->sd.ctrl_handler = hdl;
	return 0;
}

static void ov02b1b_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	memset(fmt, 0, sizeof(*fmt));
	fmt->width = OV02B1B_WIDTH;
	fmt->height = OV02B1B_HEIGHT;
	/* a monochrome sensor, but a Bayer code keeps it usable by the software ISP */
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int ov02b1b_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct ov02b1b *ov = to_ov02b1b(sd);
	int ret;

	ret = pm_runtime_resume_and_get(ov->dev);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(ov->regmap, ov02b1b_init_regs, ARRAY_SIZE(ov02b1b_init_regs));
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&ov->ctrls);
	if (!ret)
		ov02b1b_write_page(ov, stream_page, CCI_REG8(stream_reg), stream_on, &ret);
	if (!ret) {
		cci_write(ov->regmap, OV02B1B_REG_PAGE, 0x01, &ret);
		cci_write(ov->regmap, OV02B1B_REG_TRIGGER, 0x01, &ret);
	}
	if (ret) {
		dev_err(ov->dev, "failed to start streaming: %d\n", ret);
		pm_runtime_put(ov->dev);
	}
	return ret;
}

static int ov02b1b_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				   u32 pad, u64 streams_mask)
{
	struct ov02b1b *ov = to_ov02b1b(sd);
	int ret = 0;

	ov02b1b_write_page(ov, stream_page, CCI_REG8(stream_reg), stream_off, &ret);
	pm_runtime_put(ov->dev);
	return ret;
}

static int ov02b1b_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int ov02b1b_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = OV02B1B_WIDTH;
	fse->min_height = fse->max_height = OV02B1B_HEIGHT;
	return 0;
}

static int ov02b1b_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV02B1B_WIDTH;
		sel->r.height = OV02B1B_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_subdev_video_ops ov02b1b_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ov02b1b_pad_ops = {
	.enum_mbus_code = ov02b1b_enum_mbus_code,
	.enum_frame_size = ov02b1b_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = v4l2_subdev_get_fmt,	/* one fixed mode */
	.get_selection = ov02b1b_get_selection,
	.enable_streams = ov02b1b_enable_streams,
	.disable_streams = ov02b1b_disable_streams,
};

static const struct v4l2_subdev_ops ov02b1b_ops = {
	.video = &ov02b1b_video_ops,
	.pad = &ov02b1b_pad_ops,
};

static int ov02b1b_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	ov02b1b_fill_format(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_internal_ops ov02b1b_internal_ops = {
	.init_state = ov02b1b_init_state,
};

/*
 * Power-up, as found on the phone: I/O supply, analog supply, MCLK, then RESET released. The MCLK is
 * shared with the front camera. Holding RESET low makes the chip stop answering on I2C.
 */
static int ov02b1b_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov02b1b *ov = to_ov02b1b(sd);
	int ret;

	gpiod_set_value_cansleep(ov->reset_gpio, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(ov->supplies), ov->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ov->clk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(ov->supplies), ov->supplies);
		return ret;
	}
	fsleep(2000);

	gpiod_set_value_cansleep(ov->reset_gpio, 0);
	fsleep(10000);
	return 0;
}

static int ov02b1b_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov02b1b *ov = to_ov02b1b(sd);

	gpiod_set_value_cansleep(ov->reset_gpio, 1);
	fsleep(1000);
	clk_disable_unprepare(ov->clk);
	regulator_bulk_disable(ARRAY_SIZE(ov->supplies), ov->supplies);
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(ov02b1b_pm_ops, ov02b1b_power_off, ov02b1b_power_on, NULL);

static int ov02b1b_identify(struct ov02b1b *ov)
{
	u64 id = 0;
	int ret = 0;

	cci_write(ov->regmap, OV02B1B_REG_PAGE, 0x00, &ret);
	cci_read(ov->regmap, OV02B1B_REG_CHIP_ID, &id, &ret);
	if (ret)
		return dev_err_probe(ov->dev, ret, "failed to read the chip id\n");
	if (id != OV02B1B_CHIP_ID)
		return dev_err_probe(ov->dev, -ENODEV, "unexpected chip id 0x%04llx (want 0x%04x)\n",
				     id, OV02B1B_CHIP_ID);
	return 0;
}

static int ov02b1b_check_hwcfg(struct ov02b1b *ov)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long link_freq_bitmap;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(ov->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(ov->dev, -EPROBE_DEFER, "waiting for the endpoint\n");
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(ov->dev, ret, "parsing the endpoint failed\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 1)
		ret = dev_err_probe(ov->dev, -EINVAL, "%u data lanes, only 1 is supported\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
	if (!ret)
		ret = v4l2_link_freq_to_bitmap(ov->dev, bus_cfg.link_frequencies,
					       bus_cfg.nr_of_link_frequencies, ov02b1b_link_freqs,
					       ARRAY_SIZE(ov02b1b_link_freqs), &link_freq_bitmap);
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int ov02b1b_probe(struct i2c_client *client)
{
	struct ov02b1b *ov;
	unsigned int i;
	int ret;

	ov = devm_kzalloc(&client->dev, sizeof(*ov), GFP_KERNEL);
	if (!ov)
		return -ENOMEM;
	ov->dev = &client->dev;

	ret = ov02b1b_check_hwcfg(ov);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&ov->sd, client, &ov02b1b_ops);

	ov->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(ov->regmap))
		return PTR_ERR(ov->regmap);

	ov->clk = devm_v4l2_sensor_clk_get(ov->dev, NULL);
	if (IS_ERR(ov->clk))
		return dev_err_probe(ov->dev, PTR_ERR(ov->clk), "getting the clock\n");
	if (clk_get_rate(ov->clk) != OV02B1B_MCLK)
		dev_warn(ov->dev, "clock is %lu Hz, the tables expect %u\n",
			 clk_get_rate(ov->clk), OV02B1B_MCLK);

	ov->reset_gpio = devm_gpiod_get(ov->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov->reset_gpio))
		return dev_err_probe(ov->dev, PTR_ERR(ov->reset_gpio), "getting the reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(ov02b1b_supply_names); i++)
		ov->supplies[i].supply = ov02b1b_supply_names[i];
	ret = devm_regulator_bulk_get(ov->dev, ARRAY_SIZE(ov->supplies), ov->supplies);
	if (ret)
		return dev_err_probe(ov->dev, ret, "getting the supplies\n");

	ret = ov02b1b_power_on(ov->dev);
	if (ret)
		return ret;

	ret = ov02b1b_identify(ov);
	if (ret)
		goto err_power_off;

	ret = ov02b1b_init_controls(ov);
	if (ret)
		goto err_ctrls;

	ov->sd.internal_ops = &ov02b1b_internal_ops;
	ov->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov->sd.entity, 1, &ov->pad);
	if (ret)
		goto err_ctrls;

	ov->sd.state_lock = ov->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&ov->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(ov->dev);
	pm_runtime_enable(ov->dev);
	pm_runtime_set_autosuspend_delay(ov->dev, 1000);
	pm_runtime_use_autosuspend(ov->dev);

	ret = v4l2_async_register_subdev_sensor(&ov->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(ov->dev);
	dev_info(ov->dev, "OV02B1B found (chip id 0x%04x)\n", OV02B1B_CHIP_ID);
	return 0;

err_pm:
	pm_runtime_disable(ov->dev);
	pm_runtime_set_suspended(ov->dev);
	v4l2_subdev_cleanup(&ov->sd);
err_entity:
	media_entity_cleanup(&ov->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&ov->ctrls);
err_power_off:
	ov02b1b_power_off(ov->dev);
	return ret;
}

static void ov02b1b_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov02b1b *ov = to_ov02b1b(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov->ctrls);
	pm_runtime_disable(ov->dev);
	if (!pm_runtime_status_suspended(ov->dev)) {
		ov02b1b_power_off(ov->dev);
		pm_runtime_set_suspended(ov->dev);
	}
}

static const struct of_device_id ov02b1b_of_match[] = {
	{ .compatible = "ovti,ov02b1b" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov02b1b_of_match);

static struct i2c_driver ov02b1b_i2c_driver = {
	.driver = {
		.name = "ov02b1b",
		.of_match_table = ov02b1b_of_match,
		.pm = pm_ptr(&ov02b1b_pm_ops),
	},
	.probe = ov02b1b_probe,
	.remove = ov02b1b_remove,
};
module_i2c_driver(ov02b1b_i2c_driver);

MODULE_DESCRIPTION("OmniVision OV02B1B camera sensor driver");
MODULE_LICENSE("GPL");
