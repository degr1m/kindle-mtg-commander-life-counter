#include "device.h"

#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROPERTY_COUNT 3

static const char *test_program;

typedef struct {
    int battery;
    int light;
    int light_max;
    bool battery_available;
    bool light_available;
    bool max_available;
    bool read_failure;
    bool write_failure;
    bool write_acknowledged_but_unapplied;
    bool power_state_available;
    bool power_sleeping;
    int power_state_reads;
    int flip_power_state_after_reads;
    bool power_sleeping_after_flip;
    int write_attempts;
    int writes;
} FakeDevice;

static bool fake_read(void *context, const char *property, int *value) {
    FakeDevice *device = context;
    if (strcmp(property, "battLevel") == 0 && device->battery_available) {
        *value = device->battery;
        return true;
    }
    if (strcmp(property, "flIntensity") == 0 && device->light_available &&
        !device->read_failure) {
        *value = device->light;
        return true;
    }
    if (strcmp(property, "flMaxIntensity") == 0 && device->max_available) {
        *value = device->light_max;
        return true;
    }
    return false;
}

static bool fake_write(void *context, const char *property, int value) {
    FakeDevice *device = context;
    if (strcmp(property, "flIntensity") != 0 || !device->light_available ||
        value < 0 || value > device->light_max) {
        return false;
    }
    ++device->write_attempts;
    if (device->write_failure) return false;
    if (device->write_acknowledged_but_unapplied) return true;
    device->light = value;
    ++device->writes;
    return true;
}

static bool fake_read_sleeping(void *context, bool *sleeping) {
    FakeDevice *device = context;
    if (!device->power_state_available) return false;
    ++device->power_state_reads;
    *sleeping = device->flip_power_state_after_reads > 0 &&
                        device->power_state_reads >
                            device->flip_power_state_after_reads
                    ? device->power_sleeping_after_flip
                    : device->power_sleeping;
    return true;
}

static DeviceBackend fake_backend(FakeDevice *device) {
    DeviceBackend backend = {
        .context = device,
        .read_int = fake_read,
        .write_int = fake_write,
        .read_sleeping = fake_read_sleeping,
    };
    return backend;
}

static void create_script(char *path, const char *body) {
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    FILE *file = fdopen(descriptor, "w");
    assert(file);
    assert(fputs("#!/bin/sh\n", file) >= 0);
    assert(fputs(body, file) >= 0);
    assert(fclose(file) == 0);
    assert(chmod(path, 0700) == 0);
}

static double monotonic_seconds(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static int run_descendant_helper(const char *marker) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        (void)signal(SIGHUP, SIG_IGN);
        (void)signal(SIGINT, SIG_IGN);
        (void)signal(SIGTERM, SIG_IGN);
        (void)poll(NULL, 0, 400);
        FILE *file = fopen(marker, "w");
        if (file) {
            (void)fputs("leaked\n", file);
            (void)fclose(file);
        }
        _exit(0);
    }
    (void)poll(NULL, 0, 5000);
    return 0;
}

static int run_success_descendant_helper(const char *marker,
                                         bool print_value) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        (void)signal(SIGHUP, SIG_IGN);
        (void)signal(SIGINT, SIG_IGN);
        (void)signal(SIGTERM, SIG_IGN);
        (void)close(STDOUT_FILENO);
        (void)close(STDERR_FILENO);
        (void)poll(NULL, 0, 400);
        FILE *file = fopen(marker, "w");
        if (file) {
            (void)fputs("leaked\n", file);
            (void)fclose(file);
        }
        _exit(0);
    }
    if (print_value) (void)puts("12");
    return 0;
}

