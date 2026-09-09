/* Minimal target for test_spawn: stops at the marker with a known value, then
 * completes the increment so the parent can prove it resumed correctly. */
#include <stdio.h>

#include "adaptive_restore/marker.h"

int main(void) {
    volatile int x = 41;
    ar_checkpoint_here();
    x += 1;
    printf("x=%d\n", x);
    return 0;
}
