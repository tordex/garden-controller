#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "ssd1306.h"
#include "quadrature_encoder.pio.h"
#include "pins.h"
#include "app.h"

#define DEBOUNCE_MS 50

int main()
{
	stdio_init_all();

	app_init();

	gpio_init(PIN_BUTTON);
	gpio_set_dir(PIN_BUTTON, GPIO_IN);
	gpio_pull_up(PIN_BUTTON);

	sleep_ms(500);

	// Base pin to connect the A phase of the encoder.
	// The B phase must be connected to the next pin
	const uint PIN_AB = 4;
	PIO		   pio	  = pio0;
	const uint sm	  = 0;

	pio_add_program(pio, &quadrature_encoder_program);
	quadrature_encoder_program_init(pio, sm, PIN_ENCODER_A, 0);

	int32_t new_value				  = 0;
	int32_t delta					  = 0;
	int32_t old_value				  = 0;
	int32_t last_value				  = -1;
	int32_t last_delta				  = -1;

	int				button_state	  = 0;
	int				last_button_state = 0;
	absolute_time_t last_changed	  = 0;
	absolute_time_t now				  = 0;

	while(true)
	{
		// Process encoder
		new_value = quadrature_encoder_get_count(pio, sm) / 4;
		delta	  = new_value - old_value;
		old_value = new_value;
		if(new_value != last_value || delta != last_delta)
		{
			app_on_encoder_change(-delta);

			last_value = new_value;
			last_delta = delta;
		}

		// Process encoder button with debounce
		now			 = get_absolute_time();
		button_state = gpio_get(PIN_BUTTON);
		if(button_state != last_button_state)
		{
			last_button_state = button_state;
			last_changed	  = now;
		} else if(last_changed != 0 && absolute_time_diff_us(last_changed, now) > DEBOUNCE_MS * 1000)
		{
			last_changed = 0;
			if(last_button_state == 0)
			{
				app_on_click();
			}
		}

		// Call the app tick function periodically
		app_tick();

		sleep_ms(50);
	}
}
