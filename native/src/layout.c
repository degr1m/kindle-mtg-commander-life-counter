#include "layout.h"

#include <string.h>

static GameRect rect(double x, double y, double width, double height) {
    GameRect result = {x, y, width, height};
    return result;
}

static GameRect centered_rect(double center_x, double center_y, double size) {
    return rect(center_x - size / 2.0, center_y - size / 2.0, size, size);
}

bool game_rect_contains(GameRect area, double x, double y) {
    return x >= area.x && y >= area.y &&
           x < area.x + area.width && y < area.y + area.height;
}

void game_layout_init(GameLayout *layout, double width, double height) {
    memset(layout, 0, sizeof(*layout));
    if (width <= 0.0 || height <= 0.0) return;

    layout->width = width;
    layout->height = height;
    const double toolbar_height = height * 0.075;
    const double panel_height = (height - toolbar_height) / 2.0;
    const double panel_width = width / 2.0;
    const double lower_y = panel_height + toolbar_height;
    const double strip_width = panel_width * 0.21;
    const double cell_height = panel_height / GAME_PLAYER_COUNT;
    const double marker_size = panel_width * 0.27;

    layout->toolbar = rect(0.0, panel_height, width, toolbar_height);
    const double cuts[] = {0.0, 0.274, 0.726, 1.0};
    for (int item = 0; item < 3; ++item) {
        layout->toolbar_item[item] = rect(
            width * cuts[item], panel_height,
            width * (cuts[item + 1] - cuts[item]), toolbar_height);
    }

    layout->panel[0] = rect(0.0, 0.0, panel_width, panel_height);
    layout->panel[1] = rect(panel_width, 0.0, panel_width, panel_height);
    layout->panel[2] = rect(panel_width, lower_y, panel_width, panel_height);
    layout->panel[3] = rect(0.0, lower_y, panel_width, panel_height);

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        const bool right = player == 1 || player == 2;
        GameRect panel = layout->panel[player];
        const double strip_x = right
            ? panel.x + panel.width - strip_width : panel.x;
        layout->content[player] = rect(
            right ? panel.x : panel.x + strip_width,
            panel.y, panel.width - strip_width, panel.height);

        for (int row = 0; row < GAME_PLAYER_COUNT; ++row) {
            const int source = right ? GAME_PLAYER_COUNT - 1 - row : row;
            GameRect cell = rect(
                strip_x, panel.y + row * cell_height,
                strip_width, cell_height);
            layout->commander[player][source] = cell;
            const double label_width = cell.width * 0.32;
            const double value_width = cell.width * 0.82;
            layout->commander_label[player][source] = rect(
                right ? cell.x : cell.x + cell.width - label_width,
                cell.y, label_width, cell.height);
            layout->commander_value[player][source] = rect(
                right ? cell.x + cell.width - value_width : cell.x,
                cell.y, value_width, cell.height);
        }

        const bool bottom = player == 2 || player == 3;
        const double marker_x = panel.x + panel.width * (right ? 0.16 : 0.84);
        const double poison_y = panel.y + panel.height * (bottom ? 0.88 : 0.12);
        const double monarch_y = panel.y + panel.height * (bottom ? 0.12 : 0.88);
        layout->poison[player] = centered_rect(marker_x, poison_y, marker_size);
        layout->monarch[player] = centered_rect(marker_x, monarch_y, marker_size);
        GameRect poison = layout->poison[player];
        const double value_width = poison.width * 0.40;
        const double skull_width = poison.width - value_width;
        layout->poison_value[player] = rect(
            right ? poison.x + skull_width : poison.x,
            poison.y, value_width, poison.height);
        layout->poison_skull[player] = rect(
            right ? poison.x : poison.x + value_width,
            poison.y, skull_width, poison.height);

        GameRect content = layout->content[player];
        GameRect upper = rect(content.x, content.y, content.width,
                              content.height / 2.0);
        GameRect lower = rect(content.x, content.y + content.height / 2.0,
                              content.width, content.height / 2.0);
        const bool clockwise = player == 0 || player == 3;
        layout->life_minus[player] = clockwise ? upper : lower;
        layout->life_plus[player] = clockwise ? lower : upper;
        const double symbol_height = panel.height * 0.20;
        const double symbol_axis_x = right
            ? layout->toolbar_item[2].x
            : layout->toolbar_item[0].x + layout->toolbar_item[0].width;
        const double minus_center_y = panel.y + panel.height *
            (clockwise ? 0.12 : 0.88);
        const double plus_center_y = panel.y + panel.height *
            (clockwise ? 0.88 : 0.12);
        layout->life_minus_visual[player] = rect(
            symbol_axis_x - content.width / 2.0,
            minus_center_y - symbol_height / 2.0,
            content.width, symbol_height);
        layout->life_plus_visual[player] = rect(
            symbol_axis_x - content.width / 2.0,
            plus_center_y - symbol_height / 2.0,
            content.width, symbol_height);

        const double triangle_size = panel_width * 0.065;
        const double name_x = panel.x + panel.width * (right ? 0.08 : 0.92);
        layout->marker_margin_x[player] =
            panel.x + panel.width * (right ? 0.05 : 0.95);
        layout->first_triangle[player][0] = centered_rect(
            name_x, panel.y + panel.height * 0.34, triangle_size);
        layout->first_triangle[player][1] = centered_rect(
            name_x, panel.y + panel.height * 0.66, triangle_size);
    }
}

