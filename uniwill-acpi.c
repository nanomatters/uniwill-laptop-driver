// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for Uniwill notebooks.
 *
 * Special thanks go to Pőcze Barnabás, Christoffer Sandberg and Werner Sembach
 * for supporting the development of this driver either through prior work or
 * by answering questions regarding the underlying ACPI and WMI interfaces.
 *
 * Copyright (C) 2025 Armin Wolf <W_Armin@gmx.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* Uncomment to enable VRM current limit override in overboost mode */
/* #define UNIWILL_ENABLE_VRM_OVERRIDE */

#include <linux/acpi.h>
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dmi.h>
#include <linux/efi.h>
#include <linux/errno.h>
#include <linux/fixp-arith.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/wmi.h>

#include <acpi/battery.h>

#include "uniwill-wmi.h"

#define EC_ADDR_BAT_POWER_UNIT_1	0x0400

#define EC_ADDR_BAT_POWER_UNIT_2	0x0401

#define EC_ADDR_BAT_DESIGN_CAPACITY_1	0x0402

#define EC_ADDR_BAT_DESIGN_CAPACITY_2	0x0403

#define EC_ADDR_BAT_FULL_CAPACITY_1	0x0404

#define EC_ADDR_BAT_FULL_CAPACITY_2	0x0405

#define EC_ADDR_BAT_DESIGN_VOLTAGE_1	0x0408

#define EC_ADDR_BAT_DESIGN_VOLTAGE_2	0x0409

#define EC_ADDR_BAT_STATUS_1		0x0432
#define BAT_DISCHARGING			BIT(0)

#define EC_ADDR_BAT_STATUS_2		0x0433

#define EC_ADDR_BAT_CURRENT_1		0x0434

#define EC_ADDR_BAT_CURRENT_2		0x0435

#define EC_ADDR_BAT_REMAIN_CAPACITY_1	0x0436

#define EC_ADDR_BAT_REMAIN_CAPACITY_2	0x0437

#define EC_ADDR_BAT_VOLTAGE_1		0x0438

#define EC_ADDR_BAT_VOLTAGE_2		0x0439

#define EC_ADDR_CPU_TEMP		0x043E

#define EC_ADDR_ADAPTER_CURRENT		0x0449

#define EC_ADDR_GPU_TEMP		0x044F

#define EC_ADDR_SYSTEM_ID		0x0456
#define HAS_GPU				BIT(7)

#define EC_ADDR_CPU_TEMP_LIMIT		0x0463
#define TEMP_LIMIT_TJMAX		105

#define EC_ADDR_MAIN_FAN_RPM_1		0x0464

#define EC_ADDR_MAIN_FAN_RPM_2		0x0465

#define EC_ADDR_SYSTEM_POWER_LO		0x060C
#define EC_ADDR_SYSTEM_POWER_HI		0x060D

#define EC_ADDR_SCREEN_STATUS		0x0466
#define SCREEN_SUSPENDED		BIT(6)

#define EC_ADDR_SECOND_FAN_RPM_1	0x046C

#define EC_ADDR_SECOND_FAN_RPM_2	0x046D

#define EC_ADDR_DEVICE_STATUS		0x047B
#define WIFI_STATUS_ON			BIT(7)
/* BIT(5) is also unset depending on the rfkill state (bluetooth?) */

#define EC_ADDR_BAT_ALERT		0x0494

#define EC_ADDR_BAT_CYCLE_COUNT_1	0x04A6

#define EC_ADDR_BAT_CYCLE_COUNT_2	0x04A7

#define EC_ADDR_OEM_9			0x0726
#define AC_AUTO_BOOT_ENABLE		BIT(3)

#define EC_ADDR_PROJECT_ID		0x0740
#define PROJECT_ID_NONE			0x00
#define PROJECT_ID_GI			0x01
#define PROJECT_ID_GJ			0x02
#define PROJECT_ID_GK			0x03
#define PROJECT_ID_GICN			0x04
#define PROJECT_ID_GJCN			0x05
#define PROJECT_ID_GK5CN_X		0x06
#define PROJECT_ID_GK7CN_S		0x07
#define PROJECT_ID_GK7CPCS_GK5CQ7Z	0x08
#define PROJECT_ID_PF			0x09
#define PROJECT_ID_GK5CP_4X_5X_6X	0x0A
#define PROJECT_ID_IDP			0x0B
#define PROJECT_ID_IDY_6Y		0x0C
#define PROJECT_ID_IDY_7Y		0x0D
#define PROJECT_ID_PF4MU_PF4MN_PF5MU	0x0E
#define PROJECT_ID_CML_GAMING		0x0F
#define PROJECT_ID_GK7NXXR		0x10
#define PROJECT_ID_GM5MU1Y		0x11
#define PROJECT_ID_PH4TRX1		0x12
#define PROJECT_ID_PH4TUX1		0x13
#define PROJECT_ID_PH4TQX1		0x14
#define PROJECT_ID_PH6TRX1		0x15
#define PROJECT_ID_PH6TQXX		0x16
#define PROJECT_ID_PHXAXXX		0x17
#define PROJECT_ID_PHXPXXX		0x18

#define EC_ADDR_CUSTOM_PROFILE		0x0727
#define CUSTOM_PROFILE_MODE		BIT(6)

#define EC_ADDR_GPU_MUX_MODE		0x072A
#define GPU_MUX_MODE_HYBRID		0x00
#define GPU_MUX_MODE_DGPU_DIRECT	0x01
#define GPU_MUX_MODE_IGPU_ONLY		0x02

/* OemMagicVariable UEFI variable field offset for display mode */
#define OEM_MAGIC_DISPLAY_MODE_OFFSET	0x62

/* Per-mode default PL values (set by firmware, read-only) */
#define EC_ADDR_PL1_DEFAULT_GAMING	0x0730
#define EC_ADDR_PL2_DEFAULT_GAMING	0x0731
#define EC_ADDR_PL4_DEFAULT_GAMING	0x0732
#define EC_ADDR_PL1_DEFAULT_OFFICE	0x0734
#define EC_ADDR_PL2_DEFAULT_OFFICE	0x0735
#define EC_ADDR_PL4_DEFAULT_OFFICE	0x0736

#define EC_ADDR_AP_OEM			0x0741
#define	ENABLE_MANUAL_CTRL		BIT(0)
#define ITE_KBD_EFFECT_REACTIVE		BIT(3)
#define FAN_ABNORMAL			BIT(5)

#define EC_ADDR_SUPPORT_5		0x0742
#define FAN_TURBO_SUPPORTED		BIT(4)
#define FAN_SUPPORT			BIT(5)

#define EC_ADDR_CTGP_DB_CTRL		0x0743
#define CTGP_DB_GENERAL_ENABLE		BIT(0)
#define CTGP_DB_DB_ENABLE		BIT(1)
#define CTGP_DB_CTGP_ENABLE		BIT(2)

#define EC_ADDR_CTGP_DB_CTGP_OFFSET	0x0744

#define EC_ADDR_CTGP_DB_TPP_OFFSET	0x0745

#define EC_ADDR_CTGP_DB_DB_OFFSET	0x0746

#define EC_ADDR_LIGHTBAR_AC_CTRL	0x0748
#define LIGHTBAR_APP_EXISTS		BIT(0)
#define LIGHTBAR_POWER_SAVE		BIT(1)
#define LIGHTBAR_S0_OFF			BIT(2)
#define LIGHTBAR_S3_OFF			BIT(3)	// Breathing animation when suspended
#define LIGHTBAR_WELCOME		BIT(7)	// Rainbow animation

#define EC_ADDR_LIGHTBAR_AC_RED		0x0749

#define EC_ADDR_LIGHTBAR_AC_GREEN	0x074A

#define EC_ADDR_LIGHTBAR_AC_BLUE	0x074B

#define EC_ADDR_BIOS_OEM		0x074E
#define FN_LOCK_STATUS			BIT(4)

#define EC_ADDR_MANUAL_FAN_CTRL		0x0751
#define FAN_LEVEL_MASK			GENMASK(2, 0)
#define FAN_MODE_TURBO			BIT(4)
#define FAN_MODE_HIGH			BIT(5)
#define FAN_MODE_BOOST			BIT(6)
#define FAN_MODE_USER			BIT(7)

#define PROFILE_MODE_MASK		(FAN_MODE_USER | FAN_MODE_HIGH | FAN_MODE_TURBO)
#define PROFILE_QUIET			(FAN_MODE_USER | FAN_MODE_HIGH)
#define PROFILE_BALANCED		0
#define PROFILE_PERFORMANCE		FAN_MODE_TURBO

#define EC_ADDR_VRM_CURRENT_LIMIT	0x0753

#define EC_ADDR_VRM_MAX_CURRENT_LIMIT	0x0754

#define EC_ADDR_PWM_1			0x075B

#define EC_ADDR_PWM_2			0x075C

/* Unreliable */
#define EC_ADDR_SUPPORT_1		0x0765
#define AIRPLANE_MODE			BIT(0)
#define GPS_SWITCH			BIT(1)
#define OVERCLOCK			BIT(2)
#define MACRO_KEY			BIT(3)
#define SHORTCUT_KEY			BIT(4)
#define SUPER_KEY_LOCK			BIT(5)
#define LIGHTBAR			BIT(6)
#define FAN_BOOST			BIT(7)

#define EC_ADDR_SUPPORT_2		0x0766
#define SILENT_MODE			BIT(0)
#define USB_CHARGING			BIT(1)
#define RGB_KEYBOARD			BIT(2)
#define CHINA_MODE			BIT(5)
#define MY_BATTERY			BIT(6)

#define EC_ADDR_TRIGGER			0x0767
#define TRIGGER_SUPER_KEY_LOCK		BIT(0)
#define TRIGGER_LIGHTBAR		BIT(1)
#define TRIGGER_FAN_BOOST		BIT(2)
#define TRIGGER_SILENT_MODE		BIT(3)
#define TRIGGER_USB_CHARGING		BIT(4)
#define RGB_APPLY_COLOR			BIT(5)
#define RGB_LOGO_EFFECT			BIT(6)
#define RGB_RAINBOW_EFFECT		BIT(7)

#define EC_ADDR_SWITCH_STATUS		0x0768
#define SUPER_KEY_LOCK_STATUS		BIT(0)
#define LIGHTBAR_STATUS			BIT(1)
#define FAN_BOOST_STATUS		BIT(2)
#define MACRO_KEY_STATUS		BIT(3)
#define MY_BAT_POWER_BAT_STATUS		BIT(4)

#define EC_ADDR_RGB_RED			0x0769

#define EC_ADDR_RGB_GREEN		0x076A

#define EC_ADDR_RGB_BLUE		0x076B

#define EC_ADDR_ROMID_START		0x0770
#define ROMID_LENGTH			14

#define EC_ADDR_ROMID_EXTRA_1		0x077E

#define EC_ADDR_ROMID_EXTRA_2		0x077F

#define EC_ADDR_BIOS_OEM_2		0x0782
#define FAN_V2_NEW			BIT(0)
#define FAN_QKEY			BIT(1)
#define FAN_TABLE_OFFICE_MODE		BIT(2)
#define FAN_V3				BIT(3)
#define DEFAULT_MODE			BIT(4)
#define ENABLE_CHINA_MODE		BIT(6)

#define EC_ADDR_PL1_SETTING		0x0783

#define EC_ADDR_PL2_SETTING		0x0784

#define EC_ADDR_PL4_SETTING		0x0785

#define EC_ADDR_TCC_OFFSET		0x0786
#define TCC_OFFSET_VALUE_MASK		GENMASK(6, 0)
#define TCC_OFFSET_ENABLE		BIT(7)
#define TCC_OFFSET_MAX			63

#define FAN_CURVE_LENGTH		5

#define EC_ADDR_FAN_SWITCH_SPEED	0x0787
#define FAN_SWITCH_SPEED_ENABLE		BIT(7)
#define FAN_SWITCH_SPEED_DELAY_MASK	GENMASK(6, 0)

#define EC_ADDR_THERMAL_BUDGET		0x0788

#define EC_ADDR_KBD_STATUS		0x078C
#define KBD_WHITE_ONLY			BIT(0)
#define KBD_POWER_OFF			BIT(1)
#define KBD_TURBO_LEVEL_MASK		GENMASK(3, 2)
#define KBD_APPLY			BIT(4)
#define KBD_BRIGHTNESS_MASK		GENMASK(7, 5)

#define EC_ADDR_FAN_CTRL		0x078E
#define FAN3P5				BIT(1)
#define CHARGING_PROFILE		BIT(3)
#define UNIVERSAL_FAN_CTRL		BIT(6)

#define EC_ADDR_BIOS_OEM_3		0x07A3
#define FAN_REDUCED_DURY_CYCLE		BIT(5)
#define FAN_ALWAYS_ON			BIT(6)

#define EC_ADDR_BIOS_BYTE		0x07A4
#define FN_LOCK_SWITCH			BIT(3)

#define EC_ADDR_OEM_3			0x07A5
#define POWER_LED_MASK			GENMASK(1, 0)
#define POWER_LED_LEFT			0x00
#define POWER_LED_BOTH			0x01
#define POWER_LED_NONE			0x02
#define FAN_QUIET			BIT(2)
#define OVERBOOST			BIT(4)
#define HIGH_POWER			BIT(7)

#define EC_ADDR_OEM_4			0x07A6
#define OVERBOOST_DYN_TEMP_OFF		BIT(1)
#define CHARGING_PROFILE_MASK		GENMASK(5, 4)
#define CHARGING_PROFILE_HIGH_CAPACITY	0x00
#define CHARGING_PROFILE_BALANCED	0x01
#define CHARGING_PROFILE_STATIONARY	0x02
#define TOUCHPAD_TOGGLE_OFF		BIT(6)

#define EC_ADDR_CHARGE_CTRL		0x07B9
#define CHARGE_CTRL_MASK		GENMASK(6, 0)
#define CHARGE_CTRL_REACHED		BIT(7)

#define EC_ADDR_CHARGE_CTRL_START	0x07D0
#define CHARGE_CTRL_START_MASK		GENMASK(6, 0)

#define EC_ADDR_UNIVERSAL_FAN_CTRL	0x07C5
#define SPLIT_TABLES			BIT(7)

#define EC_ADDR_AP_OEM_6		0x07C6
#define ENABLE_UNIVERSAL_FAN_CTRL	BIT(2)
#define BATTERY_CHARGE_FULL_OVER_24H	BIT(3)
#define BATTERY_ERM_STATUS_REACHED	BIT(4)

#define EC_ADDR_USB_C_POWER_PRIORITY	0x07CC
#define USB_C_POWER_PRIORITY		BIT(7)

#define EC_ADDR_GPU_POWER_ALLOC		0x07D5

/* Same bits as EC_ADDR_LIGHTBAR_AC_CTRL except LIGHTBAR_S3_OFF */
#define EC_ADDR_LIGHTBAR_BAT_CTRL	0x07E2

#define EC_ADDR_LIGHTBAR_BAT_RED	0x07E3

#define EC_ADDR_LIGHTBAR_BAT_GREEN	0x07E4

#define EC_ADDR_LIGHTBAR_BAT_BLUE	0x07E5

#define EC_ADDR_MINI_LED_SUPPORT	0x0D4F

#define EC_ADDR_CPU_TEMP_END_TABLE	0x0F00

#define EC_ADDR_CPU_TEMP_START_TABLE	0x0F10

#define EC_ADDR_CPU_FAN_SPEED_TABLE	0x0F20

#define EC_ADDR_GPU_TEMP_END_TABLE	0x0F30

#define EC_ADDR_GPU_TEMP_START_TABLE	0x0F40

#define EC_ADDR_GPU_FAN_SPEED_TABLE	0x0F50

/*
 * Those two registers technically allow for manual fan control,
 * but are unstable on some models and are likely not meant to
 * be used by applications as they are only accessible when using
 * the WMI interface.
 */
#define EC_ADDR_PWM_1_WRITEABLE		0x1804

#define EC_ADDR_PWM_2_WRITEABLE		0x1809

#define DRIVER_NAME	"uniwill"

/*
 * The OEM software always sleeps up to 6 ms after reading/writing EC
 * registers, so we emulate this behaviour for maximum compatibility.
 */
#define UNIWILL_EC_DELAY_US	6000

#define PWM_MAX			200
#define FAN_ON_MIN_SPEED_PERCENT	25
#define FAN_TABLE_LENGTH	16

#define LED_CHANNELS		3

#define KBD_LED_CHANNELS	3
#define KBD_LED_MAX_INTENSITY	50

#define UNIWILL_FEATURE_FN_LOCK			BIT(0)
#define UNIWILL_FEATURE_SUPER_KEY		BIT(1)
#define UNIWILL_FEATURE_TOUCHPAD_TOGGLE		BIT(2)
#define UNIWILL_FEATURE_LIGHTBAR		BIT(3)
#define UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT	BIT(4)
/* Mutually exclusive with the charge limit feature */
#define UNIWILL_FEATURE_BATTERY_CHARGE_MODES	BIT(5)
#define UNIWILL_FEATURE_CPU_TEMP		BIT(6)
#define UNIWILL_FEATURE_GPU_TEMP		BIT(7)
#define UNIWILL_FEATURE_PRIMARY_FAN		BIT(8)
#define UNIWILL_FEATURE_SECONDARY_FAN		BIT(9)
#define UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL	BIT(10)
#define UNIWILL_FEATURE_USB_C_POWER_PRIORITY	BIT(11)
#define UNIWILL_FEATURE_KEYBOARD_BACKLIGHT	BIT(12)
#define UNIWILL_FEATURE_AC_AUTO_BOOT		BIT(13)
#define UNIWILL_FEATURE_USB_POWERSHARE		BIT(14)
#define UNIWILL_FEATURE_CPU_TDP_CONTROL		BIT(15)
#define UNIWILL_FEATURE_WATER_COOLER		BIT(16)
#define UNIWILL_FEATURE_GPU_MUX			BIT(17)
#define UNIWILL_FEATURE_TCC_OFFSET		BIT(18)

#define WC_STALE_TIMEOUT_MS	5000

enum usb_c_power_priority_options {
	USB_C_POWER_PRIORITY_CHARGING = 0,
	USB_C_POWER_PRIORITY_PERFORMANCE,
};

struct uniwill_data {
	struct device *dev;
	acpi_handle handle;
	struct regmap *regmap;
	unsigned int features;
	u8 project_id;
	struct acpi_battery_hook hook;
	unsigned int last_charge_ctrl;
	bool has_charge_limit;
	bool has_charge_start_threshold;
	struct mutex battery_lock;	/* Protects the list of currently registered batteries */
	unsigned int last_status;
	unsigned int last_switch_status;
	unsigned int last_trigger;
	struct mutex super_key_lock;	/* Protects the toggling of the super key lock state */
	struct list_head batteries;
	struct mutex led_lock;		/* Protects writes to the lightbar registers */
	unsigned int lightbar_max_brightness;
	struct led_classdev_mc led_mc_cdev;
	struct mc_subled led_mc_subled_info[LED_CHANNELS];
	bool single_color_kbd;
	unsigned int kbd_led_max_brightness;
	unsigned int last_kbd_status;
	union {
		struct {
			/* Protects writes to the RGB keyboard backlight registers */
			struct mutex kbd_rgb_led_lock;
			struct led_classdev_mc kbd_led_mc_cdev;
			struct mc_subled kbd_led_mc_subled_info[KBD_LED_CHANNELS];
		};
		struct led_classdev kbd_led_cdev;
	};
	struct mutex input_lock;	/* Protects input sequence during notify */
	struct input_dev *input_device;
	struct notifier_block nb;
	struct mutex usb_c_power_priority_lock; /* Protects dependent bit write and state safe */
	enum usb_c_power_priority_options last_usb_c_power_priority_option;
	unsigned int num_profiles;
	unsigned int last_fan_ctrl;
	struct device *pprof_dev;	/* platform_profile class device */
	bool custom_profile_mode_needed;
	bool has_universal_fan_ctrl;
	bool has_double_pl4;
	unsigned int tdp_min[3];
	unsigned int tdp_max[3];
	/* Per-profile default PL values read from EC firmware registers */
	unsigned int tdp_defaults[4][3];	/* [profile_idx][pl_idx] */
	bool overboost_active;		/* True when max-power (overboost) profile is active */
	unsigned int vrm_saved;		/* VRM current limit before overboost */
	unsigned int fan_mode;		/* 0=full-speed, 1=manual, 2=auto */
	unsigned int last_fan_pwm[2];	/* Saved PWM values per fan for suspend/resume */
	bool boost_active;		/* True when EC is in boost (FAN_MODE_BOOST) mode */
	bool has_mini_led_dimming;
	bool mini_led_dimming_state;
	bool has_dgpu_power;
	struct mutex dgpu_power_lock;	/* Protects dGPU power toggle via WMI */
	bool has_gpu_mux;
	bool has_tcc_offset;
	bool dynamic_boost_enable;
	unsigned int ctgp_max;
	unsigned int db_max;
	unsigned int tgp_base;
	/* Water cooler tunnel state */
	struct {
		struct mutex lock;	/* Protects all water cooler fields */
		unsigned long last_update;	/* jiffies of last daemon write */
		unsigned int fan_rpm;		/* Actual fan RPM from daemon */
		unsigned int pump_rpm;		/* Actual pump RPM from daemon */
		u8 fan_pwm;			/* Actual fan PWM from daemon (0-255) */
		u8 pump_pwm;			/* Actual pump PWM from daemon (0-255) */
		u8 fan_pwm_target;		/* Target fan PWM set via hwmon (0-255) */
		u8 pump_pwm_target;		/* Target pump PWM set via hwmon (0-255) */
		unsigned int fan_mode;		/* 0=full, 1=manual, 2=auto */
	} wc;
};

