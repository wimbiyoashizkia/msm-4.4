obj-$(CONFIG_PM)	+= sysfs.o generic_ops.o common.o qos.o runtime.o wakeirq.o wakelock_blocker.o
obj-$(CONFIG_PM_SLEEP)	+= main.o wakeup.o wakeup_stats.o
obj-$(CONFIG_PM_TRACE_RTC)	+= trace.o
obj-$(CONFIG_PM_OPP)	+= opp/
obj-$(CONFIG_PM_GENERIC_DOMAINS)	+=  domain.o domain_governor.o
obj-$(CONFIG_HAVE_CLK)	+= clock_ops.o

ccflags-$(CONFIG_DEBUG_DRIVER) := -DDEBUG
