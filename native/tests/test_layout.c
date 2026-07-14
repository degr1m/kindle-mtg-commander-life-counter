#include "layout.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool near(double actual, double expected) {
    return fabs(actual - expected) < 0.01;
}

static double center_x(GameRect rect) { return rect.x + rect.width / 2.0; }
static double center_y(GameRect rect) { return rect.y + rect.height / 2.0; }

static void test_layout_matches_v2_board_structure(void) {
    GameLayout layout;
    game_layout_init(&layout, 1264.0, 1680.0);

    assert(near(layout.toolbar.height, 126.0));
    assert(near(layout.panel[0].x, 0.0));
    assert(near(layout.panel[0].y, 0.0));
    assert(near(layout.panel[0].width, 632.0));
    assert(near(layout.panel[0].height, 777.0));
    assert(near(layout.panel[1].x, 632.0));
    assert(near(layout.panel[3].y, 903.0));
    assert(near(layout.panel[2].x, 632.0));
    assert(near(layout.panel[2].y, 903.0));

    assert(near(layout.commander[0][0].x, 0.0));
    assert(near(layout.commander[0][0].y, 0.0));
    assert(near(layout.commander[0][0].width, 132.72));
    assert(near(layout.commander[0][0].height, 194.25));
    assert(near(layout.commander[0][3].y, 582.75));

    assert(near(layout.commander[1][3].x, 1131.28));
    assert(near(layout.commander[1][3].y, 0.0));
    assert(near(layout.commander[1][0].y, 582.75));

    assert(layout.monarch[0].y > layout.panel[0].height / 2.0);
    assert(layout.poison[0].y < layout.panel[0].height / 2.0);
    assert(layout.monarch[3].y < layout.panel[3].y + layout.panel[3].height / 2.0);
    assert(layout.poison[3].y > layout.panel[3].y + layout.panel[3].height / 2.0);
}

static void assert_hit(const GameLayout *layout, GameRect rect,
                       GameControlType type, int player, int source, int delta) {
    GameControl control = {0};
    assert(game_layout_hit_test(layout, center_x(rect), center_y(rect), &control));
    assert(control.type == type);
    assert(control.player == player);
    assert(control.source == source);
    assert(control.delta == delta);
}

static void test_hit_testing_prioritizes_v2_controls(void) {
    GameLayout layout;
    game_layout_init(&layout, 1264.0, 1680.0);

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            assert_hit(&layout, layout.commander[player][source],
                       GAME_CONTROL_COMMANDER, player, source, 0);
        }
    }

    assert_hit(&layout, layout.poison[0], GAME_CONTROL_POISON, 0,
               GAME_NO_PLAYER, 0);
    assert_hit(&layout, layout.monarch[2], GAME_CONTROL_MONARCH, 2,
               GAME_NO_PLAYER, 0);

    assert_hit(&layout, layout.life_minus[0], GAME_CONTROL_LIFE, 0,
               GAME_NO_PLAYER, -1);
    assert_hit(&layout, layout.life_plus[0], GAME_CONTROL_LIFE, 0,
               GAME_NO_PLAYER, 1);
    assert_hit(&layout, layout.life_plus[1], GAME_CONTROL_LIFE, 1,
               GAME_NO_PLAYER, 1);
    assert_hit(&layout, layout.life_minus[1], GAME_CONTROL_LIFE, 1,
               GAME_NO_PLAYER, -1);
    assert_hit(&layout, layout.life_plus[2], GAME_CONTROL_LIFE, 2,
               GAME_NO_PLAYER, 1);
    assert_hit(&layout, layout.life_minus[3], GAME_CONTROL_LIFE, 3,
               GAME_NO_PLAYER, -1);

    GameControl control = {0};
    assert(!game_layout_hit_test(&layout, center_x(layout.toolbar),
                                 center_y(layout.toolbar), &control));
    assert(!game_layout_hit_test(&layout, -1.0, -1.0, &control));
}

static void test_rectangles_use_half_open_boundaries(void) {
    GameRect rect = {10.0, 20.0, 30.0, 40.0};
    assert(game_rect_contains(rect, 10.0, 20.0));
    assert(game_rect_contains(rect, 39.999, 59.999));
    assert(!game_rect_contains(rect, 40.0, 30.0));
    assert(!game_rect_contains(rect, 20.0, 60.0));

    GameLayout layout;
    game_layout_init(&layout, 1264.0, 1680.0);
    GameControl control = {0};
    assert(!game_layout_hit_test(&layout,
                                 center_x(layout.commander[0][3]),
                                 layout.toolbar.y, &control));
}

