// GUI Main Module — orchestrates all GUI subsystems

pub mod event;
pub mod compositor;
pub mod window;
pub mod desktop;
pub mod cursor;
pub mod widgets;
pub mod terminal;

use crate::mouse;

extern "C" {
    fn kernel_get_ticks() -> u32;
    fn kernel_get_timer_hz() -> u32;
    fn cli_gui_tick();
    fn serial_print(s: *const u8);
    fn serial_print_dec(v: u32);
}

/// Previous mouse button state for detecting press/release edges.
static mut PREV_BUTTONS: u8 = 0;
static mut LAST_FRAME_TICK: u32 = 0;
static mut LAST_CURSOR_X: i32 = -1;  // Cache cursor position to skip redundant renders
static mut LAST_CURSOR_Y: i32 = -1;
static mut LAST_FULL_COMPOSE_TICK: u32 = 0;  // drag-throttle for full composes
static mut LAST_COMPOSE_PATH: u8 = 0;        // telemetry: which branch composed

const GUI_MAX_EVENTS_PER_TICK: usize = 64;

// Auto Hz: render cadence follows the kernel timer — one frame opportunity
// per tick, whatever frequency the PIT is programmed to (240Hz today).
// Dirty-checks below keep idle ticks free.
#[inline(always)]
unsafe fn gui_frame_interval_ticks() -> u32 {
    let hz = kernel_get_timer_hz();
    if hz == 0 { return 1; }
    1
}

/// Initialize all GUI subsystems.
#[no_mangle]
pub unsafe extern "C" fn gui_init(screen_w: u32, screen_h: u32) {
    let bpp = crate::framebuffer::fb_get_bpp();

    // Init compositor (allocates back buffer)
    compositor::compositor_init(screen_w, screen_h, bpp);

    // Init window manager
    window::wm_init(bpp);

    // Init desktop
    desktop::desktop_init(screen_w, screen_h);

    // Create terminal window centered on screen
    let term_w = 648u32; // 80 cols * 8 + 2*border(1) + padding = ~642+6
    let term_h = 680u32; // 40 rows * 16 + titlebar(24) + border(1) = ~665
    let tx = ((screen_w.saturating_sub(term_w)) / 2) as i32;
    let ty = ((screen_h.saturating_sub(term_h + desktop::taskbar_height())) / 2) as i32;
    terminal::terminal_create(tx, ty);

    // Show welcome message in terminal
    terminal::terminal_set_color(10, 0);
    terminal::terminal_write_string(b"VernisOS GUI Terminal\n");
    terminal::terminal_set_color(7, 0);
    terminal::terminal_write_string(b"Type 'help' for available commands.\n\n");
    terminal::terminal_show_prompt();

    PREV_BUTTONS = 0;
    LAST_FRAME_TICK = kernel_get_ticks();
    LAST_CURSOR_X = -1;
    LAST_CURSOR_Y = -1;
}

