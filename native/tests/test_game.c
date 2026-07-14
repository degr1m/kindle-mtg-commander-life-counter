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

static void test_reset_clears_v2_trackers(void) {
    GameState game;
    memset(&game, 0x7f, sizeof(game));

    game_reset(&game);

    for (int recipient = 0; recipient < GAME_PLAYER_COUNT; ++recipient) {
        assert(game.poison[recipient] == 0);
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            assert(game.commander_damage[recipient][source] == 0);
        }
    }
}

static void test_commander_damage_tap_tracks_source_and_life(void) {
    GameState game;
    game_reset(&game);

    assert(game_add_commander_damage(&game, 1, 3));
    assert(game_add_commander_damage(&game, 1, 3));

    assert(game.commander_damage[1][3] == 2);
    assert(game.life[1] == 38);
    assert(game.commander_damage[3][1] == 0);
    assert(game.life[3] == 40);
}

static void test_commander_damage_hold_clears_and_refunds_life(void) {
    GameState game;
    game_reset(&game);
    assert(game_add_commander_damage(&game, 2, 0));
    assert(game_add_commander_damage(&game, 2, 0));
    assert(game_add_commander_damage(&game, 2, 0));
    assert(game_add_commander_damage(&game, 2, 1));

    assert(game_clear_commander_damage(&game, 2, 0));

    assert(game.commander_damage[2][0] == 0);
    assert(game.commander_damage[2][1] == 1);
    assert(game.life[2] == 39);
    assert(!game_clear_commander_damage(&game, 2, 0));
    assert(game.life[2] == 39);
}

static void test_commander_damage_actions_are_atomic_at_integer_limits(void) {
    GameState game;
    game_reset(&game);

    game.life[0] = INT_MIN;
    assert(!game_add_commander_damage(&game, 0, 1));
    assert(game.life[0] == INT_MIN);
    assert(game.commander_damage[0][1] == 0);

    game_reset(&game);
    game.commander_damage[1][2] = INT_MAX;
    assert(!game_add_commander_damage(&game, 1, 2));
    assert(game.life[1] == 40);
    assert(game.commander_damage[1][2] == INT_MAX);

    game_reset(&game);
    game.life[3] = INT_MAX;
    game.commander_damage[3][0] = 1;
    assert(!game_clear_commander_damage(&game, 3, 0));
    assert(game.life[3] == INT_MAX);
    assert(game.commander_damage[3][0] == 1);
}

