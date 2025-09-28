#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "pico/bootrom.h"
#include <hardware/sync.h>
#include <hardware/flash.h>
#include "pins.h"
#include "ssd1306.h"
#include "app.h"

static void app_reload_profiles(bool with_ui);

#define PWM_WRAP		   25000 // 5kHz at 125MHz clock (125MHz / 25000 = 5kHz)

#define MAX_PROFILES	   5 // 3 predefined + 2 custom
#define MAX_PERIODS		   6 // up to 6 periods per profile

#define PUMP_RUN_MINUTES   5									  // run pump for 5 minutes when activated
#define PUMP_WAIT_MINUTES  30									  // wait for 30 minutes before next activation
#define PUMP_TOTAL_MINUTES (PUMP_RUN_MINUTES + PUMP_WAIT_MINUTES) // maximum minutes pump can run in a day

/**
 * @struct period_t
 * @brief Represents a time period configuration for LED control.
 *
 * This structure defines the parameters for a specific period, including its duration
 * and the power levels for white/red and blue LEDs.
 *
 * @var period_t::duration
 *   Duration of the period in minutes. Set to 0 to disable this period.
 * @var period_t::led_white_red_power
 *   Power level for white and red LEDs (range: 0-100).
 * @var period_t::led_blue_power
 *   Power level for blue LED (range: 0-100).
 */
typedef struct
{
	int duration;			 // minutes. 0 to disable period
	int led_white_red_power; // white and red leds power 0-100
	int led_blue_power;		 // blue led power 0-100
} period_t;

/**
 * @struct profile_t
 * @brief Represents a user profile containing a name and a set of periods.
 *
 * @var profile_t::name
 *   The profile name (null-terminated string, up to 15 characters plus null terminator).
 *
 * @var profile_t::periods
 *   Array of periods associated with the profile (maximum defined by MAX_PERIODS, typically up to 6).
 */
typedef struct
{
	char	 name[16];			   // profile name
	period_t periods[MAX_PERIODS]; // up to 6 periods per day
} profile_t;

/**
 * @struct app_state_t
 * @brief Represents the current state of the application, including LED power levels, pump state, and timing
 * information.
 *
 * @var app_state_t::period_index
 *   Current period index (0-5).
 * @var app_state_t::white_red
 *   Current power level for white and red LEDs (0-100).
 * @var app_state_t::blue
 *   Current power level for blue LED (0-100).
 * @var app_state_t::pump
 *   Current state of the pump (true = on, false = off).
 * @var app_state_t::pump_minutes_left
 *   Minutes left for the pump to run.
 * @var app_state_t::period_minutes_left
 *   Minutes left in the current period.
 */
typedef struct
{
	int	 period_index;		  // Current period index 0-5
	int	 white_red;			  // Current white and red leds power 0-100
	int	 blue;				  // Current blue led power 0-100
	bool pump;				  // Current pump state
	int	 pump_minutes_left;	  // Minutes left for the pump to run
	int	 period_minutes_left; // Minutes left in the current period
} app_state_t;

static profile_t profiles[MAX_PROFILES] = {
	{.name = "VEG",
	 .periods =
		 {
			 {.duration = 60 * 14, .led_white_red_power = 100, .led_blue_power = 100}, // 14 hours
			 {.duration = 60 * 10, .led_white_red_power = 0, .led_blue_power = 0},	   // 10 hours
			 {.duration = 0},														   // disabled
			 {.duration = 0},														   // disabled
			 {.duration = 0},														   // disabled
			 {.duration = 0}														   // disabled
		 }},
	{.name = "FLOWER",
	 .periods =
		 {
			 {.duration = 60 * 12, .led_white_red_power = 100, .led_blue_power = 0}, // 12 hours
			 {.duration = 60 * 12, .led_white_red_power = 0, .led_blue_power = 0},	 // 12 hours
			 {.duration = 0},														 // disabled
			 {.duration = 0},														 // disabled
			 {.duration = 0},														 // disabled
			 {.duration = 0}														 // disabled
		 }},
	{.name = "FRUIT",
	 .periods =
		 {
			 {.duration = 60 * 16, .led_white_red_power = 100, .led_blue_power = 0}, // 16 hours
			 {.duration = 60 * 8, .led_white_red_power = 0, .led_blue_power = 0},	 // 8 hours
			 {.duration = 0},														 // disabled
			 {.duration = 0},														 // disabled
			 {.duration = 0},														 // disabled
			 {.duration = 0}														 // disabled
		 }},
	{.name = "CUSTOM 1",
	 .periods =
		 {
			 {.duration = 0}, // disabled
			 {.duration = 0}, // disabled
			 {.duration = 0}, // disabled
			 {.duration = 0}, // disabled
			 {.duration = 0}, // disabled
			 {.duration = 0}  // disabled
		 }},
	{.name	  = "CUSTOM 2",
	 .periods = {
		 {.duration = 0}, // disabled
		 {.duration = 0}, // disabled
		 {.duration = 0}, // disabled
		 {.duration = 0}, // disabled
		 {.duration = 0}, // disabled
		 {.duration = 0}  // disabled
	 }	  }
};

/**
 * @enum app_mode_t
 * @brief Represents the different operational modes of the application.
 *
 * Enumerates all possible modes the application can be in, such as displaying state,
 * editing profiles, adjusting levels, and navigating menus.
 *
 * - MODE_SHOW_STATE:        Display the current state.
 * - MODE_SHOW_PROFILE:      Display the current profile.
 * - MODE_EDIT_PROFILE:      Edit the profile settings.
 * - MODE_EDIT_PERIOD:       Edit the period settings.
 * - MODE_EDIT_WR_LEVEL:     Edit the white/red level.
 * - MODE_EDIT_BL_LEVEL:     Edit the blue level.
 * - MODE_EDIT_DURATION:     Edit the duration settings.
 * - MODE_TOP_MENU:          Display the top menu.
 * - MODE_TIME_SHIFT:        Adjust the time shift.
 */