struct uniwill_battery_entry {
	struct list_head head;
	struct power_supply *battery;
};

struct uniwill_device_descriptor {
	unsigned int features;
	unsigned int kbd_led_max_brightness;
	unsigned int lightbar_max_brightness;
	unsigned int num_profiles;
	bool custom_profile_mode_needed;
	/*
	 * Per-device CPU TDP limits in watts.
	 * Index 0 = PL1, 1 = PL2, 2 = PL4.
	 * A zero max value means the corresponding PL level is not supported.
	 * PL4 values are in effective watts (pre-doubling for double_pl4 devices).
	 */
	unsigned int tdp_min[3];
	unsigned int tdp_max[3];
	bool has_hidden_bios_options;
	/* Executed during driver probing */
	int (*probe)(struct uniwill_data *data);
};

static bool force;
module_param_unsafe(force, bool, 0);
MODULE_PARM_DESC(force, "Force loading without checking for supported devices\n");

static bool allow_charge_limit;
module_param(allow_charge_limit, bool, 0444);
MODULE_PARM_DESC(allow_charge_limit, "Allow writing to the charge control threshold register (default: false)");

/*
 * Contains device specific data like the feature bitmap since
 * the associated registers are not always reliable.
 */
static struct uniwill_device_descriptor device_descriptor __ro_after_init;

static const char * const uniwill_temp_labels[] = {
	"CPU",
	"GPU",
};

static const char * const uniwill_fan_labels[] = {
	"Main",
	"Secondary",
	"Water Cooler Fan",
	"Water Cooler Pump",
};

static const char * const uniwill_power_labels[] = {
	"System",
	"GPU Allocation",
	"Thermal Budget",
};

static const char * const uniwill_curr_labels[] = {
	"Adapter",
};

static const struct key_entry uniwill_keymap[] = {
	/* Reported via keyboard controller */
	{ KE_IGNORE,    UNIWILL_OSD_CAPSLOCK,                   { KEY_CAPSLOCK }},
	{ KE_IGNORE,    UNIWILL_OSD_NUMLOCK,                    { KEY_NUMLOCK }},

	/*
	 * Reported when the user enables/disables the super key.
	 * Those events might even be reported when the change was done
	 * using the sysfs attribute!
	 */
	{ KE_IGNORE,    UNIWILL_OSD_SUPER_KEY_DISABLE,		{ KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_SUPER_KEY_ENABLE,		{ KEY_UNKNOWN }},
	/* Optional, might not be reported by all devices */
	{ KE_IGNORE,	UNIWILL_OSD_SUPER_KEY_STATE_CHANGED,	{ KEY_UNKNOWN }},

	/* Reported in manual mode when toggling the airplane mode status */
	{ KE_KEY,       UNIWILL_OSD_RFKILL,                     { KEY_RFKILL }},
	{ KE_IGNORE,    UNIWILL_OSD_RADIOON,                    { KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_RADIOOFF,                   { KEY_UNKNOWN }},

	/* Handled in notifier to cycle the platform profile directly */
	{ KE_IGNORE,    UNIWILL_OSD_PERFORMANCE_MODE_TOGGLE,    { KEY_UNKNOWN }},

	/* Reported when the user wants to adjust the brightness of the keyboard */
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMDOWN,               { KEY_KBDILLUMDOWN }},
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMUP,                 { KEY_KBDILLUMUP }},

	/* Reported when the EC changed the keyboard backlight brightness */
	{ KE_IGNORE,	UNIWILL_OSD_BACKLIGHT_LEVEL_CHANGE,	{ KEY_UNKNOWN }},

	/* Reported when the user wants to toggle the microphone mute status */
	{ KE_KEY,       UNIWILL_OSD_MIC_MUTE,                   { KEY_MICMUTE }},

	/* Reported when the user wants to toggle the mute status */
	{ KE_IGNORE,    UNIWILL_OSD_MUTE,                       { KEY_MUTE }},

	/* Reported when the user wants to toggle the brightness of the keyboard */
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMTOGGLE,             { KEY_KBDILLUMTOGGLE }},

	/* FIXME: find out the exact meaning of those events */
	{ KE_IGNORE,    UNIWILL_OSD_BAT_CHARGE_FULL_24_H,       { KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_BAT_ERM_UPDATE,             { KEY_UNKNOWN }},

	/* Reported when the user wants to toggle the benchmark mode status */
	{ KE_IGNORE,    UNIWILL_OSD_BENCHMARK_MODE_TOGGLE,      { KEY_UNKNOWN }},

	/* Reported when the screen is enabled/disabled during resume/suspend */
	{ KE_IGNORE,	UNIWILL_OSD_SCREEN_STATE_CHANGED,	{ KEY_UNKNOWN }},

	/* Reported when the user wants to toggle the webcam */
	{ KE_IGNORE,    UNIWILL_OSD_WEBCAM_TOGGLE,              { KEY_UNKNOWN }},

	{ KE_END }
};

static inline bool uniwill_device_supports(const struct uniwill_data *data,
					   unsigned int features)
{
	return (data->features & features) == features;
}

static int uniwill_ec_reg_write(void *context, unsigned int reg, unsigned int val)
{
	union acpi_object params[2] = {
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = reg,
			},
		},
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = val,
			},
		},
	};
	struct uniwill_data *data = context;
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	acpi_status status;

	status = acpi_evaluate_object(data->handle, "ECRW", &input, NULL);
	if (ACPI_FAILURE(status))
		return -EIO;

	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);

	return 0;
}

static int uniwill_ec_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	union acpi_object params[1] = {
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = reg,
			},
		},
	};
	struct uniwill_data *data = context;
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	unsigned long long output;
	acpi_status status;

	status = acpi_evaluate_integer(data->handle, "ECRR", &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (output > U8_MAX)
		return -ENXIO;

	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);

	*val = output;

	return 0;
}

static const struct regmap_bus uniwill_ec_bus = {
	.reg_write = uniwill_ec_reg_write,
	.reg_read = uniwill_ec_reg_read,
	.reg_format_endian_default = REGMAP_ENDIAN_LITTLE,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

static bool uniwill_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_OEM_9:
	case EC_ADDR_AP_OEM:
	case EC_ADDR_LIGHTBAR_AC_CTRL:
	case EC_ADDR_LIGHTBAR_AC_RED:
	case EC_ADDR_LIGHTBAR_AC_GREEN:
	case EC_ADDR_LIGHTBAR_AC_BLUE:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_RGB_RED:
	case EC_ADDR_RGB_GREEN:
	case EC_ADDR_RGB_BLUE:
	case EC_ADDR_BIOS_OEM_2:
	case EC_ADDR_KBD_STATUS:
	case EC_ADDR_OEM_4:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_RED:
	case EC_ADDR_LIGHTBAR_BAT_GREEN:
	case EC_ADDR_LIGHTBAR_BAT_BLUE:
	case EC_ADDR_CUSTOM_PROFILE:
	case EC_ADDR_GPU_MUX_MODE:
	case EC_ADDR_CTGP_DB_CTRL:
	case EC_ADDR_CTGP_DB_CTGP_OFFSET:
	case EC_ADDR_CTGP_DB_TPP_OFFSET:
	case EC_ADDR_CTGP_DB_DB_OFFSET:
	case EC_ADDR_OEM_3:
	case EC_ADDR_PL1_SETTING:
	case EC_ADDR_PL2_SETTING:
	case EC_ADDR_PL4_SETTING:
	case EC_ADDR_TCC_OFFSET:
	case EC_ADDR_VRM_CURRENT_LIMIT:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_UNIVERSAL_FAN_CTRL:
	case EC_ADDR_AP_OEM_6:
	case EC_ADDR_USB_C_POWER_PRIORITY:
	case EC_ADDR_FAN_SWITCH_SPEED:
	case EC_ADDR_CPU_TEMP_END_TABLE ... EC_ADDR_CPU_TEMP_END_TABLE + 0xF:
	case EC_ADDR_CPU_TEMP_START_TABLE ... EC_ADDR_CPU_TEMP_START_TABLE + 0xF:
	case EC_ADDR_CPU_FAN_SPEED_TABLE ... EC_ADDR_CPU_FAN_SPEED_TABLE + 0xF:
	case EC_ADDR_GPU_TEMP_END_TABLE ... EC_ADDR_GPU_TEMP_END_TABLE + 0xF:
	case EC_ADDR_GPU_TEMP_START_TABLE ... EC_ADDR_GPU_TEMP_START_TABLE + 0xF:
	case EC_ADDR_GPU_FAN_SPEED_TABLE ... EC_ADDR_GPU_FAN_SPEED_TABLE + 0xF:
		return true;
	default:
		return false;
	}
}

static bool uniwill_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_CPU_TEMP:
	case EC_ADDR_GPU_TEMP:
	case EC_ADDR_MAIN_FAN_RPM_1:
	case EC_ADDR_MAIN_FAN_RPM_2:
	case EC_ADDR_SECOND_FAN_RPM_1:
	case EC_ADDR_SECOND_FAN_RPM_2:
	case EC_ADDR_BAT_ALERT:
	case EC_ADDR_OEM_9:
	case EC_ADDR_PROJECT_ID:
	case EC_ADDR_AP_OEM:
	case EC_ADDR_LIGHTBAR_AC_CTRL:
	case EC_ADDR_LIGHTBAR_AC_RED:
	case EC_ADDR_LIGHTBAR_AC_GREEN:
	case EC_ADDR_LIGHTBAR_AC_BLUE:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_PWM_1:
	case EC_ADDR_PWM_2:
	case EC_ADDR_SUPPORT_2:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_SWITCH_STATUS:
	case EC_ADDR_RGB_RED:
	case EC_ADDR_RGB_GREEN:
	case EC_ADDR_RGB_BLUE:
	case EC_ADDR_BIOS_OEM_2:
	case EC_ADDR_KBD_STATUS:
	case EC_ADDR_OEM_4:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_CHARGE_CTRL_START:
	case EC_ADDR_LIGHTBAR_BAT_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_RED:
	case EC_ADDR_LIGHTBAR_BAT_GREEN:
	case EC_ADDR_LIGHTBAR_BAT_BLUE:
	case EC_ADDR_SYSTEM_ID:
	case EC_ADDR_CUSTOM_PROFILE:
	case EC_ADDR_PL1_DEFAULT_GAMING:
	case EC_ADDR_PL2_DEFAULT_GAMING:
	case EC_ADDR_PL4_DEFAULT_GAMING:
	case EC_ADDR_PL1_DEFAULT_OFFICE:
	case EC_ADDR_PL2_DEFAULT_OFFICE:
	case EC_ADDR_PL4_DEFAULT_OFFICE:
	case EC_ADDR_CTGP_DB_CTRL:
	case EC_ADDR_CTGP_DB_CTGP_OFFSET:
	case EC_ADDR_CTGP_DB_TPP_OFFSET:
	case EC_ADDR_CTGP_DB_DB_OFFSET:
	case EC_ADDR_OEM_3:
	case EC_ADDR_PL1_SETTING:
	case EC_ADDR_PL2_SETTING:
	case EC_ADDR_PL4_SETTING:
	case EC_ADDR_TCC_OFFSET:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_FAN_CTRL:
	case EC_ADDR_UNIVERSAL_FAN_CTRL:
	case EC_ADDR_AP_OEM_6:
	case EC_ADDR_MINI_LED_SUPPORT:
	case EC_ADDR_USB_C_POWER_PRIORITY:
	case EC_ADDR_ADAPTER_CURRENT:
	case EC_ADDR_CPU_TEMP_LIMIT:
	case EC_ADDR_SYSTEM_POWER_LO:
	case EC_ADDR_SYSTEM_POWER_HI:
	case EC_ADDR_GPU_POWER_ALLOC:
	case EC_ADDR_THERMAL_BUDGET:
	case EC_ADDR_FAN_SWITCH_SPEED:
	case EC_ADDR_VRM_CURRENT_LIMIT:
	case EC_ADDR_VRM_MAX_CURRENT_LIMIT:
	case EC_ADDR_CPU_TEMP_END_TABLE ... EC_ADDR_CPU_TEMP_END_TABLE + 0xF:
	case EC_ADDR_CPU_TEMP_START_TABLE ... EC_ADDR_CPU_TEMP_START_TABLE + 0xF:
	case EC_ADDR_CPU_FAN_SPEED_TABLE ... EC_ADDR_CPU_FAN_SPEED_TABLE + 0xF:
	case EC_ADDR_GPU_TEMP_END_TABLE ... EC_ADDR_GPU_TEMP_END_TABLE + 0xF:
	case EC_ADDR_GPU_TEMP_START_TABLE ... EC_ADDR_GPU_TEMP_START_TABLE + 0xF:
	case EC_ADDR_GPU_FAN_SPEED_TABLE ... EC_ADDR_GPU_FAN_SPEED_TABLE + 0xF:
		return true;
	default:
		return false;
	}
}

static bool uniwill_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_CPU_TEMP:
	case EC_ADDR_GPU_TEMP:
	case EC_ADDR_MAIN_FAN_RPM_1:
	case EC_ADDR_MAIN_FAN_RPM_2:
	case EC_ADDR_SECOND_FAN_RPM_1:
	case EC_ADDR_SECOND_FAN_RPM_2:
	case EC_ADDR_BAT_ALERT:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_PWM_1:
	case EC_ADDR_PWM_2:
	case EC_ADDR_SUPPORT_2:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_SWITCH_STATUS:
	case EC_ADDR_KBD_STATUS:
	case EC_ADDR_CUSTOM_PROFILE:
	case EC_ADDR_PL1_SETTING:
	case EC_ADDR_PL2_SETTING:
	case EC_ADDR_PL4_SETTING:
	case EC_ADDR_TCC_OFFSET:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_CHARGE_CTRL_START:
	case EC_ADDR_USB_C_POWER_PRIORITY:
	case EC_ADDR_OEM_3:
	case EC_ADDR_VRM_CURRENT_LIMIT:
	case EC_ADDR_VRM_MAX_CURRENT_LIMIT:
	case EC_ADDR_ADAPTER_CURRENT:
	case EC_ADDR_CPU_TEMP_LIMIT:
	case EC_ADDR_SYSTEM_POWER_LO:
	case EC_ADDR_SYSTEM_POWER_HI:
	case EC_ADDR_GPU_POWER_ALLOC:
	case EC_ADDR_THERMAL_BUDGET:
	case EC_ADDR_CPU_FAN_SPEED_TABLE ... EC_ADDR_CPU_FAN_SPEED_TABLE + 0xF:
	case EC_ADDR_GPU_FAN_SPEED_TABLE ... EC_ADDR_GPU_FAN_SPEED_TABLE + 0xF:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config uniwill_ec_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.writeable_reg = uniwill_writeable_reg,
	.readable_reg = uniwill_readable_reg,
	.volatile_reg = uniwill_volatile_reg,
	.can_sleep = true,
	.max_register = 0xFFF,
	.cache_type = REGCACHE_MAPLE,
	.use_single_read = true,
	.use_single_write = true,
};

static ssize_t fn_lock_store(struct device *dev, struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		value = FN_LOCK_STATUS;
	else
		value = 0;

	ret = regmap_update_bits(data->regmap, EC_ADDR_BIOS_OEM, FN_LOCK_STATUS, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t fn_lock_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_BIOS_OEM, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(value & FN_LOCK_STATUS));
}

static DEVICE_ATTR_RW(fn_lock);

static ssize_t super_key_enable_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->super_key_lock);

	ret = regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	/*
	 * We can only toggle the super key lock, so we return early if the setting
	 * is already in the correct state.
	 */
	if (enable == !(value & SUPER_KEY_LOCK_STATUS))
		return count;

	ret = regmap_write_bits(data->regmap, EC_ADDR_TRIGGER, TRIGGER_SUPER_KEY_LOCK,
				TRIGGER_SUPER_KEY_LOCK);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t super_key_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & SUPER_KEY_LOCK_STATUS));
}

static DEVICE_ATTR_RW(super_key_enable);

static ssize_t touchpad_toggle_enable_store(struct device *dev, struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		value = 0;
	else
		value = TOUCHPAD_TOGGLE_OFF;

	ret = regmap_update_bits(data->regmap, EC_ADDR_OEM_4, TOUCHPAD_TOGGLE_OFF, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t touchpad_toggle_enable_show(struct device *dev, struct device_attribute *attr,
					   char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_OEM_4, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & TOUCHPAD_TOGGLE_OFF));
}

static DEVICE_ATTR_RW(touchpad_toggle_enable);

static ssize_t rainbow_animation_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		value = LIGHTBAR_WELCOME;
	else
		value = 0;

	guard(mutex)(&data->led_lock);

	ret = regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, LIGHTBAR_WELCOME, value);
	if (ret < 0)
		return ret;

	ret = regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_BAT_CTRL, LIGHTBAR_WELCOME, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t rainbow_animation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(value & LIGHTBAR_WELCOME));
}

static DEVICE_ATTR_RW(rainbow_animation);

static ssize_t breathing_in_suspend_store(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		value = 0;
	else
		value = LIGHTBAR_S3_OFF;

	/* We only access a single register here, so we do not need to use data->led_lock */
	ret = regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, LIGHTBAR_S3_OFF, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t breathing_in_suspend_show(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & LIGHTBAR_S3_OFF));
}

static DEVICE_ATTR_RW(breathing_in_suspend);

static ssize_t ctgp_offset_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value > U8_MAX)
		return -EINVAL;

	if (data->ctgp_max && value > data->ctgp_max)
		return -EINVAL;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t ctgp_offset_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(ctgp_offset);

static ssize_t ctgp_offset_max_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->ctgp_max);
}

static DEVICE_ATTR_RO(ctgp_offset_max);

static ssize_t db_offset_max_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->db_max);
}

static DEVICE_ATTR_RO(db_offset_max);

static ssize_t tgp_base_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tgp_base);
}

static DEVICE_ATTR_RO(tgp_base);

static ssize_t dynamic_boost_enable_store(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	ret = regmap_update_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
				 CTGP_DB_DB_ENABLE, enable ? CTGP_DB_DB_ENABLE : 0);
	if (ret < 0)
		return ret;

	data->dynamic_boost_enable = enable;

	return count;
}

static ssize_t dynamic_boost_enable_show(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_CTGP_DB_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(value & CTGP_DB_DB_ENABLE));
}

static DEVICE_ATTR_RW(dynamic_boost_enable);

static int uniwill_nvidia_ctgp_init(struct uniwill_data *data)
{
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_TPP_OFFSET, 255);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_DB_OFFSET,
			    data->db_max ? data->db_max : 25);
	if (ret < 0)
		return ret;

	ret = regmap_set_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
			      CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE | CTGP_DB_CTGP_ENABLE);
	if (ret < 0)
		return ret;

	data->dynamic_boost_enable = true;

	return 0;
}

static ssize_t cpu_pl1_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value < data->tdp_min[0] || value > data->tdp_max[0])
		return -EINVAL;

	ret = regmap_write(data->regmap, EC_ADDR_PL1_SETTING, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t cpu_pl1_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PL1_SETTING, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(cpu_pl1);

static ssize_t cpu_pl1_min_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_min[0]);
}

static DEVICE_ATTR_RO(cpu_pl1_min);

static ssize_t cpu_pl1_max_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_max[0]);
}

static DEVICE_ATTR_RO(cpu_pl1_max);

static ssize_t cpu_pl2_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value < data->tdp_min[1] || value > data->tdp_max[1])
		return -EINVAL;

	ret = regmap_write(data->regmap, EC_ADDR_PL2_SETTING, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t cpu_pl2_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PL2_SETTING, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(cpu_pl2);

static ssize_t cpu_pl2_min_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_min[1]);
}

static DEVICE_ATTR_RO(cpu_pl2_min);

static ssize_t cpu_pl2_max_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_max[1]);
}

static DEVICE_ATTR_RO(cpu_pl2_max);

static ssize_t cpu_pl4_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	u8 ec_value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value < data->tdp_min[2] || value > data->tdp_max[2])
		return -EINVAL;

	/*
	 * Some devices store half the PL4 value in the EC register.
	 * The maximum effective PL4 is thus 510 W on those devices.
	 */
	if (data->has_double_pl4)
		ec_value = value / 2;
	else
		ec_value = value;

	ret = regmap_write(data->regmap, EC_ADDR_PL4_SETTING, ec_value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t cpu_pl4_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PL4_SETTING, &value);
	if (ret < 0)
		return ret;

	if (data->has_double_pl4)
		value *= 2;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(cpu_pl4);

static ssize_t cpu_pl4_min_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_min[2]);
}

static DEVICE_ATTR_RO(cpu_pl4_min);

static ssize_t cpu_pl4_max_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->tdp_max[2]);
}

static DEVICE_ATTR_RO(cpu_pl4_max);

