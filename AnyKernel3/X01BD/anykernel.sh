# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=Radiance Kernel by wimbiyoashizkia @ github
do.devicecheck=1
do.modules=1
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=X01B
device.name2=X01BD
device.name3=ASUS_X01B
device.name4=ASUS_X01BD
device.name5=ASUS_X01BDA
supported.versions=12
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

## Overclock screen setup (DO NOT CHANGE)
# begin overclock screen
ui_print " " "Detecting Screen OC..."
case "$ZIPFILE" in
	*61fps*|*61hz*)
		ui_print " "
		ui_print "Detected 61hz!"
		ui_print "Set to 61hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=61"
		;;
	*62fps*|*62hz*)
		ui_print " "
		ui_print "Detected 62hz!"
		ui_print "Set to 62hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=62"
		;;
	*63fps*|*63hz*)
		ui_print " "
		ui_print "Detected 63hz!"
		ui_print "Set to 63hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=63"
		;;
	*64fps*|*64hz*)
		ui_print " "
		ui_print "Detected 64hz!"
		ui_print "Set to 64hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=64"
		;;
	*65fps*|*65hz*)
		ui_print " "
		ui_print "Detected 65hz!"
		ui_print "Set to 65hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=65"
		;;
	*66fps*|*66hz*)
		ui_print "Detected 66hz!"
		ui_print " "
		ui_print "Set to 66hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=66"
		;;
	*67fps*|*67hz*)
		ui_print "Detected 67hz!"
		ui_print " "
		ui_print "Set to 67hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=67"
		;;
	*68fps*|*68hz*)
		ui_print " "
		ui_print "Detected 68hz!"
		ui_print "Set to 68hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=68"
		;;
	*69fps*|*69hz*)
		ui_print " "
		ui_print "Detected 69hz!"
		ui_print "Set to 69hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=69"
		;;
	*)
		ui_print " "
		ui_print "Overclock screen no Detected!"
		ui_print "Set to 60hz refresh rate..."
		patch_cmdline "mdss_dsi.custom_refresh_rate" ""
esac
# end overclock screen

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