typedef enum
{
	MODE_SHOW_STATE,
	MODE_SHOW_PROFILE,
	MODE_EDIT_PROFILE,
	MODE_EDIT_PERIOD,
	MODE_EDIT_WR_LEVEL,
	MODE_EDIT_BL_LEVEL,
	MODE_EDIT_DURATION,
	MODE_TOP_MENU,
	MODE_TIME_SHIFT,
} app_mode_t;

/**
 * @enum top_menu_action_t
 * @brief Enumerates the possible actions in the top menu.
 *
 * This enumeration defines the available actions that can be performed
 * from the application's top menu. The values are ordered and can be
 * used for indexing or iteration.
 *
 * @var TOP_MENU_FIRST   The first menu action (used as a starting index).
 * @var TOP_MENU_SHIFT   Represents the "Shift" action in the top menu.
 * @var TOP_MENU_SAVE    Represents the "Save" action in the top menu.
 * @var TOP_MENU_RELOAD  Represents the "Reload" action in the top menu.
 * @var TOP_MENU_FLASH   Represents the "Flash" action in the top menu.
 * @var TOP_MENU_LAST    The last menu action (equal to TOP_MENU_FLASH).
 */
typedef enum
{
	TOP_MENU_FIRST = 0,
	TOP_MENU_SHIFT = TOP_MENU_FIRST,
	TOP_MENU_SAVE,
	TOP_MENU_RELOAD,
	TOP_MENU_FLASH,
	TOP_MENU_LAST = TOP_MENU_FLASH
} top_menu_action_t;

/**
 * @enum edit_mode_t
 * @brief Enumeration representing the different editing modes in the application.
 *
 * The edit_mode_t enum defines the various states or modes that can be used
 * for editing parameters. The values are:
 * - EDIT_FIRST:      The first edit mode (used as a base value).
 * - EDIT_BACK:       Edit mode for "back" action (same as EDIT_FIRST).
 * - EDIT_DURATION:   Edit mode for modifying the duration parameter.
 * - EDIT_WR_LEVEL:   Edit mode for modifying the "WR" (possibly "write" or "white") level.
 * - EDIT_BL_LEVEL:   Edit mode for modifying the "BL" (possibly "black" or "blue") level.
 * - EDIT_LAST:       The last edit mode (same as EDIT_BL_LEVEL).
 */
typedef enum
{
	EDIT_FIRST = 0,
	EDIT_BACK  = EDIT_FIRST,
	EDIT_DURATION,
	EDIT_WR_LEVEL,
	EDIT_BL_LEVEL,
	EDIT_LAST = EDIT_BL_LEVEL
} edit_mode_t;

static int			   current_profile = 0;			 // index of the current profile in use
static absolute_time_t app_start_time;				 // time when the app was started
static absolute_time_t app_start_time_without_shift; // time when the app was started without time shift
static absolute_time_t last_encoder_time = 0;		 // last time the encoder was moved
static ssd1306_t	   disp;
static app_state_t	   current_app_state	 = {.white_red = -1, .blue = -1}; // invalid state to force update on start

static app_mode_t  current_app_mode			 = MODE_SHOW_STATE;
static int		   menu_profile_index		 = 0;		  // index of the profile in the menu
static int		   current_edit_period_index = 0;		  // index of the period being edited
static edit_mode_t current_edit_value		 = EDIT_BACK; // value being edited

static top_menu_action_t current_top_menu_action = TOP_MENU_SAVE;

static int time_shift_hours						 = 0; // hours to shift the time, can be negative

/**
 * @brief Initializes the application hardware and state.
 *
 * This function sets up all peripherals and internal state required for the application to run.
 * It performs the following steps:
 *   - Resets the current profile and records the application start time.
 *   - Configures the LED pins for PWM output and initializes their PWM slices.
 *   - Sets initial LED levels to off.
 *   - Initializes the GPIO pin for the water pump and sets its direction.
 *   - Initializes the I2C bus and configures the pins for the OLED display.
 *   - Initializes and clears the OLED display.
 *   - Loads profiles from flash memory (without UI feedback).
 *   - Calls the main application tick function to update state.
 */
void app_init()
{
	current_profile				 = 0;
	app_start_time				 = get_absolute_time();
	app_start_time_without_shift = app_start_time;

	gpio_set_function(PIN_LED_BLUE, GPIO_FUNC_PWM);
	gpio_set_function(PIN_LED_WHITE_RED, GPIO_FUNC_PWM);

	// Find out which PWM slice is connected to PIN_LED_BLUE
	uint slice_num = pwm_gpio_to_slice_num(PIN_LED_BLUE);

	// Set period of 4 cycles (0 to 3 inclusive)
	pwm_set_wrap(slice_num, PWM_WRAP);
	// Set the PWM running
	pwm_set_enabled(slice_num, true);

	pwm_set_gpio_level(PIN_LED_BLUE, 0);
	pwm_set_gpio_level(PIN_LED_WHITE_RED, 0);

	// Init pin for water pump
	gpio_init(PIN_PUMP);
	gpio_set_dir(PIN_PUMP, GPIO_OUT);

	// Init I2C for OLED
	i2c_init(i2c1, 400000);
	gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C);
	gpio_set_function(PIN_OLED_SDL, GPIO_FUNC_I2C);
	gpio_pull_up(PIN_OLED_SDA);
	gpio_pull_up(PIN_OLED_SDL);

	// Init OLED display
	disp.external_vcc = false;
	ssd1306_init(&disp, 128, 64, 0x3C, i2c1);
	ssd1306_clear(&disp);

	app_reload_profiles(false); // load profiles from flash, no UI

	app_tick();
}

