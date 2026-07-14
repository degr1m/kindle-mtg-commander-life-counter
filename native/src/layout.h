#ifndef MTG_LIFE_COUNTER_LAYOUT_H
#define MTG_LIFE_COUNTER_LAYOUT_H

#include "game.h"

#include <stdbool.h>

typedef struct {
    double x;
    double y;
    double width;
    double height;
} GameRect;

typedef struct {
    double width;
    double height;
    GameRect toolbar;
    GameRect toolbar_item[3];
    GameRect panel[GAME_PLAYER_COUNT];
    GameRect content[GAME_PLAYER_COUNT];
    GameRect commander[GAME_PLAYER_COUNT][GAME_PLAYER_COUNT];
    GameRect commander_label[GAME_PLAYER_COUNT][GAME_PLAYER_COUNT];
    GameRect commander_value[GAME_PLAYER_COUNT][GAME_PLAYER_COUNT];
    GameRect poison[GAME_PLAYER_COUNT];
    GameRect poison_skull[GAME_PLAYER_COUNT];
    GameRect poison_value[GAME_PLAYER_COUNT];
    GameRect monarch[GAME_PLAYER_COUNT];
    GameRect life_minus[GAME_PLAYER_COUNT];
    GameRect life_plus[GAME_PLAYER_COUNT];
    GameRect life_minus_visual[GAME_PLAYER_COUNT];
    GameRect life_plus_visual[GAME_PLAYER_COUNT];
    GameRect first_triangle[GAME_PLAYER_COUNT][2];
    double marker_margin_x[GAME_PLAYER_COUNT];
} GameLayout;

typedef struct {
    bool active;
    bool promoted;
    GameControl control;
} GameHoldState;

typedef enum {
    GAME_HOLD_IGNORE,
    GAME_HOLD_START,
    GAME_HOLD_PROMOTE,
    GAME_HOLD_SWALLOW,
} GameHoldDecision;

void game_layout_init(GameLayout *layout, double width, double height);
bool game_rect_contains(GameRect rect, double x, double y);
bool game_layout_hit_test(const GameLayout *layout, double x, double y,
                          GameControl *control);
void game_hold_reset(GameHoldState *hold);
GameHoldDecision game_hold_press(GameHoldState *hold, GameControl control);
bool game_hold_timeout(GameHoldState *hold);
void game_hold_release(GameHoldState *hold);

#endif
