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
    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        game->life[player] = GAME_STARTING_LIFE;
        game->poison[player] = 0;
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            game->commander_damage[player][source] = 0;
        }
    }
    game->first_player = GAME_NO_PLAYER;
    game->monarch_player = GAME_NO_PLAYER;
}

void game_adjust(GameState *game, int player, int delta) {
    if (!valid_player(player)) return;
    if (delta > 0 && game->life[player] > INT_MAX - delta) return;
    if (delta < 0 && game->life[player] < INT_MIN - delta) return;
    game->life[player] += delta;
}

bool game_add_commander_damage(GameState *game, int recipient, int source) {
    if (!valid_player(recipient) || !valid_player(source)) return false;
    if (game->commander_damage[recipient][source] == INT_MAX ||
        game->life[recipient] == INT_MIN) return false;
    ++game->commander_damage[recipient][source];
    --game->life[recipient];
    return true;
}

bool game_clear_commander_damage(GameState *game, int recipient, int source) {
    if (!valid_player(recipient) || !valid_player(source)) return false;
    int damage = game->commander_damage[recipient][source];
    if (damage <= 0 || game->life[recipient] > INT_MAX - damage) return false;
    game->life[recipient] += damage;
    game->commander_damage[recipient][source] = 0;
    return true;
}

bool game_apply_tap(GameState *game, GameControl control) {
    if (!valid_player(control.player)) return false;
    if (control.type == GAME_CONTROL_LIFE) {
        if (control.delta != -1 && control.delta != 1) return false;
        int previous = game->life[control.player];
        game_adjust(game, control.player, control.delta);
        return game->life[control.player] != previous;
    }
    if (control.type == GAME_CONTROL_COMMANDER) {
        return game_add_commander_damage(game, control.player, control.source);
    }
    if (control.type == GAME_CONTROL_POISON) {
        if (game->poison[control.player] == INT_MAX) return false;
        ++game->poison[control.player];
        return true;
    }
    if (control.type == GAME_CONTROL_MONARCH) {
        if (game->monarch_player == control.player) return false;
        game->monarch_player = control.player;
        return true;
    }
    return false;
}

bool game_apply_hold_from_snapshot(GameState *game, const GameState *before,
                                   GameControl control) {
    if (!game || !before || !valid_player(control.player)) return false;

    if (control.type == GAME_CONTROL_LIFE) {
        if (control.delta != -1 && control.delta != 1) return false;
        long long target = (long long)before->life[control.player] +
                           (long long)control.delta * GAME_LONG_PRESS_LIFE_DELTA;
        if (target < INT_MIN || target > INT_MAX ||
            game->life[control.player] == (int)target) return false;
        game->life[control.player] = (int)target;
        return true;
    }
    if (control.type == GAME_CONTROL_COMMANDER) {
        if (!valid_player(control.source)) return false;
        int damage = before->commander_damage[control.player][control.source];
        long long target_life = (long long)before->life[control.player] + damage;
        if (damage < 0 || target_life < INT_MIN || target_life > INT_MAX) return false;
        bool changed = game->life[control.player] != (int)target_life ||
                       game->commander_damage[control.player][control.source] != 0;
        game->life[control.player] = (int)target_life;
        game->commander_damage[control.player][control.source] = 0;
        return changed;
    }
    if (control.type == GAME_CONTROL_POISON) {
        if (game->poison[control.player] == 0) return false;
        game->poison[control.player] = 0;
        return true;
    }
    if (control.type == GAME_CONTROL_MONARCH) {
        int target = before->monarch_player == control.player
            ? GAME_NO_PLAYER : before->monarch_player;
        if (game->monarch_player == target) return false;
        game->monarch_player = target;
        return true;
    }
    return false;
}

bool game_control_equal(GameControl left, GameControl right) {
    return left.type == right.type && left.player == right.player &&
           left.source == right.source && left.delta == right.delta;
}

void game_set_first(GameState *game, int player) {
    if (valid_player(player) || player == GAME_NO_PLAYER) game->first_player = player;
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
    bool commander_seen[GAME_PLAYER_COUNT][GAME_PLAYER_COUNT] = {{false}};
    bool poison_seen[GAME_PLAYER_COUNT] = {false};
    bool version_seen = false;
    bool first_seen = false;
    bool monarch_seen = false;
    bool valid = true;
    char line[128];
    while (valid && fgets(line, sizeof(line), file)) {
        char key[16];
        int value;
        if (!parse_line(line, key, sizeof(key), &value)) {
            valid = false;
        } else if (strcmp(key, "VERSION") == 0 && !version_seen && value == 2) {
            version_seen = true;
        } else if (key[0] == 'P' && key[1] >= '1' && key[1] <= '4' &&
                   key[2] == '\0') {
            int index = key[1] - '1';
            if (life_seen[index]) {
                valid = false;
            } else {
                loaded.life[index] = value;
                life_seen[index] = true;
            }
        } else if (key[0] == 'C' && key[1] == 'D' &&
                   key[2] >= '1' && key[2] <= '4' &&
                   key[3] >= '1' && key[3] <= '4' && key[4] == '\0') {
            int recipient = key[2] - '1';
            int source = key[3] - '1';
            if (value < 0 || commander_seen[recipient][source]) {
                valid = false;
            } else {
                loaded.commander_damage[recipient][source] = value;
                commander_seen[recipient][source] = true;
            }
        } else if (strncmp(key, "POISON", 6) == 0 &&
                   key[6] >= '1' && key[6] <= '4' && key[7] == '\0') {
            int player = key[6] - '1';
            if (value < 0 || poison_seen[player]) {
                valid = false;
            } else {
                loaded.poison[player] = value;
                poison_seen[player] = true;
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
    if (version_seen) {
        if (!first_seen || !monarch_seen) valid = false;
        for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
            if (!poison_seen[player]) valid = false;
            for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
                if (!commander_seen[player][source]) valid = false;
            }
        }
    } else {
        for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
            if (poison_seen[player]) valid = false;
            for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
                if (commander_seen[player][source]) valid = false;
            }
        }
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

    bool ok = fprintf(file, "VERSION=2\nP1=%d\nP2=%d\nP3=%d\nP4=%d\n",
                      game->life[0], game->life[1], game->life[2],
                      game->life[3]) > 0;
    for (int recipient = 0; ok && recipient < GAME_PLAYER_COUNT; ++recipient) {
        for (int source = 0; ok && source < GAME_PLAYER_COUNT; ++source) {
            ok = fprintf(file, "CD%d%d=%d\n", recipient + 1, source + 1,
                         game->commander_damage[recipient][source]) > 0;
        }
    }
    for (int player = 0; ok && player < GAME_PLAYER_COUNT; ++player) {
        ok = fprintf(file, "POISON%d=%d\n", player + 1,
                     game->poison[player]) > 0;
    }
    if (ok) {
        ok = fprintf(file, "FIRST=%d\nMONARCH=%d\n",
                     game->first_player + 1,
                     game->monarch_player + 1) > 0;
    }
    if (ok && fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temp_path, path) != 0) ok = false;
    if (!ok) unlink(temp_path);
    free(temp_path);
    return ok;
}
