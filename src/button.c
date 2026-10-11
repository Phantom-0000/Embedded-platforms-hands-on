#include "button.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define BUTTON_LONG_PRESS_MS 1000
#define BUTTON_THREAD_STACK_SIZE 1024
#define BUTTON_THREAD_PRIORITY 5
#define RAW_EVENT_QUEUE_LENGTH 8
#define ACTION_QUEUE_LENGTH 8

struct raw_button_event {
	bool pressed;
	int64_t timestamp_ms;
};

static const struct gpio_dt_spec b2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback b2_callback;

K_MSGQ_DEFINE(raw_button_msgq,
	      sizeof(struct raw_button_event),
	      RAW_EVENT_QUEUE_LENGTH,
	      8);
K_MSGQ_DEFINE(button_action_msgq,
	      sizeof(struct button_action),
	      ACTION_QUEUE_LENGTH,
	      8);

K_THREAD_STACK_DEFINE(button_thread_stack, BUTTON_THREAD_STACK_SIZE);
static struct k_thread button_thread;
static atomic_t dropped_raw_events;

static void publish_action(enum button_action_type type, int64_t timestamp_ms)
{
	struct button_action action = {
		.type = type,
		.timestamp_ms = timestamp_ms,
	};

	if (k_msgq_put(&button_action_msgq, &action, K_NO_WAIT) != 0) {
		/* The main thread normally drains this queue immediately. */
		printk("WARNING: button action queue full\n");
	}
}

static void b2_isr(const struct device *port,
		   struct gpio_callback *callback,
		   uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	int state = gpio_pin_get_dt(&b2);
	if (state < 0) {
		return;
	}

	struct raw_button_event event = {
		.pressed = (state != 0),
		.timestamp_ms = k_uptime_get(),
	};

	/* ISR context must never wait for queue space. */
	if (k_msgq_put(&raw_button_msgq, &event, K_NO_WAIT) != 0) {
		atomic_inc(&dropped_raw_events);
	}
}

static void report_dropped_events(void)
{
	atomic_val_t dropped = atomic_set(&dropped_raw_events, 0);
	if (dropped > 0) {
		printk("WARNING: dropped %d raw button event(s)\n", (int)dropped);
	}
}

static void button_thread_entry(void *unused_a, void *unused_b, void *unused_c)
{
	ARG_UNUSED(unused_a);
	ARG_UNUSED(unused_b);
	ARG_UNUSED(unused_c);

	for (;;) {
		struct raw_button_event press;

		report_dropped_events();
		if (k_msgq_get(&raw_button_msgq, &press, K_FOREVER) != 0) {
			continue;
		}

		/* A release while idle is not an action. */
		if (!press.pressed) {
			continue;
		}

		const int64_t deadline = press.timestamp_ms + BUTTON_LONG_PRESS_MS;
		bool long_reported = false;

		for (;;) {
			struct raw_button_event next;
			int64_t remaining = deadline - k_uptime_get();

			if (remaining <= 0) {
				/* The threshold is reached without waiting for release. */
				publish_action(BUTTON_ACTION_LONG_PRESS, k_uptime_get());
				long_reported = true;
				break;
			}

			int ret = k_msgq_get(&raw_button_msgq, &next, K_MSEC(remaining));
			if (ret == -EAGAIN) {
				publish_action(BUTTON_ACTION_LONG_PRESS, k_uptime_get());
				long_reported = true;
				break;
			}
			if (ret != 0) {
				continue;
			}

			if (next.pressed) {
				/* Ignore duplicate press edges while the button is held. */
				continue;
			}

			/* Check the timestamp first so a release at the threshold is long. */
			if (next.timestamp_ms - press.timestamp_ms >= BUTTON_LONG_PRESS_MS) {
				publish_action(BUTTON_ACTION_LONG_PRESS, next.timestamp_ms);
				long_reported = true;
			} else {
				publish_action(BUTTON_ACTION_SHORT_PRESS, next.timestamp_ms);
			}
			break;
		}

		if (long_reported) {
			/* Consume the eventual release without generating a short press. */
			struct raw_button_event release;
			while (k_msgq_get(&raw_button_msgq, &release, K_FOREVER) == 0) {
				if (!release.pressed) {
					break;
				}
			}
		}
	}
}

int button_init(void)
{
	if (!device_is_ready(b2.port)) {
		printk("ERROR: B2 GPIO device is not ready\n");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&b2, GPIO_INPUT);
	if (ret != 0) {
		printk("ERROR: B2 GPIO configuration failed (%d)\n", ret);
		return ret;
	}

	gpio_init_callback(&b2_callback, b2_isr, BIT(b2.pin));
	ret = gpio_add_callback(b2.port, &b2_callback);
	if (ret != 0) {
		printk("ERROR: B2 callback registration failed (%d)\n", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&b2, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		printk("ERROR: B2 interrupt configuration failed (%d)\n", ret);
		return ret;
	}

	k_thread_create(&button_thread,
			button_thread_stack,
			K_THREAD_STACK_SIZEOF(button_thread_stack),
			button_thread_entry,
			NULL, NULL, NULL,
			BUTTON_THREAD_PRIORITY,
			0,
			K_NO_WAIT);

	return 0;
}

int button_wait_for_action(struct button_action *action, k_timeout_t timeout)
{
	if (action == NULL) {
		return -EINVAL;
	}

	return k_msgq_get(&button_action_msgq, action, timeout);
}
