#!/usr/bin/env python3
"""Validate and summarize X2's probe-only relay log. Cycles are measured; means/ratios are derived."""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics


def score(text):
    head = re.search(r"# l2lock: id ([0-9a-f]+) control@100 ([0-9a-f]+) aux@104 ([0-9a-f]+)", text)
    if not head:
        raise ValueError("missing controller snapshot")
    cache_id, control, aux = (int(x, 16) for x in head.groups())
    if cache_id != 0x410000c9 or control != 1:
        raise ValueError("unexpected controller")
    regs = {}
    for phase, offset, value in re.findall(r"# l2lock: (saved|partition|rejected|restored) ([0-9a-f]{3}) ([0-9a-f]{8}) read", text):
        offset, value = int(offset, 16), int(value, 16)
        if offset in regs.setdefault(phase, {}):
            raise ValueError("duplicate register phase (multiple runs?)")
        regs[phase][offset] = value
    offsets = set(range(0x900, 0x940, 4))
    for phase in ("saved", "restored"):
        if set(regs.get(phase, {})) != offsets:
            raise ValueError("incomplete " + phase + " register snapshot")
    if regs["saved"] != regs["restored"] or "# l2lock: restore VERIFIED attempted-mask 0000" not in text:
        raise ValueError("register restoration not verified")
    if any(regs["saved"].values()):
        raise ValueError("pre-existing lockdown; this probe declines it")
    if "# l2lock: end" not in text:
        raise ValueError("truncated probe")
    result = dict(cache_id=hex(cache_id), control=hex(control), aux=hex(aux),
                  registers={p: {f"{o:03x}": f"{v:08x}" for o, v in values.items()} for p, values in regs.items()},
                  restoration="verified", timings={})
    timings = {}
    for phase, trial, traffic, cyc in re.findall(
            r"# timing xcore l2lock-(baseline|partition|restored) trial(\d+) 1024K (alone|core2-streams): cyc ([0-9.]+)", text):
        key = (phase, traffic)
        samples = timings.setdefault(key, {})
        if trial in samples:
            raise ValueError("duplicate timing")
        samples[trial] = float(cyc)
    def summarize(phase):
        result["timings"][phase] = {}
        for traffic in ("alone", "core2-streams"):
            samples = timings.get((phase, traffic), {})
            if set(samples) != {"0", "1", "2"} or any(not math.isfinite(x) or x <= 0 for x in samples.values()):
                raise ValueError("missing/invalid trials: " + phase + " " + traffic)
            values = list(samples.values())
            result["timings"][phase][traffic] = dict(measured_cycles=values,
                derived_mean=statistics.mean(values), derived_se=statistics.stdev(values) / math.sqrt(3))
    summarize("baseline")
    if "APPLY FAILED" in text:
        if "# l2lock: end measured 0 restored 1" not in text or "partition" in regs:
            raise ValueError("inconsistent rejected transaction")
        if set(regs.get("rejected", {})) != offsets:
            raise ValueError("missing rejected readbacks")
        result["verdict"] = "disproved: requested master lockdown unavailable"
        return result
    if "# l2lock: end measured 1 restored 1" not in text:
        raise ValueError("probe did not finish measurement")
    half = 0x5555 if aux & (1 << 16) else 0x55
    wanted = {offset: half if 0x908 <= offset <= 0x91c else 0 for offset in offsets}
    if regs.get("partition") != wanted:
        raise ValueError("partition readback does not match half-way plan")
    for phase in ("partition", "restored"):
        summarize(phase)
    a = result["timings"]["baseline"]["core2-streams"]["derived_mean"]
    b = result["timings"]["partition"]["core2-streams"]["derived_mean"]
    result["derived_chase_speedup"] = a / b
    result["verdict"] = ("supported: chase approaches 30-40 cycles" if b <= 40 and a >= 90
                         else "disproved: no chase improvement" if b >= a * .95
                         else "unclear: improvement falls short of criterion")
    result["limitation"] = "AXI USER selector to CPU-core wiring unverified; no-change does not isolate bus from eviction"
    return result


