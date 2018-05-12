/*
 * Copyright (C) 2004 IBM Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Device driver for TCG/TCPA TPM (trusted platform module).
 * Specifications at www.trustedcomputinggroup.org	 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 * 
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/tpm.h>

enum tpm_const {
	TPM_MINOR = 224,	/* officially assigned */
	TPM_BUFSIZE = 4096,
	TPM_NUM_DEVICES = 256,
	TPM_RETRY = 50,		/* 5 seconds */
};

enum tpm_timeout {
	TPM_TIMEOUT = 5,	/* msecs */
	TPM_TIMEOUT_RETRY = 100 /* msecs */
};

/* TPM addresses */
enum tpm_addr {
	TPM_SUPERIO_ADDR = 0x2E,
	TPM_ADDR = 0x4E,
};

#define TPM_WARN_RETRY          0x800
#define TPM_WARN_DOING_SELFTEST 0x802
#define TPM_ERR_DEACTIVATED     0x6
#define TPM_ERR_DISABLED        0x7
#define TPM_ERR_INVALID_POSTINIT 38

#define TPM_HEADER_SIZE		10
extern ssize_t tpm_show_pubek(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_pcrs(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_caps(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_caps_1_2(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_store_cancel(struct device *, struct device_attribute *attr,
				const char *, size_t);
extern ssize_t tpm_show_enabled(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_active(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_owned(struct device *, struct device_attribute *attr,
				char *);
extern ssize_t tpm_show_temp_deactivated(struct device *,
					 struct device_attribute *attr, char *);
extern ssize_t tpm_show_durations(struct device *,
				  struct device_attribute *attr, char *);
extern ssize_t tpm_show_timeouts(struct device *,
				 struct device_attribute *attr, char *);
extern ssize_t tpm2_show_ownerauth(struct device *, struct device_attribute *attr,
                char *);
extern ssize_t tpm2_show_endorseauth(struct device *, struct device_attribute *attr,
                char *);
extern ssize_t tpm2_show_phenable(struct device *, struct device_attribute *attr,
                char *);
extern ssize_t tpm2_show_shenable(struct device *, struct device_attribute *attr,
                char *);
extern ssize_t tpm2_show_ehenable(struct device *, struct device_attribute *attr,
                char *);
extern ssize_t tpm2_show_pcrs(struct device *, struct device_attribute *attr,
                char *);

struct tpm_chip;

struct tpm_vendor_specific {
	const u8 req_complete_mask;
	const u8 req_complete_val;
	bool (*req_canceled)(struct tpm_chip *chip, u8 status);
	void __iomem *iobase;		/* ioremapped address */
	unsigned long base;		/* TPM base address */

	int irq;
	int probed_irq;

	int region_size;
	int have_region;

	int (*recv) (struct tpm_chip *, u8 *, size_t);
	int (*send) (struct tpm_chip *, u8 *, size_t);
	void (*cancel) (struct tpm_chip *);
	u8 (*status) (struct tpm_chip *);
	void (*release) (struct device *);
	struct miscdevice miscdev;
	struct attribute_group *attr_group;
	struct list_head list;
	int locality;
	unsigned long timeout_a, timeout_b, timeout_c, timeout_d; /* jiffies */
	bool timeout_adjusted;
	unsigned long duration[3]; /* jiffies */
	bool duration_adjusted;
	void *priv;

	wait_queue_head_t read_queue;
	wait_queue_head_t int_queue;

	u16 manufacturer_id;
};

#define TPM_VPRIV(c)	(c)->vendor.priv

#define TPM_VID_INTEL    0x8086
#define TPM_VID_WINBOND  0x1050
#define TPM_VID_STM      0x104A

struct tpm_chip {
	struct device *dev;	/* Device stuff */

	int dev_num;		/* /dev/tpm# */
	unsigned long is_open;	/* only one allowed */
	int time_expired;

	/* Data passed to and from the tpm via the read/write calls */
	u8 *data_buffer;
	atomic_t data_pending;
	struct mutex buffer_mutex;

	struct timer_list user_read_timer;	/* user needs to claim result */
	struct work_struct work;
	struct mutex tpm_mutex;	/* tpm is processing */

	struct tpm_vendor_specific vendor;

	struct dentry **bios_dir;

	struct list_head list;
	void (*release) (struct device *);
};

#define to_tpm_chip(n) container_of(n, struct tpm_chip, vendor)

static inline void tpm_chip_put(struct tpm_chip *chip)
{
	module_put(chip->dev->driver->owner);
}

static inline int tpm_read_index(int base, int index)
{
	outb(index, base);
	return inb(base+1) & 0xFF;
}

static inline void tpm_write_index(int base, int index, int value)
{
	outb(index, base);
	outb(value & 0xFF, base+1);
}
struct tpm_input_header {
	__be16	tag;
	__be32	length;
	__be32	ordinal;
} __packed;

struct tpm_output_header {
	__be16	tag;
	__be32	length;
	__be32	return_code;
} __packed;

struct	stclear_flags_t {
	__be16	tag;
	u8	deactivated;
	u8	disableForceClear;
	u8	physicalPresence;
	u8	physicalPresenceLock;
	u8	bGlobalLock;
} __packed;

struct	tpm_version_t {
	u8	Major;
	u8	Minor;
	u8	revMajor;
	u8	revMinor;
} __packed;

struct	tpm_version_1_2_t {
	__be16	tag;
	u8	Major;
	u8	Minor;
	u8	revMajor;
	u8	revMinor;
} __packed;

struct	timeout_t {
	__be32	a;
	__be32	b;
	__be32	c;
	__be32	d;
} __packed;

struct duration_t {
	__be32	tpm_short;
	__be32	tpm_medium;
	__be32	tpm_long;
} __packed;

struct permanent_flags_t {
	__be16	tag;
	u8	disable;
	u8	ownership;
	u8	deactivated;
	u8	readPubek;
	u8	disableOwnerClear;
	u8	allowMaintenance;
	u8	physicalPresenceLifetimeLock;
	u8	physicalPresenceHWEnable;
	u8	physicalPresenceCMDEnable;
	u8	CEKPUsed;
	u8	TPMpost;
	u8	TPMpostLock;
	u8	FIPS;
	u8	operator;
	u8	enableRevokeEK;
	u8	nvLocked;
	u8	readSRKPub;
	u8	tpmEstablished;
	u8	maintenanceDone;
	u8	disableFullDALogicInfo;
} __packed;

typedef union {
	struct	permanent_flags_t perm_flags;
	struct	stclear_flags_t	stclear_flags;
	bool	owned;
	__be32	num_pcrs;
	struct	tpm_version_t	tpm_version;
	struct	tpm_version_1_2_t tpm_version_1_2;
	__be32	manufacturer_id;
	struct timeout_t  timeout;
	struct duration_t duration;
} cap_t;

struct	tpm_getcap_params_in {
	__be32	cap;
	__be32	subcap_size;
	__be32	subcap;
} __packed;

struct	tpm_getcap_params_out {
	__be32	cap_size;
	cap_t	cap;
} __packed;

struct	tpm_readpubek_params_out {
	u8	algorithm[4];
	u8	encscheme[2];
	u8	sigscheme[2];
	__be32	paramsize;
	u8	parameters[12]; /*assuming RSA*/
	__be32	keysize;
	u8	modulus[256];
	u8	checksum[20];
} __packed;

typedef union {
	struct	tpm_input_header in;
	struct	tpm_output_header out;
} tpm_cmd_header;

#define TPM_DIGEST_SIZE 20
struct tpm_pcrread_out {
	u8	pcr_result[TPM_DIGEST_SIZE];
} __packed;

struct tpm_pcrread_in {
	__be32	pcr_idx;
} __packed;

struct tpm_pcrextend_in {
	__be32	pcr_idx;
	u8	hash[TPM_DIGEST_SIZE];
} __packed;

/* 128 bytes is an arbitrary cap. This could be as large as TPM_BUFSIZE - 18
 * bytes, but 128 is still a relatively large number of random bytes and
 * anything much bigger causes users of struct tpm_cmd_t to start getting
 * compiler warnings about stack frame size. */
#define TPM_MAX_RNG_DATA	128

struct tpm_getrandom_out {
	__be32 rng_data_len;
	u8     rng_data[TPM_MAX_RNG_DATA];
} __packed;

struct tpm_getrandom_in {
	__be32 num_bytes;
} __packed;

struct tpm_startup_in {
	__be16	startup_type;
} __packed;

struct tpm2_selftest_in {
    u8 fulltest_type;
} __packed;

#define    TPM_CAP_FIRST               cpu_to_be32(0x00000000)
#define    TPM_CAP_ALGS                cpu_to_be32(0x00000000)
#define    TPM_CAP_HANDLES             cpu_to_be32(0x00000001)
#define    TPM_CAP_COMMANDS            cpu_to_be32(0x00000002)
#define    TPM_CAP_PP_COMMANDS         cpu_to_be32(0x00000003)
#define    TPM_CAP_AUDIT_COMMANDS      cpu_to_be32(0x00000004)
#define    TPM_CAP_PCRS                cpu_to_be32(0x00000005)
#define    TPM_CAP_TPM_PROPERTIES      cpu_to_be32(0x00000006)
#define    TPM_CAP_PCR_PROPERTIES      cpu_to_be32(0x00000007)
#define    TPM_CAP_ECC_CURVES          cpu_to_be32(0x00000008)
#define    TPM_CAP_LAST                cpu_to_be32(0x00000008)
#define    TPM_CAP_VENDOR_PROPERTY     cpu_to_be32(0x00000100)
#define    MAX_CAP_BUFFER              (u32)(1000)
#define    MAX_TPM_PROPERTIES          (MAX_CAP_BUFFER/16)

#define    TPM_PT_NONE                 cpu_to_be32(0x00000000)
#define    PT_GROUP                    cpu_to_be32(0x00000100)
#define    PT_FIXED                    cpu_to_be32(PT_GROUP * 1)
#define    PT_VAR                      cpu_to_be32(PT_GROUP * 2)
#define    TPM_PT_PERMANENT            cpu_to_be32(PT_VAR + 0)
#define    TPM_PT_STARTUP_CLEAR        cpu_to_be32(PT_VAR + 1)
#define    TPM_PT_PCR_COUNT            cpu_to_be32(PT_FIXED + 18)
#define    PH_ENABLE_BIT               (u32)(0x1)
#define    SH_ENABLE_BIT               (u32)(0x2)
#define    EH_ENABLE_BIT               (u32)(0x4)
#define    OWNER_AUTH_BIT              (u32)(0x1)
#define    ENDORSEMENT_AUTH_BIT        (u32)(0x2)

struct tpms_gatged_prop {
    __be32  property;
    __be32  value;
} __packed;

struct tpml_tagged_prop {
    __be32                    count;
    struct tpms_gatged_prop   tpm2_property[MAX_TPM_PROPERTIES];
} __packed;

typedef union {
    struct tpml_tagged_prop  tpm2_properties;
} tpm2_cap_t;

struct tpm2_getcap_in {
    __be32 tpm_cap;
    __be32 property;
    __be32 property_count;
} __packed;

struct tpm2_getcap_out {
    u8         more_data;
    __be32     capability;
    tpm2_cap_t cap;
} __packed;

#define    IMPLEMENTATION_PCR           (u32)(24)
#define    PLATFORM_PCR                 (u32)(24)
#define    PCR_SELECT_MIN               (u32)(3)
#define    PCR_SELECT_MAX               (u32)(3)
#define    SHA1_DIGEST_SIZE             (u32)(20)
#define    SHA256_DIGEST_SIZE           (u32)(32)
#define    SHA384_DIGEST_SIZE           (u32)(48)
#define    SHA512_DIGEST_SIZE           (u32)(64)
#define    WHIRLPOOL512_DIGEST_SIZE     (u32)(64)
#define    SM3_256_DIGEST_SIZE          (u32)(32)
#define    DIGEST_SIZE_MAX              (u32)(64)
#define    HASH_COUNT                   (u32)(3)

struct digest_2b_t {
    __be16  size;
    u8  buffer[DIGEST_SIZE_MAX];
} __packed;

typedef union {
    struct digest_2b_t  sha1;
} tpm2b_digest_t;

struct tpml_digest_t {
    __be32  count;
    tpm2b_digest_t  digests[8];
} __packed;

struct tpms_pcr_selection_t {
    __be16  hash;
    u8  size_slct;
    u8  pcr_slct[PCR_SELECT_MAX];
} __packed;

struct tpml_pcr_selection_t {
    __be32  count;
    struct tpms_pcr_selection_t tpms_pcr_slct;
} __packed;

struct tpm2_pcr_in {
    struct tpml_pcr_selection_t tpml_pcr_slct;
}__packed;

struct tpm2_pcr_out {
    __be32  pcr_update_count;
    struct tpml_pcr_selection_t tpml_pcr_slct;
    struct tpml_digest_t pcr_values;
}__packed;

typedef union {
	struct	tpm_getcap_params_out getcap_out;
	struct	tpm_readpubek_params_out readpubek_out;
	u8	readpubek_out_buffer[sizeof(struct tpm_readpubek_params_out)];
	struct	tpm_getcap_params_in getcap_in;
	struct	tpm_pcrread_in	pcrread_in;
	struct	tpm_pcrread_out	pcrread_out;
	struct	tpm_pcrextend_in pcrextend_in;
	struct	tpm_getrandom_in getrandom_in;
	struct	tpm_getrandom_out getrandom_out;
	struct tpm_startup_in startup_in;
    struct  tpm2_selftest_in tpm2selftest_in;
    struct  tpm2_getcap_in tpm2getcap_in;
    struct  tpm2_getcap_out tpm2getcap_out;
    struct  tpm2_pcr_in tpm2pcr_in;
    struct  tpm2_pcr_out tpm2pcr_out;
} tpm_cmd_params;

struct tpm_cmd_t {
	tpm_cmd_header	header;
	tpm_cmd_params	params;
} __packed;

ssize_t	tpm_getcap(struct device *, __be32, cap_t *, const char *);

extern int tpm_get_timeouts(struct tpm_chip *);
extern void tpm_gen_interrupt(struct tpm_chip *);
extern int tpm_do_selftest(struct tpm_chip *);
extern unsigned long tpm_calc_ordinal_duration(struct tpm_chip *, u32);
extern struct tpm_chip* tpm_register_hardware(struct device *,
				 const struct tpm_vendor_specific *);
extern int tpm_open(struct inode *, struct file *);
extern int tpm_release(struct inode *, struct file *);
extern void tpm_dev_vendor_release(struct tpm_chip *);
extern ssize_t tpm_write(struct file *, const char __user *, size_t,
			 loff_t *);
extern ssize_t tpm_read(struct file *, char __user *, size_t, loff_t *);
extern void tpm_remove_hardware(struct device *);
extern int tpm_pm_suspend(struct device *);
extern int tpm_pm_resume(struct device *);
extern int wait_for_tpm_stat(struct tpm_chip *, u8, unsigned long,
			     wait_queue_head_t *, bool);
extern int tpm2_do_selftest(struct tpm_chip *);

#ifdef CONFIG_ACPI
extern int tpm_add_ppi(struct kobject *);
extern void tpm_remove_ppi(struct kobject *);
#else
static inline int tpm_add_ppi(struct kobject *parent)
{
	return 0;
}

static inline void tpm_remove_ppi(struct kobject *parent)
{
}
#endif