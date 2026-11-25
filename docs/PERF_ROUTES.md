# Performance routes and A/B runs

Hand-played comparisons are noisy: a different spot on the map, a camera turn or a load in one run and not the other can move the
numbers more than the change being tested. A route is a script that plays the same thing every time, unattended, and marks the
stretches to measure ("scenes"). `tools/perf/ab_run.ps1` runs it several times for each of two settings and
`tools/perf/perf_report.py` compares the logs.

This needs the developer build (docs/BUILDING.md) and a save to load. Python comes from `tools\.venv` (or `python` on PATH).

## 1. The route

`tools/perf/routes/fefw_map.tt` boots the game, presses through the title menu into save slot 000, waits for the map to settle,
then measures two scenes:

- `map_still`: 20 s with nothing moving: the cost of drawing the scene.
- `map_pan`: 11 s with the cursor panning the camera right, up, left and down: camera turns, texture streaming, culling.

Waits are in host time (`@sleep`), not frames, so the route covers the same game time at any frame rate.

**Make it fit your save once.** Put the save you want to measure in slot 000 (a battle map with the units on it works well), then
run the route by hand and watch it:

```
set NXRT_TEST_SCRIPT=C:\ObeliskChamber-src\tools\perf\routes\fefw_map.tt
set NXRT_TEST_SHOT_DIR=C:\ObeliskChamber-src\work\perf\shots
game-build\fefw\build\release\ObeliskChamber.exe --ini C:\ObeliskChamber-run\ObeliskChamber.ini
set NXRT_TEST_SCRIPT=
```

The screenshots `01_title` … `05_end` show where each press landed. If the title needs another press, or the slot screen sits
elsewhere, change the `press` and `@sleep` lines (the grammar is in `runtime/include/nxrt/tt_script.h`). The run ends by itself at
`@pass`; the log says `tt: run ended: pass`.

Other routes are new `.tt` files: any number of `@log SCENE start <name>` / `@log SCENE end <name>` pairs.

### Record your own route instead

If the menus need other presses than the template's, play the route yourself once and turn the recording into a script. Use a
controller (mouse movement is not carried over), and start from the launch so the recording includes the menus:

```
set NXRT_TEST_RECORD=C:\ObeliskChamber-src\work\perf\rec\input.nxin
set NXRT_TEST_OUT=C:\ObeliskChamber-src\work\perf\rec
game-build\fefw\build\release\ObeliskChamber.exe --ini C:\ObeliskChamber-run\ObeliskChamber.ini
set NXRT_TEST_RECORD=
set NXRT_TEST_OUT=
```

Play to the map, then do what you want measured (stand still for a while, pan the camera, open a menu), noting roughly when each
part starts and ends (seconds from the launch), and close the window. Then:

```
tools\.venv\Scripts\python.exe -I tools\perf\record_to_route.py work\perf\rec -o tools\perf\routes\my_route.tt ^
    --scene map_still:60:80 --scene map_pan:80:95
```

Each button press and stick movement happens at the same time from the launch as in your recording (`@at`, an absolute time, so small delays never add up), so the route repeats what you
did at any frame rate. `--scene name:start:end` marks the measured parts (seconds from the launch); without it, add the
`@log SCENE start/end` lines by hand. Run it with `-Route tools\perf\routes\my_route.tt`.

Free movement drifts a little between runs (a load half a second slower, frame timing), enough to miss a spot over a minute of
walking. For a scene in a particular place, save the game there and record only the menus, standing still and turning the camera;
on a battle map, the tile cursor repeats exactly.

**Start the walk when the save has loaded.** A recorded route plays each press at its time from the launch, so on another PC (or after
a change that makes boot or the load slower) the first steps of the walk happen behind the loading screen and are lost: the character
starts in the middle of the route and never reaches the places it was meant to measure. `tools/perf/sync_route.py` rewrites a recording
so that its gameplay waits for the load instead (`@loaded`: a load began, ended and nothing loaded for 2 s), then plays the recorded
times counted from there (`@origin`):

```
tools\.venv\Scripts\python.exe -I tools\perf\sync_route.py tools\perf\routes\my_route.tt -o tools\perf\routes\my_route_sync.tt
```

The walk starts at the recording's first stick step (half a second before it; `--from <s>` picks another point). `--menu <file>` replaces
the recording's menu presses with your own steps, for a PC whose title needs other presses (with `[fefw] title_continue` on, the cursor
already sits on Continue: no DOWN). `--shots 10` adds a screenshot every 10 s of the walk (`walk_000.png` ...) to check where the
character went; leave it off for timing runs, since a screenshot costs a frame. `@loaded` needs `[display] unpaced_loading` on (the
default): that is what tells a loading screen apart. Add `draws=100` (`@loaded 2000 draws=100`) to also wait for a frame of the area
itself: a slow load can stop for a while on a loading tip that presents steadily, which the replay paces like a menu, and the quiet time
alone can end there (on the laptop this happened when the title screen came up with its heavier background).