static void test_visual_subregions_match_annotated_positions(void) {
    GameLayout layout;
    game_layout_init(&layout, 1264.0, 1680.0);

    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        const bool right = player == 1 || player == 2;
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            GameRect cell = layout.commander[player][source];
            GameRect label = layout.commander_label[player][source];
            GameRect value = layout.commander_value[player][source];
            assert(game_rect_contains(cell, center_x(label), center_y(label)));
            assert(game_rect_contains(cell, center_x(value), center_y(value)));
            assert(value.width > label.width * 2.0);
            assert(right ? label.x < value.x : value.x < label.x);
            assert(fabs(center_x(value) - center_x(label)) < cell.width * 0.45);
            assert(right
                ? label.x + label.width > value.x
                : value.x + value.width > label.x);
        }

        GameRect skull = layout.poison_skull[player];
        GameRect value = layout.poison_value[player];
        GameRect panel = layout.panel[player];
        const double expected_marker_margin =
            panel.x + panel.width * (right ? 0.05 : 0.95);
        assert(near(layout.marker_margin_x[player], expected_marker_margin));
        assert(near(layout.poison[player].width, panel.width * 0.27));
        assert(right ? skull.x < value.x : value.x < skull.x);
        assert(near(center_y(skull), center_y(value)));

        GameRect first_before = layout.first_triangle[player][0];
        GameRect first_after = layout.first_triangle[player][1];
        const double name_x = panel.x + panel.width * (right ? 0.08 : 0.92);
        assert(near(center_x(first_before), name_x));
        assert(near(center_x(first_after), name_x));
        assert(near(center_y(first_before), panel.y + panel.height * 0.34));
        assert(near(center_y(first_after), panel.y + panel.height * 0.66));

        GameRect minus_visual = layout.life_minus_visual[player];
        GameRect plus_visual = layout.life_plus_visual[player];
        const bool clockwise = player == 0 || player == 3;
        const double symbol_axis_x = right
            ? layout.toolbar_item[2].x
            : layout.toolbar_item[0].x + layout.toolbar_item[0].width;
        const double minus_y = panel.y + panel.height *
            (clockwise ? 0.12 : 0.88);
        const double plus_y = panel.y + panel.height *
            (clockwise ? 0.88 : 0.12);
        assert(near(center_y(minus_visual), minus_y));
        assert(near(center_y(plus_visual), plus_y));
        assert(near(center_x(minus_visual), symbol_axis_x));
        assert(near(center_x(plus_visual), symbol_axis_x));
        assert(near(fmin(fabs(center_y(minus_visual) - center_y(layout.poison[player])),
                         fabs(center_y(minus_visual) - center_y(layout.monarch[player]))),
                    0.0));
        assert(near(fmin(fabs(center_y(plus_visual) - center_y(layout.poison[player])),
                         fabs(center_y(plus_visual) - center_y(layout.monarch[player]))),
                    0.0));
    }
}

static void test_hold_state_promotes_once_and_swallows_late_duplicates(void) {
    GameHoldState hold;
    game_hold_reset(&hold);
    GameControl life = {
        .type = GAME_CONTROL_LIFE,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 1,
    };
    GameControl poison = {
        .type = GAME_CONTROL_POISON,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 0,
    };

    assert(game_hold_press(&hold, life) == GAME_HOLD_START);
    assert(hold.active && !hold.promoted);
    assert(game_hold_timeout(&hold));
    assert(hold.active && hold.promoted);
    assert(game_hold_press(&hold, life) == GAME_HOLD_SWALLOW);
    assert(!game_hold_timeout(&hold));

    assert(game_hold_press(&hold, poison) == GAME_HOLD_START);
    assert(game_control_equal(hold.control, poison));
    assert(!hold.promoted);
    assert(game_hold_press(&hold, poison) == GAME_HOLD_PROMOTE);
    assert(game_hold_press(&hold, poison) == GAME_HOLD_SWALLOW);

    game_hold_release(&hold);
    assert(!hold.active && !hold.promoted);
    assert(game_hold_press(&hold, life) == GAME_HOLD_START);
    game_hold_release(&hold);
    assert(!game_hold_timeout(&hold));
}

int main(void) {
    test_layout_matches_v2_board_structure();
    test_hit_testing_prioritizes_v2_controls();
    test_rectangles_use_half_open_boundaries();
    test_visual_subregions_match_annotated_positions();
    test_hold_state_promotes_once_and_swallows_late_duplicates();
    puts("PASS layout core");
    return 0;
}
