// Compositor — double-buffered rendering
// All GUI drawing goes through the back buffer, then present() copies to framebuffer

use core::ptr;
use alloc::vec::Vec;

use crate::font8x16::VGA_FONT_8X16;
use crate::framebuffer::{FONT_WIDTH, FONT_HEIGHT, write_pixel_buf, read_pixel_buf};

/// Blend src over dst with alpha 0..=255 (per-channel, 0xRRGGBB colors).
#[inline(always)]
pub fn blend_px(dst: u32, src: u32, alpha: u32) -> u32 {
    let inv = 255 - alpha;
    let r = ((src >> 16 & 0xFF) * alpha + (dst >> 16 & 0xFF) * inv) / 255;
    let g = ((src >> 8 & 0xFF) * alpha + (dst >> 8 & 0xFF) * inv) / 255;
    let b = ((src & 0xFF) * alpha + (dst & 0xFF) * inv) / 255;
    (r << 16) | (g << 8) | b
}

pub struct DirtyRect {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
}

impl DirtyRect {
    pub fn new(x: i32, y: i32, w: u32, h: u32) -> Self {
        DirtyRect { x, y, w, h }
    }

    /// Expand rect to include another rect (union).
    pub fn union(&mut self, other: &DirtyRect) {
        let x0 = if self.x < other.x { self.x } else { other.x };
        let y0 = if self.y < other.y { self.y } else { other.y };
        let x1 = {
            let self_x1 = self.x.saturating_add(self.w as i32);
            let other_x1 = other.x.saturating_add(other.w as i32);
            if self_x1 > other_x1 { self_x1 } else { other_x1 }
        };
        let y1 = {
            let self_y1 = self.y.saturating_add(self.h as i32);
            let other_y1 = other.y.saturating_add(other.h as i32);
            if self_y1 > other_y1 { self_y1 } else { other_y1 }
        };
        self.x = x0;
        self.y = y0;
        self.w = (x1 - x0) as u32;
        self.h = (y1 - y0) as u32;
    }
}

// The back buffer lives in a dedicated identity-mapped physical region, NOT
// the Rust heap: the buddy allocator rounds allocations up to a power of two,
// so a ~6-8MB buffer would demand an 8-16MB block the 8MB kernel heap can
// never provide. Region: 48MB..64MB physical (16MB), far above the kernel
// image/BSS (~9.3MB), the frame allocator pool (ends ~13.3MB), and the kernel
// stack (15MB); well inside the 128MB identity map and the 64MB minimum RAM.
const BACK_BUFFER_PHYS: usize = 0x0300_0000;
const BACK_BUFFER_MAX: usize = 16 * 1024 * 1024;

/// Raw-pointer back buffer with a Vec-like accessor surface.
pub struct BackBuffer(pub *mut u8);
impl BackBuffer {
    #[inline(always)]
    pub fn as_mut_ptr(&self) -> *mut u8 { self.0 }
    #[inline(always)]
    pub fn as_ptr(&self) -> *const u8 { self.0 }
}

pub struct Compositor {
    pub back_buffer: BackBuffer,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,   // bytes per scanline in back buffer
    pub bpp: u32,     // bits per pixel
    pub initialized: bool,
    pub dirty: bool,
    pub dirty_rect: DirtyRect,  // Bounding box of changed region
}

static mut COMP: Compositor = Compositor {
    back_buffer: BackBuffer(core::ptr::null_mut()),
    width: 0,
    height: 0,
    pitch: 0,
    bpp: 0,
    initialized: false,
    dirty: false,
    dirty_rect: DirtyRect { x: 0, y: 0, w: 0, h: 0 },
};

pub unsafe fn compositor_get() -> &'static mut Compositor {
    &mut COMP
}

pub unsafe fn compositor_init(width: u32, height: u32, bpp: u32) {
    let bpp_bytes = bpp / 8;
    let pitch = width * bpp_bytes;
    let size = (pitch * height) as usize;
    if size > BACK_BUFFER_MAX {
        return; // mode too large for the reserved region — GUI stays off
    }

    COMP.back_buffer = BackBuffer(BACK_BUFFER_PHYS as *mut u8);
    ptr::write_bytes(COMP.back_buffer.as_mut_ptr(), 0, size);
    COMP.width = width;
    COMP.height = height;
    COMP.pitch = pitch;
    COMP.bpp = bpp;
    COMP.initialized = true;
    COMP.dirty = true;
    COMP.dirty_rect = DirtyRect::new(0, 0, width, height);  // Full screen initially
}

