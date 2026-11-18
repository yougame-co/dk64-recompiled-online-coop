#!/usr/bin/env python3
"""Compile the actual patch's hold helpers against game-state stubs; no ROM."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'patches/yg_coop.c'
text = source.read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:int|void|Gfx \*)\s*' + name +
                      r'\([^\n]*\) \{.*?^\}', text, re.M | re.S)
    assert match, name
    return match.group()


# Check each consumer, including callbacks which can observe a new cutscene
# between game updates. These checks complement the executable state tests.
for name, guard in (('create_echo', 'if (spawn_held()) return 0;'),
                    ('pose_echo', 'if (spawn_held()) return;'),
                    ('yg_coop_draw_echo', 'if (spawn_held()) return dl;')):
    body = function(name).split('{', 1)[1].replace('#if YG_COOP_ENABLED', '').strip()
    assert body.startswith(guard), name
network = function('network_frame')
assert network.index('yg_coop_net_step(') < network.index('spawn_peer_frame();')
assert network.index('spawn_peer_frame();') < network.index('if (spawn_held()) return;')
assert network.index('if (spawn_held()) return;') < network.index('create_echo(')
assert 'net_local.map = -1;' in network
update = function('yg_coop_frame')
assert update.index('held = spawn_held();') < update.index('network_frame();')
assert 'if (!held && (!model_ready || echo_kong == sample->kong) && !failed)' in update
assert 'if (!held && model_ready && delayed.map == current_map)' in update
resets = []
for body in (function('yg_coop_before_map_load'), update):
    reset = re.search(r'spawn_settle_frame = frame;\s+spawn_blocked = 1;', body)
    assert reset
    resets.append(reset.group())
create = function('create_echo')
assert re.search(r'recomp_printf\("YG-COOP model-load-start[^;]+;\s*'
                 r'if \(!func_global_asm_806134B4\(', create)
assert 'func_global_asm_806136B4' not in function('spawn_held')

default = re.search(r'#define YG_COOP_SPAWN_SETTLE_FRAMES (\d+)', text).group(1)
state = text[text.index('static u32 spawn_settle_frame;'):
             text.index('static s32 spawn_peer_map = -1;') + len('static s32 spawn_peer_map = -1;')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int32_t s32;
#define MAP_MAIN_MENU 80
#define YG_NET_LIVE 1
static u32 frame, global_properties_bitfield, samples, write_index, net_status;
static s32 current_map, history_map;
static u8 is_cutscene_active, D_global_asm_8076A0B1, visible;
static int player_valid, menu_mode;
static struct { s32 map; } delayed;
static int starts, ends, despawns, assertions;
static char last_start[256], last_end[256];
static int valid_player(void) { return player_valid; }
static int gameIsInMainMenuMode(void) { return menu_mode; }
static void recomp_printf(const char *format, ...) {
    char line[256]; va_list args;
    va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (strstr(line, "spawn-hold start")) { ++starts; strcpy(last_start, line); }
    if (strstr(line, "spawn-hold end")) { ++ends; strcpy(last_end, line); }
    if (strstr(line, "despawn")) ++despawns;
}
#define CHECK(x) do { ++assertions; assert(x); } while (0)
'''
tests = r'''
static void reset(void) {
    frame = 0; current_map = history_map = 7;
    is_cutscene_active = D_global_asm_8076A0B1 = visible = 0;
    global_properties_bitfield = samples = write_index = net_status = 0;
    player_valid = 1; menu_mode = 0; delayed.map = -1;
    spawn_settle_frame = spawn_hold_start = spawn_hold_active = 0;
    spawn_blocked = 1; spawn_peer_map = -1;
    starts = ends = despawns = 0;
}
static int at(u32 tick) { frame = tick; return spawn_held(); }
static void settled(void) { CHECK(at(0)); CHECK(!at(60)); }
int main(void) {
    CHECK(YG_COOP_SPAWN_SETTLE_FRAMES == 60);
    reset();
    for (u32 tick = 0; tick < 60; ++tick) {
        CHECK(at(tick)); CHECK(spawn_held()); CHECK(spawn_held());
    }
    CHECK(!at(60)); CHECK(!spawn_held()); CHECK(starts == 1 && ends == 1);
    CHECK(strstr(last_start, "reason=settle"));
    CHECK(strstr(last_end, "held-frames=60"));

    /* A render callback sees a cutscene without another update: hide only,
     * then count a full 60 updates from its end, even after a long scene. */
    reset(); settled(); visible = 1; samples = 91; write_index = 8;
    is_cutscene_active = 1; CHECK(spawn_held());
    CHECK(!visible && samples == 0 && write_index == 0 && despawns == 1);
    CHECK(strstr(last_start, "reason=cutscene"));
    CHECK(at(300)); is_cutscene_active = 0;
    CHECK(at(301)); CHECK(at(360)); CHECK(!at(361));
    CHECK(starts == 2 && ends == 2);
    /* Sample history can keep filling during an existing hold. */
    is_cutscene_active = 1; CHECK(at(400)); samples = 12; write_index = 12;
    CHECK(at(401)); CHECK(samples == 12 && write_index == 12);
    is_cutscene_active = 0; CHECK(at(402));
    is_cutscene_active = 1; CHECK(at(440));
    is_cutscene_active = 0; CHECK(at(441)); CHECK(at(500)); CHECK(!at(501));
    CHECK(starts == 3 && ends == 3);

    /* Every local blocker, including a same-map reload and a missed map hook. */
    for (int condition = 0; condition < 6; ++condition) {
        reset(); settled(); visible = 1;
        switch (condition) {
        case 0: D_global_asm_8076A0B1 = 1; break;
        case 1: global_properties_bitfield = 2; break;
        case 2: current_map = MAP_MAIN_MENU; history_map = current_map; break;
        case 3: menu_mode = 1; break;
        case 4: player_valid = 0; break;
        case 5: current_map = 153; break;
        }
        CHECK(at(100)); CHECK(!visible); CHECK(at(160));
        CHECK(strstr(last_start, condition == 0 || condition == 5 ?
                     "reason=map-load" : "reason=menu"));
        D_global_asm_8076A0B1 = 0; global_properties_bitfield = 0;
        current_map = history_map = 153; menu_mode = 0; player_valid = 1;
        CHECK(at(161)); CHECK(at(220)); CHECK(!at(221));
        CHECK(starts == 2 && ends == 2 && despawns == 1);
    }
    reset(); settled(); frame = 100; map_load_reset();
    CHECK(spawn_held()); CHECK(at(159)); CHECK(!at(160));
    frame = 200; map_change_reset();
    CHECK(spawn_held()); CHECK(at(259)); CHECK(!at(260));

    /* The friend arrives later; peer loss/rejoin also restarts the interval. */
    reset(); settled(); frame = 500; net_status = YG_NET_LIVE; delayed.map = 7;
    spawn_peer_frame(); CHECK(spawn_held());
    for (u32 tick = 501; tick < 560; ++tick) {
        frame = tick; spawn_peer_frame(); CHECK(spawn_held());
    }
    CHECK(!at(560)); CHECK(starts == 2 && ends == 2);
    frame = 600; net_status = 0; spawn_peer_frame();
    frame = 700; net_status = YG_NET_LIVE; spawn_peer_frame();
    CHECK(spawn_held()); CHECK(at(759)); CHECK(!at(760));
    frame = 800; delayed.map = -1; spawn_peer_frame();
    frame = 900; delayed.map = 7; spawn_peer_frame();
    CHECK(spawn_held()); CHECK(at(959)); CHECK(!at(960));

    /* The peer arrives first in map 153, while this seat is still in menus. */
    reset(); current_map = history_map = MAP_MAIN_MENU; player_valid = 0;
    frame = 10; net_status = YG_NET_LIVE; delayed.map = 153; spawn_peer_frame();
    CHECK(spawn_held()); frame = 100; map_load_reset();
    current_map = history_map = 153; player_valid = 1; is_cutscene_active = 1;
    map_change_reset(); CHECK(spawn_held());
    CHECK(at(1000)); is_cutscene_active = 0;
    CHECK(at(1001)); CHECK(at(1060)); CHECK(!at(1061));
    CHECK(starts == 1 && ends == 1);
    /* Cutscene ended first, but the peer changes to this map later. */
    frame = 1100; delayed.map = 8; spawn_peer_frame();
    frame = 1200; delayed.map = 153; spawn_peer_frame();
    CHECK(spawn_held()); CHECK(at(1259)); CHECK(!at(1260));

    /* Unsigned elapsed arithmetic survives the game-frame counter wrapping. */
    reset(); frame = UINT32_MAX - 29; CHECK(spawn_held());
    CHECK(at(29)); CHECK(!at(30)); CHECK(strstr(last_end, "held-frames=60"));
    printf("PASS spawn hold: %d assertions, actual C helpers; consumer gates checked\n", assertions);
}
'''
code = prefix + '\n#define YG_COOP_SPAWN_SETTLE_FRAMES ' + default + '\n' + state
code += '\n' + function('hide_echo') + '\n' + function('spawn_held')
code += '\n' + function('spawn_peer_frame')
code += '\nstatic void map_load_reset(void) {' + resets[0] + '}\n'
code += '\nstatic void map_change_reset(void) {' + resets[1] + '}\n' + tests
with tempfile.TemporaryDirectory(prefix='coop-hold-') as tmp:
    c = Path(tmp) / 'hold.c'
    exe = Path(tmp) / 'hold-test'
    c.write_text(code)
    subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
