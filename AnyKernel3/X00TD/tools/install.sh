#!/sbin/sh

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

# Import Remover
. /tmp/anykernel/tools/remover.sh;

# begin detecting filenaming
ui_print " "
ui_print " " "# DETECTING FILENAMING !!!"

## LV/NLV (DO NOT CHANGE)
# begin selection of LV/NLV
ui_print " "
ui_print "# Detecting haptic drivers..."
case "$ZIPFILE" in
  *NLV*|*nlv*)
    ui_print " "
    ui_print "Detected QPNP Haptic (NLV)"
    ui_print "Set to QPNP Haptic support..." " "
    patch_cmdline "vibration" "vibration=0"
    ;;
  *LV*|*lv*)
    ui_print " "
    ui_print "Detected Leds QPNP Haptics (LV)"
    ui_print "Set to Leds QPNP Haptics support..." " "
    patch_cmdline "vibration" "vibration=1"
    ;;
  *)
    ui_print " "
    ui_print "Filenaming LV/NLV has no Detected!"
    ui_print "Set to Leds QPNP Haptics as default..." " "
    patch_cmdline "vibration" ""
esac

ui_print "NOTE: If the vibration not working or bootloop issue"
ui_print "      after flashing, please reflash kernel and"
ui_print "      select another option of Haptic driver." " "
# end selection of LV/NLV

## PELT (DO NOT CHANGE)
# begin selection of PELT
ui_print " "
ui_print "# Detecting set up PELT..."

case "$ZIPFILE" in
  *8ms*)
    ui_print " "
    ui_print "Detected PELT 8ms"
    ui_print "Set PELT to 8ms..." " "
    patch_cmdline "pelt" "pelt=8"
    ;;
  *16ms*)
    ui_print " "
    ui_print "Detected PELT 16ms"
    ui_print "Set PELT to 16ms..." " "
    patch_cmdline "pelt" "pelt=16"
    ;;
  *32ms*)
    ui_print " "
    ui_print "Detected PELT 32ms"
    ui_print "Set PELT to 32ms..." " "
    patch_cmdline "pelt" "pelt=32"
    ;;
  *)
    ui_print " "
    ui_print "PELT setup has no Detected!"
    ui_print "Set PELT 32ms as default..." " "
    patch_cmdline "pelt" ""
esac
# end selection of PELT

## Undervolt (DO NOT CHANGE)
# begin selection of Undervolt
ui_print " "
ui_print "# Detecting Undervolt..."
case "$ZIPFILE" in
  *20mV*|*20mv*)
    ui_print " "
    ui_print "Detected Undervolt"
    ui_print "Set Undervolt to 20mV..." " "
    patch_cmdline "uv_gpu" "uv_gpu=20000"
    patch_cmdline "uv_cpu" "uv_cpu=20000"
    ;;
  *40mV*|*40mv*)
    ui_print " "
    ui_print "Detected Undervolt"
    ui_print "Set Undervolt to 40mV..." " "
    patch_cmdline "uv_gpu" "uv_gpu=40000"
    patch_cmdline "uv_cpu" "uv_cpu=40000"
    ;;
  *60mV*|*60mv*)
    ui_print " "
    ui_print "Detected Undervolt"
    ui_print "Set Undervolt to 60mV..." " "
    patch_cmdline "uv_gpu" "uv_gpu=60000"
    patch_cmdline "uv_cpu" "uv_cpu=60000"
    ;;
  *80mV*|*80mv*)
    ui_print " "
    ui_print "Detected Undervolt"
    ui_print "Set Undervolt to 80mV..." " "
    patch_cmdline "uv_gpu" "uv_gpu=80000"
    patch_cmdline "uv_cpu" "uv_cpu=80000"
    ;;
  *)
    ui_print " "
    ui_print "Undervolt has no Detected!"
    ui_print "Undervolt CPU & GPU not set..." " "
    patch_cmdline "uv_gpu" ""
    patch_cmdline "uv_cpu" ""
esac

ui_print "NOTE: If the device bootloop after flashing,"
ui_print "      please reflash kernel and don't use"
ui_print "      Undervolt CPU & GPU." " "
# end selection of Undervolt

# end detecting filenaming
