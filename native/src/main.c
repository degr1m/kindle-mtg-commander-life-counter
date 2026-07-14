#include "device.h"
#include "game.h"
#include "layout.h"

#include <errno.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WINDOW_TITLE "L:A_N:application_ID:com.mtgcommander.lifecounter_PC:N_O:U"
#define LONG_PRESS_DELAY_MS 650
#define POWER_EVENT_LINE_MAX 128
#define POWER_EVENT_READ_BUDGET 512
#define POWER_RECONCILE_RETRY_MS 1000
#define POWER_RECONCILE_WATCHDOG_MS 5000
#define POWER_RESTART_MIN_MS 250
#define POWER_RESTART_MAX_MS 8000
#define POWER_RESTART_STABLE_MS 30000

typedef enum {
    OVERLAY_NONE,
    OVERLAY_NEW_GAME,
    OVERLAY_MENU,
} OverlayMode;

typedef struct {
    GtkWidget *window;
    GtkWidget *canvas;
    GameState game;
    GameState hold_before;
    GameLayout layout;
    GameHoldState hold;
    DeviceBackend device_backend;
    DeviceStatus device_status;
    DevicePowerState power;
    int remembered_brightness;
    char state_path[1024];
    OverlayMode overlay;
    guint long_press_timer;
    GPid power_pid;
    GIOChannel *power_channel;
    guint power_io_watch;
    guint power_child_watch;
    guint power_restart_timer;
    guint power_stability_timer;
    guint power_reconcile_timer;
    gint64 power_reconcile_due_us;
    guint power_restart_delay_ms;
    char power_event_line[POWER_EVENT_LINE_MAX];
    gsize power_event_line_length;
    gboolean power_event_line_overflow;
    gboolean shutting_down;
    gboolean exit_failed;
} App;

static volatile sig_atomic_t termination_signal;

static void request_termination(int signal_number) {
    termination_signal = signal_number;
}

static gboolean install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_termination;
    if (sigemptyset(&action.sa_mask) != 0) return FALSE;
    return sigaction(SIGHUP, &action, NULL) == 0 &&
           sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

static double minimum(double a, double b) { return a < b ? a : b; }

static void set_gray(cairo_t *cr, double gray) {
    cairo_set_source_rgb(cr, gray, gray, gray);
}

static void draw_text(cairo_t *cr, const char *text, double x, double y,
                      double width, double height, double size,
                      gboolean centered) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Sans");
    pango_font_description_set_weight(font, PANGO_WEIGHT_HEAVY);
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);

    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    double draw_x = centered ? x + (width - text_width) / 2.0 : x;
    double draw_y = y + (height - text_height) / 2.0;
    cairo_move_to(cr, draw_x, draw_y);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font);
    g_object_unref(layout);
}

static double player_angle(int player) {
    return player == 0 || player == 3 ? M_PI / 2.0 : -M_PI / 2.0;
}

static void draw_rotated_text(cairo_t *cr, const char *text, GameRect area,
                              double size, double angle) {
    cairo_save(cr);
    cairo_translate(cr, area.x + area.width / 2.0,
                    area.y + area.height / 2.0);
    cairo_rotate(cr, angle);
    draw_text(cr, text, -area.height / 2.0, -area.width / 2.0,
              area.height, area.width, size, TRUE);
    cairo_restore(cr);
}

static void draw_life_text(cairo_t *cr, const char *text, GameRect area,
                           double current_size, double size, double angle) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Sans");
    pango_font_description_set_weight(font, PANGO_WEIGHT_HEAVY);
    pango_layout_set_text(layout, text, -1);

    pango_font_description_set_absolute_size(font,
                                             current_size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    int current_width = 0;
    int current_height = 0;
    pango_layout_get_pixel_size(layout, &current_width, &current_height);

    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);

    cairo_save(cr);
    cairo_translate(cr, area.x + area.width / 2.0,
                    area.y + area.height / 2.0);
    cairo_rotate(cr, angle);
    cairo_move_to(cr, -text_width / 2.0, -current_height / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    (void)current_width;
    (void)text_height;
    pango_font_description_free(font);
    g_object_unref(layout);
}

