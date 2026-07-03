// Desktop + Taskbar rendering — glassmorphism theme
//
// The wallpaper is a vertical gradient with soft accent "blobs": it must
// have visible variation, otherwise the frosted-glass blur under windows
// would be invisible. The taskbar is a blurred, tinted glass strip.

use crate::gui::compositor;
use crate::gui::window;
use crate::framebuffer::FONT_WIDTH;

const TASKBAR_HEIGHT: u32 = 32;
const TASKBAR_TEXT_COLOR: u32 = 0xE8ECF8;
const TASKBAR_BLUR_RADIUS: u32 = 8;

// Wallpaper gradient endpoints (top -> bottom)
const WALL_TOP: u32 = 0x101332;    // deep navy
const WALL_BOTTOM: u32 = 0x2A1A4E; // dark violet

static mut SCREEN_W: u32 = 0;
static mut SCREEN_H: u32 = 0;

pub unsafe fn desktop_init(screen_w: u32, screen_h: u32) {
    SCREEN_W = screen_w;
    SCREEN_H = screen_h;
}

#[inline(always)]
fn lerp_channel(a: u32, b: u32, num: u32, den: u32) -> u32 {
    (a * (den - num) + b * num) / den
}

/// Soft filled circle: alpha fades out towards the rim.
/// Direct back-buffer access — a per-pixel fill_rect_alpha call would be
/// far too slow for blobs this large.
unsafe fn draw_blob(cx: i32, cy: i32, radius: i32, color: u32, max_alpha: u32) {
    use crate::framebuffer::{read_pixel_buf, write_pixel_buf};
    let r2 = (radius * radius) as u32;
    let comp = compositor::compositor_get();
    if !comp.initialized {
        return;
    }
    let bpp = comp.bpp;
    let bpp_bytes = (bpp / 8) as usize;
    let pitch = comp.pitch as usize;
    let buf = comp.back_buffer.as_mut_ptr();
    let y0 = (cy - radius).max(0);
    let y1 = (cy + radius).min(comp.height as i32);
    let x0 = (cx - radius).max(0);
    let x1 = (cx + radius).min(comp.width as i32);
    // Reciprocal multiply instead of a per-pixel division (painted once,
    // but a 2M-pixel divide loop still stalls boot under TCG)
    let recip = (max_alpha << 15) / r2;
    for py in y0..y1 {
        let dy = py - cy;
        let row_base = buf.add(py as usize * pitch);
        for px in x0..x1 {
            let dx = px - cx;
            let d2 = (dx * dx + dy * dy) as u32;
            if d2 >= r2 {
                continue;
            }
            // alpha falls off quadratically towards the rim
            let a = ((r2 - d2) * recip) >> 15;
            if a == 0 {
                continue;
            }
            let p = row_base.add(px as usize * bpp_bytes);
            let dst = read_pixel_buf(p, bpp);
            write_pixel_buf(p, compositor::blend_px(dst, color, a), bpp);
        }
    }
}

pub unsafe fn desktop_draw_background() {
    // Fast path: backdrop is static, so after the first paint every compose
    // restores it from the wallpaper cache with one memcpy.
    if compositor::compositor_wallpaper_restore_full() {
        return;
    }
    extern "C" { fn serial_print(s: *const u8); }
    serial_print(b"[gui] wallpaper repaint\n\0".as_ptr());
    let h = SCREEN_H.max(1);
    // Vertical gradient, one fill per row
    for row in 0..SCREEN_H {
        let r = lerp_channel(WALL_TOP >> 16 & 0xFF, WALL_BOTTOM >> 16 & 0xFF, row, h);
        let g = lerp_channel(WALL_TOP >> 8 & 0xFF, WALL_BOTTOM >> 8 & 0xFF, row, h);
        let b = lerp_channel(WALL_TOP & 0xFF, WALL_BOTTOM & 0xFF, row, h);
        compositor::compositor_fill_rect(0, row as i32, SCREEN_W, 1, (r << 16) | (g << 8) | b);
    }
    // Accent blobs — light sources for the glass to diffuse
    let w = SCREEN_W as i32;
    let hh = SCREEN_H as i32;
    draw_blob(w / 5, hh / 4, hh / 3, 0x3D5AFE, 70);        // blue, upper left
    draw_blob(w * 4 / 5, hh / 3, hh / 4, 0xB447D6, 60);    // magenta, upper right
    draw_blob(w / 2, hh * 9 / 10, hh / 3, 0x1DE9B6, 45);   // teal, bottom center

    compositor::compositor_wallpaper_capture();
}

pub unsafe fn desktop_draw_taskbar() {
    let bar_y = SCREEN_H.saturating_sub(TASKBAR_HEIGHT) as i32;

    // Glass strip: blur what's behind, then tint
    compositor::compositor_blur_rect(0, bar_y, SCREEN_W, TASKBAR_HEIGHT, TASKBAR_BLUR_RADIUS);
    compositor::compositor_fill_rect_alpha(0, bar_y, SCREEN_W, TASKBAR_HEIGHT, 0x0A0F1E, 110);

    // Top hairline highlight
    compositor::compositor_fill_rect_alpha(0, bar_y, SCREEN_W, 1, 0xFFFFFF, 60);

    // "VernisOS" label on the left
    compositor::compositor_draw_string_transparent(8, bar_y + 8, b"VernisOS", 0x7FB4FF);

    // Window entries
    let wm = window::wm_get();
    let mut tx = 100i32; // start after the logo

    for i in 0..wm.z_order.len() {
        let wid = wm.z_order[i];
        if let Some(w) = wm.windows.iter().find(|w| w.id == wid) {
            if !w.visible {
                continue;
            }
            let btn_w = (w.title_len as u32 * FONT_WIDTH + 16).min(150);

            // Focused button: brighter glass pill
            if w.focused {
                compositor::compositor_fill_rect_alpha(
                    tx, bar_y + 4, btn_w, TASKBAR_HEIGHT - 8, 0xFFFFFF, 45,
                );
            }
            compositor::compositor_draw_string_transparent(
                tx + 4,
                bar_y + 8,
                &w.title[..w.title_len],
                TASKBAR_TEXT_COLOR,
            );

            tx += btn_w as i32 + 4;
        }
    }
}

/// Returns the taskbar area height.
pub fn taskbar_height() -> u32 {
    TASKBAR_HEIGHT
}

/// Hit-test taskbar: returns window ID to focus, or None.
pub unsafe fn taskbar_hit_test(mx: i32, my: i32) -> Option<u32> {
    let bar_y = SCREEN_H.saturating_sub(TASKBAR_HEIGHT) as i32;
    if my < bar_y || my >= SCREEN_H as i32 {
        return None;
    }

    let wm = window::wm_get();
    let mut tx = 100i32;

    for i in 0..wm.z_order.len() {
        let wid = wm.z_order[i];
        if let Some(w) = wm.windows.iter().find(|w| w.id == wid) {
            if !w.visible {
                continue;
            }
            let btn_w = (w.title_len as u32 * FONT_WIDTH + 16).min(150) as i32;
            if mx >= tx && mx < tx + btn_w {
                return Some(wid);
            }
            tx += btn_w + 4;
        }
    }
    None
}
