obj-$(CONFIG_CRYPTO_DEV_ATMEL_AES) += atmel-aes.o
obj-$(CONFIG_CRYPTO_DEV_ATMEL_SHA) += atmel-sha.o
obj-$(CONFIG_CRYPTO_DEV_ATMEL_TDES) += atmel-tdes.o
obj-$(CONFIG_CRYPTO_DEV_BFIN_CRC) += bfin_crc.o
obj-$(CONFIG_CRYPTO_DEV_CCP) += ccp/
obj-$(CONFIG_CRYPTO_DEV_FSL_CAAM) += caam/
obj-$(CONFIG_CRYPTO_DEV_GEODE) += geode-aes.o
obj-$(CONFIG_CRYPTO_DEV_HIFN_795X) += hifn_795x.o
obj-$(CONFIG_CRYPTO_DEV_IMGTEC_HASH) += img-hash.o
obj-$(CONFIG_CRYPTO_DEV_IXP4XX) += ixp4xx_crypto.o
obj-$(CONFIG_CRYPTO_DEV_MV_CESA) += mv_cesa.o
obj-$(CONFIG_CRYPTO_DEV_MARVELL_CESA) += marvell/
obj-$(CONFIG_CRYPTO_DEV_MXS_DCP) += mxs-dcp.o
obj-$(CONFIG_CRYPTO_DEV_NIAGARA2) += n2_crypto.o
n2_crypto-y := n2_core.o n2_asm.o
obj-$(CONFIG_CRYPTO_DEV_NX) += nx/
obj-$(CONFIG_CRYPTO_DEV_OMAP_AES) += omap-aes.o
obj-$(CONFIG_CRYPTO_DEV_OMAP_DES) += omap-des.o
obj-$(CONFIG_CRYPTO_DEV_OMAP_SHAM) += omap-sham.o
obj-$(CONFIG_CRYPTO_DEV_PADLOCK_AES) += padlock-aes.o
obj-$(CONFIG_CRYPTO_DEV_PADLOCK_SHA) += padlock-sha.o
obj-$(CONFIG_CRYPTO_DEV_PICOXCELL) += picoxcell_crypto.o
obj-$(CONFIG_CRYPTO_DEV_PPC4XX) += amcc/
obj-$(CONFIG_CRYPTO_DEV_S5P) += s5p-sss.o
obj-$(CONFIG_CRYPTO_DEV_SAHARA) += sahara.o
obj-$(CONFIG_CRYPTO_DEV_TALITOS) += talitos.o
obj-$(CONFIG_CRYPTO_DEV_UX500) += ux500/
obj-$(CONFIG_CRYPTO_DEV_QAT) += qat/
obj-$(CONFIG_CRYPTO_DEV_QCE) += qce/
obj-$(CONFIG_CRYPTO_DEV_VMX) += vmx/
obj-$(CONFIG_CRYPTO_DEV_SUN4I_SS) += sunxi-ss/
obj-$(CONFIG_ARCH_QCOM) += msm/

ifeq ($(cc-name),gcc)
ccflags-y += -mcpu=cortex-a73.cortex-a53 -mtune=cortex-a73.cortex-a53 -march=armv8-a+crc+crypto
asflags-y += -mcpu=cortex-a73.cortex-a53 -mtune=cortex-a73.cortex-a53 -march=armv8-a+crc+crypto
endif
