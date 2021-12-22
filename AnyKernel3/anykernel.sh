# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel boot install
# begin properties
properties() { '
kernel.string=Meow Kernel by wimbiyoashizkia @ github
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


## AnyKernel install
dump_boot;

# Set Android version for kernel
ver="$(file_getprop /system/build.prop ro.build.version.release)"
if [ ! -z "$ver" ]; then
    patch_cmdline "androidboot.version" "androidboot.version=$ver"
else
    patch_cmdline "androidboot.version" ""
fi

# begin ramdisk changes

# init.rc
backup_file init.rc;
replace_string init.rc "cpuctl cpu,timer_slack" "mount cgroup none /dev/cpuctl cpu" "mount cgroup none /dev/cpuctl cpu,timer_slack";

# init.tuna.rc
backup_file init.tuna.rc;
insert_line init.tuna.rc "nodiratime barrier=0" after "mount_all /fstab.tuna" "\tmount ext4 /dev/block/platform/omap/omap_hsmmc.0/by-name/userdata /data remount nosuid nodev noatime nodiratime barrier=0";
append_file init.tuna.rc "bootscript" init.tuna;

# fstab.tuna
backup_file fstab.tuna;
patch_fstab fstab.tuna /system ext4 options "noatime,barrier=1" "noatime,nodiratime,barrier=0";
patch_fstab fstab.tuna /cache ext4 options "barrier=1" "barrier=0,nomblk_io_submit";
patch_fstab fstab.tuna /data ext4 options "data=ordered" "nomblk_io_submit,data=writeback";
append_file fstab.tuna "usbdisk" fstab;

# end ramdisk changes

# begin Screen OC
ui_print " " "Detecting Screen OC..."
case "$ZIPFILE" in
    *61fps*|*61hz*)
        ui_print "Detected 61hz! Set to 61hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=61"
        ;;
    *62fps*|*62hz*)
        ui_print "Detected 62hz! Set to 62hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=62"
        ;;
    *63fps*|*63hz*)
        ui_print "Detected 63hz! Set to 63hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=63"
        ;;
    *64fps*|*64hz*)
        ui_print "Detected 64hz! Set to 64hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=64"
        ;;
    *65fps*|*65hz*)
        ui_print "Detected 65hz! Set to 65hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=65"
        ;;
    *66fps*|*66hz*)
        ui_print "Detected 66hz! Set to 66hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=66"
        ;;
    *67fps*|*67hz*)
        ui_print "Detected 67hz! Set to 67hz refresh rate..."
        ui_print "WARNING: AT YOUR RISK!!!"
        patch_cmdline "mdss_dsi.custom_refresh_rate" "mdss_dsi.custom_refresh_rate=67"
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
#keep_vbmeta_flag=auto;

# reset for vendor_boot patching
#reset_ak;


## AnyKernel vendor_boot install
#split_boot; # skip unpack/repack ramdisk since we don't need vendor_ramdisk access

#flash_boot;
## end vendor_boot install

