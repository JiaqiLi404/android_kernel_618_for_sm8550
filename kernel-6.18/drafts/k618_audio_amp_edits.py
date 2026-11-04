#!/usr/bin/env python3
# Proposed additions to kernel-6.18/k618_tp_fix.py EDITS (audio-kernel, the kalama machine driver
# asoc/kalama.c): the two CS35L41 speaker amps on the secondary TDM back end. Same (file, old,
# new) form as k618_tp_fix.py; the anchors match the techpack as k618_tp.sh builds it (after the
# existing kalama.c edits and the k618_fix618.py sweeps, which touch neither spot).
#
# Run alone for a check against a copy: k618_audio_amp_edits.py <techpack root> applies AMP_EDITS
# there (idempotent, the same rule as k618_tp_fix.py).
#
# Why, from the stock 5.15 machine_dlkm.ko (disassembled) and this phone's devicetree:
#  - the sound node says qcom,wsa-max-devs = <0>, qcom,tdm-audio-intf = <1>, qcom,tdm-max-slots
#    = <4>, qcom,sec-mi2s-gpios = <&sec_tdm_pinctrl> (TLMM 121 SCK, 123 WS, 122 data in, 124 data
#    out, function i2s1_*); its asoc-codec-names list no amp -- the stock links them by name;
#  - stock msm_tdm_dai_links[2] "TDM-LPAIF-RX-SECONDARY": num_codecs 2, codecs
#    {"cs35l41.2-0040", "cs35l41.2-0040"} and {"cs35l41.2-0042", "cs35l41.2-0042"} (the legacy
#    I2C component/DAI names), dai_fmt 0, init NULL, ops msm_common_be_ops, playback_only,
#    ignore_suspend, ignore_pmdown_time -- the same link this release has, with the stub codec;
#  - stock "TDM-LPAIF-TX-SECONDARY": msm-stub-codec.1 / msm-stub-tx, kept as is here. The amps'
#    feedback/echo-reference slots run with the playback (their "ASPTX Ref" switch), and the ROM
#    captures them as PAL_DEVICE_IN_EXT_EC_REF on that TX back end, 2 ch, 24 bit, 48 kHz;
#  - stock msm_common_snd_hw_params: TDM bit clock = rate x 4 slots x 32 (mul + lsl #5), as this
#    release's TDM_SLOT_WIDTH_BITS 32 -- 6.144 MHz for the ROM's speaker back end (48 kHz, 2 ch,
#    S24_LE, resourcemanager_kalama_mtp.xml) and 12.288 MHz for its 96 kHz ultrasound stream;
#  - the stock codec driver did the codec side itself: cs35l41_pcm_startup() called its
#    set_dai_fmt(0x4004 = DSP_A | CBC_CFC | NB_NF) and cs35l41_pcm_hw_params() set its PLL
#    reference to rate x container width x 4 (= the same 6.144 MHz). Upstream's codec leaves the
#    format and the clock to the machine driver, hence k618_spk_amp_hw_params.

