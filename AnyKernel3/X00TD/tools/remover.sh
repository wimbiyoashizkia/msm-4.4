#!/sbin/sh
#
# NEON Remover
# remove junk from another kernel

ui_print " ";
ui_print "Removing junk from other kernel...";

if [ -f $ramdisk/balance.txt ]; then
  rm -rf $ramdisk/balance.txt;
fi
if [ -f $ramdisk/battery.txt ]; then
  rm -rf $ramdisk/battery.txt
fi
if [ -f $ramdisk/init.azure.rc ]; then
  rm -rf $ramdisk/init.azure.rc;
fi
if [ -f $ramdisk/init.boost.rc ]; then
  rm -rf $ramdisk/init.boost.rc;
fi
if [ -f $ramdisk/init.darkonah.rc ]; then
  rm -rf $ramdisk/init.darkonah.rc;
fi
if [ -f $ramdisk/init.overdose.rc ]; then
  rm -rf $ramdisk/init.overdose.rc;
fi
if [ -f $ramdisk/init.Pbh.rc ]; then
  rm -rf $ramdisk/init.Pbh.rc;
fi
if [ -f $ramdisk/init.PbH.rc ]; then
  rm -rf $ramdisk/init.PbH.rc;
fi
if [ -f $ramdisk/init.spectrum.rc ]; then
  rm -rf $ramdisk/init.spectrum.rc;
fi
if [ -f $ramdisk/init.spectrum.sh ]; then
  rm -rf $ramdisk/init.spectrum.sh;
fi
if [ -f $ramdisk/init.trb.rc ]; then
  rm -rf $ramdisk/init.trb.rc;
fi
if [ -f $ramdisk/init.special_power.sh ]; then
  rm -rf $ramdisk/init.special_power.sh;
fi
if [ -f $ramdisk/init.pk.rc ]; then
  rm -rf $ramdisk/init.pk.rc;
fi
if [ -f $ramdisk/init.thundercloud.rc ]; then
  rm -rf $ramdisk/init.thundercloud.rc;
fi
if [ -f $ramdisk/init.thundercloud.sh ]; then
  rm -rf $ramdisk/init.thundercloud.sh;
fi
if [ -f $ramdisk/init.Singh.rc ]; then
  rm -rf $ramdisk/init.Singh.rc;
fi
if [ -f $ramdisk/performance.txt ]; then
  rm -rf $ramdisk/performance.txt
fi

remove_line $ramdisk/init.rc "import /init.azure.rc"
remove_line $ramdisk/init.rc "import /init.trb.rc"
remove_line $ramdisk/init.rc "import /init.PbH.rc"
remove_line $ramdisk/init.rc "import /init.Pbh.rc"
remove_line $ramdisk/init.rc "import /init.darkonah.rc"
remove_line $ramdisk/init.rc "import /init.overdose.rc"
remove_line $ramdisk/init.rc "import /init.spectrum.rc"
remove_line $ramdisk/init.rc "import /init.stardust.rc"
remove_line $ramdisk/init.rc "import /init.thundercloud.rc"
remove_line $ramdisk/init.rc "import /init.pk.rc"
remove_line $ramdisk/init.rc "import /init.boost.rc"
remove_line $ramdisk/init.rc "import /init.Singh.rc"

ui_print "Removing junk Finish...";