static void draw_vertical_triangle(cairo_t *cr, GameRect area,
                                   gboolean points_down) {
    const double center_x = area.x + area.width / 2.0;
    const double center_y = area.y + area.height / 2.0;
    const double size = minimum(area.width, area.height);
    cairo_new_path(cr);
    cairo_move_to(cr, center_x,
                  center_y + (points_down ? size * 0.55 : -size * 0.55));
    cairo_line_to(cr, center_x - size * 0.5,
                  center_y + (points_down ? -size * 0.45 : size * 0.45));
    cairo_line_to(cr, center_x + size * 0.5,
                  center_y + (points_down ? -size * 0.45 : size * 0.45));
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_skull(cairo_t *cr, double center_x, double center_y,
                       double size, gboolean filled) {
    cairo_save(cr);
    cairo_translate(cr, center_x, center_y);
    cairo_new_path(cr);
    cairo_arc(cr, 0.0, -size * 0.10, size * 0.32, M_PI, 2.0 * M_PI);
    cairo_line_to(cr, size * 0.32, size * 0.12);
    cairo_line_to(cr, size * 0.18, size * 0.30);
    cairo_line_to(cr, -size * 0.18, size * 0.30);
    cairo_line_to(cr, -size * 0.32, size * 0.12);
    cairo_close_path(cr);
    set_gray(cr, filled ? 0.04 : 0.99);
    cairo_fill_preserve(cr);
    set_gray(cr, 0.04);
    cairo_set_line_width(cr, size * 0.055);
    cairo_stroke(cr);

    set_gray(cr, filled ? 0.99 : 0.04);
    cairo_arc(cr, -size * 0.12, -size * 0.03, size * 0.065, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_arc(cr, size * 0.12, -size * 0.03, size * 0.065, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_rectangle(cr, -size * 0.035, size * 0.10,
                    size * 0.07, size * 0.10);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_crown(cairo_t *cr, double center_x, double center_y,
                       double size, gboolean filled, double angle) {
    cairo_save(cr);
    cairo_translate(cr, center_x, center_y);
    cairo_rotate(cr, angle);
    cairo_move_to(cr, -size * 0.5, size * 0.35);
    cairo_line_to(cr, -size * 0.5, -size * 0.3);
    cairo_line_to(cr, -size * 0.16, 0.0);
    cairo_line_to(cr, 0.0, -size * 0.5);
    cairo_line_to(cr, size * 0.16, 0.0);
    cairo_line_to(cr, size * 0.5, -size * 0.3);
    cairo_line_to(cr, size * 0.5, size * 0.35);
    cairo_close_path(cr);
    set_gray(cr, filled ? 0.05 : 0.965);
    cairo_fill_preserve(cr);
    set_gray(cr, 0.05);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_commander_cell(cairo_t *cr, const App *app,
                                int player, int source) {
    GameRect cell = app->layout.commander[player][source];
    GameRect label_area = app->layout.commander_label[player][source];
    GameRect value_area = app->layout.commander_value[player][source];
    const double angle = player_angle(player);
    char label[16];
    char value[24];
    snprintf(label, sizeof(label), "P%d", source + 1);
    snprintf(value, sizeof(value), "%02d",
             app->game.commander_damage[player][source]);

    set_gray(cr, 0.83);
    cairo_rectangle(cr, cell.x, cell.y, cell.width, cell.height);
    cairo_fill(cr);
    set_gray(cr, 0.04);
    cairo_set_line_width(cr, 3.0);
    cairo_rectangle(cr, cell.x, cell.y, cell.width, cell.height);
    cairo_stroke(cr);

    draw_rotated_text(cr, label, label_area, cell.width * 0.20, angle);
    double value_size = cell.width * 0.48;
    size_t value_length = strlen(value);
    if (value_length > 3) value_size *= 3.0 / value_length;
    draw_rotated_text(cr, value, value_area, value_size, angle);
}

static void draw_poison_marker(cairo_t *cr, const App *app, int player) {
    GameRect skull = app->layout.poison_skull[player];
    GameRect value_area = app->layout.poison_value[player];
    const gboolean active = app->game.poison[player] > 0;
    const gboolean right = player == 1 || player == 2;
    const double angle = player_angle(player);
    const double skull_size =
        minimum(skull.width, skull.height) * 1.176;
    const double skull_center_x = app->layout.marker_margin_x[player] +
        (right ? skull_size / 2.0 : -skull_size / 2.0);

    cairo_save(cr);
    cairo_translate(cr, skull_center_x, skull.y + skull.height / 2.0);
    cairo_rotate(cr, angle);
    draw_skull(cr, 0.0, 0.0, skull_size, active);
    cairo_restore(cr);
    if (active) {
        char value[24];
        snprintf(value, sizeof(value), "%02d", app->game.poison[player]);
        double value_size = value_area.width * 0.58;
        size_t value_length = strlen(value);
        if (value_length > 3) value_size *= 3.0 / value_length;
        set_gray(cr, 0.04);
        draw_rotated_text(cr, value, value_area, value_size, angle);
    }
}

static void draw_first_triangles(cairo_t *cr, const App *app, int player) {
    if (app->game.first_player != player) return;
    set_gray(cr, 0.03);
    draw_vertical_triangle(cr, app->layout.first_triangle[player][0], TRUE);
    draw_vertical_triangle(cr, app->layout.first_triangle[player][1], FALSE);
}

static void draw_player_name(cairo_t *cr, const App *app, int player) {
    GameRect panel = app->layout.panel[player];
    const gboolean right = player == 1 || player == 2;
    char label[24];
    snprintf(label, sizeof(label), "PLAYER %d", player + 1);
    cairo_save(cr);
    cairo_translate(cr, panel.x + panel.width * (right ? 0.08 : 0.92),
                    panel.y + panel.height * 0.5);
    cairo_rotate(cr, player_angle(player));
    set_gray(cr, 0.04);
    draw_text(cr, label, -panel.height * 0.31, -panel.width * 0.055,
              panel.height * 0.62, panel.width * 0.11,
              panel.width * 0.058, TRUE);
    cairo_restore(cr);
}

static void draw_player(cairo_t *cr, const App *app, int player) {
    GameRect panel = app->layout.panel[player];
    GameRect content = app->layout.content[player];
    const double angle = player_angle(player);
    char life[24];
    snprintf(life, sizeof(life), "%d", app->game.life[player]);

    set_gray(cr, 0.99);
    cairo_rectangle(cr, panel.x, panel.y, panel.width, panel.height);
    cairo_fill(cr);

    set_gray(cr, 0.04);
    double current_size = minimum(content.width * 0.55, content.height * 0.38);
    size_t life_length = strlen(life);
    if (life_length > 3) current_size *= 3.0 / life_length;
    const double life_size = current_size * 1.25;
    draw_life_text(cr, life, content, current_size, life_size, angle);
    draw_rotated_text(cr, "−", app->layout.life_minus_visual[player],
                      panel.width * 0.085, angle);
    draw_rotated_text(cr, "+", app->layout.life_plus_visual[player],
                      panel.width * 0.085, angle);

    for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
        draw_commander_cell(cr, app, player, source);
    }
    draw_player_name(cr, app, player);
    draw_first_triangles(cr, app, player);
    draw_poison_marker(cr, app, player);

    GameRect crown = app->layout.monarch[player];
    const gboolean right = player == 1 || player == 2;
    const double crown_size = minimum(crown.width, crown.height) * 0.56;
    const double crown_center_x = app->layout.marker_margin_x[player] +
        (right ? crown_size / 2.0 : -crown_size / 2.0);
    draw_crown(cr, crown_center_x,
               crown.y + crown.height / 2.0,
               crown_size,
               app->game.monarch_player == player, angle);

    set_gray(cr, 0.04);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, panel.x, panel.y, panel.width, panel.height);
    cairo_stroke(cr);
}

static void draw_toolbar(cairo_t *cr, double y, double width, double height) {
    const double cuts[] = {0.0, 0.274, 0.726, 1.0};
    const char *labels[] = {"PICK FIRST", "NEW GAME", "MENU"};

    for (int i = 0; i < 3; ++i) {
        double x = width * cuts[i];
        double item_width = width * (cuts[i + 1] - cuts[i]);
        set_gray(cr, 0.06);
        cairo_rectangle(cr, x, y, item_width, height);
        cairo_fill(cr);
        set_gray(cr, 1.0);
        draw_text(cr, labels[i], x, y, item_width, height,
                  minimum(height * 0.28, item_width * 0.105), TRUE);
        set_gray(cr, 1.0);
        cairo_set_line_width(cr, 3.0);
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x, y + height);
        cairo_stroke(cr);
    }

    set_gray(cr, 0.03);
    cairo_set_line_width(cr, 5.0);
    cairo_rectangle(cr, 0, y, width, height);
    cairo_stroke(cr);
}

static GameRect centered_dialog(double width, double height,
                                double width_ratio, double height_ratio) {
    GameRect dialog = {
        (width - width * width_ratio) / 2.0,
        (height - height * height_ratio) / 2.0,
        width * width_ratio,
        height * height_ratio,
    };
    return dialog;
}

static void dialog_buttons(GameRect dialog, GameRect *left, GameRect *right) {
    const double button_y = dialog.y + dialog.height * 0.72;
    const double button_height = dialog.height * 0.19;
    const double button_width = dialog.width * 0.39;
    *left = (GameRect){dialog.x + dialog.width * 0.07, button_y,
                       button_width, button_height};
    *right = (GameRect){dialog.x + dialog.width * 0.54, button_y,
                        button_width, button_height};
}

static GameRect menu_light_button(GameRect dialog) {
    GameRect button = {
        dialog.x + dialog.width * 0.12,
        dialog.y + dialog.height * 0.43,
        dialog.width * 0.76,
        dialog.height * 0.13,
    };
    return button;
}

static void draw_dialog_button(cairo_t *cr, GameRect button,
                               const char *label, gboolean filled) {
    set_gray(cr, filled ? 0.03 : 0.965);
    cairo_rectangle(cr, button.x, button.y, button.width, button.height);
    cairo_fill(cr);
    set_gray(cr, filled ? 1.0 : 0.03);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, button.x, button.y, button.width, button.height);
    cairo_stroke(cr);
    draw_text(cr, label, button.x, button.y, button.width, button.height,
              button.height * 0.24, TRUE);
}

static void draw_new_game_confirmation(cairo_t *cr, double width,
                                       double height) {
    set_gray(cr, 0.03);
    cairo_paint(cr);

    GameRect dialog = centered_dialog(width, height, 0.78, 0.36);
    set_gray(cr, 0.965);
    cairo_rectangle(cr, dialog.x, dialog.y, dialog.width, dialog.height);
    cairo_fill(cr);
    set_gray(cr, 1.0);
    cairo_set_line_width(cr, 8.0);
    cairo_rectangle(cr, dialog.x, dialog.y, dialog.width, dialog.height);
    cairo_stroke(cr);

    set_gray(cr, 0.03);
    draw_text(cr, "START NEW GAME?", dialog.x,
              dialog.y + dialog.height * 0.08,
              dialog.width, dialog.height * 0.23,
              dialog.width * 0.07, TRUE);
    draw_text(cr, "Reset life, damage, poison, Monarch, and First player.",
              dialog.x + dialog.width * 0.05,
              dialog.y + dialog.height * 0.31,
              dialog.width * 0.90, dialog.height * 0.20,
              dialog.width * 0.031, TRUE);

    GameRect cancel;
    GameRect reset;
    dialog_buttons(dialog, &cancel, &reset);
    draw_dialog_button(cr, cancel, "CANCEL", FALSE);
    draw_dialog_button(cr, reset, "RESET", TRUE);
}

static void draw_menu(cairo_t *cr, const App *app,
                      double width, double height) {
    set_gray(cr, 0.03);
    cairo_paint(cr);

    GameRect dialog = centered_dialog(width, height, 0.78, 0.58);
    set_gray(cr, 0.965);
    cairo_rectangle(cr, dialog.x, dialog.y, dialog.width, dialog.height);
    cairo_fill(cr);
    set_gray(cr, 1.0);
    cairo_set_line_width(cr, 8.0);
    cairo_rectangle(cr, dialog.x, dialog.y, dialog.width, dialog.height);
    cairo_stroke(cr);

    set_gray(cr, 0.03);
    draw_text(cr, "MENU", dialog.x, dialog.y + dialog.height * 0.04,
              dialog.width, dialog.height * 0.15,
              dialog.width * 0.08, TRUE);

    char battery_label[32];
    if (app->device_status.battery_available) {
        snprintf(battery_label, sizeof(battery_label), "BATTERY: %d%%",
                 app->device_status.battery_percent);
    } else {
        snprintf(battery_label, sizeof(battery_label), "BATTERY: UNAVAILABLE");
    }
    draw_text(cr, battery_label,
              dialog.x + dialog.width * 0.08,
              dialog.y + dialog.height * 0.25,
              dialog.width * 0.84, dialog.height * 0.12,
              dialog.width * 0.035, TRUE);

    GameRect light_button = menu_light_button(dialog);
    const gboolean light_on = app->device_status.light_available &&
                              app->device_status.light_level > 0;
    set_gray(cr, app->device_status.light_available
                 ? (light_on ? 0.03 : 0.965) : 0.70);
    cairo_rectangle(cr, light_button.x, light_button.y,
                    light_button.width, light_button.height);
    cairo_fill(cr);
    set_gray(cr, light_on ? 1.0 : 0.15);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, light_button.x, light_button.y,
                    light_button.width, light_button.height);
    cairo_stroke(cr);

    char light_label[48];
    if (!app->device_status.light_available) {
        snprintf(light_label, sizeof(light_label), "FRONT LIGHT: UNAVAILABLE");
    } else if (light_on) {
        snprintf(light_label, sizeof(light_label), "FRONT LIGHT: ON (%d/%d)",
                 app->device_status.light_level, app->device_status.light_max);
    } else {
        snprintf(light_label, sizeof(light_label), "FRONT LIGHT: OFF");
    }
    draw_text(cr, light_label, light_button.x, light_button.y,
              light_button.width, light_button.height,
              dialog.width * 0.032, TRUE);

    if (app->exit_failed) {
        draw_text(cr, "EXIT FAILED: FRONT LIGHT STILL ON",
                  dialog.x + dialog.width * 0.08,
                  dialog.y + dialog.height * 0.59,
                  dialog.width * 0.84, dialog.height * 0.08,
                  dialog.width * 0.028, TRUE);
    }

    GameRect back;
    GameRect exit_button;
    dialog_buttons(dialog, &back, &exit_button);
    draw_dialog_button(cr, back, "BACK TO GAME", FALSE);
    draw_dialog_button(cr, exit_button, "EXIT", TRUE);
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data) {
    App *app = data;
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window) return FALSE;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    cairo_t *cr = gdk_cairo_create(window);
    cairo_rectangle(cr, event->area.x, event->area.y,
                    event->area.width, event->area.height);
    cairo_clip(cr);
    double width = allocation.width;
    double height = allocation.height;
    game_layout_init(&app->layout, width, height);

    set_gray(cr, 0.99);
    cairo_paint(cr);
    draw_player(cr, app, 0);
    draw_player(cr, app, 1);
    draw_toolbar(cr, app->layout.toolbar.y,
                 app->layout.toolbar.width, app->layout.toolbar.height);
    draw_player(cr, app, 3);
    draw_player(cr, app, 2);
    if (app->overlay == OVERLAY_NEW_GAME) {
        draw_new_game_confirmation(cr, width, height);
    } else if (app->overlay == OVERLAY_MENU) {
        draw_menu(cr, app, width, height);
    }

    cairo_destroy(cr);
    return TRUE;
}

