

# ipa_5_5_mem_part, field by field: (field, 6.12 release value, kalama value = 5.15 source =
# the phone's own ipam.ko). Same field order as both initializers.
PART = [
    ("uc_descriptor_ram_ofst",      "0x0",    "0x0"),
    ("uc_descriptor_ram_size",      "0x300",  "0x1000"),
    ("uc_ofst",                     "0x300",  "0x1000"),
    ("uc_size",                     "0x80",   "0x80"),
    ("uc_info_ofst",                "0x380",  "0x1080"),
    ("uc_info_size",                "0x80",   "0x200"),
    ("ofst_start",                  "0x400",  "0x1280"),
    ("v4_flt_hash_ofst",            "0x408",  "0x1288"),
    ("v4_flt_hash_size",            "0x78",   "0x78"),
    ("v4_flt_hash_size_ddr",        "0x4000", "0x4000"),
    ("v4_flt_nhash_ofst",           "0x488",  "0x1308"),
    ("v4_flt_nhash_size",           "0x78",   "0x78"),
    ("v4_flt_nhash_size_ddr",       "0x4000", "0x4000"),
    ("v6_flt_hash_ofst",            "0x508",  "0x1388"),
    ("v6_flt_hash_size",            "0x78",   "0x78"),
    ("v6_flt_hash_size_ddr",        "0x4000", "0x4000"),
    ("v6_flt_nhash_ofst",           "0x588",  "0x1408"),
    ("v6_flt_nhash_size",           "0x78",   "0x78"),
    ("v6_flt_nhash_size_ddr",       "0x4000", "0x4000"),
    ("v4_rt_num_index",             "0x13",   "0x13"),
    ("v4_modem_rt_index_lo",        "0x0",    "0x0"),
    ("v4_modem_rt_index_hi",        "0xa",    "0xa"),
    ("v4_apps_rt_index_lo",         "0xb",    "0xb"),
    ("v4_apps_rt_index_hi",         "0x12",   "0x12"),
    ("v4_rt_hash_ofst",             "0x608",  "0x1488"),
    ("v4_rt_hash_size",             "0x98",   "0x98"),
    ("v4_rt_hash_size_ddr",         "0x4000", "0x4000"),
    ("v4_rt_nhash_ofst",            "0x6A8",  "0x1528"),
    ("v4_rt_nhash_size",            "0x98",   "0x98"),
    ("v4_rt_nhash_size_ddr",        "0x4000", "0x4000"),
    ("v6_rt_num_index",             "0x13",   "0x13"),
    ("v6_modem_rt_index_lo",        "0x0",    "0x0"),
    ("v6_modem_rt_index_hi",        "0xa",    "0xa"),
    ("v6_apps_rt_index_lo",         "0xb",    "0xb"),
    ("v6_apps_rt_index_hi",         "0x12",   "0x12"),
    ("v6_rt_hash_ofst",             "0x748",  "0x15c8"),
    ("v6_rt_hash_size",             "0x98",   "0x98"),
    ("v6_rt_hash_size_ddr",         "0x4000", "0x4000"),
    ("v6_rt_nhash_ofst",            "0x7E8",  "0x1668"),
    ("v6_rt_nhash_size",            "0x098",  "0x098"),
    ("v6_rt_nhash_size_ddr",        "0x4000", "0x4000"),
    ("modem_hdr_ofst",              "0x888",  "0x1708"),
    ("modem_hdr_size",              "0x240",  "0x240"),
    ("apps_hdr_ofst",               "0xAC8",  "0x1948"),
    ("apps_hdr_size",               "0x1e0",  "0x1e0"),
    ("apps_hdr_size_ddr",           "0x800",  "0x800"),
    ("modem_hdr_proc_ctx_ofst",     "0xCC0",  "0x1b40"),
    ("modem_hdr_proc_ctx_size",     "0xb20",  "0xb20"),
    ("apps_hdr_proc_ctx_ofst",      "0x17E0", "0x2660"),
    ("apps_hdr_proc_ctx_size",      "0x200",  "0x200"),
    ("apps_hdr_proc_ctx_size_ddr",  "0x0",    "0x0"),
    ("stats_quota_q6_ofst",         "0x19E8", "0x2868"),
    ("stats_quota_q6_size",         "0x60",   "0x60"),
    ("stats_quota_ap_ofst",         "0x1A48", "0x28C8"),
    ("stats_quota_ap_size",         "0x48",   "0x48"),
    ("stats_tethering_ofst",        "0x1A90", "0x2910"),
    ("stats_tethering_size",        "0x3c0",  "0x3c0"),
    ("stats_flt_v4_ofst",           "0",      "0"),
    ("stats_flt_v4_size",           "0",      "0"),
    ("stats_flt_v6_ofst",           "0",      "0"),
    ("stats_flt_v6_size",           "0",      "0"),
    ("stats_rt_v4_ofst",            "0",      "0"),
    ("stats_rt_v4_size",            "0",      "0"),
    ("stats_rt_v6_ofst",            "0",      "0"),
    ("stats_rt_v6_size",            "0",      "0"),
    ("stats_fnr_ofst",              "0x1E50", "0x2cd0"),
    ("stats_fnr_size",              "0x1000", "0xba0"),
    ("stats_drop_ofst",             "0x2E50", "0x3870"),
    ("stats_drop_size",             "0x20",   "0x20"),
    ("modem_comp_decomp_ofst",      "0x0",    "0x0"),
    ("modem_comp_decomp_size",      "0x0",    "0x0"),
    ("modem_ofst",                  "0x2E78", "0x3898"),
    ("modem_size",                  "0xd48",  "0xd48"),
    ("nat_tbl_ofst",                "0x3BC0", "0x45e0"),
    ("nat_tbl_size",                "0x900",  "0x900"),
    ("apps_v4_flt_hash_ofst",       "0x2718", "0x2718"),
    ("apps_v4_flt_hash_size",       "0x0",    "0x0"),
    ("apps_v4_flt_nhash_ofst",      "0x2718", "0x2718"),
    ("apps_v4_flt_nhash_size",      "0x0",    "0x0"),
    ("apps_v6_flt_hash_ofst",       "0x2718", "0x2718"),
    ("apps_v6_flt_hash_size",       "0x0",    "0x0"),
    ("apps_v6_flt_nhash_ofst",      "0x2718", "0x2718"),
    ("apps_v6_flt_nhash_size",      "0x0",    "0x0"),
    ("apps_v4_rt_hash_ofst",        "0x2718", "0x2718"),
    ("apps_v4_rt_hash_size",        "0x0",    "0x0"),
    ("apps_v4_rt_nhash_ofst",       "0x2718", "0x2718"),
    ("apps_v4_rt_nhash_size",       "0x0",    "0x0"),
    ("apps_v6_rt_hash_ofst",        "0x2718", "0x2718"),
    ("apps_v6_rt_hash_size",        "0x0",    "0x0"),
    ("apps_v6_rt_nhash_ofst",       "0x2718", "0x2718"),
    ("apps_v6_rt_nhash_size",       "0x0",    "0x0"),
    ("pdn_config_ofst",             "0x44C8", "0x4ee8"),
    ("pdn_config_size",             "0x50",   "0x100"),
    ("end_ofst",                    "0x4518", "0x4fe8"),
]