AMP_EDITS = [
    # --- the speakers: two CS35L41 on TDM-LPAIF-RX-SECONDARY (see the header of this file)
    ("audio-kernel/asoc/kalama.c",
     "static struct snd_soc_ops msm_common_be_ops = {\n"
     "\t.hw_params = msm_common_snd_hw_params,\n"
     "\t.startup = msm_common_snd_startup,\n"
     "\t.shutdown = msm_common_snd_shutdown,\n"
     "};\n",
     "static struct snd_soc_ops msm_common_be_ops = {\n"
     "\t.hw_params = msm_common_snd_hw_params,\n"
     "\t.startup = msm_common_snd_startup,\n"
     "\t.shutdown = msm_common_snd_shutdown,\n"
     "};\n"
     "\n"
     "/*\n"
     " * k618: the speakers. No WSA here (qcom,wsa-max-devs = <0>): two Cirrus Logic CS35L41 on I2C\n"
     " * bus 2, cs35l41@40 \"B\" and cs35l41@42 \"T\" (cs35l41_dlkm.ko, upstream driver). Xiaomi's 5.15\n"
     " * machine driver puts both on this back end (its sec_tdm_rx_0 codecs: cs35l41.2-0040 and\n"
     " * cs35l41.2-0042) and leaves TDM-LPAIF-TX-SECONDARY on the stub; it names the I2C devices,\n"
     " * this takes the devicetree nodes. k618_spk_amps=0 keeps the stub codec, as before.\n"
     " */\n"
     "static bool k618_spk_amps = true;\n"
     "module_param(k618_spk_amps, bool, 0444);\n"
     "MODULE_PARM_DESC(k618_spk_amps, \"k618: CS35L41 speaker amps on TDM-LPAIF-RX-SECONDARY\");\n"
     "\n"
     "#define K618_TDM_SLOT_WIDTH\t32\t/* TDM_SLOT_WIDTH_BITS in msm_common.c */\n"
     "#define K618_CS35L41_CLKID_SCLK\t0\t/* CS35L41_CLKID_SCLK, include/sound/cs35l41.h */\n"
     "\n"
     "/*\n"
     " * The codec side the stock codec driver did by itself: DSP_A, codec as clock consumer, normal\n"
     " * polarity (its set_dai_fmt(0x4004) at every startup), PLL reference = the TDM bit clock\n"
     " * msm_common_snd_hw_params() programs, rate x 32 x qcom,tdm-max-slots (6.144 MHz at 48 kHz),\n"
     " * and 32-bit slots for the S24_LE samples. Which slot each amp takes stays with its\n"
     " * \"ASPRX1/ASPTX1 Slot Position\" controls (the HAL: T 0, B 1) and the devicetree default.\n"
     " */\n"
     "static int k618_spk_amp_hw_params(struct snd_pcm_substream *substream,\n"
     "\t\t\t\t  struct snd_pcm_hw_params *params)\n"
     "{\n"
     "\tstruct snd_soc_pcm_runtime *rtd = substream->private_data;\n"
     "\tstruct msm_common_pdata *common = msm_common_get_pdata(rtd->card);\n"
     "\tstruct snd_soc_dai *codec_dai;\n"
     "\tunsigned int slots, sclk;\n"
     "\tint i, ret;\n"
     "\n"
     "\tret = msm_common_snd_hw_params(substream, params);\n"
     "\tif (ret)\n"
     "\t\treturn ret;\n"
     "\n"
     "\tslots = (common && common->tdm_max_slots) ? common->tdm_max_slots : 4;\n"
     "\tsclk = params_rate(params) * K618_TDM_SLOT_WIDTH * slots;\n"
     "\n"
     "\tfor_each_rtd_codec_dais(rtd, i, codec_dai) {\n"
     "\t\tret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_A |\n"
     "\t\t\t\t\t  SND_SOC_DAIFMT_CBC_CFC | SND_SOC_DAIFMT_NB_NF);\n"
     "\t\tif (ret)\n"
     "\t\t\tgoto err;\n"
     "\t\t/* the slot width only (both masks = the stereo pair, as the back end has) */\n"
     "\t\tret = snd_soc_dai_set_tdm_slot(codec_dai, 0x3, 0x3, slots, K618_TDM_SLOT_WIDTH);\n"
     "\t\tif (ret && ret != -ENOTSUPP)\n"
     "\t\t\tgoto err;\n"
     "\t\tret = snd_soc_component_set_sysclk(codec_dai->component, K618_CS35L41_CLKID_SCLK,\n"
     "\t\t\t\t\t\t   0, sclk, SND_SOC_CLOCK_IN);\n"
     "\t\tif (ret)\n"
     "\t\t\tgoto err;\n"
     "\t\t/* frame-sync monitor values: the codec has them up to 6.144 MHz and for 12.288 */\n"
     "\t\tif (snd_soc_dai_set_sysclk(codec_dai, K618_CS35L41_CLKID_SCLK, sclk,\n"
     "\t\t\t\t\t   SND_SOC_CLOCK_IN))\n"
     "\t\t\tdev_dbg(rtd->card->dev, \"k618: %s: no FS monitor values for %u Hz\\n\",\n"
     "\t\t\t\tcodec_dai->name, sclk);\n"
     "\t}\n"
     "\n"
     "\treturn 0;\n"
     "err:\n"
     "\tdev_err(rtd->card->dev, \"k618: %s: amp setup failed (SCLK %u Hz): %d\\n\",\n"
     "\t\tcodec_dai->name, sclk, ret);\n"
     "\treturn ret;\n"
     "}\n"
     "\n"
     "static const struct snd_soc_ops k618_spk_amp_be_ops = {\n"
     "\t.hw_params = k618_spk_amp_hw_params,\n"
     "\t.startup = msm_common_snd_startup,\n"
     "\t.shutdown = msm_common_snd_shutdown,\n"
     "};\n"
     "\n"
     "/*\n"
     " * Every enabled \"cirrus,cs35l41\" node becomes a codec of the secondary TDM playback link, in\n"
     " * devicetree order (B @40, T @42 -- the stock order), DAI \"cs35l41-pcm\" (upstream's name;\n"
     " * the stock matched the legacy one, which equals the I2C device name). Like the stock, the\n"
     " * card then waits for both amps by deferring its probe until cs35l41_dlkm has bound them.\n"
     " */\n"
     "static void k618_link_spk_amps(struct device *dev, struct snd_soc_card *card)\n"
     "{\n"
     "\tstruct snd_soc_dai_link *link = NULL;\n"
     "\tstruct snd_soc_dai_link_component *codecs;\n"
     "\tstruct device_node *np;\n"
     "\tint i, n = 0;\n"
     "\n"
     "\tif (!k618_spk_amps)\n"
     "\t\treturn;\n"
     "\tfor (i = 0; i < card->num_links; i++)\n"
     "\t\tif (card->dai_link[i].name &&\n"
     "\t\t    !strcmp(card->dai_link[i].name, LPASS_BE_SEC_TDM_RX_0))\n"
     "\t\t\tlink = &card->dai_link[i];\n"
     "\tif (!link)\n"
     "\t\treturn;\n"
     "\tfor_each_compatible_node(np, NULL, \"cirrus,cs35l41\")\n"
     "\t\tif (of_device_is_available(np))\n"
     "\t\t\tn++;\n"
     "\tif (!n)\n"
     "\t\treturn;\n"
     "\tcodecs = devm_kcalloc(dev, n, sizeof(*codecs), GFP_KERNEL);\n"
     "\tif (!codecs)\n"
     "\t\treturn;\n"
     "\ti = 0;\n"
     "\tfor_each_compatible_node(np, NULL, \"cirrus,cs35l41\") {\n"
     "\t\tif (!of_device_is_available(np) || i == n)\n"
     "\t\t\tcontinue;\n"
     "\t\tcodecs[i].of_node = of_node_get(np);\n"
     "\t\tcodecs[i].dai_name = \"cs35l41-pcm\";\n"
     "\t\ti++;\n"
     "\t}\n"
     "\tlink->codecs = codecs;\n"
     "\tlink->num_codecs = i;\n"
     "\tlink->ops = &k618_spk_amp_be_ops;\n"
     "\tdev_info(dev, \"k618: %d CS35L41 speaker amp(s) on %s\\n\", i, link->name);\n"
     "}\n"),
    ("audio-kernel/asoc/kalama.c",
     "\tcard = populate_snd_card_dailinks(&pdev->dev, pdata->wsa_max_devs);\n"
     "\tif (!card) {\n"
     "\t\tdev_err(&pdev->dev, \"%s: Card uninitialized\\n\", __func__);\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto err;\n"
     "\t}\n",
     "\tcard = populate_snd_card_dailinks(&pdev->dev, pdata->wsa_max_devs);\n"
     "\tif (!card) {\n"
     "\t\tdev_err(&pdev->dev, \"%s: Card uninitialized\\n\", __func__);\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto err;\n"
     "\t}\n"
     "\tk618_link_spk_amps(&pdev->dev, card);\n"),
]


def main(argv):
    import os
    import sys
    root = argv[1].rstrip("/")
    done = 0
    for rel, old, new in AMP_EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if old in new:
            if new in text:
                continue
        elif new in text and old not in text:
            continue
        if old not in text:
            print(f"k618_audio_amp_edits: {rel}: text to change not found", file=sys.stderr)
            return 1
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text.replace(old, new))
        done += 1
    print(f"k618_audio_amp_edits: {done} of {len(AMP_EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
