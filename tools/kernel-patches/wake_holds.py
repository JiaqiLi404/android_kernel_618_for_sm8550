#!/usr/bin/env python3
"""Configure untimed wakelock and alarm wake holds (this project's policy) as bounded, reversible
runtime parameters: wakelock.untimed_ms and alarmtimer.short_hold. See docs/PROVENANCE.en.md.

Usage: wake_holds.py <kernel-common-dir> [1 (default: 500 ms, Y) | 0 (both as upstream)]
"""

import re
import sys

WAKELOCK = "kernel/power/wakelock.c"
ALARM = "kernel/time/alarmtimer.c"

WAKELOCK_PARAM = """
/*
 * A wake lock that user space takes through /sys/power/wake_lock without a time limit holds
 * for this many milliseconds; 0: until it is released, as upstream. A lock taken with a time
 * limit keeps its own.
 */
static unsigned int wakelock_untimed_ms __read_mostly = %d;
module_param_named(untimed_ms, wakelock_untimed_ms, uint, 0644);
"""
ALARM_PARAM = """
/*
 * An alarm due within two seconds stops a suspend. The system then stays awake until 5 ms
 * after the alarm is due (Y), or for two seconds whenever it is due (N, as upstream).
 */
static bool alarm_short_hold __read_mostly = %s;
module_param_named(short_hold, alarm_short_hold, bool, 0644);
"""
# (a line of the edit that is nowhere else, file, anchor, what follows it or replaces it, replaces?)
EDITS = [
    ("#include <linux/moduleparam.h>", WAKELOCK, "#include <linux/list.h>\n", "#include <linux/moduleparam.h>\n", False),
    ("module_param_named(untimed_ms, wakelock_untimed_ms, uint, 0644);", WAKELOCK, '#include "power.h"\n', WAKELOCK_PARAM, False),
    (
        "unsigned int ms = READ_ONCE(wakelock_untimed_ms);",
        WAKELOCK,
        """	} else {
		__pm_stay_awake(wl->ws);
	}
""",
        """	} else {
		unsigned int ms = READ_ONCE(wakelock_untimed_ms);

		if (ms)
			__pm_wakeup_event(wl->ws, ms);
		else
			__pm_stay_awake(wl->ws);
	}
""",
        True,
    ),
    (
        "module_param_named(short_hold, alarm_short_hold, bool, 0644);",
        ALARM,
        "static DEFINE_SPINLOCK(freezer_delta_lock);\n#endif\n\n#ifdef CONFIG_RTC_CLASS\n",
        ALARM_PARAM,
        False,
    ),
    (
        "if (READ_ONCE(alarm_short_hold)) {",
        ALARM,
        """	if (ktime_to_ns(min) < 2 * NSEC_PER_SEC) {
		pm_wakeup_event(dev, 2 * MSEC_PER_SEC);
		return -EBUSY;
	}
""",
        """	if (ktime_to_ns(min) < 2 * NSEC_PER_SEC) {
		unsigned int hold = 2 * MSEC_PER_SEC;

		if (READ_ONCE(alarm_short_hold)) {
			s64 ahead = ktime_to_ms(min);

			hold = (ahead > 0 ? ahead : 0) + 5;
		}
		pm_wakeup_event(dev, hold);
		return -EBUSY;
	}
""",
        True,
    ),
]
# the line of each default, and what it is with the change on and off
DEFAULTS = {
    WAKELOCK: (r"(static unsigned int wakelock_untimed_ms __read_mostly = )\d+;", "500", "0"),
    ALARM: (r"(static bool alarm_short_hold __read_mostly = )\w+;", "true", "false"),
}


def main(argv):
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] not in ("0", "1")):
        print(__doc__)
        return 2
    root = argv[1].rstrip("/")
    on = argv[2] == "1" if len(argv) == 3 else True
    texts = {}
    for rel in (WAKELOCK, ALARM):
        with open(f"{root}/{rel}", encoding="utf-8") as f:
            texts[rel] = f.read()
    before = dict(texts)
    for mark, rel, anchor, addition, replaces in EDITS:
        text = texts[rel]
        if addition in (WAKELOCK_PARAM, ALARM_PARAM):
            addition = addition % (500 if rel == WAKELOCK else "true")
        if text.count(mark) == 1 and (not replaces or anchor not in text):
            continue  # already applied (the default line is brought to what is asked for below)
        found = text.count(anchor)
        if found != 1 or mark in text:
            print(f"::error::wake_holds: {rel}: expected 1x {anchor.strip().splitlines()[0]!r}..., found {found}", file=sys.stderr)
            return 1
        texts[rel] = text.replace(anchor, addition if replaces else anchor + addition)
    for rel, (pattern, with_it, without) in DEFAULTS.items():
        texts[rel], n = re.subn(pattern, rf"\g<1>{with_it if on else without};", texts[rel])
        if n != 1:
            print(f"::error::wake_holds: {rel}: the default's line is there {n} times", file=sys.stderr)
            return 1
    for rel, text in texts.items():
        if text != before[rel]:
            with open(f"{root}/{rel}", "w", encoding="utf-8") as f:
                f.write(text)
    state = "500 ms for a wake lock without a time limit, an alarm's hold ends 5 ms after it is due" if on else "there, both as upstream by default"
    print(f"wake_holds: {state}" + ("" if texts != before else " (already applied)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