#define SMRW_BUFFER_SIZE	13
#define SMRW_CMD_READ		0xBB
#define SMRW_INDEX_PL1		0
#define SMRW_INDEX_PL2		1
#define SMRW_INDEX_PL4		2
#define SMRW_INDEX_CTGP_MAX	4
#define SMRW_INDEX_DB_MAX	5
#define SMRW_INDEX_TGP_BASE	7

/*
 * Read a single entry from the ACPI SMRW configuration table.
 *
 * The SMRW method on the INOU device provides per-device power limits
 * stored in firmware. The method takes a 13-byte buffer argument where
 * byte 0 is the command (0xBB = read) and byte 1 is the table index.
 * It returns a dword whose LSB is the config value.
 */
static int uniwill_smrw_read(struct uniwill_data *data, u8 index, u8 *val)
{
	union acpi_object param;
	struct acpi_object_list input;
	unsigned long long output;
	acpi_status status;
	u8 buf[SMRW_BUFFER_SIZE] = {};

	buf[0] = SMRW_CMD_READ;
	buf[1] = index;

	param.type = ACPI_TYPE_BUFFER;
	param.buffer.length = sizeof(buf);
	param.buffer.pointer = buf;

	input.count = 1;
	input.pointer = &param;

	status = acpi_evaluate_integer(data->handle, "SMRW", &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	*val = output & 0xFF;

	return 0;
}

/*
 * Try to override hardcoded TDP limits with dynamic values from SMRW.
 *
 * Only overrides a limit if the returned value passes the firmware's
 * own validation rules (value in range and not 0xFF). Values outside
 * these ranges indicate the firmware does not provide that limit.
 */
static int uniwill_smrw_read_tdp_limits(struct uniwill_data *data)
{
	u8 pl1, pl2, pl4;
	unsigned int pl4_effective;
	unsigned int old_max[3];
	int ret;

	ret = uniwill_smrw_read(data, SMRW_INDEX_PL1, &pl1);
	if (ret < 0)
		return ret;

	ret = uniwill_smrw_read(data, SMRW_INDEX_PL2, &pl2);
	if (ret < 0)
		return ret;

	ret = uniwill_smrw_read(data, SMRW_INDEX_PL4, &pl4);
	if (ret < 0)
		return ret;

	memcpy(old_max, data->tdp_max, sizeof(old_max));

	if (pl1 != 0xFF && pl1 >= 15 && pl1 <= 253)
		data->tdp_max[0] = pl1;

	if (pl2 != 0xFF && pl2 >= 15 && pl2 <= 253)
		data->tdp_max[1] = pl2;

	if (pl4 != 0xFF && pl4 >= 15 && pl4 <= 250) {
		pl4_effective = pl4;
		if (data->has_double_pl4)
			pl4_effective *= 2;

		data->tdp_max[2] = pl4_effective;
	}

	if (old_max[0] != data->tdp_max[0] ||
	    old_max[1] != data->tdp_max[1] ||
	    old_max[2] != data->tdp_max[2])
		dev_warn(data->dev,
			 "SMRW overrides hardcoded TDP max: PL1 %u->%u PL2 %u->%u PL4 %u->%u\n",
			 old_max[0], data->tdp_max[0],
			 old_max[1], data->tdp_max[1],
			 old_max[2], data->tdp_max[2]);

	dev_info(data->dev,
		 "SMRW dynamic TDP limits: PL1=%u PL2=%u PL4=%u\n",
		 data->tdp_max[0], data->tdp_max[1], data->tdp_max[2]);

	return 0;
}

/*
 * Read GPU power limits from SMRW and store them for runtime use.
 *
 * Index 4: cTGP offset max (watts) - upper bound for ctgp_offset sysfs
 * Index 5: Dynamic Boost max (watts) - used by nvidia_ctgp_init
 * Index 7: TGP base (watts) - base GPU power, exposed read-only
 */
static void uniwill_smrw_read_gpu_limits(struct uniwill_data *data)
{
	u8 val;
	int ret;

	ret = uniwill_smrw_read(data, SMRW_INDEX_CTGP_MAX, &val);
	if (ret == 0 && val != 0xFF && val > 0)
		data->ctgp_max = val;

	ret = uniwill_smrw_read(data, SMRW_INDEX_DB_MAX, &val);
	if (ret == 0 && val != 0xFF && val > 0)
		data->db_max = val;

	ret = uniwill_smrw_read(data, SMRW_INDEX_TGP_BASE, &val);
	if (ret == 0 && val != 0xFF && val > 0)
		data->tgp_base = val;

	dev_info(data->dev,
		 "SMRW GPU limits: cTGP_max=%u DB_max=%u TGP_base=%u\n",
		 data->ctgp_max, data->db_max, data->tgp_base);
}

static ssize_t vrm_current_limit_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_VRM_CURRENT_LIMIT, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RO(vrm_current_limit);

static ssize_t vrm_max_current_limit_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_VRM_MAX_CURRENT_LIMIT, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RO(vrm_max_current_limit);

static ssize_t fan_switch_speed_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_FAN_SWITCH_SPEED, &value);
	if (ret < 0)
		return ret;

	if (!(value & FAN_SWITCH_SPEED_ENABLE))
		return sysfs_emit(buf, "0\n");

	/* Delay is in 100ms units, report in milliseconds */
	return sysfs_emit(buf, "%u\n",
			  (unsigned int)(value & FAN_SWITCH_SPEED_DELAY_MASK) * 100);
}

static ssize_t fan_switch_speed_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value == 0) {
		ret = regmap_write(data->regmap, EC_ADDR_FAN_SWITCH_SPEED, 0);
	} else {
		/* Convert ms to 100ms units, clamp to 1-127 */
		value = DIV_ROUND_CLOSEST(value, 100);
		if (value < 1)
			value = 1;
		if (value > 127)
			value = 127;
		ret = regmap_write(data->regmap, EC_ADDR_FAN_SWITCH_SPEED,
				   FAN_SWITCH_SPEED_ENABLE | value);
	}

	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(fan_switch_speed);

static int uniwill_cpu_tdp_init(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL))
		return 0;

	/*
	 * Detect double PL4 mode. Bit 7 of EC register 0x0727 indicates
	 * that PL4 values written to the EC need to be halved.
	 */
	ret = regmap_read(data->regmap, EC_ADDR_CUSTOM_PROFILE, &value);
	if (ret < 0)
		return ret;

	data->has_double_pl4 = !!(value & BIT(7));

	/*
	 * Try to read dynamic TDP limits from the ACPI SMRW method.
	 *
	 * The SMRW method provides a per-device configuration table
	 * populated by the firmware. If valid values are returned,
	 * they override the hardcoded per-device limits since the
	 * firmware knows the exact capabilities of the hardware.
	 *
	 * SMRW takes a 13-byte buffer: byte 0 = 0xBB (read command),
	 * byte 1 = index into the config table. It returns a dword
	 * whose LSB contains the config value.
	 *
	 * Table layout:
	 *   Index 0: PL1 max (watts)
	 *   Index 1: PL2 max (watts)
	 *   Index 2: PL4 max (raw; doubled if has_double_pl4)
	 */
	ret = uniwill_smrw_read_tdp_limits(data);
	if (ret < 0)
		dev_dbg(data->dev, "SMRW dynamic TDP read unavailable: %d\n", ret);

	/*
	 * Read per-profile default PL values from EC firmware registers.
	 * These are set by the firmware and define the PL1/PL2/PL4 values
	 * that should be applied when switching between profiles.
	 *
	 * Profile 0 = Quiet (Office): EC 0x0734-0x0736
	 * Profile 1 = Balanced (Gaming): EC 0x0730-0x0732
	 * Profile 2 = Performance (Turbo): 0 = use BIOS default (max)
	 */
	ret = regmap_read(data->regmap, EC_ADDR_PL1_DEFAULT_OFFICE, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[0][0] = value;

	ret = regmap_read(data->regmap, EC_ADDR_PL2_DEFAULT_OFFICE, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[0][1] = value;

	ret = regmap_read(data->regmap, EC_ADDR_PL4_DEFAULT_OFFICE, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[0][2] = value;

	ret = regmap_read(data->regmap, EC_ADDR_PL1_DEFAULT_GAMING, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[1][0] = value;

	ret = regmap_read(data->regmap, EC_ADDR_PL2_DEFAULT_GAMING, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[1][1] = value;

	ret = regmap_read(data->regmap, EC_ADDR_PL4_DEFAULT_GAMING, &value);
	if (ret < 0)
		return ret;
	data->tdp_defaults[1][2] = value;

	/* Performance (Turbo) mode: midpoint between balanced and max values */
	data->tdp_defaults[2][0] = (data->tdp_defaults[1][0] + data->tdp_max[0]) / 2;
	data->tdp_defaults[2][1] = (data->tdp_defaults[1][1] + data->tdp_max[1]) / 2;
	data->tdp_defaults[2][2] = (data->tdp_defaults[1][2] + data->tdp_max[2]) / 2;

	/*
	 * Overboost (max-power) profile: use the SMRW-reported maximums
	 * if available, otherwise fall back to hardcoded descriptor limits.
	 * These are written to the EC together with a raised VRM current
	 * limit to unlock the full power envelope with liquid cooling.
	 */
	data->tdp_defaults[3][0] = data->tdp_max[0];
	data->tdp_defaults[3][1] = data->tdp_max[1];
	data->tdp_defaults[3][2] = data->tdp_max[2];

	dev_dbg(data->dev, "TDP defaults: quiet=%u/%u/%u gaming=%u/%u/%u\n",
		data->tdp_defaults[0][0], data->tdp_defaults[0][1],
		data->tdp_defaults[0][2], data->tdp_defaults[1][0],
		data->tdp_defaults[1][1], data->tdp_defaults[1][2]);

	return 0;
}

/*
 * Hidden BIOS options.
 *
 * Some devices store overclocking/performance switches in an EFI variable
 * ("OemMagicVariable") but hide them from the user in the BIOS setup UI.
 * Enabling them simply makes those options visible in BIOS setup.
 */

#define EFI_OEM_MAGIC_MEM_OC_SWITCH	0x33
#define EFI_OEM_MAGIC_MEM_OC_SUPPORT	0x60
#define EFI_OEM_MAGIC_CPU_OC_SUPPORT	0x6E
#define EFI_OEM_MAGIC_CPU_OC_SWITCH	0x6F

static efi_guid_t oem_magic_guid =
	EFI_GUID(0x9f33f85c, 0x13ca, 0x4fd1,
		 0x9c, 0x4a, 0x96, 0x21, 0x77, 0x22, 0xc5, 0x93);

static void uniwill_show_hidden_bios_options(struct uniwill_data *data)
{
	efi_char16_t name[] = L"OemMagicVariable";
	unsigned long size = 0;
	bool changed = false;
	efi_status_t st;
	u8 *buf = NULL;
	u32 attr = 0;
	u8 b1, b2;

	if (!efi_enabled(EFI_RUNTIME_SERVICES)) {
		dev_warn(data->dev,
			 "hidden_bios_options: EFI runtime services not available\n");
		return;
	}

	st = efi.get_variable(name, &oem_magic_guid, &attr, &size, NULL);
	if (st != EFI_BUFFER_TOO_SMALL) {
		dev_err(data->dev,
			"hidden_bios_options: get_variable probe failed: st=0x%lx\n",
			(unsigned long)st);
		return;
	}

	if (size <= EFI_OEM_MAGIC_CPU_OC_SWITCH) {
		dev_err(data->dev,
			"hidden_bios_options: EFI variable too small (%lu bytes)\n",
			size);
		return;
	}

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return;

	st = efi.get_variable(name, &oem_magic_guid, &attr, &size, buf);
	if (st != EFI_SUCCESS) {
		dev_err(data->dev,
			"hidden_bios_options: get_variable read failed: st=0x%lx\n",
			(unsigned long)st);
		goto out;
	}

	if (buf[EFI_OEM_MAGIC_MEM_OC_SUPPORT] != 0x01 ||
	    buf[EFI_OEM_MAGIC_CPU_OC_SUPPORT] != 0x01) {
		dev_warn(data->dev,
			 "hidden_bios_options: support bits not set (mem=0x%02x cpu=0x%02x)\n",
			 buf[EFI_OEM_MAGIC_MEM_OC_SUPPORT],
			 buf[EFI_OEM_MAGIC_CPU_OC_SUPPORT]);
		goto out;
	}

	b1 = buf[EFI_OEM_MAGIC_MEM_OC_SWITCH];
	b2 = buf[EFI_OEM_MAGIC_CPU_OC_SWITCH];

	if (!((b1 == 0x00 || b1 == 0x01) && (b2 == 0x00 || b2 == 0x01))) {
		dev_err(data->dev,
			"hidden_bios_options: unexpected byte values mem=0x%02x cpu=0x%02x\n",
			b1, b2);
		goto out;
	}

	if (b1 == 0x00) {
		buf[EFI_OEM_MAGIC_MEM_OC_SWITCH] = 0x01;
		changed = true;
	}

	if (b2 == 0x00) {
		buf[EFI_OEM_MAGIC_CPU_OC_SWITCH] = 0x01;
		changed = true;
	}

	if (!changed) {
		dev_dbg(data->dev, "hidden_bios_options: already enabled\n");
		goto out;
	}

	st = efi.set_variable(name, &oem_magic_guid, attr, size, buf);
	if (st != EFI_SUCCESS) {
		dev_warn(data->dev,
			 "hidden_bios_options: set_variable failed: st=0x%lx\n",
			 (unsigned long)st);
		goto out;
	}

	dev_info(data->dev, "hidden_bios_options: enabled hidden BIOS options\n");

out:
	kfree(buf);
}

static const char * const USB_C_POWER_PRIORITY_TEXT[] = {
	[USB_C_POWER_PRIORITY_CHARGING]		= "charging",
	[USB_C_POWER_PRIORITY_PERFORMANCE]	= "performance",
};

static const u8 USB_C_POWER_PRIORITY_VALUE[] = {
	[USB_C_POWER_PRIORITY_CHARGING]		= 0,
	[USB_C_POWER_PRIORITY_PERFORMANCE]	= USB_C_POWER_PRIORITY,
};

static ssize_t usb_c_power_priority_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	enum usb_c_power_priority_options option;
	unsigned int value;
	int ret;

	option = sysfs_match_string(USB_C_POWER_PRIORITY_TEXT, buf);
	if (option < 0)
		return option;

	value = USB_C_POWER_PRIORITY_VALUE[option];

	guard(mutex)(&data->usb_c_power_priority_lock);

	ret = regmap_update_bits(data->regmap, EC_ADDR_USB_C_POWER_PRIORITY,
				 USB_C_POWER_PRIORITY, value);
	if (ret < 0)
		return ret;

	data->last_usb_c_power_priority_option = option;

	return count;
}

static ssize_t usb_c_power_priority_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_USB_C_POWER_PRIORITY, &value);
	if (ret < 0)
		return ret;

	value &= USB_C_POWER_PRIORITY;

	if (USB_C_POWER_PRIORITY_VALUE[USB_C_POWER_PRIORITY_PERFORMANCE] == value)
		return sysfs_emit(buf, "%s\n",
				  USB_C_POWER_PRIORITY_TEXT[USB_C_POWER_PRIORITY_PERFORMANCE]);

	return sysfs_emit(buf, "%s\n", USB_C_POWER_PRIORITY_TEXT[USB_C_POWER_PRIORITY_CHARGING]);
}

static DEVICE_ATTR_RW(usb_c_power_priority);

static int usb_c_power_priority_restore(struct uniwill_data *data)
{
	unsigned int value;

	value = USB_C_POWER_PRIORITY_VALUE[data->last_usb_c_power_priority_option];

	guard(mutex)(&data->usb_c_power_priority_lock);

	return regmap_update_bits(data->regmap, EC_ADDR_USB_C_POWER_PRIORITY,
				  USB_C_POWER_PRIORITY, value);
}

static int usb_c_power_priority_init(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_USB_C_POWER_PRIORITY))
		return 0;

	ret = devm_mutex_init(data->dev, &data->usb_c_power_priority_lock);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, EC_ADDR_USB_C_POWER_PRIORITY, &value);
	if (ret < 0)
		return ret;

	value &= USB_C_POWER_PRIORITY;

	data->last_usb_c_power_priority_option =
		USB_C_POWER_PRIORITY_VALUE[USB_C_POWER_PRIORITY_PERFORMANCE] == value ?
			USB_C_POWER_PRIORITY_PERFORMANCE :
			USB_C_POWER_PRIORITY_CHARGING;

	return 0;
}

static ssize_t ac_auto_boot_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		regval = AC_AUTO_BOOT_ENABLE;
	else
		regval = 0;

	ret = regmap_update_bits(data->regmap, EC_ADDR_OEM_9, AC_AUTO_BOOT_ENABLE, regval);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t ac_auto_boot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_OEM_9, &regval);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(regval & AC_AUTO_BOOT_ENABLE));
}

static DEVICE_ATTR_RW(ac_auto_boot);

static ssize_t usb_powershare_high_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		regval = TRIGGER_USB_CHARGING;
	else
		regval = 0;

	/*
	 * Normaly this RMW-sequence could also trigger the super key toggle,
	 * but the EC seems to take care that those bits are always read as 0.
	 */
	ret = regmap_update_bits(data->regmap, EC_ADDR_TRIGGER, TRIGGER_USB_CHARGING, regval);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t usb_powershare_high_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int regval;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_TRIGGER, &regval);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(regval & TRIGGER_USB_CHARGING));
}

static DEVICE_ATTR_RW(usb_powershare_high);

static ssize_t mini_led_local_dimming_store(struct device *dev, struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		ret = uniwill_wmi_evaluate(UNIWILL_WMI_FUNC_FEATURE_TOGGLE,
					   UNIWILL_WMI_LOCAL_DIMMING_ON);
	else
		ret = uniwill_wmi_evaluate(UNIWILL_WMI_FUNC_FEATURE_TOGGLE,
					   UNIWILL_WMI_LOCAL_DIMMING_OFF);

	if (ret < 0)
		return ret;

	data->mini_led_dimming_state = enable;

	return count;
}

static ssize_t mini_led_local_dimming_show(struct device *dev, struct device_attribute *attr,
					   char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", data->mini_led_dimming_state);
}

static DEVICE_ATTR_RW(mini_led_local_dimming);

/*
 * CPU TCC (Thermal Control Circuit) offset.
 *
 * The TCC offset lowers the temperature at which the CPU begins thermal
 * throttling. For example, with Tjunction_max = 100°C and a TCC offset
 * of 10, throttling starts at 90°C.
 *
 * EC register 0x0786 layout (confirmed via DSDT APTC/APTN fields):
 *   bits [6:0] = offset value in degrees Celsius (0-63)
 *   bit  [7]   = enable (1 = offset active, 0 = disabled)
 *
 * Writing 0 disables the TCC offset entirely (clears enable bit).
 * Writing a non-zero value sets the offset and enables it.
 */
static ssize_t cpu_tcc_offset_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_TCC_OFFSET, &value);
	if (ret < 0)
		return ret;

	if (!(value & TCC_OFFSET_ENABLE))
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%u\n", (unsigned int)(value & TCC_OFFSET_VALUE_MASK));
}

static ssize_t cpu_tcc_offset_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int offset;
	u8 value;
	int ret;

	ret = kstrtouint(buf, 10, &offset);
	if (ret < 0)
		return ret;

	if (offset > TCC_OFFSET_MAX)
		return -EINVAL;

	if (offset == 0)
		value = 0;
	else
		value = TCC_OFFSET_ENABLE | offset;

	ret = regmap_write(data->regmap, EC_ADDR_TCC_OFFSET, value);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(cpu_tcc_offset);

/*
 * Check if an NVIDIA dGPU is present on the PCI bus.
 *
 * The DSDT DGPS() method checks PG00._STA() which starts in a stale state
 * at boot (it only becomes correct after IGPS() has been used). Checking
 * actual PCI device presence is reliable regardless of boot state.
 */
static bool uniwill_dgpu_is_on(void)
{
	struct pci_dev *pdev = NULL;

	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, pdev))) {
		if (pdev->vendor == PCI_VENDOR_ID_NVIDIA) {
			pci_dev_put(pdev);
			return true;
		}
	}

	while ((pdev = pci_get_class(PCI_CLASS_DISPLAY_3D << 8, pdev))) {
		if (pdev->vendor == PCI_VENDOR_ID_NVIDIA) {
			pci_dev_put(pdev);
			return true;
		}
	}

	return false;
}

static ssize_t dgpu_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", uniwill_dgpu_is_on() ? 1 : 0);
}

static ssize_t dgpu_power_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u32 result;
	bool enable, is_on;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	is_on = uniwill_dgpu_is_on();

	/* Already in the requested state */
	if (enable == is_on)
		return count;

	guard(mutex)(&data->dgpu_power_lock);

	/* enable=true: power on dGPU (IGPS(0)), enable=false: power off dGPU (IGPS(1)) */
	ret = uniwill_wmi_evaluate_result(UNIWILL_WMI_FUNC_IGPU_ONLY,
					  enable ? UNIWILL_WMI_DGPU_ON :
						   UNIWILL_WMI_DGPU_OFF,
					  &result);
	if (ret < 0)
		return ret;

	dev_dbg(dev, "dgpu_power %s: IGPS returned 0x%x\n",
		enable ? "on" : "off", result);

	/*
	 * IGPS returns:
	 *   0x00 = dGPU powered on successfully
	 *   0x01 = dGPU powered off successfully
	 *   0x02 = timeout waiting for GPU to enter D3
	 *   0xAA = operation failed (power gate stale or already in state)
	 *   0x55 = PCIe slot was already powered down
	 */
	if (result == 0x02)
		return -ETIMEDOUT;

	/* Verify the operation actually took effect */
	if (enable != uniwill_dgpu_is_on())
		return -EIO;

	return count;
}

