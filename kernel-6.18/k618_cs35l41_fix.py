

import os
import sys

COMPAT = r'''// SPDX-License-Identifier: GPL-2.0
/*
 * k618_cs35l41_compat.h -- included once, into cs35l41.c, by k618_cs35l41_fix.py.
 *
 * The Xiaomi 13 Pro (nuwa) ROM's view of its two speaker amps, kept beside the upstream driver:
 * B = cs35l41@40 (bottom, cirrus,right-channel-amp), T = cs35l41@42 (top, also the earpiece),
 * both on I2C bus 2 and on the secondary TDM interface. The phone's own 5.15 driver was read
 * from its disassembly; each part says which stock function it stands in for.
 */
#include <linux/log2.h>
#include <linux/property.h>

/*
 * Hibernation: upstream puts a preloaded, running DSP into hibernation on every runtime
 * suspend (3 s after the last use) and wakes it over I2C on resume. The stock driver does that
 * only with "cirrus,hibernate-enable", which this phone's devicetree does not have, so on the
 * phone the DSP stays loaded and paused between streams. Off by default, as there.
 */
static bool k618_hibernate;
module_param(k618_hibernate, bool, 0444);
MODULE_PARM_DESC(k618_hibernate,
		 "k618: hibernate the amp DSP on runtime suspend (the phone's own driver does not)");

static bool k618_fast_switch = true;
module_param(k618_fast_switch, bool, 0444);
MODULE_PARM_DESC(k618_fast_switch, "k618: apply the cirrus,fast-switch use-case delta files");

/*
 * The firmware the ROM loads (/odm/firmware/cs35l41-dsp1-spk-prot.wmfw, 5.15 log:
 * "Firmware: 400a4 vendor: 0x2 v0.43.1, 2 algorithms", "0: ID cd v29.63.1"): 0x400a4 is the
 * firmware's own algorithm, 0xcd the speaker protection (CSPL). Its controls are XM/YM words.
 */
#define K618_ALG_FW			0x400a4
#define K618_ALG_CSPL			0xcd
#define K618_FW_SPK_PROT		9	/* WM_ADSP_FW_SPK_PROT in wm_adsp.c */
#define K618_FW_SPK_CALI		10	/* WM_ADSP_FW_SPK_CALI */
#define K618_CSPL_CMD_UPDATE_PARAMS	8	/* what the stock writes to CSPL_COMMAND */
#define K618_FS_MAX_WORDS		256

static int k618_ctl_write_locked(struct cs35l41_private *cs35l41, const char *name,
				 int type, unsigned int alg, u32 val)
{
	__be32 be = cpu_to_be32(val);
	int ret;

	ret = cs_dsp_coeff_write_ctrl(cs_dsp_get_ctl(&cs35l41->dsp.cs_dsp, name, type, alg),
				      0, &be, sizeof(be));
	if (ret < 0)
		dev_warn(cs35l41->dev, "k618: %s (alg %#x) not written: %d\n", name, alg, ret);

	return ret < 0 ? ret : 0;
}

/*
 * wm_adsp's pre_run hook: cs_dsp_run() calls it with pwr_lock held, after the firmware and the
 * tuning are loaded and before the written controls are synced, so these values reach the DSP
 * before it starts. It stands in for the stock wm_halo_apply_calibration(), which the stock
 * wm_adsp_event() called at the same point:
 *  - protection firmware: MAX_LRCLK_DELAY = 32 (algorithm 400a4), then, once the HAL has handed
 *    over this unit's calibration, CAL_R = calr, CAL_STATUS = 1, CAL_CHECKSUM = calr + 1
 *    (algorithm cd). The HAL's libcrussp reads it from /mnt/vendor/persist/audio/crus_calr.bin
 *    and writes "<prefix> Calibration Resistance" (5.15 log: calr = 9367 T, 9156 B);
 *  - calibration firmware: CAL_AMBIENT = "<prefix> Ambient Temperature".
 * Upstream's DSP keeps running between streams (paused, not stopped), so this runs once per
 * firmware load; a value that arrives later is written straight away (k618_apply_cal_live).
 */
static int k618_cs35l41_pre_run(struct wm_adsp *dsp)
{
	struct cs35l41_private *cs35l41 = container_of(dsp, struct cs35l41_private, dsp);
	struct k618_cs35l41 *k = &cs35l41->k618;

	switch (dsp->fw) {
	case K618_FW_SPK_PROT:
		k618_ctl_write_locked(cs35l41, "MAX_LRCLK_DELAY", WMFW_ADSP2_XM, K618_ALG_FW, 32);
		if (!k->cal_r) {
			dev_warn(cs35l41->dev,
				 "k618: no calibration yet: protection starts on the tuning's CAL_R\n");
			break;
		}
		k618_ctl_write_locked(cs35l41, "CAL_R", WMFW_ADSP2_XM, K618_ALG_CSPL, k->cal_r);
		k618_ctl_write_locked(cs35l41, "CAL_STATUS", WMFW_ADSP2_XM, K618_ALG_CSPL, 1);
		k618_ctl_write_locked(cs35l41, "CAL_CHECKSUM", WMFW_ADSP2_XM, K618_ALG_CSPL,
				      k->cal_r + 1);
		dev_info(cs35l41->dev, "k618: calibration CAL_R %u applied\n", k->cal_r);
		break;
	case K618_FW_SPK_CALI:
		k618_ctl_write_locked(cs35l41, "CAL_AMBIENT", WMFW_ADSP2_XM, K618_ALG_CSPL,
				      k->cal_ambient);
		break;
	default:
		break;
	}

	return 0;
}

static void k618_apply_cal_live(struct cs35l41_private *cs35l41)
{
	mutex_lock(&cs35l41->dsp.cs_dsp.pwr_lock);
	if (cs35l41->dsp.cs_dsp.running)
		k618_cs35l41_pre_run(&cs35l41->dsp);
	mutex_unlock(&cs35l41->dsp.cs_dsp.pwr_lock);
}

/*
 * Fast use-case switch: Xiaomi's tuning comes with per-use-case deltas (cirrus,fast-switch:
 * B-music.txt B-handfree.txt / T-music.txt T-handset.txt T-handfree.txt in /odm/firmware).
 * A file is one line of decimal integers whose first is the length of the whole list. The
 * stock cs35l41_do_fast_switch() writes the list big-endian to the CSPL control
 * CSPL_UPDATE_PARAMS_CONFIG (YM), then CSPL_COMMAND = 8 (XM), and polls CSPL_STATE (XM) for 0
 * five times 100-200 us apart. The HAL picks "<prefix>-handfree.txt" for a speakerphone call
 * (path speaker-mono-2) and goes back to "-music.txt" after it. At boot it selects music
 * before it enables the switch, so media play on the base tuning, as on the stock. Files are
 * read on first use and kept.
 */
static int k618_fs_load(struct cs35l41_private *cs35l41, unsigned int idx)
{
	struct k618_cs35l41 *k = &cs35l41->k618;
	const struct firmware *fw;
	char *text, *p, *tok;
	__be32 *vals = NULL;
	int count = -1, n = 0, v, ret = 0;

	if (k->fs_vals[idx])
		return 0;

	ret = request_firmware(&fw, k->fs_names[idx], cs35l41->dev);
	if (ret) {
		dev_err(cs35l41->dev, "k618: fast switch file %s: %d\n", k->fs_names[idx], ret);
		return ret;
	}
	text = kmemdup_nul(fw->data, fw->size, GFP_KERNEL);
	release_firmware(fw);
	if (!text)
		return -ENOMEM;

	p = text;
	while ((tok = strsep(&p, ",")) != NULL) {
		tok = strim(tok);
		if (!*tok)
			continue;
		ret = kstrtoint(tok, 10, &v);
		if (ret)
			break;
		if (count < 0) {
			if (v < 2 || v > K618_FS_MAX_WORDS) {
				ret = -EINVAL;
				break;
			}
			count = v;
			vals = kcalloc(count, sizeof(*vals), GFP_KERNEL);
			if (!vals) {
				ret = -ENOMEM;
				break;
			}
		}
		if (n == count) {
			ret = -EINVAL;
			break;
		}
		vals[n++] = cpu_to_be32((u32)v);
	}
	kfree(text);
	if (!ret && (count < 0 || n != count))
		ret = -EINVAL;
	if (!ret) {
		k->fs_vals[idx] = devm_kmemdup(cs35l41->dev, vals, count * sizeof(*vals),
					       GFP_KERNEL);
		k->fs_count[idx] = count;
		if (!k->fs_vals[idx])
			ret = -ENOMEM;
	}
	kfree(vals);
	if (ret)
		dev_err(cs35l41->dev, "k618: fast switch file %s unusable: %d\n",
			k->fs_names[idx], ret);

	return ret;
}

static int k618_do_fast_switch(struct cs35l41_private *cs35l41)
{
	struct k618_cs35l41 *k = &cs35l41->k618;
	unsigned int idx = k->fs_index;
	__be32 cmd = cpu_to_be32(K618_CSPL_CMD_UPDATE_PARAMS), state = 0;
	int i, ret;

	ret = k618_fs_load(cs35l41, idx);
	if (ret)
		return ret;

	ret = wm_adsp_write_ctl(&cs35l41->dsp, "CSPL_UPDATE_PARAMS_CONFIG", WMFW_ADSP2_YM,
				K618_ALG_CSPL, k->fs_vals[idx],
				k->fs_count[idx] * sizeof(__be32));
	if (!ret)
		ret = wm_adsp_write_ctl(&cs35l41->dsp, "CSPL_COMMAND", WMFW_ADSP2_XM,
					K618_ALG_CSPL, &cmd, sizeof(cmd));
	for (i = 0; !ret && i < 5; i++) {
		ret = wm_adsp_read_ctl(&cs35l41->dsp, "CSPL_STATE", WMFW_ADSP2_XM,
				       K618_ALG_CSPL, &state, sizeof(state));
		if (ret || !state)
			break;
		usleep_range(100, 200);
	}
	if (!ret && state)
		ret = -ETIMEDOUT;
	if (ret)
		dev_err(cs35l41->dev, "k618: fast switch to %s failed: %d (CSPL_STATE %u)\n",
			k->fs_names[idx], ret, be32_to_cpu(state));
	else
		dev_info(cs35l41->dev, "k618: fast switch to %s\n", k->fs_names[idx]);

	return ret;
}

/* from the Main AMP power-up: where the stock applied a delta chosen while the DSP was off */
static void k618_fast_switch_pending(struct cs35l41_private *cs35l41)
{
	struct k618_cs35l41 *k = &cs35l41->k618;

	if (!k->fs_pending || !cs35l41->dsp.cs_dsp.running)
		return;
	k->fs_pending = false;
	k618_do_fast_switch(cs35l41);
}

static struct cs35l41_private *k618_priv(struct snd_kcontrol *kcontrol)
{
	return snd_soc_component_get_drvdata(snd_kcontrol_chip(kcontrol));
}

/* stock cs35l41_calr_get/put and cs35l41_ambient_get/put: the value is kept for pre_run */
static int k618_calr_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = k618_priv(kcontrol)->k618.cal_r;
	return 0;
}

static int k618_calr_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l41_private *cs35l41 = k618_priv(kcontrol);
	long v = ucontrol->value.integer.value[0];

	if (v < 0 || v > 0xFFFFFF)
		return -EINVAL;
	if (v == cs35l41->k618.cal_r)
		return 0;
	cs35l41->k618.cal_r = v;
	dev_info(cs35l41->dev, "k618: calr = %ld\n", v);
	k618_apply_cal_live(cs35l41);

	return 1;
}

static int k618_ambient_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = k618_priv(kcontrol)->k618.cal_ambient;
	return 0;
}

static int k618_ambient_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l41_private *cs35l41 = k618_priv(kcontrol);
	long v = ucontrol->value.integer.value[0];

	if (v < 0 || v > 0xFFFFFF)
		return -EINVAL;
	if (v == cs35l41->k618.cal_ambient)
		return 0;
	cs35l41->k618.cal_ambient = v;
	k618_apply_cal_live(cs35l41);

	return 1;
}

/* stock cs35l41_amp_active_status_get: GLOBAL_EN (PWR_CTRL1 bit 0); libcrussp checks it */
static int k618_amp_active_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	unsigned int val = 0;

	regmap_read(k618_priv(kcontrol)->regmap, CS35L41_PWR_CTRL1, &val);
	ucontrol->value.integer.value[0] = !!(val & CS35L41_GLOBAL_EN_MASK);

	return 0;
}

static int k618_dsp_booted_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = k618_priv(kcontrol)->dsp.cs_dsp.booted;
	return 0;
}

/* status controls: accepted and ignored, so a HAL that writes them gets no error */
static int k618_ro_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int k618_fs_en_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = k618_priv(kcontrol)->k618.fs_en;
	return 0;
}

static int k618_fs_en_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l41_private *cs35l41 = k618_priv(kcontrol);
	bool en = !!ucontrol->value.integer.value[0];

	if (en == cs35l41->k618.fs_en)
		return 0;
	cs35l41->k618.fs_en = en;

	return 1;
}

static int k618_fs_file_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = k618_priv(kcontrol)->k618.fs_index;
	return 0;
}

/* stock cs35l41_fast_switch_file_put: nothing happens while the switch is off */
static int k618_fs_file_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l41_private *cs35l41 = k618_priv(kcontrol);
	struct k618_cs35l41 *k = &cs35l41->k618;
	unsigned int idx = ucontrol->value.enumerated.item[0];

	if (idx >= k->fs_num)
		return -EINVAL;
	if (idx == k->fs_index || !k->fs_en)
		return 0;
	k->fs_index = idx;
	if (!k618_fast_switch)
		return 1;
	if (cs35l41->dsp.cs_dsp.running) {
		k->fs_pending = false;
		k618_do_fast_switch(cs35l41);
	} else {
		k->fs_pending = true;
	}

	return 1;
}

/*
 * The controls the HAL sets that upstream does not have, named and placed as the stock driver
 * had them (its cs35l41_aud_controls, disassembled), so mixer_paths_overlay_static.xml works
 * unchanged:
 *  - AMP PCM Gain: CS35L41_AMP_GAIN_CTRL[9:5], max 20 -- upstream's "Analog PCM Volume" field;
 *    T 18 / B 19 for the speaker, T 5 for the earpiece;
 *  - ASPTXn / ASPRXn Slot Position: SP_FRAME_TX_SLOT / SP_FRAME_RX_SLOT, one byte each, 0..7.
 *    The HAL gives T RX1 slot 0 + TX1 slot 0, B RX1 slot 1 + TX1 slot 1 (TDM: 4 slots of 32
 *    bits, stereo in slots 0/1);
 *  - Noise Gate: CS35L41_NG_CFG[13:0] raw (default 0x33 = 51; the speaker path sets 16245);
 *  - Boost Class-H Tracking Enable: BSTCVRT_VCTRL2 bit 0; Boost Target Voltage: BSTCVRT_VCTRL1
 *    [7:0], max 0xAA (the HAL: T 55, B 75);
 *  - Calibration Resistance / Ambient Temperature / AMP Active Status / DSP Booted: libcrussp;
 *  - Fast Use Case Switch Enable (and "Fast Use Case Delta File", added per amp at probe:
 *    its choices are this amp's cirrus,fast-switch names).
 * Upstream already has, under the same names: Digital PCM Volume (817 = 0 dB), PCM Soft Ramp,
 * DRE Switch, PCM Source, ASP TXn Source, DSP RXn Source, DSP1 Preload Switch, DSP1 Firmware.
 * Not carried: the stock's debug and factory controls (AMP Reset, CCM Reset, Force Interrupt,
 * Hibernate Force Wake, Firmware Reload Tuning, GLOBAL_EN/AMP_EN Control, Boost Converter
 * Enable, the volume ramp set, Audio Output Device, Audio_PA_Debug_Setting, AMP WR Registers,
 * Passport, Spksw Gpio Swtich / Enable RCV Pin Control -- no spksw GPIO in this devicetree).
 */
#define K618_CS35L41_CONTROLS \
	SOC_SINGLE("AMP PCM Gain", CS35L41_AMP_GAIN_CTRL, 5, 0x14, 0), \
	SOC_SINGLE_RANGE("ASPTX1 Slot Position", CS35L41_SP_FRAME_TX_SLOT, 0, 0, 7, 0), \
	SOC_SINGLE_RANGE("ASPTX2 Slot Position", CS35L41_SP_FRAME_TX_SLOT, 8, 0, 7, 0), \
	SOC_SINGLE_RANGE("ASPTX3 Slot Position", CS35L41_SP_FRAME_TX_SLOT, 16, 0, 7, 0), \
	SOC_SINGLE_RANGE("ASPTX4 Slot Position", CS35L41_SP_FRAME_TX_SLOT, 24, 0, 7, 0), \
	SOC_SINGLE_RANGE("ASPRX1 Slot Position", CS35L41_SP_FRAME_RX_SLOT, 0, 0, 7, 0), \
	SOC_SINGLE_RANGE("ASPRX2 Slot Position", CS35L41_SP_FRAME_RX_SLOT, 8, 0, 7, 0), \
	SOC_SINGLE("Noise Gate", CS35L41_NG_CFG, 0, 0x3FFF, 0), \
	SOC_SINGLE("Boost Class-H Tracking Enable", CS35L41_BSTCVRT_VCTRL2, 0, 1, 0), \
	SOC_SINGLE("Boost Target Voltage", CS35L41_BSTCVRT_VCTRL1, 0, 0xAA, 0), \
	SOC_SINGLE_EXT("Calibration Resistance", SND_SOC_NOPM, 0, 0xFFFFFF, 0, \
		       k618_calr_get, k618_calr_put), \
	SOC_SINGLE_EXT("Ambient Temperature", SND_SOC_NOPM, 0, 0xFFFFFF, 0, \
		       k618_ambient_get, k618_ambient_put), \
	SOC_SINGLE_EXT("AMP Active Status", SND_SOC_NOPM, 0, 1, 0, \
		       k618_amp_active_get, k618_ro_put), \
	SOC_SINGLE_EXT("DSP Booted", SND_SOC_NOPM, 0, 1, 0, k618_dsp_booted_get, k618_ro_put), \
	SOC_SINGLE_EXT("Fast Use Case Switch Enable", SND_SOC_NOPM, 0, 1, 0, \
		       k618_fs_en_get, k618_fs_en_put)

/*
 * Two DAPM controls of the stock driver that the HAL's paths set:
 *  - "AMP Enable Switch" gates the playback stream into the amp (stock routes: AMP Playback ->
 *    AMP Enable -> ASPRX1/2). Both amps sit on one TDM link, so this is what keeps the bottom
 *    speaker quiet in the handset path, which enables the top amp only;
 *  - "ASPTX Ref" = Ref powers the transmit slots (ASPTX1/2, fed by DSPTX1) together with the
 *    speaker (stock routes: ASPTX1/2 -> ASPTX Ref -> Main AMP), so the processed speaker
 *    signal goes back to the SoC on TDM-LPAIF-TX-SECONDARY as the external echo reference
 *    (PAL_DEVICE_IN_EXT_EC_REF / ext_ec_ref_tx in the ROM) with no ASoC capture stream -- the
 *    stock machine driver leaves that TX link on the stub codec.
 */
static const char * const k618_asptx_ref_text[] = { "None", "Ref" };
static SOC_ENUM_SINGLE_VIRT_DECL(k618_asptx_ref_enum, k618_asptx_ref_text);
static const struct snd_kcontrol_new k618_asptx_ref_mux =
	SOC_DAPM_ENUM("Virt Connect", k618_asptx_ref_enum);
static const struct snd_kcontrol_new k618_amp_enable_ctrl =
	SOC_DAPM_SINGLE_VIRT("Switch", 1);

#define K618_CS35L41_WIDGETS \
	SND_SOC_DAPM_MUX("ASPTX Ref", SND_SOC_NOPM, 0, 0, &k618_asptx_ref_mux), \
	SND_SOC_DAPM_SWITCH("AMP Enable", SND_SOC_NOPM, 0, 0, &k618_amp_enable_ctrl)

/*
 * The slot width, from the machine driver: the Qualcomm TDM frame has 32-bit slots whatever
 * the sample format (msm_common.c TDM_SLOT_WIDTH_BITS; 4 slots, qcom,tdm-max-slots). Upstream
 * hw_params takes the slot width from the sample width, i.e. 24 for the ROM's S24_LE speaker
 * back end, and would read slot 1 from bit 24; the stock took the container width (32).
 * Which slot each amp uses stays with the slot-position controls and the devicetree default.
 */
static int k618_cs35l41_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
				     unsigned int rx_mask, int slots, int slot_width)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(dai->component);

	switch (slot_width) {
	case 0:
	case 16:
	case 24:
	case 32:
		cs35l41->k618.slot_width = slot_width;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * This devicetree uses the older Cirrus binding for the GPIO pins: a child node per pin
 * ("cirrus,gpio-config2 { cirrus,gpio-output-enable; cirrus,gpio-src-select = <4>; }"), where
 * upstream reads flat cirrus,gpio2-* properties. Without it GPIO2 stays high-impedance and the
 * amp's interrupt line (TLMM 99 for B, 98 for T, level-low) never moves: no error interrupts and
 * no protection-release cycle after one. Source 4 = push-pull, active-low interrupt, the same
 * field value in both drivers (CS35L41_GPIO2_INT_PUSH_PULL_LOW).
 */
static void k618_gpio_subnode(struct device *dev, const char *name,
			      struct cs35l41_gpio_cfg *gpio)
{
	struct fwnode_handle *child;
	unsigned int val;

	if (gpio->valid)
		return;
	child = device_get_named_child_node(dev, name);
	if (!child)
		return;
	gpio->pol_inv = fwnode_property_read_bool(child, "cirrus,gpio-polarity-invert");
	gpio->out_en = fwnode_property_read_bool(child, "cirrus,gpio-output-enable");
	if (!fwnode_property_read_u32(child, "cirrus,gpio-src-select", &val)) {
		gpio->func = val;
		gpio->valid = true;
	}
	fwnode_handle_put(child);
}

/*
 * Devicetree properties of the stock binding that upstream ignores, applied after
 * cs35l41_set_pdata():
 *  - cirrus,right-channel-amp (B): the stock cs35l41_restore()/probe set ASP RX1 and RX2 to
 *    slot 1, else slot 0. TX1 goes to the same slot here too, so the two amps never drive one
 *    TX slot before the HAL has set the slot positions (it sets the same values);
 *  - cirrus,temp-warn_threshold = <3>: stock cs35l41_set_pdata() writes it to DTEMP_WARN_THLD;
 *  - cirrus,fast-switch: the delta file names, the choices of "Fast Use Case Delta File".
 */
static void k618_cs35l41_pdata(struct cs35l41_private *cs35l41)
{
	struct device *dev = cs35l41->dev;
	struct k618_cs35l41 *k = &cs35l41->k618;
	unsigned int slot, val;
	int n;

	slot = device_property_read_bool(dev, "cirrus,right-channel-amp") ? 1 : 0;
	regmap_update_bits(cs35l41->regmap, CS35L41_SP_FRAME_RX_SLOT,
			   CS35L41_ASP_RX1_SLOT_MASK | CS35L41_ASP_RX2_SLOT_MASK,
			   slot << CS35L41_ASP_RX1_SLOT_SHIFT | slot << CS35L41_ASP_RX2_SLOT_SHIFT);
	regmap_update_bits(cs35l41->regmap, CS35L41_SP_FRAME_TX_SLOT, 0x3F, slot);

	if (!device_property_read_u32(dev, "cirrus,temp-warn_threshold", &val))
		regmap_update_bits(cs35l41->regmap, CS35L41_DTEMP_WARN_THLD,
				   CS35L41_TEMP_THLD_MASK, val);

	n = device_property_string_array_count(dev, "cirrus,fast-switch");
	if (n <= 0 || n > 8)
		return;
	k->fs_names = devm_kcalloc(dev, n, sizeof(*k->fs_names), GFP_KERNEL);
	k->fs_vals = devm_kcalloc(dev, n, sizeof(*k->fs_vals), GFP_KERNEL);
	k->fs_count = devm_kcalloc(dev, n, sizeof(*k->fs_count), GFP_KERNEL);
	if (!k->fs_names || !k->fs_vals || !k->fs_count ||
	    device_property_read_string_array(dev, "cirrus,fast-switch", k->fs_names, n) != n)
		return;
	k->fs_num = n;
	k->fs_enum.reg = SND_SOC_NOPM;
	k->fs_enum.items = n;
	k->fs_enum.texts = k->fs_names;
	k->fs_enum.mask = roundup_pow_of_two(n) - 1;
	k->fs_ctl.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	k->fs_ctl.name = "Fast Use Case Delta File";
	k->fs_ctl.info = snd_soc_info_enum_double;
	k->fs_ctl.get = k618_fs_file_get;
	k->fs_ctl.put = k618_fs_file_put;
	k->fs_ctl.private_value = (unsigned long)&k->fs_enum;
	dev_info(dev, "k618: right=%u, %d fast-switch files (%s first)\n", slot, n, k->fs_names[0]);
}
'''