/**
 * @brief Calculates and updates the application state based on the current time and profile periods.
 *
 * This function determines the current state of the application, including LED power levels and pump status,
 * by comparing the elapsed time since the application started to the configured profile periods and pump cycle.
 * It updates the global `current_app_state` structure accordingly.
 *
 * The function performs the following:
 * - Computes the number of minutes since the application started.
 * - Calculates the total duration of all active periods in the current profile.
 * - Handles pump operation based on a cyclic schedule (run/off periods).
 * - Determines the current active period and updates LED power levels and remaining time.
 * - Turns off LEDs if no active period is found.
 *
 * @return true if the application state has changed and requires action (e.g., updating hardware), false otherwise.
 */
static bool app_calculate_state()
{
	absolute_time_t now					= get_absolute_time();
	uint64_t		us_since_start		= to_us_since_boot(now) - to_us_since_boot(app_start_time);
	uint32_t		minutes_since_start = us_since_start / 1000000 / 60;
	bool			ret					= false;

	profile_t* profile					= &profiles[current_profile];
	uint32_t   profile_total_minutes	= 0;

	for(int i = 0; i < MAX_PERIODS; i++)
	{
		profile_total_minutes += profile->periods[i].duration;
	}

	if(profile_total_minutes == 0)
	{
		if(current_app_state.white_red != 0 || current_app_state.blue != 0)
		{
			current_app_state.white_red = 0;
			current_app_state.blue		= 0;
			return true; // state changed
		}
		return false; // no active periods, nothing to do
	}

	{
		uint64_t us_since_start		 = to_us_since_boot(now) - to_us_since_boot(app_start_time_without_shift);
		uint32_t minutes_since_start = us_since_start / 1000000 / 60;
		uint32_t pump_cycle_position = minutes_since_start % PUMP_TOTAL_MINUTES;
		if(pump_cycle_position < PUMP_RUN_MINUTES)
		{
			// within pump run time
			if(!current_app_state.pump)
			{
				current_app_state.pump				= true;
				current_app_state.pump_minutes_left = PUMP_RUN_MINUTES - pump_cycle_position;
				ret									= true; // state changed
			} else
			{
				ret = (current_app_state.pump_minutes_left != PUMP_RUN_MINUTES - pump_cycle_position);
				// pump already running, just update minutes left
				current_app_state.pump_minutes_left = PUMP_RUN_MINUTES - pump_cycle_position;
			}
		} else
		{
			// outside pump run time
			if(current_app_state.pump)
			{
				current_app_state.pump				= false;
				current_app_state.pump_minutes_left = PUMP_TOTAL_MINUTES - pump_cycle_position;
				ret									= true; // state changed
			} else
			{
				ret = (current_app_state.pump_minutes_left != PUMP_TOTAL_MINUTES - pump_cycle_position);
				// pump already off, nothing to do
				current_app_state.pump_minutes_left = PUMP_TOTAL_MINUTES - pump_cycle_position;
			}
		}
	}

	if(minutes_since_start >= profile_total_minutes)
	{
		minutes_since_start = minutes_since_start % profile_total_minutes;
	}

	uint32_t elapsed_minutes = 0;
	for(int i = 0; i < MAX_PERIODS; i++)
	{
		period_t* period = &profile->periods[i];
		if(period->duration == 0)
		{
			continue; // disabled period
		}
		if(minutes_since_start < elapsed_minutes + period->duration)
		{
			// we are in this period
			if(current_app_state.white_red != period->led_white_red_power ||
			   current_app_state.blue != period->led_blue_power ||
			   current_app_state.period_minutes_left != elapsed_minutes + period->duration - minutes_since_start ||
			   current_app_state.period_index != i)
			{
				current_app_state.white_red			  = period->led_white_red_power;
				current_app_state.blue				  = period->led_blue_power;
				current_app_state.period_minutes_left = elapsed_minutes + period->duration - minutes_since_start;
				current_app_state.period_index		  = i;
				return true; // state changed
			}
			return ret; // state not changed
		}
		elapsed_minutes += period->duration;
	}

	// no active period, turn off leds
	if(current_app_state.white_red != 0 || current_app_state.blue != 0 || current_app_state.period_minutes_left != 0)
	{
		current_app_state.white_red			  = 0;
		current_app_state.blue				  = 0;
		current_app_state.period_minutes_left = 0;
		return true; // state changed
	}
	return ret; // state not changed
}

/**
 * @brief Applies the current application state to the hardware.
 *
 * This function updates the hardware outputs based on the values stored in
 * the global `current_app_state` structure. It controls the pump and two LEDs:
 * - Turns the pump on or off depending on `current_app_state.pump`.
 * - Sets the PWM level for the white/red LED based on `current_app_state.white_red` (as a percentage).
 * - Sets the PWM level for the blue LED based on `current_app_state.blue` (as a percentage).
 *
 * Assumes that the GPIO and PWM peripherals have been initialized.
 */
static void app_apply_state()
{
	// Apply the current state to the pump
	if(current_app_state.pump)
	{
		gpio_put(PIN_PUMP, 1); // on
	} else
	{
		gpio_put(PIN_PUMP, 0); // off
	}

	pwm_set_gpio_level(PIN_LED_WHITE_RED, current_app_state.white_red * PWM_WRAP / 100);
	pwm_set_gpio_level(PIN_LED_BLUE, current_app_state.blue * PWM_WRAP / 100);
}

/**
 * @brief Draws the current application state on the SSD1306 display.
 *
 * This function clears the display and renders the following information:
 * - The current profile name.
 * - A horizontal separator line.
 * - The current period index and the remaining time in hours and minutes.
 * - The current white/red and blue percentage values.
 * - The pump state (ON/OFF) and the remaining pump minutes.
 *
 * The function uses the global variables `profiles`, `current_profile`, and `current_app_state`
 * to retrieve the necessary data for display.
 */
