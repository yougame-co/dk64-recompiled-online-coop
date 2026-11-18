/* YouGame local milestone 2. No actor registration, player slot, physics,
 * animation scripts, camera, or save data belongs to the puppet.
 * These defaults are deliberately local to this experimental build. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-declarations"
#include "common_structs.h"
#pragma clang diagnostic pop
#include "yg_coop.h"
#include "PR/os_cont.h"
#include "misc_funcs.h"
#include "../include/yg_coop_wire.h"

#ifndef YG_COOP_ENABLED
#define YG_COOP_ENABLED 1
#endif
#ifndef YG_COOP_AUTOPILOT
#define YG_COOP_AUTOPILOT 0
#endif
#ifndef YG_COOP_FRAME_DUMP
#define YG_COOP_FRAME_DUMP 0
#endif
#ifndef YG_COOP_PUPPET_OFFSET
#define YG_COOP_PUPPET_OFFSET 1
#endif
#ifndef YG_COOP_SPAWN_SETTLE_FRAMES
/* frame counts 30 Hz game updates, independently of interpolated rendering. */
#define YG_COOP_SPAWN_SETTLE_FRAMES 60
#endif

extern Actor *gPlayerPointer;
extern Actor *gCurrentPlayer;
extern PlayerAdditionalActorData *extra_player_info_pointer;
extern u8 cc_player_index;
extern s16 D_global_asm_807FD584;
extern Maps current_map;
extern u8 current_character_index[];
extern u8 cc_number_of_players;
extern u8 is_cutscene_active;
extern u8 D_global_asm_8076A0B1;
extern u8 D_global_asm_807444FC;
extern u8 D_global_asm_807ECD08;
extern u8 D_global_asm_807ECD09;
extern volatile u8 D_global_asm_807467C4;
extern u16 *D_global_asm_807FBB58;
extern s32 *D_global_asm_807FBB50;
extern u16 D_global_asm_807FBB30;
extern void *D_global_asm_80744470[];
extern Gfx *D_global_asm_8076A050[];
extern s16 D_global_asm_80744490;
extern s16 D_global_asm_80744494;
extern u8 D_global_asm_80750AC0;
extern s32 D_global_asm_80755338;
extern CharacterChange *character_change_array;
extern u32 global_properties_bitfield;
extern s32 object_timer;

/* decomp include/structs.h:Struct807FB630 (12 bytes on MIPS). */
typedef struct {
    u16 model;
    u16 references;
    void *data;
    u8 flags;
} YGModelCacheEntry;
extern YGModelCacheEntry D_global_asm_807FB630[];

int gameIsInAdventureMode(void);
int gameIsInMainMenuMode(void);
s32 func_global_asm_806134B4(Actor *actor, u16 model);
void func_global_asm_806136B4(Actor *actor);
void func_global_asm_80613794(Actor *actor, u8 slot);
void func_global_asm_80613CA8(Actor *actor, s16 animation, f32 start, f32 blend);
void func_global_asm_80614644(Actor *actor, AnimationStateUnk0 *track, f32 frame);
void func_global_asm_80614A64(Actor *actor);
void renderActor(Actor *actor, u8 multiply);
Gfx *func_global_asm_80630DCC(s32 camera, Actor *actor, Gfx *dl, s32 pass);
void func_global_asm_8060AA58(u8 controllers);
void func_global_asm_8060DC3C(u8 file, s32 start);
void func_global_asm_80731030(void);
void func_global_asm_80712490(Maps map, s32 exit, u8 mode);
void func_global_asm_80712EDC(void);
void func_global_asm_806C8E58(s16 actor_type);
void func_global_asm_806F0C18(Actor *actor);
void func_global_asm_806CFF9C(Actor *actor);
void playAnimation(Actor *actor, s32 animation);
f32 func_global_asm_80612790(s16 angle);
f32 func_global_asm_80612794(s16 angle);

/* Absolute import in syms.ld; implemented in src/game/recomp_api.cpp. */
void yg_coop_request_frame(void *task_dl, void *framebuffer, u32 dimensions, u32 seconds);
u32 yg_coop_camera_input(void);
u32 yg_coop_net_mode(void);
u32 yg_coop_net_step(void *local, void *remote, u32 frame, u32 size);
u32 yg_coop_test_kong(void); /* 0=off; low byte=Kong+1; bit 8=file source. */

#if YG_COOP_ENABLED
#define YG_LAG 90
#define YG_HISTORY (YG_LAG + 1)
#define YG_OFFSET_RIGHT (YG_COOP_PUPPET_OFFSET ? 60.0f : 0.0f)
#define YG_STICK_DEADBAND 8
#define YG_WALK_SECONDS 30

enum { YG_MENU, YG_WAIT_MAP, YG_DIALOG, YG_SETTLE, YG_WALK, YG_STAND };

typedef struct {
    Maps map;
    tuple_f position;
    s16 rotation[3];
    s16 animation;
    s16 logical_animation;
    u16 model;
    u16 hands;
    f32 animation_frame;
    f32 animation_start;
    f32 animation_end;
    u32 tick;
    u8 moving;
    f32 scale[3];
    u8 kong;
    u8 control_state;
    u8 net_flags; /* Uses 1b's tail padding; echo sample size is unchanged. */
    u8 spawn_epoch;
} YGSample;

