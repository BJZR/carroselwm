#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#define MAX_WORKSPACES 100
#define MAX_WINDOWS_PER_WS 2
#define CONFIG_PATH "/.config/cwm/cwmrc"

typedef struct {
    xcb_window_t id;
    int x, y, w, h;
    int max_x, max_y, max_w, max_h;
    int maximized;
    int hidden;
} window_t;

typedef struct {
    window_t windows[MAX_WINDOWS_PER_WS];
    int count;
} workspace_t;

typedef struct {
    int mod_key;
    int key_left;
    int key_right;
    int key_close;
    int key_hide;
    int key_max;
    int key_menu;
    int key_terminal;
    int key_launcher;
    char terminal_cmd[256];
    char launcher_cmd[256];
} config_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_key_symbols_t *keysyms;
static xcb_ewmh_connection_t ewmh;
static xcb_atom_t wm_delete_window;
static xcb_atom_t wm_protocols;
static workspace_t workspaces[MAX_WORKSPACES];
static int current_ws = 0;
static int total_ws = 1;
static config_t config;
static int running = 1;

static void init_config(void) {
    config.mod_key = XCB_MOD_MASK_4;
    config.key_left = XK_Left;
    config.key_right = XK_Right;
    config.key_close = XK_q;
    config.key_hide = XK_h;
    config.key_max = XK_m;
    config.key_menu = XK_Tab;
    config.key_terminal = XK_Return;
    config.key_launcher = XK_d;
    strcpy(config.terminal_cmd, "alacritty");
    strcpy(config.launcher_cmd, "dmenu_run");
}

static void load_config(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", getenv("HOME"), CONFIG_PATH);
    
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "mod_key=", 8) == 0) {
            if (strstr(line, "Alt")) config.mod_key = XCB_MOD_MASK_1;
            else if (strstr(line, "Super")) config.mod_key = XCB_MOD_MASK_4;
        } else if (strncmp(line, "terminal=", 9) == 0) {
            sscanf(line, "terminal=%255s", config.terminal_cmd);
        } else if (strncmp(line, "launcher=", 9) == 0) {
            sscanf(line, "launcher=%255s", config.launcher_cmd);
        }
    }
    fclose(f);
}

static void create_config_dir(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/cwm", getenv("HOME"));
    mkdir(dir, 0755);
}

static xcb_keycode_t keysym_to_keycode(xcb_keysym_t keysym) {
    xcb_keycode_t *keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
    xcb_keycode_t result = keycode ? *keycode : 0;
    free(keycode);
    return result;
}

