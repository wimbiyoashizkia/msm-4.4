# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel boot install
# begin properties
properties() { '
kernel.string=NEON Kernel by wimbiyoashizkia @ github
do.devicecheck=1
do.modules=0
do.systemless=1
do.spectrum=1
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


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;


## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
set_perm_recursive 0 0 755 644 $ramdisk/*;
set_perm_recursive 0 0 750 750 $ramdisk/init* $ramdisk/sbin;


## AnyKernel install
dump_boot;

# begin EAS patch changes
if [ ! -e "/vendor/etc/powerhint.json" ]; then
    ui_print " " "HMP ROM Detected!"
    ui_print "Installing Module EAS Perf HAL..."
    rm -rf /data/adb/modules/neon;
    cp -rf $home/patch/eas-perfhal /data/adb/modules/neon;
else
    ui_print " " "EAS ROM Detected!"
    ui_print "No need Module EAS Perf HAL!!!"
fi
# end EAS patch changes

# begin Screen OC
ui_print " " "Detecting Screen OC..."
case "$ZIPFILE" in
	*63fps*|*63hz*)
		ui_print "Detected 63hz! Set to 63hz refresh rate..."
		ui_print "WARNING: AT YOUR RISK!!!"
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=63"
		;;
	*65fps*|*65hz*)
		ui_print "Detected 65hz! Set to 65hz refresh rate..."
		ui_print "WARNING: AT YOUR RISK!!!"
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=65"
		;;
	*67fps*|*67hz*)
		ui_print "Detected 67hz! Set to 67hz refresh rate..."
		ui_print "WARNING: AT YOUR RISK!!!"
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=67"
		;;
	*69fps*|*69hz*)
		ui_print "Detected 69hz! Set to 69hz refresh rate..."
		ui_print "WARNING: AT YOUR RISK!!!"
		ui_print "You’re using the max refresh rate for X00T/D currently."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=69"
		;;
	*)
		ui_print "Screen OC no Detected! Set to 60hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" ""
esac
# end Screen OC

write_boot;
## end boot install


# shell variables
#block=vendor_boot;
#is_slot_device=1;
#ramdisk_compression=auto;

# reset for vendor_boot patching
#reset_ak;


## AnyKernel vendor_boot install
#split_boot; # skip unpack/repack ramdisk since we don't need vendor_ramdisk access

#flash_boot;
## end vendor_boot install

