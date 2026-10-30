# Submitting measurements, and how they are checked

A number from someone else's console is only useful if it can be compared with ours, and only trustworthy if it can be
checked. This file says what to send, what is deliberately **not** collected, and how a submission is verified.

## Nothing is collected automatically

gc3ds sends nothing anywhere on its own. There is no analytics, no crash upload, no phone-home: the only network code
in the program is the dev relay (`core/host3ds/relay.c`), which runs **only** when you put your own `relay.txt` on the
card and point it at **your own** server (`docs/REMOTE_RUNS.md`). Nobody else's runs reach us unless they choose to
attach a log to an issue.

If that ever changes it will be opt-in, off by default, documented here before it ships, and it will never include the
things listed under "Never collected".

## What to send

Open an issue with the **Measurement** template (`.github/ISSUE_TEMPLATE/measurement.md`) and attach the `perf.csv`
of each run. A submission is one comparison:

| | |
|---|---|
| **Console** | New 3DS, New 3DS XL or New 2DS XL (they differ: clocks and heat), and the CFW you boot through |
| **Build** | the commit you built, and the build id the relay prints (`relay.sh push`) if you used one |
| **Scene** | the game, the state and whether any input was scripted. **A state that takes no input, every time** |
| **Arms** | the baseline and the change, **two runs each**, from one session on one build |
| **Numbers** | frames a second (`core/tools/runs.py --skip 80`), not `speed_pct` alone - see below |
| **Logs** | the `perf.csv` of each run, unedited |

`python3 core/tools/runs.py --skip 80 <logs>` prints the lines to paste; `core/tools/clock_split.py` and
`core/tools/port_cost.py` add where the emulated clock went and what the ports cost.

## Never collected, never attached

- **Game data of any kind**: no disc images, no save states of commercial games, no assets, no disc hashes, no
  screenshots of copyrighted material beyond what a bug needs. (Repository rule 1; a save state of a commercial game
  is game data.)
- **Anything identifying you or your console**: no serials, no friend codes, no MAC or IP addresses, no account names,
  no file paths from your machine. A `perf.csv` contains none of these - check before attaching if you edited the
  build.
- **Secrets**: your `relay.txt` holds a token. It stays on your machine; it is in `.gitignore` for a reason.

## Why the rules about scenes, builds and repeats exist

Each of these was learned the hard way, and a submission that ignores them cannot be used:

- **The scripted walk cannot judge a change that shifts the guest's timeline.** The presses are applied when the guest
  polls the pad, so a moved clock lands the poll in another field and Link walks a different path: the arms then render
  different scenes, which differed by up to 50% in draws a picture and produced swings of ±20% that had nothing to do
  with the change (REDESIGN_CANDIDATES C41, jobs 1423-1432). Use a state that takes no input, or prove the arms drew
  the same scene (`draws_s / (fields_s / 2)` at matched `emu_s`).
- **`speed_pct` is the emulated clock, not the frame rate.** It is `cycles / real second / 486 MHz`, and native
  stand-ins and ports charge the clock for work they did natively - so a change to those charges moves `speed_pct`
  without the game getting faster. Frames a second is what the game delivers.
- **The device drifts ~2% between days** and a rebuild moves a baseline by up to ~0.8%. A baseline from last week
  against a change from today measures the weather.
- **Two runs an arm.** On a state that takes no input they agree to ~0.1%, which is how a 0.5% effect becomes
  believable. Single runs have reversed on repeat in this project more than once.

## How a submission is checked

Anyone can check these; they are arithmetic over the attached logs, and they are what we run before quoting a number.

1. **Internal consistency.** `speed_pct` ≈ `fields_s / 59.94`; `fields_s / 2` ≈ frames a second while the game is
   locked to two fields a picture (`# new frames hist`); the clock sources in `# clock 10s` sum to 100%; the PMU's
   cycles a second are near 804 M times the share of the wall clock the run covers.
2. **The arms differ only where they should.** The option columns of every row record the settings; the two arms must
   differ in the one switch under test and nothing else, and carry the same build.
3. **The scene matches.** Draws and vertices a picture at matched emulated seconds agree between arms (this is the
   check that exposed C41). If they do not, the arms were not the same scene and the comparison is void.
4. **The spread is reported, not hidden.** Two runs an arm, both quoted. A difference smaller than the spread is
   reported as "below the noise floor" - that is a result, not a failure.
5. **Nothing was warm-up.** Score from 80-100 s of run time: the first seconds load a state, compile code and settle.

A submission that passes these is quoted with its job or issue number in `docs/research/MEASUREMENTS.md`, with your
name if you want it there, and a default only changes on top of it.

## If you want to run a lot of measurements

The relay (`docs/REMOTE_RUNS.md`) is a small HTTP server you run yourself; `tools/relay-server/server.js` is a
reference with no dependencies. It gives you a build store, a job queue and logs streamed off the console, which is
how the numbers in this repository are produced - one console, hours of unattended runs. It is per-person by design:
your token, your server, your data. There is no central collection point, and we are not asking for one.
