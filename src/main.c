#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "button.h"

/* Take one ADC sample every two seconds in NORMAL mode. */
#define SAMPLE_PERIOD_MS 2000
/* Use 12 bits when the ADC does not give a resolution value. */
#define ADC_DEFAULT_RESOLUTION 12
/* Keep the LDR scale at the normal value. */
#define LDR_SENSITIVITY_PERCENT 100U

/* This divider's ADC voltage falls as the light level increases. */
#define LDR_BRIGHTNESS_INVERTED 1

enum app_mode {
	/* The three states used by the main control loop. */
	MODE_NORMAL,
	MODE_BLUE,
	MODE_OFF,
};

enum brightness_color {
	/* The colors selected from the LDR value. */
	BRIGHTNESS_RED,
	BRIGHTNESS_YELLOW,
	BRIGHTNESS_GREEN,
};

/* RGB pins and the LDR channel come from app.overlay. */
static const struct gpio_dt_spec rgb_red =
	GPIO_DT_SPEC_GET(DT_ALIAS(rgb_red), gpios);
static const struct gpio_dt_spec rgb_green =
	GPIO_DT_SPEC_GET(DT_ALIAS(rgb_green), gpios);
static const struct gpio_dt_spec rgb_blue =
	GPIO_DT_SPEC_GET(DT_ALIAS(rgb_blue), gpios);
static const struct adc_dt_spec ldr_adc =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* Get the color name used in the serial log. */
static const char *brightness_color_name(enum brightness_color color)
{
	switch (color) {
	case BRIGHTNESS_RED:
		return "RED";
	case BRIGHTNESS_YELLOW:
		return "YELLOW";
	case BRIGHTNESS_GREEN:
		return "GREEN";
	default:
		return "UNKNOWN";
	}
}

static int init_rgb(void)
{
	/* Check and configure all three RGB pins as outputs. */
	const struct gpio_dt_spec *channels[] = {
		&rgb_red,
		&rgb_green,
		&rgb_blue,
	};

	for (size_t i = 0; i < ARRAY_SIZE(channels); ++i) {
		if (!device_is_ready(channels[i]->port)) {
			printk("ERROR: RGB channel %u GPIO device is not ready\n",
			       (unsigned int)i);
			return -ENODEV;
		}

		int ret = gpio_pin_configure_dt(channels[i], GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			printk("ERROR: RGB channel %u configuration failed (%d)\n",
			       (unsigned int)i, ret);
			return ret;
		}
	}

	return 0;
}

static int set_rgb(bool red, bool green, bool blue)
{
	/* Set the three color channels. The device tree handles active-low logic. */
	int ret = gpio_pin_set_dt(&rgb_red, red);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&rgb_green, green);
	if (ret != 0) {
		return ret;
	}

	return gpio_pin_set_dt(&rgb_blue, blue);
}

static int set_rgb_color(enum brightness_color color)
{
	/* Map a brightness color to the RGB channels. */
	switch (color) {
	case BRIGHTNESS_RED:
		return set_rgb(true, false, false);
	case BRIGHTNESS_YELLOW:
		return set_rgb(true, true, false);
	case BRIGHTNESS_GREEN:
		return set_rgb(false, true, false);
	default:
		return -EINVAL;
	}
}

static int set_rgb_blue(void)
{
	/* Show the BLUE mode color. */
	return set_rgb(false, false, true);
}

static int set_rgb_off(void)
{
	/* Turn all RGB channels off. */
	return set_rgb(false, false, false);
}

