// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Danny Lin <danny@kdrag0n.dev>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/touchpaint.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

#define MAX_FINGERS 10

struct point {
	int x;
	int y;
};

enum tp_mode {
	MODE_PAINT,
	MODE_FILL,
	MODE_BOX
};

/* Config */
static phys_addr_t fb_phys_addr = 0x9c000000;
static size_t fb_max_size = 0x02400000;
/* Pixel format is assumed to be ARGB_8888 */
static int fb_width = 1080;
static int fb_height = 2340;
static enum tp_mode mode = MODE_PAINT;
module_param(mode, int, 0644);
/* Brush size in pixels - odd = slower but centered, even = faster but not centered */
static int brush_size = 2;
module_param(brush_size, int, 0644);

/* State */
static u32 __iomem *fb_mem;
static size_t fb_size;
static bool init_done;
static unsigned int fingers;
static bool finger_down[MAX_FINGERS];
static struct point last_point[MAX_FINGERS];
static struct task_struct *box_thread;

static void blank_screen(void)
{
	u64 before = ktime_get_ns();
	u64 delta;
	memset(fb_mem, 0, fb_size);
	delta = ktime_get_ns() - before;
	pr_info("TPM: [blank] %llu ns to fill %zu bytes on cpu%d\n", delta, fb_size, smp_processor_id());
}

static void blank_callback(unsigned long data)
{
	blank_screen();
}
static DEFINE_TIMER(blank_timer, blank_callback, 0, 0);

static void fill_screen_white(void)
{
	u64 before = ktime_get_ns();
	u64 delta;
	memset(fb_mem, 0xffffffff, fb_size);
	delta = ktime_get_ns() - before;
	pr_info("TPM: [fill] %llu ns to fill %zu bytes on cpu%d\n", delta, fb_size, smp_processor_id());
}

static size_t point_to_offset(int x, int y)
{
	return x + (y * fb_width);
}

static u32 rgb_to_pixel(u8 r, u8 g, u8 b)
{
	u32 pixel = 0xff000000;
	pixel |= r << 16;
	pixel |= g << 8;
	pixel |= b;

	return pixel;
}

static void set_pixel(size_t offset_px, u32 pixel)
{
	*(volatile u32 *)(fb_mem + offset_px) = pixel;
}

static void set_2pixels(size_t offset_px, u32 pixel)
{
	u64 pixels = ((u64)pixel << 32) | pixel;
	*(volatile u64 *)(fb_mem + offset_px) = pixels;
}

#if defined(CONFIG_ARCH_SUPPORTS_INT128) && defined(__SIZEOF_INT128__)
static void set_4pixels(size_t offset_px, u32 pixel32)
{
	unsigned __int128 pixel128 = (unsigned __int128)pixel32;
	unsigned __int128 pixels = (pixel128 << 96) | (pixel128 << 64) |
		(pixel128 << 32) | pixel128;

	*(volatile unsigned __int128 *)(fb_mem + offset_px) = pixels;
}
#endif

static int draw_pixels(int x, int y, int count, u8 r, u8 g, u8 b)
{
	size_t offset_px = point_to_offset(x, y);
	u32 pixel = rgb_to_pixel(r, g, b);

	pr_debug("set pixel: x=%d y=%d offset=%zupx count=%d color=(%d, %d, %d)\n",
		 x, y, offset_px, count, r, g, b);

#if defined(CONFIG_ARCH_SUPPORTS_INT128) && defined(__SIZEOF_INT128__)
	if (count >= 4) {
		set_4pixels(offset_px, pixel);
		return 4;
	}
#endif

	if (count >= 2) {
		set_2pixels(offset_px, pixel);
		return 2;
	}

	if (count >= 1) {
		set_pixel(offset_px, pixel);
		return 1;
	}

	return 0;
}

static void draw_segment(int x, int y, int size, u8 r, u8 g, u8 b)
{
	int base_x = clamp(x - max(1, (size - 1) / 2), 0, fb_width);
	int target_x = min(base_x + size, fb_width);
	int cur_x = base_x;

	pr_debug("draw segment: x=%d y=%d size=%d r=%d g=%d b=%d\n", x, y, size,
		 r, g, b);
	while (cur_x < target_x) {
		int remaining_px = target_x - cur_x;
		cur_x += draw_pixels(cur_x, y, remaining_px, r, g, b);
	}
}

static void draw_point(int x, int y, int size, u8 r, u8 g, u8 b)
{
	int base_y = clamp(y - max(1, (size - 1) / 2), 0, fb_height);
	int off_y;
	u64 before;

	pr_debug("draw point: x=%d y=%d size=%d r=%d g=%d b=%d\n", x, y, size, r, g, b);
	before = ktime_get_ns();
	for (off_y = 0; off_y < size; off_y++) {
		draw_segment(x, base_y + off_y, size, r, g, b);
	}

	pr_debug("draw point took %llu ns\n", ktime_get_ns() - before);
}