pub unsafe fn compositor_mark_dirty() {
    COMP.dirty = true;
}

pub unsafe fn compositor_mark_dirty_rect(x: i32, y: i32, w: u32, h: u32) {
    if !COMP.initialized {
        return;
    }
    COMP.dirty = true;
    if COMP.dirty_rect.w == 0 || COMP.dirty_rect.h == 0 {
        // First dirty region
        COMP.dirty_rect = DirtyRect::new(x, y, w, h);
    } else {
        // Union with existing dirty rect
        let new_rect = DirtyRect::new(x, y, w, h);
        COMP.dirty_rect.union(&new_rect);
    }
}

pub unsafe fn compositor_is_dirty() -> bool {
    COMP.dirty
}

pub unsafe fn compositor_reset_dirty() {
    COMP.dirty = false;
    COMP.dirty_rect = DirtyRect::new(0, 0, 0, 0);
}


pub unsafe fn compositor_clear(color: u32) {
    if !COMP.initialized {
        return;
    }
    COMP.dirty = true;
    compositor_mark_dirty_rect(0, 0, COMP.width, COMP.height);  // Full screen dirty
    let total = (COMP.height * COMP.pitch) as usize;
    let buf = COMP.back_buffer.as_mut_ptr();
    if color == 0 {
        ptr::write_bytes(buf, 0, total);
    } else if COMP.bpp == 24 {
        // Fill 24bpp: write 3 bytes per pixel, but optimize with a repeating pattern
        let b0 = color as u8;
        let b1 = (color >> 8) as u8;
        let b2 = (color >> 16) as u8;
        // Build a 12-byte repeating pattern (LCM of 3 and 4)
        let pattern: [u8; 12] = [b0,b1,b2, b0,b1,b2, b0,b1,b2, b0,b1,b2];
        let row_bytes = (COMP.width * 3) as usize;
        for row in 0..COMP.height {
            let row_base = buf.add((row * COMP.pitch) as usize);
            // Fill 12 bytes at a time
            let mut off = 0usize;
            while off + 12 <= row_bytes {
                ptr::copy_nonoverlapping(pattern.as_ptr(), row_base.add(off), 12);
                off += 12;
            }
            // Remainder
            while off + 3 <= row_bytes {
                *row_base.add(off) = b0;
                *row_base.add(off + 1) = b1;
                *row_base.add(off + 2) = b2;
                off += 3;
            }
        }
    } else {
        // 32bpp: fill as u32
        let buf32 = buf as *mut u32;
        let count = total / 4;
        for i in 0..count {
            ptr::write(buf32.add(i), color);
        }
    }
}

pub unsafe fn compositor_fill_rect(x: i32, y: i32, w: u32, h: u32, color: u32) {
    if !COMP.initialized {
        return;
    }
    COMP.dirty = true;
    compositor_mark_dirty_rect(x, y, w, h);  // Track this rect as changed
    let bpp_bytes = COMP.bpp / 8;
    let buf = COMP.back_buffer.as_mut_ptr();

    let x0 = if x < 0 { 0 } else { x as u32 };
    let y0 = if y < 0 { 0 } else { y as u32 };
    let x_end = if (x + w as i32) < 0 {
        0u32
    } else {
        let xe = (x + w as i32) as u32;
        if xe > COMP.width { COMP.width } else { xe }
    };
    let y_end = if (y + h as i32) < 0 {
        0u32
    } else {
        let ye = (y + h as i32) as u32;
        if ye > COMP.height { COMP.height } else { ye }
    };

    if x0 >= x_end || y0 >= y_end {
        return;
    }

    let cols = x_end - x0;

    if color == 0 {
        // Fast memset zero
        let row_bytes = (cols * bpp_bytes) as usize;
        for row in y0..y_end {
            let dst = buf.add((row * COMP.pitch + x0 * bpp_bytes) as usize);
            ptr::write_bytes(dst, 0, row_bytes);
        }
    } else if bpp_bytes == 3 {
        // 24bpp: build one template row, memcpy to the rest
        let row0_base = buf.add((y0 * COMP.pitch + x0 * 3) as usize);
        let b0 = color as u8;
        let b1 = (color >> 8) as u8;
        let b2 = (color >> 16) as u8;
        // Fill first row pixel by pixel
        for col in 0..cols {
            let off = (col * 3) as usize;
            *row0_base.add(off) = b0;
            *row0_base.add(off + 1) = b1;
            *row0_base.add(off + 2) = b2;
        }
        // Copy first row to remaining rows
        let row_bytes = (cols * 3) as usize;
        for row in (y0 + 1)..y_end {
            let dst = buf.add((row * COMP.pitch + x0 * 3) as usize);
            ptr::copy_nonoverlapping(row0_base, dst, row_bytes);
        }
    } else {
        // 32bpp: fill as u32, first row then memcpy
        let row0_base = buf.add((y0 * COMP.pitch + x0 * 4) as usize) as *mut u32;
        for col in 0..cols {
            ptr::write(row0_base.add(col as usize), color);
        }
        let row_bytes = (cols * 4) as usize;
        let row0_src = row0_base as *const u8;
        for row in (y0 + 1)..y_end {
            let dst = buf.add((row * COMP.pitch + x0 * 4) as usize);
            ptr::copy_nonoverlapping(row0_src, dst, row_bytes);
        }
    }
}

