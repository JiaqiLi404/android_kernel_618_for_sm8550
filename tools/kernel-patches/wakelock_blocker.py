#!/usr/bin/env python3
"""Wakelock blocker: wakeup sources named in a list never keep the system awake.

Usage: wakelock_blocker.py <kernel-common-dir> <default list, e.g. "a;b"> | --defaults

Adds the blocker to drivers/base/power/wakeup.c. Its sysfs directory is
/sys/devices/virtual/misc/wakelock_blocker: wakelock_blocker (user list), wakelock_blocker_default
(built-in list, writable), debug and version; names are separated by ';'. Both lists are parsed
when written and swapped under RCU; the check reads a 64-bit filter of first characters and then
compares names. --defaults selects DEFAULT_LIST. Idempotent; a re-run only changes the default list.
"""

import sys

DEFAULT_LIST = (
    "qcom_rx_wakelock;IPA_CLIENT_APPS_WAN_COAL_CONS;IPA_CLIENT_APPS_WAN_LOW_LAT_CONS;IPA_CLIENT_APPS_LAN_CONS;"
    "IPA_WS;DIAG_WS;RMNET_DFC;RMNET_SHS;rmnet_ctl;rmnet_ipa%d;hal_bluetooth_lock;IPA_CLIENT_APPS_LAN_COAL_CONS;1-0008"
)

BLOCK = r"""/*
 * Wakelock blocker: wakeup sources named in the default or the user list never keep the system
 * awake. /sys/devices/virtual/misc/wakelock_blocker/{wakelock_blocker,wakelock_blocker_default,
 * debug,version}; names separated by ';'.
 */
#define WL_BLOCKER_VERSION	"1.0.0"
#define WL_BLOCKER_LEN		1024
#define WL_BLOCKER_NAME		61	/* a longer name is never blocked */

/*
 * Both lists the way the check wants them: the names one by one, and 64 bits that say with which
 * two characters a name on the lists can begin. The check runs at every event of every wakeup
 * source, under the source's spinlock; nearly all of them are on neither list, and for those it
 * is over after the two characters.
 */
struct wl_blocker_list {
	struct rcu_head rcu;
	u64 starts;
	unsigned int nr;
	const char *name[];	/* and behind them the names themselves */
};

static char wl_blocker_default[WL_BLOCKER_LEN] = "@DEFAULT@";
static char wl_blocker_user[WL_BLOCKER_LEN];
static struct wl_blocker_list __rcu *wl_blocker_search;
static DEFINE_MUTEX(wl_blocker_mutex);
static bool wl_blocker_debug;

/* @name is not empty (the character after a single one is the 0 at its end) */
static inline u64 wl_blocker_start(const char *name)
{
	return BIT_ULL(((u8)name[0] * 31 + (u8)name[1]) & 63);
}

/* under ws->lock; true if @ws is blocked (and then let go if it was active) */
static bool wl_blocker_check(struct wakeup_source *ws)
{
	const char *name = ws->name;
	struct wl_blocker_list *l;
	bool blocked = false;
	unsigned int i;

	if (unlikely(wl_blocker_debug))
		pr_info("WL blocker: %s requested\n", name);
	if (!name || !name[0])
		return false;
	rcu_read_lock();
	l = rcu_dereference(wl_blocker_search);
	if (l && (l->starts & wl_blocker_start(name)))
		for (i = 0; i < l->nr && !blocked; i++)
			blocked = !strcmp(l->name[i], name);
	rcu_read_unlock();
	if (!blocked)
		return false;
	if (unlikely(wl_blocker_debug))
		pr_info("WL blocker: %s blocked\n", ws->name);
	if (ws->active) {
		wakeup_source_deactivate(ws);
		if (unlikely(wl_blocker_debug))
			pr_info("WL blocker: %s killed\n", ws->name);
	}
	return true;
}

/* the next name of a list ("a;b;c") from *@p on, its length in *@len; NULL when there is none */
static const char *wl_blocker_next(const char **p, size_t *len)
{
	const char *s = *p, *end;

	while (*s == ';')
		s++;
	if (!*s)
		return NULL;
	end = strchrnul(s, ';');
	*len = end - s;
	*p = end;
	return s;
}

/* under wl_blocker_mutex */
static int wl_blocker_rebuild(void)
{
	const char *const lists[] = { wl_blocker_default, wl_blocker_user };
	struct wl_blocker_list *l = NULL, *old;
	unsigned int nr = 0, i;
	size_t len, bytes = 0;
	const char *p, *s;
	char *names;

	for (i = 0; i < ARRAY_SIZE(lists); i++)
		for (p = lists[i]; (s = wl_blocker_next(&p, &len));)
			if (len <= WL_BLOCKER_NAME) {
				nr++;
				bytes += len + 1;
			}
	if (nr) {
		l = kzalloc(struct_size(l, name, nr) + bytes, GFP_KERNEL);
		if (!l)
			return -ENOMEM;
		names = (char *)&l->name[nr];
		for (i = 0; i < ARRAY_SIZE(lists); i++)
			for (p = lists[i]; (s = wl_blocker_next(&p, &len));)
				if (len <= WL_BLOCKER_NAME) {
					memcpy(names, s, len);	/* the 0 behind it is there */
					l->starts |= wl_blocker_start(names);
					l->name[l->nr++] = names;
					names += len + 1;
				}
	}
	old = rcu_replace_pointer(wl_blocker_search, l, lockdep_is_held(&wl_blocker_mutex));
	if (old)
		kfree_rcu(old, rcu);
	return 0;
}

static ssize_t wl_blocker_store_list(char *list, const char *buf, size_t count)
{
	size_t len = count;
	int ret;

	if (len && buf[len - 1] == '\n')
		len--;
	if (len >= WL_BLOCKER_LEN)
		return -EINVAL;
	mutex_lock(&wl_blocker_mutex);
	memcpy(list, buf, len);
	list[len] = 0;
	ret = wl_blocker_rebuild();
	mutex_unlock(&wl_blocker_mutex);
	return ret ? ret : count;
}

static ssize_t wakelock_blocker_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", wl_blocker_user);
}

static ssize_t wakelock_blocker_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	return wl_blocker_store_list(wl_blocker_user, buf, count);
}

static ssize_t wakelock_blocker_default_show(struct device *dev, struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%s\n", wl_blocker_default);
}

static ssize_t wakelock_blocker_default_store(struct device *dev, struct device_attribute *attr,
					      const char *buf, size_t count)
{
	return wl_blocker_store_list(wl_blocker_default, buf, count);
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", wl_blocker_debug);
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr, const char *buf,
			   size_t count)
{
	bool v;

	if (kstrtobool(buf, &v))
		return -EINVAL;
	WRITE_ONCE(wl_blocker_debug, v);
	return count;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", WL_BLOCKER_VERSION);
}

static DEVICE_ATTR_RW(wakelock_blocker);
static DEVICE_ATTR_RW(wakelock_blocker_default);
static DEVICE_ATTR_RW(debug);
static DEVICE_ATTR_RO(version);

static struct attribute *wl_blocker_attrs[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_wakelock_blocker_default.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wl_blocker);

static struct miscdevice wl_blocker_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wakelock_blocker",
	.groups = wl_blocker_groups,
};

static int __init wl_blocker_init(void)
{
	int ret;

	mutex_lock(&wl_blocker_mutex);
	ret = wl_blocker_rebuild();
	mutex_unlock(&wl_blocker_mutex);
	if (!ret)
		ret = misc_register(&wl_blocker_device);
	if (ret)
		pr_err("WL blocker: not started (%d)\n", ret);
	else
		pr_info("WL blocker: driver version %s started\n", WL_BLOCKER_VERSION);
	return 0;
}
late_initcall(wl_blocker_init);

"""

