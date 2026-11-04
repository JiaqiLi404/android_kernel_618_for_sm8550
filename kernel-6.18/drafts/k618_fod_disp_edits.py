#!/usr/bin/env python3
'Device support and compatibility helpers.'

# (enum name, DT property) -- appended to k618_tp_fix.py MI_CMD_SETS. Order is free (positional with
# the rest); keep it stable once chosen so cmd_sets[] indices do not move between builds.
FOD_CMD_SETS = [
    # --- local HBM nit levels (normal) + their 16-pulse (1920 Hz) variants ---
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT",
     "mi,mdss-dsi-local-hbm-normal-white-1000nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-white-1000nit-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT",
     "mi,mdss-dsi-local-hbm-normal-white-110nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-white-110nit-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT",
     "mi,mdss-dsi-local-hbm-normal-green-500nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-green-500nit-em-cycle-16pulse-command"),
    # --- H-doze (AOD) local HBM ---
    ("DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT",
     "mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT",
     "mi,mdss-dsi-local-hbm-hlpm-white-110nit-command"),
    # --- local HBM off transitions ---
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL",
     "mi,mdss-dsi-local-hbm-off-to-normal-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_16P",
     "mi,mdss-dsi-local-hbm-off-to-normal-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM",
     "mi,mdss-dsi-local-hbm-off-to-hbm-command"),
    # --- AOD / doze brightness command sets ---
    ("DSI_CMD_SET_MI_DOZE_HBM",      "mi,mdss-dsi-doze-hbm-command"),
    ("DSI_CMD_SET_MI_DOZE_LBM",      "mi,mdss-dsi-doze-lbm-command"),
    ("DSI_CMD_SET_MI_DOZE_HBM_NOLP", "mi,mdss-dsi-doze-hbm-nolp-command"),
    ("DSI_CMD_SET_MI_DOZE_LBM_NOLP", "mi,mdss-dsi-doze-lbm-nolp-command"),
]

# k618_mi_disp.h: no NEW cross-module prototypes. FOD/doze functions are static inside mi_disp_k618.c
# (they are reached through the ioctl / sysfs / set_feature paths already in the file). The only
# header-level change is already covered by the brightness round (the LP1/LP2/nolp/panel-off hooks).
HEADER_PROTOTYPES = ""

# Sanity: the enum names must be unique and match k618_fod_disp.c's references.
_USED_IN_C = {
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT",
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_16P",
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT",
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_16P",
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT",
    "DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_16P",
    "DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT",
    "DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT",
    "DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL",
    "DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_16P",
    "DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM",
    "DSI_CMD_SET_MI_DOZE_HBM",
    "DSI_CMD_SET_MI_DOZE_LBM",
    "DSI_CMD_SET_MI_DOZE_HBM_NOLP",
    "DSI_CMD_SET_MI_DOZE_LBM_NOLP",
}


def main(argv):
    import sys
    names = [e for e, _ in FOD_CMD_SETS]
    props = [p for _, p in FOD_CMD_SETS]
    ok = True
    if len(set(names)) != len(names):
        print("duplicate enum name in FOD_CMD_SETS", file=sys.stderr)
        ok = False
    if len(set(props)) != len(props):
        print("duplicate DT property in FOD_CMD_SETS", file=sys.stderr)
        ok = False
    missing = _USED_IN_C - set(names)
    if missing:
        print("enum used in k618_fod_disp.c but not declared here:", sorted(missing), file=sys.stderr)
        ok = False
    extra = set(names) - _USED_IN_C
    if extra:
        print("declared but unused (fine, reserved):", sorted(extra))
    print("%d FOD/doze command sets, %s" % (len(FOD_CMD_SETS), "OK" if ok else "PROBLEM"))
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