static void app_draw_current_state()
{
	char buffer[32];
	int	 y = 0;

	ssd1306_clear(&disp);

	// Draw profile name
	ssd1306_draw_string(&disp, 0, y, 2, profiles[current_profile].name);
	y += 16;
	ssd1306_draw_line(&disp, 0, y, 128, y);
	ssd1306_draw_line(&disp, 0, y + 1, 128, y + 1);
	y			+= 4;

	// Draw current period and time left
	int hours	 = current_app_state.period_minutes_left / 60;
	int minutes	 = current_app_state.period_minutes_left % 60;
	snprintf(buffer, sizeof(buffer), "#%d %d:%02d", current_app_state.period_index + 1, hours, minutes);
	ssd1306_draw_string(&disp, 0, y, 2, buffer);
	y += 18;

	snprintf(buffer, sizeof(buffer), "W/R:%d%% B:%d%%", current_app_state.white_red, current_app_state.blue);
	ssd1306_draw_string(&disp, 0, y, 1, buffer);
	y += 10;

	// Draw pump state
	if(current_app_state.pump)
	{
		snprintf(buffer, sizeof(buffer), "P:ON %dm", current_app_state.pump_minutes_left);
	} else
	{
		snprintf(buffer, sizeof(buffer), "P:OFF %dm", current_app_state.pump_minutes_left);
	}
	ssd1306_draw_string(&disp, 0, y, 2, buffer);

	ssd1306_show(&disp);
}

/**
 * @brief Draws the current profile information on the SSD1306 display.
 *
 * This function clears the display and renders the selected profile's name and its periods.
 * For each period, it displays the period number, duration in hours, white/red LED power, and blue LED power.
 * The information is formatted and drawn line by line on the display.
 *
 * Assumes that 'profiles', 'menu_profile_index', 'MAX_PERIODS', 'profile_t', 'period_t', and 'disp'
 * are defined elsewhere in the codebase.
 */
static void app_draw_profile()
{
	char buffer[32];
	int	 y = 0;
	int	 x = 0;

	ssd1306_clear(&disp);

	profile_t* profile = &profiles[menu_profile_index];

	// Draw profile name
	ssd1306_draw_string(&disp, x, y, 2, profile->name);
	y += 16;

	for(int i = 0; i < MAX_PERIODS; i++)
	{
		period_t* period = &profile->periods[i];
		int		  hours	 = period->duration / 60;
		snprintf(buffer, sizeof(buffer), "%d-T:%2d|W:%3d|B:%3d", i + 1, hours, period->led_white_red_power,
				 period->led_blue_power);
		ssd1306_draw_string(&disp, x, y, 1, buffer);
		y += 8;
	}

	ssd1306_show(&disp);
}

/**
 * @brief Draws the "Edit Profile" screen on the SSD1306 display.
 *
 * This function clears the display and renders the UI for editing a profile's periods.
 * It highlights the currently selected period, displays a "BACK" option, and lists up to
 * MAX_PERIODS periods starting from a calculated top index. For each period, it shows:
 *   - The period index and duration in hours.
 *   - The white/red LED power percentage.
 *   - The blue LED power percentage.
 * The currently selected period is indicated with a '>' marker.
 *
 * Globals used:
 *   - disp: The SSD1306 display context.
 *   - current_edit_period_index: Index of the currently selected period.
 *   - profiles: Array of profile structures containing periods.
 *   - menu_profile_index: Index of the currently selected profile.
 *
 * Functions used:
 *   - ssd1306_clear: Clears the display.
 *   - ssd1306_draw_string: Draws a string on the display at a given position and size.
 *   - ssd1306_show: Updates the display with the drawn content.
 */
static void app_draw_edit_profile()
{
	char buffer[32];
	int	 y = 0;
	int	 x = 0;

	ssd1306_clear(&disp);

	if(current_edit_period_index < 0)
	{
		ssd1306_draw_string(&disp, x, y, 2, ">"); // indicate selected period
	}
	ssd1306_draw_string(&disp, x + 11, y, 2, "BACK");
	y			  += 16;

	int top_index  = current_edit_period_index - 2;
	if(top_index < 0)
	{
		top_index = 0;
	}

	for(int i = top_index; i < MAX_PERIODS; i++)
	{
		period_t* period = &profiles[menu_profile_index].periods[i];
		int		  hours	 = period->duration / 60;
		snprintf(buffer, sizeof(buffer), "%d-T:%2d", i + 1, hours);
		if(i == current_edit_period_index)
		{
			ssd1306_draw_string(&disp, x, y, 2, ">"); // indicate selected period
		}
		ssd1306_draw_string(&disp, x + 10, y, 2, buffer);

		snprintf(buffer, sizeof(buffer), "W:%3d%%", period->led_white_red_power);
		ssd1306_draw_string(&disp, x + 85, y, 1, buffer);
		snprintf(buffer, sizeof(buffer), "B:%3d%%", period->led_blue_power);
		ssd1306_draw_string(&disp, x + 85, y + 8, 1, buffer);

		y += 16;
	}

	ssd1306_show(&disp);
}

/**
 * @brief Draws the "Edit Period" screen on the SSD1306 display.
 *
 * This function displays the editable parameters of a period (duration, white/red LED power, blue LED power)
 * for the currently selected profile and period. It visually indicates which parameter is currently selected
 * or being edited using special symbols ('>' for selected, '=' for editing). The "BACK" option is also shown
 * at the top to allow returning to the previous menu.
 *
 * The function uses the following global variables:
 * - profiles: Array of profile_t structures containing period data.
 * - current_profile: Index of the currently selected profile.
 * - current_edit_period_index: Index of the period being edited.
 * - current_edit_value: Indicates which field is currently selected (e.g., BACK, DURATION, WR_LEVEL, BL_LEVEL).
 * - current_app_mode: Indicates the current editing mode (e.g., MODE_EDIT_DURATION, MODE_EDIT_WR_LEVEL, etc.).
 *
 * The function updates the display using the SSD1306 driver functions.
 */