_Static_assert(sizeof(YGSample) == YG_SAMPLE_SIZE, "YGSample wire size");
#define YG_CHECK(field, offset) _Static_assert(__builtin_offsetof(YGSample, field) == offset, "YGSample " #field)
YG_CHECK(map, YG_S_MAP); YG_CHECK(position, YG_S_POSITION);
YG_CHECK(rotation, YG_S_ROTATION); YG_CHECK(animation, YG_S_ANIMATION);
YG_CHECK(logical_animation, YG_S_LOGICAL); YG_CHECK(model, YG_S_MODEL);
YG_CHECK(hands, YG_S_HANDS); YG_CHECK(animation_frame, YG_S_ANIMATION_FRAME);
YG_CHECK(animation_start, YG_S_ANIMATION_START); YG_CHECK(animation_end, YG_S_ANIMATION_END);
YG_CHECK(tick, YG_S_TICK); YG_CHECK(moving, YG_S_MOVING); YG_CHECK(scale, YG_S_SCALE);
YG_CHECK(kong, YG_S_KONG); YG_CHECK(control_state, YG_S_CONTROL);
YG_CHECK(net_flags, YG_S_NET_FLAGS); YG_CHECK(spawn_epoch, YG_S_SPAWN);
#undef YG_CHECK

static u32 net_mode;
static u32 net_status;
static YGSample net_local;
static Actor *net_player;
static u8 net_spawn_epoch;
static u8 net_wait_logged;

static YGSample history[YG_HISTORY];
/* Static storage is outside the game's heap and outside both actor lists.
 * Only its owned model/animation allocations use the normal map heap. */
static Actor echo_models[5] __attribute__((aligned(16)));
/* code_CC800.c:25-29, enums.h:244-248; indexed by wire/current Kong. */
static const u16 kong_models[5] = { 4, 1, 6, 9, 12 };
static const u16 kong_actors[5] = { ACTOR_DK, ACTOR_DIDDY, ACTOR_LANKY, ACTOR_TINY, ACTOR_CHUNKY };
static Actor *echo = &echo_models[0];
static YGSample delayed;
static u32 frame;
static u32 samples;
static u32 write_index;
static s32 history_map = -1;
static u8 model_ready;
static u8 cached_models;
static u8 failed_models;
static u8 cache_full_models;
static u8 logged_kong_fallback;
static u8 pose_reset;
static s8 logged_puppet_kong = -1;
static s8 logged_remote_kong = -1;
static u8 visible;
static u8 failed;
static u8 echo_kong;
static u8 dump_index;
/* Release play starts in the same state as the test's real-input handoff. */
static u8 autopilot_disabled = !YG_COOP_AUTOPILOT;
static u8 net_role_seen;
static u8 relay_lost_logged;
static u8 autopilot_step;
static OSTime walk_start;
static OSTime stand_start;
static u32 stand_frame;
static u32 ready_frames;
static u8 walk_started;
static u8 logged_tight_circle;
static u32 menu_frames;
static u8 skipped_intro;
static s16 circle_heading;
static u8 logged_animation_fallback;
static u8 logged_idle_proxy;
static u8 test_kong_done;
static u32 spawn_settle_frame;
static u32 spawn_hold_start;
static u8 spawn_hold_active;
static u8 spawn_blocked = 1;
static s32 spawn_peer_map = -1;

static tuple_f echo_position(void) {
    tuple_f position = delayed.position;
    /* Game forward=(sin(yaw),cos(yaw)); right=(cos(yaw),-sin(yaw)).
     * Offset the delayed facing, never the live player's pose or history. */
    if (net_mode) return position;
    position.x += YG_OFFSET_RIGHT * func_global_asm_80612790(delayed.rotation[1]);
    position.z -= YG_OFFSET_RIGHT * func_global_asm_80612794(delayed.rotation[1]);
    return position;
}

static int valid_player(void) {
    return gameIsInAdventureMode() && cc_number_of_players == 1 &&
        character_change_array && character_change_array[0].does_player_exist &&
        gPlayerPointer && gPlayerPointer == character_change_array[0].playerPointer &&
        (gPlayerPointer->interactable & 1) &&
        (gPlayerPointer->object_properties_bitfield & 0x10) &&
        gPlayerPointer->animation_state && gPlayerPointer->animation_state->unk0 &&
        current_character_index[0] < 5;
}

static void hide_echo(const char *reason) {
    if (visible) {
        recomp_printf("YG-COOP despawn frame=%u map=%d reason=%s\n", frame, history_map, reason);
    }
    visible = 0;
    samples = 0;
    write_index = 0;
}

/* Shared by update, model selection, pose and draw: a render callback can see
 * a transition/cutscene before the next game update. Never touch map-owned
 * puppet buffers here; hide_echo only clears visibility/history. */