static void grab_keys(void) {
    xcb_keycode_t codes[] = {
        keysym_to_keycode(config.key_left),
        keysym_to_keycode(config.key_right),
        keysym_to_keycode(config.key_close),
        keysym_to_keycode(config.key_hide),
        keysym_to_keycode(config.key_max),
        keysym_to_keycode(config.key_menu),
        keysym_to_keycode(config.key_terminal),
        keysym_to_keycode(config.key_launcher)
    };
    
    for (int i = 0; i < 8; i++) {
        if (codes[i]) {
            xcb_grab_key(conn, 1, screen->root, config.mod_key, codes[i],
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }
    }
}

static void ungrab_keys(void) {
    xcb_ungrab_key(conn, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
}

static window_t* find_window(xcb_window_t win) {
    for (int ws = 0; ws < total_ws; ws++) {
        for (int i = 0; i < workspaces[ws].count; i++) {
            if (workspaces[ws].windows[i].id == win) {
                return &workspaces[ws].windows[i];
            }
        }
    }
    return NULL;
}

static void arrange_windows(int ws) {
    if (ws >= total_ws || workspaces[ws].count == 0) return;
    
    int w = screen->width_in_pixels / workspaces[ws].count;
    int h = screen->height_in_pixels;
    
    for (int i = 0; i < workspaces[ws].count; i++) {
        window_t *win = &workspaces[ws].windows[i];
        if (!win->maximized && !win->hidden) {
            win->x = i * w;
            win->y = 0;
            win->w = w;
            win->h = h;
            
            uint32_t values[] = {win->x, win->y, win->w, win->h};
            xcb_configure_window(conn, win->id,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                values);
        }
    }
}

static void show_workspace(int ws) {
    for (int i = 0; i < MAX_WORKSPACES; i++) {
        for (int j = 0; j < workspaces[i].count; j++) {
            if (i == ws && !workspaces[i].windows[j].hidden) {
                xcb_map_window(conn, workspaces[i].windows[j].id);
            } else {
                xcb_unmap_window(conn, workspaces[i].windows[j].id);
            }
        }
    }
    arrange_windows(ws);
}

static void switch_workspace(int ws) {
    if (ws < 0 || ws >= total_ws) return;
    current_ws = ws;
    show_workspace(ws);
}

static void add_window(xcb_window_t win) {
    if (workspaces[current_ws].count >= MAX_WINDOWS_PER_WS) {
        if (current_ws + 1 >= total_ws) {
            total_ws++;
        }
        current_ws++;
    }
    
    window_t *w = &workspaces[current_ws].windows[workspaces[current_ws].count];
    w->id = win;
    w->maximized = 0;
    w->hidden = 0;
    workspaces[current_ws].count++;
    
    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn,
        xcb_get_geometry(conn, win), NULL);
    if (geom) {
        w->x = geom->x;
        w->y = geom->y;
        w->w = geom->width;
        w->h = geom->height;
        free(geom);
    }
    
    arrange_windows(current_ws);
}

static void remove_window(xcb_window_t win) {
    for (int ws = 0; ws < total_ws; ws++) {
        for (int i = 0; i < workspaces[ws].count; i++) {
            if (workspaces[ws].windows[i].id == win) {
                for (int j = i; j < workspaces[ws].count - 1; j++) {
                    workspaces[ws].windows[j] = workspaces[ws].windows[j + 1];
                }
                workspaces[ws].count--;
                arrange_windows(ws);
                return;
            }
        }
    }
}

static void toggle_maximize(xcb_window_t win) {
    window_t *w = find_window(win);
    if (!w) return;
    
    if (w->maximized) {
        uint32_t values[] = {w->x, w->y, w->w, w->h};
        xcb_configure_window(conn, win,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values);
        w->maximized = 0;
    } else {
        w->max_x = w->x;
        w->max_y = w->y;
        w->max_w = w->w;
        w->max_h = w->h;
        
        uint32_t values[] = {0, 0, screen->width_in_pixels, screen->height_in_pixels};
        xcb_configure_window(conn, win,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values);
        w->maximized = 1;
    }
}

static void toggle_hide(xcb_window_t win) {
    window_t *w = find_window(win);
    if (!w) return;
    
    if (w->hidden) {
        xcb_map_window(conn, win);
        w->hidden = 0;
    } else {
        xcb_unmap_window(conn, win);
        w->hidden = 1;
    }
}

static void close_window(xcb_window_t win) {
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = win;
    ev.type = wm_protocols;
    ev.format = 32;
    ev.data.data32[0] = wm_delete_window;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    
    xcb_send_event(conn, 0, win, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
}

static void spawn_program(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        exit(1);
    }
}

static void show_menu(void) {
    printf("CWM Workspaces:\n");
    for (int i = 0; i < total_ws; i++) {
        printf("%s[%d] Windows: %d\n",
               i == current_ws ? "*" : " ", i, workspaces[i].count);
    }
}

static void handle_key_press(xcb_key_press_event_t *e) {
    xcb_keysym_t keysym = xcb_key_symbols_get_keysym(keysyms, e->detail, 0);
    
    if (!(e->state & config.mod_key)) return;
    
    xcb_window_t focused;
    xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(conn,
        xcb_get_input_focus(conn), NULL);
    focused = focus ? focus->focus : XCB_WINDOW_NONE;
    if (focus) free(focus);
    
    switch (keysym) {
        case XK_Left:
            if (current_ws > 0) switch_workspace(current_ws - 1);
            break;
        case XK_Right:
            if (current_ws < total_ws - 1) switch_workspace(current_ws + 1);
            break;
        case XK_q:
            if (focused != XCB_WINDOW_NONE && focused != screen->root)
                close_window(focused);
            break;
        case XK_h:
            if (focused != XCB_WINDOW_NONE && focused != screen->root)
                toggle_hide(focused);
            break;
        case XK_m:
            if (focused != XCB_WINDOW_NONE && focused != screen->root)
                toggle_maximize(focused);
            break;
        case XK_Tab:
            show_menu();
            break;
        case XK_Return:
            spawn_program(config.terminal_cmd);
            break;
        case XK_d:
            spawn_program(config.launcher_cmd);
            break;
    }
}

static void handle_map_request(xcb_map_request_event_t *e) {
    add_window(e->window);
    xcb_map_window(conn, e->window);
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, e->window,
                        XCB_CURRENT_TIME);
}

static void handle_destroy_notify(xcb_destroy_notify_event_t *e) {
    remove_window(e->window);
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *e) {
    if (e->event != screen->root) {
        remove_window(e->window);
    }
}

static void signal_handler(int sig __attribute__((unused))) {
    running = 0;
}

static int init_wm(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) return 0;
    
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    keysyms = xcb_key_symbols_alloc(conn);
    
    xcb_intern_atom_cookie_t *ewmh_cookies = xcb_ewmh_init_atoms(conn, &ewmh);
    xcb_ewmh_init_atoms_replies(&ewmh, ewmh_cookies, NULL);
    
    xcb_intern_atom_cookie_t cookie1 = xcb_intern_atom(conn, 0, 12, "WM_PROTOCOLS");
    xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(conn, 0, 16, "WM_DELETE_WINDOW");
    xcb_intern_atom_reply_t *reply1 = xcb_intern_atom_reply(conn, cookie1, NULL);
    xcb_intern_atom_reply_t *reply2 = xcb_intern_atom_reply(conn, cookie2, NULL);
    
    if (reply1) {wm_protocols = reply1->atom; free(reply1);}
    if (reply2) {wm_delete_window = reply2->atom; free(reply2);}
    
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS
    };
    
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(conn,
        screen->root, mask, values);
    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error) {
        free(error);
        return 0;
    }
    
    return 1;
}

static void cleanup(void) {
    ungrab_keys();
    xcb_ewmh_connection_wipe(&ewmh);
    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
}

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    init_config();
    create_config_dir();
    load_config();
    
    if (!init_wm()) {
        fprintf(stderr, "Failed to initialize window manager\n");
        return 1;
    }
    
    grab_keys();
    xcb_flush(conn);
    
    while (running) {
        xcb_generic_event_t *event = xcb_wait_for_event(conn);
        if (!event) break;
        
        switch (event->response_type & ~0x80) {
            case XCB_KEY_PRESS:
                handle_key_press((xcb_key_press_event_t*)event);
                break;
            case XCB_MAP_REQUEST:
                handle_map_request((xcb_map_request_event_t*)event);
                break;
            case XCB_DESTROY_NOTIFY:
                handle_destroy_notify((xcb_destroy_notify_event_t*)event);
                break;
            case XCB_UNMAP_NOTIFY:
                handle_unmap_notify((xcb_unmap_notify_event_t*)event);
                break;
        }
        
        free(event);
        xcb_flush(conn);
    }
    
    cleanup();
    return 0;
}