static void app_draw_edit_period()
{
	char buffer[32];
	int	 y = 0;
	int	 x = 11;

	ssd1306_clear(&disp);

	profile_t* profile = &profiles[current_profile];
	period_t*  period  = &profile->periods[current_edit_period_index];
	int		   hours   = period->duration / 60;

	if(current_edit_value == EDIT_BACK)
	{
		ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected value
	}
	ssd1306_draw_string(&disp, x, y, 2, "BACK");
	y += 16;

	if(current_edit_value == EDIT_DURATION)
	{
		if(current_app_mode == MODE_EDIT_DURATION)
		{
			ssd1306_draw_string(&disp, 0, y, 2, "="); // indicate selected value
		} else
		{
			ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected value
		}
	}
	snprintf(buffer, sizeof(buffer), "TIME:%d", hours);
	ssd1306_draw_string(&disp, x, y, 2, buffer);
	y += 16;

	if(current_edit_value == EDIT_WR_LEVEL)
	{
		if(current_app_mode == MODE_EDIT_WR_LEVEL)
		{
			ssd1306_draw_string(&disp, 0, y, 2, "="); // indicate selected value
		} else
		{
			ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected value
		}
	}
	snprintf(buffer, sizeof(buffer), "WRED:%3d%%", period->led_white_red_power);
	ssd1306_draw_string(&disp, x, y, 2, buffer);
	y += 16;

	if(current_edit_value == EDIT_BL_LEVEL)
	{
		if(current_app_mode == MODE_EDIT_BL_LEVEL)
		{
			ssd1306_draw_string(&disp, 0, y, 2, "="); // indicate selected value
		} else
		{
			ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected value
		}
	}
	snprintf(buffer, sizeof(buffer), "BLUE:%3d%%", period->led_blue_power);
	ssd1306_draw_string(&disp, x, y, 2, buffer);

	ssd1306_show(&disp);
}

/**
 * @brief Draws the current time shift value on the SSD1306 display.
 *
 * This function clears the display, prints the label "SHIFT HOURS:",
 * then displays the current value of the time shift (in hours) with sign.
 * Finally, it updates the display to show the changes.
 *
 * Globals:
 *   - time_shift_hours: The current time shift value to display.
 *   - disp: The SSD1306 display context.
 */
static void app_draw_time_shift()
{
	char buffer[32];
	int	 y = 0;

	ssd1306_clear(&disp);

	snprintf(buffer, sizeof(buffer), "SHIFT HOURS:");
	ssd1306_draw_string(&disp, 0, y, 2, buffer);
	y += 16;

	snprintf(buffer, sizeof(buffer), "%+d", time_shift_hours);
	ssd1306_draw_string(&disp, 40, y, 4, buffer);
	y += 32;

	ssd1306_show(&disp);
}

/**
 * @brief Draws the top menu on the SSD1306 display.
 *
 * This function clears the display and draws the top menu options:
 * "TIME SHIFT", "SAVE", "RELOAD", and "FLASH". It highlights the currently
 * selected menu action by drawing a '>' indicator next to it.
 *
 * The selection is determined by the value of the global variable
 * `current_top_menu_action`, which should be set to one of the following:
 * - TOP_MENU_SHIFT
 * - TOP_MENU_SAVE
 * - TOP_MENU_RELOAD
 * - TOP_MENU_FLASH
 *
 * The function uses the SSD1306 display driver functions to render the menu.
 */
static void app_draw_top_menu()
{
	int y = 0;
	int x = 11;

	ssd1306_clear(&disp);

	if(current_top_menu_action == TOP_MENU_SHIFT)
	{
		ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected action
	}
	ssd1306_draw_string(&disp, x, y, 2, "TIME SHIFT");
	y += 16;

	if(current_top_menu_action == TOP_MENU_SAVE)
	{
		ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected action
	}
	ssd1306_draw_string(&disp, x, y, 2, "SAVE");
	y += 16;

	if(current_top_menu_action == TOP_MENU_RELOAD)
	{
		ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected action
	}
	ssd1306_draw_string(&disp, x, y, 2, "RELOAD");
	y += 16;

	if(current_top_menu_action == TOP_MENU_FLASH)
	{
		ssd1306_draw_string(&disp, 0, y, 2, ">"); // indicate selected action
	}
	ssd1306_draw_string(&disp, x, y, 2, "FLASH");

	ssd1306_show(&disp);
}

/**
 * @brief Redraws the application UI based on the current application mode.
 *
 * This function checks the value of the global variable `current_app_mode`
 * and calls the appropriate drawing function to update the display.
 * Each case in the switch statement corresponds to a different application mode,
 * such as showing the current state, profile, editing profile or period, top menu,
 * or time shift. If the mode does not match any known value, the function does nothing.
 */
static void app_redraw()
{
	switch(current_app_mode)
	{
	case MODE_SHOW_STATE:
		app_draw_current_state();
		break;
	case MODE_SHOW_PROFILE:
		app_draw_profile();
		break;
	case MODE_EDIT_PROFILE:
		app_draw_edit_profile();
		break;
	case MODE_EDIT_PERIOD:
	case MODE_EDIT_BL_LEVEL:
	case MODE_EDIT_WR_LEVEL:
	case MODE_EDIT_DURATION:
		app_draw_edit_period();
		break;
	case MODE_TOP_MENU:
		app_draw_top_menu();
		break;
	case MODE_TIME_SHIFT:
		app_draw_time_shift();
		break;
	default:
		break;
	}
}

/**
 * @brief Reboots the device into bootloader mode for firmware flashing.
 *
 * This function clears the display, shows a message indicating that the device is preparing
 * to flash new firmware, and then sets a specific magic value at a predefined memory address
 * to signal the bootloader. It then calls `reset_usb_boot()` to initiate the reboot process.
 * The function enters an infinite loop to prevent further code execution after the reboot is triggered.
 *
 * Note:
 * - The magic value and memory address are specific to the device's bootloader implementation.
 * - This function is typically used when a firmware update is required via USB.
 */