static int spawn_held(void) {
    const char *reason = 0;
    if (is_cutscene_active) reason = "cutscene";
    else if ((D_global_asm_8076A0B1 & 1) ||
             history_map != (s32)current_map) reason = "map-load";
    else if (current_map == MAP_MAIN_MENU || gameIsInMainMenuMode() ||
             (global_properties_bitfield & 2) || !valid_player())
        reason = "menu";
    if (reason) {
        spawn_settle_frame = frame;
        spawn_blocked = 1;
    } else {
        /* Start a full settle interval on the first safe frame, including
         * the frame on which a cutscene ends or a player becomes valid. */
        if (spawn_blocked) {
            spawn_settle_frame = frame;
            spawn_blocked = 0;
        }
        if ((u32)(frame - spawn_settle_frame) < YG_COOP_SPAWN_SETTLE_FRAMES)
            reason = "settle";
    }
    if (reason) {
        if (!spawn_hold_active) {
            spawn_hold_active = 1;
            spawn_hold_start = frame;
            recomp_printf("YG-COOP spawn-hold start frame=%u map=%d reason=%s\n",
                          frame, current_map, reason);
            hide_echo(reason);
        } else if (visible) hide_echo(reason);
        return 1;
    }
    if (spawn_hold_active) {
        recomp_printf("YG-COOP spawn-hold end frame=%u map=%d held-frames=%u\n",
                      frame, current_map, (u32)(frame - spawn_hold_start));
        spawn_hold_active = 0;
    }
    return 0;
}

static void spawn_peer_frame(void) {
    s32 peer_map = (net_status & YG_NET_LIVE) ? (s32)delayed.map : -1;
    if (spawn_peer_map != peer_map) {
        spawn_peer_map = peer_map;
        if (peer_map == (s32)current_map) spawn_settle_frame = frame;
    }
}

static int test_kong_frame(void) {
#if YG_COOP_AUTOPILOT
    if (!test_kong_done) {
        u32 selection = yg_coop_test_kong();
        u8 kong = (selection & 0xFF) - 1;
        test_kong_done = 1;
        if (!selection || kong >= 5) return 0;
        if (current_character_index[0] != kong) {
            /* Retry on a later settled frame if the additional data is absent. */
            if (!gPlayerPointer->PaaD) {
                test_kong_done = 0;
                return 1;
            }
            Actor *saved_player = gCurrentPlayer;
            PlayerAdditionalActorData *saved_info = extra_player_info_pointer;
            u8 saved_index = cc_player_index;
            s16 saved_character = D_global_asm_807FD584;
            if (D_global_asm_807FBB30 >= 28) {
                recomp_printf("YG-COOP error test-kong model-cache-full want=%u\n", kong);
                return 0;
            }
            /* The game's character-swap command: code_2C910.c:209-211,
             * code_CC800.c:475-520. No unlock flags or save writes. Supply
             * its player context explicitly: this hook runs before updates. */
            gCurrentPlayer = gPlayerPointer;
            extra_player_info_pointer = gPlayerPointer->PaaD;
            cc_player_index = 0;
            func_global_asm_806C8E58(kong_actors[kong]);
            extra_player_info_pointer->unk1EE = kong_actors[kong];
            func_global_asm_806F0C18(gPlayerPointer);
            func_global_asm_806CFF9C(gPlayerPointer);
            playAnimation(gPlayerPointer, 1); /* relative idle for the new Kong */
            gCurrentPlayer = saved_player;
            extra_player_info_pointer = saved_info;
            cc_player_index = saved_index;
            if (saved_player != gPlayerPointer) D_global_asm_807FD584 = saved_character;
        }
        recomp_printf("YG-COOP test-kong local=%u source=%s\n", kong,
                      selection & 0x100 ? "file" : "env");
        return 1; /* Settle the new model before advertising network readiness. */
    }
#endif
    return 0;
}