static void test_first_player_selection(void) {
    GameState game;
    game_reset(&game);
    game_set_first(&game, 2);
    assert(game.first_player == 2);
    game_set_first(&game, GAME_NO_PLAYER);
    assert(game.first_player == GAME_NO_PLAYER);
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

static void test_game_control_life_tap_and_hold_total_ten(void) {
    GameState game;
    game_reset(&game);
    GameControl plus = {
        .type = GAME_CONTROL_LIFE,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 1,
    };
    GameControl minus = plus;
    minus.player = 1;
    minus.delta = -1;

    GameState before_press = game;
    assert(game_apply_tap(&game, plus));
    assert(game_apply_hold_from_snapshot(&game, &before_press, plus));
    assert(game.life[0] == 50);

    before_press = game;
    assert(game_apply_tap(&game, minus));
    assert(game_apply_hold_from_snapshot(&game, &before_press, minus));
    assert(game.life[1] == 30);
}

static void test_game_control_commander_tap_and_hold_refund(void) {
    GameState game;
    game_reset(&game);
    GameControl commander = {
        .type = GAME_CONTROL_COMMANDER,
        .player = 2,
        .source = 1,
        .delta = 0,
    };

    GameState before_press = game;
    assert(game_apply_tap(&game, commander));
    assert(game.commander_damage[2][1] == 1);
    assert(game.life[2] == 39);
    assert(game_apply_hold_from_snapshot(&game, &before_press, commander));
    assert(game.commander_damage[2][1] == 0);
    assert(game.life[2] == 40);
}

static void test_game_control_poison_taps_and_hold_clears(void) {
    GameState game;
    game_reset(&game);
    GameControl poison = {
        .type = GAME_CONTROL_POISON,
        .player = 3,
        .source = GAME_NO_PLAYER,
        .delta = 0,
    };

    assert(game_apply_tap(&game, poison));
    assert(game.poison[3] == 1);
    GameState before_press = game;
    assert(game_apply_tap(&game, poison));
    assert(game.poison[3] == 2);
    assert(game_apply_hold_from_snapshot(&game, &before_press, poison));
    assert(game.poison[3] == 0);
    before_press = game;
    assert(!game_apply_hold_from_snapshot(&game, &before_press, poison));
}

static void test_poison_tap_stops_at_integer_limit(void) {
    GameState game;
    game_reset(&game);
    game.poison[0] = INT_MAX;
    GameControl poison = {
        .type = GAME_CONTROL_POISON,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 0,
    };

    assert(!game_apply_tap(&game, poison));
    assert(game.poison[0] == INT_MAX);
}

static void test_game_control_monarch_tap_assigns_and_active_hold_clears(void) {
    GameState game;
    game_reset(&game);
    GameControl crown_0 = {
        .type = GAME_CONTROL_MONARCH,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 0,
    };
    GameControl crown_2 = crown_0;
    crown_2.player = 2;

    assert(game_apply_tap(&game, crown_0));
    assert(game.monarch_player == 0);
    assert(!game_apply_tap(&game, crown_0));
    assert(game.monarch_player == 0);
    assert(game_apply_tap(&game, crown_2));
    assert(game.monarch_player == 2);

    GameState before_press = game;
    assert(!game_apply_tap(&game, crown_2));
    assert(game_apply_hold_from_snapshot(&game, &before_press, crown_2));
    assert(game.monarch_player == GAME_NO_PLAYER);
}

static void test_monarch_hold_restores_press_time_owner(void) {
    GameState game;
    game_reset(&game);
    GameControl crown_0 = {
        .type = GAME_CONTROL_MONARCH,
        .player = 0,
        .source = GAME_NO_PLAYER,
        .delta = 0,
    };
    GameControl crown_2 = crown_0;
    crown_2.player = 2;

    assert(game_apply_tap(&game, crown_0));
    GameState before_press = game;
    assert(game_apply_tap(&game, crown_2));
    assert(game.monarch_player == 2);
    assert(game_apply_hold_from_snapshot(&game, &before_press, crown_2));
    assert(game.monarch_player == 0);

    before_press = game;
    assert(!game_apply_tap(&game, crown_0));
    assert(game_apply_hold_from_snapshot(&game, &before_press, crown_0));
    assert(game.monarch_player == GAME_NO_PLAYER);
}

static void test_game_control_equality_uses_every_identity_field(void) {
    GameControl base = {
        .type = GAME_CONTROL_COMMANDER,
        .player = 1,
        .source = 3,
        .delta = 0,
    };
    GameControl changed = base;
    assert(game_control_equal(base, changed));
    changed.type = GAME_CONTROL_POISON;
    assert(!game_control_equal(base, changed));
    changed = base;
    changed.player = 2;
    assert(!game_control_equal(base, changed));
    changed = base;
    changed.source = 0;
    assert(!game_control_equal(base, changed));
    changed = base;
    changed.delta = 1;
    assert(!game_control_equal(base, changed));
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
    written.commander_damage[0][3] = 7;
    written.commander_damage[2][1] = 21;
    written.poison[1] = 4;
    written.poison[3] = 10;
    written.first_player = 3;
    written.monarch_player = 1;
    assert(game_save(&written, path));

    GameState loaded;
    assert(game_load(&loaded, path));
    assert(memcmp(written.life, loaded.life, sizeof(written.life)) == 0);
    assert(memcmp(written.commander_damage, loaded.commander_damage,
                  sizeof(written.commander_damage)) == 0);
    assert(memcmp(written.poison, loaded.poison, sizeof(written.poison)) == 0);
    assert(loaded.first_player == 3);
    assert(loaded.monarch_player == 1);
    unlink(path);
}

static void test_saved_state_declares_version_two(void) {
    char path[] = "/tmp/mtg-game-version-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    GameState game;
    game_reset(&game);
    assert(game_save(&game, path));

    FILE *file = fopen(path, "r");
    assert(file);
    char line[32];
    assert(fgets(line, sizeof(line), file));
    assert(strcmp(line, "VERSION=2\n") == 0);
    assert(fclose(file) == 0);
    unlink(path);
}

static void test_v1_state_migrates_with_zeroed_v2_trackers(void) {
    char path[] = "/tmp/mtg-game-v1-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("P1=35\nP2=40\nP3=42\nP4=17\nFIRST=3\nMONARCH=2\n",
                 file) >= 0);
    assert(fclose(file) == 0);

    GameState game;
    assert(game_load(&game, path));
    assert(game.life[0] == 35);
    assert(game.life[2] == 42);
    assert(game.first_player == 2);
    assert(game.monarch_player == 1);
    for (int player = 0; player < GAME_PLAYER_COUNT; ++player) {
        assert(game.poison[player] == 0);
        for (int source = 0; source < GAME_PLAYER_COUNT; ++source) {
            assert(game.commander_damage[player][source] == 0);
        }
    }
    unlink(path);
}

static void test_incomplete_v2_state_is_rejected(void) {
    char path[] = "/tmp/mtg-game-v2-incomplete-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("VERSION=2\nP1=40\nP2=40\nP3=40\nP4=40\n"
                 "FIRST=0\nMONARCH=0\n", file) >= 0);
    assert(fclose(file) == 0);

    GameState game;
    assert(!game_load(&game, path));
    unlink(path);
}

static void test_unversioned_state_rejects_v2_keys(void) {
    char path[] = "/tmp/mtg-game-unversioned-v2-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("P1=40\nP2=40\nP3=40\nP4=40\n"
                 "CD11=1\nFIRST=0\nMONARCH=0\n", file) >= 0);
    assert(fclose(file) == 0);

    GameState game;
    game_reset(&game);
    game.life[0] = 123;
    assert(!game_load(&game, path));
    assert(game.life[0] == 123);
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
    test_reset_clears_v2_trackers();
    test_commander_damage_tap_tracks_source_and_life();
    test_commander_damage_hold_clears_and_refunds_life();
    test_commander_damage_actions_are_atomic_at_integer_limits();
    test_first_player_selection();
    test_adjustment_does_not_overflow();
    test_game_control_life_tap_and_hold_total_ten();
    test_game_control_commander_tap_and_hold_refund();
    test_game_control_poison_taps_and_hold_clears();
    test_poison_tap_stops_at_integer_limit();
    test_game_control_monarch_tap_assigns_and_active_hold_clears();
    test_monarch_hold_restores_press_time_owner();
    test_game_control_equality_uses_every_identity_field();

    test_state_round_trip();
    test_saved_state_declares_version_two();
    test_v1_state_migrates_with_zeroed_v2_trackers();
    test_incomplete_v2_state_is_rejected();
    test_unversioned_state_rejects_v2_keys();
    test_duplicate_state_key_is_rejected();
    puts("PASS game core");
    return 0;
}