def _part(col):
    """The ipa_5_5_mem_part initializer with column col of PART (1 = 6.12 text, 2 = kalama)."""
    return ("static struct ipa3_mem_partition ipa_5_5_mem_part = {\n"
            + "".join("\t.%s = %s,\n" % (r[0], r[col]) for r in PART)
            + "};\n")


IPA_V3 = "dataipa/drivers/platform/msm/ipa/ipa_v3/"

EDITS = [
    # --- 1. kalama's IPA 5.5 local-memory map (see the header). Everything that addresses IPA
    # SRAM reads this table: the uC mailbox ioremap (ipa3_uc_interface_init: SRAM + uc_ofst), the
    # canaries (_ipa_init_sram_v3), header/proc-ctx/routing/filter/NAT/stats setup and commits,
    # the Q6 area zeroing at every modem power-up (ipa3_init_q6_smem from ipa3_wwan_probe), and
    # every offset QMI_IPA_INIT_MODEM_DRIVER_REQ hands the modem. All alignment checks in
    # ipa3_init_mem_partition() pass with these values (they did on 5.15).
    (IPA_V3 + "ipa_utils.c",
     _part(1),
     '/* Device compatibility. */\n'
     + _part(2)),

    # --- 2. the modem's FnR (filter/route) stats window, QMI hw_filter_stats_info. Stock sends
    # start index 128, end 179, 832 bytes at stats_fnr_ofst (the phone's ipam.ko: mov w12, #0x340
    # and strh #0xb380 = bytes 0x80, 0xb3, in ipa3_q6_clnt_svc_arrive). 6.12's 64 + 191 counters
    # (3056 bytes) fit its own 0x1000-byte area but would run past kalama's 0xba0-byte one, into
    # stats_drop and the modem area. Only ipa_qmi_service.c uses the three macros. The AP's own FnR
    # counters stay at 1..IPA_MAX_FLT_RT_CNT_INDEX (32), well below 128.
    (IPA_V3 + "ipa_i.h",
     "#define IPA_Q6_FNR_START_IDX (64)\n"
     "#define IPA_Q6_FNR_IDX_CNT (191)\n",
     "/* k618: kalama's modem FnR window, as the 5.15 driver and the phone's ipam.ko give it */\n"
     "#define IPA_Q6_FNR_START_IDX (128)\n"
     "#define IPA_Q6_FNR_IDX_CNT (52)\n"),

    # --- 3. _ipa_init_sram_v3 writes 0 and then 0xABCDEF01 into the first word of the uC
    # descriptor RAM for IPA >= 5.5 -- a marker of this release's IPA 5.5, absent from the 5.15
    # driver and from the phone's ipam.ko. With kalama's map that word is the start of the uC's own
    # 4 KB descriptor RAM; leave it to the uC, as stock does.
    (IPA_V3 + "ipa.c",
     "\t\tIPA_MEM_PART(modem_hdr_proc_ctx_ofst));\n"
     "\tif (IPA_MEM_PART(uc_descriptor_ram_size) > 0)\n"
     "\t\tipa_sram_mmio[IPA_MEM_PART(uc_descriptor_ram_ofst) / 4] = 0x00000000;\n",
     "\t\tIPA_MEM_PART(modem_hdr_proc_ctx_ofst));\n"
     "\t/*\n"
     "\t * k618: no write to the first word of the uC descriptor RAM (here or at the end): kalama's\n"
     "\t * 5.15 driver never makes one, and on this phone that word belongs to the uC.\n"
     "\t */\n"),
    (IPA_V3 + "ipa.c",
     "\tif (ipa_get_hw_type() >= IPA_HW_v5_5) {\n"
     "\t\tif (IPA_MEM_PART(uc_descriptor_ram_size) > 0)\n"
     "\t\t\tipa_sram_mmio[IPA_MEM_PART(uc_descriptor_ram_ofst) / 4] = 0xABCDEF01;\n"
     "\t}\n"
     "\n"
     "\tiounmap(ipa_sram_mmio);\n",
     "\t/* k618: no uC descriptor RAM marker (see above) */\n"
     "\tiounmap(ipa_sram_mmio);\n"),
]


