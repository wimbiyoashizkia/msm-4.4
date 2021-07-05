# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel boot install
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

ui_print " "
ui_print "Swipe up the screen or press any volume key first"

# Keycheck
INSTALLER=$(pwd)
KEYCHECK=$INSTALLER/tools/keycheck
chmod 755 $KEYCHECK

keytest() {
  (/system/bin/getevent -lc 1 2>&1 | /system/bin/grep VOLUME | /system/bin/grep " DOWN" > $INSTALLER/events) || return 1
  return 0
}

choose() {
  #note from chainfire @xda-developers: getevent behaves weird when piped, and busybox grep likes that even less than toolbox/toybox grep
  while true; do
    /system/bin/getevent -lc 1 2>&1 | /system/bin/grep VOLUME | /system/bin/grep " DOWN" > $INSTALLER/events
    if (`cat $INSTALLER/events 2>/dev/null | /system/bin/grep VOLUME >/dev/null`); then
      break
    fi
  done
  if (`cat $INSTALLER/events 2>/dev/null | /system/bin/grep VOLUMEUP >/dev/null`); then
    return 0
  else
    return 1
  fi
}

chooseold() {
  # Calling it first time detects previous input. Calling it second time will do what we want
  $KEYCHECK
  $KEYCHECK
  SEL=$?
  if [ "$1" == "UP" ]; then
    UP=$SEL
  elif [ "$1" == "DOWN" ]; then
    DOWN=$SEL
  elif [ $SEL -eq $UP ]; then
    return 0
  elif [ $SEL -eq $DOWN ]; then
    return 1
  else
    ui_print " Vol key not detected!"
    abort " Use name change method in TWRP"
  fi
}

if [ -z $NEW ]; then
  if keytest; then
    FUNCTION=choose
  else
    FUNCTION=chooseold
    ui_print " "
    ui_print "- Vol Key Programming -"
    ui_print "   Press Volume Up Key: "
    $FUNCTION "UP"
    ui_print "   Press Volume Down Key: "
    $FUNCTION "DOWN"
  fi
  ui_print " "
  ui_print "- Select Option -"
  ui_print "  Choose which option EAS PowerHAL you want: "
  ui_print "  + Volume Up = PowerHAL GooglePixel"
  ui_print "  - Volume Down = PowerHAL common "
  if $FUNCTION; then
    NEW=true
  else
    NEW=false
  fi
else
  ui_print " Option specified in zipname!"
fi


## AnyKernel install
dump_boot;

# Set Android version for kernel
ver="$(file_getprop /system/build.prop ro.build.version.release)"
if [ ! -z "$ver" ]; then
    patch_cmdline "androidboot.version" "androidboot.version=$ver"
else
    patch_cmdline "androidboot.version" ""
fi

# Concatenate all of the dtbs to the kernel
if $NEW; then
	rm -rf /data/adb/modules/neon;
	cp -rf $home/patch/powerhal-google /data/adb/modules/neon;
else
	rm -rf /data/adb/modules/neon;
	cp -rf $home/patch/powerhal-common /data/adb/modules/neon;
fi

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
#patch_vbmeta_flag=auto;

# reset for vendor_boot patching
#reset_ak;


## AnyKernel vendor_boot install
#split_boot; # skip unpack/repack ramdisk since we don't need vendor_ramdisk access

#flash_boot;
## end vendor_boot install

