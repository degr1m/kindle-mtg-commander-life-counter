#include "game.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WINDOW_TITLE "L:A_N:application_ID:com.mtgcommander.lifecounter_PC:N_O:U"
#define FIRST_ICON_PATH "/mnt/us/extensions/mtg-life-counter/assets/first-player.png"
#define FIRST_ICON_PANEL_SCALE 0.12
#define LONG_PRESS_DELAY_MS 650

typedef enum {
    CONFIRM_NONE,
    CONFIRM_NEW_GAME,
    CONFIRM_EXIT,
} ConfirmAction;

typedef struct {
    GtkWidget *window;
    GtkWidget *canvas;
    GdkPixbuf *first_icon;
    GameState game;
    char state_path[1024];
    ConfirmAction confirmation;
    guint long_press_timer;
    int long_press_player;
    int long_press_delta;
} App;

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

static void draw_first_marker(cairo_t *cr, const App *app, double center_x,
                              double center_y, double angle, int pixel_size) {
    if (!app->first_icon) return;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
        app->first_icon, pixel_size, pixel_size, GDK_INTERP_BILINEAR);
    if (!scaled) return;

    cairo_save(cr);
    cairo_translate(cr, center_x, center_y);
    cairo_rotate(cr, angle);
    gdk_cairo_set_source_pixbuf(cr, scaled, -pixel_size / 2.0,
                                -pixel_size / 2.0);
    cairo_paint(cr);
    cairo_restore(cr);
    g_object_unref(scaled);
}

static void draw_player_content(cairo_t *cr, const App *app, int player,
                                double x, double y, double width, double height) {
    char life[16];
    snprintf(life, sizeof(life), "%d", app->game.life[player]);

    set_gray(cr, 0.05);
    double life_size = minimum(width * 0.55, height * 0.43);
    size_t life_length = strlen(life);
    if (life_length > 3) life_size *= 3.0 / life_length;
    draw_text(cr, life, x, y + height * 0.12, width, height * 0.76,
              life_size, TRUE);
}

static void draw_player_name(cairo_t *cr, int player, double x, double y,
                             double width, double height, double angle) {
    char label[24];
    snprintf(label, sizeof(label), "PLAYER %d", player + 1);
    const gboolean clockwise = player == 0 || player == 3;

    cairo_save(cr);
    cairo_translate(cr, x + width * (clockwise ? 0.94 : 0.06),
                    y + height * 0.5);
    cairo_rotate(cr, angle);
    set_gray(cr, 0.05);
    draw_text(cr, label, -height * 0.37, -width * 0.07,
              height * 0.74, width * 0.14, width * 0.07, TRUE);
    cairo_restore(cr);
}

static void draw_player(cairo_t *cr, const App *app, int player,
                        double x, double y, double width, double height) {
    set_gray(cr, 0.965);
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);

    const gboolean clockwise = player == 0 || player == 3;
    const double angle = clockwise ? M_PI / 2.0 : -M_PI / 2.0;
    cairo_save(cr);
    cairo_translate(cr, x + width / 2.0, y + height / 2.0);
    cairo_rotate(cr, angle);
    cairo_translate(cr, -(x + width / 2.0), -(y + height / 2.0));
    draw_player_content(cr, app, player, x, y, width, height);
    cairo_restore(cr);
    draw_player_name(cr, player, x, y, width, height, angle);

    const gboolean left_crown = player == 1 || player == 2;
    const gboolean bottom_crown = player >= 2;
    draw_crown(cr,
               x + width * (left_crown ? 0.14 : 0.86),
               y + height * (bottom_crown ? 0.91 : 0.09),
               width * 0.12,
               app->game.monarch_player == player,
               angle);

    if (app->game.first_player == player) {
        const gboolean marker_right = player == 1 || player == 2;
        draw_first_marker(cr, app,
                          x + width * (marker_right ? 0.88 : 0.12),
                          y + height * 0.5,
                          angle,
                          (int)(width * FIRST_ICON_PANEL_SCALE));
    }

    set_gray(cr, 0.05);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, x, y, width, height);
    cairo_stroke(cr);
}

