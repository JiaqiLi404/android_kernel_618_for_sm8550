#!/usr/bin/env python3
"""k618_mi_disp_edits.py <techpack root>: the display-driver (msm_drm) side of the mi_disp port.

Fold these into k618_tp_fix.py once reviewed. They:
  - drop the trimmed mi_disp source + its header into display-drivers/msm/ (GENERATED),
  - link it into msm_drm and start it from the DSI display probe,
  - add the handful of Xiaomi command sets the port sends to enum dsi_cmd_set_type +
    cmd_set_prop_map[]/cmd_set_state_map[] so Qualcomm's own per-mode parser reads them from the
    panel's timing nodes (same blob format as qcom,mdss-dsi-on-command),
  - hook panel enable (POWER event + panel-on dimming) and the DCS backlight path (51h event).

Format matches k618_tp_fix.py: SWEEPS is unused here; GENERATED is (relpath, text); EDITS is
(file, old, new). Paths are relative to the techpack root (the dir k618_tp_fix.py is given).
The mi_disp source itself is this repo's drafts/k618_midisp_v2.c, copied in verbatim as
display-drivers/msm/mi_disp_k618.c -- keep the two in sync (COPY below reads it at apply time).

Scope: brightness / colour / flicker. Not ported: FOD / local-HBM / AOD / DC / gamma / the
per-command -update byte patching (HBM/em-cycle brightness bytes) and the DBV-threshold em-cycle
(1920pwm) switch -- those are open items in k618_midisp_notes.md.
"""

import os
import sys

# display-drivers/msm/mi_disp_k618.c is copied from this file's sibling draft at apply time.
COPY = [
    ("display-drivers/msm/mi_disp_k618.c", "k618_midisp_v2.c"),
    ("display-drivers/msm/mi_disp_k618.h", "mi_disp_k618.h"),
]

# The eight Xiaomi command sets the port sends. Each is appended to enum dsi_cmd_set_type (before
# DSI_CMD_SET_MAX) and, at the SAME ordinal, to cmd_set_prop_map[] and cmd_set_state_map[] (both are
# positional arrays sized [DSI_CMD_SET_MAX]). Names are the per-timing DT properties on this panel.
MI_CMD_SETS = [
    ("DSI_CMD_SET_MI_DIMMINGON",            "mi,mdss-dsi-dimmingon-command"),
    ("DSI_CMD_SET_MI_DIMMINGOFF",           "mi,mdss-dsi-dimmingoff-command"),
    ("DSI_CMD_SET_MI_HBM_ON",               "mi,mdss-dsi-hbm-on-command"),
    ("DSI_CMD_SET_MI_HBM_OFF",              "mi,mdss-dsi-hbm-off-command"),
    ("DSI_CMD_SET_MI_FLAT_MODE_ON",         "mi,mdss-dsi-flat-mode-on-command"),
    ("DSI_CMD_SET_MI_FLAT_MODE_OFF",        "mi,mdss-dsi-flat-mode-off-command"),
    ("DSI_CMD_SET_MI_EM_CYCLE_16PULSE_ON",  "mi,mdss-dsi-panel-em-cycle-16pulse-on-command"),
    ("DSI_CMD_SET_MI_EM_CYCLE_16PULSE_OFF", "mi,mdss-dsi-panel-em-cycle-16pulse-off-command"),
]

_enum_block = "\n".join("\t%s," % e for e, _ in MI_CMD_SETS) + "\n"
_prop_block = "".join('\t"%s",\n' % p for _, p in MI_CMD_SETS)
_state_block = "".join('\t"%s-state",\n' % p for _, p in MI_CMD_SETS)