def score_run(text):
    """Whole-run mode verifies the identical transaction around emulation, including the handled-error plant."""
    head = re.search(r"# l2lock run: id ([0-9a-f]+) control ([0-9a-f]+) aux ([0-9a-f]+)", text)
    if not head or "# l2lock run: begin VERIFIED" not in text or "# l2lock run: restore VERIFIED" not in text:
        raise ValueError("whole-run transaction did not verify")
    cache_id, control, aux = (int(x, 16) for x in head.groups())
    if cache_id != 0x410000c9 or control != 1:
        raise ValueError("unexpected whole-run controller")
    phases = {}
    for phase, offset, value in re.findall(r"# l2lock: (saved|partition|restored) ([0-9a-f]{3}) ([0-9a-f]{8}) read", text):
        offset, value = int(offset, 16), int(value, 16)
        if offset in phases.setdefault(phase, {}):
            raise ValueError("duplicate whole-run register")
        phases[phase][offset] = value
    offsets = set(range(0x900, 0x940, 4))
    for phase in ("saved", "partition", "restored"):
        if set(phases.get(phase, {})) != offsets:
            raise ValueError("incomplete whole-run snapshot")
    half = 0x5555 if aux & (1 << 16) else 0x55
    expected = {o: half if 0x908 <= o <= 0x91c else 0 for o in offsets}
    if any(phases["saved"].values()) or phases["partition"] != expected or phases["restored"] != phases["saved"]:
        raise ValueError("whole-run register comparison failed")
    return dict(restoration="verified", planted_handled_error="PLANTED handled error" in text,
                aux=hex(aux), registers={p: {f"{o:03x}": f"{v:08x}" for o, v in values.items()}
                                        for p, values in phases.items()})


def score_title_clock(text, windows=None):
    """Compare identical guest-clock work; fixed real-time windows can mix geometry and THP at different shares."""
    if windows is None:
        windows = {"cutscene": (25.0, 60.0), "later_geometry": (75.0, 92.0)}
    rows = list(csv.DictReader(line for line in text.splitlines() if line and not line.startswith("#")))
    try:
        rows = [{key: float(row[key]) for key in ("real_s", "emu_s", "gxwait_pct")} for row in rows]
    except (ValueError, KeyError, TypeError) as error:
        raise ValueError("invalid guest-clock CSV") from error
    if len(rows) < 2:
        raise ValueError("missing clock rows")
    for row in rows:
        if not all(math.isfinite(v) for v in row.values()):
            raise ValueError("invalid wait/clock")
    for prev, row in zip(rows, rows[1:]):
        if row["real_s"] <= prev["real_s"] or row["emu_s"] < prev["emu_s"]:
            raise ValueError("clock not monotonic (duplicate or multiple runs?)")
    def host_time(guest):
        for prev, row in zip(rows, rows[1:]):
            span = row["emu_s"] - prev["emu_s"]
            if span > 0 and prev["emu_s"] <= guest <= row["emu_s"]:
                return prev["real_s"] + (guest - prev["emu_s"]) / span * (row["real_s"] - prev["real_s"])
        raise ValueError("guest-clock window not covered")
    result = {}
    for name, (lo, hi) in windows.items():
        if hi <= lo:
            raise ValueError("empty window")
        start, end = host_time(lo), host_time(hi)
        active = 0.0
        for prev, row in zip(rows, rows[1:]):
            overlap = max(0.0, min(end, row["real_s"]) - max(start, prev["real_s"]))
            if overlap and not 0 <= row["gxwait_pct"] < 100:
                raise ValueError("invalid wait in selected window")
            active += overlap * (1 - row["gxwait_pct"] / 100)
        result[name] = dict(guest_seconds=[lo, hi], derived_host_seconds=[start, end],
                            derived_speed_pct=100 * (hi-lo) / (end-start),
                            derived_core0_speed_pct=100 * (hi-lo) / active)
    return result