pub unsafe fn compositor_draw_char(x: i32, y: i32, ch: u8, fg: u32, bg: u32) {
    if !COMP.initialized || x >= COMP.width as i32 || y >= COMP.height as i32 {
        return;
    }
    COMP.dirty = true;
    // Mark the 8x16 character region as dirty
    compositor_mark_dirty_rect(x, y, FONT_WIDTH, FONT_HEIGHT);
    
    let bpp_bytes = COMP.bpp / 8;
    let buf = COMP.back_buffer.as_mut_ptr();
    let glyph_offset = (ch as usize) * (FONT_HEIGHT as usize);
    let font_len = VGA_FONT_8X16.len();

    for row in 0..FONT_HEIGHT {
        let py = y + row as i32;
        if py < 0 {
            continue;
        }
        if py >= COMP.height as i32 {
            break;
        }
        let idx = glyph_offset + row as usize;
        let glyph_row = if idx < font_len { VGA_FONT_8X16[idx] } else { 0 };
        let row_base = buf.add((py as u32 * COMP.pitch) as usize);

        for col in 0..FONT_WIDTH {
            let px = x + col as i32;
            if px < 0 {
                continue;
            }
            if px >= COMP.width as i32 {
                break;
            }
            let color = if (glyph_row >> (7 - col)) & 1 != 0 {
                fg
            } else {
                bg
            };
            write_pixel_buf(
                row_base.add((px as u32 * bpp_bytes) as usize),
                color,
                COMP.bpp,
            );
        }
    }
}

pub unsafe fn compositor_draw_string(x: i32, y: i32, s: &[u8], fg: u32, bg: u32) {
    let mut cx = x;
    for &ch in s {
        if ch == 0 {
            break;
        }
        compositor_draw_char(cx, y, ch, fg, bg);
        cx += FONT_WIDTH as i32;
    }
}

/// Blit a source pixel buffer onto the compositor back buffer.
/// src_pitch is the byte stride of the source.
pub unsafe fn compositor_blit(
    src: *const u8,
    dst_x: i32,
    dst_y: i32,
    w: u32,
    h: u32,
    src_pitch: u32,
) {
    if !COMP.initialized || src.is_null() {
        return;
    }
    COMP.dirty = true;
    let bpp_bytes = COMP.bpp / 8;
    let buf = COMP.back_buffer.as_mut_ptr();

    // Clip to screen bounds
    let src_start_col = if dst_x < 0 { (-dst_x) as u32 } else { 0 };
    let src_start_row = if dst_y < 0 { (-dst_y) as u32 } else { 0 };
    let dst_x0 = if dst_x < 0 { 0i32 } else { dst_x };
    let dst_y0 = if dst_y < 0 { 0i32 } else { dst_y };

    let visible_w = w.saturating_sub(src_start_col);
    let visible_h = h.saturating_sub(src_start_row);
    let clip_w = if (dst_x0 as u32 + visible_w) > COMP.width {
        COMP.width - dst_x0 as u32
    } else {
        visible_w
    };
    let clip_h = if (dst_y0 as u32 + visible_h) > COMP.height {
        COMP.height - dst_y0 as u32
    } else {
        visible_h
    };

    if clip_w == 0 || clip_h == 0 {
        return;
    }

    let row_bytes = (clip_w * bpp_bytes) as usize;

    for row in 0..clip_h {
        let src_row = src.add(((src_start_row + row) * src_pitch + src_start_col * bpp_bytes) as usize);
        let dst_row = buf.add(((dst_y0 as u32 + row) * COMP.pitch + dst_x0 as u32 * bpp_bytes) as usize);
        ptr::copy_nonoverlapping(src_row, dst_row, row_bytes);
    }
}

