// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2022 Jonathan Senkerik
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/srandom.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "chacha.h"

/*
 * Set to 0 for Chacha8 mode,
 * set to 1 to enable Ultra High
 * Speed Mode (XorShift).
 */
#define ULTRA_HIGH_SPEED_MODE 1

/*
 * Dev name as it appears in /proc/devices
 */
#define SDEVICE_NAME "srandom"
#define APP_VERSION "2.0.0"

/*
 * Amount of time in seconds, worker thread
 * should sleep between each operation.
 * Recommended prime.
 */
#define PAID 0

/*
 * Size of Array.
 * Must be >= 65
 * (actual size used will be 65,
 * anything greater is thrown away).
 */
#define rndArraySize 67

/*
 * Number of 512b Array
 * (Must be power of 2).
 */
#define numberOfRndArrays 64

/*
 * Amount of time in seconds,
 * the background thread should sleep
 * between each operation.
 */
#define THREAD_SLEEP_VALUE 601

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#define COPY_TO_USER raw_copy_to_user
#define COPY_FROM_USER raw_copy_from_user
#else
#define COPY_TO_USER copy_to_user
#define COPY_FROM_USER copy_from_user
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
#define KTIME_GET_NS ktime_get_real_ts64
#define TIMESPEC timespec64
#else
#define KTIME_GET_NS getnstimeofday
#define TIMESPEC timespec
#endif

/*
 * Prototypes
 */
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static uint64_t wyhash64(void);
static uint64_t wyhash64_2(void);
static uint64_t xoroshiro256(void);
static inline uint64_t rotl(uint64_t, int);

static void update_sarray(int);
static uint8_t get_next_buffer(void);
static int proc_read(struct seq_file *m, void *v);
static int proc_open(struct inode *inode, struct file *file);
static void shuffle_sarray(int);
static uint64_t swapInt64(uint64_t);
static uint64_t reverseInt64(uint64_t);
static int work_thread(void *data);

/*
 * Global variables are declared as static, so are global within the file.
 */
const struct file_operations sfops = {
        .owner   = THIS_MODULE,
        .open    = device_open,
        .read    = sdevice_read,
        .write   = sdevice_write,
        .release = device_release
};

static struct miscdevice srandom_dev = {
        MISC_DYNAMIC_MINOR,
        "srandom",
        &sfops
};

static const struct file_operations proc_fops = {
        .owner   = THIS_MODULE,
        .read    = seq_read,
        .open    = proc_open,
        .llseek  = seq_lseek,
        .release = single_release,
};

static struct mutex UpArr_mutex;
static struct mutex Open_mutex;
static struct mutex ArrBusy_mutex;
static struct chacha_context ctx;
static struct task_struct *kthread;

#if !ULTRA_HIGH_SPEED_MODE
static struct task_struct *kthread;
#endif

/* Global variables */
uint64_t wyhash64_x;                            /* x for wyhash64 */
uint64_t wyhash64_x2;                           /* x for wyhash64 in-module use only */
uint64_t xoroshiro_s[4];                        /* s for xoroshiro256** */
uint64_t (*prngArrays)[rndArraySize];           /* Array of Array of SECURE RND numbers */
uint8_t chacha_key[32];
uint8_t chacha_nonce[12];
uint64_t chacha_counter = 0;
int8_t ArraysBusyFlags[rndArraySize];           /* Binary Flags for Busy Arrays */

/* Global counters */
int16_t sdevOpenCurrent;                        /* srandom device current open count */
int32_t sdevOpenTotal;                          /* srandom device total open count */
uint64_t generatedCount;                        /* Total generated (512 bytes) */

/*
 * Called when the module is loaded
 */