static void persist(const App *app) {
    if (!game_save(&app->game, app->state_path)) {
        fprintf(stderr, "Could not save game state to %s\n", app->state_path);
    }
}

static void queue_player(App *app, int player) {
    if (player < 0 || player >= GAME_PLAYER_COUNT) return;
    GtkAllocation allocation;
    gtk_widget_get_allocation(app->canvas, &allocation);
    game_layout_init(&app->layout, allocation.width, allocation.height);
    GameRect panel = app->layout.panel[player];
    gtk_widget_queue_draw_area(app->canvas,
                               (int)floor(panel.x), (int)floor(panel.y),
                               (int)ceil(panel.width), (int)ceil(panel.height));
}

static void cancel_hold_timer(App *app) {
    if (app->long_press_timer) g_source_remove(app->long_press_timer);
    app->long_press_timer = 0;
}

static void cancel_hold(App *app) {
    cancel_hold_timer(app);
    game_hold_reset(&app->hold);
}

static gboolean start_power_watch(App *app);
static void schedule_power_restart(App *app);
static void schedule_power_reconcile(App *app);
static void schedule_power_reconcile_now(App *app);
static void schedule_power_watchdog(App *app);

static void close_power_channel(App *app) {
    if (app->power_io_watch) {
        g_source_remove(app->power_io_watch);
        app->power_io_watch = 0;
    }
    if (app->power_channel) {
        (void)g_io_channel_shutdown(app->power_channel, FALSE, NULL);
        g_io_channel_unref(app->power_channel);
        app->power_channel = NULL;
    }
    app->power_event_line_length = 0;
    app->power_event_line_overflow = FALSE;
}

