/**
 * @file lv_conf.h
 * LVGL 9 configuration for BRL Laptimer
 * Waveshare ESP32-S3 7-Inch LCD (800x480, RGB565)
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/

/* Color depth: 16 (RGB565) */
#define LV_COLOR_DEPTH 16

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/

/* Use built-in LVGL malloc/free (backed by heap_caps on ESP32) */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* Memory pool size for LVGL (only used when LV_STDLIB_BUILTIN) */
#define LV_MEM_SIZE (256 * 1024U)  /* 256 KB */
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC   malloc
#define LV_MEM_POOL_FREE    free

/*====================
   HAL SETTINGS
 *====================*/

/* Default display refresh period in ms */
#define LV_DEF_REFR_PERIOD  10

/* Input device read period in ms */
#define LV_INDEV_DEF_READ_PERIOD 30

/* Align draw buffer to this many bytes */
#define LV_DRAW_BUF_ALIGN   4


/*=====================
   RENDERING SETTINGS
 *=====================*/

/* Default draw context (software) */
#define LV_USE_DRAW_SW 1

/* Enable 16-bit color swap for flush */
#define LV_DRAW_SW_COMPLEX 1

/* Disable ARM-specific ASM optimisations (Helium/NEON) — ESP32-S3 uses Xtensa */
#define LV_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE

/*=======================
   FEATURE CONFIGURATION
 *=======================*/

/* Large memory - enable all font/widget features */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Enable extra drawing capabilities */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1
#define LV_USE_CHART      1
#define LV_USE_LED        1
#define LV_USE_MSGBOX     1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1
#define LV_USE_SPAN       1
#define LV_USE_METER      1

/* Animation */
#define LV_USE_ANIM 1

/* Enable image decoder */
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_PNG 0

/* Logging */
#define LV_USE_LOG 0

/* Use assert */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*===================
   DEMO USAGE
 *===================*/

/* Enable demo widgets for testing */
#define LV_USE_DEMO_WIDGETS 1
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

/*==================
 * EXAMPLES
 *==================*/
#define LV_BUILD_EXAMPLES 0

/*===================
 * PERF MONITOR
 *==================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

#endif /* LV_CONF_H */