static void autopilot_frame(void) {
#if YG_COOP_AUTOPILOT
    if (autopilot_disabled || net_mode == YG_NET_ERROR) return;
    if (yg_coop_camera_input()) {
        autopilot_disabled = 1;
        recomp_printf("YG-COOP autopilot off-real-camera-input\n");
        return;
    }
    if (autopilot_step == YG_MENU) {
        if (frame == 1) recomp_printf("YG-COOP autopilot wait-main-menu\n");
        if (!skipped_intro && !gameIsInAdventureMode() &&
            current_map != MAP_MAIN_MENU && object_timer > 30 &&
            !(D_global_asm_8076A0B1 & 1) && is_cutscene_active <= 1) {
            /* Normal return-to-menu routine. Avoid waiting for the timed
             * Nintendo/opening/Rap sequence on every unattended run. */
            func_global_asm_80712EDC();
            skipped_intro = 1;
            recomp_printf("YG-COOP autopilot skip-intro-to-menu\n");
        }
        if (gameIsInMainMenuMode() && current_map == MAP_MAIN_MENU &&
            !(D_global_asm_8076A0B1 & 1) && is_cutscene_active == 0) {
            if (++menu_frames >= 60) {
                /* Same file initialization used by file-select preview, with
                 * start=0: no explicit save/EEPROM write. Warp through the
                 * game's normal transition, with its adventure mode set. */
                func_global_asm_8060AA58(1);
                func_global_asm_80731030();
                func_global_asm_8060DC3C(0, 0);
                D_global_asm_80750AC0 = 1;
                D_global_asm_80755338 = 0;
                func_global_asm_80712490(MAP_TRAINING_GROUNDS, 1, GAME_MODE_ADVENTURE);
                autopilot_step = YG_WAIT_MAP;
                recomp_printf("YG-COOP autopilot load-training-grounds file=0 map=%d exit=1\n", MAP_TRAINING_GROUNDS);
            }
        } else {
            menu_frames = 0;
        }
    } else if (autopilot_step == YG_WAIT_MAP && valid_player() &&
               current_map == MAP_TRAINING_GROUNDS && !(D_global_asm_8076A0B1 & 1)) {
        autopilot_step = YG_DIALOG;
        recomp_printf("YG-COOP autopilot squawks-dialog B-pulse A-fast-text\n");
    } else if (autopilot_step == YG_DIALOG || autopilot_step == YG_SETTLE) {
        if (!valid_player() || is_cutscene_active || (D_global_asm_8076A0B1 & 1) ||
            (global_properties_bitfield & 2) || object_timer <= 90 ||
            gPlayerPointer->control_state != 0xC) {
            ready_frames = 0;
        } else {
            if (autopilot_step == YG_DIALOG) {
                autopilot_step = YG_SETTLE;
                recomp_printf("YG-COOP autopilot dialog-clear settle-30-frames\n");
            }
            if (ready_frames < 30) ++ready_frames;
            if (ready_frames >= 30) {
                if (current_map == MAP_TRAINING_GROUNDS && test_kong_frame()) {
                    ready_frames = 0;
                    return;
                }
                if (net_mode) {
                    if (!net_wait_logged) {
                        net_wait_logged = 1;
                        recomp_printf("YG-COOP autopilot net-ready role=%s waiting-peer\n",
                                      net_mode == YG_NET_ROLE_A ? "a" : "b");
                    }
                    /* A starts on B's readiness; B starts on A's walk bit.
                     * Both bits travel in ordinary frame packets. No clocks
                     * or extra sockets, and no forced game state. */
                    u8 required = net_mode == YG_NET_ROLE_A ? YG_NET_READY : YG_NET_WALK;
                    if (!(net_status & YG_NET_LIVE) || delayed.map != current_map ||
                        !(delayed.net_flags & required)) return;
                }
                walk_start = osGetTime();
                walk_started = 1;
                circle_heading = gPlayerPointer->y_rotation;
                autopilot_step = YG_WALK;
                if (net_mode == YG_NET_ROLE_B) {
                    recomp_printf("YG-COOP autopilot walk-start role=b duration=30s leadout=3s wait=1s reverse-circle-period=4s stick=40\n");
                } else {
                    recomp_printf("YG-COOP autopilot walk-start duration=30s leadout=3s circle-period=4s stick=40\n");
                }
            }
        }
    } else if (autopilot_step == YG_WALK) {
        OSTime elapsed = osGetTime() - walk_start;
        if (!logged_tight_circle && elapsed >= OS_USEC_TO_CYCLES(
                net_mode == YG_NET_ROLE_B ? 4000000ULL : 3000000ULL)) {
            logged_tight_circle = 1;
            if (net_mode == YG_NET_ROLE_B) {
                recomp_printf("YG-COOP autopilot tight-circle role=b period=4s reverse-heading left-turn\n");
            } else {
                recomp_printf("YG-COOP autopilot tight-circle period=4s right-turn\n");
            }
        }
        if (elapsed >= OS_USEC_TO_CYCLES(YG_WALK_SECONDS * 1000000ULL)) {
            autopilot_step = YG_STAND;
            stand_start = osGetTime();
            stand_frame = frame;
            recomp_printf("YG-COOP autopilot stand walk-seconds=30\n");
        }
    }
#endif
}

static void autopilot_input(OSContPad *pads) {
#if YG_COOP_AUTOPILOT
    if (autopilot_disabled) return;
    s32 i;
    for (i = 0; i < 4; i++) {
        if ((D_global_asm_807ECD09 & (1 << i)) && pads[i].errno == 0 &&
            (pads[i].button || pads[i].stick_x > YG_STICK_DEADBAND ||
             pads[i].stick_x < -YG_STICK_DEADBAND || pads[i].stick_y > YG_STICK_DEADBAND ||
             pads[i].stick_y < -YG_STICK_DEADBAND)) {
            if (!autopilot_disabled) {
                autopilot_disabled = 1;
                recomp_printf("YG-COOP autopilot off-real-input controller=%d\n", i);
            }
            return;
        }
    }
    if (!autopilot_disabled && yg_coop_camera_input()) {
        autopilot_disabled = 1;
        recomp_printf("YG-COOP autopilot off-real-camera-input\n");
    }
    if (autopilot_disabled || autopilot_step < YG_DIALOG || !valid_player()) return;
    /* Suppress idle drift throughout dialog, walk and the final standing pose.
     * Real input was tested above, before writing any synthetic input. */
    pads[0].button = 0;
    pads[0].stick_x = pads[0].stick_y = 0;
    if (is_cutscene_active &&
        (autopilot_step == YG_DIALOG || autopilot_step == YG_SETTLE)) {
        /* Text actor: code_A8400.c:448 B dismisses; :456 A speeds text.
         * Release B between pulses so newly_pressed_input_copy gets an edge.
         * Never inject Start (pause), alter cutscene state, or write a flag. */
        pads[0].button = frame % 12 == 0 ? B_BUTTON : A_BUTTON;
        return;
    }
    if (autopilot_step != YG_WALK || is_cutscene_active ||
        (D_global_asm_8076A0B1 & 1) || (global_properties_bitfield & 2)) return;
    /* Walk off DK's raised starting platform for 3s, then stay in a small
     * clockwise circle. At 30Hz the 90-frame echo is 3s behind: a 270-degree
     * turn rather than a point directly behind the follow camera. */
    OSTime elapsed = osGetTime() - walk_start;
    if (elapsed < OS_USEC_TO_CYCLES(YG_WALK_SECONDS * 1000000ULL)) {
        u32 milliseconds = (u32)(elapsed / OS_USEC_TO_CYCLES(1000));
        u32 turn_ms = milliseconds > 3000 ? milliseconds - 3000 : 0;
        s32 turn = ((turn_ms % 4000) * 4096) / 4000;
        if (net_mode == YG_NET_ROLE_B && milliseconds >= 3000) {
            if (milliseconds < 4000) return; /* stand at the shared leadout */
            /* Reverse tangent AND turn direction: same circle center as A.
             * At +5/+15/+25 both head toward the other's quarter-circle point. */
            turn = 2048 - (((milliseconds - 4000) % 4000) * 4096) / 4000;
        }
        /* Inverse of handleInputsForControlState's camera+atan2(x,-y). */
        s16 angle = (circle_heading + turn - character_change_array[0].unk2C8) & 0xFFF;
        pads[0].stick_x = (s8)(40.0f * func_global_asm_80612794(angle));
        pads[0].stick_y = (s8)(-40.0f * func_global_asm_80612790(angle));
    }
#endif
}

