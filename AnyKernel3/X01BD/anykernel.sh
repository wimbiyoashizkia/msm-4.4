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
device.name1=X01B
device.name2=X01BD
device.name3=ASUS_X01B
device.name4=ASUS_X01BD
device.name5=ASUS_X01BDA
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

