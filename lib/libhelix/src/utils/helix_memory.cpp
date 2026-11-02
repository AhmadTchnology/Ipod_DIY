#include "Allocator.h"
#include "helix_log.h"
#include "ConfigHelix.h"
#include "Arduino.h"

char log_buffer_helix[HELIX_LOG_SIZE];
ALLOCATOR alloc;

#ifdef __cplusplus
extern "C" {
#endif

// MyPod instrumentation: log every helix allocation (size + running total)
// so the decoder's true RAM appetite is visible on the serial console.
static int hxTotal = 0;

void* helix_malloc(int size) {
  void* p = alloc.allocate(size);
  hxTotal += size;
  Serial.printf("[hx] +%d = %d %s\n", size, hxTotal, p ? "" : "(FAILED)");
  return p;
}

void helix_free(void* ptr) { alloc.free(ptr); }

#ifdef __cplusplus
}
#endif