EDITS = [
    # 1. enum dsi_cmd_set_type: the Xiaomi sets go right before DSI_CMD_SET_MAX, so priv_info->
    #    cmd_sets[] (sized [DSI_CMD_SET_MAX]) grows to hold them and the parser's
    #    "for (i = PRE_ON; i < MAX; i++)" loop picks them up with no further change.
    ("display-drivers/msm/dsi/dsi_defs.h",
     "\tDSI_CMD_SET_FPS_SWITCH,\n"
     "\tDSI_CMD_SET_MAX\n",
     "\tDSI_CMD_SET_FPS_SWITCH,\n"
     + _enum_block +
     "\tDSI_CMD_SET_MAX\n"),

    # 2. cmd_set_prop_map[] / cmd_set_state_map[]: same ordinal as the enum (both positional).
    #    The map's last stock entry is the fps-switch command; append ours after it.
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"qcom,mdss-dsi-fps-switch-command",\n'
     "};\n\n"
     "const char *cmd_set_state_map[DSI_CMD_SET_MAX] = {",
     '\t"qcom,mdss-dsi-fps-switch-command",\n'
     + _prop_block +
     "};\n\n"
     "const char *cmd_set_state_map[DSI_CMD_SET_MAX] = {"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"qcom,mdss-dsi-fps-switch-command-state",\n'
     "};\n#endif /* OPLUS_FEATURE_DISPLAY */",
     '\t"qcom,mdss-dsi-fps-switch-command-state",\n'
     + _state_block +
     "};\n#endif /* OPLUS_FEATURE_DISPLAY */"),

    # 3. dsi_panel.c: include the hooks; fire panel-on (POWER + dimming) after the panel is up;
    #    pass the DCS backlight level through as a 51_brightness event.
    ("display-drivers/msm/dsi/dsi_panel.c",
     '#include "dsi_panel.h"\n',
     '#include "dsi_panel.h"\n#include "mi_disp_k618.h"\n'),
    # The k618_bl_level block exists twice (the live path and a CONFIG_PXLW_IRIS dead-code copy);
    # anchor the panel-on hook on the live "} \n panel->panel_initialized = true;" (the video-mode
    # else-if close + single-tab assignment), which is unique to the live path.
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\t}\n\tpanel->panel_initialized = true;\n",
     "\t}\n\tpanel->panel_initialized = true;\n\tk618_mi_disp_panel_on(panel);\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\tcase DSI_BACKLIGHT_DCS:\n"
     "\t\trc = dsi_panel_update_backlight(panel, bl_lvl);\n"
     "\t\tbreak;\n",
     "\tcase DSI_BACKLIGHT_DCS:\n"
     "\t\trc = dsi_panel_update_backlight(panel, bl_lvl);\n"
     "\t\tk618_mi_disp_bl(panel, bl_lvl);\n"
     "\t\tbreak;\n"),

    # 4. dsi_display.c: include the hooks; start the mi_disp char device/class/sysfs early in the
    #    DSI display probe (idempotent), so the composer's open of /dev/mi_display/disp_feature and
    #    its read of .../disp-DSI-0/panel_info succeed before any HAL connects.
    ("display-drivers/msm/dsi/dsi_display.c",
     "#include <linux/list.h>\n",
     "#include <linux/list.h>\n#include \"mi_disp_k618.h\"\n"),
    ("display-drivers/msm/dsi/dsi_display.c",
     "\tdisplay = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);\n",
     "\tk618_mi_disp_init();\n\n"
     "\tdisplay = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);\n"),

    # 5. Kbuild: link the port into msm_drm.
    ("display-drivers/msm/Kbuild",
     "obj-m += msm_drm.o\n",
     "obj-m += msm_drm.o\n"
     "msm_drm-y += mi_disp_k618.o\n"),
]


def main(argv):
    root = argv[1].rstrip("/")
    here = os.path.dirname(os.path.abspath(__file__))
    done = 0

    for rel, src in COPY:
        text = open(os.path.join(here, src), encoding="utf-8").read()
        path = os.path.join(root, rel)
        if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        print(f"k618_mi_disp: wrote {rel}")

    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            print(f"k618_mi_disp: no {rel}", file=sys.stderr)
            continue
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if new in text and old not in text:
            continue
        if old not in text:
            print(f"k618_mi_disp: {rel}: anchor not found", file=sys.stderr)
            continue
        open(path, "w", encoding="utf-8", errors="surrogateescape",
             newline="").write(text.replace(old, new, 1))
        print(f"k618_mi_disp: {rel}")
        done += 1
    print(f"k618_mi_disp: {done} of {len(EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
