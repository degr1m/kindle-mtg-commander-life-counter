#include "game.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool valid_player(int player) {
    return player >= 0 && player < GAME_PLAYER_COUNT;
}

void game_reset(GameState *game) {
    for (int i = 0; i < GAME_PLAYER_COUNT; ++i) game->life[i] = GAME_STARTING_LIFE;
    game->first_player = GAME_NO_PLAYER;
    game->monarch_player = GAME_NO_PLAYER;
}

void game_adjust(GameState *game, int player, int delta) {
    if (!valid_player(player)) return;
    if (delta > 0 && game->life[player] > INT_MAX - delta) return;
    if (delta < 0 && game->life[player] < INT_MIN - delta) return;
    game->life[player] += delta;
}

int game_long_press_extra_delta(int delta) {
    return delta * (GAME_LONG_PRESS_LIFE_DELTA - 1);
}

void game_set_first(GameState *game, int player) {
    if (valid_player(player) || player == GAME_NO_PLAYER) game->first_player = player;
}

void game_toggle_monarch(GameState *game, int player) {
    if (!valid_player(player)) return;
    game->monarch_player = game->monarch_player == player ? GAME_NO_PLAYER : player;
}

bool game_map_touch(int x, int y, int width, int height, int bar_height,
                    int *player, int *delta) {
    if (width <= 0 || height <= 0 || bar_height < 0 || !player || !delta) return false;

    const int mid_x = width / 2;
    const int mid_y = height / 2;
    const int bar_top = mid_y - bar_height / 2;
    const int bar_bottom = mid_y + bar_height / 2;
    if (y >= bar_top && y <= bar_bottom) return false;

    const bool right = x >= mid_x;
    const bool bottom = y > bar_bottom;
    int panel_height;
    int local_y;
    if (bottom) {
        panel_height = height - bar_bottom;
        local_y = y - bar_bottom;
        *player = right ? 2 : 3;
    } else {
        panel_height = bar_top;
        local_y = y;
        *player = right ? 1 : 0;
    }

    const bool lower_half = local_y >= panel_height / 2;
    const bool clockwise = *player == 0 || *player == 3;
    *delta = clockwise ? (lower_half ? 1 : -1)
                       : (lower_half ? -1 : 1);
    return true;
}

static bool parse_line(char *line, char *key, size_t key_size, int *value) {
    char *separator = strchr(line, '=');
    if (!separator || separator == line) return false;
    *separator = '\0';
    if (strlen(line) >= key_size) return false;
    strcpy(key, line);

    errno = 0;
    char *end = NULL;
    long parsed = strtol(separator + 1, &end, 10);
    if (errno == ERANGE || end == separator + 1 || parsed < INT_MIN ||
        parsed > INT_MAX) return false;
    while (*end == '\n' || *end == '\r') ++end;
    if (*end != '\0') return false;
    *value = (int)parsed;
    return true;
}

bool game_load(GameState *game, const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return false;

    GameState loaded;
    game_reset(&loaded);
    bool life_seen[GAME_PLAYER_COUNT] = {false};
    bool first_seen = false;
    bool monarch_seen = false;
    bool valid = true;
    char line[128];
    while (valid && fgets(line, sizeof(line), file)) {
        char key[16];
        int value;
        if (!parse_line(line, key, sizeof(key), &value)) {
            valid = false;
        } else if (key[0] == 'P' && key[1] >= '1' && key[1] <= '4' &&
                   key[2] == '\0') {
            int index = key[1] - '1';
            if (life_seen[index]) {
                valid = false;
            } else {
                loaded.life[index] = value;
                life_seen[index] = true;
            }
        } else if (strcmp(key, "FIRST") == 0 && !first_seen &&
                   value >= 0 && value <= GAME_PLAYER_COUNT) {
            loaded.first_player = value - 1;
            first_seen = true;
        } else if (strcmp(key, "MONARCH") == 0 && !monarch_seen &&
                   value >= 0 && value <= GAME_PLAYER_COUNT) {
            loaded.monarch_player = value - 1;
            monarch_seen = true;
        } else {
            valid = false;
        }
    }
    if (ferror(file)) valid = false;
    if (fclose(file) != 0) valid = false;

    for (int i = 0; i < GAME_PLAYER_COUNT; ++i) {
        if (!life_seen[i]) valid = false;
    }
    if (valid) *game = loaded;
    return valid;
}

bool game_save(const GameState *game, const char *path) {
    if (!path) return false;
    size_t temp_size = strlen(path) + 5;
    char *temp_path = malloc(temp_size);
    if (!temp_path) return false;
    snprintf(temp_path, temp_size, "%s.tmp", path);

    FILE *file = fopen(temp_path, "w");
    if (!file) {
        free(temp_path);
        return false;
    }

    bool ok = fprintf(file,
                      "P1=%d\nP2=%d\nP3=%d\nP4=%d\nFIRST=%d\nMONARCH=%d\n",
                      game->life[0], game->life[1], game->life[2],
                      game->life[3], game->first_player + 1,
                      game->monarch_player + 1) > 0;
    if (ok && fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temp_path, path) != 0) ok = false;
    if (!ok) unlink(temp_path);
    free(temp_path);
    return ok;
}
