# Makefile for Linux samples code

obj-$(CONFIG_SAMPLES)	+= kobject/ kprobes/ trace_events/ livepatch/ \
			   hw_breakpoint/ kfifo/ kdb/ hidraw/ rpmsg/ seccomp/ \
			   configfs/

# Squeeze a little bit more juice
ccflags-y := -O3
ccflags-y += $(call cc-option, -g0)
ccflags-y += $(call cc-option, -Os)