static int create_echo(u8 player_kong) {
    if (spawn_held()) return 0;
    u8 wanted = net_mode ? player_kong : player_kong == 1 ? 0 : 1;
    Actor *candidate;
    int loaded = 0;
    int attempted = 0;
    u16 entries_before;
    u16 references_before[32];
    if (wanted >= 5 || (failed_models & (1 << wanted))) return 0;
    candidate = &echo_models[wanted];
    if (cached_models & (1 << wanted)) goto select_model;
    /* Keep at most two private models for this map and four free cache slots
     * after a new load. Refuse a third; retain the last selected puppet.
     * The loader reads
     * TABLE_5 on demand, irrespective of unlock flags/map residency:
     * code_17B90.c:25-67. Never discard the previous puppet on failure. */
    if ((cached_models && (cached_models & (cached_models - 1))) ||
        D_global_asm_807FBB30 >= 28) {
        recomp_printf("YG-COOP error model-cache-full frame=%u map=%d\n", frame, current_map);
        cache_full_models |= 1 << wanted;
        goto load_failed;
    }
    bzero(candidate, sizeof(*candidate));
    candidate->unk58 = kong_actors[wanted];
    candidate->unk54 = 0xFF0 + wanted; /* private RT64 interpolation tags */
    candidate->object_properties_bitfield = 0x414; /* visible, initialized, no distance cull */
    candidate->interactable = 0;
    candidate->noclip_byte = 1;
    candidate->draw_distance = 2000;
    candidate->shadow_opacity = 255;
    candidate->unk12C = -1;
    candidate->unk146 = 0;
    candidate->unk16D = 15;
    entries_before = D_global_asm_807FBB30;
    for (u32 i = 0; i < entries_before && i < 32; i++)
        references_before[i] = D_global_asm_807FB630[i].references;
    attempted = 1;
    recomp_printf("YG-COOP model-load-start frame=%u map=%d kong=%d\n", frame, current_map, wanted);
    if (!func_global_asm_806134B4(candidate, kong_models[wanted]) ||
        !candidate->unk0 || !candidate->animation_state || !candidate->animation_state->unk0)
        goto load_failed;
    /* The game's exhausted-cache path can return another model's data.
     * Verify identity as well as the loader's success result. */
    for (u32 i = 0; i < D_global_asm_807FBB30 && i < 32; i++) {
        if (D_global_asm_807FB630[i].data == (void *)candidate->unk0 &&
            D_global_asm_807FB630[i].model == kong_models[wanted]) loaded = 1;
    }
    if (!loaded) goto load_failed;
    cached_models |= 1 << wanted;
select_model:
    pose_reset = !model_ready || echo != candidate;
    echo_kong = wanted;
    echo = candidate;
    return 1;
load_failed:
    /* Same model/animation teardown as character replacement and actor disposal:
     * code_CC800.c:507, code_7CA80.c:331-332, code_17B90.c:304-322.
     * A partial loader result may own only the model reference (slot 0). */
    if (attempted) {
        int owns_model = 0;
        for (u32 i = 0; i < D_global_asm_807FBB30 && i < 32; i++) {
            if (D_global_asm_807FB630[i].data == (void *)candidate->unk0 &&
                (i >= entries_before || D_global_asm_807FB630[i].references > references_before[i]))
                owns_model = 1;
        }
        /* code_17B90.c:37-38 can return borrowed fallback data without taking
         * a reference. Free its new animation state, not another actor's model. */
        if (!owns_model) candidate->unk0 = 0;
        if (candidate->animation_state) func_global_asm_806136B4(candidate);
        else if (candidate->unk0) func_global_asm_80613794(candidate, 0);
        bzero(candidate, sizeof(*candidate));
    }
    failed_models |= 1 << wanted;
    recomp_printf("YG-COOP error model-load frame=%u map=%d kong=%d\n", frame, current_map, wanted);
    return 0;
}

