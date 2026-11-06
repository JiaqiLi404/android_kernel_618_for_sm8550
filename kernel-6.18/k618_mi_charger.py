#!/usr/bin/env python3
'Device support and compatibility helpers.'

import os
import re
import sys

CHG = "drivers/power/supply/qti_battery_charger.c"

# ------------------------------------------------------------------------------------------------
# Part A: 5.15 -> 6.x, exactly as k618_mi_charger_haptics.py (charger entries only)
# ------------------------------------------------------------------------------------------------

RE_EDITS = [
    (re.compile(r"(?<!const )\bstruct class \*c,"), "const struct class *c,"),
    (re.compile(r"(?<!const )\bstruct class_attribute \*attr"), "const struct class_attribute *attr"),
]

A_EDITS = [
    ("#include <linux/soc/qcom/pmic_glink.h>", "#include <linux/soc/qcom/qti_pmic_glink.h>"),
    ("""static enum power_supply_usb_type usb_psy_supported_types[] = {
\tPOWER_SUPPLY_USB_TYPE_UNKNOWN,
\tPOWER_SUPPLY_USB_TYPE_SDP,
\tPOWER_SUPPLY_USB_TYPE_DCP,
\tPOWER_SUPPLY_USB_TYPE_CDP,
\tPOWER_SUPPLY_USB_TYPE_ACA,
\tPOWER_SUPPLY_USB_TYPE_C,
\tPOWER_SUPPLY_USB_TYPE_PD,
\tPOWER_SUPPLY_USB_TYPE_PD_DRP,
\tPOWER_SUPPLY_USB_TYPE_PD_PPS,
\tPOWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
};

""", ""),
    ("\t.usb_types\t\t= usb_psy_supported_types,\n"
     "\t.num_usb_types\t\t= ARRAY_SIZE(usb_psy_supported_types),\n",
     "\t.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_C)       |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD)      |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),\n"),
    ("\tpsy = tzd->devdata;\n", "\tpsy = thermal_zone_device_priv(tzd);\n"),
    ("\tpsy->tzd = thermal_zone_device_register(psy->desc->name,\n"
     "\t\t\t\t\t0, 0, psy, &psy_tzd_ops, NULL, 0, 0);\n",
     "\tpsy->tzd = thermal_tripless_zone_device_register(psy->desc->name,\n"
     "\t\t\t\t\tpsy, &psy_tzd_ops, NULL);\n"
     "\tif (IS_ERR(psy->tzd)) {\n"
     "\t\tpr_err(\"Failed to register battery thermal zone, rc=%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(psy->tzd));\n"
     "\t\tpsy->tzd = NULL;\n"
     "\t} else if (thermal_zone_device_enable(psy->tzd)) {\n"
     "\t\tpr_err(\"Failed to enable battery thermal zone\\n\");\n"
     "\t}\n"),
    ("static int add_xiaomi_uevent(struct device *dev, struct kobj_uevent_env *env)",
     "static int add_xiaomi_uevent(const struct device *dev, struct kobj_uevent_env *env)"),
    ("\trc = pmic_glink_unregister_client(bcdev->client);\n"
     "\tif (rc < 0) {\n"
     "\t\tpr_err(\"Error unregistering from pmic_glink, rc=%d\\n\", rc);\n"
     "\t\treturn rc;\n"
     "\t}\n",
     "\trc = pmic_glink_unregister_client(bcdev->client);\n"
     "\tif (rc < 0)\n"
     "\t\tpr_err(\"Error unregistering from pmic_glink, rc=%d\\n\", rc);\n"),
]

VOID_REMOVE = ["battery_chg_remove"]

KCONFIG = [("drivers/power/supply/Kconfig", "MI_WIRELESS")]

# ------------------------------------------------------------------------------------------------
# Part B: the stock module's numbering and behaviour
# ------------------------------------------------------------------------------------------------

