// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices AD7768-1 SPI ADC driver
 *
 * Copyright 2017 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

/* AD7768 registers definition */
#define AD7768_REG_CHIP_TYPE		0x3
#define AD7768_REG_PROD_ID_L		0x4
#define AD7768_REG_PROD_ID_H		0x5
#define AD7768_REG_CHIP_GRADE		0x6
#define AD7768_REG_SCRATCH_PAD		0x0A
#define AD7768_REG_VENDOR_L		0x0C
#define AD7768_REG_VENDOR_H		0x0D
#define AD7768_REG_INTERFACE_FORMAT	0x14
#define AD7768_REG_POWER_CLOCK		0x15
#define AD7768_REG_ANALOG		0x16
#define AD7768_REG_ANALOG2		0x17
#define AD7768_REG_CONVERSION		0x18
#define AD7768_REG_DIGITAL_FILTER	0x19
#define AD7768_REG_SINC3_DEC_RATE_MSB	0x1A
#define AD7768_REG_SINC3_DEC_RATE_LSB	0x1B
#define AD7768_REG_DUTY_CYCLE_RATIO	0x1C
#define AD7768_REG_SYNC_RESET		0x1D
#define AD7768_REG_GPIO_CONTROL		0x1E
#define AD7768_REG_GPIO_WRITE		0x1F
#define AD7768_REG_GPIO_READ		0x20
#define AD7768_REG_OFFSET_HI		0x21
#define AD7768_REG_OFFSET_MID		0x22
#define AD7768_REG_OFFSET_LO		0x23
#define AD7768_REG_GAIN_HI		0x24
#define AD7768_REG_GAIN_MID		0x25
#define AD7768_REG_GAIN_LO		0x26
#define AD7768_REG_SPI_DIAG_ENABLE	0x28
#define AD7768_REG_ADC_DIAG_ENABLE	0x29
#define AD7768_REG_DIG_DIAG_ENABLE	0x2A
#define AD7768_REG_ADC_DATA		0x2C
#define AD7768_REG_MASTER_STATUS	0x2D
#define AD7768_REG_SPI_DIAG_STATUS	0x2E
#define AD7768_REG_ADC_DIAG_STATUS	0x2F
#define AD7768_REG_DIG_DIAG_STATUS	0x30
#define AD7768_REG_MCLK_COUNTER		0x31

/* AD7768_REG_CONVERSION */
#define AD7768_CONV_MODE_MSK		GENMASK(2, 0)
#define AD7768_CONV_MODE(x)		FIELD_PREP(AD7768_CONV_MODE_MSK, x)

#define AD7768_RD_FLAG_MSK(x)		(BIT(6) | ((x) & 0x3F))
#define AD7768_WR_FLAG_MSK(x)		((x) & 0x3F)

enum ad7768_conv_mode {
	AD7768_CONTINUOUS,
	AD7768_ONE_SHOT,
	AD7768_SINGLE,
	AD7768_PERIODIC,
	AD7768_STANDBY
};

enum ad7768_pwrmode {
	AD7768_ECO_MODE = 0,
	AD7768_MED_MODE = 2,
	AD7768_FAST_MODE = 3
};

static const struct iio_chan_spec ad7768_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.indexed = 1,
		.channel = 0,
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 24,
			.storagebits = 32,
			.shift = 8,
			.endianness = IIO_BE,
		},
	},
};

struct ad7768_state {
	struct spi_device *spi;
	struct regulator *vref;
	struct mutex lock;
	struct completion completion;
	struct iio_trigger *trig;
	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	union {
		__be32 d32;
		u8 d8[2];
	} data ____cacheline_aligned;
};

static int ad7768_spi_reg_read(struct ad7768_state *st, unsigned int addr,
			       unsigned int len)
{
	unsigned int shift;
	int ret;

	shift = 32 - (8 * len);
	st->data.d8[0] = AD7768_RD_FLAG_MSK(addr);

	ret = spi_write_then_read(st->spi, st->data.d8, 1,
				  &st->data.d32, len);
	if (ret < 0)
		return ret;

	return (be32_to_cpu(st->data.d32) >> shift);
}