static void app_reboot_to_bootloader()
{
	ssd1306_clear(&disp);

	ssd1306_draw_string(&disp, 0, 24, 2, "TO FLASH...");

	ssd1306_show(&disp);
	// Reboot to bootloader for flashing new firmware
	const uint32_t BOOTLOADER_MAGIC = 0xF01669EF;
	uint32_t*	   bootloader_magic = (uint32_t*) 0x20041FF0;
	*bootloader_magic				= BOOTLOADER_MAGIC;
	reset_usb_boot(0, 0);
	while(1)
		;
}

/**
 * @brief Buffer used to temporarily store data for a flash memory sector.
 *
 * This buffer has a size equal to FLASH_SECTOR_SIZE and is typically used
 * for read, write, or erase operations involving flash memory sectors.
 */
uint8_t flash_buffer[FLASH_SECTOR_SIZE];

/**
 * @brief Saves the current profile and all profiles to flash memory.
 *
 * This function prepares a buffer with magic bytes and profile data,
 * disables interrupts, erases the target flash sector, and writes the buffer
 * to flash memory. After saving, it displays a "SAVED..." message on the
 * SSD1306 display for 2 seconds, updates the menu profile index and app mode,
 * and redraws the application UI.
 *
 * The function ensures data integrity by using magic bytes and disabling
 * interrupts during the flash operation.
 */
static void app_save_profiles()
{
	uint32_t FLASH_TARGET_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
	flash_buffer[0]				 = 0xA5; // magic byte to indicate valid data
	flash_buffer[1]				 = 0x5A; // magic byte to indicate valid data
	flash_buffer[2]				 = 0xA5; // magic byte to indicate valid data
	flash_buffer[3]				 = 0x5A; // magic byte to indicate valid data
	memcpy(flash_buffer + 4, &current_profile, sizeof(current_profile));
	memcpy(flash_buffer + 4 + sizeof(current_profile), &profiles, sizeof(profiles));

	uint32_t ints = save_and_disable_interrupts();
	flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(FLASH_TARGET_OFFSET, flash_buffer, sizeof(flash_buffer));
	restore_interrupts(ints);

	ssd1306_clear(&disp);
	ssd1306_draw_string(&disp, 0, 24, 2, "SAVED...");
	ssd1306_show(&disp);
	sleep_ms(2000);

	menu_profile_index = current_profile;
	current_app_mode   = MODE_SHOW_STATE;

	app_redraw();
}

/**
 * @brief Reloads user profiles from flash memory and updates the application state.
 *
 * This function reads profile data from a specific location in flash memory.
 * It first checks for a valid data signature (0xA5, 0x5A, 0xA5, 0x5A) at the beginning
 * of the data block. If the signature is invalid, it optionally updates the UI to indicate
 * that no data is available and returns early.
 *
 * If valid data is found, it loads the current profile index and the profiles array from flash.
 * If the loaded profile index is out of bounds, it resets it to 0.
 * The function also updates the menu profile index and sets the application mode to show the state.
 * Optionally, it updates the UI to indicate that data has been loaded.
 *
 * @param with_ui If true, updates the UI to reflect the loading status.
 */
static void app_reload_profiles(bool with_ui)
{
	uint8_t* address = (uint8_t*) (XIP_BASE + PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE);
	if(address[0] != 0xA5 || address[1] != 0x5A || address[2] != 0xA5 || address[3] != 0x5A)
	{
		// no valid data
		if(with_ui)
		{
			ssd1306_clear(&disp);
			ssd1306_draw_string(&disp, 0, 24, 2, "NO DATA");
			ssd1306_show(&disp);
			sleep_ms(2000);
			app_redraw();
		}
		return;
	}
	memcpy(&current_profile, address + 4, sizeof(current_profile));
	memcpy(&profiles, address + 4 + sizeof(current_profile), sizeof(profiles));
	if(current_profile < 0 || current_profile >= MAX_PROFILES)
	{
		current_profile = 0; // invalid profile index, reset to 0
	}
	menu_profile_index = current_profile;
	current_app_mode   = MODE_SHOW_STATE;
	if(with_ui)
	{
		ssd1306_clear(&disp);
		ssd1306_draw_string(&disp, 0, 24, 2, "DATA LOADED");
		ssd1306_show(&disp);
		sleep_ms(2000);
		app_redraw();
	}
}

/**
 * @brief Periodic application tick handler.
 *
 * This function is called periodically to handle application state updates.
 * - Checks if 10 seconds have passed since the last encoder event (`last_encoder_time`).
 *   - If so, and the current mode is not `MODE_SHOW_STATE`, switches to `MODE_SHOW_STATE`,
 *     sets the menu profile index to the current profile, and triggers a redraw.
 * - If the current mode is `MODE_SHOW_STATE`:
 *   - Calls `app_calculate_state()`. If it returns true, applies the new state and redraws the UI.
 */
void app_tick()
{
	absolute_time_t now = get_absolute_time();
	if(now - last_encoder_time > 10000 * 6000) // 10 seconds
	{
		if(current_app_mode != MODE_SHOW_STATE)
		{
			current_app_mode   = MODE_SHOW_STATE;
			menu_profile_index = current_profile;
			app_redraw();
		}
	}
	if(current_app_mode == MODE_SHOW_STATE)
	{
		if(app_calculate_state())
		{
			app_apply_state();
			app_redraw();
		}
	}
}

/**
 * @brief Handles encoder input to change and display the current profile.
 *
 * This function updates the currently selected profile based on the encoder's delta value.
 * - If the new profile index is less than 0, it returns to the top menu and redraws the UI.
 * - If the new profile index exceeds the maximum allowed profiles, it wraps around to the first profile.
 * - Updates the menu profile index if it has changed.
 * - Sets the application mode to either show the profile or show the current state, depending on whether
 *   the selected profile matches the current profile.
 * - Triggers a UI redraw after processing.
 *
 * @param delta The change in profile index, typically from encoder input.
 */