def self_test():
    # Synthetic passing log, then mutations that each must be rejected. Never send plants to MMIO hardware.
    lines = ["# l2lock: id 410000c9 control@100 00000001 aux@104 32890000"]
    for phase in ("saved", "partition", "restored"):
        for offset in range(0x900, 0x940, 4):
            value = 0x5555 if phase == "partition" and 0x908 <= offset <= 0x91c else 0
            lines.append(f"# l2lock: {phase} {offset:03x} {value:08x} read")
        for trial in range(3):
            for traffic in ("alone", "core2-streams"):
                cyc = 35 if phase == "partition" or traffic == "alone" else 120
                lines.append(f"# timing xcore l2lock-{'baseline' if phase == 'saved' else phase} trial{trial} 1024K {traffic}: cyc {cyc}.000")
    lines += ["# l2lock: restore VERIFIED attempted-mask 0000", "# l2lock: end measured 1 restored 1"]
    good = "\n".join(lines)
    assert score(good)["derived_chase_speedup"] > 3
    plants = {"wrong partition": good.replace("partition 908 00005555", "partition 908 00000000"),
              "failed restoration": good.replace("restored 908 00000000", "restored 908 00005555"),
              "missing sample": good.replace("l2lock-partition trial2 1024K core2-streams", "missing sample"),
              "truncated run": good.replace("# l2lock: end", "truncated"),
              "duplicate snapshot": good + "\n# l2lock: saved 900 00000000 read"}
    for label, planted in plants.items():
        try:
            score(planted)
        except ValueError:
            print("X2 report plant caught:", label)
        else:
            raise AssertionError("PLANT ESCAPED: " + label)
    rejected_lines = [line for line in lines if not ("partition" in line or "l2lock-restored trial" in line or "# l2lock: end" in line)]
    rejected_lines += [f"# l2lock: rejected {o:03x} 00000000 read" for o in range(0x900, 0x940, 4)]
    rejected_lines += ["# l2lock: APPLY FAILED at 908", "# l2lock: end measured 0 restored 1"]
    assert score("\n".join(rejected_lines))["verdict"].startswith("disproved")
    no_change = good.replace("core2-streams: cyc 35.000", "core2-streams: cyc 120.000")
    assert score(no_change)["verdict"].startswith("disproved")
    whole = good.replace("# l2lock: id 410000c9 control@100 00000001 aux@104 32890000",
                         "# l2lock run: id 410000c9 control 00000001 aux 32890000")
    whole += "\n# l2lock run: begin VERIFIED\n# l2lock run: restore VERIFIED"
    assert score_run(whole)["restoration"] == "verified"
    for label, planted in (("whole-run missing cleanup", whole.replace("# l2lock run: restore VERIFIED", "lost cleanup")),
                           ("whole-run bad restore", whole.replace("restored 90c 00000000", "restored 90c 00005555"))):
        try:
            score_run(planted)
        except ValueError:
            print("X2 report plant caught:", label)
        else:
            raise AssertionError("PLANT ESCAPED: " + label)
    clock_csv = "real_s,emu_s,gxwait_pct\n" + "\n".join(f"{i*2},{i},20" for i in range(21))
    known_window = {"known": (5.25,15.25)}  # fractional boundaries exercise interpolation as well as averaging
    def check_known(value):
        assert math.isclose(value["derived_speed_pct"], 50) and math.isclose(value["derived_core0_speed_pct"], 62.5)
    clock = score_title_clock(clock_csv, known_window)["known"]
    check_known(clock)
    for label, planted in (("reversed clock", clock_csv.replace("20,10,20", "1,10,20")),
                           ("missing clock coverage", "\n".join(clock_csv.splitlines()[:8])),
                           ("invalid wait", clock_csv.replace(",20", ",100"))):
        try:
            score_title_clock(planted, known_window)
        except ValueError:
            print("X2 clock plant caught:", label)
        else:
            raise AssertionError("PLANT ESCAPED: " + label)
    changed = clock_csv.replace(",20\n", ",10\n")
    try:
        check_known(score_title_clock(changed, known_window)["known"])
    except AssertionError:
        pass
    else:
        raise AssertionError("known-value comparison missed planted wait change")
    print("X2 known-clock comparison detects planted wait change")
    print("X2 report normal and no-change verdict checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--whole-run", action="store_true")
    parser.add_argument("--title-clock", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.log:
        method = score_title_clock if args.title_clock else score_run if args.whole_run else score
        print(json.dumps(method(args.log.read_text()), indent=2))