# name: (stock id, "v" = an immediate in the stock module's own code for this name,
#        "p" = position only: fuxi's enum is stock's with entries inserted, this name fills a gap
#        exactly as wide as fuxi's run of names there; nothing in this file sends it)
XM_STOCK = [
    ("XM_PROP_RESISTANCE_ID", 0, "v"), ("XM_PROP_VERIFY_DIGEST", 1, "v"),
    ("XM_PROP_CONNECTOR_TEMP", 2, "v"), ("XM_PROP_AUTHENTIC", 3, "v"),
    ("XM_PROP_BATTERY_ADAPT_POWER_MATCH", 4, "v"), ("XM_PROP_CHIP_OK", 5, "v"),
    ("XM_PROP_VBUS_DISABLE", 6, "p"), ("XM_PROP_REAL_TYPE", 7, "v"),
    ("XM_PROP_THERMAL_BOARD_TEMP", 8, "v"), ("XM_PROP_NTC_ALARM", 9, "v"),
    ("XM_PROP_CHARGER_USER_VALUE_MAP", 10, "v"), ("XM_PROP_CLOUD_CONTROL_CID_MODE", 11, "v"),
    ("XM_PROP_VERIFY_PROCESS", 12, "v"), ("XM_PROP_VDM_CMD_CHARGER_VERSION", 13, "v"),
    ("XM_PROP_VDM_CMD_CHARGER_VOLTAGE", 14, "v"), ("XM_PROP_VDM_CMD_CHARGER_TEMP", 15, "v"),
    ("XM_PROP_VDM_CMD_SESSION_SEED", 16, "v"), ("XM_PROP_VDM_CMD_AUTHENTICATION", 17, "v"),
    ("XM_PROP_VDM_CMD_VERIFIED", 18, "v"), ("XM_PROP_VDM_CMD_REMOVE_COMPENSATION", 19, "v"),
    ("XM_PROP_VDM_CMD_REVERSE_AUTHEN", 20, "v"), ("XM_PROP_CURRENT_STATE", 21, "v"),
    ("XM_PROP_ADAPTER_ID", 22, "v"), ("XM_PROP_ADAPTER_SVID", 23, "v"),
    ("XM_PROP_PD_VERIFED", 24, "v"), ("XM_PROP_PDO2", 25, "v"), ("XM_PROP_UVDM_STATE", 26, "v"),
    ("XM_PROP_BQ2597X_CHIP_OK", 27, "v"), ("XM_PROP_BQ2597X_SLAVE_CHIP_OK", 28, "v"),
    ("XM_PROP_BQ2597X_BUS_CURRENT", 29, "v"), ("XM_PROP_BQ2597X_SLAVE_BUS_CURRENT", 30, "v"),
    ("XM_PROP_BQ2597X_BUS_DELTA", 31, "v"), ("XM_PROP_BQ2597X_BUS_VOLTAGE", 32, "v"),
    ("XM_PROP_BQ2597X_BATTERY_PRESENT", 33, "v"),
    ("XM_PROP_BQ2597X_SLAVE_BATTERY_PRESENT", 34, "v"),
    ("XM_PROP_BQ2597X_BATTERY_VOLTAGE", 35, "v"),
    ("XM_PROP_MASTER_SMB1396_ONLINE", 36, "p"), ("XM_PROP_MASTER_SMB1396_IIN", 37, "p"),
    ("XM_PROP_SLAVE_SMB1396_ONLINE", 38, "p"), ("XM_PROP_SLAVE_SMB1396_IIN", 39, "p"),
    ("XM_PROP_SMB_IIN_DIFF", 40, "p"),
    ("XM_PROP_CC_ORIENTATION", 41, "v"), ("XM_PROP_INPUT_SUSPEND", 42, "v"),
    ("XM_PROP_FASTCHGMODE", 43, "v"), ("XM_PROP_NIGHT_CHARGING", 44, "v"),
    ("XM_PROP_BLANK_STATUS", 45, "v"), ("XM_PROP_SCREEN_CCTOG", 46, "v"),
    ("XM_PROP_SOC_DECIMAL", 47, "v"), ("XM_PROP_SOC_DECIMAL_RATE", 48, "v"),
    ("XM_PROP_QUICK_CHARGE_TYPE", 49, "p"), ("XM_PROP_APDO_MAX", 50, "v"),
    ("XM_PROP_POWER_MAX", 51, "v"), ("XM_PROP_DIE_TEMPERATURE", 52, "p"),
    ("XM_PROP_SLAVE_DIE_TEMPERATURE", 53, "p"), ("XM_PROP_FG_RAW_SOC", 54, "v"),
    ("XM_PROP_WLS_START", 55, "p"),
    ("XM_PROP_TX_MACL", 56, "v"), ("XM_PROP_TX_MACH", 57, "v"), ("XM_PROP_RX_CRL", 58, "v"),
    ("XM_PROP_RX_CRH", 59, "v"), ("XM_PROP_RX_CEP", 60, "v"), ("XM_PROP_BT_STATE", 61, "v"),
    ("XM_PROP_REVERSE_CHG_MODE", 62, "v"), ("XM_PROP_REVERSE_CHG_STATE", 63, "v"),
    ("XM_PROP_RX_VOUT", 64, "v"), ("XM_PROP_RX_VRECT", 65, "v"), ("XM_PROP_RX_IOUT", 66, "v"),
    ("XM_PROP_TX_ADAPTER", 67, "v"), ("XM_PROP_OP_MODE", 68, "v"),
    ("XM_PROP_WLS_DIE_TEMP", 69, "v"), ("XM_PROP_WLS_BIN", 70, "v"),
    ("XM_PROP_WLSCHARGE_CONTROL_LIMIT", 71, "v"), ("XM_PROP_FW_VER", 72, "v"),
    ("XM_PROP_WIRELESS_TX_UUID", 73, "v"), ("XM_PROP_WLS_THERMAL_REMOVE", 74, "v"),
    ("XM_PROP_CHG_DEBUG", 75, "v"), ("XM_PROP_WLS_FW_STATE", 76, "v"),
    ("XM_PROP_WLS_CAR_ADAPTER", 77, "v"), ("XM_PROP_WLS_TX_SPEED", 78, "v"),
    ("XM_PROP_WLS_FC_FLAG", 79, "v"), ("XM_PROP_RX_SS", 80, "v"), ("XM_PROP_RX_OFFSET", 81, "v"),
    ("XM_PROP_TX_Q", 82, "v"),
    ("XM_PROP_PEN_HALL3", 88, "v"), ("XM_PROP_PEN_HALL4", 89, "v"),
    ("XM_PROP_SHUTDOWN_DELAY", 94, "v"), ("XM_PROP_FAKE_TEMP", 95, "v"),
    ("XM_PROP_THERMAL_REMOVE", 96, "v"), ("XM_PROP_TYPEC_MODE", 97, "v"),
    ("XM_PROP_MTBF_CURRENT", 98, "v"), ("XM_PROP_THERMAL_TEMP", 99, "v"),
    ("XM_PROP_FB_BLANK_STATE", 100, "p"), ("XM_PROP_SMART_BATT", 101, "v"),
    ("XM_PROP_SHIPMODE_COUNT_RESET", 102, "v"), ("XM_PROP_SPORT_MODE", 103, "p"),
    ("XM_PROP_BATT_CONNT_ONLINE", 104, "v"), ("XM_PROP_FAKE_CYCLE", 105, "v"),
    ("XM_PROP_AFP_TEMP", 106, "v"), ("XM_PROP_PLATE_SHOCK", 107, "v"),
    ("XM_PROP_CC_SHORT_VBUS", 108, "v"), ("XM_PROP_OTG_UI_SUPPORT", 109, "v"),
    ("XM_PROP_CID_STATUS", 110, "v"), ("XM_PROP_CC_TOGGLE", 111, "v"),
    ("XM_PROP_SMART_CHG", 112, "v"), ("XM_PROP_BATTERY_PARAM_RESET", 113, "v"),
    ("XM_PROP_NVTFG_MONITOR_ISC", 114, "v"), ("XM_PROP_NVTFG_MONITOR_SOA", 115, "v"),
    ("XM_PROP_OVER_PEAK_FLAG", 116, "p"), ("XM_PROP_CURRENT_DEVIATION", 117, "p"),
    ("XM_PROP_POWER_DEVIATION", 118, "p"), ("XM_PROP_AVERAGE_CURRENT", 119, "p"),
    ("XM_PROP_AVERAGE_TEMPERATURE", 120, "p"), ("XM_PROP_START_LEARNING", 121, "p"),
    ("XM_PROP_STOP_LEARNING", 122, "p"), ("XM_PROP_SET_LEARNING_POWER", 123, "p"),
    ("XM_PROP_GET_LEARNING_POWER", 124, "p"), ("XM_PROP_GET_LEARNING_POWER_DEV", 125, "p"),
    ("XM_PROP_GET_LEARNING_TIME_DEV", 126, "p"), ("XM_PROP_SET_CONSTANT_POWER", 127, "p"),
    ("XM_PROP_GET_REMAINING_TIME", 128, "p"), ("XM_PROP_SET_REFERANCE_POWER", 129, "p"),
    ("XM_PROP_GET_REFERANCE_CURRENT", 130, "p"), ("XM_PROP_GET_REFERANCE_POWER", 131, "p"),
    ("XM_PROP_START_LEARNING_B", 132, "p"), ("XM_PROP_STOP_LEARNING_B", 133, "p"),
    ("XM_PROP_SET_LEARNING_POWER_B", 134, "p"), ("XM_PROP_GET_LEARNING_POWER_B", 135, "p"),
    ("XM_PROP_GET_LEARNING_POWER_DEV_B", 136, "p"),
    ("XM_PROP_FG1_QMAX", 137, "v"), ("XM_PROP_FG1_RM", 138, "v"), ("XM_PROP_FG1_FCC", 139, "v"),
    ("XM_PROP_FG1_SOH", 140, "v"), ("XM_PROP_FG1_FCC_SOH", 141, "v"),
    ("XM_PROP_FG1_CYCLE", 142, "v"), ("XM_PROP_FG1_FAST_CHARGE", 143, "v"),
    ("XM_PROP_FG1_CURRENT_MAX", 144, "v"), ("XM_PROP_FG1_VOL_MAX", 145, "v"),
    ("XM_PROP_FG1_TSIM", 146, "v"), ("XM_PROP_FG1_TAMBIENT", 147, "v"),
    ("XM_PROP_FG1_TREMQ", 148, "v"), ("XM_PROP_FG1_TFULLQ", 149, "v"),
    ("XM_PROP_FG1_RSOC", 150, "v"), ("XM_PROP_FG1_AI", 151, "v"),
    ("XM_PROP_FG1_CELL1_VOL", 152, "p"), ("XM_PROP_FG1_CELL2_VOL", 153, "p"),
    ("XM_PROP_FG1_TEMP_MAX", 154, "v"), ("XM_PROP_FG1_TIME_HT", 155, "v"),
    ("XM_PROP_FG1_TIME_OT", 156, "v"), ("XM_PROP_FG1_SEAL_SET", 157, "v"),
    ("XM_PROP_FG1_SEAL_STATE", 158, "v"), ("XM_PROP_FG1_DF_CHECK", 159, "v"),
    ("XM_PROP_BATT_SN", 160, "v"), ("XM_PROP_MAX_LIFE_VOL", 161, "v"),
    ("XM_PROP_MAX_LIFE_TEMP", 162, "v"), ("XM_PROP_OVER_VOL_DURATION", 163, "v"),
    ("XM_PROP_BATT_EMERG", 164, "v"), ("XM_PROP_SLAVE_AUTHENTIC", 166, "v"),
    ("XM_PROP_FG_VENDOR_ID", 217, "v"),
    ("XM_PROP_DAM_OVPGATE", 219, "v"), ("XM_PROP_CHARGING_SUSPEND_BATTERY", 220, "v"),
    ("XM_PROP_IS_EU_MODEL", 221, "v"), ("XM_PROP_PPS_PTF", 222, "v"),
    ("XM_PROP_DOD_COUNT", 223, "v"), ("XM_PROP_BATT_CAPACITY_INCREASE", 224, "v"),
    ("XM_PROP_BATT_SPEC", 225, "v"), ("XM_PROP_BATT_SPEC_ENABLE", 226, "v"),
    ("XM_PROP_BATT_DEVICE_NAME", 227, "v"), ("XM_PROP_PACK_VENDOR", 228, "v"),
    ("XM_PROP_LAST_NODE", 229, "v"),
    ("XM_PROP_MAX", 230, "v"),
]


