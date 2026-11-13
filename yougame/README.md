# Online co-op for Donkey Kong 64: Recompiled (YouGame fork)

This branch is a fork of [Rainchus/Donkey-Kong-64-Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled)
at commit `3b0c36d3fbfeb0b5d2c47923f35cf84b31edcbf7`. All credit for the recompilation goes to its authors and to the
projects it builds on (N64Recomp, RT64, the DK64 decompilation). The licence is unchanged: GPL-3.0, see `LICENSE`.

It adds an experimental online co-op mode: two players each run their own game and see the other player's Kong
(position, animation and which Kong) in the same level. Nothing else is shared yet: no items, enemies or progress.
It is played through [YouGame](https://yougame.co), whose desktop app pairs the two players and relays the packets.

**You need your own copy of Donkey Kong 64 (USA). No game data is in this repository or in its releases.**

## What the fork changes

| Where | What |
|---|---|
| `patches/yg_coop.c`, `patches/yg_coop.h`, `include/yg_coop_wire.h` | The co-op patch: sends the local Kong's state, draws the remote Kong as a puppet. |
| `include/yg_coop_relay.hpp`, `include/yougame_net.h` | A relay worker that talks to the YouGame desktop app over stdin/stdout (ticket, queue, room). |
| `include/yg_coop_net.hpp`, `include/yg_coop_smooth.hpp`, `include/yg_coop_capture.h` | A direct UDP transport used in tests, remote-motion smoothing, test frame capture. |
| `src/main/yougame_data.hpp`, `src/main/main.cpp` | Uses the data folder YouGame gives the game; `--rom <path>` takes the player's own verified copy from the app. |
| `src/game/recomp_api.cpp`, `patches/*` (small edits), `CMakeLists.txt` | Hooks for the patch, Windows fixes, an option to build from already generated code. |
| `yougame/submodule-patches/` | Our small changes to the `lib/RecompFrontend` and `lib/rt64` submodules, as patches to apply after `git submodule update --init --recursive`. |
| `yougame/yougame-native.json` | The launch manifest the YouGame app reads. |

## Building

Follow `BUILDING.md`. After fetching submodules, apply the two patches:

```
git -C lib/RecompFrontend apply ../../yougame/submodule-patches/RecompFrontend.patch
git -C lib/rt64 apply ../../yougame/submodule-patches/rt64.patch
```

## State

Experimental and not tested by players yet. Known gaps: the build starts itself in the Training Grounds on a fresh
file (test scaffolding that still needs removing), controls have not been tried by a person, and only the other
player's Kong is shared.