static GameControl make_control(GameControlType type, int player,
                                int source, int delta) {
    GameControl control = {type, player, source, delta};
    return control;
}

bool game_layout_hit_test(const GameLayout *layout, double x, double y,
                          GameControl *control) {
    if (!layout || !control || x < 0.0 || y < 0.0 ||
        x > layout->width || y > layout->height) return false;
    *control = make_control(GAME_CONTROL_NONE, GAME_NO_PLAYER,
                            GAME_NO_PLAYER, 0);

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            if (game_rect_contains(layout->commander[player][source], x, y)) {
                *control = make_control(GAME_CONTROL_COMMANDER, player, source, 0);
                return true;
            }
        }
    }

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        if (game_rect_contains(layout->poison[player], x, y)) {
            *control = make_control(GAME_CONTROL_POISON, player,
                                    GAME_NO_PLAYER, 0);
            return true;
        }
        if (game_rect_contains(layout->monarch[player], x, y)) {
            *control = make_control(GAME_CONTROL_MONARCH, player,
                                    GAME_NO_PLAYER, 0);
            return true;
        }
    }

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        if (game_rect_contains(layout->life_minus[player], x, y)) {
            *control = make_control(GAME_CONTROL_LIFE, player,
                                    GAME_NO_PLAYER, -1);
            return true;
        }
        if (game_rect_contains(layout->life_plus[player], x, y)) {
            *control = make_control(GAME_CONTROL_LIFE, player,
                                    GAME_NO_PLAYER, 1);
            return true;
        }
    }
    return false;
}

void game_hold_reset(GameHoldState *hold) {
    if (!hold) return;
    memset(hold, 0, sizeof(*hold));
    hold->control = make_control(GAME_CONTROL_NONE, GAME_NO_PLAYER,
                                 GAME_NO_PLAYER, 0);
}

GameHoldDecision game_hold_press(GameHoldState *hold, GameControl control) {
    if (!hold || control.type == GAME_CONTROL_NONE) return GAME_HOLD_IGNORE;
    if (!hold->active || !game_control_equal(hold->control, control)) {
        hold->active = true;
        hold->promoted = false;
        hold->control = control;
        return GAME_HOLD_START;
    }
    if (hold->promoted) return GAME_HOLD_SWALLOW;
    hold->promoted = true;
    return GAME_HOLD_PROMOTE;
}

bool game_hold_timeout(GameHoldState *hold) {
    if (!hold || !hold->active || hold->promoted) return false;
    hold->promoted = true;
    return true;
}

void game_hold_release(GameHoldState *hold) {
    game_hold_reset(hold);
}