def xm_enum_text():
    out = ["/*\n"
           " * k618: XM property ids in THIS ROM's ADSP charger firmware numbering (OS4.0.24.0 vendor),\n"
           " * read off the phone's own 5.15 qti_battery_charger.ko attribute by attribute. fuxi's\n"
           " * Android-T list is the same list with entries inserted, so every id past XM_PROP_REAL_TYPE\n"
           " * moved -- with fuxi's ids fg_vendor reads FG1_FCC (139) and indexes an 8-entry table with\n"
           " * it. Explicit numbers, keyed by name, never by position:\n"
           " *   v = an immediate in the stock module's code for this name;\n"
           " *   p = position only (fills a gap exactly as wide as fuxi's run there; nothing here\n"
           " *       sends it). Ids 83-87, 90-93, 165, 167-216 and 218 are unused by stock code.\n"
           " * XM_PROP_MAX is the stock prop_count: replies with an id >= it are dropped.\n"
           " */\n",
           "enum xm_property_id {\n"]
    for name, val, how in XM_STOCK:
        tag = "" if name == "XM_PROP_MAX" else f"\t/* {how} */"
        out.append(f"\t{name} = {val},{tag}\n")
    out.append("};\n")
    return "".join(out)


def replace_xm_enum(text):
    m = re.search(r"enum xm_property_id \{.*?\n\};\n", text, re.S)
    if not m:
        return None
    return text[:m.start()] + xm_enum_text() + text[m.end():]