static void test_refresh_reads_battery_and_front_light(void) {
    FakeDevice fake = {
        .battery = 73,
        .light = 18,
        .light_max = 25,
        .battery_available = true,
        .light_available = true,
        .max_available = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;

    assert(device_refresh(&backend, &status));
    assert(status.battery_available);
    assert(status.battery_percent == 73);
    assert(status.light_available);
    assert(status.light_level == 18);
    assert(status.light_max == 25);
}

static void test_toggle_restores_previous_nonzero_brightness(void) {
    FakeDevice fake = {
        .battery = 50,
        .light = 18,
        .light_max = 25,
        .battery_available = true,
        .light_available = true,
        .max_available = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    int remembered = 0;
    assert(device_refresh(&backend, &status));

    assert(device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.light == 0);
    assert(status.light_level == 0);
    assert(remembered == 18);

    assert(device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.light == 18);
    assert(status.light_level == 18);
    assert(remembered == 18);
    assert(fake.writes == 2);
}

static void test_unavailable_properties_degrade_safely(void) {
    FakeDevice fake = {0};
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    int remembered = 0;

    assert(!device_refresh(&backend, &status));
    assert(!status.battery_available);
    assert(!status.light_available);
    assert(!device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.writes == 0);
}

static void test_turning_on_without_memory_uses_safe_midlevel(void) {
    FakeDevice fake = {
        .light = 0,
        .light_max = 25,
        .light_available = true,
        .max_available = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    int remembered = 0;
    assert(device_refresh(&backend, &status));

    assert(device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.light == 12);
    assert(status.light_level == 12);
    assert(remembered == 12);
}

static void test_toggle_requires_exact_physical_readback(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .max_available = true,
        .write_acknowledged_but_unapplied = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    int remembered = 0;
    assert(device_refresh(&backend, &status));

    assert(!device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.light == 18);
    assert(status.light_available);
    assert(status.light_level == 18);
    assert(remembered == 18);

    fake.write_acknowledged_but_unapplied = false;
    fake.read_failure = true;
    assert(!device_toggle_front_light(&backend, &status, &remembered));
    assert(fake.light == 0);
    assert(status.light_level == 18);
}

static void test_power_events_parse_strictly(void) {
    assert(device_power_event_parse("goingToScreenSaver 2\n") ==
           DEVICE_POWER_SLEEP);
    assert(device_power_event_parse("readyToSuspend 10\n") ==
           DEVICE_POWER_SLEEP);
    assert(device_power_event_parse("outOfScreenSaver 1\n") ==
           DEVICE_POWER_WAKE);
    assert(device_power_event_parse("wakeupFromSuspend\n") ==
           DEVICE_POWER_WAKE);
    assert(device_power_event_parse("goingToScreenSaverFake\n") ==
           DEVICE_POWER_NONE);
    assert(device_power_event_parse("noise outOfScreenSaver\n") ==
           DEVICE_POWER_NONE);
}

static void test_sleep_disables_light_and_wake_restores_exact_level(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .max_available = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    DevicePowerState power;
    device_power_state_init(&power);
    assert(device_refresh(&backend, &status));

    assert(device_power_sleep(&backend, &status, &power));
    assert(power.sleeping);
    assert(power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 18);
    assert(fake.light == 0);
    assert(status.light_level == 0);
    assert(fake.writes == 1);

    assert(device_power_sleep(&backend, &status, &power));
    assert(fake.writes == 1);
    assert(device_power_wake(&backend, &status, &power));
    assert(!power.sleeping);
    assert(!power.restore_light);
    assert(fake.light == 18);
    assert(status.light_level == 18);
    assert(fake.writes == 2);
    assert(device_power_wake(&backend, &status, &power));
    assert(fake.writes == 2);
}

static void test_fallback_sleep_event_retries_failed_light_off(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .write_failure = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 18,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(!device_power_sleep(&backend, &status, &power));
    assert(power.sleeping);
    assert(!power.light_off);
    assert(fake.light == 18);
    assert(fake.write_attempts == 1);

    fake.write_failure = false;
    assert(device_power_sleep(&backend, &status, &power));
    assert(power.light_off);
    assert(fake.light == 0);
    assert(fake.write_attempts == 2);
    assert(device_power_wake(&backend, &status, &power));
    assert(fake.light == 18);
}

static void test_exit_retries_light_off_before_failing(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .write_failure = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 18,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);
    power.sleeping = true;
    power.restore_light = true;
    power.light_before_sleep = 18;

    assert(!device_power_exit(&backend, &status, &power));
    assert(fake.light == 18);
    assert(fake.write_attempts == 3);
    assert(power.sleeping);
    assert(power.restore_light);
    assert(power.light_before_sleep == 18);
}

static void test_sleep_keeps_an_off_light_off_and_exit_forces_off(void) {
    FakeDevice fake = {
        .light = 0,
        .light_max = 25,
        .light_available = true,
        .max_available = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status;
    DevicePowerState power;
    device_power_state_init(&power);
    assert(device_refresh(&backend, &status));

    assert(device_power_sleep(&backend, &status, &power));
    assert(power.sleeping);
    assert(!power.restore_light);
    assert(fake.writes == 0);
    assert(device_power_wake(&backend, &status, &power));
    assert(fake.light == 0);
    assert(fake.writes == 0);

    fake.light = 11;
    status.light_level = 11;
    assert(device_power_exit(&backend, &status, &power));
    assert(fake.light == 0);
    assert(status.light_level == 0);
    assert(fake.writes == 1);
}

static void test_sleep_records_observed_zero_and_reconciles_missed_wake(void) {
    FakeDevice fake = {
        .light = 0,
        .light_max = 25,
        .light_available = true,
        .power_state_available = true,
        .power_sleeping = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 18,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(device_power_sleep(&backend, &status, &power));
    assert(status.light_level == 0);
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.sleeping);

    fake.power_sleeping = false;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_AWAKE);
    assert(!power.sleeping);
    assert(fake.light == 0);
}

static void test_reconcile_recovers_lost_events_and_retries_commands(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .power_state_available = true,
        .power_sleeping = true,
        .write_failure = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 18,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.sleeping);
    assert(!power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 18);

    fake.write_failure = false;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.light_off);
    assert(fake.light == 0);

    fake.power_sleeping = false;
    fake.write_failure = true;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_AWAKE);
    assert(!power.sleeping);
    assert(power.restore_light);
    assert(fake.light == 0);

    fake.power_sleeping = true;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.sleeping);
    assert(power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 18);

    fake.power_sleeping = false;
    fake.write_failure = false;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_AWAKE);
    assert(!power.restore_light);
    assert(fake.light == 18);

    fake.power_state_available = false;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_UNKNOWN);
}

