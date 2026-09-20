#pragma once
#include <stdint.h>
// A critical section guards the panel against the network thread. The page has
// one thread for all of that -- the audio thread never touches this state -- so
// the section is a pair of no-ops rather than a lock that could never contend.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
