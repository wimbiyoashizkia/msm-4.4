# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=NEON Kernel by wimbiyoashizkia @ github
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=X00T
device.name2=X00TD
device.name3=ASUS_X00T
device.name4=ASUS_X00TD
device.name5=ASUS_X00TDA
supported.versions=
supported.patchlevels=
'; } # end properties

# shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=0;
ramdisk_compression=auto;
patch_vbmeta_flag=auto;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;


## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
set_perm_recursive 0 0 755 644 $ramdisk/*;
set_perm_recursive 0 0 750 750 $ramdisk/init* $ramdisk/sbin;


## AnyKernel boot install
dump_boot;

## Get Android version (DO NOT CHANGE)
# begin checker android version
ver="$(file_getprop /system/build.prop ro.build.version.release)"
if [ ! -z "$ver" ]; then
	ui_print " "
	ui_print "Checking Android version..."
	ui_print "Android version $ver" " "
	patch_cmdline "androidboot.version" "androidboot.version=$ver"
else
	patch_cmdline "androidboot.version" ""
fi
#end checker android version

## LV/NLV (DO NOT CHANGE)
# begin selection of LV/NLV
ui_print " " "Detect supported vibration..."
case "$ZIPFILE" in
	*NLV*|*nlv*)
		ui_print " "
		ui_print "Detected No Leds Vibration"
		ui_print "Set to No Leds Vibration support..." " "
		patch_cmdline "vibration" "vibration=0"
		;;
	*LV*|*lv*)
		ui_print " "
		ui_print "Detected Leds Vibration"
		ui_print "Set to Leds Vibration support..." " "
		patch_cmdline "vibration" "vibration=1"
		;;
	*)
		ui_print " "
		ui_print "File naming LV/NLV has no Detected!"
		ui_print "Set to LV as default..." " "
		patch_cmdline "vibration" ""
esac
# begin selection of LV/NLV

## Undervolt (DO NOT CHANGE)
# begin selection of Undervolt
ui_print " " "Detect Undervolt..."
case "$ZIPFILE" in
	*20mV*|*20mv*)
		ui_print " "
		ui_print "Detected Undervolt"
		ui_print "Set Undervolt 20mV..." " "
		patch_cmdline "uv_gpu" "uv_gpu=20000"
		patch_cmdline "uv_cpu" "uv_cpu=20000"
		;;
	*40mV*|*40mv*)
		ui_print " "
		ui_print "Detected Undervolt"
		ui_print "Set Undervolt 40mV..." " "
		patch_cmdline "uv_gpu" "uv_gpu=40000"
		patch_cmdline "uv_cpu" "uv_cpu=40000"
		;;
	*60mV*|*60mv*)
		ui_print " "
		ui_print "Detected Undervolt"
		ui_print "Set Undervolt 60mV..." " "
		patch_cmdline "uv_gpu" "uv_gpu=60000"
		patch_cmdline "uv_cpu" "uv_cpu=60000"
		;;
	*80mV*|*80mv*)
		ui_print " "
		ui_print "Detected Undervolt"
		ui_print "Set Undervolt 80mV..." " "
		patch_cmdline "uv_gpu" "uv_gpu=80000"
		patch_cmdline "uv_cpu" "uv_cpu=80000"
		;;
	*)
		ui_print " "
		ui_print "Undervolt has no Detected!"
		ui_print "Set to 0mV as default..." " "
		patch_cmdline "uv_gpu" ""
		patch_cmdline "uv_cpu" ""
esac
# begin selection of Undervolt

write_boot;
## end boot install


# shell variables
#block=vendor_boot;
#is_slot_device=1;
#ramdisk_compression=auto;
#patch_vbmeta_flag=auto;

# reset for vendor_boot patching
#reset_ak;


## AnyKernel vendor_boot install
#split_boot; # skip unpack/repack ramdisk since we don't need vendor_ramdisk access

#flash_boot;
## end vendor_boot install