static void capture_sample(YGSample *sample, Actor *player, AnimationStateUnk0 *track) {
    sample->map = current_map;
    sample->position = player->position;
    sample->rotation[0] = player->x_rotation;
    sample->rotation[1] = player->y_rotation;
    sample->rotation[2] = player->z_rotation;
    sample->animation = track->unk10;
    sample->animation_frame = track->unk4;
    sample->animation_start = track->unk14;
    sample->animation_end = track->unk18;
    sample->tick = frame;
    sample->logical_animation = player->PaaD ? player->PaaD->unk4E : -1;
    sample->model = 0;
    for (u32 i = 0; i < D_global_asm_807FBB30 && i < 32; i++) {
        if (D_global_asm_807FB630[i].data == (void *)player->unk0) {
            sample->model = D_global_asm_807FB630[i].model;
            break;
        }
    }
    sample->hands = player->unk146;
    sample->scale[0] = player->animation_state->scale_x;
    sample->scale[1] = player->animation_state->scale_y;
    sample->scale[2] = player->animation_state->scale_z;
    sample->kong = current_character_index[0];
    sample->control_state = player->control_state;
}

static void network_frame(void) {
    tuple_f previous = net_local.position;
    s32 previous_map = net_local.map;
    int local_valid = valid_player() && !(D_global_asm_8076A0B1 & 1);
    bzero(&net_local, sizeof(net_local));
    net_local.map = -1; /* Heartbeat hides the peer's puppet during transitions. */
    net_local.tick = frame;
    if (local_valid) {
        /* Generation persists through heartbeats and relay latest-frame drops.
         * A reused actor address still changes generation after any invalid frame. */
        if (net_player != gPlayerPointer) ++net_spawn_epoch;
        net_player = gPlayerPointer;
        capture_sample(&net_local, gPlayerPointer, gPlayerPointer->animation_state->unk0);
        f32 dx = net_local.position.x - previous.x;
        f32 dz = net_local.position.z - previous.z;
        net_local.moving = previous_map == (s32)current_map && dx * dx + dz * dz > 0.01f;
        if (!autopilot_disabled && !is_cutscene_active && !(global_properties_bitfield & 2)) {
            if ((autopilot_step == YG_SETTLE && ready_frames >= 30) || walk_started)
                net_local.net_flags |= YG_NET_READY;
            if (walk_started) net_local.net_flags |= YG_NET_WALK;
        }
    } else net_player = 0;
    net_local.spawn_epoch = net_spawn_epoch;
    net_status = yg_coop_net_step(&net_local, &delayed, frame, sizeof(net_local));
    spawn_peer_frame();
    if (spawn_held()) return;
    if (!local_valid || !(net_status & YG_NET_LIVE) || delayed.map != current_map) {
        hide_echo(!local_valid ? "player-unavailable" :
                  !(net_status & YG_NET_LIVE) ? "peer-unavailable" : "peer-map");
        return;
    }
    if (delayed.kong >= 5) {
        hide_echo("peer-kong-invalid");
        return;
    }
    if (!model_ready || echo_kong != delayed.kong) {
        if (create_echo(delayed.kong)) {
            hide_echo("kong-change");
            model_ready = 1;
        } else if (!(logged_kong_fallback & (1 << delayed.kong))) {
            logged_kong_fallback |= 1 << delayed.kong;
            recomp_printf("YG-COOP kong-fallback want=%u drew=%d reason=%s\n",
                          delayed.kong, model_ready ? (s32)echo_kong : -1,
                          cache_full_models & (1 << delayed.kong) ? "model-cache-full" : "model-load");
        }
    }
    if (model_ready && !visible) {
        visible = 1;
        recomp_printf("YG-COOP spawn frame=%u map=%d kong=%d remote-kong=%d lag=0 offset=0 approach=remote-private-model\n",
                      frame, current_map, echo_kong, delayed.kong);
    }
}

