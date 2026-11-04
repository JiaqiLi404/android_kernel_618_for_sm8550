### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers

### AnyKernel setup
# global properties
properties() { '
kernel.string=Xiaomi 13 Pro - Linux 6.18
do.devicecheck=0
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
'; } # end properties


### AnyKernel install
## boot shell variables
# A/B GKI boot partition and the variable names required by the pinned AnyKernel3 core.
BLOCK=boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;
# The package replaces the complete ramdisk and integrates KernelSU. A compressed replacement
# ramdisk is not valid input for the core's Magisk cpio test, so disable that detection branch.
NO_MAGISK_CHECK=1;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# boot install
# The zip carries the kernel (Image) and its ramdisk (ramdisk-new.cpio.lz4, which
# ak3-core.sh takes as the whole ramdisk, and magiskboot keeps as it is because it is already
# compressed -- RAMDISK_COMPRESSION is forced to auto for a v4 header anyway). Both are needed:
# the kernel does carry its own initramfs, but the bootloader also hands over the ROM's vendor
# ramdisk, whose 328 5.15 modules would be unpacked over the built-in ones; the boot partition's
# ramdisk is unpacked last, as it is when the image is `fastboot boot`ed.
split_boot;


k618_cmdline="
log_buf_len=16M
sysctl.kernel.panic_on_oops=0
printk.devkmsg=on
sysctl.kernel.printk=1
panic=5
initcall_blacklist=qcom_geni_serial_init
msm_drm.k618_cont_splash=0
msm_drm.k618_bl_level=1200
kernelsu.allow_shell=1
sysctl.kernel.sched_util_clamp_min_rt_default=0
android_arch_task_struct_size=1024
sched_proxy_exec=0
erofs.reserved_pages=64
restrict_cma_redirect=false
trace_buf_size=8K
rcu_nocbs=all
rcutree.enable_rcu_lazy=1
";
k618_cmdline=$(echo $k618_cmdline);
if [ -f $SPLITIMG/cmdline.txt ]; then
  echo "$k618_cmdline" > $SPLITIMG/cmdline.txt;
  [ "$(cat $SPLITIMG/cmdline.txt)" = "$k618_cmdline" ] || abort "Setting the command line failed. Aborting...";
else
  sed -i "s|^cmdline=.*|cmdline=$k618_cmdline|" $SPLITIMG/header;
  grep -q -F -x "cmdline=$k618_cmdline" $SPLITIMG/header || abort "Setting the command line failed. Aborting...";
fi;

flash_boot;
## end boot install
