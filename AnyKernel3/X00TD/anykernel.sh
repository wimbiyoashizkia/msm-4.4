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

## PELT scheduler (DO NOT CHANGE)
# begin selection of PELT
ui_print " " "Detecting set up PELT..."
case "$ZIPFILE" in
	*8ms*)
		ui_print " "
		ui_print "Detected PELT 8ms"
		ui_print "Set to PELT 8ms..."
		patch_cmdline "pelt" "pelt=8"
		;;
	*16ms*)
		ui_print " "
		ui_print "Detected PELT 16ms"
		ui_print "Set to PELT 16ms..."
		patch_cmdline "pelt" "pelt=16"
		;;
	*32ms*)
		ui_print " "
		ui_print "Detected PELT 32ms"
		ui_print "Set to PELT 32ms..."
		patch_cmdline "pelt" "pelt=32"
		;;
	*)
		ui_print " "
		ui_print "Set up PELT has no Detected!"
		ui_print "Set to 32ms as default..."
		patch_cmdline "pelt" ""
esac
# end selection of PELT

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

