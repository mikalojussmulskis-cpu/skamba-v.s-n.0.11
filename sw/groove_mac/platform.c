// platform.c - macOS clock and lock for engine.c (kept apart: Apple's headers
// define bool, which clashes with the firmware's core.h)
#include <stdint.h>
#include <pthread.h>
#include <mach/mach_time.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
void plat_lock(void) { pthread_mutex_lock(&mu); }
void plat_unlock(void) { pthread_mutex_unlock(&mu); }
uint32_t plat_ms(void) {
	static mach_timebase_info_data_t tb;
	if (!tb.denom) mach_timebase_info(&tb);
	return (uint32_t)(mach_absolute_time() * tb.numer / tb.denom / 1000000ull);
}