static DEVICE_ATTR_RW(dgpu_power);

static ssize_t gpu_mux_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	const char *mode;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_GPU_MUX_MODE, &value);
	if (ret < 0)
		return ret;

	switch (value) {
	case GPU_MUX_MODE_HYBRID:
		mode = "hybrid";
		break;
	case GPU_MUX_MODE_DGPU_DIRECT:
		mode = "dgpu_direct";
		break;
	case GPU_MUX_MODE_IGPU_ONLY:
		mode = "igpu_only";
		break;
	default:
		mode = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", mode);
}

static int uniwill_update_oem_display_mode(struct device *dev, u8 mode)
{
	efi_char16_t name[] = L"OemMagicVariable";
	unsigned long size = 0;
	efi_status_t st;
	u8 *buf = NULL;
	u32 attr = 0;
	int ret = 0;

	if (!efi_enabled(EFI_RUNTIME_SERVICES))
		return -EOPNOTSUPP;

	st = efi.get_variable(name, &oem_magic_guid, &attr, &size, NULL);
	if (st != EFI_BUFFER_TOO_SMALL) {
		dev_err(dev, "gpu_mux: get_variable probe failed: 0x%lx\n",
			(unsigned long)st);
		return -EIO;
	}

	if (size <= OEM_MAGIC_DISPLAY_MODE_OFFSET) {
		dev_err(dev, "gpu_mux: OemMagicVariable too small (%lu bytes)\n", size);
		return -EINVAL;
	}

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	st = efi.get_variable(name, &oem_magic_guid, &attr, &size, buf);
	if (st != EFI_SUCCESS) {
		dev_err(dev, "gpu_mux: get_variable read failed: 0x%lx\n",
			(unsigned long)st);
		ret = -EIO;
		goto out;
	}

	if (buf[OEM_MAGIC_DISPLAY_MODE_OFFSET] == mode)
		goto out;

	buf[OEM_MAGIC_DISPLAY_MODE_OFFSET] = mode;

	st = efi.set_variable(name, &oem_magic_guid, attr, size, buf);
	if (st != EFI_SUCCESS) {
		dev_err(dev, "gpu_mux: set_variable failed: 0x%lx\n",
			(unsigned long)st);
		ret = -EIO;
		goto out;
	}

out:
	kfree(buf);
	return ret;
}

static ssize_t gpu_mux_mode_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int mode;
	unsigned int current_mode;
	int ret;

	if (sysfs_streq(buf, "hybrid"))
		mode = GPU_MUX_MODE_HYBRID;
	else if (sysfs_streq(buf, "dgpu_direct"))
		mode = GPU_MUX_MODE_DGPU_DIRECT;
	else if (sysfs_streq(buf, "igpu_only"))
		mode = GPU_MUX_MODE_IGPU_ONLY;
	else
		return -EINVAL;

	ret = regmap_read(data->regmap, EC_ADDR_GPU_MUX_MODE, &current_mode);
	if (ret < 0)
		return ret;

	if (mode == current_mode)
		return count;

	ret = uniwill_update_oem_display_mode(dev, mode);
	if (ret < 0)
		return ret;

	dev_info(dev, "GPU MUX mode set to %s (reboot required to take effect)\n",
		 mode == GPU_MUX_MODE_HYBRID ? "hybrid" :
		 mode == GPU_MUX_MODE_DGPU_DIRECT ? "dgpu_direct" : "igpu_only");

	return count;
}

static DEVICE_ATTR_RW(gpu_mux_mode);

static struct attribute *uniwill_attrs[] = {
	/* Keyboard-related */
	&dev_attr_fn_lock.attr,
	&dev_attr_super_key_enable.attr,
	&dev_attr_touchpad_toggle_enable.attr,
	/* Lightbar-related */
	&dev_attr_rainbow_animation.attr,
	&dev_attr_breathing_in_suspend.attr,
	/* Power-management-related */
	&dev_attr_ctgp_offset.attr,
	&dev_attr_ctgp_offset_max.attr,
	&dev_attr_db_offset_max.attr,
	&dev_attr_tgp_base.attr,
	&dev_attr_dynamic_boost_enable.attr,
	&dev_attr_cpu_pl1.attr,
	&dev_attr_cpu_pl1_min.attr,
	&dev_attr_cpu_pl1_max.attr,
	&dev_attr_cpu_pl2.attr,
	&dev_attr_cpu_pl2_min.attr,
	&dev_attr_cpu_pl2_max.attr,
	&dev_attr_cpu_pl4.attr,
	&dev_attr_cpu_pl4_min.attr,
	&dev_attr_cpu_pl4_max.attr,
	&dev_attr_cpu_tcc_offset.attr,
	&dev_attr_usb_c_power_priority.attr,
	&dev_attr_ac_auto_boot.attr,
	&dev_attr_usb_powershare_high.attr,
	/* Display-related */
	&dev_attr_mini_led_local_dimming.attr,
	&dev_attr_vrm_current_limit.attr,
	&dev_attr_vrm_max_current_limit.attr,
	&dev_attr_fan_switch_speed.attr,
	/* GPU-related */
	&dev_attr_dgpu_power.attr,
	&dev_attr_gpu_mux_mode.attr,
	NULL
};

static umode_t uniwill_attr_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct uniwill_data *data = dev_get_drvdata(dev);

	if (attr == &dev_attr_fn_lock.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_FN_LOCK))
			return attr->mode;
	}

	if (attr == &dev_attr_super_key_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY))
			return attr->mode;
	}

	if (attr == &dev_attr_touchpad_toggle_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_TOUCHPAD_TOGGLE))
			return attr->mode;
	}

	if (attr == &dev_attr_rainbow_animation.attr ||
	    attr == &dev_attr_breathing_in_suspend.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_LIGHTBAR))
			return attr->mode;
	}

	if (attr == &dev_attr_ctgp_offset.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
			return attr->mode;
	}

	if (attr == &dev_attr_ctgp_offset_max.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL) &&
		    data->ctgp_max)
			return attr->mode;
	}

	if (attr == &dev_attr_db_offset_max.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL) &&
		    data->db_max)
			return attr->mode;
	}

	if (attr == &dev_attr_tgp_base.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL) &&
		    data->tgp_base)
			return attr->mode;
	}

	if (attr == &dev_attr_dynamic_boost_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
			return attr->mode;
	}

	if (attr == &dev_attr_cpu_pl1.attr ||
	    attr == &dev_attr_cpu_pl1_min.attr ||
	    attr == &dev_attr_cpu_pl1_max.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL) &&
		    data->tdp_max[0])
			return attr->mode;
	}

	if (attr == &dev_attr_cpu_pl2.attr ||
	    attr == &dev_attr_cpu_pl2_min.attr ||
	    attr == &dev_attr_cpu_pl2_max.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL) &&
		    data->tdp_max[1])
			return attr->mode;
	}

	if (attr == &dev_attr_cpu_pl4.attr ||
	    attr == &dev_attr_cpu_pl4_min.attr ||
	    attr == &dev_attr_cpu_pl4_max.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL) &&
		    data->tdp_max[2])
			return attr->mode;
	}

	if (attr == &dev_attr_cpu_tcc_offset.attr) {
		if (data->has_tcc_offset)
			return attr->mode;
	}

	if (attr == &dev_attr_usb_c_power_priority.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_USB_C_POWER_PRIORITY))
			return attr->mode;
	}

	if (attr == &dev_attr_ac_auto_boot.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_AC_AUTO_BOOT))
			return attr->mode;
	}

	if (attr == &dev_attr_usb_powershare_high.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_USB_POWERSHARE))
			return attr->mode;
	}

	if (attr == &dev_attr_mini_led_local_dimming.attr) {
		if (data->has_mini_led_dimming)
			return attr->mode;
	}

	if (attr == &dev_attr_vrm_current_limit.attr ||
	    attr == &dev_attr_vrm_max_current_limit.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL))
			return attr->mode;
	}

	if (attr == &dev_attr_fan_switch_speed.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_PRIMARY_FAN))
			return attr->mode;
	}

	if (attr == &dev_attr_dgpu_power.attr) {
		if (data->has_dgpu_power)
			return attr->mode;
	}

	if (attr == &dev_attr_gpu_mux_mode.attr) {
		if (data->has_gpu_mux)
			return attr->mode;
	}

	return 0;
}

static const struct attribute_group uniwill_group = {
	.is_visible = uniwill_attr_is_visible,
	.attrs = uniwill_attrs,
};

/* Water cooler sysfs bridge attributes */

static ssize_t wc_fan_rpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int rpm;

	mutex_lock(&data->wc.lock);
	rpm = data->wc.fan_rpm;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", rpm);
}

static ssize_t wc_fan_rpm_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int rpm;

	if (kstrtouint(buf, 10, &rpm))
		return -EINVAL;

	mutex_lock(&data->wc.lock);
	data->wc.fan_rpm = rpm;
	data->wc.last_update = jiffies;
	mutex_unlock(&data->wc.lock);
	return count;
}

static ssize_t wc_pump_rpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int rpm;

	mutex_lock(&data->wc.lock);
	rpm = data->wc.pump_rpm;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", rpm);
}

static ssize_t wc_pump_rpm_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int rpm;

	if (kstrtouint(buf, 10, &rpm))
		return -EINVAL;

	mutex_lock(&data->wc.lock);
	data->wc.pump_rpm = rpm;
	data->wc.last_update = jiffies;
	mutex_unlock(&data->wc.lock);
	return count;
}

static ssize_t wc_fan_pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 pwm;

	mutex_lock(&data->wc.lock);
	pwm = data->wc.fan_pwm;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", pwm);
}

static ssize_t wc_fan_pwm_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int pwm;

	if (kstrtouint(buf, 10, &pwm) || pwm > U8_MAX)
		return -EINVAL;

	mutex_lock(&data->wc.lock);
	data->wc.fan_pwm = pwm;
	data->wc.last_update = jiffies;
	mutex_unlock(&data->wc.lock);
	return count;
}

static ssize_t wc_pump_pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 pwm;

	mutex_lock(&data->wc.lock);
	pwm = data->wc.pump_pwm;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", pwm);
}

static ssize_t wc_pump_pwm_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int pwm;

	if (kstrtouint(buf, 10, &pwm) || pwm > U8_MAX)
		return -EINVAL;

	mutex_lock(&data->wc.lock);
	data->wc.pump_pwm = pwm;
	data->wc.last_update = jiffies;
	mutex_unlock(&data->wc.lock);
	return count;
}

static ssize_t wc_fan_pwm_target_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 target;

	mutex_lock(&data->wc.lock);
	target = data->wc.fan_pwm_target;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", target);
}

static ssize_t wc_pump_pwm_target_show(struct device *dev, struct device_attribute *attr,
				       char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 target;

	mutex_lock(&data->wc.lock);
	target = data->wc.pump_pwm_target;
	mutex_unlock(&data->wc.lock);
	return sysfs_emit(buf, "%u\n", target);
}

static ssize_t wc_fan_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", data->wc.fan_mode);
}

static DEVICE_ATTR_RW(wc_fan_rpm);
static DEVICE_ATTR_RW(wc_pump_rpm);
static DEVICE_ATTR_RW(wc_fan_pwm);
static DEVICE_ATTR_RW(wc_pump_pwm);
static DEVICE_ATTR_RO(wc_fan_pwm_target);
static DEVICE_ATTR_RO(wc_pump_pwm_target);
static DEVICE_ATTR_RO(wc_fan_mode);

static struct attribute *uniwill_wc_attrs[] = {
	&dev_attr_wc_fan_rpm.attr,
	&dev_attr_wc_pump_rpm.attr,
	&dev_attr_wc_fan_pwm.attr,
	&dev_attr_wc_pump_pwm.attr,
	&dev_attr_wc_fan_pwm_target.attr,
	&dev_attr_wc_pump_pwm_target.attr,
	&dev_attr_wc_fan_mode.attr,
	NULL
};

static umode_t uniwill_wc_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct uniwill_data *data = dev_get_drvdata(dev);

	if (uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER))
		return attr->mode;

	return 0;
}

static const struct attribute_group uniwill_wc_group = {
	.name = "water_cooler",
	.is_visible = uniwill_wc_is_visible,
	.attrs = uniwill_wc_attrs,
};

static const struct attribute_group *uniwill_groups[] = {
	&uniwill_group,
	&uniwill_wc_group,
	NULL
};

static umode_t uniwill_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	const struct uniwill_data *data = drvdata;
	unsigned int feature;

	switch (type) {
	case hwmon_temp:
		switch (channel) {
		case 0:
			feature = UNIWILL_FEATURE_CPU_TEMP;
			break;
		case 1:
			feature = UNIWILL_FEATURE_GPU_TEMP;
			break;
		default:
			return 0;
		}

		if (!uniwill_device_supports(data, feature))
			return 0;

		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
		case hwmon_temp_max:
			return 0444;
		default:
			return 0;
		}
	case hwmon_fan:
		switch (channel) {
		case 0:
			feature = UNIWILL_FEATURE_PRIMARY_FAN;
			break;
		case 1:
			feature = UNIWILL_FEATURE_SECONDARY_FAN;
			break;
		case 2:
		case 3:
			feature = UNIWILL_FEATURE_WATER_COOLER;
			break;
		default:
			return 0;
		}
		break;
	case hwmon_pwm:
		switch (channel) {
		case 0:
			feature = UNIWILL_FEATURE_PRIMARY_FAN;
			break;
		case 1:
			feature = UNIWILL_FEATURE_SECONDARY_FAN;
			break;
		case 2:
		case 3:
			feature = UNIWILL_FEATURE_WATER_COOLER;
			break;
		default:
			return 0;
		}

		if (!uniwill_device_supports(data, feature))
			return 0;

		switch (attr) {
		case hwmon_pwm_enable:
			return 0644;
		case hwmon_pwm_input:
			if (data->has_universal_fan_ctrl ||
			    channel >= 2)
				return 0644;
			return 0444;
		default:
			return 0;
		}
	case hwmon_power:
		switch (channel) {
		case 0: /* System power — needs GPU (discrete GPU implies full power monitoring) */
			feature = UNIWILL_FEATURE_GPU_TEMP;
			break;
		case 1: /* GPU power allocation */
			feature = UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL;
			break;
		case 2: /* Thermal budget */
			feature = UNIWILL_FEATURE_GPU_TEMP;
			break;
		default:
			return 0;
		}

		if (uniwill_device_supports(data, feature))
			return 0444;

		return 0;
	case hwmon_curr:
		switch (channel) {
		case 0: /* Adapter current */
			feature = UNIWILL_FEATURE_GPU_TEMP;
			break;
		default:
			return 0;
		}

		if (uniwill_device_supports(data, feature))
			return 0444;

		return 0;
	default:
		return 0;
	}

	if (uniwill_device_supports(data, feature))
		return 0444;

	return 0;
}

static int uniwill_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			long *val)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value, value_hi;
	__be16 rpm;
	int ret;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			switch (channel) {
			case 0:
				ret = regmap_read(data->regmap, EC_ADDR_CPU_TEMP, &value);
				break;
			case 1:
				ret = regmap_read(data->regmap, EC_ADDR_GPU_TEMP, &value);
				break;
			default:
				return -EOPNOTSUPP;
			}

			if (ret < 0)
				return ret;

			*val = value * MILLIDEGREE_PER_DEGREE;
			return 0;
		case hwmon_temp_max:
			if (channel != 0)
				return -EOPNOTSUPP;

			ret = regmap_read(data->regmap, EC_ADDR_CPU_TEMP_LIMIT, &value);
			if (ret < 0)
				return ret;

			*val = (long)(TEMP_LIMIT_TJMAX - value) * MILLIDEGREE_PER_DEGREE;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_fan:
		switch (channel) {
		case 0:
			ret = regmap_bulk_read(data->regmap, EC_ADDR_MAIN_FAN_RPM_1, &rpm,
					       sizeof(rpm));
			break;
		case 1:
			ret = regmap_bulk_read(data->regmap, EC_ADDR_SECOND_FAN_RPM_1, &rpm,
					       sizeof(rpm));
			break;
		case 2:
		case 3:
			mutex_lock(&data->wc.lock);
			if (data->wc.last_update == 0 ||
			    time_after(jiffies, data->wc.last_update +
				       msecs_to_jiffies(WC_STALE_TIMEOUT_MS)))
				*val = 0;
			else
				*val = (channel == 2) ? data->wc.fan_rpm
						      : data->wc.pump_rpm;
			mutex_unlock(&data->wc.lock);
			return 0;
		default:
			return -EOPNOTSUPP;
		}

		if (ret < 0)
			return ret;

		*val = be16_to_cpu(rpm);
		return 0;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			if (channel >= 2) {
				*val = data->wc.fan_mode;
				return 0;
			}
			*val = data->fan_mode;
			return 0;
		case hwmon_pwm_input:
			if (channel >= 2) {
				mutex_lock(&data->wc.lock);
				*val = (channel == 2) ? data->wc.fan_pwm
						      : data->wc.pump_pwm;
				mutex_unlock(&data->wc.lock);
				return 0;
			}
			/*
			 * When boost is active, the EC reports 0xFF in the
			 * PWM status register. Return the last value written
			 * by the user instead. When manual mode is set but
			 * boost isn't active yet, read the real EC value.
			 */
			if (data->fan_mode == 1 && data->boost_active) {
				*val = fixp_linear_interpolate(0, 0, PWM_MAX,
							      U8_MAX,
							      data->last_fan_pwm[channel]);
				return 0;
			}

			switch (channel) {
			case 0:
				ret = regmap_read(data->regmap, EC_ADDR_PWM_1, &value);
				break;
			case 1:
				ret = regmap_read(data->regmap, EC_ADDR_PWM_2, &value);
				break;
			default:
				return -EOPNOTSUPP;
			}

			if (ret < 0)
				return ret;

			*val = fixp_linear_interpolate(0, 0, PWM_MAX, U8_MAX, value);
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_power:
		switch (channel) {
		case 0: /* System total power (16-bit LE, watts) */
			ret = regmap_read(data->regmap, EC_ADDR_SYSTEM_POWER_LO, &value);
			if (ret < 0)
				return ret;

			ret = regmap_read(data->regmap, EC_ADDR_SYSTEM_POWER_HI, &value_hi);
			if (ret < 0)
				return ret;

			/* hwmon power is in microwatts */
			*val = (long)((value_hi << 8) | value) * 1000000;
			return 0;
		case 1: /* GPU power allocation (raw × 8 = watts) */
			ret = regmap_read(data->regmap, EC_ADDR_GPU_POWER_ALLOC, &value);
			if (ret < 0)
				return ret;

			*val = (long)value * 8 * 1000000;
			return 0;
		case 2: /* Thermal budget remaining (raw × 8 = watts) */
			ret = regmap_read(data->regmap, EC_ADDR_THERMAL_BUDGET, &value);
			if (ret < 0)
				return ret;

			*val = (long)value * 8 * 1000000;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_curr:
		switch (channel) {
		case 0: /* Adapter current (raw ÷ 10 = amps) */
			ret = regmap_read(data->regmap, EC_ADDR_ADAPTER_CURRENT, &value);
			if (ret < 0)
				return ret;

			/* hwmon current is in milliamps; raw ÷ 10 = amps → raw × 100 = mA */
			*val = (long)value * 100;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int uniwill_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = uniwill_temp_labels[channel];
		return 0;
	case hwmon_fan:
		*str = uniwill_fan_labels[channel];
		return 0;
	case hwmon_power:
		*str = uniwill_power_labels[channel];
		return 0;
	case hwmon_curr:
		*str = uniwill_curr_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

#define FAN_TABLE_ZONES		16
#define FAN_TABLE_MAX_TEMP	115
#define FAN_TABLE_MAX_SPEED	0xC8	/* 200 = full speed */

static int uniwill_fan_init_tables(struct uniwill_data *data)
{
	unsigned int cpu_speed, gpu_speed;
	int i, ret;

	/*
	 * Initialize custom fan tables with a single controllable zone spanning
	 * from 0 to 115 degrees, and fill the remaining zones with safety dummy
	 * entries at increasing temperatures beyond the controllable range.
	 *
	 * For zone 0 (the controllable zone), preserve the current fan speed
	 * read from the EC so that switching to manual mode does not cause an
	 * unexpected speed change. Fall back to full speed if the read fails.
	 */
	if (regmap_read(data->regmap, EC_ADDR_PWM_1, &cpu_speed) < 0)
		cpu_speed = FAN_TABLE_MAX_SPEED;
	else
		cpu_speed = min(cpu_speed, (unsigned int)FAN_TABLE_MAX_SPEED);

	if (regmap_read(data->regmap, EC_ADDR_PWM_2, &gpu_speed) < 0)
		gpu_speed = FAN_TABLE_MAX_SPEED;
	else
		gpu_speed = min(gpu_speed, (unsigned int)FAN_TABLE_MAX_SPEED);

	data->last_fan_pwm[0] = cpu_speed;
	data->last_fan_pwm[1] = gpu_speed;

	ret = regmap_write(data->regmap, EC_ADDR_CPU_TEMP_END_TABLE, FAN_TABLE_MAX_TEMP);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CPU_TEMP_START_TABLE, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CPU_FAN_SPEED_TABLE, cpu_speed);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_GPU_TEMP_END_TABLE, FAN_TABLE_MAX_TEMP + 5);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_GPU_TEMP_START_TABLE, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_GPU_FAN_SPEED_TABLE, gpu_speed);
	if (ret < 0)
		return ret;

	for (i = 1; i < FAN_TABLE_ZONES; i++) {
		ret = regmap_write(data->regmap,
				   EC_ADDR_CPU_TEMP_END_TABLE + i,
				   FAN_TABLE_MAX_TEMP + i + 1);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap,
				   EC_ADDR_CPU_TEMP_START_TABLE + i,
				   FAN_TABLE_MAX_TEMP + i);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap,
				   EC_ADDR_CPU_FAN_SPEED_TABLE + i,
				   FAN_TABLE_MAX_SPEED);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap,
				   EC_ADDR_GPU_TEMP_END_TABLE + i,
				   FAN_TABLE_MAX_TEMP + i + 1);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap,
				   EC_ADDR_GPU_TEMP_START_TABLE + i,
				   FAN_TABLE_MAX_TEMP + i);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap,
				   EC_ADDR_GPU_FAN_SPEED_TABLE + i,
				   FAN_TABLE_MAX_SPEED);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int uniwill_fan_enable_custom_tables(struct uniwill_data *data)
{
	int ret;

	/* Enable split tables so CPU and GPU fans use separate curves */
	ret = regmap_set_bits(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL,
			      SPLIT_TABLES);
	if (ret < 0)
		return ret;

	/* Enable custom fan tables */
	return regmap_set_bits(data->regmap, EC_ADDR_AP_OEM_6,
			       ENABLE_UNIVERSAL_FAN_CTRL);
}

