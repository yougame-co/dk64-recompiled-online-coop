#ifndef YG_COOP_WIRE_H
#define YG_COOP_WIRE_H

/* YGSample's N64 ABI, not a host struct. Multi-byte values are big endian.
 * Explicit offsets are checked in yg_coop.c and exercised with MEM_* in tests.
 * Bytes 30..31 and 49..51 are padding and are always zero on the wire. */
enum {
    YG_SAMPLE_SIZE = 68,
    YG_S_MAP = 0, YG_S_POSITION = 4, YG_S_ROTATION = 16,
    YG_S_ANIMATION = 22, YG_S_LOGICAL = 24, YG_S_MODEL = 26,
    YG_S_HANDS = 28, YG_S_ANIMATION_FRAME = 32,
    YG_S_ANIMATION_START = 36, YG_S_ANIMATION_END = 40,
    YG_S_TICK = 44, YG_S_MOVING = 48, YG_S_SCALE = 52,
    YG_S_KONG = 64, YG_S_CONTROL = 65, YG_S_NET_FLAGS = 66,
    YG_S_SPAWN = 67, /* Network-only spawn generation; ABI stays 68 bytes. */
    YG_NET_READY = 1, YG_NET_WALK = 2,
    YG_NET_HAS_SAMPLE = 1, YG_NET_FRESH = 2, YG_NET_LIVE = 4,
    YG_NET_ECHO = 0, YG_NET_ROLE_A = 1, YG_NET_ROLE_B = 2,
    YG_NET_ERROR = 3
};
#endif
