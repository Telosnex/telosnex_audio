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