static int uniwill_fan_disable_custom_tables(struct uniwill_data *data)
{
	int ret;

	/* Disable custom fan tables */
	ret = regmap_clear_bits(data->regmap, EC_ADDR_AP_OEM_6,
				ENABLE_UNIVERSAL_FAN_CTRL);
	if (ret < 0)
		return ret;

	/* Disable split tables */
	return regmap_clear_bits(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL,
				 SPLIT_TABLES);
}

static int uniwill_set_fan_mode(struct uniwill_data *data, long mode)
{
	int ret;

	switch (mode) {
	case 0:	/* Full speed */
		ret = regmap_set_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
				      FAN_MODE_BOOST);
		if (ret < 0)
			return ret;

		data->boost_active = true;
		data->fan_mode = 0;
		return 0;
	case 1:	/* Manual */
		if (!data->has_universal_fan_ctrl)
			return -EOPNOTSUPP;

		/*
		 * Don't enable boost mode here. The EC briefly ramps fans
		 * to full speed on boost entry. Defer boost activation to
		 * the first PWM write where we have the desired speed to
		 * immediately counter the ramp-up via burst writes.
		 */
		data->fan_mode = 1;
		return 0;
	case 2:	/* Automatic */
		ret = regmap_clear_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
					FAN_MODE_BOOST);
		if (ret < 0)
			return ret;

		data->boost_active = false;
		data->fan_mode = 2;
		return 0;
	default:
		return -EINVAL;
	}
}

static int uniwill_set_pwm(struct uniwill_data *data, int channel, long val)
{
	unsigned int ec_val, fan_min;
	int ret, i;

	if (data->fan_mode != 1)
		return -EBUSY;

	if (val < 0 || val > U8_MAX)
		return -EINVAL;

	ec_val = fixp_linear_interpolate(0, 0, U8_MAX, PWM_MAX, val);

	/*
	 * Enforce minimum fan-on speed. Values below half the minimum
	 * threshold are treated as fan-off (0); values between half-min
	 * and min are snapped up to the minimum on-speed to prevent
	 * the fan from stalling.
	 */
	fan_min = FAN_ON_MIN_SPEED_PERCENT * PWM_MAX / 100;
	if (ec_val < fan_min / 2)
		ec_val = 0;
	else if (ec_val < fan_min)
		ec_val = fan_min;

	data->last_fan_pwm[channel] = ec_val;

	if (!data->boost_active) {
		/*
		 * Enable boost mode so direct writes to the fan speed
		 * registers (0x1804/0x1809) are respected by the EC.
		 *
		 * The EC briefly ramps fans to full speed on boost
		 * entry, overriding a single write. Repeat the write
		 * until the EC settles.
		 */
		ret = regmap_set_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
				      FAN_MODE_BOOST);
		if (ret < 0)
			return ret;

		for (i = 0; i < 10; i++) {
			uniwill_wmi_ec_write(EC_ADDR_PWM_1_WRITEABLE,
					     data->last_fan_pwm[0]);
			uniwill_wmi_ec_write(EC_ADDR_PWM_2_WRITEABLE,
					     data->last_fan_pwm[1]);
			msleep(10);
		}

		data->boost_active = true;
		return 0;
	}

	/*
	 * Write fan speed via WMI since these registers are not
	 * reachable through ACPI ECRW.
	 */
	switch (channel) {
	case 0:
		return uniwill_wmi_ec_write(EC_ADDR_PWM_1_WRITEABLE, ec_val);
	case 1:
		return uniwill_wmi_ec_write(EC_ADDR_PWM_2_WRITEABLE, ec_val);
	default:
		return -EINVAL;
	}
}

static int uniwill_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			 int channel, long val)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			if (channel >= 2) {
				if (val < 0 || val > 2)
					return -EINVAL;
				data->wc.fan_mode = val;
				sysfs_notify(&data->dev->kobj, "water_cooler",
					     "fan_mode");
				return 0;
			}
			return uniwill_set_fan_mode(data, val);
		case hwmon_pwm_input:
			if (channel >= 2) {
				if (val < 0 || val > U8_MAX)
					return -EINVAL;
				mutex_lock(&data->wc.lock);
				if (channel == 2)
					data->wc.fan_pwm_target = val;
				else
					data->wc.pump_pwm_target = val;
				mutex_unlock(&data->wc.lock);
				sysfs_notify(&data->dev->kobj, "water_cooler",
					     (channel == 2) ? "fan_pwm_target"
							    : "pump_pwm_target");
				return 0;
			}
			return uniwill_set_pwm(data, channel, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static const unsigned int fan_speed_table_base[2] = {
	EC_ADDR_CPU_FAN_SPEED_TABLE,
	EC_ADDR_GPU_FAN_SPEED_TABLE,
};

static const unsigned int fan_temp_end_table_base[2] = {
	EC_ADDR_CPU_TEMP_END_TABLE,
	EC_ADDR_GPU_TEMP_END_TABLE,
};

static const unsigned int fan_temp_start_table_base[2] = {
	EC_ADDR_CPU_TEMP_START_TABLE,
	EC_ADDR_GPU_TEMP_START_TABLE,
};

/*
 * Fan curve auto_point attributes.
 *
 * Each of the FAN_TABLE_LENGTH zones is described by three attributes:
 *   pwmN_auto_pointM_pwm       - target fan speed at this zone (0-255 hwmon
 *                                scale)
 *   pwmN_auto_pointM_temp      - upper temperature threshold (millidegrees
 *                                Celsius)
 *   pwmN_auto_pointM_temp_hyst - lower temperature threshold (millidegrees
 *                                Celsius)
 *
 * attr->nr encodes the fan channel (0 = primary, 1 = secondary).
 * attr->index encodes the zone index (0 .. FAN_TABLE_LENGTH - 1).
 */
static ssize_t uniwill_auto_point_pwm_show(struct device *dev,
					   struct device_attribute *devattr,
					   char *buf)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(data->regmap,
			  fan_speed_table_base[attr->nr] + attr->index,
			  &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n",
			  fixp_linear_interpolate(0, 0, PWM_MAX, U8_MAX, val));
}

static ssize_t uniwill_auto_point_pwm_store(struct device *dev,
					    struct device_attribute *devattr,
					    const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val > U8_MAX)
		return -EINVAL;

	ret = regmap_write(data->regmap,
			   fan_speed_table_base[attr->nr] + attr->index,
			   fixp_linear_interpolate(0, 0, U8_MAX, PWM_MAX, val));
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t uniwill_auto_point_temp_show(struct device *dev,
					    struct device_attribute *devattr,
					    char *buf)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(data->regmap,
			  fan_temp_end_table_base[attr->nr] + attr->index,
			  &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", val * MILLIDEGREE_PER_DEGREE);
}

static ssize_t uniwill_auto_point_temp_store(struct device *dev,
					     struct device_attribute *devattr,
					     const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	val = DIV_ROUND_CLOSEST(val, MILLIDEGREE_PER_DEGREE);
	if (val > U8_MAX)
		return -EINVAL;

	ret = regmap_write(data->regmap,
			   fan_temp_end_table_base[attr->nr] + attr->index,
			   val);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t uniwill_auto_point_temp_hyst_show(struct device *dev,
						 struct device_attribute *devattr,
						 char *buf)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(data->regmap,
			  fan_temp_start_table_base[attr->nr] + attr->index,
			  &val);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", val * MILLIDEGREE_PER_DEGREE);
}

static ssize_t uniwill_auto_point_temp_hyst_store(struct device *dev,
						  struct device_attribute *devattr,
						  const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *attr = to_sensor_dev_attr_2(devattr);
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	val = DIV_ROUND_CLOSEST(val, MILLIDEGREE_PER_DEGREE);
	if (val > U8_MAX)
		return -EINVAL;

	ret = regmap_write(data->regmap,
			   fan_temp_start_table_base[attr->nr] + attr->index,
			   val);
	if (ret < 0)
		return ret;

	return count;
}

/*
 * Macro generating the three SENSOR_DEVICE_ATTR_2 declarations for one
 * (fan, zone) pair.  _fn is 1-based (matches the sysfs "pwmN" index),
 * _zn is 1-based (matches the sysfs "auto_pointM" index).
 */
#define UNIWILL_AUTO_POINT_ATTRS(_fn, _zn)					\
static SENSOR_DEVICE_ATTR_2_RW(pwm##_fn##_auto_point##_zn##_pwm,		\
			       uniwill_auto_point_pwm, (_fn) - 1, (_zn) - 1);	\
static SENSOR_DEVICE_ATTR_2_RW(pwm##_fn##_auto_point##_zn##_temp,		\
			       uniwill_auto_point_temp, (_fn) - 1, (_zn) - 1);	\
static SENSOR_DEVICE_ATTR_2_RW(pwm##_fn##_auto_point##_zn##_temp_hyst,		\
			       uniwill_auto_point_temp_hyst, (_fn) - 1, (_zn) - 1)

/* Primary fan (pwm1) zones */
UNIWILL_AUTO_POINT_ATTRS(1,  1);
UNIWILL_AUTO_POINT_ATTRS(1,  2);
UNIWILL_AUTO_POINT_ATTRS(1,  3);
UNIWILL_AUTO_POINT_ATTRS(1,  4);
UNIWILL_AUTO_POINT_ATTRS(1,  5);
UNIWILL_AUTO_POINT_ATTRS(1,  6);
UNIWILL_AUTO_POINT_ATTRS(1,  7);
UNIWILL_AUTO_POINT_ATTRS(1,  8);
UNIWILL_AUTO_POINT_ATTRS(1,  9);
UNIWILL_AUTO_POINT_ATTRS(1, 10);
UNIWILL_AUTO_POINT_ATTRS(1, 11);
UNIWILL_AUTO_POINT_ATTRS(1, 12);
UNIWILL_AUTO_POINT_ATTRS(1, 13);
UNIWILL_AUTO_POINT_ATTRS(1, 14);
UNIWILL_AUTO_POINT_ATTRS(1, 15);
UNIWILL_AUTO_POINT_ATTRS(1, 16);

/* Secondary fan (pwm2) zones */
UNIWILL_AUTO_POINT_ATTRS(2,  1);
UNIWILL_AUTO_POINT_ATTRS(2,  2);
UNIWILL_AUTO_POINT_ATTRS(2,  3);
UNIWILL_AUTO_POINT_ATTRS(2,  4);
UNIWILL_AUTO_POINT_ATTRS(2,  5);
UNIWILL_AUTO_POINT_ATTRS(2,  6);
UNIWILL_AUTO_POINT_ATTRS(2,  7);
UNIWILL_AUTO_POINT_ATTRS(2,  8);
UNIWILL_AUTO_POINT_ATTRS(2,  9);
UNIWILL_AUTO_POINT_ATTRS(2, 10);
UNIWILL_AUTO_POINT_ATTRS(2, 11);
UNIWILL_AUTO_POINT_ATTRS(2, 12);
UNIWILL_AUTO_POINT_ATTRS(2, 13);
UNIWILL_AUTO_POINT_ATTRS(2, 14);
UNIWILL_AUTO_POINT_ATTRS(2, 15);
UNIWILL_AUTO_POINT_ATTRS(2, 16);

#define UNIWILL_AUTO_POINT_ATTRS_REF(_fn, _zn)					\
	&sensor_dev_attr_pwm##_fn##_auto_point##_zn##_pwm.dev_attr.attr,	\
	&sensor_dev_attr_pwm##_fn##_auto_point##_zn##_temp.dev_attr.attr,	\
	&sensor_dev_attr_pwm##_fn##_auto_point##_zn##_temp_hyst.dev_attr.attr

static struct attribute *uniwill_auto_point_attrs[] = {
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  1),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  2),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  3),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  4),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  5),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  6),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  7),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  8),
	UNIWILL_AUTO_POINT_ATTRS_REF(1,  9),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 10),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 11),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 12),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 13),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 14),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 15),
	UNIWILL_AUTO_POINT_ATTRS_REF(1, 16),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  1),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  2),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  3),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  4),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  5),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  6),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  7),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  8),
	UNIWILL_AUTO_POINT_ATTRS_REF(2,  9),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 10),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 11),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 12),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 13),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 14),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 15),
	UNIWILL_AUTO_POINT_ATTRS_REF(2, 16),
	NULL
};

static umode_t uniwill_auto_point_is_visible(struct kobject *kobj,
					     struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct uniwill_data *data = dev_get_drvdata(dev);
	const struct sensor_device_attribute_2 *sda =
		container_of(attr, struct sensor_device_attribute_2, dev_attr.attr);
	unsigned int fan_feature;

	if (!data->has_universal_fan_ctrl)
		return 0;

	switch (sda->nr) {
	case 0:
		fan_feature = UNIWILL_FEATURE_PRIMARY_FAN;
		break;
	case 1:
		fan_feature = UNIWILL_FEATURE_SECONDARY_FAN;
		break;
	default:
		return 0;
	}

	if (uniwill_device_supports(data, fan_feature))
		return attr->mode;

	return 0;
}

static const struct attribute_group uniwill_auto_point_group = {
	.is_visible = uniwill_auto_point_is_visible,
	.attrs	    = uniwill_auto_point_attrs,
};

static const struct attribute_group *uniwill_auto_point_groups[] = {
	&uniwill_auto_point_group,
	NULL,
};

static const struct hwmon_ops uniwill_ops = {
	.is_visible = uniwill_is_visible,
	.read = uniwill_read,
	.read_string = uniwill_read_string,
	.write = uniwill_write,
};

static const struct hwmon_channel_info * const uniwill_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info uniwill_chip_info = {
	.ops = &uniwill_ops,
	.info = uniwill_info,
};

static const struct hwmon_channel_info * const uniwill_info_wc[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info uniwill_chip_info_wc = {
	.ops = &uniwill_ops,
	.info = uniwill_info_wc,
};

static int uniwill_hwmon_init(struct uniwill_data *data)
{
	const struct hwmon_chip_info *chip_info;
	struct device *hdev;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TEMP) &&
	    !uniwill_device_supports(data, UNIWILL_FEATURE_GPU_TEMP) &&
	    !uniwill_device_supports(data, UNIWILL_FEATURE_PRIMARY_FAN) &&
	    !uniwill_device_supports(data, UNIWILL_FEATURE_SECONDARY_FAN) &&
	    !uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER))
		return 0;

	if (uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER))
		chip_info = &uniwill_chip_info_wc;
	else
		chip_info = &uniwill_chip_info;

	hdev = devm_hwmon_device_register_with_info(data->dev, "uniwill", data,
						    chip_info,
						    uniwill_auto_point_groups);

	return PTR_ERR_OR_ZERO(hdev);
}

static bool uniwill_has_mini_led_dimming(struct uniwill_data *data)
{
	unsigned int value;

	if (!wmi_has_guid(UNIWILL_WMI_MGMT_GUID_BC))
		return false;

	if (regmap_read(data->regmap, EC_ADDR_MINI_LED_SUPPORT, &value) < 0)
		return false;

	return value != 0xFF && (value & BIT(0));
}

static bool uniwill_has_universal_fan_ctrl(struct uniwill_data *data)
{
	unsigned int value;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_PRIMARY_FAN))
		return false;

	if (regmap_read(data->regmap, EC_ADDR_FAN_CTRL, &value) < 0)
		return false;

	return !!(value & UNIVERSAL_FAN_CTRL);
}

/*
 * Check if the firmware supports runtime dGPU power control (iGPU-only mode).
 * Uses WMI WMBC method 0x04 with function 3 (SAC1=0x0300), sub-command 3.
 * The firmware checks GFID != 0 (discrete GPU present) and IGPM == 1
 * (iGPU-only power management enabled) before returning 0x55 (supported).
 */
static bool uniwill_has_dgpu_power(void)
{
	u32 result;
	int ret;

	if (!wmi_has_guid(UNIWILL_WMI_MGMT_GUID_BC))
		return false;

	ret = uniwill_wmi_evaluate_result(UNIWILL_WMI_FUNC_IGPU_ONLY,
					  UNIWILL_WMI_DGPU_SUPPORT, &result);
	if (ret < 0)
		return false;

	return result == UNIWILL_WMI_DGPU_POWER_ON;
}

/*
 * Check if the firmware supports GPU MUX switching by reading the MUX mode
 * register. A valid value (0 = hybrid, 1 = dGPU direct) indicates support.
 * Systems without a MUX typically have 0xFF or don't have a dGPU at all.
 */
static bool uniwill_has_gpu_mux(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_GPU_TEMP))
		return false;

	ret = regmap_read(data->regmap, EC_ADDR_GPU_MUX_MODE, &value);
	if (ret < 0)
		return false;

	return value == GPU_MUX_MODE_HYBRID ||
	       value == GPU_MUX_MODE_DGPU_DIRECT ||
	       value == GPU_MUX_MODE_IGPU_ONLY;
}

static int uniwill_mini_led_init(struct uniwill_data *data)
{
	int ret;

	if (!data->has_mini_led_dimming)
		return 0;

	/* Set default to off */
	ret = uniwill_wmi_evaluate(UNIWILL_WMI_FUNC_FEATURE_TOGGLE,
				   UNIWILL_WMI_LOCAL_DIMMING_OFF);
	if (ret < 0)
		return ret;

	data->mini_led_dimming_state = false;

	return 0;
}

/*
 * Enable or disable overboost mode by setting the OVERBOOST bit in the
 * EC OEM_3 register and raising the VRM current limit to the maximum.
 * This unlocks higher CPU power envelopes (e.g. 160W sustained with
 * external liquid cooling vs 135W on air).
 */
static int uniwill_set_overboost(struct uniwill_data *data, bool enable)
{
	int ret;
#ifdef UNIWILL_ENABLE_VRM_OVERRIDE
	unsigned int vrm_max;
#endif

	ret = regmap_update_bits(data->regmap, EC_ADDR_OEM_3, OVERBOOST,
				 enable ? OVERBOOST : 0);
	if (ret < 0)
		return ret;

#ifdef UNIWILL_ENABLE_VRM_OVERRIDE
	if (enable) {
		/* Save current VRM limit and raise to maximum */
		ret = regmap_read(data->regmap, EC_ADDR_VRM_CURRENT_LIMIT,
				  &data->vrm_saved);
		if (ret < 0)
			return ret;

		ret = regmap_read(data->regmap, EC_ADDR_VRM_MAX_CURRENT_LIMIT,
				  &vrm_max);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap, EC_ADDR_VRM_CURRENT_LIMIT,
				   vrm_max);
		if (ret < 0)
			return ret;
	} else {
		/* Restore previous VRM current limit */
		ret = regmap_write(data->regmap, EC_ADDR_VRM_CURRENT_LIMIT,
				   data->vrm_saved);
		if (ret < 0)
			return ret;
	}
#endif

	data->overboost_active = enable;
	return 0;
}

static int uniwill_profile_probe(void *drvdata, unsigned long *choices)
{
	struct uniwill_data *data = drvdata;

	set_bit(PLATFORM_PROFILE_QUIET, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);

	if (data->num_profiles >= 3)
		set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	if (data->num_profiles >= 3 &&
	    uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER))
		set_bit(PLATFORM_PROFILE_MAX_POWER, choices);

	return 0;
}