# (old, new): each old must occur exactly once in Xiaomi's file (before the Part A regexes run,
# so new show/store functions are written the 5.15 way and converted with the rest)
B_EDITS = [
    # B6/B7/B8: four fields at the end of struct battery_chg_dev
    ("\t/*shutdown delay is supported, dtsi config*/\n"
     "\tbool\t\t\tshutdown_delay_en;\n"
     "\tbool\t\t\treport_power_absent;\n"
     "};\n",
     "\t/*shutdown delay is supported, dtsi config*/\n"
     "\tbool\t\t\tshutdown_delay_en;\n"
     "\tbool\t\t\treport_power_absent;\n"
     "\t/* k618: as the stock 5.15 module */\n"
     "\tstruct thermal_cooling_device\t*tcd;\t/* \"battery\": mi_thermald's charge limit */\n"
     "\tstruct notifier_block\tcharger_nb;\t\t/* charger_notifier: board temperature */\n"
     "\tint\t\t\tthermal_board_temp;\t/* 0.1 degC, last value sent (XM 8) */\n"
     "\tstruct work_struct\tblank_work;\t\t/* XM_PROP_BLANK_STATUS off the display thread */\n"
     "};\n"),

    # B3: the vendor table stock has (fuxi's had no LISHEN: [6] was NULL)
    ("\t[POWER_SUPPLY_VENDOR_TWS]\t\t= \"TWS\",\n"
     "\t[POWER_SUPPLY_VENDOR_DESAY]\t\t= \"DESAY\",\n",
     "\t[POWER_SUPPLY_VENDOR_TWS]\t\t= \"TWS\",\n"
     "\t[POWER_SUPPLY_VENDOR_LISHEN]\t= \"LISHEN\",\n"
     "\t[POWER_SUPPLY_VENDOR_DESAY]\t\t= \"DESAY\",\n"),

    # B4: the battery model string from the firmware, terminated before strcmp()/sysfs read it
    ("\t\t\tmemcpy(pst->model, model_resp_msg->model, MAX_STR_LEN);\n"
     "\t\t\tack_set = true;\n",
     "\t\t\tmemcpy(pst->model, model_resp_msg->model, MAX_STR_LEN);\n"
     "\t\t\tpst->model[MAX_STR_LEN - 1] = '\\0';\t/* k618 */\n"
     "\t\t\tack_set = true;\n"),

    # B4 + B10: ADSP log with a length, wls_debug terminated, the battery_report push dropped
    ("\t\t\tif (chg_debug_data->type == CHG_ADSP_LOG) {\n"
     "\t\t\t\tpr_err(\"[ADSP] %s\\n\", chg_debug_data->data); // print adsp log\n"
     "\t\t\t} else if (chg_debug_data->type == CHG_WLS_DEBUG) {\n"
     "\t\t\t\tmemcpy(bcdev->wls_debug_data, chg_debug_data->data, CHG_DEBUG_DATA_LEN);\n"
     "\t\t\t\tack_set = true;\n"
     "\t\t\t}\n"
     "\t\t\tbreak;\n"
     "\t\t}\n",
     "\t\t\tif (chg_debug_data->type == CHG_ADSP_LOG) {\n"
     "\t\t\t\t/* k618: the firmware's text need not end in a NUL */\n"
     "\t\t\t\tpr_err(\"[ADSP] %.*s\\n\", CHG_DEBUG_DATA_LEN, chg_debug_data->data);\n"
     "\t\t\t} else if (chg_debug_data->type == CHG_WLS_DEBUG) {\n"
     "\t\t\t\tmemcpy(bcdev->wls_debug_data, chg_debug_data->data, CHG_DEBUG_DATA_LEN);\n"
     "\t\t\t\tbcdev->wls_debug_data[CHG_DEBUG_DATA_LEN - 1] = '\\0';\n"
     "\t\t\t\tack_set = true;\n"
     "\t\t\t}\n"
     "\t\t\tbreak;\n"
     "\t\t}\n"
     "\t\t/*\n"
     "\t\t * k618: the firmware pushes \"battery_report\" (the text right after the header) on\n"
     "\t\t * its own; stock turns it into a BATTERY_DATA_REPORT uevent for miui-services. Not a\n"
     "\t\t * reply to anything we sent: no ack, no \"Incorrect response length\".\n"
     "\t\t */\n"
     "\t\tif (len >= sizeof(struct pmic_glink_hdr) + 14 &&\n"
     "\t\t    !strncmp((const char *)data + sizeof(struct pmic_glink_hdr),\n"
     "\t\t\t     \"battery_report\", 14)) {\n"
     "\t\t\tpr_debug(\"battery_report from the firmware (%zu bytes) dropped\\n\", len);\n"
     "\t\t\tbreak;\n"
     "\t\t}\n"),

    # B3: typec_mode indexes a 10-entry table with the firmware's value
    ("\treturn scnprintf(buf, PAGE_SIZE, \"%s\\n\", "
     "power_supply_usbc_text[pst->prop[XM_PROP_TYPEC_MODE]]);\n",
     "\t/* k618: bounds-checked (stock: brk #0x5512 past entry 9) */\n"
     "\tif (pst->prop[XM_PROP_TYPEC_MODE] >= ARRAY_SIZE(power_supply_usbc_text))\n"
     "\t\treturn scnprintf(buf, PAGE_SIZE, \"Unknown\\n\");\n"
     "\treturn scnprintf(buf, PAGE_SIZE, \"%s\\n\", "
     "power_supply_usbc_text[pst->prop[XM_PROP_TYPEC_MODE]]);\n"),

    # B4: the wireless chip firmware version (16 bytes from the firmware, maybe no NUL)
    ("\treturn scnprintf(buf, PAGE_SIZE, \"%s\\n\", bcdev->wireless_chip_fw_version);\n",
     "\treturn scnprintf(buf, PAGE_SIZE, \"%.*s\\n\", WIRELESS_CHIP_FW_VERSION_LEN,\n"
     "\t\t\t bcdev->wireless_chip_fw_version);\n"),

    # B5: count user bytes into a 200-byte stack array -> FORTIFY panic on 6.18
    ("\tmemset(req_msg.data, '\\0', sizeof(req_msg.data));\n"
     "\tstrncpy(req_msg.data, buf, count);\n",
     "\tmemset(req_msg.data, '\\0', sizeof(req_msg.data));\n"
     "\t/* k618: at most 199 bytes -- the array is 200, the rest stays NUL */\n"
     "\tmemcpy(req_msg.data, buf, min_t(size_t, count, sizeof(req_msg.data) - 1));\n"),

    # B5: batterysecret's digest -- 70-byte stack copy of count - 1 bytes, then StringToHex into a
    # 33-byte array (stock traps in the same place, brk #1); 64 hex digits is all there is
    ("\tmemset(kbuf_1s, 0, sizeof(kbuf_1s));\n"
     "\tstrncpy(kbuf_1s, buf, count - 1);\n",
     "\tmemset(kbuf_1s, 0, sizeof(kbuf_1s));\n"
     "\t/* k618: at most 64 hex digits (kbuf_1s is 70 bytes, random_1s 32 + 1) */\n"
     "\tstrncpy(kbuf_1s, buf, min_t(size_t, count - 1, BATTERY_DIGEST_LEN * 2));\n"),

    # B5: request_vdm_cmd: "%s" into a 64-byte stack buffer, then StringToHex into 32 bytes
    ("\tunsigned char buffer[64];\n",
     "\tunsigned char buffer[64] = {0};\t/* k618: stock zeroes it too */\n"),
    ("\tret = sscanf(buf, \"%d,%s\\n\", &cmd, buffer);\n",
     "\tret = sscanf(buf, \"%d,%63s\\n\", &cmd, buffer);\t/* k618: bounded */\n"),

    # B4
    ("\treturn scnprintf(buf, PAGE_SIZE, \"%s\", bcdev->wls_debug_data);\n",
     "\treturn scnprintf(buf, PAGE_SIZE, \"%.*s\", CHG_DEBUG_DATA_LEN, bcdev->wls_debug_data);\n"),

    # B2: fg_vendor as stock prints it -- the number (hwinfo reads it), no table
    ("\tconst char* prop = NULL;\n"
     "\tint rc;\n"
     "\n"
     "\trc = read_property_id(bcdev, pst, XM_PROP_FG_VENDOR_ID);\n"
     "\tif (rc < 0)\n"
     "\t\treturn rc;\n"
     "\tprop = POWER_SUPPLY_VENDOR_TEXT[pst->prop[XM_PROP_FG_VENDOR_ID]];\n"
     "\treturn scnprintf(buf, PAGE_SIZE, \"%s\\n\", prop);\n",
     """	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_FG_VENDOR_ID);
	if (rc < 0)
		return rc;
	/*
	 * k618: the number, as the stock module prints it (\"%d\\n\"; 5.15 shows 2). fuxi indexed
	 * POWER_SUPPLY_VENDOR_TEXT with it (a UBSAN bounds trap). The name is pack_vendor_str.
	 */
	return scnprintf(buf, PAGE_SIZE, \"%d\\n\", pst->prop[XM_PROP_FG_VENDOR_ID]);
"""),

    # B8: the panel notifier hands the screen state to the firmware, as stock does
    ("\tpr_info(\"%s, blank_state = %d\\n\", __func__, bcdev->blank_state);\n"
     "}\n",
     "\tpr_info(\"%s, blank_state = %d\\n\", __func__, bcdev->blank_state);\n"
     "\t/* k618: stock writes XM_PROP_BLANK_STATUS here; we do it from a work item */\n"
     "\tschedule_work(&bcdev->blank_work);\n"
     "}\n"),

    # B6 B7 B9: the new functions and attributes, before the attribute list
    ("static struct attribute *battery_class_attrs[] = {\n",
     None),  # filled in below (NEW_CODE)

    # B9: the new attributes in the list
    ("\t&class_attr_fg1_df_check.attr,\n"
     "\tNULL,\n",
     "\t&class_attr_fg1_df_check.attr,\n"
     "\t/* k618: from the stock module (ids in the enum above) */\n"
     "\t&class_attr_thermal_board_temp.attr,\n"
     "\t&class_attr_is_eu_model.attr,\n"
     "\t&class_attr_pack_vendor_id.attr,\n"
     "\t&class_attr_pack_vendor_str.attr,\n"
     "\tNULL,\n"),

    # B8: the work item, before the panel notifier can fire
    ("\tINIT_DELAYED_WORK( &bcdev->batt_update_work, xm_batt_update_work);\n",
     "\tINIT_DELAYED_WORK( &bcdev->batt_update_work, xm_batt_update_work);\n"
     "\tINIT_WORK(&bcdev->blank_work, battery_chg_blank_work);\t/* k618 */\n"),

    # B6 + B7: cooling device and charger notifier at the end of probe; B11
    ("\tbcdev->wls_fw_update_time_ms = WLS_FW_UPDATE_TIME_MS;\n"
     "\tbattery_chg_add_debugfs(bcdev);\n",
     "\t/*\n"
     "\t * k618: the \"battery\" cooling device, as the stock module (and soc's driver) register it.\n"
     "\t * 5.15's power-supply core made one for every supply with CHARGE_CONTROL_LIMIT; 6.18's\n"
     "\t * does not, and mi_thermald limits charging only through this device. Not fatal: without\n"
     "\t * it the battery still works, unthrottled by mi_thermald.\n"
     "\t */\n"
     "\tbcdev->tcd = devm_thermal_of_cooling_device_register(dev, dev->of_node,\n"
     "\t\t\t(char *)bcdev->psy_list[PSY_TYPE_BATTERY].psy->desc->name,\n"
     "\t\t\tbcdev, &battery_tcd_ops);\n"
     "\tif (IS_ERR_OR_NULL(bcdev->tcd)) {\n"
     "\t\tdev_err(dev, \"Failed to register thermal cooling device rc=%ld\\n\",\n"
     "\t\t\tPTR_ERR(bcdev->tcd));\n"
     "\t\tbcdev->tcd = NULL;\n"
     "\t}\n"
     "\n"
     "\t/* k618: mi_thermal's board temperature, as stock (charger_notifier_event) */\n"
     "\tbcdev->charger_nb.notifier_call = charger_notifier_event;\n"
     "\tcharger_reg_notifier(&bcdev->charger_nb);\n"
     "\n"
     "\tbcdev->wls_fw_update_time_ms = WLS_FW_UPDATE_TIME_MS;\n"
     "\tbattery_chg_add_debugfs(bcdev);\n"),
    ("\tdev_err(dev, \"battery_chg probe done %d\\n\");\n",
     "\tdev_err(dev, \"battery_chg probe done\\n\");\t/* k618: the %d had no argument */\n"),

    # remove(): undo B7/B8
    ("\tif (bcdev->notifier_cookie)\n"
     "\t\tpanel_event_notifier_unregister(bcdev->notifier_cookie);\n"
     "\n"
     "\tdevice_init_wakeup(bcdev->dev, false);\n",
     "\tif (bcdev->notifier_cookie)\n"
     "\t\tpanel_event_notifier_unregister(bcdev->notifier_cookie);\n"
     "\tcharger_unreg_notifier(&bcdev->charger_nb);\t/* k618 */\n"
     "\tcancel_work_sync(&bcdev->blank_work);\t\t/* k618 */\n"
     "\n"
     "\tdevice_init_wakeup(bcdev->dev, false);\n"),
]