OPTIONAL_EDITS = [
    ("dataipa/drivers/platform/msm/gsi/gsi.c",
     "\tif (ctx->state == GSI_CHAN_STATE_STOP_IN_PROC) {\n"
     "\t\tGSIERR(\"chan=%lu busy try again\\n\", chan_hdl);\n"
     "\t\tres = -GSI_STATUS_AGAIN;\n"
     "\t\tgoto free_lock;\n"
     "\t}\n"
     "\n"
     "\t/* If channel is stopped succesfully",
     "\tif (ctx->state == GSI_CHAN_STATE_STOP_IN_PROC) {\n"
     "\t\tGSIDBG(\"chan=%lu busy try again\\n\", chan_hdl);\n"
     "\t\tres = -GSI_STATUS_AGAIN;\n"
     "\t\tgoto free_lock;\n"
     "\t}\n"
     "\n"
     "\t/* If channel is stopped succesfully"),


    ("dataipa/drivers/platform/msm/ipa/ipa_v3/ipa_dp.c",
     "\t\tdelete_avail_tx_wrapper_list(ep);\n"
     "\n"
     "\t\tif (ep->sys->napi_tx_enable)\n"
     "\t\t\tnetif_napi_del(&ep->sys->napi_tx);\n",
     "\t\tdelete_avail_tx_wrapper_list(ep);\n"
     "\n"
     "\t\tif (ep->sys->napi_tx_enable) {\n"
     "\t\t\t/* k618: 6.x warns on deleting a NAPI that is still enabled */\n"
     "\t\t\tnapi_disable(&ep->sys->napi_tx);\n"
     "\t\t\tnetif_napi_del(&ep->sys->napi_tx);\n"
     "\t\t}\n"),
]


def main(argv):
    import os
    import sys
    if len(argv) < 2:
        print("usage: k618_ipa_hwp_edits.py <techpack root> [--optional]", file=sys.stderr)
        return 2
    root = argv[1].rstrip("/")
    edits = EDITS + (OPTIONAL_EDITS if "--optional" in argv[2:] else [])
    done = 0
    for rel, old, new in edits:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if old in new:
            if new in text:
                continue
        elif new in text and old not in text:
            continue
        if old not in text:
            print(f"k618_ipa_hwp_edits: {rel}: text to change not found", file=sys.stderr)
            return 1
        if text.count(old) != 1:
            print(f"k618_ipa_hwp_edits: {rel}: text to change found {text.count(old)} times",
                  file=sys.stderr)
            return 1
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(
            text.replace(old, new))
        print(f"k618_ipa_hwp_edits: {rel}")
        done += 1
    print(f"k618_ipa_hwp_edits: {done} of {len(edits)} edits applied")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
