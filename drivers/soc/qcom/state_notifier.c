/*
 * State Notifier Driver
 *
 * Copyright (c) 2013-2018, Pranav Vashi <neobuddy89@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/state_notifier.h>

bool state_suspended;

void state_suspend(void)
{
	state_suspended = true;
}

void state_resume(void)
{
	state_suspended = false;
}

void state_boost(void)
{
	state_suspended = false;
}

MODULE_AUTHOR("Pranav Vashi <neobuddy89@gmail.com>");
MODULE_DESCRIPTION("Suspend state tracker");
MODULE_LICENSE("GPLv2");
