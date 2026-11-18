/* The game module's <time.h> (slim split): the module's sources include it, use none of it but
 * the types. */
#pragma once
typedef long long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
