/* Compiles the vendored Sonic with probed allocations (I1) and one read-only
 * accessor that the E2 position needs (ADR D9). */
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "alloc_probe.h"
#define calloc tsnx_probe_calloc
#define realloc tsnx_probe_realloc
#define malloc tsnx_probe_malloc
#include "sonic.c"
#undef calloc
#undef realloc
#undef malloc

/* Source frames inside Sonic that are not yet output. */
int tsnx_sonic_pending_input_frames(sonicStream s) {
  return s->numInputSamples + s->numPitchSamples;
}

/* Drops all buffered audio without allocating. The next input starts clean
 * (ADR I5). */
void tsnx_sonic_reset(sonicStream s) {
  s->numInputSamples = 0;
  s->numOutputSamples = 0;
  s->numPitchSamples = 0;
  s->remainingInputToCopy = 0;
  s->inputPlayTime = 0.0f;
  s->timeError = 0.0f;
  s->oldRatePosition = 0;
  s->newRatePosition = 0;
  s->prevPeriod = 0;
  s->prevMinDiff = 0;
}