The menus of a recorded route depend on load times staying about the same: keep the same save, cache and settings, and leave a few
seconds of margin before each press in the recording (don't rush through the menus).

## 2. What a scene measures

At `SCENE end` the log gets one line for exactly the frames between the two marks:

```
hitch: scene 'map_still': 2340 frames in 20.0 s: mean 117.0 fps, 1% low 92.3 fps, worst 14.1 ms, 0 over 25 ms, 0 over 50 ms,
       0 over 2x the median (8.40 ms); render thread ms/frame: replay 5.62, present 2.10 (limiter 1.90), idle 0.40
```

`[nvn] replay_stats = 600` (render-thread cost per draw) and `[prof] gpu_stats = 600` (GPU time per frame) add their own lines; the
report uses the ones that fall inside the run.

Three more lines follow every scene, per frame and by the function and line that recorded them: the full memory barriers, the render
pass ends, and the passes reopened on the same targets (by what ended them: a pass split only for a layout change, copy or barrier).
(`hitch: scene 'map_move': full barriers by caller ...`, `... pass ends by caller ...`, `... passes reopened on the same targets by
caller ...`). They are always there; no switch is needed.

## 3. A/B runs

```
pwsh -File tools\perf\ab_run.ps1 -B "NXRT_NVN_REAPER=false" -Names on,off
```

- `-A` and `-B` are environment variables, `NAME=value` separated by `;`. Every ini key has one: `NXRT_<SECTION>_<KEY>`
  (runtime/docs/CONFIG.md); it overrides the ini for that run only. Leave one side empty for "the ini as it is".
- `{run}` in a value becomes a text new to every run. `NXRT_NVN_SPIRV_SALT={run}` on both sides makes every run compile its
  shaders from scratch, as on a player's first start, without deleting any cache (the way to test shader compile stutter).
- `-Runs 3` (default) runs each side three times, alternating A B, B A, A B, so slow drift over the session falls on both.
- Logs, screenshots and `report.md` go to `work\perf\<date-time>`. Each run takes about two minutes (plus one warm-up run); don't use the PC meanwhile.
- `-Exe`, `-Ini`, `-Route`, `-Out` point elsewhere; `-TimeoutS` (600) stops a run whose route got stuck.
- `-Warmup 1` (default) runs side A once before the measured runs and leaves it out: the first start after a build or a reboot finds
  the files, the driver's caches and the CPU clocks cold, and it was the outlier of nearly every A/B. `-Warmup 0` turns it off.
- A run whose route did not pass (it failed, or `-TimeoutS` stopped it) is left out of the comparison, and the report names it. A
  run that passed but has no line for one of the scenes is named with a warning: the route went somewhere else in that run.
- The report compares each side's numbers as the median of its runs, with the range in brackets.
- Pictures: the game writes a small copy of every screenshot next to it (`<shot>.png.thumb`, `[gfx] screenshot_thumb`, set by the
  script), and `tools/perf/shot_diff.py` compares them, A against B and each side's two runs against each other (the run-to-run
  noise: people, the clock, particles). The report's "Pictures" table marks a screenshot that differs by more than twice the noise,
  and `diffs\<shot>.png` shows A, B and the difference side by side. A route with `@shot` lines during the walk (`sync_route.py
  --shots`) checks both the picture and that every run went the same way.
- `-ExeB` runs side B with another build, to compare two builds with the same settings (for example master against a pull
  request). Copy the whole `game-build\fefw\build\release` folder of the first build somewhere else before building the second:
  `-Exe C:\ab\master\ObeliskChamber.exe -ExeB game-build\fefw\build\release\ObeliskChamber.exe -Names master,pr`.

## 3b. Potato mode and the CPU limit

To see what a small PC gets, or where a change to the CPU side pays, the CPU has to be the limit:

- `[sched] cpu_limit = 4` holds the game to 4 physical cores (`cpu_limit_smt = false`: one thread each, `cpu_limit_cores = slow`: the
  E-cores of a hybrid CPU). The runtime's thread pools and automatic choices follow the cores kept. The log's `sched` line names them.
- `[fefw] fps240 = 1` (in the ini only; F1 > Frame rate then shows "Custom") lifts the game's own 120 fps target, so at a low resolution the frame rate rises
  until the CPU stops it. Set the frame limit to Unlimited and vsync off.
- A low output size keeps the GPU out of the way (for example `[fefw] output = 720p`).
- Optional caps for memory: `[gfx] residency_test_budget_mb` (video memory a smaller card would have) and `[nvn] tex_prep_mb` (texture preparation).

As environment variables for `ab_run.ps1`: `NXRT_SCHED_CPU_LIMIT=4;NXRT_FEFW_FPS240=1` on both sides, and the change under test on one.

What to read in the log:
- `session:` lines (`[diag] session_s`, default every 5 minutes, and at exit): the whole session's mean fps, 1% and 0.1% lows, longest
  frame and frames over 100 ms.
- `nvn.gpustats` (`[prof] gpu_stats = 600`): GPU time per frame, and the `full barriers by caller` / `pass ends by caller` lines that
  say which code records the barriers and ends the render passes.

## 4. The report

`report.md` has one table per scene: each metric's median over the runs of each side, the spread (min..max) when there were
several runs, and the change from A to B (marked better or worse past 3%). Above it, each run's setup (output size, DLSS mode and
model, HDR, which switches were set by the environment) and warnings: a run whose setup differs from the others, Streamline missing,
a build without the newest log lines, a route that failed.

The report also works on any logs, scripted or not:

```
python -I tools\perf\perf_report.py ObeliskChamber.log
python -I tools\perf\perf_report.py --a a1.log a2.log --b b1.log b2.log --names off on
```

Without scenes it uses the steady 10 s windows of the hitch monitor (all but the first, worst frame under 50 ms).