NEW_CODE = r'''/*
 * k618: what the stock 5.15 module has besides fuxi's attributes.
 */

/* B6: the "battery" cooling device -- 16 levels, the level number itself goes to the firmware */
static int battery_chg_get_max_charge_cntl_limit(struct thermal_cooling_device *tcd,
						 unsigned long *state)
{
	struct battery_chg_dev *bcdev = tcd->devdata;

	*state = bcdev->num_thermal_levels;
	return 0;
}

static int battery_chg_get_cur_charge_cntl_limit(struct thermal_cooling_device *tcd,
						 unsigned long *state)
{
	struct battery_chg_dev *bcdev = tcd->devdata;

	*state = bcdev->curr_thermal_level;
	return 0;
}

static int battery_chg_set_cur_charge_cntl_limit(struct thermal_cooling_device *tcd,
						 unsigned long state)
{
	struct battery_chg_dev *bcdev = tcd->devdata;

	if (state > INT_MAX)
		return -EINVAL;
	return battery_psy_set_charge_current(bcdev, (int)state);
}

static const struct thermal_cooling_device_ops battery_tcd_ops = {
	.get_max_state = battery_chg_get_max_charge_cntl_limit,
	.get_cur_state = battery_chg_get_cur_charge_cntl_limit,
	.set_cur_state = battery_chg_set_cur_charge_cntl_limit,
};

/*
 * B7: Xiaomi's charger notifier chain: the three calls the stock module exports (stock exports
 * the chain head as well; nothing on 6.18 uses it, so it stays static). k618_mi_thermal
 * symbol_get()s charger_notifier_call_chain and calls it from board_sensor_temp_store with event 0
 * and the board temperature in tenths of a degree (stock mi_thermal_interface divides mi_thermald's
 * milli-degrees by 100); the firmware gets it as XM_PROP_THERMAL_BOARD_TEMP.
 */
#define CHARGER_EVENT_BOARD_TEMP	0
static BLOCKING_NOTIFIER_HEAD(charger_notifier);

int charger_reg_notifier(struct notifier_block *nb);
int charger_unreg_notifier(struct notifier_block *nb);
int charger_notifier_call_chain(unsigned long event, int val);

int charger_reg_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&charger_notifier, nb);
}
EXPORT_SYMBOL(charger_reg_notifier);

int charger_unreg_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&charger_notifier, nb);
}
EXPORT_SYMBOL(charger_unreg_notifier);

int charger_notifier_call_chain(unsigned long event, int val)
{
	return blocking_notifier_call_chain(&charger_notifier, event, &val);
}
EXPORT_SYMBOL(charger_notifier_call_chain);

static int charger_notifier_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct battery_chg_dev *bcdev = container_of(nb, struct battery_chg_dev, charger_nb);
	int rc;

	if (event != CHARGER_EVENT_BOARD_TEMP || !data) {
		pr_debug("charger notifier event %lu ignored\n", event);
		return NOTIFY_DONE;
	}

	bcdev->thermal_board_temp = *(int *)data;
	pr_debug("thermal_board_temp: %d\n", bcdev->thermal_board_temp);
	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_THERMAL_BOARD_TEMP, bcdev->thermal_board_temp);
	if (rc < 0)
		pr_err("Failed to send thermal_board_temp, rc=%d\n", rc);

	return NOTIFY_DONE;
}

/* B8: the screen state to the firmware (stock: from the panel callback itself) */
static void battery_chg_blank_work(struct work_struct *work)
{
	struct battery_chg_dev *bcdev = container_of(work, struct battery_chg_dev, blank_work);
	int rc;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_BLANK_STATUS, bcdev->blank_state);
	if (rc < 0)
		pr_err("Failed to send blank_status %d, rc=%d\n", bcdev->blank_state, rc);
}

/* B7: stock's thermal_board_temp attribute (XM 8): show reads, then prints the last value sent */
static ssize_t thermal_board_temp_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	int val, rc;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_THERMAL_BOARD_TEMP, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t thermal_board_temp_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_THERMAL_BOARD_TEMP);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d", bcdev->thermal_board_temp);
}
static CLASS_ATTR_RW(thermal_board_temp);

/* B9: is_eu_model (XM 221) -- batterysecret reads it */
static ssize_t is_eu_model_store(struct class *c,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	bool val;
	int rc;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	rc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM],
			       XM_PROP_IS_EU_MODEL, val);
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t is_eu_model_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_IS_EU_MODEL);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%u\n", pst->prop[XM_PROP_IS_EU_MODEL]);
}
static CLASS_ATTR_RW(is_eu_model);

/* B9: the pack vendor (XM 228) as a number and as a name -- the micharge HAL reads the name */
static ssize_t pack_vendor_id_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_PACK_VENDOR);
	if (rc < 0)
		return rc;

	return scnprintf(buf, PAGE_SIZE, "%d\n", pst->prop[XM_PROP_PACK_VENDOR]);
}
static CLASS_ATTR_RO(pack_vendor_id);

static ssize_t pack_vendor_str_show(struct class *c,
					struct class_attribute *attr, char *buf)
{
	struct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,
						battery_class);
	struct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];
	u32 id;
	int rc;

	rc = read_property_id(bcdev, pst, XM_PROP_PACK_VENDOR);
	if (rc < 0)
		return rc;

	/* bounds-checked: stock traps (brk #0x5512) past entry 7 */
	id = pst->prop[XM_PROP_PACK_VENDOR];
	if (id >= ARRAY_SIZE(POWER_SUPPLY_VENDOR_TEXT) || !POWER_SUPPLY_VENDOR_TEXT[id])
		return scnprintf(buf, PAGE_SIZE, "Unknown\n");

	return scnprintf(buf, PAGE_SIZE, "%s\n", POWER_SUPPLY_VENDOR_TEXT[id]);
}
static CLASS_ATTR_RO(pack_vendor_str);

'''