/// Copy the entire back buffer to the framebuffer. Skip if not dirty.
pub unsafe fn compositor_present() {
    if !COMP.initialized || !COMP.dirty || COMP.dirty_rect.w == 0 || COMP.dirty_rect.h == 0 {
        return;
    }
    
    // Use dirty rect, not full screen
    let bpp_bytes = COMP.bpp / 8;
    let blit_x = if COMP.dirty_rect.x < 0 { 0 } else { COMP.dirty_rect.x as u32 };
    let blit_y = if COMP.dirty_rect.y < 0 { 0 } else { COMP.dirty_rect.y as u32 };
    
    let blit_x_end = {
        let xe = COMP.dirty_rect.x.saturating_add(COMP.dirty_rect.w as i32);
        let xe_u = if xe < 0 { 0 } else { xe as u32 };
        if xe_u > COMP.width { COMP.width } else { xe_u }
    };
    
    let blit_y_end = {
        let ye = COMP.dirty_rect.y.saturating_add(COMP.dirty_rect.h as i32);
        let ye_u = if ye < 0 { 0 } else { ye as u32 };
        if ye_u > COMP.height { COMP.height } else { ye_u }
    };
    
    if blit_x >= blit_x_end || blit_y >= blit_y_end {
        COMP.dirty = false;
        COMP.dirty_rect = DirtyRect::new(0, 0, 0, 0);
        return;
    }
    
    let blit_w = blit_x_end - blit_x;
    let blit_h = blit_y_end - blit_y;
    
    let src = COMP.back_buffer.as_ptr()
        .add((blit_y * COMP.pitch + blit_x * bpp_bytes) as usize);
    
    use crate::framebuffer::fb_blit;
    fb_blit(src, blit_x, blit_y, blit_w, blit_h, COMP.pitch);
    
    COMP.dirty = false;
    COMP.dirty_rect = DirtyRect::new(0, 0, 0, 0);
}

// =============================================================================
// Glassmorphism primitives — blur, alpha fill, color-key blit, transparent text
// =============================================================================

/// Clip helper: (x, y, w, h) in screen space -> (x0, y0, x1, y1) or None.
unsafe fn clip_rect(x: i32, y: i32, w: u32, h: u32) -> Option<(u32, u32, u32, u32)> {
    let x0 = if x < 0 { 0 } else { x as u32 };
    let y0 = if y < 0 { 0 } else { y as u32 };
    let x1 = {
        let xe = x.saturating_add(w as i32);
        if xe <= 0 { return None; }
        (xe as u32).min(COMP.width)
    };
    let y1 = {
        let ye = y.saturating_add(h as i32);
        if ye <= 0 { return None; }
        (ye as u32).min(COMP.height)
    };
    if x0 >= x1 || y0 >= y1 { return None; }
    Some((x0, y0, x1, y1))
}

/// Scratch line buffer for the separable box blur (one row or column).
static mut BLUR_SCRATCH: Vec<u32> = Vec::new();