static int init_adc(void)
{
	/* Check the ADC and apply the channel setup from app.overlay. */
	if (!adc_is_ready_dt(&ldr_adc)) {
		printk("ERROR: brightness ADC device is not ready\n");
		return -ENODEV;
	}

	int ret = adc_channel_setup_dt(&ldr_adc);
	if (ret != 0) {
		printk("ERROR: brightness ADC channel setup failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

static uint32_t adc_full_scale(void)
{
	/* Return the largest value for the current ADC resolution. */
	uint8_t resolution = ldr_adc.resolution;

	if (resolution == 0) {
		resolution = ADC_DEFAULT_RESOLUTION;
	}
	if (resolution >= 31) {
		return UINT32_MAX;
	}

	return BIT(resolution) - 1U;
}

static int read_brightness(uint16_t *raw_value, uint32_t *full_scale)
{
	/* Take one ADC reading and return its raw value and full scale. */
	int16_t sample;
	struct adc_sequence sequence = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	int ret = adc_sequence_init_dt(&ldr_adc, &sequence);
	if (ret != 0) {
		return ret;
	}

	ret = adc_read(ldr_adc.dev, &sequence);
	if (ret != 0) {
		return ret;
	}
	if (sample < 0) {
		return -EIO;
	}

	*raw_value = (uint16_t)sample;
	*full_scale = adc_full_scale();
	return 0;
}

static enum brightness_color color_for_sample(uint16_t raw_value,
					     uint32_t full_scale,
					     uint32_t *percentage_x100)
{
	/* Convert the ADC value to a brightness percent and choose a color. */
	uint32_t adjusted = MIN((uint32_t)raw_value, full_scale);

	if (LDR_BRIGHTNESS_INVERTED != 0) {
		adjusted = full_scale - adjusted;
	}

	/*
	 * Keep the required 33%/66% color thresholds, but scale the sensor
	 * response so an LDR module with a narrow voltage range can exercise all
	 * three colors. 100 is neutral; 200 doubles the effective brightness and
	 * clamps at the ADC full scale.
	 */
	adjusted = MIN((adjusted * LDR_SENSITIVITY_PERCENT + 50U) / 100U,
			       full_scale);

	/* Keep two decimal places in the log without using floating point. */
	*percentage_x100 = (adjusted * 10000U + (full_scale / 2U)) / full_scale;

	/* Compare rational percentages to preserve the exact 33/66 boundaries. */
	if (adjusted * 100U < full_scale * 33U) {
		return BRIGHTNESS_RED;
	}
	if (adjusted * 100U <= full_scale * 66U) {
		return BRIGHTNESS_YELLOW;
	}
	return BRIGHTNESS_GREEN;
}

static void sample_and_report(void)
{
	/* Read the LDR, update the LED, and print the result. */
	uint16_t raw_value;
	uint32_t full_scale;
	uint32_t percentage_x100;

	int ret = read_brightness(&raw_value, &full_scale);
	if (ret != 0) {
		printk("ERROR: ADC reading failed (%d)\n", ret);
		return;
	}
	if (full_scale == 0) {
		printk("ERROR: invalid ADC full-scale value\n");
		return;
	}

	enum brightness_color color =
		color_for_sample(raw_value, full_scale, &percentage_x100);
	ret = set_rgb_color(color);
	if (ret != 0) {
		printk("ERROR: RGB update failed (%d)\n", ret);
		return;
	}

	printk("ADC: raw=%u, full_scale=%u, brightness=%u.%02u%%, color=%s\n",
	       (unsigned int)raw_value,
	       (unsigned int)full_scale,
	       (unsigned int)(percentage_x100 / 100U),
	       (unsigned int)(percentage_x100 % 100U),
	       brightness_color_name(color));
}

static void enter_normal(enum app_mode *mode,
			 int64_t *next_sample_deadline,
			 bool adc_ready)
{
	/* Restore NORMAL mode and take an immediate sample when possible. */
	*mode = MODE_NORMAL;
	printk("Mode: NORMAL; ADC acquisition resumed\n");

	if (adc_ready) {
		/* Immediate sample on restoration, then continue every two seconds. */
		sample_and_report();
		*next_sample_deadline = k_uptime_get() + SAMPLE_PERIOD_MS;
	} else {
		(void)set_rgb_off();
		*next_sample_deadline = 0;
		printk("ADC unavailable; NORMAL sampling is disabled\n");
	}
}

static void process_button_action(const struct button_action *action,
				  enum app_mode *mode,
				  int64_t *next_sample_deadline,
				  bool adc_ready)
{
	/* Apply one short-press or long-press action to the state machine. */
	if (action->type == BUTTON_ACTION_LONG_PRESS) {
		if (*mode == MODE_NORMAL || *mode == MODE_BLUE) {
			*mode = MODE_OFF;
			*next_sample_deadline = 0;
			(void)set_rgb_off();
			printk("Mode: OFF\n");
			printk("System OFF\n");
		}
		return;
	}

	if (action->type != BUTTON_ACTION_SHORT_PRESS) {
		return;
	}

	switch (*mode) {
	case MODE_NORMAL:
		*mode = MODE_BLUE;
		*next_sample_deadline = 0;
		(void)set_rgb_blue();
		printk("Mode: BLUE; ADC acquisition suspended\n");
		break;
	case MODE_BLUE:
	case MODE_OFF:
		enter_normal(mode, next_sample_deadline, adc_ready);
		break;
	default:
		break;
	}
}

int main(void)
{
	/* Start the hardware, then run the main control loop. */
	int ret = init_rgb();
	if (ret != 0) {
		printk("RGB initialization failed (%d)\n", ret);
	}

	ret = button_init();
	if (ret != 0) {
		printk("Button initialization failed (%d)\n", ret);
	}

	bool adc_ready = (init_adc() == 0);
	enum app_mode mode = MODE_NORMAL;
	int64_t next_sample_deadline = 0;

	printk("Brightness monitor started\n");
	printk("Mode: NORMAL; ADC period: %d ms\n", SAMPLE_PERIOD_MS);

	if (adc_ready) {
		sample_and_report();
		next_sample_deadline = k_uptime_get() + SAMPLE_PERIOD_MS;
	} else {
		(void)set_rgb_off();
		printk("ADC initialization failed; sampling is disabled\n");
	}

	for (;;) {
		struct button_action action;
		int wait_result;

		/* NORMAL waits for the next sample or a button action. */
		if (mode == MODE_NORMAL && adc_ready) {
			int64_t remaining = next_sample_deadline - k_uptime_get();

			if (remaining <= 0) {
				sample_and_report();
				next_sample_deadline = k_uptime_get() + SAMPLE_PERIOD_MS;
				continue;
			}

			wait_result = button_wait_for_action(&action, K_MSEC(remaining));
			if (wait_result == -EAGAIN) {
				sample_and_report();
				next_sample_deadline = k_uptime_get() + SAMPLE_PERIOD_MS;
				continue;
			}
		} else {
			/* BLUE and OFF wait only for a button action. */
			wait_result = button_wait_for_action(&action, K_FOREVER);
		}

		if (wait_result == 0) {
			process_button_action(&action,
					      &mode,
					      &next_sample_deadline,
					      adc_ready);
		} else if (wait_result != -EAGAIN) {
			printk("ERROR: button action queue read failed (%d)\n",
			       wait_result);
		}
	}
}