static int ad7768_spi_reg_write(struct ad7768_state *st,
				unsigned int addr,
				unsigned int val)
{
	st->data.d8[0] = AD7768_WR_FLAG_MSK(addr);
	st->data.d8[1] = val & 0xFF;

	return spi_write(st->spi, st->data.d8, 2);
}

static int ad7768_set_mode(struct ad7768_state *st,
			   enum ad7768_conv_mode mode)
{
	int regval;

	regval = ad7768_spi_reg_read(st, AD7768_REG_CONVERSION, 1);
	if (regval < 0)
		return regval;

	regval &= ~AD7768_CONV_MODE_MSK;
	regval |= AD7768_CONV_MODE(mode);

	return ad7768_spi_reg_write(st, AD7768_REG_CONVERSION, regval);
}

static int ad7768_scan_direct(struct iio_dev *indio_dev)
{
	struct ad7768_state *st = iio_priv(indio_dev);
	int readval, ret;

	reinit_completion(&st->completion);

	ret = ad7768_set_mode(st, AD7768_ONE_SHOT);
	if (ret < 0)
		return ret;

	ret = wait_for_completion_timeout(&st->completion,
					  msecs_to_jiffies(1000));
	if (!ret)
		return -ETIMEDOUT;

	readval = ad7768_spi_reg_read(st, AD7768_REG_ADC_DATA, 3);
	if (readval < 0)
		return readval;
	/*
	 * Any SPI configuration of the AD7768-1 can only be
	 * performed in continuous conversion mode.
	 */
	ret = ad7768_set_mode(st, AD7768_CONTINUOUS);
	if (ret < 0)
		return ret;

	return readval;
}

static int ad7768_reg_access(struct iio_dev *indio_dev,
			     unsigned int reg,
			     unsigned int writeval,
			     unsigned int *readval)
{
	struct ad7768_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);
	if (readval) {
		ret = ad7768_spi_reg_read(st, reg, 1);
		if (ret < 0)
			goto err_unlock;
		*readval = ret;
		ret = 0;
	} else {
		ret = ad7768_spi_reg_write(st, reg, writeval);
	}
err_unlock:
	mutex_unlock(&st->lock);

	return ret;
}

static int ad7768_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long info)
{
	struct ad7768_state *st = iio_priv(indio_dev);
	int scale_uv, ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		ret = ad7768_scan_direct(indio_dev);
		if (ret >= 0)
			*val = ret;

		iio_device_release_direct_mode(indio_dev);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		scale_uv = regulator_get_voltage(st->vref);
		if (scale_uv < 0)
			return scale_uv;

		*val = (scale_uv * 2) / 1000;
		*val2 = chan->scan_type.realbits;

		return IIO_VAL_FRACTIONAL_LOG2;
	}

	return -EINVAL;
}

static const struct iio_info ad7768_info = {
	.read_raw = &ad7768_read_raw,
	.debugfs_reg_access = &ad7768_reg_access,
};

static int ad7768_setup(struct ad7768_state *st)
{
	int ret;

	/*
	 * Two writes to the SPI_RESET[1:0] bits are required to initiate
	 * a software reset. The bits must first be set to 11, and then
	 * to 10. When the sequence is detected, the reset occurs.
	 * See the datasheet, page 70.
	 */
	ret = ad7768_spi_reg_write(st, AD7768_REG_SYNC_RESET, 0x3);
	if (ret)
		return ret;

	ret = ad7768_spi_reg_write(st, AD7768_REG_SYNC_RESET, 0x2);
	if (ret)
		return ret;

	/* Set power mode to fast */
	return ad7768_spi_reg_write(st, AD7768_REG_POWER_CLOCK,
				    AD7768_FAST_MODE);
}

static irqreturn_t ad7768_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad7768_state *st = iio_priv(indio_dev);
	int ret;

	mutex_lock(&st->lock);

	ret = spi_read(st->spi, &st->data.d32, 3);
	if (ret < 0)
		goto err_unlock;

	iio_push_to_buffers_with_timestamp(indio_dev, &st->data.d32,
					   iio_get_time_ns(indio_dev));

	iio_trigger_notify_done(indio_dev->trig);