/// Box-blur a back-buffer region in place (separable: one horizontal +
/// one vertical pass, sliding-window sums). This is what makes windows
/// "frosted": call it on the backdrop before tinting the glass.
pub unsafe fn compositor_blur_rect(x: i32, y: i32, w: u32, h: u32, radius: u32) {
    if !COMP.initialized || radius == 0 {
        return;
    }
    let Some((x0, y0, x1, y1)) = clip_rect(x, y, w, h) else { return; };
    let rw = (x1 - x0) as usize;
    let rh = (y1 - y0) as usize;
    let r = radius as usize;
    let bpp = COMP.bpp;
    let bpp_bytes = (bpp / 8) as usize;
    let buf = COMP.back_buffer.as_mut_ptr();
    let pitch = COMP.pitch as usize;

    let need = rw.max(rh);
    if BLUR_SCRATCH.len() < need {
        BLUR_SCRATCH.resize(need, 0);
    }
    let line = BLUR_SCRATCH.as_mut_ptr();

    // Horizontal pass
    for row in 0..rh {
        let row_base = buf.add((y0 as usize + row) * pitch + x0 as usize * bpp_bytes);
        for i in 0..rw {
            *line.add(i) = read_pixel_buf(row_base.add(i * bpp_bytes), bpp);
        }
        let (mut sr, mut sg, mut sb) = (0u32, 0u32, 0u32);
        let mut count = 0u32;
        // Prime window [0, r]
        for i in 0..=(r.min(rw - 1)) {
            let p = *line.add(i);
            sr += p >> 16 & 0xFF; sg += p >> 8 & 0xFF; sb += p & 0xFF;
            count += 1;
        }
        for i in 0..rw {
            let out = ((sr / count) << 16) | ((sg / count) << 8) | (sb / count);
            write_pixel_buf(row_base.add(i * bpp_bytes), out, bpp);
            // Slide window: add i+r+1, remove i-r
            let add_i = i + r + 1;
            if add_i < rw {
                let p = *line.add(add_i);
                sr += p >> 16 & 0xFF; sg += p >> 8 & 0xFF; sb += p & 0xFF;
                count += 1;
            }
            if i >= r {
                let p = *line.add(i - r);
                sr -= p >> 16 & 0xFF; sg -= p >> 8 & 0xFF; sb -= p & 0xFF;
                count -= 1;
            }
        }
    }

    // Vertical pass
    for col in 0..rw {
        let col_base = buf.add(y0 as usize * pitch + (x0 as usize + col) * bpp_bytes);
        for i in 0..rh {
            *line.add(i) = read_pixel_buf(col_base.add(i * pitch), bpp);
        }
        let (mut sr, mut sg, mut sb) = (0u32, 0u32, 0u32);
        let mut count = 0u32;
        for i in 0..=(r.min(rh - 1)) {
            let p = *line.add(i);
            sr += p >> 16 & 0xFF; sg += p >> 8 & 0xFF; sb += p & 0xFF;
            count += 1;
        }
        for i in 0..rh {
            let out = ((sr / count) << 16) | ((sg / count) << 8) | (sb / count);
            write_pixel_buf(col_base.add(i * pitch), out, bpp);
            let add_i = i + r + 1;
            if add_i < rh {
                let p = *line.add(add_i);
                sr += p >> 16 & 0xFF; sg += p >> 8 & 0xFF; sb += p & 0xFF;
                count += 1;
            }
            if i >= r {
                let p = *line.add(i - r);
                sr -= p >> 16 & 0xFF; sg -= p >> 8 & 0xFF; sb -= p & 0xFF;
                count -= 1;
            }
        }
    }

    compositor_mark_dirty_rect(x, y, w, h);
    COMP.dirty = true;
}

/// Alpha-blend a solid color over a back-buffer rect (the glass tint).
pub unsafe fn compositor_fill_rect_alpha(x: i32, y: i32, w: u32, h: u32, color: u32, alpha: u32) {
    if !COMP.initialized {
        return;
    }
    let Some((x0, y0, x1, y1)) = clip_rect(x, y, w, h) else { return; };
    let bpp = COMP.bpp;
    let bpp_bytes = (bpp / 8) as usize;
    let buf = COMP.back_buffer.as_mut_ptr();
    for row in y0..y1 {
        let row_base = buf.add((row * COMP.pitch) as usize + x0 as usize * bpp_bytes);
        for col in 0..(x1 - x0) as usize {
            let p = row_base.add(col * bpp_bytes);
            let dst = read_pixel_buf(p, bpp);
            write_pixel_buf(p, blend_px(dst, color, alpha), bpp);
        }
    }
    compositor_mark_dirty_rect(x, y, w, h);
    COMP.dirty = true;
}