/// Main loop tick — called repeatedly from kernel main loop.
/// Processes events, updates state, composes frame, presents to screen.
/// Skips rendering when nothing has changed (idle optimization).
#[no_mangle]
pub unsafe extern "C" fn gui_main_loop_tick() {
    // Allow C-side CLI background tasks (e.g. ps realtime in GUI terminal)
    // to update without blocking this render loop.
    cli_gui_tick();

    // CRITICAL: Throttle to GUI_TARGET_FPS to sync with PIT timer
    // Without this, loop runs ahead of framebuffer capacity, causing stutter
    let current_tick = kernel_get_ticks();
    let ticks_since_last = current_tick.wrapping_sub(LAST_FRAME_TICK);
    let frame_interval = gui_frame_interval_ticks();
    if ticks_since_last < frame_interval {
        // Not enough time has passed—skip this frame
        return;
    }
    LAST_FRAME_TICK = current_tick;

    let mut had_events = false;
    let mut had_mouse_move = false;
    let mut had_mouse_button = false;
    let mut had_key = false;
    let mut processed_events = 0usize;

    // 1. Process all queued events
    while processed_events < GUI_MAX_EVENTS_PER_TICK {
        let Some(ev) = event::event_pop() else { break; };
        processed_events += 1;
        had_events = true;
        match ev.kind {
            event::EventKind::MouseDown => {
                had_mouse_button = true;
                handle_mouse_down(ev.mouse_x, ev.mouse_y, ev.button);
            }
            event::EventKind::MouseUp => {
                had_mouse_button = true;
                handle_mouse_up(ev.mouse_x, ev.mouse_y);
            }
            event::EventKind::MouseMove => {
                had_mouse_move = true;
                handle_mouse_move(ev.mouse_x, ev.mouse_y);
            }
            event::EventKind::KeyPress => {
                had_key = true;
                handle_key_press(ev.scancode);
            }
            event::EventKind::KeyRelease | event::EventKind::None => {}
        }
    }

    // 2. Render terminal content (only if dirty — skips internally)
    let term_was_dirty = terminal::terminal_get().dirty;
    if term_was_dirty {
        terminal::terminal_render();
    }

    // 3. Only compose and present if something changed. wm_windows_dirty()
    //    must be included: a drag-throttled tick can leave layout changes
    //    pending with no new events.
    let need_compose = had_events || term_was_dirty || window::wm_windows_dirty();
    if !need_compose {
        return;
    }

    // Fast path: only mouse movement (no dragging / no terminal updates)
    // -> update cursor incrementally and present only dirty cursor region.
    let cursor_only = had_mouse_move
        && !had_mouse_button
        && !had_key
        && !term_was_dirty
        && !window::wm_is_dragging()
        && !window::wm_windows_dirty();

    if cursor_only {
        let mx = mouse::mouse_get_x();
        let my = mouse::mouse_get_y();
                // Skip redundant cursor renders for same position during frame skip intervals
                // (multiple move events can queue up while we're throttled)
                if mx == LAST_CURSOR_X && my == LAST_CURSOR_Y {
                    return;
                }
                LAST_CURSOR_X = mx;
                LAST_CURSOR_Y = my;
        if let Some((dx, dy, dw, dh)) = cursor::cursor_move_incremental(mx, my) {
            compositor::compositor_present_rect(dx, dy, dw, dh);
        }
        return;
    }

    // Glass must always be re-blurred from a fresh backdrop (re-blurring a
    // composited frame compounds the blur). Two tiers:
    //  - layout change (click/drag/focus/close): full compose, throttled
    //    to ~60fps while dragging
    //  - typing / terminal output only: partial compose — restore the
    //    wallpaper under the window, re-frost just that window, present
    //    just that rect (avoids the 2Mpx repaint + 8MB MMIO copy per key)
    let layout_dirty = had_mouse_button || window::wm_windows_dirty();
    if layout_dirty {
        let dragging = window::wm_is_dragging();
        if dragging && current_tick.wrapping_sub(LAST_FULL_COMPOSE_TICK) < 4 {
            return; // wins_dirty stays set; a later tick composes the final state
        }
        // While dragging: tint-only glass (no blur) keeps frames cheap
        // under TCG; the drop composes once more with full frost.
        let do_blur = !dragging;
        let taskbar_dirty = window::wm_take_taskbar_dirty();
        let damage = window::wm_take_damage();

        match damage {
            Some((dx0, dy0, dw0, dh0)) => {
                // Union in the previous and current cursor rects: the old
                // cursor pixels are baked into the back buffer and must be
                // restored if they sit outside the window damage.
                let (mut dx, mut dy, mut dw, mut dh) = (dx0, dy0, dw0, dh0);
                for (cx, cy) in [(LAST_CURSOR_X, LAST_CURSOR_Y),
                                 (mouse::mouse_get_x(), mouse::mouse_get_y())] {
                    if cx >= 0 && cy >= 0 {
                        let x1 = (dx.saturating_add(dw as i32)).max(cx + 16);
                        let y1 = (dy.saturating_add(dh as i32)).max(cy + 24);
                        dx = dx.min(cx);
                        dy = dy.min(cy);
                        dw = (x1 - dx) as u32;
                        dh = (y1 - dy) as u32;
                    }
                }
                if window::wm_compose_partial(dx, dy, dw, dh, do_blur) {
                    LAST_COMPOSE_PATH = 1; // layout-partial
                    // Taskbar only if its buttons changed or damage reaches it
                    if taskbar_dirty
                        || window::wm_rect_hits_taskbar(dy, dh, desktop::taskbar_height())
                    {
                        desktop::desktop_draw_taskbar();
                    }
                } else {
                    LAST_COMPOSE_PATH = 2; // layout-full fallback
                    // Wallpaper cache not populated yet — full compose paints
                    // and captures it
                    desktop::desktop_draw_background();
                    window::wm_compose_all(do_blur);
                    desktop::desktop_draw_taskbar();
                }
            }
            None => {
                if taskbar_dirty {
                    desktop::desktop_draw_taskbar();
                }
                // No damage recorded (e.g. click on empty desktop): nothing
                // else to recompose — fall through to cursor + present
            }
        }
        window::wm_windows_rendered();
        LAST_FULL_COMPOSE_TICK = current_tick;
    } else if had_key || term_was_dirty {
        if partial_compose_single_window() {
            LAST_COMPOSE_PATH = 3; // typing partial
        } else {
            LAST_COMPOSE_PATH = 4; // typing full fallback
            desktop::desktop_draw_background();
            window::wm_compose_all(true);
            desktop::desktop_draw_taskbar();
            window::wm_windows_rendered();
            LAST_FULL_COMPOSE_TICK = current_tick;
        }
    } else {
        // No changes at all—this shouldn't happen (caught earlier), but safety check
        return;
    }

    // 5. Draw mouse cursor on top
    let mx = mouse::mouse_get_x();
    let my = mouse::mouse_get_y();
    cursor::cursor_draw_fresh(mx, my);
    LAST_CURSOR_X = mx;
    LAST_CURSOR_Y = my;

    // 6. Present to framebuffer (only dirty region via dirty rect system)
    compositor::compositor_present();

    // Perf telemetry: warn when one compose+present exceeds 8 ticks (~33ms)
    let spent = kernel_get_ticks().wrapping_sub(current_tick);
    if spent > 8 {
        serial_print(b"[gui] slow compose: ticks=\0".as_ptr());
        serial_print_dec(spent);
        serial_print(b" path=\0".as_ptr());
        serial_print_dec(LAST_COMPOSE_PATH as u32);
        serial_print(b"\n\0".as_ptr());
    }
}

