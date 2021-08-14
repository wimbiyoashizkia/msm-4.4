#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2020 Wimbi Yoas Hizkia (wimbiyoas)
#

# Color
green='\033[0;32m'
echo -e "$green"

# Main Environment
KERNEL_DIR="${PWD}"
CCACHE=$(command -v ccache)
HOME=/home/wimbiyoas
KERN_IMG=$KERNEL_DIR/out/arch/arm64/boot/Image.gz-dtb
ZIP_DIR=$KERNEL_DIR/AnyKernel3
CONFIG_DIR=$KERNEL_DIR/arch/arm64/configs
CONFIG="X00T_defconfig"
CORES=$(grep -c ^processor /proc/cpuinfo)
THREAD="-j$CORES"

# Export
export ARCH=arm64
export KBUILD_BUILD_USER="wimbiyoas"
export KBUILD_BUILD_HOST="Linux"
export PATH="$HOME/android/toolchain/neon-clang/bin:$PATH"

# Main script
while true; do
echo -e "\n################################"
echo -e "[1] Build"
echo -e "[2] Create flashable zip"
echo -e "[3] Cleanup Source"
echo -e "[4] Quit"
echo -e "################################"
echo -ne "\nPlease enter a choice[1-4]: "
    
    read choice
    
    if [ "$choice" == "1" ]; then
        BUILD_START=$(date +"%s")
        DATE=`date`

        echo -e "\nBuild started at $DATE using $CORES thread"
        echo -e "This takes a few minutes, please wait a moment !!!\n"

        make O=out clean &>/dev/null
        make mrproper &>/dev/null
        make O=out $CONFIG $THREAD &>/dev/null & pid=$!
        make -j$(nproc --all) O=out \
                        ARCH=${ARCH} \
                        CC=clang \
                        AR=llvm-ar \
                        NM=llvm-nm \
                        OBJCOPY=llvm-objcopy \
                        OBJDUMP=llvm-objdump \
                        STRIP=llvm-strip \
                        CROSS_COMPILE=aarch64-linux-gnu- \
                        CROSS_COMPILE_ARM32=arm-linux-gnueabi-

        spin[0]="-"
        spin[1]="\\"
        spin[2]="|"
        spin[3]="/"
        echo -e "\n[Please wait...] ${spin[0]}"
        echo -e " "
        while kill -0 $pid &>/dev/null
        do
            for i in "${spin[@]}"
            do
                echo -ne "\b$i"
                sleep 0.1
            done
        done
    
        if ! [ -a $KERN_IMG ]; then
            echo -e "\n[!] Kernel compilation failed, See buildlog to fix errors"
            exit 1
        fi
    
        BUILD_END=$(date +"%s")
        DIFF=$(($BUILD_END - $BUILD_START))

        echo -e "Image-dtb compiled successfully."
        echo -e "Total time elapsed: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
    fi
    
    if [ "$choice" == "2" ]; then
        cd $ZIP_DIR
        make clean &>/dev/null
        cp $KERN_IMG $ZIP_DIR/Image.gz-dtb
        make normal &>/dev/null
        cd ..

        echo -e "\nFlashable zip generated," 
        echo -e "File in $ZIP_DIR."
    fi

    if [ "$choice" == "3" ]; then
        rm -rf out/*

        echo -e "\nKernel source cleaned up."
    fi
    
    if [ "$choice" == "4" ]; then
        exit 
    fi

done
echo -e "$nc"