static void terminate_power_child(App *app) {
    const gboolean has_child_watch = app->power_child_watch != 0;
    app->power_child_watch = 0;
    if (app->power_pid > 0) {
        const GPid pid = app->power_pid;
        app->power_pid = 0;
        const int killed = kill((pid_t)pid, SIGKILL);
        if (killed < 0 && errno != ESRCH) {
            fprintf(stderr, "Could not stop power event listener: %s\n",
                    strerror(errno));
        }
        if (!has_child_watch) {
            if (killed == 0 || errno == ESRCH) {
                int status = 0;
                while (waitpid((pid_t)pid, &status, 0) < 0) {
                    if (errno != EINTR) break;
                }
            }
            g_spawn_close_pid(pid);
        }
    }
}

static void cancel_power_stability_timer(App *app) {
    if (!app->power_stability_timer) return;
    g_source_remove(app->power_stability_timer);
    app->power_stability_timer = 0;
}

static void clear_power_watch(App *app) {
    cancel_power_stability_timer(app);
    close_power_channel(app);
    terminate_power_child(app);
}

static void stop_power_watch(App *app) {
    app->shutting_down = TRUE;
    if (app->power_restart_timer) {
        g_source_remove(app->power_restart_timer);
        app->power_restart_timer = 0;
    }
    if (app->power_reconcile_timer) {
        g_source_remove(app->power_reconcile_timer);
        app->power_reconcile_timer = 0;
    }
    app->power_reconcile_due_us = 0;
    clear_power_watch(app);
}