static void fill_screen(u8 r, u8 g, u8 b)
{
	int y;

	for (y = 0; y < fb_height; y++) {
		int x = 0;

		while (x < fb_width) {
			x += draw_pixels(x, y, fb_width - x, r, g, b);
		}
	}
}

static int box_thread_func(void *data)
{
	static const struct sched_param rt_prio = { .sched_priority = 1 };
	int x = fb_width / 2;
	int y = fb_height / 12;
	int step = 7;
	int size = 301;
	int radius = max(1, (size - 1) / 2);

	sched_setscheduler_nocheck(current, SCHED_FIFO, &rt_prio);

	fill_screen(64, 0, 128);
	draw_point(x, y, size, 255, 255, 0);

	while (!kthread_should_stop()) {
		int off_y;

		if (y > fb_height - (fb_height / 12) || y < fb_height / 12)
			step *= -1;

		y += step;

		/* Draw damage rather than redrawing the entire box */
		for (off_y = 0; off_y < abs(step); off_y++) {
			if (step < 0) {
				/* Going up */
				draw_segment(x, y + radius + off_y, size, 64, 0, 128);
				draw_segment(x, y - radius - off_y, size, 255, 255, 0);
			} else {
				/* Going down */
				draw_segment(x, y - radius - off_y, size, 64, 0, 128);
				draw_segment(x, y + radius + off_y, size, 255, 255, 0);
			}
		}

		usleep_range(8000, 8000);
	}

	return 0;
}

static void start_box_thread(void)
{
	if (box_thread) {
		pr_warn("tried to start duplicate box thread!\n");
		return;
	}

	box_thread = kthread_run(box_thread_func, NULL, "touchpaint_box");
	if (IS_ERR(box_thread)) {
		pr_err("failed to start box thread! err=%d\n", PTR_ERR(box_thread));
		box_thread = NULL;
	}
}

static void stop_box_thread(void)
{
	int ret = kthread_stop(box_thread);
	if (ret)
		pr_err("failed to stop box thread! err=%d\n", ret);

	box_thread = NULL;
}

void touchpaint_finger_down(int slot)
{
	pr_debug("finger %d down event from driver\n", slot);
	if (!init_done || finger_down[slot])
		return;

	pr_debug("finger %d down\n", slot);
	finger_down[slot] = true;
	if (++fingers == 1) {
		switch (mode) {
		case MODE_PAINT:
			blank_screen();
			break;
		case MODE_FILL:
			del_timer(&blank_timer);
			fill_screen_white();
			break;
		case MODE_BOX:
			if (box_thread) {
				stop_box_thread();
				blank_screen();
			} else {
				start_box_thread();
			}

			break;
		}
	}
}

void touchpaint_finger_up(int slot)
{
	pr_debug("finger %d up event from driver\n", slot);
	if (!init_done || !finger_down[slot])
		return;

	pr_debug("finger %d up\n", slot);
	finger_down[slot] = false;
	last_point[slot].x = 0;
	last_point[slot].y = 0;

	if (--fingers == 0 && mode == MODE_FILL) {
		mod_timer(&blank_timer, jiffies + msecs_to_jiffies(250));
	}
}

/*
 * Bresenham's line drawing algorithm
 * Source: https://rosettacode.org/wiki/Bitmap/Bresenham%27s_line_algorithm#C
 */
static void draw_line(int x1, int y1, int x2, int y2, u8 r, u8 g, u8 b)
{
	int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
	int dy = abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2;
	int err2;
	int x = x1, y = y1;

	while (true) {
		draw_point(x, y, brush_size, r, g, b);

		if (x == x2 && y == y2)
			break;
		
		err2 = err;
		if (err2 > -dx) {
			err -= dy;
			x += sx;
		}

		if (err2 < dy) {
			err += dx;
			y += sy;
		}
	}
}

void touchpaint_finger_point(int slot, int x, int y)
{
	if (mode != MODE_PAINT || !init_done || !finger_down[slot])
		return;

	draw_point(x, y, brush_size, 255, 255, 255);

	if (last_point[slot].x && last_point[slot].y)
		draw_line(last_point[slot].x, last_point[slot].y, x, y, 255, 255, 255);

	last_point[slot].x = x;
	last_point[slot].y = y;
}

static int __init touchpaint_init(void)
{
	fb_mem = ioremap_wc(fb_phys_addr, fb_max_size);
	if (!fb_mem) {
		pr_err("failed to map %zu-byte framebuffer at %pa!\n", fb_max_size,
		       &fb_phys_addr);
		return -ENOMEM;
	}

	fb_size = min((size_t)(fb_width * fb_height * 4), fb_max_size);

	pr_info("using %dx%d framebuffer spanning %zu bytes at %pa (mapped to %px)\n",
		fb_width, fb_height, fb_size, &fb_phys_addr, fb_mem);
	blank_screen();

	init_done = 1;
	return 0;
}
late_initcall_sync(touchpaint_init);