/// Blit with a transparent color key: source pixels equal to `key` are
/// skipped, letting the glass backdrop show through (terminal cell
/// backgrounds are pure black = key).
pub unsafe fn compositor_blit_colorkey(
    src: *const u8,
    dst_x: i32,
    dst_y: i32,
    w: u32,
    h: u32,
    src_pitch: u32,
    key: u32,
) {
    if !COMP.initialized || src.is_null() {
        return;
    }
    let Some((x0, y0, x1, y1)) = clip_rect(dst_x, dst_y, w, h) else { return; };
    let bpp = COMP.bpp;
    let bpp_bytes = (bpp / 8) as usize;
    let buf = COMP.back_buffer.as_mut_ptr();
    let src_x_off = (x0 as i32 - dst_x) as usize;
    let src_y_off = (y0 as i32 - dst_y) as usize;

    for row in 0..(y1 - y0) as usize {
        let src_row = src.add((src_y_off + row) * src_pitch as usize + src_x_off * bpp_bytes);
        let dst_row = buf.add(((y0 as usize + row) * COMP.pitch as usize) + x0 as usize * bpp_bytes);
        for col in 0..(x1 - x0) as usize {
            let px = read_pixel_buf(src_row.add(col * bpp_bytes), bpp);
            if px != key {
                write_pixel_buf(dst_row.add(col * bpp_bytes), px, bpp);
            }
        }
    }
    compositor_mark_dirty_rect(dst_x, dst_y, w, h);
    COMP.dirty = true;
}

/// Draw a character with transparent background (only glyph pixels).
pub unsafe fn compositor_draw_char_transparent(x: i32, y: i32, ch: u8, fg: u32) {
    if !COMP.initialized || x >= COMP.width as i32 || y >= COMP.height as i32 {
        return;
    }
    COMP.dirty = true;
    compositor_mark_dirty_rect(x, y, FONT_WIDTH, FONT_HEIGHT);
    let bpp_bytes = COMP.bpp / 8;
    let buf = COMP.back_buffer.as_mut_ptr();
    let glyph_offset = (ch as usize) * (FONT_HEIGHT as usize);
    let font_len = VGA_FONT_8X16.len();

    for row in 0..FONT_HEIGHT {
        let py = y + row as i32;
        if py < 0 { continue; }
        if py >= COMP.height as i32 { break; }
        let idx = glyph_offset + row as usize;
        let glyph_row = if idx < font_len { VGA_FONT_8X16[idx] } else { 0 };
        let row_base = buf.add((py as u32 * COMP.pitch) as usize);
        for col in 0..FONT_WIDTH {
            let px = x + col as i32;
            if px < 0 { continue; }
            if px >= COMP.width as i32 { break; }
            if (glyph_row >> (7 - col)) & 1 != 0 {
                write_pixel_buf(row_base.add((px as u32 * bpp_bytes) as usize), fg, COMP.bpp);
            }
        }
    }
}

/// Draw a string with transparent background.
pub unsafe fn compositor_draw_string_transparent(x: i32, y: i32, s: &[u8], fg: u32) {
    let mut cx = x;
    for &ch in s {
        if ch == 0 { break; }
        compositor_draw_char_transparent(cx, y, ch, fg);
        cx += FONT_WIDTH as i32;
    }
}

/// Copy a clipped rectangle from back buffer to framebuffer.
/// Used by cursor-only fast path to avoid full-frame presents.
pub unsafe fn compositor_present_rect(x: i32, y: i32, w: u32, h: u32) {
    if !COMP.initialized || w == 0 || h == 0 {
        return;
    }

    let bpp_bytes = COMP.bpp / 8;

    let x0 = if x < 0 { 0 } else { x as u32 };
    let y0 = if y < 0 { 0 } else { y as u32 };

    if x0 >= COMP.width || y0 >= COMP.height {
        return;
    }

    let x_end = {
        let xe = x.saturating_add(w as i32);
        if xe <= 0 {
            0
        } else {
            let xe_u = xe as u32;
            if xe_u > COMP.width { COMP.width } else { xe_u }
        }
    };

    let y_end = {
        let ye = y.saturating_add(h as i32);
        if ye <= 0 {
            0
        } else {
            let ye_u = ye as u32;
            if ye_u > COMP.height { COMP.height } else { ye_u }
        }
    };

    if x_end <= x0 || y_end <= y0 {
        return;
    }

    let clip_w = x_end - x0;
    let clip_h = y_end - y0;
    let src = COMP
        .back_buffer
        .as_ptr()
        .add((y0 * COMP.pitch + x0 * bpp_bytes) as usize);

    crate::framebuffer::fb_blit(src, x0, y0, clip_w, clip_h, COMP.pitch);
}
