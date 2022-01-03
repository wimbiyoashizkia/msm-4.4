#!/system/bin/sh
MODDIR=${0%/*}

# Detect whether Unlocked into System
while $(dumpsys window policy | grep mIsShowing | awk -F= '{print $2}')
do
sleep 1
done

# Optimizations
stop energy-awareness

# Kill system's Power HAL
stop vendor.power-hal-1-2
stop vendor.power-hal-1-1
stop vendor.power-hal-1-0

# Init Power HAL
# Without this hal will waiting for it
setprop vendor.powerhal.init 1

chown system system /dev/stune/top-app/schedtune.boost
    chown system system /sys/class/kgsl/kgsl-3d0/devfreq/min_freq
    chown system system /sys/class/kgsl/kgsl-3d0/devfreq/max_freq
    chown system system /sys/class/kgsl/kgsl-3d0/force_rail_on
    chown system system /sys/class/kgsl/kgsl-3d0/force_clk_on
    chown system system /sys/class/kgsl/kgsl-3d0/idle_timer
    chown system system /sys/class/devfreq/soc:qcom,cpubw/min_freq
    chown system system /sys/class/devfreq/soc:qcom,cpubw/bw_hwmon/hyst_trigger_count
    chown system system /sys/class/devfreq/soc:qcom,cpubw/bw_hwmon/hist_memory
    chown system system /sys/class/devfreq/soc:qcom,cpubw/bw_hwmon/hyst_length

# Start EAS SDM660 Power HAL
/vendor/bin/hw/android.hardware.power@1.3-service.pixel-libperfmgr
# Boot ended
