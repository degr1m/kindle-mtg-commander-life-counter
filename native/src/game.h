#ifndef MTG_LIFE_COUNTER_GAME_H
#define MTG_LIFE_COUNTER_GAME_H

#include <stdbool.h>

#define GAME_PLAYER_COUNT 4
#define GAME_STARTING_LIFE 40
#define GAME_LONG_PRESS_LIFE_DELTA 10
#define GAME_NO_PLAYER (-1)

typedef enum {
    GAME_CONTROL_NONE,
    GAME_CONTROL_LIFE,
    GAME_CONTROL_COMMANDER,
    GAME_CONTROL_POISON,
    GAME_CONTROL_MONARCH,
} GameControlType;

typedef struct {
    GameControlType type;
    int player;
    int source;
    int delta;
} GameControl;

typedef struct {
    int life[GAME_PLAYER_COUNT];
    int commander_damage[GAME_PLAYER_COUNT][GAME_PLAYER_COUNT];
    int poison[GAME_PLAYER_COUNT];
    int first_player;
    int monarch_player;
} GameState;

void game_reset(GameState *game);
void game_adjust(GameState *game, int player, int delta);
bool game_add_commander_damage(GameState *game, int recipient, int source);
bool game_clear_commander_damage(GameState *game, int recipient, int source);
bool game_apply_tap(GameState *game, GameControl control);
bool game_apply_hold_from_snapshot(GameState *game, const GameState *before,
                                   GameControl control);
bool game_control_equal(GameControl left, GameControl right);
void game_set_first(GameState *game, int player);
bool game_load(GameState *game, const char *path);
bool game_save(const GameState *game, const char *path);

#endif