err_unlock:
	mutex_unlock(&st->lock);

	return IRQ_HANDLED;
}

static irqreturn_t ad7768_interrupt(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ad7768_state *st = iio_priv(indio_dev);

	if (iio_buffer_enabled(indio_dev))
		iio_trigger_poll(st->trig);
	else
		complete(&st->completion);

	return IRQ_HANDLED;
};

static int ad7768_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ad7768_state *st = iio_priv(indio_dev);

	iio_triggered_buffer_postenable(indio_dev);
	/*
	 * Write a 1 to the LSB of the INTERFACE_FORMAT register to enter
	 * continuous read mode. Subsequent data reads do not require an
	 * initial 8-bit write to query the ADC_DATA register.
	 */
	return ad7768_spi_reg_write(st, AD7768_REG_INTERFACE_FORMAT, 0x01);
}

static int ad7768_buffer_predisable(struct iio_dev *indio_dev)
{
	struct ad7768_state *st = iio_priv(indio_dev);
	int ret;

	/*
	 * To exit continuous read mode, perform a single read of the ADC_DATA
	 * reg (0x2C), which allows further configuration of the device.
	 */
	ret = ad7768_spi_reg_read(st, AD7768_REG_ADC_DATA, 3);
	if (ret < 0)
		return ret;

	return iio_triggered_buffer_predisable(indio_dev);
}

static const struct iio_buffer_setup_ops ad7768_buffer_ops = {
	.postenable = &ad7768_buffer_postenable,
	.predisable = &ad7768_buffer_predisable,
};

static const struct iio_trigger_ops ad7768_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static void ad7768_regulator_disable(void *data)
{
	struct ad7768_state *st = data;

	regulator_disable(st->vref);
}

static int ad7768_probe(struct spi_device *spi)
{
	struct ad7768_state *st;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	st->vref = devm_regulator_get(&spi->dev, "vref");
	if (IS_ERR(st->vref))
		return PTR_ERR(st->vref);

	ret = regulator_enable(st->vref);
	if (ret) {
		dev_err(&spi->dev, "Failed to enable specified vref supply\n");
		return ret;
	}

	ret = devm_add_action_or_reset(&spi->dev, ad7768_regulator_disable, st);
	if (ret)
		return ret;

	spi_set_drvdata(spi, indio_dev);
	mutex_init(&st->lock);

	indio_dev->channels = ad7768_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad7768_channels);
	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->info = &ad7768_info;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;

	ret = ad7768_setup(st);
	if (ret < 0) {
		dev_err(&spi->dev, "AD7768 setup failed\n");
		return ret;
	}

	st->trig = devm_iio_trigger_alloc(&spi->dev, "%s-dev%d",
					  indio_dev->name, indio_dev->id);
	if (!st->trig)
		return -ENOMEM;

	st->trig->ops = &ad7768_trigger_ops;
	st->trig->dev.parent = &spi->dev;
	iio_trigger_set_drvdata(st->trig, indio_dev);
	ret = devm_iio_trigger_register(&spi->dev, st->trig);
	if (ret)
		return ret;

	indio_dev->trig = iio_trigger_get(st->trig);

	init_completion(&st->completion);

	ret = devm_request_irq(&spi->dev, spi->irq,
			       &ad7768_interrupt,
			       IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			       indio_dev->name, indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(&spi->dev, indio_dev,
					      &iio_pollfunc_store_time,
					      &ad7768_trigger_handler,
					      &ad7768_buffer_ops);
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct spi_device_id ad7768_id_table[] = {
	{ "ad7768-1", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, ad7768_id_table);

static const struct of_device_id ad7768_of_match[] = {
	{ .compatible = "adi,ad7768-1" },
	{ },
};
MODULE_DEVICE_TABLE(of, ad7768_of_match);

static struct spi_driver ad7768_driver = {
	.driver = {
		.name = "ad7768-1",
		.of_match_table = ad7768_of_match,
	},
	.probe = ad7768_probe,
	.id_table = ad7768_id_table,
};
module_spi_driver(ad7768_driver);

MODULE_AUTHOR("Stefan Popa <stefan.popa@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD7768-1 ADC driver");
MODULE_LICENSE("GPL v2");
