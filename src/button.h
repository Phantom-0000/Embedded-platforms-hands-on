#ifndef BUTTON_H_
#define BUTTON_H_

#include <zephyr/kernel.h>

#include <stdint.h>

enum button_action_type {
	BUTTON_ACTION_SHORT_PRESS,
	BUTTON_ACTION_LONG_PRESS,
};

struct button_action {
	enum button_action_type type;
	int64_t timestamp_ms;
};

/**
 * Configure B2, install its interrupt callback, and start the button thread.
 *
 * @return 0 on success or a negative errno value on failure.
 */
int button_init(void);

/**
 * Get a high-level button action produced by the button thread.
 *
 * @return 0 when an action is returned, or a negative errno value.
 */
int button_wait_for_action(struct button_action *action, k_timeout_t timeout);

#endif /* BUTTON_H_ */