static void app_encoder_show_profile(int delta)
{
	int new_profile = menu_profile_index + delta;
	if(new_profile < 0)
	{
		current_app_mode		= MODE_TOP_MENU;
		current_top_menu_action = TOP_MENU_FIRST;
		app_redraw();
		return;
	}
	if(new_profile >= MAX_PROFILES)
	{
		new_profile = 0;
	}

	if(new_profile != menu_profile_index)
	{
		menu_profile_index = new_profile;
	}

	if(menu_profile_index != current_profile)
	{
		current_app_mode = MODE_SHOW_PROFILE;
	} else
	{
		current_app_mode = MODE_SHOW_STATE;
	}
	app_redraw();
}

/**
 * @brief Updates the current edit period index based on encoder input.
 *
 * This function adjusts the `current_edit_period_index` by the specified `delta`.
 * If the new index goes below -1, it wraps around to `MAX_PERIODS - 1`.
 * If the new index exceeds or equals `MAX_PERIODS`, it wraps around to -1.
 * If the index changes, it updates `current_edit_period_index` and triggers a redraw.
 *
 * @param delta The amount to change the current edit period index by (positive or negative).
 */
static void app_encoder_edit_profile(int delta)
{
	int new_index = current_edit_period_index + delta;
	if(new_index < -1)
	{
		new_index = MAX_PERIODS - 1;
	} else if(new_index >= MAX_PERIODS)
	{
		new_index = -1;
	}

	if(new_index != current_edit_period_index)
	{
		current_edit_period_index = new_index;
		app_redraw();
	}
}

/**
 * @brief Adjusts the duration of the currently edited period by a specified delta.
 *
 * This function modifies the duration of the period currently being edited within the
 * active profile. The duration is adjusted in steps of 60 minutes (1 hour) multiplied
 * by the given delta. The resulting duration is clamped between 0 and 1440 minutes
 * (24 hours). If the duration changes, the display is updated by calling app_redraw().
 *
 * @param delta The number of hours (positive or negative) to adjust the duration by.
 */
static void app_encoder_edit_duration(int delta)
{
	period_t* period	   = &profiles[current_profile].periods[current_edit_period_index];
	int		  new_duration = period->duration + delta * 60; // change in 1 hour steps
	if(new_duration < 0)
	{
		new_duration = 0;
	} else if(new_duration > 24 * 60)
	{
		new_duration = 24 * 60;
	}

	if(new_duration != period->duration)
	{
		period->duration = new_duration;
		app_redraw();
	}
}

/**
 * @brief Handles encoder input to edit the current period value.
 *
 * This function updates the `current_edit_value` by adding the given `delta`.
 * If the new value goes below `EDIT_FIRST`, it wraps around to `EDIT_LAST`.
 * If it exceeds `EDIT_LAST`, it wraps around to `EDIT_FIRST`.
 * If the value changes, it updates `current_edit_value` and triggers a redraw
 * of the application UI by calling `app_redraw()`.
 *
 * @param delta The increment or decrement value to apply to the current edit value.
 */
static void app_encoder_edit_period(int delta)
{
	int new_value = current_edit_value + delta;
	if(new_value < EDIT_FIRST)
	{
		new_value = EDIT_LAST;
	} else if(new_value > EDIT_LAST)
	{
		new_value = EDIT_FIRST;
	}

	if(new_value != current_edit_value)
	{
		current_edit_value = new_value;
		app_redraw();
	}
}

/**
 * @brief Adjusts the white-red LED power level for the currently edited period.
 *
 * This function modifies the `led_white_red_power` field of the currently selected period
 * by a delta value (in 5% increments), ensuring the result stays within the 0-100% range.
 * If the power level changes, the display is updated and the new state is applied immediately.
 *
 * @param delta The increment or decrement value (multiplied by 5) to adjust the power level.
 */
static void app_encoder_edit_white_red_level(int delta)
{
	period_t* period	= &profiles[current_profile].periods[current_edit_period_index];
	int		  new_level = period->led_white_red_power + delta * 5; // change in 5% steps
	if(new_level < 0)
	{
		new_level = 0;
	} else if(new_level > 100)
	{
		new_level = 100;
	}
	if(new_level != period->led_white_red_power)
	{
		period->led_white_red_power = new_level;
		app_redraw();
	}
	current_app_state.white_red = new_level; // update current state immediately
	app_apply_state();
}

/**
 * @brief Adjusts the blue LED power level for the currently edited period.
 *
 * This function modifies the blue LED power level by a specified delta,
 * in steps of 5%. The new level is clamped between 0% and 100%. If the
 * level changes, the period's blue power is updated and the UI is redrawn.
 * The current application state is also updated and applied immediately.
 *
 * @param delta The amount to change the blue level, in 5% increments.
 */
static void app_encoder_edit_blue_level(int delta)
{
	period_t* period	= &profiles[current_profile].periods[current_edit_period_index];
	int		  new_level = period->led_blue_power + delta * 5; // change in 5% steps
	if(new_level < 0)
	{
		new_level = 0;
	} else if(new_level > 100)
	{
		new_level = 100;
	}
	if(new_level != period->led_blue_power)
	{
		period->led_blue_power = new_level;
		app_redraw();
	}
	current_app_state.blue = new_level; // update current state immediately
	app_apply_state();
}

/**
 * @brief Handles encoder input for navigating the top menu.
 *
 * This function updates the current top menu action based on the encoder delta.
 * If the new action is less than the first menu item, it resets the menu profile index
 * and changes the application mode depending on whether the selected profile is different
 * from the current profile. If the new action exceeds the last menu item, it wraps around
 * to the first menu item. Otherwise, it simply updates the current top menu action.
 * After processing, it triggers a redraw of the application UI.
 *
 * @param delta The change in encoder position (positive or negative integer).
 */
static void app_encoder_top_menu(int delta)
{
	int new_action = current_top_menu_action + delta;
	if(new_action < TOP_MENU_FIRST)
	{
		menu_profile_index = 0;

		if(menu_profile_index != current_profile)
		{
			current_app_mode = MODE_SHOW_PROFILE;
		} else
		{
			current_app_mode = MODE_SHOW_STATE;
		}
	} else if(new_action > TOP_MENU_LAST)
	{
		new_action				= TOP_MENU_FIRST;
		current_top_menu_action = new_action;
	} else
	{
		current_top_menu_action = new_action;
	}
	app_redraw();
}

