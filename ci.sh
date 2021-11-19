#! /bin/bash

#
# Script for building Android arm64 Kernel
#
# Copyright (c) 2021 Wimbi Yoas Hizkia <wimbiyoashizkia@yahoo.com>
# Based on Panchajanya1999 script.
#

# Function to show an informational message
msg() {
    echo -e "\e[1;32m$*\e[0m"
}

err() {
    echo -e "\e[1;41m$*\e[0m"
    exit 1
}

# Set environment for directory
KERNEL_DIR=$PWD

# Set enviroment for naming kernel
MODEL="Asus Zenfone Max Pro M1"
DEVICE="X00T"
KERNEL="NEON"
KERNELTYPE="EAS"
TYPE="Private"

# Get defconfig file
DEFCONFIG=X00T_defconfig

# Get branch name
CI_BRANCH=$(git rev-parse --abbrev-ref HEAD)
export CI_BRANCH

# Set environment for etc.
export ARCH=arm64
export SUBARCH=arm64
export KBUILD_BUILD_USER="wimbiyoas"

#
# Set if do you use GCC or clang compiler
# Default is clang compiler
#
COMPILER=gcc

# Set environment for telegram
export CHATID="-1001347363864"
export token="1686322470:AAGXAiglWR8ktsqyjwPx4AXr66LZjWoQt80"
export BOT_MSG_URL="https://api.telegram.org/bot$token/sendMessage"
export BOT_BUILD_URL="https://api.telegram.org/bot$token/sendDocument"

# Get distro name
DISTRO=$(cat /etc/issue)

# Get all cores of CPU
PROCS=$(nproc --all)
export PROCS

# Check for CI
if [[ -n "$CI" ]]; then
	if [[ -n "$CIRCLECI" ]]; then
		export KBUILD_BUILD_VERSION=$CIRCLE_BUILD_NUM
		export KBUILD_BUILD_HOST="CircleCI"
		export CI_BRANCH=$CIRCLE_BRANCH
	else
		export KBUILD_BUILD_HOST="Windows"
	fi
fi

# Check kernel version
KERVER=$(make kernelversion)

# Get last commit
COMMIT_HEAD=$(git log --oneline -1)

# Set Date 
DATE=$(TZ=Asia/Jakarta date +"%Y%m%d-%T")

# Set function for cloning repository
clone() {
	echo " "
	if [[ $COMPILER == "clang" ]]; then
		# Clone NEON clang
		git clone --depth=1 https://github.com/wimbiyoashizkia/NEON-Clang clang
		# Set environment for clang
		TC_DIR=$KERNEL_DIR/clang
		# Get path and compiler string
		KBUILD_COMPILER_STRING=$("$TC_DIR"/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')
		PATH=$TC_DIR/bin/:$PATH
	elif [[ $COMPILER == "gcc" ]]; then
		# Clone GCC ARM64 and ARM32
		git clone --depth=1 https://github.com/fiqri19102002/aarch64-gcc -b elf-gcc-10-tarballs gcc64
		git clone --depth=1 https://github.com/fiqri19102002/arm-gcc -b elf-gcc-10-tarballs gcc32
		# Set environment for GCC ARM64 and ARM32
		GCC64_DIR=$KERNEL_DIR/gcc64
		GCC32_DIR=$KERNEL_DIR/gcc32
		# Get path and compiler string
		KBUILD_COMPILER_STRING=$("$GCC64_DIR"/bin/aarch64-elf-gcc --version | head -n 1)
		PATH=$GCC64_DIR/bin/:$GCC32_DIR/bin/:/usr/bin:$PATH
	fi

	export PATH KBUILD_COMPILER_STRING
}

# Set function for telegram
tg_post_msg() {
	curl -s -X POST "$BOT_MSG_URL" -d chat_id="$2" \
	-d "disable_web_page_preview=true" \
	-d "parse_mode=html" \
	-d text="$1"
}

tg_post_build() {
	# Post MD5 Checksum alongwith for easeness
	MD5CHECK=$(md5sum "$1" | cut -d' ' -f1)

	# Show the Checksum alongwith caption
	curl --progress-bar -F document=@"$1" "$BOT_BUILD_URL" \
	-F chat_id="$2"  \
	-F "disable_web_page_preview=true" \
	-F "parse_mode=html" \
	-F caption="$3 | <b>MD5 Checksum : </b><code>$MD5CHECK</code>"  
}

# Set function for naming zip file
setversioning() {
    KERNELNAME="$KERNEL-$DEVICE-$KERNELTYPE-$TYPE-$DATE"
    export KERNELTYPE KERNELNAME
    export ZIPNAME="$KERNELNAME.zip"
}

# Set function for starting compile
build_kernel() {
	echo -e "Kernel compilation starting"

	tg_post_msg "<b>Device : </b><code>$MODEL [$DEVICE]</code>%0A<b>Branch : </b><code>$CI_BRANCH</code>%0A<b>Type : </b><code>$TYPE</code>%0A<b>Kernel Version : </b><code>$KERVER</code>%0A<b>Date : </b><code>$(TZ=Asia/Jakarta date)</code>%0A<b>Compiler Used : </b><code>$KBUILD_COMPILER_STRING</code>%0a<b>Last Commit : </b><code>$COMMIT_HEAD</code>%0A" "$CHATID"

	make O=out $DEFCONFIG

	BUILD_START=$(date +"%s")

	if [[ $COMPILER == "clang" ]]; then
		make -j"$PROCS" O=out \
				CC=clang \
				ARCH=arm64 \
				AR=llvm-ar \
				NM=llvm-nm \
				OBJCOPY=llvm-objcopy \
				OBJDUMP=llvm-objdump \
				STRIP=llvm-strip \
				CLANG_TRIPLE=aarch64-linux-gnu- \
				CROSS_COMPILE=aarch64-linux-gnu- \
				CROSS_COMPILE_ARM32=arm-linux-gnueabi-
	elif [[ $COMPILER == "gcc" ]]; then
		export CROSS_COMPILE_ARM32=$GCC32_DIR/bin/arm-eabi-
		make -j"$PROCS" O=out CROSS_COMPILE=aarch64-elf-
	fi

	BUILD_END=$(date +"%s")
	DIFF=$((BUILD_END - BUILD_START))

	if [[ -f "$KERNEL_DIR"/out/arch/arm64/boot/Image.gz-dtb ]]; then
		echo -e "Kernel successfully compiled"
	elif ! [[ -f $KERNEL_DIR/out/arch/arm64/boot/Image.gz-dtb ]]; then
		echo -e "Kernel compilation failed"
		tg_post_msg "<b>Build failed to compile after $((DIFF / 60)) minute(s) and $((DIFF % 60)) seconds</b>" "$CHATID" 
		exit 1
	fi
}

# Set function for zipping into a flashable zip
gen_zip() {
	mv "$KERNEL_DIR"/out/arch/arm64/boot/Image.gz-dtb AnyKernel3/Image.gz-dtb
	cd AnyKernel3 || exit
	zip -r9 "$ZIPNAME" * -x .git README.md

	# Prepare a final zip variable
	ZIP_FINAL="$ZIPNAME"

	tg_post_build "$ZIP_FINAL" "$CHATID" "Build took : $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)"
	cd ..
}

setversioning
clone
build_kernel
gen_zip
