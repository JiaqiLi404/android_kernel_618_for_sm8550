#!/usr/bin/env python3
'Device support and compatibility helpers.'

HEADER_PROTOTYPES = (
    "/* dsi_panel_set_backlight(), DCS, before the 51h write: em-cycle switch + dead zone; false = skip 51h */\n"
    "bool k618_mi_disp_bl_pre(struct dsi_panel *panel, u32 *bl_lvl);\n"
    "/* dsi_panel_switch(), before DSI_CMD_SET_TIMING_SWITCH: em bytes of the new mode for the current level */\n"
    "void k618_mi_disp_timing_switch(struct dsi_panel *panel);\n"
    "/* dsi_panel_send_qsync_on/off_dcs(), before the command */\n"
    "void k618_mi_disp_qsync(struct dsi_panel *panel, bool on);\n"
    "/* dsi_panel_set_nolp(), around DSI_CMD_SET_NOLP */\n"
    "void k618_mi_disp_pre_nolp(struct dsi_panel *panel);\n"
    "void k618_mi_disp_post_nolp(struct dsi_panel *panel, int rc);\n"
    "/* dsi_panel_disable(), after the panel is marked off */\n"
    "void k618_mi_disp_panel_off(struct dsi_panel *panel);\n"
)

DSI_PANEL = "display-drivers/msm/dsi/dsi_panel.c"

EDITS = [
    # 1. before the backlight-type switch, so the remapped level flows into the DCS case unchanged
    (DSI_PANEL,
     '\tDSI_DEBUG("backlight type:%d lvl:%d\\n", bl->type, bl_lvl);\n'
     "\tswitch (bl->type) {\n",
     '\tDSI_DEBUG("backlight type:%d lvl:%d\\n", bl->type, bl_lvl);\n'
     "\t/* k618: the 13 Pro's 1920 Hz PWM switch and the dead zone around it go before the\n"
     "\t * 51h write (stock dsi_panel_update_backlight); no 51h while qsync is on */\n"
     "\tif (bl->type == DSI_BACKLIGHT_DCS && !k618_mi_disp_bl_pre(panel, &bl_lvl)) {\n"
     "\t\tk618_mi_disp_bl(panel, bl_lvl);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tswitch (bl->type) {\n"),

    # 2. the live (non-Iris) branch of dsi_panel_switch(); panel_lock is held, cur_mode = new mode
    (DSI_PANEL,
     "#else /* CONFIG_PXLW_IRIS */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH, false);\n"
     "#endif /* CONFIG_PXLW_IRIS */\n",
     "#else /* CONFIG_PXLW_IRIS */\n"
     "\tk618_mi_disp_timing_switch(panel);\t/* k618: em-cycle bytes for the current level */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH, false);\n"
     "#endif /* CONFIG_PXLW_IRIS */\n"),

    # 3. qsync on: state first (stock sets panel+0xea0 before the rewrite)
    (DSI_PANEL,
     '\tDSI_DEBUG("ctrl:%d qsync on\\n", ctrl_idx);\n'
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON, false);\n",
     '\tDSI_DEBUG("ctrl:%d qsync on\\n", ctrl_idx);\n'
     "\tk618_mi_disp_qsync(panel, true);\t/* k618: em bytes for the current level */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON, false);\n"),

    # 4. qsync off: its DT bytes end in 51 00 00
    (DSI_PANEL,
     '\tDSI_DEBUG("ctrl:%d qsync off\\n", ctrl_idx);\n'
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF, false);\n",
     '\tDSI_DEBUG("ctrl:%d qsync off\\n", ctrl_idx);\n'
     "\tk618_mi_disp_qsync(panel, false);\t/* k618: em bytes + 51h = the current level */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF, false);\n"),

    # 5. nolp (LP1/LP2 -> ON): its DT bytes carry 51 00 00 and switch the panel to 16 pulses
    (DSI_PANEL,
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP, false);\n"
     "\tif (rc)\n"
     '\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\\n",\n'
     "\t\t       panel->name, rc);\n",
     "\tk618_mi_disp_pre_nolp(panel);\t/* k618: 51h of nolp = the current level */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP, false);\n"
     "\tif (rc)\n"
     '\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\\n",\n'
     "\t\t       panel->name, rc);\n"
     "\tk618_mi_disp_post_nolp(panel, rc);\t/* k618: nolp leaves the panel in 16-pulse mode */\n"),

    # 6. dsi_panel_disable(): stock clears panel+0xc14 / 0xa11 / feature_val[HBM] here
    (DSI_PANEL,
     "\tpanel->panel_initialized = false;\n"
     "\tpanel->power_mode = SDE_MODE_DPMS_OFF;\n",
     "\tpanel->panel_initialized = false;\n"
     "\tpanel->power_mode = SDE_MODE_DPMS_OFF;\n"
     "\tk618_mi_disp_panel_off(panel);\t/* k618: em-cycle / qsync / HBM state back to power-on */\n"),
]


def apply(text, old, new):
    """k618_tp_fix.py's rule: returns (new text, what happened)."""
    if old in new:
        if new in text:
            return text, "already"
    elif new in text and old not in text:
        return text, "already"
    n = text.count(old)
    if n != 1:
        return text, "found %d times" % n
    return text.replace(old, new), "applied"


def main(argv):
    import os
    import sys
    if len(argv) < 2:
        print("usage: k618_mi_brightness_edits.py <techpack root>", file=sys.stderr)
        return 2
    root = argv[1].rstrip("/")
    done = 0
    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        text2, what = apply(text, old, new)
        if what.startswith("found"):
            print(f"k618_mi_brightness_edits: {rel}: text to change {what}", file=sys.stderr)
            return 1
        if what == "applied":
            open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text2)
            print(f"k618_mi_brightness_edits: {rel}")
            done += 1
    print(f"k618_mi_brightness_edits: {done} of {len(EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