static int uniwill_profile_get(struct device *dev, enum platform_profile_option *profile)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_MANUAL_FAN_CTRL, &value);
	if (ret < 0)
		return ret;

	switch (value & PROFILE_MODE_MASK) {
	case PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		return 0;
	case PROFILE_PERFORMANCE:
		if (data->overboost_active)
			*profile = PLATFORM_PROFILE_MAX_POWER;
		else
			*profile = PLATFORM_PROFILE_PERFORMANCE;
		return 0;
	default:
		*profile = PLATFORM_PROFILE_BALANCED;
		return 0;
	}
}

static int uniwill_write_pl_values(struct uniwill_data *data, int profile_idx)
{
	unsigned int pl4_val;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL))
		return 0;

	ret = regmap_write(data->regmap, EC_ADDR_PL1_SETTING,
			   data->tdp_defaults[profile_idx][0]);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_PL2_SETTING,
			   data->tdp_defaults[profile_idx][1]);
	if (ret < 0)
		return ret;

	/*
	 * Some devices store half the PL4 value in the EC register.
	 * Apply the halving when writing profile defaults.
	 */
	pl4_val = data->tdp_defaults[profile_idx][2];
	if (data->has_double_pl4 && pl4_val)
		pl4_val /= 2;

	return regmap_write(data->regmap, EC_ADDR_PL4_SETTING, pl4_val);
}

static int uniwill_profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int profile_idx;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		value = PROFILE_QUIET;
		profile_idx = 0;
		break;
	case PLATFORM_PROFILE_BALANCED:
		value = PROFILE_BALANCED;
		profile_idx = 1;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		value = PROFILE_PERFORMANCE;
		profile_idx = 2;
		break;
	case PLATFORM_PROFILE_MAX_POWER:
		value = PROFILE_PERFORMANCE;
		profile_idx = 3;
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Disable overboost when leaving max-power profile */
	if (data->overboost_active && profile != PLATFORM_PROFILE_MAX_POWER) {
		ret = uniwill_set_overboost(data, false);
		if (ret < 0)
			return ret;
	}

	/*
	 * Set the fan/profile mode BEFORE writing PL values. The EC firmware
	 * may ignore PL writes unless the target profile mode is active.
	 * The tuxedo control center follows this same order.
	 */
	ret = regmap_update_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
				 PROFILE_MODE_MASK, value);
	if (ret < 0)
		return ret;

	/* Track EC mode for Fn key cycling logic */
	data->last_fan_ctrl = value;

	ret = uniwill_write_pl_values(data, profile_idx);
	if (ret < 0)
		return ret;

	/* Enable overboost after setting performance EC mode */
	if (profile == PLATFORM_PROFILE_MAX_POWER && !data->overboost_active) {
		ret = uniwill_set_overboost(data, true);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static const struct platform_profile_ops uniwill_profile_ops = {
	.probe = uniwill_profile_probe,
	.profile_get = uniwill_profile_get,
	.profile_set = uniwill_profile_set,
};

/*
 * Custom profile cycling that includes MAX_POWER in the rotation.
 * The kernel's platform_profile_cycle() explicitly skips MAX_POWER
 * and CUSTOM profiles, so we implement our own cycling logic for
 * the Fn key handler.
 */
static int uniwill_profile_cycle(struct uniwill_data *data)
{
	enum platform_profile_option cur, next;
	int ret;

	ret = uniwill_profile_get(data->pprof_dev, &cur);
	if (ret < 0)
		return ret;

	switch (cur) {
	case PLATFORM_PROFILE_QUIET:
		next = PLATFORM_PROFILE_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED:
		if (data->num_profiles >= 3)
			next = PLATFORM_PROFILE_PERFORMANCE;
		else
			next = PLATFORM_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		if (uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER))
			next = PLATFORM_PROFILE_MAX_POWER;
		else
			next = PLATFORM_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_MAX_POWER:
		next = PLATFORM_PROFILE_QUIET;
		break;
	default:
		next = PLATFORM_PROFILE_BALANCED;
		break;
	}

	ret = uniwill_profile_set(data->pprof_dev, next);
	if (ret < 0)
		return ret;

	platform_profile_notify(data->pprof_dev);
	return 0;
}

static int uniwill_profile_init(struct uniwill_data *data)
{
	struct device *pprof;

	if (data->num_profiles == 0)
		return 0;

	/*
	 * Initialize to balanced mode. This matches the tuxedo driver behavior
	 * which resets EC_ADDR_MANUAL_FAN_CTRL to 0x00 during probe.
	 * Also write the corresponding default PL values.
	 */
	regmap_update_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
			   PROFILE_MODE_MASK, PROFILE_BALANCED);
	data->last_fan_ctrl = PROFILE_BALANCED;
	uniwill_write_pl_values(data, 1);

	pprof = devm_platform_profile_register(data->dev, "uniwill-platform-profile",
					       data, &uniwill_profile_ops);
	if (IS_ERR(pprof))
		return PTR_ERR(pprof);

	data->pprof_dev = pprof;
	return 0;
}

static int uniwill_custom_profile_init(struct uniwill_data *data)
{
	int ret;

	if (!data->custom_profile_mode_needed)
		return 0;

	/*
	 * Some devices require EC register 0x0727 bit 6 (custom profile mode)
	 * to be set for TDP and fan control to work properly. The bit is first
	 * cleared and then set  after a short delay, since certain devices need
	 * this sequence for reliable activation.
	 */
	ret = regmap_clear_bits(data->regmap, EC_ADDR_CUSTOM_PROFILE,
				CUSTOM_PROFILE_MODE);
	if (ret < 0)
		return ret;

	msleep(50);

	return regmap_set_bits(data->regmap, EC_ADDR_CUSTOM_PROFILE,
			       CUSTOM_PROFILE_MODE);
}

static const unsigned int uniwill_led_channel_to_bat_reg[LED_CHANNELS] = {
	EC_ADDR_LIGHTBAR_BAT_RED,
	EC_ADDR_LIGHTBAR_BAT_GREEN,
	EC_ADDR_LIGHTBAR_BAT_BLUE,
};

static const unsigned int uniwill_led_channel_to_ac_reg[LED_CHANNELS] = {
	EC_ADDR_LIGHTBAR_AC_RED,
	EC_ADDR_LIGHTBAR_AC_GREEN,
	EC_ADDR_LIGHTBAR_AC_BLUE,
};

static int uniwill_led_brightness_set(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	struct led_classdev_mc *led_mc_cdev = lcdev_to_mccdev(led_cdev);
	struct uniwill_data *data = container_of(led_mc_cdev, struct uniwill_data, led_mc_cdev);
	unsigned int value;
	int ret;

	ret = led_mc_calc_color_components(led_mc_cdev, brightness);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->led_lock);

	for (int i = 0; i < LED_CHANNELS; i++) {
		/* Prevent the brightness values from overflowing */
		value = min(data->lightbar_max_brightness, data->led_mc_subled_info[i].brightness);
		ret = regmap_write(data->regmap, uniwill_led_channel_to_ac_reg[i], value);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap, uniwill_led_channel_to_bat_reg[i], value);
		if (ret < 0)
			return ret;
	}

	if (brightness)
		value = 0;
	else
		value = LIGHTBAR_S0_OFF;

	ret = regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, LIGHTBAR_S0_OFF, value);
	if (ret < 0)
		return ret;

	return regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_BAT_CTRL, LIGHTBAR_S0_OFF, value);
}

#define LIGHTBAR_MASK	(LIGHTBAR_APP_EXISTS | LIGHTBAR_S0_OFF | LIGHTBAR_S3_OFF | LIGHTBAR_WELCOME)

static int uniwill_led_init(struct uniwill_data *data)
{
	struct led_init_data init_data = {
		.devicename = DRIVER_NAME,
		.default_label = "multicolor:" LED_FUNCTION_STATUS,
		.devname_mandatory = true,
	};
	unsigned int color_indices[3] = {
		LED_COLOR_ID_RED,
		LED_COLOR_ID_GREEN,
		LED_COLOR_ID_BLUE,
	};
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_LIGHTBAR))
		return 0;

	ret = devm_mutex_init(data->dev, &data->led_lock);
	if (ret < 0)
		return ret;

	/*
	 * The EC has separate lightbar settings for AC and battery mode,
	 * so we have to ensure that both settings are the same.
	 */
	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	value |= LIGHTBAR_APP_EXISTS;
	ret = regmap_write(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, value);
	if (ret < 0)
		return ret;

	/*
	 * The breathing animation during suspend is not supported when
	 * running on battery power.
	 */
	value |= LIGHTBAR_S3_OFF;
	ret = regmap_update_bits(data->regmap, EC_ADDR_LIGHTBAR_BAT_CTRL, LIGHTBAR_MASK, value);
	if (ret < 0)
		return ret;

	data->led_mc_cdev.led_cdev.color = LED_COLOR_ID_MULTI;
	data->led_mc_cdev.led_cdev.max_brightness = data->lightbar_max_brightness;
	data->led_mc_cdev.led_cdev.flags = LED_REJECT_NAME_CONFLICT;
	data->led_mc_cdev.led_cdev.brightness_set_blocking = uniwill_led_brightness_set;

	if (value & LIGHTBAR_S0_OFF)
		data->led_mc_cdev.led_cdev.brightness = 0;
	else
		data->led_mc_cdev.led_cdev.brightness = data->lightbar_max_brightness;

	for (int i = 0; i < LED_CHANNELS; i++) {
		data->led_mc_subled_info[i].color_index = color_indices[i];

		ret = regmap_read(data->regmap, uniwill_led_channel_to_ac_reg[i], &value);
		if (ret < 0)
			return ret;

		/*
		 * Make sure that the initial intensity value is not greater than
		 * the maximum brightness.
		 */
		value = min(data->lightbar_max_brightness, value);
		ret = regmap_write(data->regmap, uniwill_led_channel_to_ac_reg[i], value);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap, uniwill_led_channel_to_bat_reg[i], value);
		if (ret < 0)
			return ret;

		data->led_mc_subled_info[i].intensity = value;
		data->led_mc_subled_info[i].channel = i;
	}

	data->led_mc_cdev.subled_info = data->led_mc_subled_info;
	data->led_mc_cdev.num_colors = LED_CHANNELS;

	return devm_led_classdev_multicolor_register_ext(data->dev, &data->led_mc_cdev,
							 &init_data);
}

static int uniwill_notify_kbd_led(struct uniwill_data *data, int brightness)
{
	struct led_classdev *led_cdev;
	int ret;

	if (data->single_color_kbd)
		led_cdev = &data->kbd_led_cdev;
	else
		led_cdev = &data->kbd_led_mc_cdev.led_cdev;

	guard(mutex)(&led_cdev->led_access);

	/* Sync the LED brightness with the actual hardware state */
	ret = led_update_brightness(led_cdev);
	if (ret < 0)
		return ret;

	led_classdev_notify_brightness_hw_changed(led_cdev, brightness);

	return 0;
}

#define KBD_LED_MASK	(KBD_BRIGHTNESS_MASK | KBD_APPLY | KBD_POWER_OFF)

static int uniwill_kbd_led_write_brightness(struct uniwill_data *data, int brightness)
{
	/* KBD_POWER_OFF is always implicitly cleared */
	unsigned int regval = FIELD_PREP(KBD_BRIGHTNESS_MASK, brightness) | KBD_APPLY;

	/* We must ensure that the "apply" bit is always written */
	return regmap_write_bits(data->regmap, EC_ADDR_KBD_STATUS, KBD_LED_MASK, regval);
}

static int uniwill_kbd_led_read_brightness(struct uniwill_data *data)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_KBD_STATUS, &regval);
	if (ret < 0)
		return ret;

	return min(FIELD_GET(KBD_BRIGHTNESS_MASK, regval), data->kbd_led_max_brightness);
}

static int uniwill_kbd_led_brightness_set(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct uniwill_data *data = container_of(led_cdev, struct uniwill_data, kbd_led_cdev);

	return uniwill_kbd_led_write_brightness(data, brightness);
}

static enum led_brightness uniwill_kbd_led_brightness_get(struct led_classdev *led_cdev)
{
	struct uniwill_data *data = container_of(led_cdev, struct uniwill_data, kbd_led_cdev);

	return uniwill_kbd_led_read_brightness(data);
}

static const unsigned int uniwill_kbd_led_channel_to_reg[KBD_LED_CHANNELS] = {
	EC_ADDR_RGB_RED,
	EC_ADDR_RGB_GREEN,
	EC_ADDR_RGB_BLUE,
};

static int uniwill_kbd_led_mc_brightness_set(struct led_classdev *led_cdev,
					     enum led_brightness brightness)
{
	struct led_classdev_mc *led_mc_cdev = lcdev_to_mccdev(led_cdev);
	struct uniwill_data *data = container_of(led_mc_cdev, struct uniwill_data, kbd_led_mc_cdev);
	unsigned int min_intensity = 0;
	unsigned int regval;
	int ret;

	guard(mutex)(&data->kbd_rgb_led_lock);

	/*
	 * The EC interprets a RGB value of 0x000000 as a command to restore
	 * the device-specfic default RGB value. Work around this by writing
	 * a RGB value of 0x010101 (faint white) instead.
	 */
	if (data->kbd_led_mc_subled_info[0].intensity == 0 &&
	    data->kbd_led_mc_subled_info[1].intensity == 0 &&
	    data->kbd_led_mc_subled_info[2].intensity == 0)
		min_intensity = 1;

	for (int i = 0; i < KBD_LED_CHANNELS; i++) {
		/* Prevent the intensity values from overflowing */
		regval = clamp_val(data->kbd_led_mc_subled_info[i].intensity, min_intensity,
				   KBD_LED_MAX_INTENSITY);
		ret = regmap_write(data->regmap, uniwill_kbd_led_channel_to_reg[i], regval);
		if (ret < 0)
			return ret;
	}

	ret = regmap_write_bits(data->regmap, EC_ADDR_TRIGGER, RGB_APPLY_COLOR, RGB_APPLY_COLOR);
	if (ret < 0)
		return ret;

	return uniwill_kbd_led_write_brightness(data, brightness);
}

static enum led_brightness uniwill_kbd_led_mc_brightness_get(struct led_classdev *led_cdev)
{
	struct led_classdev_mc *led_mc_cdev = lcdev_to_mccdev(led_cdev);
	struct uniwill_data *data = container_of(led_mc_cdev, struct uniwill_data, kbd_led_mc_cdev);

	return uniwill_kbd_led_read_brightness(data);
}

static int uniwill_kbd_led_init(struct uniwill_data *data)
{
	unsigned int color_indices[KBD_LED_CHANNELS] = {
		LED_COLOR_ID_RED,
		LED_COLOR_ID_GREEN,
		LED_COLOR_ID_BLUE,
	};
	struct led_init_data init_data = {
		.devicename = DRIVER_NAME,
		.devname_mandatory = true,
	};
	bool needs_trigger = false;
	unsigned int regval;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
		return 0;

	ret = regmap_read(data->regmap, EC_ADDR_SUPPORT_2, &regval);
	if (ret < 0)
		return ret;

	if (!(regval & CHINA_MODE)) {
		ret = regmap_set_bits(data->regmap, EC_ADDR_BIOS_OEM_2, ENABLE_CHINA_MODE);
		if (ret < 0)
			return ret;
	}

	ret = regmap_read(data->regmap, EC_ADDR_KBD_STATUS, &regval);
	if (ret < 0)
		return ret;

	regval |= KBD_APPLY;
	regval &= ~KBD_POWER_OFF;
	ret = regmap_write(data->regmap, EC_ADDR_KBD_STATUS, regval);
	if (ret < 0)
		return ret;

	switch (data->project_id) {
	case PROJECT_ID_PF:
	case PROJECT_ID_PF4MU_PF4MN_PF5MU:
	case PROJECT_ID_PH4TRX1:
	case PROJECT_ID_PH4TUX1:
	case PROJECT_ID_PH4TQX1:
	case PROJECT_ID_PH6TRX1:
	case PROJECT_ID_PH6TQXX:
	case PROJECT_ID_PHXAXXX:
	case PROJECT_ID_PHXPXXX:
		data->single_color_kbd = true;
		break;
	default:
		data->single_color_kbd = regval & KBD_WHITE_ONLY;
		break;
	}

	if (data->single_color_kbd) {
		init_data.default_label = "white:" LED_FUNCTION_KBD_BACKLIGHT;
		data->kbd_led_cdev.max_brightness = data->kbd_led_max_brightness;
		data->kbd_led_cdev.color = LED_COLOR_ID_WHITE;
		data->kbd_led_cdev.flags = LED_BRIGHT_HW_CHANGED | LED_REJECT_NAME_CONFLICT;
		data->kbd_led_cdev.brightness_set_blocking = uniwill_kbd_led_brightness_set;
		data->kbd_led_cdev.brightness_get = uniwill_kbd_led_brightness_get;

		return devm_led_classdev_register_ext(data->dev, &data->kbd_led_cdev, &init_data);
	}

	for (int i = 0; i < KBD_LED_CHANNELS; i++) {
		data->kbd_led_mc_subled_info[i].color_index = color_indices[i];

		ret = regmap_read(data->regmap, uniwill_kbd_led_channel_to_reg[i], &regval);
		if (ret < 0)
			return ret;

		/*
		 * Make sure that the initial intensity value is not greater than
		 * the maximum intensity.
		 */
		if (regval > KBD_LED_MAX_INTENSITY) {
			regval = KBD_LED_MAX_INTENSITY;
			ret = regmap_write(data->regmap, uniwill_kbd_led_channel_to_reg[i], regval);
			if (ret < 0)
				return ret;

			needs_trigger = true;
		}

		data->kbd_led_mc_subled_info[i].intensity = regval;
		data->kbd_led_mc_subled_info[i].channel = i;
	}

	if (needs_trigger) {
		ret = regmap_write_bits(data->regmap, EC_ADDR_TRIGGER, RGB_APPLY_COLOR,
					RGB_APPLY_COLOR);
		if (ret < 0)
			return ret;
	}

	ret = devm_mutex_init(data->dev, &data->kbd_rgb_led_lock);
	if (ret < 0)
		return ret;

	init_data.default_label = "multicolor:" LED_FUNCTION_KBD_BACKLIGHT;
	data->kbd_led_mc_cdev.led_cdev.max_brightness = data->kbd_led_max_brightness;
	data->kbd_led_mc_cdev.led_cdev.color = LED_COLOR_ID_MULTI;
	data->kbd_led_mc_cdev.led_cdev.flags = LED_BRIGHT_HW_CHANGED | LED_REJECT_NAME_CONFLICT;
	data->kbd_led_mc_cdev.led_cdev.brightness_set_blocking = uniwill_kbd_led_mc_brightness_set;
	data->kbd_led_mc_cdev.led_cdev.brightness_get = uniwill_kbd_led_mc_brightness_get;
	data->kbd_led_mc_cdev.subled_info = data->kbd_led_mc_subled_info;
	data->kbd_led_mc_cdev.num_colors = KBD_LED_CHANNELS;

	return devm_led_classdev_multicolor_register_ext(data->dev, &data->kbd_led_mc_cdev,
							 &init_data);
}

static int uniwill_get_property(struct power_supply *psy, const struct power_supply_ext *ext,
				void *drvdata, enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct uniwill_data *data = drvdata;
	union power_supply_propval prop;
	unsigned int regval;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		ret = regmap_read(data->regmap, EC_ADDR_OEM_4, &regval);
		if (ret < 0)
			return ret;

		switch (FIELD_GET(CHARGING_PROFILE_MASK, regval)) {
		case CHARGING_PROFILE_HIGH_CAPACITY:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_STANDARD;
			return 0;
		case CHARGING_PROFILE_BALANCED:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_LONGLIFE;
			return 0;
		case CHARGING_PROFILE_STATIONARY:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
			return 0;
		default:
			return -EPROTO;
		}
	case POWER_SUPPLY_PROP_HEALTH:
		ret = power_supply_get_property_direct(psy, POWER_SUPPLY_PROP_PRESENT, &prop);
		if (ret < 0)
			return ret;

		if (!prop.intval) {
			val->intval = POWER_SUPPLY_HEALTH_NO_BATTERY;
			return 0;
		}

		ret = power_supply_get_property_direct(psy, POWER_SUPPLY_PROP_STATUS, &prop);
		if (ret < 0)
			return ret;

		if (prop.intval == POWER_SUPPLY_STATUS_UNKNOWN) {
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
			return 0;
		}

		ret = regmap_read(data->regmap, EC_ADDR_BAT_ALERT, &regval);
		if (ret < 0)
			return ret;

		if (regval) {
			/* Charging issue */
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			return 0;
		}

		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		ret = regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL, &regval);
		if (ret < 0)
			return ret;

		regval = FIELD_GET(CHARGE_CTRL_MASK, regval);
		if (!regval)
			val->intval = 100;
		else
			val->intval = min(regval, 100);

		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		ret = regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL_START, &regval);
		if (ret < 0)
			return ret;

		regval = FIELD_GET(CHARGE_CTRL_START_MASK, regval);
		if (!regval)
			val->intval = 0;
		else
			val->intval = min(regval, 100);

		return 0;
	default:
		return -EINVAL;
	}
}