static void handle_power_event(App *app, DevicePowerEvent event) {
    if (event == DEVICE_POWER_NONE) return;
    cancel_hold(app);
    gtk_widget_set_sensitive(app->canvas, FALSE);
    schedule_power_reconcile_now(app);
}

static void reconcile_power_state(App *app) {
    DevicePowerObservedState observed = device_power_reconcile(
        &app->device_backend, &app->device_status, &app->power);
    gboolean settled = FALSE;
    if (observed == DEVICE_POWER_OBSERVED_SLEEPING) {
        cancel_hold(app);
        gtk_widget_set_sensitive(app->canvas, FALSE);
        if (app->power.light_before_sleep > 0) {
            app->remembered_brightness = app->power.light_before_sleep;
        }
        settled = app->power.sleeping && app->power.light_off;
    } else if (observed == DEVICE_POWER_OBSERVED_AWAKE) {
        settled = !app->power.sleeping && !app->power.restore_light;
        gtk_widget_set_sensitive(app->canvas, settled);
        if (settled) {
            gtk_widget_queue_draw(app->canvas);
        } else {
            cancel_hold(app);
        }
    } else if (observed == DEVICE_POWER_OBSERVED_UNKNOWN) {
        cancel_hold(app);
        gtk_widget_set_sensitive(app->canvas, FALSE);
    }
    if (settled) {
        schedule_power_watchdog(app);
    } else {
        schedule_power_reconcile(app);
    }
}

static void reject_power_event_record(App *app) {
    if (app->power_event_line_overflow) return;
    app->power_event_line_overflow = TRUE;
    cancel_hold(app);
    gtk_widget_set_sensitive(app->canvas, FALSE);
    schedule_power_reconcile_now(app);
}

static void finish_power_event_line(App *app) {
    DevicePowerEvent event = DEVICE_POWER_NONE;
    if (!app->power_event_line_overflow && app->power_event_line_length) {
        app->power_event_line[app->power_event_line_length] = '\0';
        event = device_power_event_parse(app->power_event_line);
    }
    app->power_event_line_length = 0;
    app->power_event_line_overflow = FALSE;
    if (event == DEVICE_POWER_NONE) {
        cancel_hold(app);
        gtk_widget_set_sensitive(app->canvas, FALSE);
        schedule_power_reconcile_now(app);
    } else {
        handle_power_event(app, event);
    }
}