static void pose_echo(void) {
    if (spawn_held()) return;
    s16 animation;
    s32 logical = delayed.logical_animation;
    s32 mapped = 0;
    int direct = 0;
    /* Loaded TABLE_13 header offsets [2..3] bound the relative-animation
     * table (110 seven-Kong rows in this ROM), not an arbitrary 0x60 limit. */
    s32 rows = (D_global_asm_807FBB50[3] - D_global_asm_807FBB50[2]) / (7 * sizeof(u16));
    AnimationStateUnk0 *track = echo->animation_state->unk0;
    echo->position = echo_position();
    echo->x_rotation = delayed.rotation[0];
    echo->y_rotation = delayed.rotation[1];
    echo->z_rotation = delayed.rotation[2];
    echo->unk146 = delayed.hands;
    echo->control_state = delayed.control_state;
    echo->animation_state->scale_x = delayed.scale[0];
    echo->animation_state->scale_y = delayed.scale[1];
    echo->animation_state->scale_z = delayed.scale[2];
    /* Never run animation scripts: those can play sounds/spawn items. */
    if (delayed.kong < 5 && logical >= 0 && logical < rows &&
        D_global_asm_807FBB58[logical * 7 + delayed.kong] == delayed.animation) {
        animation = D_global_asm_807FBB58[logical * 7 + echo_kong];
        mapped = 1;
    } else if (delayed.kong < 5) {
        /* Idle scripts can change the raw animation without updating PaaD's
         * last relative ID. Recover that ID by comparing the source column. */
        for (logical = 0; logical < rows; logical++) {
            if (D_global_asm_807FBB58[logical * 7 + delayed.kong] == delayed.animation) {
                animation = D_global_asm_807FBB58[logical * 7 + echo_kong];
                mapped = 1;
                break;
            }
        }
    }
    /* Only a raw clip present in the sender Kong's own column may reach the
     * loader. Matching skeletons do not make a network animation ID trusted. */
    if (mapped && net_mode && echo_kong == delayed.kong) {
        animation = delayed.animation;
        direct = 1;
    }
    if (!mapped) {
        /* DK raw 15 is an idle-script variant, absent from all 110 relative
         * rows. Use the target's relative idle 1 (Diddy raw 163), with a live
         * loop rather than pretending raw 15 matches another skeleton.
         * Other script-only clips use a moving walk / stationary idle proxy. */
        logical = delayed.moving ? 2 : 1;
        animation = D_global_asm_807FBB58[logical * 7 + echo_kong];
        if (delayed.kong == 0 && delayed.animation == 15) {
            if (!logged_idle_proxy) {
                logged_idle_proxy = 1;
                recomp_printf("YG-COOP animation-proxy source=15 relative=%d target=%d animated-loop\n",
                              logical, animation);
            }
        } else if (!logged_animation_fallback) {
            logged_animation_fallback = 1;
            recomp_printf("YG-COOP animation-proxy source=%d logical=%d relative=%d target=%d animated-loop\n",
                          delayed.animation, delayed.logical_animation, logical, animation);
        }
    }
    if (pose_reset || track->unk10 != animation || !track->unk0) {
        func_global_asm_80613CA8(echo, animation, 0.0f, 0.0f);
        pose_reset = 0;
        /* That function rotates the four animation-track pointers. */
        track = echo->animation_state->unk0;
    }
    if (track->unk0) {
        f32 phase;
        if (mapped && delayed.animation_end >= delayed.animation_start) {
            /* Preserve cycle phase across different clip lengths. Copying a
             * raw DK frame then clamping can freeze a shorter Diddy clip. */
            phase = (delayed.animation_frame - delayed.animation_start) /
                    (delayed.animation_end - delayed.animation_start + 1.0f);
            if (phase < 0.0f) phase = 0.0f;
            if (phase >= 1.0f) phase = 0.999f;
        } else {
            phase = (delayed.tick % 60) / 60.0f;
        }
        f32 animation_frame = direct ? delayed.animation_frame :
            track->unk14 + phase * (track->unk18 - track->unk14);
        if (animation_frame < track->unk14) animation_frame = track->unk14;
        if (animation_frame > track->unk18) animation_frame = track->unk18;
        func_global_asm_80614644(echo, track, animation_frame);
    }
    renderActor(echo, 0);
    echo->object_properties_bitfield &= ~0x200;
    func_global_asm_80614A64(echo);
}
#endif

/* Preserve the original input wrapper; examine real pads before injecting.
 * The caller still derives button edges and applies game/cutscene masks. */
RECOMP_PATCH void func_global_asm_8060AC34(OSContPad *pads) {
    if (D_global_asm_807467C4 == 0 && D_global_asm_807ECD08 != 0) {
        osContGetReadData(pads);
#if YG_COOP_ENABLED
        autopilot_input(pads);
#endif
    }
    D_global_asm_807ECD08 = 0;
}

void yg_coop_before_map_load(void) {
#if YG_COOP_ENABLED
    hide_echo("map-load");
    spawn_settle_frame = frame;
    spawn_blocked = 1;
    /* The loader resets the entire map heap (patches_heap.c:80610350).
     * Do not free model/bone buffers ahead of the loader's synchronization:
     * the previously queued display list may still reference those buffers. */
    bzero(echo_models, sizeof(echo_models));
    echo = &echo_models[0];
    model_ready = 0;
    cached_models = 0;
    failed_models = 0;
    cache_full_models = 0;
    logged_kong_fallback = 0;
    logged_puppet_kong = logged_remote_kong = -1;
    pose_reset = 1;
    failed = 0;
    logged_animation_fallback = 0;
    logged_idle_proxy = 0;
    history_map = -1;
#endif
}