int mod_init(void)
{
        int16_t C, buffer_id;

        sdevOpenCurrent = 0;
        sdevOpenTotal = 0;
        generatedCount = 0;

        mutex_init(&UpArr_mutex);
        mutex_init(&Open_mutex);
        mutex_init(&ArrBusy_mutex);

        /* Register char device */
        if (misc_register(&srandom_dev))
                pr_debug("/dev/srandom registration failed..\n");
        else
                pr_debug("/dev/srandom registered..\n");

        /* Create /proc/srandom */
        if (!proc_create("srandom", 0, NULL, &proc_fops))
                pr_debug("/proc/srandom registration failed..\n");
        else
                pr_debug("/proc/srandom registration registered..\n");

#if ULTRA_HIGH_SPEED_MODE
        pr_debug("Module version: "APP_VERSION" UHS Mode\n");
#else
        pr_debug("Module version: "APP_VERSION"\n");
#endif

        prngArrays = kzalloc((numberOfRndArrays + 1) * rndArraySize * sizeof(uint64_t), GFP_KERNEL);
        while (!prngArrays) {
                pr_debug("kzalloc failed to allocate initial memory. retrying...\n");
                prngArrays = kzalloc((numberOfRndArrays + 1) * rndArraySize * sizeof(uint64_t), GFP_KERNEL);
        }

        /* Seed everything */
        get_random_bytes(&wyhash64_x, sizeof(uint64_t));
        get_random_bytes(&wyhash64_x2, sizeof(uint64_t));
        get_random_bytes(&xoroshiro_s[0], sizeof(uint64_t));
        get_random_bytes(&xoroshiro_s[1], sizeof(uint64_t));
        get_random_bytes(&xoroshiro_s[2], sizeof(uint64_t));
        get_random_bytes(&xoroshiro_s[3], sizeof(uint64_t));

        chacha_init_context(&ctx, chacha_key, chacha_nonce, chacha_counter);

        /* Init the sarray */
        for (buffer_id = 0; buffer_id <= numberOfRndArrays; buffer_id++) {
                for (C = 0; rndArraySize >= C; C++)
                        prngArrays[buffer_id][C] = wyhash64() ^ xoroshiro256();
                update_sarray(buffer_id);
        }

        kthread = kthread_create(work_thread, NULL, "srandom-kthread");
        wake_up_process(kthread);

        return 0;
}

/*
 * Called when the module is unloaded
 */
void mod_exit(void)
{
        kthread_stop(kthread);
        misc_deregister(&srandom_dev);
        remove_proc_entry("srandom", NULL);
        pr_debug("srandom deregistered..\n");
}

/*
 * Called when a process tries to open the device file.
 * "dd if=/dev/srandom"
 */
static int device_open(struct inode *inode, struct file *file)
{
        while (mutex_lock_interruptible(&Open_mutex));

        sdevOpenCurrent++;
        sdevOpenTotal++;
        mutex_unlock(&Open_mutex);

        pr_debug("(current open) :%d\n", sdevOpenCurrent);
        pr_debug("(total open)   :%d\n", sdevOpenTotal);

        return 0;
}

/*
 * Called when a process closes the device file
 */
static int device_release(struct inode *inode, struct file *file)
{
        while (mutex_lock_interruptible(&Open_mutex));

        sdevOpenCurrent--;
        mutex_unlock(&Open_mutex);

        pr_debug("(current open) :%d\n", sdevOpenCurrent);

        return 0;
}

/*
 * Called when a process reads from the device
 */
ssize_t sdevice_read(struct file *file, char *buf, size_t requestedCount,
                     loff_t *ppos)
{
        int Block, ret;
        uint8_t buffer_id;
        /* Buffer to hold numbers to send */
        char *new_buf;
        bool isVMalloc = 0;

        pr_debug("requestedCount:%zu\n", requestedCount);

        new_buf = kzalloc((requestedCount + 512) * sizeof(uint8_t), GFP_KERNEL|__GFP_NOWARN);
        while (!new_buf) {
                pr_debug("Using vmalloc to allocate large blocksize.\n");

                isVMalloc = 1;
                new_buf = vmalloc((requestedCount + 512) * sizeof(uint8_t));
        }

        for (Block = 0; Block <= (requestedCount / 512); Block++) {
                buffer_id = get_next_buffer();
                generatedCount++;

                /* Fill new_buf from a prngArrays block until requestedCount is met */
                pr_debug("Block:%u buffer_id:%d\n", Block, buffer_id);

                memcpy(new_buf + (Block * 512), prngArrays[buffer_id], 512);
#if ULTRA_HIGH_SPEED_MODE
                /* UHS mode will update the prngArrays block with new values for next request */
                update_sarray(buffer_id);
#endif

                /* Clear ArraysBusyFlags */
                if (mutex_lock_interruptible(&ArrBusy_mutex))
                        return -ERESTARTSYS;
                ArraysBusyFlags[buffer_id] = 0;
                mutex_unlock(&ArrBusy_mutex);
        }

        /*  Use Chacha to cipher new_buf */
#if !ULTRA_HIGH_SPEED_MODE
        chacha_xor(&ctx, new_buf, requestedCount);
        chacha_counter += requestedCount;
#endif

        /* Send new_buf to device */
        ret = COPY_TO_USER(buf, new_buf, requestedCount);

        /* Free allocated memory */
        if (isVMalloc)
                vfree(new_buf);
        else
                kfree(new_buf);

        /* Return the number of chars we sent */
        return requestedCount;
}
EXPORT_SYMBOL(sdevice_read);