/// Partial-compose fast path for terminal updates: valid only with a single
/// visible window that doesn't overlap the taskbar.
/// Preferred: restore the window's cached glass base (memcpy — no blur at
/// all) and re-blit the content. Fallback within the fast path: restore the
/// wallpaper rect and re-frost this one window.
unsafe fn partial_compose_single_window() -> bool {
    let wm = window::wm_get();
    if wm.z_order.len() != 1 {
        return false;
    }
    let Some(w) = wm.windows.iter().find(|w| w.visible) else {
        return false;
    };
    let (wx, wy, ww, wh, wid) = (w.x, w.y, w.width, w.height, w.id);
    let comp = compositor::compositor_get();
    let bar_y = comp.height.saturating_sub(desktop::taskbar_height()) as i32;
    if wy + wh as i32 > bar_y {
        return false; // overlaps taskbar strip — needs a taskbar repaint too
    }

    // Typing hot path: cached frosted glass + content blit, zero blur.
    if compositor::compositor_glass_restore(wid, wx, wy, ww, wh) {
        return window::wm_blit_content_by_id(wid);
    }

    // Glass cache miss (e.g. first keystroke after boot): re-frost once —
    // this also repopulates the glass cache for subsequent keystrokes.
    if !compositor::compositor_wallpaper_restore_rect(wx, wy, ww, wh) {
        return false; // wallpaper cache not populated yet (first frame)
    }
    window::wm_compose_by_id(wid)
}

/// Handle keyboard input — called from C IRQ handler.
#[no_mangle]
pub unsafe extern "C" fn gui_handle_key(scancode: u8, _pressed: u8) {
    // Push key event to queue
    event::gui_push_key_event(scancode, _pressed);
}

/// Handle mouse input — called from C IRQ12 handler.
/// This processes the raw PS/2 packet, updates mouse position,
/// and generates high-level events.
#[no_mangle]
pub unsafe extern "C" fn gui_handle_mouse(flags: u8, dx: i32, dy: i32) {
    // Update mouse position via the mouse module
    mouse::mouse_handle_packet(flags, dx, dy);

    let mx = mouse::mouse_get_x();
    let my = mouse::mouse_get_y();
    let buttons = mouse::mouse_get_buttons();

    // Generate movement event
    event::gui_push_mouse_move(mx, my);

    // Generate button press/release events (edge detection)
    let prev = PREV_BUTTONS;
    for bit in 0..3u8 {
        let mask = 1u8 << bit;
        let was = prev & mask;
        let now = buttons & mask;
        if was == 0 && now != 0 {
            event::gui_push_mouse_button(mx, my, bit, 1);
        } else if was != 0 && now == 0 {
            event::gui_push_mouse_button(mx, my, bit, 0);
        }
    }
    PREV_BUTTONS = buttons;
}

// --- Internal event handlers ---

unsafe fn handle_mouse_down(mx: i32, my: i32, button: u8) {
    if button != 0 {
        return; // Only handle left button
    }

    // Check taskbar first
    if let Some(wid) = desktop::taskbar_hit_test(mx, my) {
        window::wm_focus_window(wid);
        return;
    }

    // Hit-test windows
    if let Some((wid, area)) = window::wm_hit_test(mx, my) {
        window::wm_focus_window(wid);

        match area {
            window::HitArea::CloseButton => {
                widgets::widget_remove_all(wid);
                window::wm_close_window(wid);
            }
            window::HitArea::TitleBar => {
                window::wm_start_drag(wid, mx, my);
            }
            window::HitArea::Content => {
                // Convert to window-local coordinates and handle widget click
                let wm = window::wm_get();
                if let Some(w) = wm.windows.iter().find(|w| w.id == wid) {
                    let local_x = (mx - w.content_x()) as u32;
                    let local_y = (my - w.content_y()) as u32;
                    widgets::widget_handle_click(wid, local_x, local_y);
                }
            }
        }
    }
}

unsafe fn handle_mouse_up(_mx: i32, _my: i32) {
    if window::wm_is_dragging() {
        window::wm_stop_drag();
    }
}

unsafe fn handle_mouse_move(mx: i32, my: i32) {
    if window::wm_is_dragging() {
        window::wm_drag_to(mx, my);
    }
}

unsafe fn handle_key_press(scancode: u8) {
    // Route to focused window's terminal
    let focused = window::wm_get_focused_id();
    if let Some(wid) = focused {
        let term = terminal::terminal_get();
        if term.initialized && term.window_id == wid {
            terminal::terminal_handle_key(scancode);
        }
    }
}