static int uniwill_set_property(struct power_supply *psy, const struct power_supply_ext *ext,
				void *drvdata, enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct uniwill_data *data = drvdata;
	unsigned int regval;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		switch (val->intval) {
		case POWER_SUPPLY_CHARGE_TYPE_TRICKLE:
			regval = FIELD_PREP(CHARGING_PROFILE_MASK, CHARGING_PROFILE_STATIONARY);
			break;
		case POWER_SUPPLY_CHARGE_TYPE_STANDARD:
			regval = FIELD_PREP(CHARGING_PROFILE_MASK, CHARGING_PROFILE_HIGH_CAPACITY);
			break;
		case POWER_SUPPLY_CHARGE_TYPE_LONGLIFE:
			regval = FIELD_PREP(CHARGING_PROFILE_MASK, CHARGING_PROFILE_BALANCED);
			break;
		default:
			return -EINVAL;
		}

		return regmap_update_bits(data->regmap, EC_ADDR_OEM_4, CHARGING_PROFILE_MASK,
					  regval);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval < 0 || val->intval > 100)
			return -EINVAL;

		return regmap_update_bits(data->regmap, EC_ADDR_CHARGE_CTRL, CHARGE_CTRL_MASK,
					  max(val->intval, 1));
	default:
		return -EINVAL;
	}
}

static int uniwill_property_is_writeable(struct power_supply *psy,
					 const struct power_supply_ext *ext, void *drvdata,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_TYPES:
		return true;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return allow_charge_limit;
	default:
		return false;
	}
}

static const enum power_supply_property uniwill_charge_limit_properties[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext uniwill_charge_limit_extension = {
	.name = DRIVER_NAME,
	.properties = uniwill_charge_limit_properties,
	.num_properties = ARRAY_SIZE(uniwill_charge_limit_properties),
	.get_property = uniwill_get_property,
	.set_property = uniwill_set_property,
	.property_is_writeable = uniwill_property_is_writeable,
};

static const enum power_supply_property uniwill_charge_limit_start_properties[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
};

static const struct power_supply_ext uniwill_charge_limit_start_extension = {
	.name = DRIVER_NAME,
	.properties = uniwill_charge_limit_start_properties,
	.num_properties = ARRAY_SIZE(uniwill_charge_limit_start_properties),
	.get_property = uniwill_get_property,
	.set_property = uniwill_set_property,
	.property_is_writeable = uniwill_property_is_writeable,
};

static const enum power_supply_property uniwill_charge_modes_properties[] = {
	POWER_SUPPLY_PROP_CHARGE_TYPES,
	POWER_SUPPLY_PROP_HEALTH,
};

static const struct power_supply_ext uniwill_charge_modes_extension = {
	.name = DRIVER_NAME,
	.charge_types = BIT(POWER_SUPPLY_CHARGE_TYPE_TRICKLE) |
			BIT(POWER_SUPPLY_CHARGE_TYPE_STANDARD) |
			BIT(POWER_SUPPLY_CHARGE_TYPE_LONGLIFE),
	.properties = uniwill_charge_modes_properties,
	.num_properties = ARRAY_SIZE(uniwill_charge_modes_properties),
	.get_property = uniwill_get_property,
	.set_property = uniwill_set_property,
	.property_is_writeable = uniwill_property_is_writeable,
};

static int uniwill_add_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct uniwill_data *data = container_of(hook, struct uniwill_data, hook);
	struct uniwill_battery_entry *entry;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	if (data->has_charge_limit) {
		if (data->has_charge_start_threshold)
			ret = power_supply_register_extension(battery,
							      &uniwill_charge_limit_start_extension,
							      data->dev, data);
		else
			ret = power_supply_register_extension(battery,
							      &uniwill_charge_limit_extension,
							      data->dev, data);
	} else {
		ret = power_supply_register_extension(battery, &uniwill_charge_modes_extension,
						      data->dev, data);
	}

	if (ret < 0) {
		kfree(entry);
		return ret;
	}

	guard(mutex)(&data->battery_lock);

	entry->battery = battery;
	list_add(&entry->head, &data->batteries);

	return 0;
}

static int uniwill_remove_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct uniwill_data *data = container_of(hook, struct uniwill_data, hook);
	struct uniwill_battery_entry *entry, *tmp;

	scoped_guard(mutex, &data->battery_lock) {
		list_for_each_entry_safe(entry, tmp, &data->batteries, head) {
			if (entry->battery == battery) {
				list_del(&entry->head);
				kfree(entry);
				break;
			}
		}
	}

	if (data->has_charge_limit) {
		if (data->has_charge_start_threshold)
			power_supply_unregister_extension(battery,
							  &uniwill_charge_limit_start_extension);
		else
			power_supply_unregister_extension(battery,
							  &uniwill_charge_limit_extension);
	} else {
		power_supply_unregister_extension(battery, &uniwill_charge_modes_extension);
	}

	return 0;
}

static int uniwill_battery_init(struct uniwill_data *data)
{
	unsigned int value, threshold;
	int ret;

	/*
	 * Auto-detect charge limit support by checking whether the charge
	 * control register contains a reasonable initial value. The ACPI
	 * _BST method validates this same register (CGLM at 0x7B9)
	 * unconditionally, confirming it exists across all INOU device
	 * generations. A valid threshold is in the range 0-100, with 100
	 * being the factory default ("no limit") and 0 meaning the register
	 * is uninitialized but present.
	 */
	ret = regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL, &value);
	if (ret < 0)
		return ret;

	threshold = FIELD_GET(CHARGE_CTRL_MASK, value);
	if (threshold <= 100) {
		data->has_charge_limit = true;

		/*
		 * When writing is enabled, initialize an uninitialized
		 * threshold (0) to 100 to signal that we want to take
		 * control of battery charging.
		 */
		if (allow_charge_limit && threshold == 0) {
			FIELD_MODIFY(CHARGE_CTRL_MASK, &value, 100);
			ret = regmap_write(data->regmap, EC_ADDR_CHARGE_CTRL, value);
			if (ret < 0)
				return ret;
		}

		/*
		 * Probe the charge start threshold register (read-only).
		 * The register only exists on newer ECs that support
		 * dual-threshold charge control. On older ECs 0x07D0
		 * may be unmapped (returning 0xFF) or uninitialized
		 * (returning 0x00). A valid start threshold is in the
		 * range 1-100.
		 */
		ret = regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL_START, &value);
		if (ret < 0)
			return ret;

		threshold = FIELD_GET(CHARGE_CTRL_START_MASK, value);
		if (threshold >= 1 && threshold <= 100)
			data->has_charge_start_threshold = true;
	}

	if (!data->has_charge_limit) {
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY_CHARGE_MODES))
			return 0;
	}

	ret = devm_mutex_init(data->dev, &data->battery_lock);
	if (ret < 0)
		return ret;

	INIT_LIST_HEAD(&data->batteries);
	data->hook.name = "Uniwill Battery Extension";
	data->hook.add_battery = uniwill_add_battery;
	data->hook.remove_battery = uniwill_remove_battery;

	return devm_battery_hook_register(data->dev, &data->hook);
}

static int uniwill_notifier_call(struct notifier_block *nb, unsigned long action, void *dummy)
{
	struct uniwill_data *data = container_of(nb, struct uniwill_data, nb);
	struct uniwill_battery_entry *entry;

	switch (action) {
	case UNIWILL_OSD_BATTERY_ALERT:
		if (!data->has_charge_limit &&
		    !uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY_CHARGE_MODES))
			return NOTIFY_DONE;

		mutex_lock(&data->battery_lock);
		list_for_each_entry(entry, &data->batteries, head) {
			power_supply_changed(entry->battery);
		}
		mutex_unlock(&data->battery_lock);

		return NOTIFY_OK;
	case UNIWILL_OSD_DC_ADAPTER_CHANGED:
		/*
		 * Re-apply custom profile mode on AC adapter change. Some
		 * devices lose this setting when the power source changes.
		 */
		if (data->custom_profile_mode_needed)
			regmap_set_bits(data->regmap, EC_ADDR_CUSTOM_PROFILE,
					CUSTOM_PROFILE_MODE);

		/*
		 * Re-apply PL values on AC/DC change. The EC may reset them
		 * to defaults when the power source changes.
		 */
		if (data->num_profiles > 0 &&
		    uniwill_device_supports(data, UNIWILL_FEATURE_CPU_TDP_CONTROL)) {
			unsigned int fan_ctrl;

			if (regmap_read(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
					&fan_ctrl) == 0) {
				int idx;

				switch (fan_ctrl & PROFILE_MODE_MASK) {
				case PROFILE_QUIET:
					idx = 0;
					break;
				case PROFILE_PERFORMANCE:
					idx = data->overboost_active ? 3 : 2;
					break;
				default:
					idx = 1;
					break;
				}
				uniwill_write_pl_values(data, idx);
				if (data->overboost_active)
					uniwill_set_overboost(data, true);
			}
		}

		if (uniwill_device_supports(data, UNIWILL_FEATURE_USB_C_POWER_PRIORITY))
			return notifier_from_errno(usb_c_power_priority_restore(data));

		return NOTIFY_OK;
	case UNIWILL_OSD_FN_LOCK:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_FN_LOCK))
			return NOTIFY_DONE;

		sysfs_notify(&data->dev->kobj, NULL, "fn_lock");

		return NOTIFY_OK;
	case UNIWILL_OSD_KB_LED_LEVEL0:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_notify_kbd_led(data, 0));
	case UNIWILL_OSD_KB_LED_LEVEL1:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_notify_kbd_led(data, 1));
	case UNIWILL_OSD_KB_LED_LEVEL2:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_notify_kbd_led(data, 2));
	case UNIWILL_OSD_KB_LED_LEVEL3:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_notify_kbd_led(data, 3));
	case UNIWILL_OSD_KB_LED_LEVEL4:
		if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_notify_kbd_led(data, 4));
	case UNIWILL_OSD_PERFORMANCE_MODE_TOGGLE:
		if (data->num_profiles == 0)
			return NOTIFY_DONE;

		return notifier_from_errno(uniwill_profile_cycle(data));
	default:
		mutex_lock(&data->input_lock);
		sparse_keymap_report_event(data->input_device, action, 1, true);
		mutex_unlock(&data->input_lock);

		return NOTIFY_OK;
	}
}

static int uniwill_input_init(struct uniwill_data *data)
{
	int ret;

	ret = devm_mutex_init(data->dev, &data->input_lock);
	if (ret < 0)
		return ret;

	data->input_device = devm_input_allocate_device(data->dev);
	if (!data->input_device)
		return -ENOMEM;

	ret = sparse_keymap_setup(data->input_device, uniwill_keymap, NULL);
	if (ret < 0)
		return ret;

	data->input_device->name = "Uniwill WMI hotkeys";
	data->input_device->phys = "wmi/input0";
	data->input_device->id.bustype = BUS_HOST;
	ret = input_register_device(data->input_device);
	if (ret < 0)
		return ret;

	data->nb.notifier_call = uniwill_notifier_call;

	return devm_uniwill_wmi_register_notifier(data->dev, &data->nb);
}

static void uniwill_disable_manual_control(void *context)
{
	struct uniwill_data *data = context;

	regmap_clear_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
}

static int uniwill_ec_init(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PROJECT_ID, &value);
	if (ret < 0)
		return ret;

	data->project_id = value;
	dev_dbg(data->dev, "Project ID: %u\n", value);

	ret = regmap_set_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return ret;

	return devm_add_action_or_reset(data->dev, uniwill_disable_manual_control, data);
}

static int uniwill_probe(struct platform_device *pdev)
{
	struct uniwill_data *data;
	struct regmap *regmap;
	acpi_handle handle;
	int ret;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return -ENODEV;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->handle = handle;
	platform_set_drvdata(pdev, data);

	regmap = devm_regmap_init(&pdev->dev, &uniwill_ec_bus, data, &uniwill_ec_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data->regmap = regmap;

	ret = devm_mutex_init(&pdev->dev, &data->super_key_lock);
	if (ret < 0)
		return ret;

	ret = usb_c_power_priority_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_ec_init(data);
	if (ret < 0)
		return ret;

	data->features = device_descriptor.features;
	data->kbd_led_max_brightness = device_descriptor.kbd_led_max_brightness;
	data->lightbar_max_brightness = device_descriptor.lightbar_max_brightness;
	data->num_profiles = device_descriptor.num_profiles;
	data->custom_profile_mode_needed = device_descriptor.custom_profile_mode_needed;
	memcpy(data->tdp_min, device_descriptor.tdp_min, sizeof(data->tdp_min));
	memcpy(data->tdp_max, device_descriptor.tdp_max, sizeof(data->tdp_max));

	if (uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT))
		allow_charge_limit = true;

	/* Auto-detect mini LED local dimming support */
	data->has_mini_led_dimming = uniwill_has_mini_led_dimming(data);

	/* Auto-detect dGPU power control and GPU MUX switching support */
	if (uniwill_device_supports(data, UNIWILL_FEATURE_GPU_MUX)) {
		data->has_dgpu_power = uniwill_has_dgpu_power();
		if (data->has_dgpu_power) {
			ret = devm_mutex_init(&pdev->dev, &data->dgpu_power_lock);
			if (ret < 0)
				return ret;
		}

		data->has_gpu_mux = uniwill_has_gpu_mux(data);
	}

	/* TCC offset: gated behind descriptor flag, no runtime detection needed */
	data->has_tcc_offset = uniwill_device_supports(data, UNIWILL_FEATURE_TCC_OFFSET);

	/* Auto-detect universal fan control support */
	data->has_universal_fan_ctrl = uniwill_has_universal_fan_ctrl(data);
	data->fan_mode = 2;

	/* Initialise water cooler tunnel state */
	if (uniwill_device_supports(data, UNIWILL_FEATURE_WATER_COOLER)) {
		ret = devm_mutex_init(&pdev->dev, &data->wc.lock);
		if (ret < 0)
			return ret;
		data->wc.fan_mode = 2;
	}

	/*
	 * Initialise the fan curve tables with sensible defaults so that
	 * manual mode can be enabled without prior auto_point programming.
	 * Zone 0 is set to the current fan speed to avoid a sudden change
	 * on the first mode 1 entry; zones 1-15 are populated with dummy
	 * safety entries well beyond the normal operating temperature range.
	 */
	if (data->has_universal_fan_ctrl) {
		ret = uniwill_fan_init_tables(data);
		if (ret < 0)
			return ret;
	}

	/*
	 * Some devices might need to perform some device-specific initialization steps
	 * before the supported features are initialized. Because of this we have to call
	 * this callback just after the EC itself was initialized.
	 */
	if (device_descriptor.probe) {
		ret = device_descriptor.probe(data);
		if (ret < 0)
			return ret;
	}

	ret = uniwill_battery_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_led_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_kbd_led_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_hwmon_init(data);
	if (ret < 0)
		return ret;

	uniwill_smrw_read_gpu_limits(data);

	ret = uniwill_nvidia_ctgp_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_cpu_tdp_init(data);
	if (ret < 0)
		return ret;

	if (device_descriptor.has_hidden_bios_options)
		uniwill_show_hidden_bios_options(data);

	ret = uniwill_profile_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_custom_profile_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_mini_led_init(data);
	if (ret < 0)
		return ret;

	return uniwill_input_init(data);
}

static void uniwill_shutdown(struct platform_device *pdev)
{
	struct uniwill_data *data = platform_get_drvdata(pdev);

	regmap_clear_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
}

static int uniwill_suspend_fn_lock(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_FN_LOCK))
		return 0;

	/*
	 * The EC_ADDR_BIOS_OEM is marked as volatile, so we have to restore it
	 * ourselves.
	 */
	return regmap_read(data->regmap, EC_ADDR_BIOS_OEM, &data->last_status);
}

static int uniwill_suspend_super_key(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY))
		return 0;

	/*
	 * The EC_ADDR_SWITCH_STATUS is marked as volatile, so we have to restore it
	 * ourselves.
	 */
	return regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &data->last_switch_status);
}

static int uniwill_suspend_battery(struct uniwill_data *data)
{
	if (!data->has_charge_limit || !allow_charge_limit)
		return 0;

	/*
	 * Save the current charge limit in order to restore it during resume.
	 * We cannot use the regmap code for that since the register needs to
	 * be declared as volatile due to CHARGE_CTRL_REACHED.
	 */
	return regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL, &data->last_charge_ctrl);
}

static int uniwill_suspend_kbd_led(struct uniwill_data *data)
{
	unsigned int regval;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
		return 0;

	ret = regmap_read(data->regmap, EC_ADDR_KBD_STATUS, &regval);
	if (ret < 0)
		return ret;

	/*
	 * Save the current keyboard backlight settings in order to restore them
	 * during resume. We cannot use the regmap code for that since this register
	 * needs to be declared as volatile because the brightness can be changed
	 * by the EC.
	 */
	data->last_kbd_status = regval;
	FIELD_MODIFY(KBD_BRIGHTNESS_MASK, &regval, 0);
	regval |= KBD_APPLY | KBD_POWER_OFF;

	return regmap_write(data->regmap, EC_ADDR_KBD_STATUS, regval);
}

static int uniwill_suspend_usb_powershare(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_USB_POWERSHARE))
		return 0;

	/*
	 * Save the current usb powershare setting in order to restore it during
	 * resume. We cannot use the regmap code for that since this register needs
	 * to be declared as volatile.
	 */
	return regmap_read(data->regmap, EC_ADDR_TRIGGER, &data->last_trigger);
}

static int uniwill_suspend_nvidia_ctgp(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	return regmap_clear_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
				 CTGP_DB_DB_ENABLE | CTGP_DB_CTGP_ENABLE);
}

static int uniwill_suspend_profile(struct uniwill_data *data)
{
	if (data->num_profiles == 0)
		return 0;

	/*
	 * Save the current fan control mode to restore it during resume.
	 * This register is volatile since the EC can change it.
	 */
	return regmap_read(data->regmap, EC_ADDR_MANUAL_FAN_CTRL, &data->last_fan_ctrl);
}

static int uniwill_suspend_custom_profile(struct uniwill_data *data)
{
	if (!data->custom_profile_mode_needed)
		return 0;

	/*
	 * Clear custom profile mode before suspend to prevent at least one
	 * known device from immediately waking up after entering suspend.
	 */
	return regmap_clear_bits(data->regmap, EC_ADDR_CUSTOM_PROFILE,
				CUSTOM_PROFILE_MODE);
}

static int uniwill_suspend_fan_mode(struct uniwill_data *data)
{
	switch (data->fan_mode) {
	case 0:	/* Full speed - clear boost before suspend */
		return regmap_clear_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
					 FAN_MODE_BOOST);
	case 1:	/* Manual - disable custom tables before suspend */
		return uniwill_fan_disable_custom_tables(data);
	default:
		return 0;
	}
}

static int uniwill_suspend(struct device *dev)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	int ret;

	ret = uniwill_suspend_fn_lock(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_super_key(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_battery(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_kbd_led(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_usb_powershare(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_nvidia_ctgp(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_profile(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_custom_profile(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_fan_mode(data);
	if (ret < 0)
		return ret;

	regcache_cache_only(data->regmap, true);
	regcache_mark_dirty(data->regmap);

	return 0;
}

static int uniwill_resume_fn_lock(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_FN_LOCK))
		return 0;

	return regmap_update_bits(data->regmap, EC_ADDR_BIOS_OEM, FN_LOCK_STATUS,
				  data->last_status);
}

static int uniwill_resume_super_key(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY))
		return 0;

	ret = regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	if ((data->last_switch_status & SUPER_KEY_LOCK_STATUS) == (value & SUPER_KEY_LOCK_STATUS))
		return 0;

	return regmap_write_bits(data->regmap, EC_ADDR_TRIGGER, TRIGGER_SUPER_KEY_LOCK,
				 TRIGGER_SUPER_KEY_LOCK);
}

static int uniwill_resume_battery(struct uniwill_data *data)
{
	if (!data->has_charge_limit || !allow_charge_limit)
		return 0;

	return regmap_update_bits(data->regmap, EC_ADDR_CHARGE_CTRL, CHARGE_CTRL_MASK,
				  data->last_charge_ctrl);
}

static int uniwill_resume_kbd_led(struct uniwill_data *data)
{
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_KEYBOARD_BACKLIGHT))
		return 0;

	ret = regmap_write(data->regmap, EC_ADDR_KBD_STATUS, data->last_kbd_status | KBD_APPLY);
	if (ret < 0)
		return ret;

	if (data->single_color_kbd)
		return 0;

	return regmap_write_bits(data->regmap, EC_ADDR_TRIGGER, RGB_APPLY_COLOR, RGB_APPLY_COLOR);
}

static int uniwill_resume_usb_powershare(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_USB_POWERSHARE))
		return 0;

	return regmap_update_bits(data->regmap, EC_ADDR_TRIGGER, TRIGGER_USB_CHARGING,
				  data->last_trigger);
}

static int uniwill_resume_nvidia_ctgp(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	return regmap_update_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
				  CTGP_DB_DB_ENABLE | CTGP_DB_CTGP_ENABLE,
				  (data->dynamic_boost_enable ? CTGP_DB_DB_ENABLE : 0) |
				  CTGP_DB_CTGP_ENABLE);
}

