#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/setup.h>

static char new_command_line[COMMAND_LINE_SIZE];

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", new_command_line);
	return 0;
}

static int cmdline_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cmdline_proc_show, NULL);
}

static const struct file_operations cmdline_proc_fops = {
	.open		= cmdline_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static char *padding = " ";

static void replace_flag(char *cmd, const char *flag, const char *flag_new)
{
	char *start_addr, *end_addr;

	/* Ensure all instances of a flag are replaced */
	while ((start_addr = strstr(cmd, flag))) {
		end_addr = strchr(start_addr, ' ');
		if (end_addr) {
			if (strlen(flag) < strlen(flag_new)) {
				int length_to_copy = strlen(start_addr + (strlen(flag))) + 1;
				int length_diff = strlen(flag_new)-strlen(flag);

				memcpy(start_addr + (strlen(flag) + length_diff), start_addr + (strlen(flag)), length_to_copy);
				memcpy(start_addr + (strlen(flag)), padding, length_diff);
			}
			memcpy(start_addr, flag_new, strlen(flag_new));
		} else {
			*(start_addr - 1) = '\0';
		}
	}
}

static void replace_cmdline_flags(char *cmd)
{
	/* 
	 * WARNING
	 * Be aware that you can't replace shorter string 
	 * with longer ones in the function called here...
	 */
	replace_flag(cmd, "sched_enable_hmp=1", "sched_enable_hmp=0");
}

static int __init proc_cmdline_init(void)
{
	strcpy(new_command_line, saved_command_line);

	/*
	 * Patch various flags from command line seen 
	 * by userspace in order to pass custom cmdline.
	 */
	replace_cmdline_flags(new_command_line);

	proc_create("cmdline", 0, NULL, &cmdline_proc_fops);
	return 0;
}
fs_initcall(proc_cmdline_init);