static void draw_toolbar(cairo_t *cr, double y, double width, double height) {
    const double cuts[] = {0.0, 0.274, 0.726, 1.0};
    const char *labels[] = {"PICK FIRST", "NEW GAME", "EXIT"};

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

static void draw_confirmation(cairo_t *cr, double width, double height,
                              ConfirmAction action) {
    const gboolean exiting = action == CONFIRM_EXIT;
    set_gray(cr, 0.03);
    cairo_paint(cr);

    double dialog_width = width * 0.78;
    double dialog_height = height * 0.36;
    double x = (width - dialog_width) / 2.0;
    double y = (height - dialog_height) / 2.0;
    set_gray(cr, 0.965);
    cairo_rectangle(cr, x, y, dialog_width, dialog_height);
    cairo_fill(cr);
    set_gray(cr, 1.0);
    cairo_set_line_width(cr, 8.0);
    cairo_rectangle(cr, x, y, dialog_width, dialog_height);
    cairo_stroke(cr);

    set_gray(cr, 0.03);
    draw_text(cr, exiting ? "EXIT GAME?" : "START NEW GAME?",
              x, y + dialog_height * 0.08,
              dialog_width, dialog_height * 0.23,
              dialog_width * 0.07, TRUE);
    draw_text(cr, exiting ? "Save this game and return to KUAL."
                          : "Reset life, first player, and monarch.",
              x, y + dialog_height * 0.31, dialog_width,
              dialog_height * 0.16, dialog_width * 0.035, TRUE);

    double button_y = y + dialog_height * 0.64;
    double button_height = dialog_height * 0.24;
    double button_width = dialog_width * 0.39;
    set_gray(cr, 0.965);
    cairo_rectangle(cr, x + dialog_width * 0.07, button_y,
                    button_width, button_height);
    cairo_fill(cr);
    set_gray(cr, 0.03);
    cairo_set_line_width(cr, 4.0);
    cairo_rectangle(cr, x + dialog_width * 0.07, button_y,
                    button_width, button_height);
    cairo_stroke(cr);
    draw_text(cr, "CANCEL", x + dialog_width * 0.07, button_y,
              button_width, button_height, button_height * 0.24, TRUE);

    set_gray(cr, 0.03);
    cairo_rectangle(cr, x + dialog_width * 0.54, button_y,
                    button_width, button_height);
    cairo_fill(cr);
    set_gray(cr, 1.0);
    draw_text(cr, exiting ? "EXIT" : "RESET", x + dialog_width * 0.54, button_y,
              button_width, button_height, button_height * 0.24, TRUE);
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
    double bar_height = height * 0.075;
    double panel_height = (height - bar_height) / 2.0;
    double half_width = width / 2.0;

    set_gray(cr, 0.965);
    cairo_paint(cr);
    draw_player(cr, app, 0, 0, 0, half_width, panel_height);
    draw_player(cr, app, 1, half_width, 0, half_width, panel_height);
    draw_toolbar(cr, panel_height, width, bar_height);
    draw_player(cr, app, 3, 0, panel_height + bar_height,
                half_width, panel_height);
    draw_player(cr, app, 2, half_width, panel_height + bar_height,
                half_width, panel_height);
    if (app->confirmation != CONFIRM_NONE) {
        draw_confirmation(cr, width, height, app->confirmation);
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
    int bar_height = (int)(allocation.height * 0.075);
    int panel_height = (allocation.height - bar_height) / 2;
    int panel_width = allocation.width / 2;
    gboolean right = player == 1 || player == 2;
    gboolean bottom = player == 2 || player == 3;
    gtk_widget_queue_draw_area(app->canvas,
                               right ? panel_width : 0,
                               bottom ? panel_height + bar_height : 0,
                               panel_width,
                               panel_height);
}

static void cancel_long_press(App *app) {
    if (app->long_press_timer) g_source_remove(app->long_press_timer);
    app->long_press_timer = 0;
    app->long_press_player = GAME_NO_PLAYER;
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    cancel_long_press(data);
    gtk_main_quit();
}

static void promote_long_press(App *app) {
    if (app->long_press_player == GAME_NO_PLAYER) return;

    int player = app->long_press_player;
    int extra_delta = game_long_press_extra_delta(app->long_press_delta);
    cancel_long_press(app);
    game_adjust(&app->game, player, extra_delta);
    persist(app);
    queue_player(app, player);
}

static gboolean on_long_press(gpointer data) {
    App *app = data;
    app->long_press_timer = 0;
    promote_long_press(app);
    return FALSE;
}

static void start_long_press(App *app, int player, int delta) {
    cancel_long_press(app);
    app->long_press_player = player;
    app->long_press_delta = delta;
    app->long_press_timer = g_timeout_add(LONG_PRESS_DELAY_MS,
                                          on_long_press, app);
}

static gboolean handle_confirmation(App *app, double x, double y,
                                    double width, double height) {
    double dialog_width = width * 0.78;
    double dialog_height = height * 0.36;
    double dialog_x = (width - dialog_width) / 2.0;
    double dialog_y = (height - dialog_height) / 2.0;
    double button_y = dialog_y + dialog_height * 0.64;
    double button_bottom = button_y + dialog_height * 0.24;
    if (y < button_y || y > button_bottom) return TRUE;

    if (x >= dialog_x + dialog_width * 0.07 &&
        x <= dialog_x + dialog_width * 0.46) {
        app->confirmation = CONFIRM_NONE;
    } else if (x >= dialog_x + dialog_width * 0.54 &&
               x <= dialog_x + dialog_width * 0.93) {
        ConfirmAction action = app->confirmation;
        app->confirmation = CONFIRM_NONE;
        if (action == CONFIRM_NEW_GAME) game_reset(&app->game);
        persist(app);
        if (action == CONFIRM_EXIT) {
            gtk_widget_destroy(app->window);
            return TRUE;
        }
    }
    gtk_widget_queue_draw(app->canvas);
    return TRUE;
}

static int monarch_at(double x, double y, double width, double height,
                      double bar_height) {
    const double half_width = width / 2.0;
    const double panel_height = (height - bar_height) / 2.0;
    const double bar_bottom = panel_height + bar_height;
    const gboolean right = x >= half_width;
    const gboolean bottom = y >= bar_bottom;
    const int player = bottom ? (right ? 2 : 3) : (right ? 1 : 0);
    const gboolean left_crown = player == 1 || player == 2;
    const double panel_x = right ? half_width : 0.0;
    const double panel_y = bottom ? bar_bottom : 0.0;
    const double crown_x = panel_x + half_width * (left_crown ? 0.14 : 0.86);
    const double crown_y = panel_y + panel_height * (bottom ? 0.91 : 0.09);
    const double radius = half_width * 0.12;
    return fabs(x - crown_x) <= radius && fabs(y - crown_y) <= radius
        ? player : GAME_NO_PLAYER;
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                                gpointer data) {
    App *app = data;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int width = allocation.width;
    int height = allocation.height;
    int bar_height = (int)(height * 0.075);
    int bar_top = (height - bar_height) / 2;
    int bar_bottom = bar_top + bar_height;

    if (app->confirmation != CONFIRM_NONE) {
        return handle_confirmation(app, event->x, event->y, width, height);
    }

    if (event->y >= bar_top && event->y <= bar_bottom) {
        if (event->x < width * 0.274) {
            int next;
            do {
                next = (int)g_random_int_range(0, GAME_PLAYER_COUNT);
            } while (next == app->game.first_player);
            game_set_first(&app->game, next);
            persist(app);
        } else if (event->x < width * 0.726) {
            app->confirmation = CONFIRM_NEW_GAME;
        } else {
            app->confirmation = CONFIRM_EXIT;
        }
        gtk_widget_queue_draw(app->canvas);
        return TRUE;
    }

    int monarch = monarch_at(event->x, event->y, width, height, bar_height);
    if (monarch != GAME_NO_PLAYER) {
        int previous = app->game.monarch_player;
        game_toggle_monarch(&app->game, monarch);
        persist(app);
        queue_player(app, previous);
        queue_player(app, monarch);
        return TRUE;
    }

    int player = -1;
    int delta = 0;
    if (game_map_touch((int)event->x, (int)event->y, width, height,
                       bar_height, &player, &delta)) {
        if (app->long_press_timer && app->long_press_player == player &&
            app->long_press_delta == delta) {
            promote_long_press(app);
            return TRUE;
        }
        game_adjust(&app->game, player, delta);
        persist(app);
        queue_player(app, player);
        start_long_press(app, player, delta);
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer data) {
    (void)widget;
    (void)event;
    cancel_long_press(data);
    return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)widget;
    App *app = data;
    if (event->keyval == GDK_Escape) {
        persist(app);
        gtk_widget_destroy(app->window);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    App app;
    memset(&app, 0, sizeof(app));
    app.long_press_player = GAME_NO_PLAYER;
    const char *state_path = argc > 1 ? argv[1] :
        "/mnt/us/extensions/mtg-life-counter/data/state.txt";
    snprintf(app.state_path, sizeof(app.state_path), "%s", state_path);
    if (!game_load(&app.game, app.state_path)) {
        game_reset(&app.game);
        persist(&app);
    }
    GError *icon_error = NULL;
    app.first_icon = gdk_pixbuf_new_from_file(FIRST_ICON_PATH, &icon_error);
    if (!app.first_icon) {
        fprintf(stderr, "Could not load first-player icon: %s\n",
                icon_error ? icon_error->message : "unknown error");
        if (icon_error) g_error_free(icon_error);
    }

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), WINDOW_TITLE);
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1264, 1680);
    gtk_window_fullscreen(GTK_WINDOW(app.window));
    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), &app);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);

    app.canvas = gtk_drawing_area_new();
    gtk_widget_add_events(app.canvas,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(app.canvas, "expose-event", G_CALLBACK(on_expose), &app);
    g_signal_connect(app.canvas, "button-press-event",
                     G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.canvas, "button-release-event",
                     G_CALLBACK(on_button_release), &app);
    gtk_container_add(GTK_CONTAINER(app.window), app.canvas);

    gtk_widget_show_all(app.window);
    gtk_main();
    if (app.first_icon) g_object_unref(app.first_icon);
    return 0;
}
