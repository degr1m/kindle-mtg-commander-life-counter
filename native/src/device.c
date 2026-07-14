#include "device.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POWERD_SERVICE "com.lab126.powerd"
#define DEFAULT_LIGHT_MAX 24
#define LIPC_TIMEOUT_MS 500
#define WAIT_INTERVAL_MS 10

static long long monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remaining_milliseconds(long long deadline) {
    long long now = monotonic_milliseconds();
    if (now < 0 || now >= deadline) return 0;
    long long remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static void terminate_and_reap(pid_t pid) {
    if (kill(-pid, SIGKILL) != 0) (void)kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
}

static void terminate_reaped_process_group(pid_t pid) {
    while (kill(-pid, SIGKILL) != 0 && errno == EINTR) {
    }
}

static bool establish_process_group(pid_t pid) {
    for (;;) {
        if (setpgid(pid, pid) == 0) return true;
        if (errno == EINTR) continue;
        if (errno == EACCES) return getpgid(pid) == pid;
        return false;
    }
}

static bool wait_for_success_until(pid_t pid, long long deadline) {
    for (;;) {
        int status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            terminate_reaped_process_group(pid);
            return success;
        }
        if (result < 0 && errno != EINTR) return false;

        int remaining = remaining_milliseconds(deadline);
        if (remaining <= 0) {
            terminate_and_reap(pid);
            return false;
        }
        int pause_ms = remaining < WAIT_INTERVAL_MS
            ? remaining : WAIT_INTERVAL_MS;
        while (poll(NULL, 0, pause_ms) < 0) {
            if (errno != EINTR) {
                terminate_and_reap(pid);
                return false;
            }
            pause_ms = remaining_milliseconds(deadline);
            if (pause_ms <= 0) {
                terminate_and_reap(pid);
                return false;
            }
            if (pause_ms > WAIT_INTERVAL_MS) pause_ms = WAIT_INTERVAL_MS;
        }
    }
}

static void redirect_to_null(int descriptor) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd < 0) return;
    (void)dup2(null_fd, descriptor);
    close(null_fd);
}

static bool command_read_text(void *context, const char *property,
                              char *buffer, size_t buffer_size) {
    DeviceCommandConfig *config = context;
    if (!config || !config->get_program || !config->timeout_ms ||
        !property || !buffer || buffer_size < 2) return false;
    long long started = monotonic_milliseconds();
    if (started < 0) return false;
    long long deadline = started + config->timeout_ms;

    int output[2];
    if (pipe(output) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(output[0]);
        close(output[1]);
        return false;
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(127);
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0) _exit(127);
        close(output[1]);
        redirect_to_null(STDERR_FILENO);
        execlp(config->get_program, config->get_program, POWERD_SERVICE,
               property, (char *)NULL);
        _exit(127);
    }

    if (!establish_process_group(pid)) {
        close(output[0]);
        close(output[1]);
        terminate_and_reap(pid);
        return false;
    }

    close(output[1]);
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int remaining = remaining_milliseconds(deadline);
        if (remaining <= 0) {
            close(output[0]);
            terminate_and_reap(pid);
            return false;
        }
        struct pollfd poll_fd = {
            .fd = output[0],
            .events = POLLIN | POLLHUP,
            .revents = 0,
        };
        int ready = poll(&poll_fd, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (poll_fd.revents & POLLNVAL)) {
            close(output[0]);
            terminate_and_reap(pid);
            return false;
        }

        char chunk[64];
        ssize_t count = read(output[0], chunk, sizeof(chunk));
        if (count > 0) {
            size_t available = buffer_size - 1 - used;
            size_t received = (size_t)count;
            size_t copy = received < available ? received : available;
            if (copy) memcpy(buffer + used, chunk, copy);
            used += copy;
            if (received > copy) overflow = true;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            close(output[0]);
            terminate_and_reap(pid);
            return false;
        }
    }
    close(output[0]);
    if (!wait_for_success_until(pid, deadline) || overflow ||
        memchr(buffer, '\0', used)) return false;
    buffer[used] = '\0';

    return true;
}

static bool command_read_int(void *context, const char *property, int *value) {
    char buffer[64];
    if (!value || !command_read_text(context, property,
                                     buffer, sizeof(buffer))) return false;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(buffer, &end, 10);
    if (errno == ERANGE || end == buffer || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end != '\0') return false;
    *value = (int)parsed;
    return true;
}

