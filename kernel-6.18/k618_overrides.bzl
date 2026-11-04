k618_overrides_config = {
    # Qualcomm's TZ memory allocator mode on the phone (the kernel's choice, not in gki_defconfig)
    "CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE": "y",
    # WALT is written against 6.12's scheduler internals: off until it is ported (or madrid's
    # 6.18 sched-walt.ko is used); the modules that use it have it behind this config
    "CONFIG_SCHED_WALT": "n",
    # the 13 Pro's (kalama's) USB 2 PHY: eUSB2 with a repeater; canoe has another
    "CONFIG_USB_MSM_EUSB2_PHY": "m",
    # kalama's CPU frequency driver, and Gunyah's virtual watchdog the 13 Pro loads at first stage
    "CONFIG_ARM_QCOM_CPUFREQ_HW": "m",
    "CONFIG_GH_VIRT_WATCHDOG": "m",
    # kalama's SoC drivers (kernel-6.12/k618_kalama.py)
    "CONFIG_ARCH_KALAMA": "y",
    "CONFIG_SM_GCC_KALAMA": "m",
    "CONFIG_SM_CAMCC_KALAMA": "m",
    "CONFIG_SM_DISPCC_KALAMA": "m",
    "CONFIG_SM_GPUCC_KALAMA": "m",
    "CONFIG_SM_TCSRCC_KALAMA": "m",
    "CONFIG_SM_VIDEOCC_KALAMA": "m",
    "CONFIG_SM_DEBUGCC_KALAMA": "m",
    "CONFIG_PINCTRL_KALAMA": "m",
    "CONFIG_INTERCONNECT_QCOM_KALAMA": "m",
    "CONFIG_PHY_QCOM_UFS_V4_KALAMA": "m",
    # second-stage modules the 13 Pro loads that canoe leaves off (k618_stage2.txt): run-queue stats,
    # the USB-C audio switch, the camera flash LED
    "CONFIG_QCOM_RUN_QUEUE_STATS": "m",
    "CONFIG_QCOM_FSA4480_I2C": "m",
    "CONFIG_LEDS_QTI_FLASH": "m",
    # the second-stage modules imported from Xiaomi's 5.15 tree (kernel-6.12/k618_fuxi_mods.py):
    # sleep and memory-latency statistics, thermal, LEDs and PWM, MHI, and the rest of the phone's list
    "CONFIG_QCOM_SOC_SLEEP_STATS": "m",
    "CONFIG_QCOM_SUBSYSTEM_SLEEP_STATS": "m",
    "CONFIG_MSM_CORE_HANG_DETECT": "m",
    "CONFIG_USB_BAM": "m",
    "CONFIG_MSM_QBT_HANDLER": "m",
    "CONFIG_QTI_SCMI_MEMLAT_PROTOCOL": "m",
    "CONFIG_QCOM_RIMPS": "m",
    "CONFIG_QTI_THERMAL_LIMITS_DCVS": "m",
    "CONFIG_QTI_CPU_VOLTAGE_COOLING_DEVICE": "m",
    "CONFIG_QTI_POLICY_ENGINE_SENSOR": "m",
    "CONFIG_QTI_SDPM_CLOCK_MONITOR": "m",
    "CONFIG_SPMI_QTI_GLINK_DEBUG": "m",
    "CONFIG_XLOGCHAR": "m",
    # the trust management engine's mailbox client (tmecom-intf), imported from the 5.15 tree:
    # ice.c's one-time key-manager setup needs it before the first wrapped key
    "CONFIG_MSM_TMECOM_QMP": "m",
    "CONFIG_FINGERPRINT_GOODIX_FOD": "m",
    "CONFIG_STM_PROTO_BASIC": "m",
    # the notification light and the PWM provider it hangs off
    "CONFIG_PWM_QTI_LPG": "m",
    "CONFIG_LEDS_QTI_TRI_LED": "m",
    # qcom_stats is the 6.12 successor of the sleep-stats driver this phone actually wants, and the
    # two export the same ddr_stats_* symbols: with both in /lib/modules the second one to be
    # insmod'ed fails ("exports duplicate symbol ddr_stats_get_freq_count"), which in the first
    # stage is fatal. The phone's DT has qcom,rpmh-sleep-stats and no qcom,rpmh-stats, and the ROM
    # ships soc_sleep_stats.ko and no qcom_stats.ko, so this is the one to leave out. The only
    # module that asked for it, sys_pm_vx, needs has_system_slept and has_subsystem_slept, which
    # subsystem_sleep_stats exports as well.
    "CONFIG_QCOM_STATS": "n",
    # canoe's flash-LED driver (soc's drivers/leds/flash/leds-qcom-flash.c) binds
    # "qcom,spmi-flash-led"; this phone's node is "qcom,pm8350c-flash-led", which its own 5.15
    # kernel drives with the older leds-qti-flash (CONFIG_LEDS_QTI_FLASH above). With both options
    # on, the camera driver calls qcom_flash_led_get_max_avail_current -- canoe's driver's -- and
    # would talk to a driver that never binds here, so this one is off and the camera takes the
    # same QTI path as the phone's own camera.ko. No soc module reads the option, only a header's
    # stub, so the techpack build filters it out of the vendor header as well (k618_tp.sh).
    "CONFIG_LEDS_QCOM_FLASH": "n",






    "CONFIG_QCE_OPTIMIZED_BAM": "n",
    # kalama's CPUCP firmware speaks the 5.15 per-algorithm SCMI protocols (memory latency 0x80,
    # PMU map 0x86, C1DCVS 0x87 -- k618_fuxi_mods.py), and the unified Qualcomm vendor protocol 6.x's
    # drivers use claims 0x80 too: with it off qcom-pmu-lib and memlat take the per-protocol path,
    # and qcom_scmi_client (which only that protocol needs) is not built
    "CONFIG_QTI_SCMI_VENDOR_PROTOCOL": "n",
    "CONFIG_QTI_QCOM_SCMI_CLIENT": "n",
    # the protocols and their clients, as the phone's 5.15 kernel loads them
    "CONFIG_QTI_SCMI_PMU_PROTOCOL": "m",
    "CONFIG_QTI_SCMI_C1DCVS_PROTOCOL": "m",
    "CONFIG_QTI_PMU_SCMI_CLIENT": "m",
    "CONFIG_QTI_C1DCVS_SCMI_CLIENT": "m",
    "CONFIG_QTI_HW_MEMLAT_SCMI_CLIENT": "m",



}
