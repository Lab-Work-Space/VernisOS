// Start bar / start menu + a small app registry.
//
// The taskbar's left "VernisOS" label doubles as a Start button. Clicking it
// toggles a glass menu listing registered apps and system-maintenance
// actions. Programs register themselves via gui_register_app() (FFI) or
// register_app() (Rust) — that's the "register a program" facility.

use crate::gui::compositor;

pub const START_BTN_W: i32 = 96;   // width of the Start button in the taskbar
const ITEM_H: i32 = 24;
const MENU_W: i32 = 210;
const PAD: i32 = 6;
const MAX_APPS: usize = 16;
const LABEL_MAX: usize = 22;

// Action codes (also the FFI contract for gui_register_app):
pub const ACT_TERMINAL: u8 = 0; // open / focus the terminal
pub const ACT_RESTART: u8 = 3;  // reboot
pub const ACT_SHUTDOWN: u8 = 4; // power off

#[derive(Copy, Clone)]
struct AppEntry {
    label: [u8; LABEL_MAX],
    len: usize,
    action: u8,
    used: bool,
}

impl AppEntry {
    const fn empty() -> Self {
        AppEntry { label: [0; LABEL_MAX], len: 0, action: 0, used: false }
    }
}

struct StartMenu {
    apps: [AppEntry; MAX_APPS],
    count: usize,
    open: bool,
    dirty: bool, // menu appearance changed since last compose
}

static mut MENU: StartMenu = StartMenu {
    apps: [AppEntry::empty(); MAX_APPS],
    count: 0,
    open: false,
    dirty: false,
};

/// Register an app/action in the start menu.
pub unsafe fn register_app(label: &[u8], action: u8) {
    if MENU.count >= MAX_APPS {
        return;
    }
    let e = &mut MENU.apps[MENU.count];
    let n = if label.len() > LABEL_MAX { LABEL_MAX } else { label.len() };
    e.label[..n].copy_from_slice(&label[..n]);
    e.len = n;
    e.action = action;
    e.used = true;
    MENU.count += 1;
}

/// FFI: register an app from C (NUL-terminated label + action code).
#[no_mangle]
pub unsafe extern "C" fn gui_register_app(label: *const u8, action: u8) {
    if label.is_null() {
        return;
    }
    let mut n = 0usize;
    while n < LABEL_MAX && *label.add(n) != 0 {
        n += 1;
    }
    let slice = core::slice::from_raw_parts(label, n);
    register_app(slice, action);
}

/// Seed the default apps + system-maintenance entries.
pub unsafe fn startmenu_init() {
    MENU.count = 0;
    MENU.open = false;
    MENU.dirty = false;
    register_app(b"Terminal", ACT_TERMINAL);
    register_app(b"Restart", ACT_RESTART);
    register_app(b"Shut Down", ACT_SHUTDOWN);
}

pub unsafe fn is_open() -> bool {
    MENU.open
}

/// Take + clear the "menu appearance changed" flag (drives a full compose).
pub unsafe fn take_dirty() -> bool {
    let d = MENU.dirty;
    MENU.dirty = false;
    d
}

pub unsafe fn close() {
    if MENU.open {
        MENU.open = false;
        MENU.dirty = true;
    }
}

fn menu_geom(screen_w: u32, screen_h: u32, taskbar_h: u32, count: usize) -> (i32, i32, i32, i32) {
    let _ = screen_w;
    let bar_y = screen_h.saturating_sub(taskbar_h) as i32;
    let h = count as i32 * ITEM_H + PAD * 2;
    let x = 4;
    let y = bar_y - h;
    (x, y, MENU_W, h)
}

/// Is (mx,my) on the Start button in the taskbar?
pub unsafe fn hit_start_button(mx: i32, my: i32, screen_h: u32, taskbar_h: u32) -> bool {
    let bar_y = screen_h.saturating_sub(taskbar_h) as i32;
    my >= bar_y && my < screen_h as i32 && mx >= 0 && mx < START_BTN_W
}

/// Toggle the menu (called when the Start button is clicked).
pub unsafe fn toggle() {
    MENU.open = !MENU.open;
    MENU.dirty = true;
}

/// If the menu is open and (mx,my) hit an item, return its action code and
/// close the menu. Returns 0xFF if the click was inside the open menu but not
/// on an item (swallow it), or None if outside/closed.
pub unsafe fn hit_item(mx: i32, my: i32, screen_w: u32, screen_h: u32, taskbar_h: u32) -> Option<u8> {
    if !MENU.open {
        return None;
    }
    let (x, y, w, h) = menu_geom(screen_w, screen_h, taskbar_h, MENU.count);
    if mx < x || mx >= x + w || my < y || my >= y + h {
        return None; // outside menu
    }
    let idx = (my - y - PAD) / ITEM_H;
    if idx >= 0 && (idx as usize) < MENU.count {
        let action = MENU.apps[idx as usize].action;
        MENU.open = false;
        MENU.dirty = true;
        return Some(action);
    }
    Some(0xFF) // inside panel, between items — swallow
}

/// Draw the Start button highlight + the menu panel (if open). Called at the
/// end of desktop_draw_taskbar so it lands on top of everything.
pub unsafe fn draw(screen_w: u32, screen_h: u32, taskbar_h: u32) {
    let bar_y = screen_h.saturating_sub(taskbar_h) as i32;

    // Start button: brighter glass pill when the menu is open
    if MENU.open {
        compositor::compositor_fill_rect_alpha(0, bar_y, START_BTN_W as u32, taskbar_h, 0xFFFFFF, 40);
    }
    // A little "≡" glyph before the label to read as a button
    compositor::compositor_draw_string_transparent(8, bar_y + 8, b"= VernisOS", 0x7FB4FF);

    if !MENU.open {
        return;
    }
    let (x, y, w, h) = menu_geom(screen_w, screen_h, taskbar_h, MENU.count);
    // Glass panel: frost + tint + edge
    compositor::compositor_blur_rect(x, y, w as u32, h as u32, 9);
    compositor::compositor_fill_rect_alpha(x, y, w as u32, h as u32, 0x0E1626, 165);
    compositor::compositor_fill_rect_alpha(x, y, w as u32, 1, 0xFFFFFF, 90);
    compositor::compositor_fill_rect_alpha(x, y, 1, h as u32, 0xFFFFFF, 60);

    for i in 0..MENU.count {
        let iy = y + PAD + i as i32 * ITEM_H;
        let e = &MENU.apps[i];
        // separator tint above the maintenance actions (Restart/Shut Down)
        if e.action == ACT_RESTART {
            compositor::compositor_fill_rect_alpha(x + 6, iy - 1, (w - 12) as u32, 1, 0xFFFFFF, 40);
        }
        let color = if e.action == ACT_RESTART || e.action == ACT_SHUTDOWN { 0xFF9E9E } else { 0xE8ECF8 };
        compositor::compositor_draw_string_transparent(x + 12, iy + 4, &e.label[..e.len], color);
    }
}