# ------------------------------------------------------------------------------------------------
# The 13 Ultra's own nodes. Its stock qti_battery_charger.ko (ishtar OS3.0.308.0.WMACNXM, read the
# same way as this phone's, function by function) sends the same id as this phone's for every node
# both have, and the 13's (fuxi OS3.0.309.0.WMCCNXM) is this phone's exactly. The 13 Ultra adds the
# NVT fuel gauge's learning nodes on 116-136 -- the enum positions above that were "p" (position
# only) here, now confirmed by its code -- and has_dp on 218. Each is a plain get (and set):
# "%d" and kstrtoint, as its stock functions. Nothing on the 13 or 13 Pro reads them.
# ------------------------------------------------------------------------------------------------
K618_ULTRA_NODES = [
    # (node, id, writable)
    ("over_peak_flag", "XM_PROP_OVER_PEAK_FLAG", False),
    ("current_deviation", "XM_PROP_CURRENT_DEVIATION", False),
    ("power_deviation", "XM_PROP_POWER_DEVIATION", False),
    ("average_current", "XM_PROP_AVERAGE_CURRENT", False),
    ("average_temp", "XM_PROP_AVERAGE_TEMPERATURE", False),
    ("start_learn", "XM_PROP_START_LEARNING", True),
    ("stop_learn", "XM_PROP_STOP_LEARNING", True),
    ("set_learn_power", "XM_PROP_SET_LEARNING_POWER", True),
    ("get_learn_power", "XM_PROP_GET_LEARNING_POWER", False),
    ("get_learn_power_dev", "XM_PROP_GET_LEARNING_POWER_DEV", False),
    ("get_learn_time_dev", "XM_PROP_GET_LEARNING_TIME_DEV", False),
    ("constant_power", "XM_PROP_SET_CONSTANT_POWER", True),
    ("remaining_time", "XM_PROP_GET_REMAINING_TIME", False),
    ("referance_power", "XM_PROP_SET_REFERANCE_POWER", True),
    ("nvt_referance_current", "XM_PROP_GET_REFERANCE_CURRENT", False),
    ("nvt_referance_power", "XM_PROP_GET_REFERANCE_POWER", False),
    ("start_learn_b", "XM_PROP_START_LEARNING_B", True),
    ("stop_learn_b", "XM_PROP_STOP_LEARNING_B", True),
    ("set_learn_power_b", "XM_PROP_SET_LEARNING_POWER_B", True),
    ("get_learn_power_b", "XM_PROP_GET_LEARNING_POWER_B", False),
    ("get_learn_power_dev_b", "XM_PROP_GET_LEARNING_POWER_DEV_B", False),
    ("has_dp", "K618_XM_PROP_HAS_DP", False),
]


def ultra_node(name, pid, rw):
    s = (f"static ssize_t {name}_show(struct class *c,\n"
         "\t\t\tstruct class_attribute *attr, char *buf)\n"
         "{\n"
         "\tstruct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,\n"
         "\t\t\t\t\t\tbattery_class);\n"
         "\tstruct psy_state *pst = &bcdev->psy_list[PSY_TYPE_XM];\n"
         "\tint rc;\n\n"
         f"\trc = read_property_id(bcdev, pst, {pid});\n"
         "\tif (rc < 0)\n\t\treturn rc;\n\n"
         f"\treturn scnprintf(buf, PAGE_SIZE, \"%d\\n\", pst->prop[{pid}]);\n"
         "}\n")
    if rw:
        s += (f"\nstatic ssize_t {name}_store(struct class *c,\n"
              "\t\t\tstruct class_attribute *attr, const char *buf, size_t count)\n"
              "{\n"
              "\tstruct battery_chg_dev *bcdev = container_of(c, struct battery_chg_dev,\n"
              "\t\t\t\t\t\tbattery_class);\n"
              "\tint rc, val;\n\n"
              "\tif (kstrtoint(buf, 10, &val))\t/* decimal, as the 13 Ultra's source */\n\t\treturn -EINVAL;\n"
              f"\trc = write_property_id(bcdev, &bcdev->psy_list[PSY_TYPE_XM], {pid}, val);\n"
              "\tif (rc < 0)\n\t\treturn rc;\n\n"
              "\treturn count;\n"
              "}\n")
    return s + f"static CLASS_ATTR_{'RW' if rw else 'RO'}({name});\n\n"