GENERATED = [("k618_cs35l41_compat.h", COMPAT)]

EDITS = [
    # --- cs35l41.h
    # S32_LE as well, as the stock DAI offered (formats 0x444 = S16_LE | S24_LE | S32_LE for both
    # streams): the ROM configures S24_LE for the speaker, handset and echo-reference back ends,
    # but a 32-bit stream must not fail at hw_params where the phone's own driver took it
    ("cs35l41.h",
     "#define CS35L41_RX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)\n"
     "#define CS35L41_TX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)\n",
     "/* k618: S32_LE too, as the phone's own driver */\n"
     "#define CS35L41_RX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \\\n"
     "\t\t\t    SNDRV_PCM_FMTBIT_S32_LE)\n"
     "#define CS35L41_TX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \\\n"
     "\t\t\t    SNDRV_PCM_FMTBIT_S32_LE)\n"),
    # the per-amp state the compat part keeps
    ("cs35l41.h",
     "struct cs35l41_private {\n",
     "/* k618: what the phone's own driver kept per amp; see k618_cs35l41_compat.h */\n"
     "struct k618_cs35l41 {\n"
     "\tunsigned int slot_width;\t/* from set_tdm_slot(); 0 = the sample container */\n"
     "\tu32 cal_r;\t\t\t/* \"Calibration Resistance\", applied when protection starts */\n"
     "\tu32 cal_ambient;\t\t/* \"Ambient Temperature\", for the calibration firmware */\n"
     "\tbool fs_en;\t\t\t/* \"Fast Use Case Switch Enable\" */\n"
     "\tbool fs_pending;\t\t/* a delta chosen while the DSP was not running */\n"
     "\tunsigned int fs_index;\t\t/* \"Fast Use Case Delta File\" */\n"
     "\tunsigned int fs_num;\n"
     "\tconst char **fs_names;\t\t/* cirrus,fast-switch */\n"
     "\t__be32 **fs_vals;\t\t/* the files, parsed on first use */\n"
     "\tunsigned int *fs_count;\n"
     "\tstruct soc_enum fs_enum;\n"
     "\tstruct snd_kcontrol_new fs_ctl;\n"
     "};\n\n"
     "struct cs35l41_private {\n"),
    ("cs35l41.h",
     "\t/* GPIO for /RST */\n\tstruct gpio_desc *reset_gpio;\n};\n",
     "\t/* GPIO for /RST */\n\tstruct gpio_desc *reset_gpio;\n\tstruct k618_cs35l41 k618;\n};\n"),

    # --- cs35l41.c
    # "None" first, as the stock list ({"None", "ASP", "DSP"} = 0x00, 0x08, 0x32): the HAL resets
    # PCM Source to None when a path ends, and None leaves the amp's DAC without a source
    ("cs35l41.c",
     "static const char * const cs35l41_pcm_source_texts[] = {\"ASP\", \"DSP\"};\n"
     "static const unsigned int cs35l41_pcm_source_values[] = {0x08, 0x32};\n",
     "/* k618: \"None\" as in the phone's own driver -- the HAL's reset value */\n"
     "static const char * const cs35l41_pcm_source_texts[] = {\"None\", \"ASP\", \"DSP\"};\n"
     "static const unsigned int cs35l41_pcm_source_values[] = {0x00, 0x08, 0x32};\n"),
    ("cs35l41.c",
     "static const struct snd_kcontrol_new cs35l41_aud_controls[] = {\n",
     "#include \"k618_cs35l41_compat.h\"\n\n"
     "static const struct snd_kcontrol_new cs35l41_aud_controls[] = {\n"),
    ("cs35l41.c",
     "\tWM_ADSP_FW_CONTROL(\"DSP1\", 0),\n};\n",
     "\tWM_ADSP_FW_CONTROL(\"DSP1\", 0),\n\tK618_CS35L41_CONTROLS,\n};\n"),
    ("cs35l41.c",
     "\tSND_SOC_DAPM_SWITCH(\"DRE\", SND_SOC_NOPM, 0, 0, &dre_ctrl),\n};\n",
     "\tSND_SOC_DAPM_SWITCH(\"DRE\", SND_SOC_NOPM, 0, 0, &dre_ctrl),\n\tK618_CS35L41_WIDGETS,\n};\n"),
    ("cs35l41.c",
     "\t{\"ASPRX1\", NULL, \"AMP Playback\"},\n\t{\"ASPRX2\", NULL, \"AMP Playback\"},\n",
     "\t/* k618: the stock routes -- see K618_CS35L41_WIDGETS */\n"
     "\t{\"AMP Enable\", \"Switch\", \"AMP Playback\"},\n"
     "\t{\"ASPRX1\", NULL, \"AMP Enable\"},\n"
     "\t{\"ASPRX2\", NULL, \"AMP Enable\"},\n"
     "\t{\"ASPTX Ref\", \"Ref\", \"ASPTX1\"},\n"
     "\t{\"ASPTX Ref\", \"Ref\", \"ASPTX2\"},\n"
     "\t{\"Main AMP\", NULL, \"ASPTX Ref\"},\n"),


    ("cs35l41.c",
     "\tret = regmap_read_poll_timeout(cs35l41->regmap, CS35L41_IRQ1_STATUS4,\n"
     "\t\t\t\t       int_status, int_status & CS35L41_OTP_BOOT_DONE,\n"
     "\t\t\t\t       1000, 100000);\n",
     "\t{\t/* k618: a read that fails (NACK) while the part boots is retried, as stock */\n"
     "\t\tunsigned long k618_end = jiffies + msecs_to_jiffies(200);\n"
     "\n"
     "\t\tdo {\n"
     "\t\t\tret = regmap_read(cs35l41->regmap, CS35L41_IRQ1_STATUS4, &int_status);\n"
     "\t\t\tif (!ret && (int_status & CS35L41_OTP_BOOT_DONE))\n"
     "\t\t\t\tbreak;\n"
     "\t\t\tusleep_range(1000, 2000);\n"
     "\t\t} while (time_before(jiffies, k618_end));\n"
     "\t\tif (!ret && !(int_status & CS35L41_OTP_BOOT_DONE))\n"
     "\t\t\tret = -ETIMEDOUT;\n"
     "\t\tdev_info(cs35l41->dev, \"k618: OTP_BOOT_DONE wait: %d\\n\", ret);\n"
     "\t}\n"),
    ("cs35l41.c", "\tu8 asp_wl;\n\tint i;\n", "\tu8 asp_wl, asp_width;\n\tint i;\n"),
    ("cs35l41.c",
     "\tasp_wl = params_width(params);\n",
     "\tasp_wl = params_width(params);\n"
     "\t/* k618: slot width != sample width on the Qualcomm TDM (32-bit slots, S24_LE) */\n"
     "\tasp_width = cs35l41->k618.slot_width ?: params_physical_width(params);\n"
     "\t/* k618: of S32_LE samples the amp takes the top 24 bits, its longest word */\n"
     "\tif (asp_wl > 24)\n"
     "\t\tasp_wl = 24;\n"),
    ("cs35l41.c", "asp_wl << CS35L41_ASP_WIDTH_RX_SHIFT", "asp_width << CS35L41_ASP_WIDTH_RX_SHIFT"),
    ("cs35l41.c", "asp_wl << CS35L41_ASP_WIDTH_TX_SHIFT", "asp_width << CS35L41_ASP_WIDTH_TX_SHIFT"),
    # Main AMP up: the stock applied a fast-switch delta chosen while the DSP was not running at
    # its Main AMP power-up; DAPM runs the DSP1 widget (no register) before Main AMP, so the
    # firmware is running here
    ("cs35l41.c",
     "\t\t\t\t\t    1, &cs35l41->dsp.cs_dsp);\n\t\tbreak;\n",
     "\t\t\t\t\t    1, &cs35l41->dsp.cs_dsp);\n"
     "\t\tk618_fast_switch_pending(cs35l41);\n\t\tbreak;\n"),
    # the older GPIO binding (see k618_gpio_subnode)
    ("cs35l41.c",
     "\thw_cfg->valid = true;\n",
     "\tk618_gpio_subnode(dev, \"cirrus,gpio-config1\", gpio1);\n"
     "\tk618_gpio_subnode(dev, \"cirrus,gpio-config2\", gpio2);\n\n"
     "\thw_cfg->valid = true;\n"),
    ("cs35l41.c",
     "\t\tdev_err_probe(cs35l41->dev, ret, \"Set pdata failed\\n\");\n\t\tgoto err;\n\t}\n",
     "\t\tdev_err_probe(cs35l41->dev, ret, \"Set pdata failed\\n\");\n\t\tgoto err;\n\t}\n"
     "\tk618_cs35l41_pdata(cs35l41);\n"),
    # Speaker protection never starts without this speaker's tuning: with bin_mandatory, a
    # missing <prefix>-cs35l41-dsp1-spk-prot.bin keeps the firmware off -- the DSP path (PCM
    # Source = DSP) then plays nothing, rather than the protection algorithm running on the
    # firmware's built-in defaults. pre_run hands over the calibration (k618_cs35l41_pre_run).
    ("cs35l41.c",
     "\tdsp->toggle_preload = true;\n",
     "\tdsp->toggle_preload = true;\n"
     "\t/* k618: no tuning, no protection firmware; calibration before it starts */\n"
     "\tdsp->bin_mandatory = true;\n"
     "\tdsp->pre_run = k618_cs35l41_pre_run;\n"),
    # the per-amp "Fast Use Case Delta File" control, and the stock's ignore_suspend set (its
    # component probe: "using ignore_suspend WITHOUT name_prefix")
    ("cs35l41.c",
     "\treturn wm_adsp2_component_probe(&cs35l41->dsp, component);\n}\n",
     "\tif (cs35l41->k618.fs_num) {\n"
     "\t\tret = snd_soc_add_component_controls(component, &cs35l41->k618.fs_ctl, 1);\n"
     "\t\tif (ret)\n"
     "\t\t\tdev_warn(cs35l41->dev, \"k618: no fast-switch control: %d\\n\", ret);\n"
     "\t}\n"
     "\tsnd_soc_dapm_ignore_suspend(dapm, \"SPK\");\n"
     "\tsnd_soc_dapm_ignore_suspend(dapm, \"AMP Playback\");\n"
     "\tsnd_soc_dapm_ignore_suspend(dapm, \"AMP Capture\");\n\n"
     "\treturn wm_adsp2_component_probe(&cs35l41->dsp, component);\n}\n"),
    ("cs35l41.c",
     "\t.set_channel_map = cs35l41_set_channel_map,\n};\n",
     "\t.set_channel_map = cs35l41_set_channel_map,\n"
     "\t.set_tdm_slot = k618_cs35l41_set_tdm_slot,\n};\n"),
    # no hibernation unless asked for (k618_hibernate): runtime suspend and resume both return
    # early, so the DSP stays loaded and paused as on the stock
    ("cs35l41.c",
     "\tif (!cs35l41->dsp.preloaded || !cs35l41->dsp.cs_dsp.running)\n\t\treturn 0;\n",
     "\tif (!k618_hibernate || !cs35l41->dsp.preloaded || !cs35l41->dsp.cs_dsp.running)\n"
     "\t\treturn 0;\n"),

    # --- wm_adsp.c: the ROM's tuning file names
    # Xiaomi's driver names the tuning "<prefix>-<part>-<dsp>-<firmware>.bin" with the ASoC name
    # prefix as it is: B-cs35l41-dsp1-spk-prot.bin, T-cs35l41-dsp1-spk-prot.bin (and -spk-cali)
    # in /odm/firmware, found by ueventd (5.15 log: "cirrus firm para B-cs35l41-dsp1-spk-prot.bin",
    # "ueventd: found /odm/firmware/B-..."). Upstream lower-cases every name and puts the prefix
    # last, so it would look for cs35l41-dsp1-spk-prot.bin and find no tuning. The firmware
    # itself, cs35l41-dsp1-spk-prot.wmfw, already has upstream's legacy name.
    ("wm_adsp.c",
     "static const char * const cirrus_dir = \"cirrus/\";\n",
     "/* k618: the phone's tuning name, prefix first and kept as it is (B-/T-...) */\n"
     "static int k618_wm_adsp_request_prefixed_coeff(struct wm_adsp *dsp,\n"
     "\t\t\t\t\t       const struct firmware **firmware,\n"
     "\t\t\t\t\t       char **filename, const char *prefix)\n"
     "{\n"
     "\tconst char *fwf = dsp->fwf_name ? dsp->fwf_name : dsp->cs_dsp.name;\n"
     "\tchar *s;\n"
     "\tint ret;\n\n"
     "\t*filename = kasprintf(GFP_KERNEL, \"%s-%s-%s-%s.bin\", prefix, dsp->part, fwf,\n"
     "\t\t\t      wm_adsp_fw[dsp->fw].file);\n"
     "\tif (!*filename)\n"
     "\t\treturn -ENOMEM;\n"
     "\t/* the rest lower-case, as the stock's \"dsp1\" */\n"
     "\tfor (s = *filename + strlen(prefix); *s; s++)\n"
     "\t\t*s = tolower(*s);\n\n"
     "\tret = firmware_request_nowarn(firmware, *filename, dsp->cs_dsp.dev);\n"
     "\tif (ret) {\n"
     "\t\tadsp_dbg(dsp, \"Failed to request '%s'\\n\", *filename);\n"
     "\t\tkfree(*filename);\n"
     "\t\t*filename = NULL;\n"
     "\t} else {\n"
     "\t\tadsp_info(dsp, \"k618: tuning '%s'\\n\", *filename);\n"
     "\t}\n\n"
     "\treturn ret;\n"
     "}\n\n"
     "static const char * const cirrus_dir = \"cirrus/\";\n"),
    ("wm_adsp.c",
     "\t/* Check legacy location */\n"
     "\tif (!wm_adsp_request_firmware_file(dsp, wmfw_firmware, wmfw_filename,\n"
     "\t\t\t\t\t   \"\", NULL, NULL, \"wmfw\")) {\n"
     "\t\twm_adsp_request_firmware_file(dsp, coeff_firmware, coeff_filename,\n"
     "\t\t\t\t\t      \"\", NULL, NULL, \"bin\");\n"
     "\t\treturn 0;\n"
     "\t}\n",
     "\t/* Check legacy location */\n"
     "\tif (!wm_adsp_request_firmware_file(dsp, wmfw_firmware, wmfw_filename,\n"
     "\t\t\t\t\t   \"\", NULL, NULL, \"wmfw\")) {\n"
     "\t\t/* k618: <prefix>-cs35l41-dsp1-spk-prot.bin first (the ROM's name) */\n"
     "\t\tif (suffix)\n"
     "\t\t\tk618_wm_adsp_request_prefixed_coeff(dsp, coeff_firmware, coeff_filename,\n"
     "\t\t\t\t\t\t\t    suffix);\n"
     "\t\tif (!*coeff_firmware)\n"
     "\t\t\twm_adsp_request_firmware_file(dsp, coeff_firmware, coeff_filename,\n"
     "\t\t\t\t\t\t      \"\", NULL, NULL, \"bin\");\n"
     "\t\treturn 0;\n"
     "\t}\n"),
    # say why the firmware did not start (bin_mandatory, set by the cs35l41 edit above)
    ("wm_adsp.c",
     "\tif (dsp->bin_mandatory && !coeff_firmware) {\n\t\tret = -ENOENT;\n",
     "\tif (dsp->bin_mandatory && !coeff_firmware) {\n"
     "\t\tadsp_err(dsp, \"k618: no tuning (.bin) for %s: firmware not started\\n\",\n"
     "\t\t\t wm_adsp_fw_text[dsp->fw]);\n"
     "\t\tret = -ENOENT;\n"),
]


def main(argv):
    root = argv[1]
    done = 0
    for rel, text in GENERATED:
        path = os.path.join(root, rel)
        if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
            continue
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        done += 1
    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if new in text and (old not in text or old in new):
            continue
        if old not in text:
            print(f"k618_cs35l41_fix: {rel}: text to change not found: {old[:60]!r}", file=sys.stderr)
            return 1
        # every occurrence: the runtime-PM test appears in suspend and resume alike
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text.replace(old, new))
        done += 1
    print(f"k618_cs35l41_fix: {done} of {len(EDITS) + len(GENERATED)} changes applied")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