static bool command_read_sleeping(void *context, bool *sleeping) {
    char buffer[64];
    if (!sleeping || !command_read_text(context, "state",
                                        buffer, sizeof(buffer))) return false;

    char *start = buffer;
    while (*start && isspace((unsigned char)*start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    if (strcmp(start, "active") == 0) {
        *sleeping = false;
        return true;
    }
    if (strcmp(start, "screenSaver") == 0 ||
        strcmp(start, "readyToSuspend") == 0 ||
        strcmp(start, "suspended") == 0) {
        *sleeping = true;
        return true;
    }
    return false;
}

static bool command_write_int(void *context, const char *property, int value) {
    DeviceCommandConfig *config = context;
    if (!config || !config->set_program || !config->timeout_ms || !property) {
        return false;
    }
    long long started = monotonic_milliseconds();
    if (started < 0) return false;
    long long deadline = started + config->timeout_ms;

    char value_text[16];
    int length = snprintf(value_text, sizeof(value_text), "%d", value);
    if (length < 0 || (size_t)length >= sizeof(value_text)) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(127);
        redirect_to_null(STDOUT_FILENO);
        redirect_to_null(STDERR_FILENO);
        execlp(config->set_program, config->set_program, POWERD_SERVICE,
               property, value_text, (char *)NULL);
        _exit(127);
    }
    if (!establish_process_group(pid)) {
        terminate_and_reap(pid);
        return false;
    }
    return wait_for_success_until(pid, deadline);
}

DeviceBackend device_command_backend(DeviceCommandConfig *config) {
    DeviceBackend backend = {
        .context = config,
        .read_int = command_read_int,
        .write_int = command_write_int,
        .read_sleeping = command_read_sleeping,
    };
    return backend;
}

DeviceBackend device_lipc_backend(void) {
    static DeviceCommandConfig config = {
        .get_program = "lipc-get-prop",
        .set_program = "lipc-set-prop",
        .timeout_ms = LIPC_TIMEOUT_MS,
    };
    return device_command_backend(&config);
}

bool device_refresh(DeviceBackend *backend, DeviceStatus *status) {
    if (!backend || !backend->read_int || !status) return false;
    memset(status, 0, sizeof(*status));

    int value = 0;
    if (backend->read_int(backend->context, "battLevel", &value) &&
        value >= 0 && value <= 100) {
        status->battery_available = true;
        status->battery_percent = value;
    }

    int light_max = DEFAULT_LIGHT_MAX;
    if (backend->read_int(backend->context, "flMaxIntensity", &value) &&
        value > 0 && value <= 100) {
        light_max = value;
    }
    status->light_max = light_max;
    if (backend->read_int(backend->context, "flIntensity", &value) &&
        value >= 0 && value <= light_max) {
        status->light_available = true;
        status->light_level = value;
    }
    return status->battery_available || status->light_available;
}

bool device_toggle_front_light(DeviceBackend *backend, DeviceStatus *status,
                               int *remembered_brightness) {
    if (!backend || !backend->write_int || !status || !remembered_brightness ||
        !status->light_available || status->light_max <= 0) return false;

    int target;
    if (status->light_level > 0) {
        *remembered_brightness = status->light_level;
        target = 0;
    } else {
        target = *remembered_brightness > 0
            ? *remembered_brightness : status->light_max / 2;
        if (target < 1) target = 1;
        if (target > status->light_max) target = status->light_max;
        *remembered_brightness = target;
    }
    if (!backend->write_int(backend->context, "flIntensity", target)) return false;

    DeviceStatus refreshed;
    if (!device_refresh(backend, &refreshed) || !refreshed.light_available ||
        refreshed.light_level != target) return false;
    *status = refreshed;
    return true;
}

static bool event_name_matches(const char *line, const char *name) {
    if (!line || !name) return false;
    const size_t length = strlen(name);
    if (strncmp(line, name, length) != 0) return false;
    const char delimiter = line[length];
    return delimiter == '\0' || delimiter == ' ' || delimiter == '\t' ||
           delimiter == '\r' || delimiter == '\n';
}

DevicePowerEvent device_power_event_parse(const char *line) {
    if (event_name_matches(line, "goingToScreenSaver") ||
        event_name_matches(line, "readyToSuspend")) {
        return DEVICE_POWER_SLEEP;
    }
    if (event_name_matches(line, "outOfScreenSaver") ||
        event_name_matches(line, "wakeupFromSuspend")) {
        return DEVICE_POWER_WAKE;
    }
    return DEVICE_POWER_NONE;
}

void device_power_state_init(DevicePowerState *state) {
    if (state) memset(state, 0, sizeof(*state));
}

static bool read_light_level(DeviceBackend *backend, int *level) {
    if (!backend || !level) return false;
    int current = 0;
    if (backend->read_int &&
        backend->read_int(backend->context, "flIntensity", &current) &&
        current >= 0 && current <= 100) {
        *level = current;
        return true;
    }
    return false;
}

static void record_light_level(DeviceStatus *status, int level) {
    if (!status) return;
    status->light_available = true;
    status->light_level = level;
}

static bool light_level_is(DeviceBackend *backend, int expected) {
    int actual = 0;
    return read_light_level(backend, &actual) && actual == expected;
}

bool device_power_sleep(DeviceBackend *backend, DeviceStatus *status,
                        DevicePowerState *state) {
    if (!backend || !backend->write_int || !status || !state) return false;
    if (state->sleeping && state->light_off) {
        int level = 0;
        if (!read_light_level(backend, &level)) {
            state->light_off = false;
            return false;
        }
        if (level == 0) {
            record_light_level(status, 0);
            return true;
        }
        state->light_off = false;
    }

    if (!state->sleeping) {
        state->sleeping = true;
        state->light_off = false;
        if (!state->restore_light) {
            state->light_before_sleep_known = false;
            state->light_before_sleep = 0;
        }
    }
    if (!state->light_before_sleep_known) {
        int level = 0;
        if (!read_light_level(backend, &level)) return false;
        state->light_before_sleep_known = true;
        if (level > 0) {
            state->restore_light = true;
            state->light_before_sleep = level;
        }
        if (level == 0) {
            state->light_off = true;
            record_light_level(status, 0);
            return true;
        }
    } else if (light_level_is(backend, 0)) {
        state->light_off = true;
        record_light_level(status, 0);
        return true;
    }
    if (!backend->write_int(backend->context, "flIntensity", 0)) return false;
    if (!light_level_is(backend, 0)) return false;
    state->light_off = true;
    record_light_level(status, 0);
    return true;
}

bool device_power_wake(DeviceBackend *backend, DeviceStatus *status,
                       DevicePowerState *state) {
    if (!backend || !backend->write_int || !status || !state) return false;
    if (!state->sleeping && !state->restore_light) return true;

    state->sleeping = false;
    if (!state->restore_light) {
        state->light_off = false;
        state->light_before_sleep_known = false;
        state->light_before_sleep = 0;
        return true;
    }
    const int target = state->light_before_sleep;
    if (!state->light_before_sleep_known || target <= 0 || target > 100) {
        return false;
    }
    if (!light_level_is(backend, target)) {
        if (!backend->write_int(backend->context, "flIntensity", target)) {
            return false;
        }
        if (!light_level_is(backend, target)) return false;
    }
    state->restore_light = false;
    state->light_before_sleep_known = false;
    state->light_before_sleep = 0;
    state->light_off = false;
    record_light_level(status, target);
    return true;
}

static void complete_power_exit(DeviceStatus *status,
                                DevicePowerState *state) {
    state->sleeping = false;
    state->light_off = true;
    state->restore_light = false;
    state->light_before_sleep_known = false;
    state->light_before_sleep = 0;
    record_light_level(status, 0);
}

bool device_power_exit(DeviceBackend *backend, DeviceStatus *status,
                       DevicePowerState *state) {
    if (!backend || !backend->write_int || !status || !state) return false;
    if (light_level_is(backend, 0)) {
        complete_power_exit(status, state);
        return true;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!backend->write_int(backend->context, "flIntensity", 0)) continue;
        if (!light_level_is(backend, 0)) continue;
        complete_power_exit(status, state);
        return true;
    }
    return false;
}

DevicePowerObservedState device_power_reconcile(DeviceBackend *backend,
                                                 DeviceStatus *status,
                                                 DevicePowerState *state) {
    if (!backend || !backend->read_sleeping || !status || !state) {
        return DEVICE_POWER_OBSERVED_UNKNOWN;
    }
    bool sleeping = true;
    if (!backend->read_sleeping(backend->context, &sleeping)) {
        return DEVICE_POWER_OBSERVED_UNKNOWN;
    }
    if (sleeping) {
        (void)device_power_sleep(backend, status, state);
        return DEVICE_POWER_OBSERVED_SLEEPING;
    }
    if (state->sleeping || state->restore_light) {
        (void)device_power_wake(backend, status, state);
    }
    bool sleeping_after_wake = true;
    if (!backend->read_sleeping(backend->context, &sleeping_after_wake)) {
        return DEVICE_POWER_OBSERVED_UNKNOWN;
    }
    if (sleeping_after_wake) {
        (void)device_power_sleep(backend, status, state);
        return DEVICE_POWER_OBSERVED_SLEEPING;
    }
    return DEVICE_POWER_OBSERVED_AWAKE;
}
