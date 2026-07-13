#ifndef MTG_LIFE_COUNTER_GAME_H
#define MTG_LIFE_COUNTER_GAME_H

#include <stdbool.h>

#define GAME_PLAYER_COUNT 4
#define GAME_STARTING_LIFE 40
#define GAME_LONG_PRESS_LIFE_DELTA 10
#define GAME_NO_PLAYER (-1)

typedef struct {
    int life[GAME_PLAYER_COUNT];
    int first_player;
    int monarch_player;
} GameState;

void game_reset(GameState *game);
void game_adjust(GameState *game, int player, int delta);
int game_long_press_extra_delta(int delta);
void game_set_first(GameState *game, int player);
void game_toggle_monarch(GameState *game, int player);
bool game_map_touch(int x, int y, int width, int height, int bar_height,
                    int *player, int *delta);
bool game_load(GameState *game, const char *path);
bool game_save(const GameState *game, const char *path);

#endif
