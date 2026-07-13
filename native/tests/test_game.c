#include "game.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_reset_and_adjust(void) {
    GameState game;
    game_reset(&game);
    for (int i = 0; i < GAME_PLAYER_COUNT; ++i) assert(game.life[i] == 40);
    assert(game.first_player == GAME_NO_PLAYER);
    assert(game.monarch_player == GAME_NO_PLAYER);

    game_adjust(&game, 0, -1);
    game_adjust(&game, 3, 1);
    assert(game.life[0] == 39);
    assert(game.life[3] == 41);
}

static void test_first_player_selection(void) {
    GameState game;
    game_reset(&game);
    game_set_first(&game, 2);
    assert(game.first_player == 2);
    game_set_first(&game, GAME_NO_PLAYER);
    assert(game.first_player == GAME_NO_PLAYER);
}

static void test_monarch_is_unique_and_toggleable(void) {
    GameState game;
    game_reset(&game);
    game_toggle_monarch(&game, 0);
    assert(game.monarch_player == 0);
    game_toggle_monarch(&game, 3);
    assert(game.monarch_player == 3);
    game_toggle_monarch(&game, 3);
    assert(game.monarch_player == GAME_NO_PLAYER);
}

static void test_adjustment_does_not_overflow(void) {
    GameState game;
    game_reset(&game);
    game.life[0] = INT_MAX;
    game_adjust(&game, 0, 1);
    assert(game.life[0] == INT_MAX);
    game.life[1] = INT_MIN;
    game_adjust(&game, 1, -1);
    assert(game.life[1] == INT_MIN);
}

static void test_long_press_promotes_a_tap_to_ten(void) {
    GameState game;
    game_reset(&game);
    game_adjust(&game, 0, 1);
    game_adjust(&game, 0, game_long_press_extra_delta(1));
    game_adjust(&game, 1, -1);
    game_adjust(&game, 1, game_long_press_extra_delta(-1));
    assert(game.life[0] == 50);
    assert(game.life[1] == 30);
}

static void test_long_press_changes_life_by_ten(void) {
    GameState game;
    game_reset(&game);
    game_adjust(&game, 0, GAME_LONG_PRESS_LIFE_DELTA);
    game_adjust(&game, 1, -GAME_LONG_PRESS_LIFE_DELTA);
    assert(game.life[0] == 50);
    assert(game.life[1] == 30);
}

static void test_touch_mapping_matches_four_seat_orientations(void) {
    int player = -1;
    int delta = 0;

    assert(game_map_touch(100, 100, 1000, 1400, 100, &player, &delta));
    assert(player == 0 && delta == -1);
    assert(game_map_touch(100, 500, 1000, 1400, 100, &player, &delta));
    assert(player == 0 && delta == 1);

    assert(game_map_touch(600, 100, 1000, 1400, 100, &player, &delta));
    assert(player == 1 && delta == 1);
    assert(game_map_touch(600, 500, 1000, 1400, 100, &player, &delta));
    assert(player == 1 && delta == -1);

    assert(game_map_touch(100, 900, 1000, 1400, 100, &player, &delta));
    assert(player == 3 && delta == -1);
    assert(game_map_touch(100, 1300, 1000, 1400, 100, &player, &delta));
    assert(player == 3 && delta == 1);

    assert(game_map_touch(800, 900, 1000, 1400, 100, &player, &delta));
    assert(player == 2 && delta == 1);
    assert(game_map_touch(800, 1300, 1000, 1400, 100, &player, &delta));
    assert(player == 2 && delta == -1);

    assert(!game_map_touch(500, 700, 1000, 1400, 100, &player, &delta));
}

static void test_state_round_trip(void) {
    char path[] = "/tmp/mtg-game-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    GameState written;
    game_reset(&written);
    written.life[0] = 35;
    written.life[2] = 47;
    written.first_player = 3;
    written.monarch_player = 1;
    assert(game_save(&written, path));

    GameState loaded;
    assert(game_load(&loaded, path));
    assert(memcmp(written.life, loaded.life, sizeof(written.life)) == 0);
    assert(loaded.first_player == 3);
    assert(loaded.monarch_player == 1);
    unlink(path);
}

static void test_duplicate_state_key_is_rejected(void) {
    char path[] = "/tmp/mtg-game-bad-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("P1=40\nP1=9\nP2=40\nP3=40\nP4=40\n", file) >= 0);
    assert(fclose(file) == 0);

    GameState game;
    assert(!game_load(&game, path));
    unlink(path);
}

int main(void) {
    test_reset_and_adjust();
    test_first_player_selection();
    test_monarch_is_unique_and_toggleable();
    test_adjustment_does_not_overflow();
    test_long_press_promotes_a_tap_to_ten();
    test_long_press_changes_life_by_ten();
    test_touch_mapping_matches_four_seat_orientations();
    test_state_round_trip();
    test_duplicate_state_key_is_rejected();
    puts("PASS game core");
    return 0;
}