void yg_coop_frame(void) {
#if YG_COOP_ENABLED
    YGSample *sample;
    Actor *player;
    AnimationStateUnk0 *track;
    int held;
    frame++;
#if !YG_COOP_AUTOPILOT
    if (frame == 1) recomp_printf("YG-COOP autopilot disabled-by-build\n");
#endif
    net_mode = yg_coop_net_mode(); /* 4B: the room sets the role asynchronously; re-read every frame. */
    if (net_mode == YG_NET_ROLE_A || net_mode == YG_NET_ROLE_B) net_role_seen = 1;
    else if (net_mode == YG_NET_ERROR && net_role_seen) {
        autopilot_disabled = 1;
        if (!relay_lost_logged) {
            relay_lost_logged = 1;
            recomp_printf("YG-COOP relay-lost frame=%u\n", frame);
        }
    }
    autopilot_frame();
    if (history_map != (s32)current_map) {
        /* Map-load entry point must have disposed the old model first. No
         * stale map allocation is ever dereferenced on a missed transition. */
        hide_echo("map-change");
        spawn_settle_frame = frame;
        spawn_blocked = 1;
        model_ready = 0;
        cached_models = 0;
        failed_models = 0;
        cache_full_models = 0;
        logged_kong_fallback = 0;
        logged_puppet_kong = logged_remote_kong = -1;
        pose_reset = 1;
        failed = 0;
        history_map = current_map;
    }
    held = spawn_held();
    if (net_mode) {
        network_frame();
        return; /* Echo history, 90-frame lag and echo logs are untouched below. */
    }
    if (!valid_player() || (D_global_asm_8076A0B1 & 1)) {
        hide_echo("player-unavailable");
    } else {
        player = gPlayerPointer;
        track = player->animation_state->unk0;
        sample = &history[write_index];
        sample->moving = 0;
        if (samples) {
            YGSample *previous = &history[(write_index + YG_HISTORY - 1) % YG_HISTORY];
            f32 dx = player->x_position - previous->position.x;
            f32 dz = player->z_position - previous->position.z;
            sample->moving = dx * dx + dz * dz > 0.01f;
        }
        capture_sample(sample, player, track);
        write_index = (write_index + 1) % YG_HISTORY;
        if (samples < YG_HISTORY) samples++;
        if (samples == YG_HISTORY) {
            /* exactly 90 samples behind; explicit copy: a struct assignment this size compiles to a bare memcpy the recompiler cannot link */
            memcpy_recomp(&delayed, &history[write_index], sizeof(delayed));
            if (!held && (!model_ready || echo_kong == sample->kong) && !failed) {
                if (visible) {
                    recomp_printf("YG-COOP despawn frame=%u map=%d reason=kong-change\n", frame, history_map);
                    visible = 0;
                }
                model_ready = create_echo(sample->kong);
                failed = !model_ready;
            }
            if (!held && model_ready && delayed.map == current_map) {
                if (!visible) {
                    visible = 1;
                    recomp_printf("YG-COOP spawn frame=%u map=%d kong=%d lag=90 approach=private-model\n",
                                  frame, current_map, echo_kong);
                    recomp_printf("YG-COOP puppet-offset enabled=%d right=%.0f units frame=echo-facing\n",
                                  YG_COOP_PUPPET_OFFSET, YG_OFFSET_RIGHT);
                }
            }
        }
    }
    if (frame % 60 == 0) {
        f32 x = 0, y = 0, z = 0;
        s32 kong = -1;
        if (valid_player()) {
            x = gPlayerPointer->x_position;
            y = gPlayerPointer->y_position;
            z = gPlayerPointer->z_position;
            kong = current_character_index[0];
        }
        if (visible) {
            tuple_f position = echo_position();
            recomp_printf("YG-COOP frame=%u map=%d player=<%.2f,%.2f,%.2f> puppet=<%.2f,%.2f,%.2f> echo=<%.2f,%.2f,%.2f> lag=90 kong=%d source-animation=%d\n",
                          frame, current_map, x, y, z, position.x, position.y, position.z,
                          delayed.position.x, delayed.position.y, delayed.position.z, kong, delayed.animation);
        } else {
            recomp_printf("YG-COOP frame=%u map=%d player=<%.2f,%.2f,%.2f> puppet=none lag=90 kong=%d\n",
                          frame, current_map, x, y, z, kong);
        }
    }
#endif
}

Gfx *yg_coop_draw_echo(s32 camera, Actor *actor, Gfx *dl, s32 pass) {
#if YG_COOP_ENABLED
    if (spawn_held()) return dl;
    if (visible && model_ready && actor == gPlayerPointer && valid_player() &&
        history_map == (s32)current_map && !(D_global_asm_8076A0B1 & 1)) {
        pose_echo();
        if (net_mode && (logged_puppet_kong != echo_kong || logged_remote_kong != delayed.kong)) {
            recomp_printf("YG-COOP puppet-kong frame=%u remote-kong=%u drew=%u model=%u\n",
                          frame, delayed.kong, echo_kong, kong_models[echo_kong]);
            logged_puppet_kong = echo_kong;
            logged_remote_kong = delayed.kong;
        }
        /* Re-enters once with echo. The actor==gPlayerPointer test prevents
         * recursion, and no gameplay update function sees this object. */
        dl = func_global_asm_80630DCC(camera, echo, dl, pass);
    }
#endif
    return dl;
}

Gfx *yg_coop_framebuffer_begin(Gfx *dl) {
#if YG_COOP_ENABLED
    /* Reset on every list, since the RT64 extended setting persists. */
    gEXSetRenderToRAM(dl++, 0);
#if YG_COOP_FRAME_DUMP
    static const u8 dump_seconds[] = { 5, 15, 25, 33 };
    if (walk_started && !autopilot_disabled && (net_mode || visible) && dump_index < 4 &&
        current_map == MAP_TRAINING_GROUNDS && !is_cutscene_active &&
        !(D_global_asm_8076A0B1 & 1) && !(global_properties_bitfield & 2) &&
        osGetTime() - walk_start >= OS_USEC_TO_CYCLES((u64)dump_seconds[dump_index] * 1000000) &&
        (dump_index != 3 || (autopilot_step == YG_STAND && (net_mode || frame - stand_frame >= YG_LAG) &&
         osGetTime() - stand_start >= OS_USEC_TO_CYCLES(3000000ULL)))) {
        u32 seconds = dump_seconds[dump_index];
        gEXSetRenderToRAM(dl++, 1);
        yg_coop_request_frame(D_global_asm_8076A050[D_global_asm_807444FC],
                             D_global_asm_80744470[D_global_asm_807444FC],
                             ((u32)D_global_asm_80744490 << 16) | (u16)D_global_asm_80744494,
                             seconds);
        recomp_printf("YG-COOP capture-request walk-seconds=%u frame=%u mode=%s\n",
                      seconds, frame, autopilot_step == YG_STAND ? "stand" : "walk");
        dump_index++;
    }
#endif
#endif
    return dl;
}