static void test_sleeping_watchdog_rechecks_physical_light(void) {
    FakeDevice fake = {
        .light = 12,
        .light_max = 25,
        .light_available = true,
        .power_state_available = true,
        .power_sleeping = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 12,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 12);
    assert(fake.light == 0);

    fake.light = 7;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 12);
    assert(fake.light == 0);

    fake.light = 8;
    fake.read_failure = true;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(!power.light_off);
    assert(fake.light == 8);

    fake.read_failure = false;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.light_off);
    assert(fake.light == 0);
}

static void test_wake_rechecks_physical_state_before_settling(void) {
    FakeDevice fake = {
        .light = 16,
        .light_max = 25,
        .light_available = true,
        .power_state_available = true,
        .power_sleeping = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 16,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(fake.light == 0);

    fake.power_sleeping = false;
    fake.power_state_reads = 0;
    fake.flip_power_state_after_reads = 1;
    fake.power_sleeping_after_flip = true;
    assert(device_power_reconcile(&backend, &status, &power) ==
           DEVICE_POWER_OBSERVED_SLEEPING);
    assert(power.sleeping);
    assert(power.light_off);
    assert(power.restore_light);
    assert(power.light_before_sleep == 16);
    assert(fake.light == 0);
}

static void test_command_backend_reads_power_state_strictly(void) {
    char script[] = "/tmp/mtg-device-state-XXXXXX";
    create_script(script, "printf 'screenSaver\\n'\n");
    DeviceCommandConfig config = {
        .get_program = script,
        .set_program = "/usr/bin/true",
        .timeout_ms = 500,
    };
    DeviceBackend backend = device_command_backend(&config);
    bool sleeping = false;

    assert(backend.read_sleeping(backend.context, &sleeping));
    assert(sleeping);
    assert(unlink(script) == 0);
}

static void test_power_transitions_require_readback_confirmation(void) {
    FakeDevice fake = {
        .light = 18,
        .light_max = 25,
        .light_available = true,
        .write_acknowledged_but_unapplied = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 18,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(!device_power_sleep(&backend, &status, &power));
    assert(fake.light == 18);
    assert(power.restore_light);
    assert(!power.light_off);

    fake.write_acknowledged_but_unapplied = false;
    assert(device_power_sleep(&backend, &status, &power));
    assert(fake.light == 0);
    assert(power.light_off);

    fake.write_acknowledged_but_unapplied = true;
    assert(!device_power_wake(&backend, &status, &power));
    assert(fake.light == 0);
    assert(power.restore_light);

    fake.write_acknowledged_but_unapplied = false;
    assert(device_power_wake(&backend, &status, &power));
    assert(fake.light == 18);
    assert(!power.restore_light);

    power.restore_light = true;
    power.light_before_sleep = 18;
    power.light_before_sleep_known = true;
    fake.write_acknowledged_but_unapplied = true;
    assert(!device_power_exit(&backend, &status, &power));
    assert(fake.light == 18);
    assert(power.restore_light);
    assert(power.light_before_sleep == 18);
}

static void test_sleep_and_exit_do_not_trust_stale_off_status(void) {
    FakeDevice fake = {
        .light = 14,
        .light_max = 25,
        .light_available = true,
        .max_available = true,
        .read_failure = true,
    };
    DeviceBackend backend = fake_backend(&fake);
    DeviceStatus status = {
        .light_available = true,
        .light_level = 0,
        .light_max = 25,
    };
    DevicePowerState power;
    device_power_state_init(&power);

    assert(!device_power_sleep(&backend, &status, &power));
    assert(fake.light == 14);
    assert(fake.writes == 0);
    assert(!power.light_before_sleep_known);

    fake.read_failure = false;
    assert(device_power_sleep(&backend, &status, &power));
    assert(fake.light == 0);
    assert(fake.writes == 1);
    assert(power.restore_light);
    assert(power.light_before_sleep_known);
    assert(power.light_before_sleep == 14);
    assert(device_power_wake(&backend, &status, &power));
    assert(fake.light == 14);

    fake.light = 9;
    fake.read_failure = true;
    status.light_level = 0;
    assert(!device_power_exit(&backend, &status, &power));
    assert(fake.light == 0);
    assert(fake.writes == 5);

    fake.read_failure = false;
    assert(device_power_exit(&backend, &status, &power));
    assert(status.light_level == 0);
}

static void test_command_backend_rejects_oversized_output(void) {
    char script[] = "/tmp/mtg-device-oversized-XXXXXX";
    create_script(script,
        "printf '12                                                                X'\n");
    DeviceCommandConfig config = {
        .get_program = script,
        .set_program = "/usr/bin/true",
        .timeout_ms = 500,
    };
    DeviceBackend backend = device_command_backend(&config);
    int value = 0;

    assert(!backend.read_int(backend.context, "battLevel", &value));
    assert(unlink(script) == 0);
}

static void test_command_backend_reads_strict_integer_output(void) {
    char script[] = "/tmp/mtg-device-integer-XXXXXX";
    create_script(script, "printf '12\\n'\n");
    DeviceCommandConfig config = {
        .get_program = script,
        .set_program = "/usr/bin/true",
        .timeout_ms = 500,
    };
    DeviceBackend backend = device_command_backend(&config);
    int value = 0;

    assert(backend.read_int(backend.context, "battLevel", &value));
    assert(value == 12);
    assert(unlink(script) == 0);
}

static void test_command_backend_rejects_embedded_nul_output(void) {
    char script[] = "/tmp/mtg-device-nul-XXXXXX";
    create_script(script, "printf '12\\000junk'\n");
    DeviceCommandConfig config = {
        .get_program = script,
        .set_program = "/usr/bin/true",
        .timeout_ms = 500,
    };
    DeviceBackend backend = device_command_backend(&config);
    int value = 0;

    assert(!backend.read_int(backend.context, "battLevel", &value));
    assert(unlink(script) == 0);
}

static void test_command_backend_kills_descendants_on_timeout(void) {
    char marker[] = "/tmp/mtg-device-descendant-marker-XXXXXX";
    int marker_fd = mkstemp(marker);
    assert(marker_fd >= 0);
    assert(close(marker_fd) == 0);
    assert(unlink(marker) == 0);
    assert(setenv("MTG_TEST_DESCENDANT_MARKER", marker, 1) == 0);
    DeviceCommandConfig config = {
        .get_program = test_program,
        .set_program = "/usr/bin/true",
        .timeout_ms = 100,
    };
    DeviceBackend backend = device_command_backend(&config);
    int value = 0;

    assert(!backend.read_int(backend.context, "battLevel", &value));
    assert(unsetenv("MTG_TEST_DESCENDANT_MARKER") == 0);
    assert(poll(NULL, 0, 600) == 0);
    assert(access(marker, F_OK) != 0);
}

static void test_command_backend_kills_descendants_after_success(void) {
    const char *modes[] = {"read", "write"};
    for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        char marker[] = "/tmp/mtg-device-success-marker-XXXXXX";
        int marker_fd = mkstemp(marker);
        assert(marker_fd >= 0);
        assert(close(marker_fd) == 0);
        assert(unlink(marker) == 0);
        assert(setenv("MTG_TEST_SUCCESS_DESCENDANT_MARKER", marker, 1) == 0);
        assert(setenv("MTG_TEST_SUCCESS_DESCENDANT_MODE", modes[index], 1) == 0);
        DeviceCommandConfig config = {
            .get_program = test_program,
            .set_program = test_program,
            .timeout_ms = 500,
        };
        DeviceBackend backend = device_command_backend(&config);
        if (index == 0) {
            int value = 0;
            assert(backend.read_int(backend.context, "battLevel", &value));
            assert(value == 12);
        } else {
            assert(backend.write_int(backend.context, "flIntensity", 12));
        }
        assert(unsetenv("MTG_TEST_SUCCESS_DESCENDANT_MARKER") == 0);
        assert(unsetenv("MTG_TEST_SUCCESS_DESCENDANT_MODE") == 0);
        assert(poll(NULL, 0, 600) == 0);
        assert(access(marker, F_OK) != 0);
    }
}

static void test_command_backend_bounds_hanging_reads_and_writes(void) {
    char script[] = "/tmp/mtg-device-hang-XXXXXX";
    create_script(script, "exec /bin/sleep 5\n");
    DeviceCommandConfig config = {
        .get_program = script,
        .set_program = script,
        .timeout_ms = 100,
    };
    DeviceBackend backend = device_command_backend(&config);
    int value = 0;

    double started = monotonic_seconds();
    assert(!backend.read_int(backend.context, "battLevel", &value));
    assert(monotonic_seconds() - started < 1.0);

    started = monotonic_seconds();
    assert(!backend.write_int(backend.context, "flIntensity", 12));
    assert(monotonic_seconds() - started < 1.0);
    assert(unlink(script) == 0);
}

int main(int argc, char **argv) {
    const char *marker = getenv("MTG_TEST_DESCENDANT_MARKER");
    if (marker && argc >= 3) return run_descendant_helper(marker);
    marker = getenv("MTG_TEST_SUCCESS_DESCENDANT_MARKER");
    const char *mode = getenv("MTG_TEST_SUCCESS_DESCENDANT_MODE");
    if (marker && mode && argc >= 3) {
        return run_success_descendant_helper(marker,
                                             strcmp(mode, "read") == 0);
    }
    test_program = argv[0];
    test_refresh_reads_battery_and_front_light();
    test_toggle_restores_previous_nonzero_brightness();
    test_unavailable_properties_degrade_safely();
    test_turning_on_without_memory_uses_safe_midlevel();
    test_toggle_requires_exact_physical_readback();
    test_power_events_parse_strictly();
    test_sleep_disables_light_and_wake_restores_exact_level();
    test_fallback_sleep_event_retries_failed_light_off();
    test_exit_retries_light_off_before_failing();
    test_sleep_keeps_an_off_light_off_and_exit_forces_off();
    test_sleep_records_observed_zero_and_reconciles_missed_wake();
    test_reconcile_recovers_lost_events_and_retries_commands();
    test_sleeping_watchdog_rechecks_physical_light();
    test_wake_rechecks_physical_state_before_settling();
    test_sleep_and_exit_do_not_trust_stale_off_status();
    test_command_backend_reads_power_state_strictly();
    test_power_transitions_require_readback_confirmation();
    test_command_backend_rejects_oversized_output();
    test_command_backend_reads_strict_integer_output();
    test_command_backend_rejects_embedded_nul_output();
    test_command_backend_kills_descendants_on_timeout();
    test_command_backend_kills_descendants_after_success();
    test_command_backend_bounds_hanging_reads_and_writes();
    puts("PASS device adapter");
    return 0;
}