NEW_CODE += ("/* k618: the 13 Ultra's NVT fuel gauge nodes and has_dp (its stock ids) */\n"
             "#define K618_XM_PROP_HAS_DP\t218\n\n"
             + "".join(ultra_node(*n) for n in K618_ULTRA_NODES))

# fill the placeholder: the new code goes right before the attribute list
B_EDITS = [(o, n if n is not None else NEW_CODE + o) for (o, n) in B_EDITS]
B_EDITS.append(("\t&class_attr_pack_vendor_str.attr,\n\tNULL,\n",
                "\t&class_attr_pack_vendor_str.attr,\n"
                "\t/* k618: the 13 Ultra's */\n"
                + "".join(f"\t&class_attr_{n}.attr,\n" for n, _, _ in K618_ULTRA_NODES)
                + "\tNULL,\n"))

# panel events for the charger (k618_registered_panel, retry; see the C comment)
B_EDITS += [
    # struct field
    ('\tstruct work_struct\tblank_work;\t\t/* XM_PROP_BLANK_STATUS off the display thread */\n',
     '\tstruct work_struct\tblank_work;\t\t/* XM_PROP_BLANK_STATUS off the display thread */\n\tstruct delayed_work\tk618_panel_dw;\t\t/* k618: panel events once msm_drm is up */\n\tint\t\t\tk618_panel_tries;\n'),
    # register function
    ('static int battery_chg_register_panel_notifier(struct battery_chg_dev *bcdev)\n{\n\t//struct device_node *np = bcdev->dev->of_node;\n\tstruct device_node *pnode;\n\tstruct drm_panel *panel, *active_panel = NULL;\n\tvoid *cookie = NULL;\n\tint i, count, rc = 0;\n\n\tpnode = of_find_node_by_name(NULL, "charge-screen");\n\tif (!pnode) {\n\t\tpr_err("%s ERROR: Cannot find node with panel!", __func__);\n\t\treturn 0;\n\t}\n\tcount = of_count_phandle_with_args(pnode, "panel", NULL);\n\tif (count <= 0)\n\t\treturn 0;\n\n\tfor (i = 0; i < count; i++) {\n\t\tpnode = of_parse_phandle(pnode, "panel", i);\n\t\tif (!pnode) {\n\t\t\tpr_err("%s ERROR: Cannot fine panel of node!", __func__);\n\t\t\treturn 0;\n\t\t}\n\n\t\tpanel = of_drm_find_panel(pnode);\n\t\tof_node_put(pnode);\n\t\tif (!IS_ERR(panel)) {\n\t\t\tactive_panel = panel;\n\t\t\tbreak;\n\t\t}\n\t}\n\n\tif (!active_panel) {\n\t\trc = PTR_ERR(panel);\n\t\tif (rc != -EPROBE_DEFER)\n\t\t\tdev_err(bcdev->dev, "Failed to find active panel, rc=%d\\n",rc);\n\t\treturn 0;\n\t}\n\n\tcookie = panel_event_notifier_register(\n\t\t\tPANEL_EVENT_NOTIFICATION_PRIMARY,\n\t\t\tPANEL_EVENT_NOTIFIER_CLIENT_BATTERY_CHARGER,\n\t\t\tactive_panel,\n\t\t\tpanel_event_notifier_callback,\n\t\t\t(void *)bcdev);\n\tif (IS_ERR(cookie)) {\n\t\trc = PTR_ERR(cookie);\n\t\tdev_err(bcdev->dev, "Failed to register panel event notifier, rc=%d\\n", rc);\n\t}\n\n\tpr_err("register panel notifier successful\\n");\n\tbcdev->notifier_cookie = cookie;\n\treturn 0;\n}\n',
     '/*\n * k618: the panel this phone has, when none of a node\'s "panel" phandles is registered -- the\n * 13 Pro\'s second panel (42 02 0b) is in none of the DT\'s touch / charge-screen / thermal-screen\n * lists. Only the active panel is registered with drm, so the first one found among the DSI\n * panel nodes is it.\n */\nstatic struct drm_panel *k618_registered_panel(void)\n{\n\tstruct device_node *np;\n\tstruct drm_panel *panel;\n\n\tfor_each_node_with_property(np, "qcom,mdss-dsi-panel-name") {\n\t\tpanel = of_drm_find_panel(np);\n\t\tif (!IS_ERR(panel)) {\n\t\t\tof_node_put(np);\n\t\t\treturn panel;\n\t\t}\n\t}\n\treturn NULL;\n}\n\n/*\n * k618: msm_drm loads after this driver (batch 248 against 106), so at probe no panel is\n * registered yet and the stock code gave up for good: the charger never saw a screen event.\n * Now it tries again every 5 s for 5 minutes. The stock loop also overwrote the charge-screen\n * node with the first phandle, so a second entry was looked up under the first panel.\n */\nstatic int battery_chg_register_panel_notifier(struct battery_chg_dev *bcdev)\n{\n\tstruct device_node *pnode, *node;\n\tstruct drm_panel *panel = ERR_PTR(-ENODEV), *active_panel = NULL;\n\tvoid *cookie = NULL;\n\tint i, count = 0, rc = 0;\n\n\tif (bcdev->notifier_cookie)\n\t\treturn 0;\n\tpnode = of_find_node_by_name(NULL, "charge-screen");\n\tif (pnode)\n\t\tcount = of_count_phandle_with_args(pnode, "panel", NULL);\n\tfor (i = 0; i < count && !active_panel; i++) {\n\t\tnode = of_parse_phandle(pnode, "panel", i);\n\t\tif (!node)\n\t\t\tcontinue;\n\t\tpanel = of_drm_find_panel(node);\n\t\tof_node_put(node);\n\t\tif (!IS_ERR(panel))\n\t\t\tactive_panel = panel;\n\t}\n\tof_node_put(pnode);\n\tif (!active_panel) {\n\t\tactive_panel = k618_registered_panel();\n\t\tif (active_panel)\n\t\t\tdev_info(bcdev->dev, "k618: panel events from the registered panel (not in charge-screen)\\n");\n\t}\n\tif (!active_panel) {\n\t\trc = PTR_ERR(panel);\n\t\tif (bcdev->k618_panel_tries++ < 60)\n\t\t\tschedule_delayed_work(&bcdev->k618_panel_dw, msecs_to_jiffies(5000));\n\t\telse\n\t\t\tdev_err(bcdev->dev, "Failed to find active panel, rc=%d\\n", rc);\n\t\treturn 0;\n\t}\n\n\tcookie = panel_event_notifier_register(\n\t\t\tPANEL_EVENT_NOTIFICATION_PRIMARY,\n\t\t\tPANEL_EVENT_NOTIFIER_CLIENT_BATTERY_CHARGER,\n\t\t\tactive_panel,\n\t\t\tpanel_event_notifier_callback,\n\t\t\t(void *)bcdev);\n\tif (IS_ERR(cookie)) {\n\t\trc = PTR_ERR(cookie);\n\t\tdev_err(bcdev->dev, "Failed to register panel event notifier, rc=%d\\n", rc);\n\t\treturn 0;\n\t}\n\n\tdev_info(bcdev->dev, "register panel notifier successful (try %d)\\n", bcdev->k618_panel_tries);\n\tbcdev->notifier_cookie = cookie;\n\treturn 0;\n}\n\nstatic void k618_bc_panel_fn(struct work_struct *work)\n{\n\tstruct battery_chg_dev *bcdev = container_of(to_delayed_work(work), struct battery_chg_dev,\n\t\t\t\t\t\t     k618_panel_dw);\n\n\tbattery_chg_register_panel_notifier(bcdev);\n}\n'),
    # init
    ('\tINIT_WORK(&bcdev->blank_work, battery_chg_blank_work);\t/* k618 */\n',
     '\tINIT_WORK(&bcdev->blank_work, battery_chg_blank_work);\t/* k618 */\n\tINIT_DELAYED_WORK(&bcdev->k618_panel_dw, k618_bc_panel_fn);\t/* k618 */\n'),
    # remove
    ('\tif (bcdev->notifier_cookie)\n\t\tpanel_event_notifier_unregister(bcdev->notifier_cookie);\n\tcharger_unreg_notifier(&bcdev->charger_nb);\t/* k618 */\n',
     '\tcancel_delayed_work_sync(&bcdev->k618_panel_dw);\t/* k618 */\n\tif (bcdev->notifier_cookie)\n\t\tpanel_event_notifier_unregister(bcdev->notifier_cookie);\n\tcharger_unreg_notifier(&bcdev->charger_nb);\t/* k618 */\n'),
]

