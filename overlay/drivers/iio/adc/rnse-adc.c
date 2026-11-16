// SPDX-License-Identifier: GPL-2.0
/*
 * The Audi RNS-E 192's A/D converter as an IIO device: seven inputs of
 * ten bits (docs/hardware.md, "A/D converter and the fold-down
 * display").  Input 3 is the display's position (at most 94 open, at
 * least 894 closed); what the others measure is not known (a unit at
 * rest read 459, 331, 933, 982, 424, 589 and 423).
 *
 * The converter belongs to arch/sh/boards/rnse-controls.c, which runs
 * the display's motor by it; this asks it for a conversion.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/platform_data/rnse.h>

#define RNSE_ADC_CHANNEL(n) {					\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = (n),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
}

static const struct iio_chan_spec rnse_adc_channels[] = {
	RNSE_ADC_CHANNEL(0), RNSE_ADC_CHANNEL(1), RNSE_ADC_CHANNEL(2),
	RNSE_ADC_CHANNEL(3), RNSE_ADC_CHANNEL(4), RNSE_ADC_CHANNEL(5),
	RNSE_ADC_CHANNEL(6),
};

static int rnse_adc_read_raw(struct iio_dev *iio, struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	int v;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	v = rnse_adc_read(chan->channel);
	if (v < 0)
		return v;
	*val = v;
	return IIO_VAL_INT;
}

static const struct iio_info rnse_adc_info = {
	.read_raw = rnse_adc_read_raw,
};

static int rnse_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *iio = devm_iio_device_alloc(&pdev->dev, 0);

	if (!iio)
		return -ENOMEM;
	iio->name = "rnse-adc";
	iio->info = &rnse_adc_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = rnse_adc_channels;
	iio->num_channels = ARRAY_SIZE(rnse_adc_channels);
	return devm_iio_device_register(&pdev->dev, iio);
}

static struct platform_driver rnse_adc_driver = {
	.driver = { .name = "rnse-adc" },
	.probe = rnse_adc_probe,
};
module_platform_driver(rnse_adc_driver);
MODULE_DESCRIPTION("Audi RNS-E 192 A/D converter");
MODULE_LICENSE("GPL");
