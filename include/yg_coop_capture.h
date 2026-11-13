#ifndef YG_COOP_CAPTURE_H
#define YG_COOP_CAPTURE_H

#include <cstdint>

// Called only by the app's graphics worker, after RT64 parsed the matching DL.
// gEXSetRenderToRAM makes that parse finish the native framebuffer readback.
void yg_coop_capture_after_dl(uint8_t *rdram, uint32_t task_dl);

#endif