static gboolean on_power_event_output(GIOChannel *source,
                                      GIOCondition condition,
                                      gpointer data) {
    App *app = data;
    gboolean keep = TRUE;
    guint bytes_read = 0;
    while (bytes_read < POWER_EVENT_READ_BUDGET) {
        gchar byte = '\0';
        gsize count = 0;
        GError *error = NULL;
        GIOStatus status = g_io_channel_read_chars(source, &byte, 1, &count,
                                                   &error);
        if (status == G_IO_STATUS_NORMAL && count == 1) {
            ++bytes_read;
            if (byte == '\n') {
                finish_power_event_line(app);
            } else if (byte == '\0') {
                reject_power_event_record(app);
            } else if (!app->power_event_line_overflow) {
                if (app->power_event_line_length + 1 < POWER_EVENT_LINE_MAX) {
                    app->power_event_line[app->power_event_line_length++] = byte;
                } else {
                    reject_power_event_record(app);
                }
            }
            if (error) g_error_free(error);
            continue;
        }
        if (error) {
            fprintf(stderr, "Power event read failed: %s\n", error->message);
            g_error_free(error);
        }
        if (status != G_IO_STATUS_AGAIN &&
            !(status == G_IO_STATUS_NORMAL && count == 0)) keep = FALSE;
        break;
    }
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) keep = FALSE;
    if (!keep) {
        app->power_io_watch = 0;
        cancel_hold(app);
        gtk_widget_set_sensitive(app->canvas, FALSE);
        schedule_power_reconcile_now(app);
        if (app->power_pid > 0) {
            (void)kill((pid_t)app->power_pid, SIGKILL);
        }
    }
    return keep;
}

static void on_power_watch_exit(GPid pid, gint status, gpointer data) {
    App *app = data;
    (void)status;
    if (app->power_pid != pid) {
        g_spawn_close_pid(pid);
        return;
    }
    app->power_pid = 0;
    app->power_child_watch = 0;
    cancel_power_stability_timer(app);
    close_power_channel(app);
    g_spawn_close_pid(pid);
    cancel_hold(app);
    gtk_widget_set_sensitive(app->canvas, FALSE);
    schedule_power_reconcile_now(app);
    schedule_power_restart(app);
}

static gboolean mark_power_watch_stable(gpointer data) {
    App *app = data;
    app->power_stability_timer = 0;
    if (!app->shutting_down && app->power_pid > 0) {
        app->power_restart_delay_ms = POWER_RESTART_MIN_MS;
    }
    return FALSE;
}