/*
 * Called when someone tries to write to /dev/srandom device
 */
ssize_t sdevice_write(struct file *file, const char __user *buf,
                      size_t receivedCount, loff_t *ppos)
{
        char *newdata;
        int result;

        pr_debug("receivedCount:%zu\n", receivedCount);

        /* Allocate memory to read from device */
        newdata = kzalloc(receivedCount, GFP_KERNEL);
        while (!newdata)
                newdata = kzalloc(receivedCount, GFP_KERNEL);

        result = COPY_FROM_USER(newdata, buf, receivedCount);

        /* Free memory */
        kfree(newdata);

        pr_debug("sdevice_write COPY_FROM_USER receivedCount:%zu\n", receivedCount);

        return receivedCount;
}
EXPORT_SYMBOL(sdevice_write);

/*
 * Get the next available buffer
 */
uint8_t get_next_buffer(void)
{
        uint8_t next;

        next = (uint8_t)wyhash64_2() >> 2;
        while (mutex_lock_interruptible(&ArrBusy_mutex));
        while (ArraysBusyFlags[next] != 0) {
                next += 1;
                if (next >= numberOfRndArrays) {
                        next = 0;
                }
        }

        ArraysBusyFlags[next] = 1;
        mutex_unlock(&ArrBusy_mutex);

        return next;
}

void update_sarray(int buffer_id)
{
        int16_t C;
        int64_t X[2], Z[2], temp;
        int8_t mixer;

        mixer = (uint8_t)wyhash64_2();
        if ((mixer & 1) == 1)
                Z[0] = wyhash64();
        else
                Z[0] = xoroshiro256();

        if ((mixer & 2) == 2)
                Z[1] = wyhash64();
        else
                Z[1] = xoroshiro256();

        /* This must run exclusivly */
        while (mutex_lock_interruptible(&UpArr_mutex));

        for (C = 0; C < (rndArraySize - 4); C = C + 4) {
                mixer = (uint8_t)wyhash64_2();
                X[0]  = wyhash64();
                X[1]  = wyhash64();
                temp                              = prngArrays[buffer_id][C];
                prngArrays[buffer_id][C]     = prngArrays[buffer_id][C + 1] ^ X[(mixer & 1) == 1] ^ Z[(mixer & 16) == 16];
                prngArrays[buffer_id][C + 1] = prngArrays[buffer_id][C + 2] ^ X[(mixer & 2) == 2] ^ Z[(mixer & 32) == 32];
                prngArrays[buffer_id][C + 2] = prngArrays[buffer_id][C + 3] ^ X[(mixer & 4) == 4] ^ Z[(mixer & 64) == 64];
                prngArrays[buffer_id][C + 3] = temp ^ X[(mixer & 8) == 8] ^ Z[(mixer & 128) == 128];
        }

        shuffle_sarray(buffer_id);

        mutex_unlock(&UpArr_mutex);

        pr_debug("update_sarray buffer_id:%d, X:%llu, Z:%llu", buffer_id, X, Z);
}

/*
 * Shuffle the sarray
 */
