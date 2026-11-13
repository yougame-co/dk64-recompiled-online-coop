#ifndef YG_COOP_H
#define YG_COOP_H

/* Include common_structs.h first. All entry points become no-ops with the
 * single YG_COOP_ENABLED switch in yg_coop.c disabled. */
void yg_coop_frame(void);
void yg_coop_before_map_load(void);
Gfx *yg_coop_draw_echo(s32 camera, Actor *actor, Gfx *dl, s32 pass);
Gfx *yg_coop_framebuffer_begin(Gfx *dl);

#endif