# --mi-thermal: the unit k618_mi_thermal.c hands over (stock divides by 100)
MI_THERMAL_EDITS = [
    ("\t\tret = k618_charger_notify_fn(MI_CHARGER_EVENT_BOARD_TEMP, millicelsius / 1000);\n",
     "\t\t/* tenths of a degree: stock divides by 100 (0x51eb851f, asr #37 at 0xb98) */\n"
     "\t\tret = k618_charger_notify_fn(MI_CHARGER_EVENT_BOARD_TEMP, millicelsius / 100);\n"),
]


def void_remove(text, func):
    sig = f"static int {func}(struct platform_device *pdev)"
    if f"static void {func}(" in text:
        return text
    if sig not in text:
        print(f"k618_mi_charger: no {sig}", file=sys.stderr)
        return text
    start = text.index(sig)
    end = text.index("\n}\n", start) + 3
    body = text[start:end].replace(sig, f"static void {func}(struct platform_device *pdev)")
    body = re.sub(r"\n\treturn 0;\n\}\n$", "\n}\n", body)
    return text[:start] + body + text[end:]


def apply_literal(text, edits, label):
    ok = True
    for old, new in edits:
        n = text.count(old)
        if n != 1:
            print(f"k618_mi_charger: {label}: expected the text once, found it {n} times: "
                  f"{old[:70]!r}", file=sys.stderr)
            ok = False
            continue
        text = text.replace(old, new)
    return text, ok


def port(fuxi):
    src = os.path.join(fuxi, CHG)
    text = open(src, encoding="utf-8", errors="surrogateescape").read()
    text = replace_xm_enum(text)
    if text is None:
        print("k618_mi_charger: enum xm_property_id not found", file=sys.stderr)
        return None
    text, ok_b = apply_literal(text, B_EDITS, "B")
    text, ok_a = apply_literal(text, A_EDITS, "A")
    ok = ok_a and ok_b
    for pat, new in RE_EDITS:
        text, n = pat.subn(new, text)
        if not n:
            print(f"k618_mi_charger: no match for {pat.pattern!r}", file=sys.stderr)
            ok = False
    for func in VOID_REMOVE:
        text = void_remove(text, func)
    return text if ok else None


def kconfig_block(text, opt):
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() in (f"config {opt}", f"menuconfig {opt}"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith(("config ", "menuconfig ", "endif",
                                                              "endmenu", "menu ", "if ", "source ",
                                                              "choice", "comment ")):
                j += 1
            return "\n".join(lines[i:j]).rstrip() + "\n"
    return None


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in argv[1:] if a.startswith("--") and "=" in a)
    if len(args) < 2:
        print(__doc__.split("\n")[0], file=sys.stderr)
        return 2
    fuxi, soc = args[0].rstrip("/"), args[1].rstrip("/")
    text = port(fuxi)
    if text is None:
        print("k618_mi_charger: left alone", file=sys.stderr)
        return 1
    dst = opts.get("out") or os.path.join(soc, CHG)
    changed = 0
    if not (os.path.exists(dst) and
            open(dst, encoding="utf-8", errors="surrogateescape").read() == text):
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
        print(f"k618_mi_charger: {dst}: Xiaomi's driver, stock XM numbering "
              f"({len(text.splitlines())} lines)")
        changed += 1
    if "out" not in opts:
        for kc, opt in KCONFIG:
            path = os.path.join(soc, kc)
            if kconfig_block(open(path, encoding="utf-8").read(), opt):
                continue
            blk = kconfig_block(open(os.path.join(fuxi, kc), encoding="utf-8",
                                     errors="replace").read(), opt)
            if not blk:
                print(f"k618_mi_charger: {opt} is not in Xiaomi's {kc}", file=sys.stderr)
                return 1
            with open(path, "a", encoding="utf-8", newline="\n") as f:
                f.write(f"\n# from Xiaomi's 13 Pro tree (fuxi-t-oss)\n{blk}")
            print(f"k618_mi_charger: {kc}: {opt}")
            changed += 1
    mt = opts.get("mi-thermal")
    if mt:
        t = open(mt, encoding="utf-8").read()
        if "millicelsius / 100);" not in t:
            t, ok = apply_literal(t, MI_THERMAL_EDITS, "mi-thermal")
            if not ok:
                return 1
            open(mt, "w", encoding="utf-8", newline="").write(t)
            print(f"k618_mi_charger: {mt}: board temperature in tenths of a degree")
            changed += 1
    print(f"k618_mi_charger: {changed} files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