inline void shuffle_sarray(int buffer_id)
{
        uint64_t temp;
        uint16_t mixer = (uint16_t)wyhash64_2();
        uint8_t mixtype = (mixer & 448) >> 7;
        uint8_t istart = (mixer & 56) >> 4;
        uint8_t increment = (mixer & 3) + 1;
        int i;

        pr_debug("shuffle_sarray istart: %d, increment: %d, buffer_id:%d, first:%llu, last:%llu\n", istart, increment, buffer_id, prngArrays[buffer_id][0], prngArrays[buffer_id][rndArraySize - 1]);

        for (i = istart; i < rndArraySize / 2; i = i + increment) {
                if (mixtype == 0) {
                        temp = prngArrays[buffer_id][i];
                        if ((mixer & 64) == 64)
                                prngArrays[buffer_id][i] = swapInt64(prngArrays[buffer_id][rndArraySize - i - 1]);
                        else
                                prngArrays[buffer_id][i] = prngArrays[buffer_id][rndArraySize - i - 1];

                        if ((mixer & 128) == 128)
                                prngArrays[buffer_id][rndArraySize - i - 1] = temp;
                        else
                                prngArrays[buffer_id][rndArraySize - i - 1] = reverseInt64(temp);
                } else if (mixtype == 1) {
                        prngArrays[buffer_id][i] = ((prngArrays[buffer_id][i] & 0xFFFFFFFF00000000ULL) >> 32) | ((prngArrays[buffer_id][i] & 0x00000000FFFFFFFFULL) << 32);
                        prngArrays[buffer_id][rndArraySize - i - 1] = ((prngArrays[buffer_id][rndArraySize - i - 1] & 0xFFFFFFFF00000000ULL) >> 32) | ((prngArrays[buffer_id][rndArraySize - i - 1] & 0x00000000FFFFFFFFULL) << 32);

                } else if (mixtype == 2) {
                        prngArrays[buffer_id][i] = ((prngArrays[buffer_id][i] & 0xFFFF0000FFFF0000ULL) >> 16) | ((prngArrays[buffer_id][i] & 0x0000FFFF0000FFFFULL) << 16);
                        prngArrays[buffer_id][rndArraySize - i - 1] = ((prngArrays[buffer_id][rndArraySize - i - 1] & 0xFFFF0000FFFF0000ULL) >> 16) | ((prngArrays[buffer_id][rndArraySize - i - 1] & 0x0000FFFF0000FFFFULL) << 16);

                } else if (mixtype == 3) {
                        prngArrays[buffer_id][i] = ((prngArrays[buffer_id][i] & 0xFF00FF00FF00FF00ULL) >> 8) | ((prngArrays[buffer_id][i] & 0x00FF00FF00FF00FFULL) << 8);
                        prngArrays[buffer_id][rndArraySize - i - 1] = ((prngArrays[buffer_id][rndArraySize - i - 1] & 0xFF00FF00FF00FF00ULL) >> 8) | ((prngArrays[buffer_id][rndArraySize - i - 1] & 0x00FF00FF00FF00FFULL) << 8);;

                }
        }
}

/*
 * PRNG functions
 */
uint64_t wyhash64(void)
{
        __uint128_t tmp;
        uint64_t m1;
        uint64_t m2;

        wyhash64_x += 0x60bee2bee120fc15;

        tmp = (__uint128_t) wyhash64_x * 0xa3b195354a39b70d;
        m1 = (tmp >> 64) ^ tmp;
        tmp = (__uint128_t)m1 * 0x1b03738712fad5c9;
        m2 = (tmp >> 64) ^ tmp;

        return m2;
}

uint64_t wyhash64_2(void)
{
        __uint128_t tmp;
        uint64_t m1;
        uint64_t m2;

        wyhash64_x2 += 0x60bee2bee120fc15;

        tmp = (__uint128_t) wyhash64_x2 * 0xa3b195354a39b70d;
        m1 = (tmp >> 64) ^ tmp;
        tmp = (__uint128_t)m1 * 0x1b03738712fad5c9;
        m2 = (tmp >> 64) ^ tmp;

        return m2;
}

uint64_t xoroshiro256(void)
{
        const uint64_t result = rotl(xoroshiro_s[1] * 5, 7) * 9;
        const uint64_t t = xoroshiro_s[1] << 17;

        xoroshiro_s[2] ^= xoroshiro_s[0];
        xoroshiro_s[3] ^= xoroshiro_s[1];
        xoroshiro_s[1] ^= xoroshiro_s[2];
        xoroshiro_s[0] ^= xoroshiro_s[3];

        xoroshiro_s[2] ^= t;

        xoroshiro_s[3] = rotl(xoroshiro_s[3], 45);

        return result;
}

inline uint64_t rotl(const uint64_t x, int k)
{
        return (x << k) | (x >> (64 - k));
}

/*
 * Swap a 64-bit integer
 */