# where a block put there by an earlier run begins (its opening comment, by this or an earlier
# version of this script)
BLOCK_START = "/*\n * Wakelock blocker"
# the blocker goes in front of the function's kernel-doc comment
DOC = "/**\n * wakeup_source_report_event - Report wakeup event using the given source.\n"
REPORT_OLD = """static void wakeup_source_report_event(struct wakeup_source *ws, bool hard)
{
	ws->event_count++;
	/* This is racy, but the counter is approximate anyway. */
	if (events_check_enabled)
		ws->wakeup_count++;

	if (!ws->active)
		wakeup_source_activate(ws);

	if (hard)
		pm_system_wakeup();
}
"""
REPORT_NEW = """static void wakeup_source_report_event(struct wakeup_source *ws, bool hard)
{
	/* a blocked wakeup source neither counts, nor activates, nor aborts a suspend */
	if (wl_blocker_check(ws))
		return;

	ws->event_count++;
	/* This is racy, but the counter is approximate anyway. */
	if (events_check_enabled)
		ws->wakeup_count++;

	if (!ws->active)
		wakeup_source_activate(ws);

	if (hard)
		pm_system_wakeup();
}
"""

INCLUDE_OLD = '#include "power.h"\n'
INCLUDE_NEW = '#include <linux/miscdevice.h>\n#include <linux/rcupdate.h>\n\n#include "power.h"\n'
# used by the blocker before its definition
DEACT_DECL_ANCHOR = "static void wakeup_source_deactivate(struct wakeup_source *ws);\n"


def apply(root, default):
    path = f"{root.rstrip('/')}/drivers/base/power/wakeup.c"
    text = open(path, encoding="utf-8").read()
    if "wl_blocker_check(ws)" in text:
        # applied before, by this version or an earlier one: the block as it is now, with this list
        start, end = text.find(BLOCK_START), text.find(DOC)
        if not 0 <= start < end:
            return ["drivers/base/power/wakeup.c: has wl_blocker_check(), but not the block this script put there"]
        open(path, "w", encoding="utf-8").write(text[:start] + BLOCK.replace("@DEFAULT@", default) + text[end:])
        return []
    problems = []
    for old, want in ((REPORT_OLD, 1), (INCLUDE_OLD, 1), (DOC, 1)):
        if text.count(old) != want:
            problems.append(f"drivers/base/power/wakeup.c: expected {want}x `{old.splitlines()[0]}`, found {text.count(old)}")
    if problems:
        return problems
    block = DEACT_DECL_ANCHOR + "\n" + BLOCK.replace("@DEFAULT@", default)
    text = text.replace(INCLUDE_OLD, INCLUDE_NEW)
    text = text.replace(DOC, block + DOC).replace(REPORT_OLD, REPORT_NEW)
    open(path, "w", encoding="utf-8").write(text)
    return []


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    default = DEFAULT_LIST if argv[2] == "--defaults" else argv[2]
    if '"' in default or "\\" in default or len(default) >= 1024:
        print("::error::wakelock_blocker: the default list must be under 1024 characters, without quotes or backslashes", file=sys.stderr)
        return 2
    problems = apply(argv[1], default)
    for p in problems:
        print(f"::error::wakelock_blocker: {p}", file=sys.stderr)
    if not problems:
        print(f"wakelock_blocker: default list {'(empty)' if not default else default}")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