static gboolean start_power_watch(App *app) {
    gchar *arguments[] = {
        "lipc-wait-event",
        "-m",
        "-s",
        "0",
        "com.lab126.powerd",
        "goingToScreenSaver,readyToSuspend,outOfScreenSaver,wakeupFromSuspend",
        NULL,
    };
    gint output_fd = -1;
    GError *error = NULL;
    if (!g_spawn_async_with_pipes(
            NULL, arguments, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
            NULL, NULL, &app->power_pid, NULL, &output_fd, NULL, &error)) {
        fprintf(stderr, "Could not start power event listener: %s\n",
                error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return FALSE;
    }

    app->power_child_watch = g_child_watch_add(app->power_pid,
                                               on_power_watch_exit, app);
    if (!app->power_child_watch) {
        close(output_fd);
        clear_power_watch(app);
        return FALSE;
    }

    app->power_channel = g_io_channel_unix_new(output_fd);
    g_io_channel_set_close_on_unref(app->power_channel, TRUE);
    if (g_io_channel_set_encoding(app->power_channel, NULL, &error) !=
        G_IO_STATUS_NORMAL) {
        fprintf(stderr, "Could not configure power event encoding: %s\n",
                error ? error->message : "unknown error");
        if (error) g_error_free(error);
        clear_power_watch(app);
        return FALSE;
    }
    GIOFlags flags = g_io_channel_get_flags(app->power_channel);
    if (g_io_channel_set_flags(app->power_channel,
                               flags | G_IO_FLAG_NONBLOCK, &error) !=
        G_IO_STATUS_NORMAL) {
        fprintf(stderr, "Could not configure power event listener: %s\n",
                error ? error->message : "unknown error");
        if (error) g_error_free(error);
        clear_power_watch(app);
        return FALSE;
    }

    app->power_io_watch = g_io_add_watch(
        app->power_channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        on_power_event_output, app);
    if (!app->power_io_watch) {
        clear_power_watch(app);
        return FALSE;
    }
    app->power_stability_timer = g_timeout_add(
        POWER_RESTART_STABLE_MS, mark_power_watch_stable, app);
    return TRUE;
}

static gboolean restart_power_watch(gpointer data) {
    App *app = data;
    app->power_restart_timer = 0;
    if (!app->shutting_down) {
        const gboolean started = start_power_watch(app);
        reconcile_power_state(app);
        if (!started) schedule_power_restart(app);
    }
    return FALSE;
}

static gboolean retry_power_reconcile(gpointer data) {
    App *app = data;
    app->power_reconcile_timer = 0;
    app->power_reconcile_due_us = 0;
    if (!app->shutting_down) reconcile_power_state(app);
    return FALSE;
}

static void schedule_power_reconcile_after(App *app, guint delay_ms) {
    if (app->shutting_down) return;
    const gint64 due_us = g_get_monotonic_time() +
                          (gint64)delay_ms * 1000;
    if (app->power_reconcile_timer) {
        if (app->power_reconcile_due_us > 0 &&
            app->power_reconcile_due_us <= due_us) {
            return;
        }
        g_source_remove(app->power_reconcile_timer);
    }
    app->power_reconcile_timer = g_timeout_add(delay_ms,
                                               retry_power_reconcile, app);
    app->power_reconcile_due_us = app->power_reconcile_timer ? due_us : 0;
}

static void schedule_power_reconcile(App *app) {
    schedule_power_reconcile_after(app, POWER_RECONCILE_RETRY_MS);
}

static void schedule_power_reconcile_now(App *app) {
    schedule_power_reconcile_after(app, 1);
}

static void schedule_power_watchdog(App *app) {
    schedule_power_reconcile_after(app, POWER_RECONCILE_WATCHDOG_MS);
}

static void schedule_power_restart(App *app) {
    if (app->shutting_down || app->power_restart_timer) return;
    const guint delay = app->power_restart_delay_ms
                            ? app->power_restart_delay_ms
                            : POWER_RESTART_MIN_MS;
    app->power_restart_timer = g_timeout_add(delay, restart_power_watch, app);
    app->power_restart_delay_ms = delay >= POWER_RESTART_MAX_MS / 2
                                      ? POWER_RESTART_MAX_MS
                                      : delay * 2;
}

static void open_menu(App *app) {
    (void)device_refresh(&app->device_backend, &app->device_status);
    if (app->device_status.light_available &&
        app->device_status.light_level > 0) {
        app->remembered_brightness = app->device_status.light_level;
    }
    app->overlay = OVERLAY_MENU;
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event,
                                 gpointer data) {
    (void)widget;
    (void)event;
    App *app = data;
    cancel_hold(app);
    persist(app);
    if (device_power_exit(&app->device_backend, &app->device_status,
                          &app->power)) return FALSE;
    app->exit_failed = TRUE;
    app->overlay = OVERLAY_MENU;
    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    App *app = data;
    cancel_hold(app);
    stop_power_watch(app);
    gtk_main_quit();
}

static gboolean poll_termination(gpointer data) {
    App *app = data;
    if (!termination_signal) return TRUE;
    cancel_hold(app);
    gtk_widget_set_sensitive(app->canvas, FALSE);
    persist(app);
    stop_power_watch(app);
    if (!device_power_exit(&app->device_backend, &app->device_status,
                           &app->power)) return TRUE;
    termination_signal = 0;
    gtk_widget_destroy(app->window);
    return FALSE;
}

static void apply_hold_promotion(App *app) {
    if (!app->hold.active) return;
    GameControl control = app->hold.control;
    if (game_apply_hold_from_snapshot(&app->game, &app->hold_before, control)) {
        persist(app);
        if (control.type == GAME_CONTROL_MONARCH) {
            gtk_widget_queue_draw(app->canvas);
        } else {
            queue_player(app, control.player);
        }
    }
}

static gboolean on_long_press(gpointer data) {
    App *app = data;
    app->long_press_timer = 0;
    if (game_hold_timeout(&app->hold)) apply_hold_promotion(app);
    return FALSE;
}

static gboolean handle_overlay(App *app, double x, double y,
                               double width, double height) {
    cancel_hold(app);
    if (app->overlay == OVERLAY_NEW_GAME) {
        GameRect dialog = centered_dialog(width, height, 0.78, 0.36);
        GameRect cancel;
        GameRect reset;
        dialog_buttons(dialog, &cancel, &reset);
        if (game_rect_contains(cancel, x, y)) {
            app->overlay = OVERLAY_NONE;
        } else if (game_rect_contains(reset, x, y)) {
            game_reset(&app->game);
            app->overlay = OVERLAY_NONE;
            persist(app);
        }
        gtk_widget_queue_draw(app->canvas);
        return TRUE;
    }
    if (app->overlay == OVERLAY_MENU) {
        GameRect dialog = centered_dialog(width, height, 0.78, 0.58);
        GameRect light_button = menu_light_button(dialog);
        GameRect back;
        GameRect exit_button;
        dialog_buttons(dialog, &back, &exit_button);
        if (game_rect_contains(light_button, x, y) &&
            app->device_status.light_available) {
            app->exit_failed = FALSE;
            (void)device_toggle_front_light(&app->device_backend,
                                            &app->device_status,
                                            &app->remembered_brightness);
            gtk_widget_queue_draw(app->canvas);
        } else if (game_rect_contains(back, x, y)) {
            app->exit_failed = FALSE;
            app->overlay = OVERLAY_NONE;
            gtk_widget_queue_draw(app->canvas);
        } else if (game_rect_contains(exit_button, x, y)) {
            persist(app);
            if (device_power_exit(&app->device_backend,
                                  &app->device_status, &app->power)) {
                gtk_widget_destroy(app->window);
            } else {
                app->exit_failed = TRUE;
                gtk_widget_queue_draw(app->canvas);
            }
        }
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                gpointer data) {
    App *app = data;
    if (app->power.sleeping) {
        cancel_hold(app);
        return TRUE;
    }
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return TRUE;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    game_layout_init(&app->layout, allocation.width, allocation.height);

    if (app->overlay != OVERLAY_NONE) {
        return handle_overlay(app, event->x, event->y,
                              allocation.width, allocation.height);
    }

    for (int item = 0; item < 3; ++item) {
        if (!game_rect_contains(app->layout.toolbar_item[item],
                                event->x, event->y)) continue;
        cancel_hold(app);
        if (item == 0) {
            int next;
            do {
                next = (int)g_random_int_range(0, GAME_PLAYER_COUNT);
            } while (next == app->game.first_player);
            game_set_first(&app->game, next);
            persist(app);
        } else if (item == 1) {
            app->overlay = OVERLAY_NEW_GAME;
        } else {
            open_menu(app);
        }
        gtk_widget_queue_draw(app->canvas);
        return TRUE;
    }

    GameControl control;
    if (!game_layout_hit_test(&app->layout, event->x, event->y, &control)) {
        cancel_hold(app);
        return TRUE;
    }
    if (app->hold.active && !game_control_equal(app->hold.control, control)) {
        cancel_hold(app);
    }

    GameHoldDecision decision = game_hold_press(&app->hold, control);
    if (decision == GAME_HOLD_START) {
        app->hold_before = app->game;
        cancel_hold_timer(app);
        app->long_press_timer = g_timeout_add(LONG_PRESS_DELAY_MS,
                                              on_long_press, app);
        if (game_apply_tap(&app->game, control)) {
            persist(app);
            if (control.type == GAME_CONTROL_MONARCH) {
                gtk_widget_queue_draw(app->canvas);
            } else {
                queue_player(app, control.player);
            }
        }
    } else if (decision == GAME_HOLD_PROMOTE) {
        cancel_hold_timer(app);
        apply_hold_promotion(app);
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer data) {
    (void)widget;
    if (event->button == 1) cancel_hold(data);
    return TRUE;
}

static gboolean on_motion(GtkWidget *widget, GdkEventMotion *event,
                          gpointer data) {
    App *app = data;
    if (app->power.sleeping) {
        cancel_hold(app);
        return TRUE;
    }
    if (!app->hold.active) return TRUE;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    game_layout_init(&app->layout, allocation.width, allocation.height);
    GameControl control;
    if (!game_layout_hit_test(&app->layout, event->x, event->y, &control) ||
        !game_control_equal(app->hold.control, control)) {
        cancel_hold(app);
    }
    return TRUE;
}

static gboolean on_leave(GtkWidget *widget, GdkEventCrossing *event,
                         gpointer data) {
    (void)widget;
    (void)event;
    cancel_hold(data);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)widget;
    App *app = data;
    if (app->power.sleeping) return TRUE;
    if (event->keyval == GDK_Escape) {
        cancel_hold(app);
        if (app->overlay == OVERLAY_NONE) {
            open_menu(app);
        } else {
            app->overlay = OVERLAY_NONE;
        }
        gtk_widget_queue_draw(app->canvas);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    if (setpgid(0, 0) != 0 || !install_signal_handlers()) {
        fprintf(stderr, "Could not initialize process lifecycle: %s\n",
                strerror(errno));
        return 1;
    }
    gtk_init(&argc, &argv);

    App app;
    memset(&app, 0, sizeof(app));
    game_hold_reset(&app.hold);
    device_power_state_init(&app.power);
    app.device_backend = device_lipc_backend();
    const char *state_path = argc > 1 ? argv[1] :
        "/mnt/us/extensions/mtg-life-counter/data/state.txt";
    snprintf(app.state_path, sizeof(app.state_path), "%s", state_path);
    if (!game_load(&app.game, app.state_path)) {
        game_reset(&app.game);
        persist(&app);
    }

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), WINDOW_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1264, 1680);
    gtk_window_fullscreen(GTK_WINDOW(app.window));
    g_signal_connect(app.window, "delete-event",
                     G_CALLBACK(on_window_delete), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);

    app.canvas = gtk_drawing_area_new();
    gtk_widget_add_events(app.canvas,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(app.canvas, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.canvas, "button-press-event",
                     G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.canvas, "button-release-event",
                     G_CALLBACK(on_button_release), &app);
    g_signal_connect(app.canvas, "motion-notify-event",
                     G_CALLBACK(on_motion), &app);
    g_signal_connect(app.canvas, "leave-notify-event",
                     G_CALLBACK(on_leave), &app);
    gtk_container_add(GTK_CONTAINER(app.window), app.canvas);

    gtk_widget_show_all(app.window);
    if (!start_power_watch(&app)) schedule_power_restart(&app);
    reconcile_power_state(&app);
    (void)g_timeout_add(100, poll_termination, &app);
    gtk_main();
    return 0;
}