#define SWAPINT64(x) ( \
   (((uint64_t)(x) & 0x00000000000000FFULL) << 56) | \
   (((uint64_t)(x) & 0x000000000000FF00ULL) << 40) | \
   (((uint64_t)(x) & 0x0000000000FF0000ULL) << 24) | \
   (((uint64_t)(x) & 0x00000000FF000000ULL) << 8) | \
   (((uint64_t)(x) & 0x000000FF00000000ULL) >> 8) | \
   (((uint64_t)(x) & 0x0000FF0000000000ULL) >> 24) | \
   (((uint64_t)(x) & 0x00FF000000000000ULL) >> 40) | \
   (((uint64_t)(x) & 0xFF00000000000000ULL) >> 56))

inline uint64_t swapInt64(uint64_t x)
{
    return SWAPINT64(x);
}

inline uint64_t reverseInt64(uint64_t value)
{
    value = ((value & 0xFFFFFFFF00000000ULL) >> 32) | ((value & 0x00000000FFFFFFFFULL) << 32);
    value = ((value & 0xFFFF0000FFFF0000ULL) >> 16) | ((value & 0x0000FFFF0000FFFFULL) << 16);
    value = ((value & 0xFF00FF00FF00FF00ULL) >> 8) | ((value & 0x00FF00FF00FF00FFULL) << 8);
    value = ((value & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((value & 0x0F0F0F0F0F0F0F0FULL) << 4);
    value = ((value & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((value & 0x3333333333333333ULL) << 2);
    value = ((value & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((value & 0x5555555555555555ULL) << 1);

    return value;
}

/*
 *  The Kernel thread refreshing the arrays.
 */
int work_thread(void *data)
{
        int buffer_id = 0;

        while (!kthread_should_stop()) {
                msleep_interruptible(THREAD_SLEEP_VALUE * 1000);

                buffer_id ++;
                if (buffer_id == numberOfRndArrays)
                        buffer_id = 0;

                update_sarray(buffer_id);

                pr_debug("work_thread buffer_id:%d\n", buffer_id);

        }

        return 0;
 }

/*
 * Called when reading /proc filesystem
 */
int proc_read(struct seq_file *m, void *v)
{
        seq_puts(m, "---------------------------:---------------------------\n");
        seq_puts(m, "Device                     : /dev/"SDEVICE_NAME"\n");
        seq_puts(m, "Module version             : "APP_VERSION"\n");
        seq_printf(m, "Current open count       : %d\n", sdevOpenCurrent);
        seq_printf(m, "Total open count         : %d\n", sdevOpenTotal);
        seq_printf(m, "Total K bytes            : %llu\n", generatedCount / 2);
        if (PAID == 0) {
                seq_puts(m, "-----------------------:-----------------------\n");
                seq_puts(m, "Please support my work and efforts contributing\n");
                seq_puts(m, "to the Linux community. A $25 payment per\n");
                seq_puts(m, "server would be highly appreciated.\n");
        }
        seq_puts(m, "-------------------:-------------------\n");
        seq_puts(m, "Author             : Jonathan Senkerik\n");
        seq_puts(m, "Website            : https://www.jintegrate.co\n");
        seq_puts(m, "GitHub             : https://github.com/josenk/srandom\n");
        if (PAID == 0) {
                seq_puts(m, "Paypal             : josenk@jintegrate.co\n");
                seq_puts(m, "Bitcoin            : 1GEtkAm97DphwJbJTPyywv6NbqJKLMtDzA\n");
                seq_puts(m, "Commercial Invoice : Avail on request.\n");
        }
        return 0;
}

int proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, proc_read, NULL);
}

/*
 *  ChaCha
 *  Adapted from: https://github.com/Ginurx/chacha20-c
 */
static uint32_t rotl32(uint32_t x, int n) 
{
        return (x << n) | (x >> (32 - n));
}

static uint32_t pack4(const uint8_t *a)
{
        uint32_t res = 0;

        res |= (uint32_t)a[0] << 0 * 8;
        res |= (uint32_t)a[1] << 1 * 8;
        res |= (uint32_t)a[2] << 2 * 8;
        res |= (uint32_t)a[3] << 3 * 8;
        return res;
}

static void chacha_init_block(struct chacha_context *ctx, uint8_t key[], uint8_t nonce[])
{
        const uint8_t *magic_constant = (uint8_t*)"expand 32-byte k";

        memcpy(ctx->key, key, sizeof(ctx->key));
        memcpy(ctx->nonce, nonce, sizeof(ctx->nonce));

        ctx->state[0] = pack4(magic_constant + 0 * 4);
        ctx->state[1] = pack4(magic_constant + 1 * 4);
        ctx->state[2] = pack4(magic_constant + 2 * 4);
        ctx->state[3] = pack4(magic_constant + 3 * 4);
        ctx->state[4] = pack4(key + 0 * 4);
        ctx->state[5] = pack4(key + 1 * 4);
        ctx->state[6] = pack4(key + 2 * 4);
        ctx->state[7] = pack4(key + 3 * 4);
        ctx->state[8] = pack4(key + 4 * 4);
        ctx->state[9] = pack4(key + 5 * 4);
        ctx->state[10] = pack4(key + 6 * 4);
        ctx->state[11] = pack4(key + 7 * 4);
        /* 64 bit counter initialized to zero by default */
        ctx->state[12] = 0;
        ctx->state[13] = pack4(nonce + 0 * 4);
        ctx->state[14] = pack4(nonce + 1 * 4);
        ctx->state[15] = pack4(nonce + 2 * 4);

        memcpy(ctx->nonce, nonce, sizeof(ctx->nonce));
}

static void chacha_block_set_counter(struct chacha_context *ctx, uint64_t counter)
{
        ctx->state[12] = (uint32_t)counter;
        ctx->state[13] = pack4(ctx->nonce + 0 * 4) + (uint32_t)(counter >> 32);
}

static void chacha_block_next(struct chacha_context *ctx)
{
        uint32_t *counter = ctx->state + 12;
        int i;

        /*
         * This is where the crazy voodoo magic happens.
         * Mix the bytes a lot and hope that nobody finds
         * out how to undo it.
         */
        for (i = 0; i < 16; i++)
                ctx->keystream32[i] = ctx->state[i];

#define CHACHA_QUARTERROUND(x, a, b, c, d) \
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16); \
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12); \
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8); \
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);

        for (i = 0; i < 4; i++) {
                CHACHA_QUARTERROUND(ctx->keystream32, 0, 4, 8, 12)
                CHACHA_QUARTERROUND(ctx->keystream32, 1, 5, 9, 13)
                CHACHA_QUARTERROUND(ctx->keystream32, 2, 6, 10, 14)
                CHACHA_QUARTERROUND(ctx->keystream32, 3, 7, 11, 15)
                CHACHA_QUARTERROUND(ctx->keystream32, 0, 5, 10, 15)
                CHACHA_QUARTERROUND(ctx->keystream32, 1, 6, 11, 12)
                CHACHA_QUARTERROUND(ctx->keystream32, 2, 7, 8, 13)
                CHACHA_QUARTERROUND(ctx->keystream32, 3, 4, 9, 14)
        }

        for (i = 0; i < 16; i++) ctx->keystream32[i] += ctx->state[i];

        /* Increment counter */
        counter[0]++;
        if (0 == counter[0]) {
                /* Wrap around occured, increment higher 32 bits of counter */
                counter[1]++;
                /*
                 * Limited to 2^64 blocks of 64 bytes each.
                 * If you want to process more than
                 * 1180591620717411303424 bytes (1.6 PB),
                 * you have other problems.
                 * We could keep counting with counter[2] and
                 * counter[3] (nonce), but then we risk reusing
                 * the nonce which is very bad.
                 */
                //assert(0 != counter[1]);
        }
}

void chacha_init_context(struct chacha_context *ctx, uint8_t key[],
        uint8_t nonce[], uint64_t counter)
{
        memset(ctx, 0, sizeof(struct chacha_context));

        chacha_init_block(ctx, key, nonce);
        chacha_block_set_counter(ctx, counter);

        ctx->counter = counter;
        ctx->position = 64;
}

void chacha_xor(struct chacha_context *ctx, uint8_t *bytes, size_t n_bytes)
{
        uint8_t *keystream8 = (uint8_t*)ctx->keystream32;
        size_t i;

        for (i = 0; i < n_bytes; i++)  {
                if (ctx->position >= 64) {
                        chacha_block_next(ctx);
                        ctx->position = 0;
                }
                bytes[i] ^= keystream8[ctx->position];
                ctx->position++;
        }
}

module_init(mod_init);
module_exit(mod_exit);

/*
 * Module license information
 */
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jonathan Senkerik <josenk@jintegrate.co>");
MODULE_DESCRIPTION("Improved random number generator.");
