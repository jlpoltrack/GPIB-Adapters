/* board.h timing for the simulated bus on the host (tests, vxi11_host); a board's sim build uses its own */

#include <time.h>
#include "gpib_sim.h"
#include "gpib_sim_pins.h"
#include "board.h"

/* The instruments see the lines once per delay. The host server (SIM_WALL_CLOCK) times out
 * in real time, as VISA clients expect; the tests run on simulated time. */
#ifdef SIM_WALL_CLOCK
uint32_t board_millis(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	sim_now_us++;
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
#else
uint32_t board_millis(void) { return (uint32_t)(++sim_now_us / 1000u); }
#endif
void board_delay_us(uint32_t us) { sim_bus(); sim_now_us += us; }
void board_delay_ms(uint32_t ms) { sim_bus(); sim_now_us += 1000ull * ms; }
void board_led_error(void) { sim_led_errors++; }