/**
 * @brief Adjusts the global time shift value by a specified delta.
 *
 * This function modifies the global variable `time_shift_hours` by adding the given
 * `delta` value. The resulting value is clamped within the range [-23, 23]. If the
 * time shift value changes, the display is redrawn by calling `app_redraw()`.
 *
 * @param delta The amount to adjust the time shift, in hours.
 */
static void app_encoder_time_shift(int delta)
{
	int new_shift = time_shift_hours + delta;
	if(new_shift < -23)
	{
		new_shift = -23;
	} else if(new_shift > 23)
	{
		new_shift = 23;
	}
	if(new_shift != time_shift_hours)
	{
		time_shift_hours = new_shift;
		app_redraw();
	}
}

/**
 * @brief Handles changes in the encoder input and dispatches actions based on the current application mode.
 *
 * This function is called whenever the encoder value changes. It updates the timestamp of the last encoder event,
 * and then, depending on the current application mode, delegates the handling of the encoder change to the appropriate
 * function. If the delta is zero, the function returns immediately without taking any action.
 *
 * @param delta The change in encoder value. If zero, no action is taken.
 */
void app_on_encoder_change(int delta)
{
	if(delta == 0)
	{
		return;
	}

	last_encoder_time = get_absolute_time();

	switch(current_app_mode)
	{
	case MODE_SHOW_PROFILE:
	case MODE_SHOW_STATE:
		app_encoder_show_profile(delta);
		break;
	case MODE_EDIT_PROFILE:
		app_encoder_edit_profile(delta);
		break;
	case MODE_EDIT_PERIOD:
		app_encoder_edit_period(delta);
		break;
	case MODE_EDIT_DURATION:
		app_encoder_edit_duration(delta);
		break;
	case MODE_EDIT_WR_LEVEL:
		app_encoder_edit_white_red_level(delta);
		break;
	case MODE_EDIT_BL_LEVEL:
		app_encoder_edit_blue_level(delta);
		break;
	case MODE_TOP_MENU:
		app_encoder_top_menu(delta);
		break;
	case MODE_TIME_SHIFT:
		app_encoder_time_shift(delta);
		break;
	default:
		break;
	}
}

/**
 * @brief Handles the main click event in the application, performing actions based on the current application mode.
 *
 * This function is called when a click event occurs (e.g., button press or encoder click).
 * It updates the application state and transitions between different modes such as profile selection,
 * profile editing, period editing, and top menu actions. Depending on the current mode and context,
 * it may switch profiles, enter edit modes, apply state changes, save or reload profiles, shift time,
 * or reboot to bootloader. After handling the event, it triggers a redraw of the application UI.
 *
 * The function uses several global variables to track the current mode, selected profile, edit indices,
 * and application state.
 */
void app_on_click()
{
	last_encoder_time = get_absolute_time();

	switch(current_app_mode)
	{
	case MODE_SHOW_PROFILE:
		if(menu_profile_index != current_profile)
		{
			// Switch to selected profile
			current_profile	 = menu_profile_index;
			current_app_mode = MODE_SHOW_STATE;
			app_calculate_state();
			app_apply_state();
		}
		break;
	case MODE_SHOW_STATE:
		{
			// Enter profile edit mode
			current_edit_period_index = -1;
			current_app_mode		  = MODE_EDIT_PROFILE;
		}
		break;
	case MODE_EDIT_PROFILE:
		{
			if(current_edit_period_index == -1)
			{
				// Back button
				current_app_mode = MODE_SHOW_STATE;
			} else
			{
				// Edit selected period
				current_app_mode			= MODE_EDIT_PERIOD;
				current_edit_value			= EDIT_BACK;
				profile_t* profile			= &profiles[current_profile];
				period_t*  period			= &profile->periods[current_edit_period_index];
				current_app_state.white_red = period->led_white_red_power; // update current state immediately
				current_app_state.blue		= period->led_blue_power;	   // update current state immediately
				app_apply_state();
			}
		}
		break;
	case MODE_EDIT_PERIOD:
		{
			switch(current_edit_value)
			{
			case EDIT_BACK:
				current_app_mode = MODE_EDIT_PROFILE;
				break;
			case EDIT_WR_LEVEL:
				current_app_mode = MODE_EDIT_WR_LEVEL;
				break;
			case EDIT_BL_LEVEL:
				current_app_mode = MODE_EDIT_BL_LEVEL;
				break;
			case EDIT_DURATION:
				current_app_mode = MODE_EDIT_DURATION;
				break;
			default:
				break;
			}
		}
		break;
	case MODE_EDIT_DURATION:
	case MODE_EDIT_WR_LEVEL:
	case MODE_EDIT_BL_LEVEL:
		current_app_mode = MODE_EDIT_PERIOD;
		break;
	case MODE_TOP_MENU:
		{
			switch(current_top_menu_action)
			{
			case TOP_MENU_SAVE:
				app_save_profiles();
				break;
			case TOP_MENU_RELOAD:
				app_reload_profiles(true);
				break;
			case TOP_MENU_FLASH:
				app_reboot_to_bootloader();
				break;
			case TOP_MENU_SHIFT:
				time_shift_hours = 0;
				current_app_mode = MODE_TIME_SHIFT;
				break;
			default:
				break;
			}
		}
		break;
	case MODE_TIME_SHIFT:
		// Not implemented yet
		current_app_mode = MODE_TOP_MENU;
		if(time_shift_hours != 0)
		{
			app_start_time += (uint64_t) (time_shift_hours) * 60L * 60L * 1000L * 1000L; // apply time shift
			app_calculate_state();
		}
		break;
	default:
		break;
	}

	app_redraw();
}
