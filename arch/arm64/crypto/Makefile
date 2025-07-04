#
# linux/arch/arm64/crypto/Makefile
#
# Copyright (C) 2014 Linaro Ltd <ard.biesheuvel@linaro.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#

obj-$(CONFIG_CRYPTO_SHA1_ARM64_CE) += sha1-ce.o
sha1-ce-y := sha1-ce-glue.o sha1-ce-core.o

obj-$(CONFIG_CRYPTO_SHA2_ARM64_CE) += sha2-ce.o
sha2-ce-y := sha2-ce-glue.o sha2-ce-core.o

obj-$(CONFIG_CRYPTO_GHASH_ARM64_CE) += ghash-ce.o
ghash-ce-y := ghash-ce-glue.o ghash-ce-core.o

obj-$(CONFIG_CRYPTO_POLY_HASH_ARM64_CE) += poly-hash-ce.o
poly-hash-ce-y := poly-hash-ce-glue.o poly-hash-ce-core.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE) += aes-ce-cipher.o
aes-ce-cipher-y := aes-ce-core.o aes-ce-glue.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE_CCM) += aes-ce-ccm.o
aes-ce-ccm-y := aes-ce-ccm-glue.o aes-ce-ccm-core.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE_BLK) += aes-ce-blk.o
aes-ce-blk-y := aes-glue-ce.o aes-ce.o

obj-$(CONFIG_CRYPTO_AES_ARM64_NEON_BLK) += aes-neon-blk.o
aes-neon-blk-y := aes-glue-neon.o aes-neon.o

obj-$(CONFIG_CRYPTO_AES_ARM64) += aes-arm64.o
aes-arm64-y := aes-cipher-core.o aes-cipher-glue.o

AFLAGS_aes-ce.o		:= -DINTERLEAVE=4
AFLAGS_aes-neon.o	:= -DINTERLEAVE=4

ifeq ($(cc-name),gcc)
ccflags-y += -mcpu=cortex-a73.cortex-a53 -mtune=cortex-a73.cortex-a53 -march=armv8-a+crc+crypto
asflags-y += -mcpu=cortex-a73.cortex-a53 -mtune=cortex-a73.cortex-a53 -march=armv8-a+crc+crypto
endif

CFLAGS_aes-glue-ce.o	:= -DUSE_V8_CRYPTO_EXTENSIONS

obj-$(CONFIG_CRYPTO_CRC32_ARM64) += crc32-arm64.o

CFLAGS_crc32-arm64.o	:= -mcpu=generic+crc

$(obj)/aes-glue-%.o: $(src)/aes-glue.c FORCE
	$(call if_changed_rule,cc_o_c)