static int uniwill_resume_usb_c_power_priority(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_USB_C_POWER_PRIORITY))
		return 0;

	return usb_c_power_priority_restore(data);
}

static int uniwill_resume_profile(struct uniwill_data *data)
{
	int profile_idx;
	int ret;

	if (data->num_profiles == 0)
		return 0;

	switch (data->last_fan_ctrl & PROFILE_MODE_MASK) {
	case PROFILE_QUIET:
		profile_idx = 0;
		break;
	case PROFILE_PERFORMANCE:
		profile_idx = data->overboost_active ? 3 : 2;
		break;
	default:
		profile_idx = 1;
		break;
	}

	/*
	 * Re-apply profile mode first, then PL values. The EC firmware
	 * may only apply PL writes when the target profile is active.
	 */
	ret = regmap_update_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
				 PROFILE_MODE_MASK, data->last_fan_ctrl);
	if (ret < 0)
		return ret;

	ret = uniwill_write_pl_values(data, profile_idx);
	if (ret < 0)
		return ret;

	/* Re-apply overboost (VRM current + OVERBOOST bit) after resume */
	if (data->overboost_active)
		return uniwill_set_overboost(data, true);

	return 0;
}

static int uniwill_resume_custom_profile(struct uniwill_data *data)
{
	if (!data->custom_profile_mode_needed)
		return 0;

	return regmap_set_bits(data->regmap, EC_ADDR_CUSTOM_PROFILE,
			       CUSTOM_PROFILE_MODE);
}

static int uniwill_resume_fan_mode(struct uniwill_data *data)
{
	int ret;

	switch (data->fan_mode) {
	case 0:	/* Full speed - restore boost */
		return regmap_set_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL,
				       FAN_MODE_BOOST);
	case 1:	/* Manual - re-write speed tables and re-enable custom tables */
		/*
		 * The fan speed table registers are volatile and lost during
		 * sleep. Re-write the last user-set speeds for zone 0 before
		 * re-enabling the custom table control bits.
		 */
		ret = regmap_write(data->regmap, EC_ADDR_CPU_FAN_SPEED_TABLE,
				   data->last_fan_pwm[0]);
		if (ret < 0)
			return ret;

		ret = regmap_write(data->regmap, EC_ADDR_GPU_FAN_SPEED_TABLE,
				   data->last_fan_pwm[1]);
		if (ret < 0)
			return ret;

		ret = regmap_set_bits(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL,
				      SPLIT_TABLES);
		if (ret < 0)
			return ret;

		return regmap_set_bits(data->regmap, EC_ADDR_AP_OEM_6,
				       ENABLE_UNIVERSAL_FAN_CTRL);
	default:
		return 0;
	}
}

static int uniwill_resume_mini_led(struct uniwill_data *data)
{
	if (!data->has_mini_led_dimming)
		return 0;

	if (data->mini_led_dimming_state)
		return uniwill_wmi_evaluate(UNIWILL_WMI_FUNC_FEATURE_TOGGLE,
					    UNIWILL_WMI_LOCAL_DIMMING_ON);

	return uniwill_wmi_evaluate(UNIWILL_WMI_FUNC_FEATURE_TOGGLE,
				    UNIWILL_WMI_LOCAL_DIMMING_OFF);
}

static int uniwill_resume(struct device *dev)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	int ret;

	regcache_cache_only(data->regmap, false);

	ret = regcache_sync(data->regmap);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_fn_lock(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_super_key(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_battery(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_kbd_led(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_usb_powershare(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_nvidia_ctgp(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_usb_c_power_priority(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_profile(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_custom_profile(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_fan_mode(data);
	if (ret < 0)
		return ret;

	return uniwill_resume_mini_led(data);
}

static DEFINE_SIMPLE_DEV_PM_OPS(uniwill_pm_ops, uniwill_suspend, uniwill_resume);

/*
 * We only use the DMI table for auoloading because the ACPI device itself
 * does not guarantee that the underlying EC implementation is supported.
 */
static const struct acpi_device_id uniwill_id_table[] = {
	{ "INOU0000" },
	{ },
};

static struct platform_driver uniwill_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.dev_groups = uniwill_groups,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.acpi_match_table = uniwill_id_table,
		.pm = pm_sleep_ptr(&uniwill_pm_ops),
	},
	.probe = uniwill_probe,
	.shutdown = uniwill_shutdown,
};

static struct uniwill_device_descriptor machenike_l16p_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL |
		    UNIWILL_FEATURE_KEYBOARD_BACKLIGHT |
		    UNIWILL_FEATURE_AC_AUTO_BOOT |
		    UNIWILL_FEATURE_USB_POWERSHARE,
	.kbd_led_max_brightness = 4,
};

static struct uniwill_device_descriptor lapqc71a_lapqc71b_descriptor __initdata = {
	.features = UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_LIGHTBAR |
		    UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN,
	.lightbar_max_brightness = 36,
};

static struct uniwill_device_descriptor lapac71h_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_TOUCHPAD_TOGGLE |
		    UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN,
};

static struct uniwill_device_descriptor lapkc71f_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_TOUCHPAD_TOGGLE |
		    UNIWILL_FEATURE_LIGHTBAR |
		    UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN,
	.lightbar_max_brightness = 200,
};

/*
 * The featuresets below reflect somewhat chronological changes:
 * 1 -> 2: UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL is added to the EC firmware.
 * 2 -> 3: UNIWILL_FEATURE_USB_C_POWER_PRIORITY is removed from the EC firmware.
 * Some devices might divert from this timeline.
 */

static struct uniwill_device_descriptor tux_featureset_1_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
};

static struct uniwill_device_descriptor pulse_gen1_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
	.num_profiles = 2,
};

static struct uniwill_device_descriptor tux_featureset_1_nvidia_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
};

static struct uniwill_device_descriptor polaris_gen1_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
	.num_profiles = 3,
};

static struct uniwill_device_descriptor tux_featureset_2_nvidia_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
};

static struct uniwill_device_descriptor polaris_gen2_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
	.num_profiles = 3,
};

static struct uniwill_device_descriptor tux_featureset_3_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_KEYBOARD_BACKLIGHT |
		    UNIWILL_FEATURE_AC_AUTO_BOOT |
		    UNIWILL_FEATURE_USB_POWERSHARE,
	.kbd_led_max_brightness = 4,
	.num_profiles = 3,
};

static struct uniwill_device_descriptor tux_featureset_3_nvidia_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL,
};

static struct uniwill_device_descriptor tux_featureset_3_nvidia_wc_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_GPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL |
		    UNIWILL_FEATURE_WATER_COOLER,
};

static struct uniwill_device_descriptor tux_featureset_3_cpm_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_KEYBOARD_BACKLIGHT |
		    UNIWILL_FEATURE_AC_AUTO_BOOT |
		    UNIWILL_FEATURE_USB_POWERSHARE |
		    UNIWILL_FEATURE_CPU_TDP_CONTROL,
	.kbd_led_max_brightness = 4,
	.num_profiles = 3,
	.custom_profile_mode_needed = true,
};

/*
 * The featureset 3 nvidia cpm devices share the same base features but
 * have different CPU TDP limits per board. Each board that supports TDP
 * gets its own descriptor with the appropriate bounds.
 */

#define TUX_FEATURESET_3_NVIDIA_CPM_FEATURES		\
	(UNIWILL_FEATURE_FN_LOCK |			\
	 UNIWILL_FEATURE_SUPER_KEY |			\
	 UNIWILL_FEATURE_CPU_TEMP |			\
	 UNIWILL_FEATURE_GPU_TEMP |			\
	 UNIWILL_FEATURE_PRIMARY_FAN |			\
	 UNIWILL_FEATURE_SECONDARY_FAN |		\
	 UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL |		\
	 UNIWILL_FEATURE_CPU_TDP_CONTROL)

/* TUXEDO Stellaris Slim 15 Gen6 AMD (GMxHGxx) */
static struct uniwill_device_descriptor gmxhgxx_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 90, 90, 100 },
};

/* TUXEDO Stellaris Slim 15 Gen6 Intel (GM5IXxA) */
static struct uniwill_device_descriptor gm5ixxa_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 140, 140, 200 },
};

/* TUXEDO Stellaris 16 Gen6 Intel MB1 (GM6IXxB_MB1) */
static struct uniwill_device_descriptor gm6ixxb_mb1_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES |
		    UNIWILL_FEATURE_WATER_COOLER,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 205, 205, 400 },
};

/* TUXEDO Stellaris 16 Gen6 Intel MB2 (GM6IXxB_MB2) */
static struct uniwill_device_descriptor gm6ixxb_mb2_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES |
		    UNIWILL_FEATURE_WATER_COOLER,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 160, 160, 250 },
};

/* TUXEDO Stellaris 17 Gen6 Intel (GM7IXxN) */
static struct uniwill_device_descriptor gm7ixxn_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES |
		    UNIWILL_FEATURE_WATER_COOLER,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 160, 160, 250 },
};

/* TUXEDO Stellaris 16 Gen7 Intel (X6AR5xxY / X6AR5xxY_mLED) */
static struct uniwill_device_descriptor x6ar5xxy_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES |
		    UNIWILL_FEATURE_WATER_COOLER |
		    UNIWILL_FEATURE_GPU_MUX |
		    UNIWILL_FEATURE_TCC_OFFSET,
	.num_profiles = 3,
	.custom_profile_mode_needed = true,
	.tdp_min = { 5, 5, 5 },
	.tdp_max = { 210, 210, 420 },
	.has_hidden_bios_options = true,
};

/* TUXEDO Stellaris 16 Gen7 AMD (X6FR5xxY) */
static struct uniwill_device_descriptor x6fr5xxy_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES |
		    UNIWILL_FEATURE_WATER_COOLER |
		    UNIWILL_FEATURE_GPU_MUX |
		    UNIWILL_FEATURE_TCC_OFFSET,
	.num_profiles = 3,
	.custom_profile_mode_needed = true,
	.tdp_min = { 25, 25, 25 },
	.tdp_max = { 162, 162, 195 },
};

/* TUXEDO InfinityBook Max 15 Gen10 AMD (X5KK45xS_X5SP45xS) */
static struct uniwill_device_descriptor x5kk45xs_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 10, 10, 10 },
	.tdp_max = { 100, 100, 105 },
};

/* TUXEDO InfinityBook Max 16 Gen10 AMD (X6KK45xU_X6SP45xU) */
static struct uniwill_device_descriptor x6kk45xu_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 10, 10, 10 },
	.tdp_max = { 100, 100, 105 },
};

/* TUXEDO InfinityBook Max 15 Gen10 Intel (X5AR45xS) */
static struct uniwill_device_descriptor x5ar45xs_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 10, 10, 10 },
	.tdp_max = { 90, 90, 230 },
};

/* TUXEDO InfinityBook Max 16 Gen10 Intel (X6AR55xU) */
static struct uniwill_device_descriptor x6ar55xu_descriptor __initdata = {
	.features = TUX_FEATURESET_3_NVIDIA_CPM_FEATURES,
	.custom_profile_mode_needed = true,
	.tdp_min = { 10, 10, 10 },
	.tdp_max = { 145, 155, 290 },
	.has_hidden_bios_options = true,
};

static int phxtxx1_probe(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PROJECT_ID, &value);
	if (ret < 0)
		return ret;

	if (value == PROJECT_ID_PH4TRX1 || value == PROJECT_ID_PH6TRX1)
		data->features |= UNIWILL_FEATURE_SECONDARY_FAN;

	return 0;
};

static struct uniwill_device_descriptor phxtxx1_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
	.probe = phxtxx1_probe,
};

static int phxarx1_phxaqf1_probe(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_SYSTEM_ID, &value);
	if (ret < 0)
		return ret;

	if (value & HAS_GPU)
		data->features |= UNIWILL_FEATURE_GPU_TEMP |
				  UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL;

	return 0;
};

static struct uniwill_device_descriptor phxarx1_phxaqf1_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_USB_C_POWER_PRIORITY,
	.probe = phxarx1_phxaqf1_probe,
};

static struct uniwill_device_descriptor pf5pu1g_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN,
	.num_profiles = 2,
};

static struct uniwill_device_descriptor x4sp4nal_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK |
		    UNIWILL_FEATURE_SUPER_KEY |
		    UNIWILL_FEATURE_BATTERY_CHARGE_MODES |
		    UNIWILL_FEATURE_CPU_TEMP |
		    UNIWILL_FEATURE_PRIMARY_FAN |
		    UNIWILL_FEATURE_SECONDARY_FAN |
		    UNIWILL_FEATURE_KEYBOARD_BACKLIGHT |
		    UNIWILL_FEATURE_AC_AUTO_BOOT |
		    UNIWILL_FEATURE_USB_POWERSHARE,
	.kbd_led_max_brightness = 2,
};

static const struct dmi_system_id uniwill_dmi_table[] __initconst = {
	{
		.ident = "AiStone X4SP4NAL",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "AiStone"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X4SP4NAL"),
		},
		.driver_data = &x4sp4nal_descriptor,
	},
	{
		.ident = "MACHENIKE L16 Pro",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MACHENIKE"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "L16P"),
		},
		.driver_data = &machenike_l16p_descriptor,
	},
	{
		.ident = "XMG FUSION 15 (L19)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71A"),
		},
		.driver_data = &lapqc71a_lapqc71b_descriptor,
	},
	{
		.ident = "XMG FUSION 15 (L19)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71B"),
		},
		.driver_data = &lapqc71a_lapqc71b_descriptor,
	},
	{
		.ident = "XMG FUSION 15 (L19)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71A"),
		},
		.driver_data = &lapqc71a_lapqc71b_descriptor,
	},
	{
		.ident = "XMG FUSION 15 (L19)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71B"),
		},
		.driver_data = &lapqc71a_lapqc71b_descriptor,
	},
	{
		.ident = "Intel NUC x15",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel(R) Client Systems"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "LAPAC71H"),
		},
		.driver_data = &lapac71h_descriptor,
	},
	{
		.ident = "Intel NUC x15",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel(R) Client Systems"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "LAPKC71F"),
		},
		.driver_data = &lapkc71f_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14 Gen6 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PHxTxX1"),
		},
		.driver_data = &phxtxx1_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14 Gen6 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PHxTQx1"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/16 Gen7 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PHxARX1_PHxAQF1"),
		},
		.driver_data = &phxarx1_phxaqf1_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 16 Gen7 Intel/Commodore Omnia-Book Pro Gen 7",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PH6AG01_PH6AQ71_PH6AQI1"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/16 Gen8 Intel/Commodore Omnia-Book Pro Gen 8",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PH4PRX1_PH6PRX1"),
		},
		.driver_data = &tux_featureset_1_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14 Gen8 Intel/Commodore Omnia-Book Pro Gen 8",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PH4PG31"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 16 Gen8 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PH6PG01_PH6PG71"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/15 Gen9 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GXxHRXx"),
		},
		.driver_data = &tux_featureset_3_cpm_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/15 Gen9 Intel/Commodore Omnia-Book 15 Gen9",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GXxMRXx"),
		},
		.driver_data = &tux_featureset_3_cpm_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/15 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "XxHP4NAx"),
		},
		.driver_data = &tux_featureset_3_cpm_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 14/15 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "XxKK4NAx_XxSP4NAx"),
		},
		.driver_data = &tux_featureset_3_cpm_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Pro 15 Gen10 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "XxAR4NAx"),
		},
		.driver_data = &tux_featureset_3_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Max 15 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X5KK45xS_X5SP45xS"),
		},
		.driver_data = &x5kk45xs_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Max 16 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6HP45xU"),
		},
		.driver_data = &tux_featureset_3_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Max 16 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6KK45xU_X6SP45xU"),
		},
		.driver_data = &x6kk45xu_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Max 15 Gen10 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X5AR45xS"),
		},
		.driver_data = &x5ar45xs_descriptor,
	},
	{
		.ident = "TUXEDO InfinityBook Max 16 Gen10 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6AR55xU"),
		},
		.driver_data = &x6ar55xu_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1501A1650TI"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1501A2060"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 17 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1701A1650TI"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 17 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1701A2060"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15 Gen1 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1501I1650TI"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15 Gen1 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1501I2060"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 17 Gen1 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1701I1650TI"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 17 Gen1 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "POLARIS1701I2060"),
		},
		.driver_data = &polaris_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Trinity 15 Intel Gen1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "TRINITY1501I"),
		},
		.driver_data = &tux_featureset_1_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Trinity 17 Intel Gen1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "TRINITY1701I"),
		},
		.driver_data = &tux_featureset_1_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15/17 Gen2 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxMGxx"),
		},
		.driver_data = &polaris_gen2_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15/17 Gen2 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxNGxx"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris/Polaris 15/17 Gen3 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxZGxx"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris/Polaris 15/17 Gen3 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxTGxx"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris/Polaris 15/17 Gen4 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxRGxx"),
		},
		.driver_data = &tux_featureset_3_nvidia_wc_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 15 Gen4 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxAGxx"),
		},
		.driver_data = &tux_featureset_3_nvidia_wc_descriptor,
	},
	{
		.ident = "TUXEDO Polaris 15/17 Gen5 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxXGxx"),
		},
		.driver_data = &tux_featureset_2_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen5 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GM6XGxX"),
		},
		.driver_data = &tux_featureset_3_nvidia_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16/17 Gen5 Intel/Commodore ORION Gen 5",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxPXxx"),
		},
		.driver_data = &tux_featureset_3_nvidia_wc_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris Slim 15 Gen6 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GMxHGxx"),
		},
		.driver_data = &gmxhgxx_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris Slim 15 Gen6 Intel/Commodore ORION Slim 15 Gen6",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GM5IXxA"),
		},
		.driver_data = &gm5ixxa_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen6 Intel/Commodore ORION 16 Gen6",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GM6IXxB_MB1"),
		},
		.driver_data = &gm6ixxb_mb1_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen6 Intel/Commodore ORION 16 Gen6",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GM6IXxB_MB2"),
		},
		.driver_data = &gm6ixxb_mb2_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 17 Gen6 Intel/Commodore ORION 17 Gen6",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "GM7IXxN"),
		},
		.driver_data = &gm7ixxn_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen7 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6FR5xxY"),
		},
		.driver_data = &x6fr5xxy_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen7 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6AR5xxY"),
		},
		.driver_data = &x6ar5xxy_descriptor,
	},
	{
		.ident = "TUXEDO Stellaris 16 Gen7 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6AR5xxY_mLED"),
		},
		.driver_data = &x6ar5xxy_descriptor,
	},
	{
		.ident = "XMG NEO 16 (E25)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6AR5xxY"),
		},
		.driver_data = &x6ar5xxy_descriptor,
	},
	{
		.ident = "XMG NEO 16 (A25)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "X6FR5xxY"),
		},
		.driver_data = &x6fr5xxy_descriptor,
	},
	{
		.ident = "TUXEDO Book BA15 Gen10 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PF5PU1G"),
		},
		.driver_data = &pf5pu1g_descriptor,
	},
	{
		.ident = "TUXEDO Pulse 14 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PULSE1401"),
		},
		.driver_data = &pulse_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Pulse 15 Gen1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PULSE1501"),
		},
		.driver_data = &pulse_gen1_descriptor,
	},
	{
		.ident = "TUXEDO Pulse 15 Gen2 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "PF5LUXG"),
		},
		.driver_data = &tux_featureset_1_descriptor,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, uniwill_dmi_table);

static int __init uniwill_init(void)
{
	const struct uniwill_device_descriptor *descriptor;
	const struct dmi_system_id *id;
	int ret;

	id = dmi_first_match(uniwill_dmi_table);
	if (!id) {
		if (!force)
			return -ENODEV;

		pr_warn("Loading on a potentially unsupported device\n");
	} else {
		/*
		 * Some devices might support additional features depending on
		 * the BIOS version/date, so we call this callback to let them
		 * modify their device descriptor accordingly.
		 */
		if (id->callback) {
			ret = id->callback(id);
			if (ret < 0)
				return ret;
		}

		descriptor = id->driver_data;
		device_descriptor = *descriptor;
	}

	if (force) {
		/* Assume that the device supports all features except the charge limit and GPU MUX */
		device_descriptor.features = UINT_MAX & ~(UNIWILL_FEATURE_BATTERY_CHARGE_LIMIT |
							  UNIWILL_FEATURE_GPU_MUX |
							  UNIWILL_FEATURE_TCC_OFFSET);
		/* Some models only support 3 brightness levels */
		device_descriptor.kbd_led_max_brightness = 4;
		/* Some models only support 36 brightness levels per color component */
		device_descriptor.lightbar_max_brightness = 200;
		/* Assume three performance profiles */
		device_descriptor.num_profiles = 3;
		/* Some devices need custom profile mode for TDP control */
		device_descriptor.custom_profile_mode_needed = true;
		pr_warn("Enabling potentially unsupported features\n");
	}

	ret = platform_driver_register(&uniwill_driver);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_register_driver();
	if (ret < 0) {
		platform_driver_unregister(&uniwill_driver);
		return ret;
	}

	return 0;
}
module_init(uniwill_init);

static void __exit uniwill_exit(void)
{
	uniwill_wmi_unregister_driver();
	platform_driver_unregister(&uniwill_driver);
}
module_exit(uniwill_exit);

MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("Uniwill notebook driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: led-class-multicolor");
