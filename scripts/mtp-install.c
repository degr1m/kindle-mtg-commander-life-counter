#include <libmtp.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AMAZON_VENDOR_ID 0x1949
#define APP_NAME "mtg-life-counter"
#define STAGE_NAME ".mtg-life-counter.new"
#define BACKUP_NAME ".mtg-life-counter.old"
#define INSTALLER_MARKER ".mtp-installer-lock"
#define HOST_LOCK_PATH "/tmp/mtg-life-counter-mtp-installer.lock"
#define BACKUP_TEMPLATE "/tmp/mtg-life-counter-backup-XXXXXX"
#define PATH_BUFFER_SIZE 4096

typedef struct {
    uint32_t app;
    uint32_t bin;
    uint32_t launch;
    uint32_t binary;
    uint32_t config;
    uint32_t menu;
    uint32_t ready;
} StageObjects;

typedef struct {
    LIBMTP_folder_t *bin;
    LIBMTP_folder_t *assets;
    LIBMTP_file_t *launch;
    LIBMTP_file_t *binary;
    LIBMTP_file_t *config;
    LIBMTP_file_t *menu;
} OldObjects;

typedef struct {
    char root[PATH_BUFFER_SIZE];
    char launch[PATH_BUFFER_SIZE];
    char binary[PATH_BUFFER_SIZE];
    char config[PATH_BUFFER_SIZE];
    char menu[PATH_BUFFER_SIZE];
    char ready[PATH_BUFFER_SIZE];
    int has_bin;
    int has_launch;
    int has_binary;
    int has_config;
    int has_menu;
    int has_ready;
} CodeBackup;

typedef struct {
    uint32_t final_app;
    int had_existing;
    int final_app_created;
} CutoverState;

typedef struct {
    uint32_t app;
    uint32_t data;
    uint32_t folder;
    uint32_t pid;
    uint32_t marker;
    int data_created;
} InstallerLock;

typedef struct {
    int exists;
    size_t folders;
    size_t files;
    uint64_t xor_hash;
    uint64_t sum_hash;
} DataSnapshot;

static LIBMTP_folder_t *find_unique_root_child(LIBMTP_folder_t *folders,
                                               const char *name) {
    LIBMTP_folder_t *found = NULL;
    for (LIBMTP_folder_t *folder = folders; folder; folder = folder->sibling) {
        if (!folder->name || strcmp(folder->name, name) != 0) continue;
        if (found) return NULL;
        found = folder;
    }
    return found;
}


static int find_unique_direct_folder(LIBMTP_folder_t *parent,
                                     const char *name,
                                     LIBMTP_folder_t **result) {
    *result = NULL;
    if (!parent) return 0;
    for (LIBMTP_folder_t *folder = parent->child; folder;
         folder = folder->sibling) {
        if (!folder->name || strcmp(folder->name, name) != 0) continue;
        if (*result) return 1;
        *result = folder;
    }
    return 0;
}


static int find_unique_direct_file(LIBMTP_file_t *files, uint32_t parent,
                                   const char *name,
                                   LIBMTP_file_t **result) {
    *result = NULL;
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id != parent || !file->filename ||
            strcmp(file->filename, name) != 0) continue;
        if (*result) return 1;
        *result = file;
    }
    return 0;
}

static void destroy_file_list(LIBMTP_file_t *files) {
    while (files) {
        LIBMTP_file_t *next = files->next;
        files->next = NULL;
        LIBMTP_destroy_file_t(files);
        files = next;
    }
}

static LIBMTP_mtpdevice_t *open_single_kindle(void) {
    LIBMTP_raw_device_t *raw = NULL;
    int count = 0;
    LIBMTP_error_number_t result = LIBMTP_Detect_Raw_Devices(&raw, &count);
    if (result != LIBMTP_ERROR_NONE || count <= 0) {
        fprintf(stderr, "No MTP Kindle found. Reconnect and unlock the device.\n");
        free(raw);
        return NULL;
    }

    int selected = -1;
    for (int index = 0; index < count; ++index) {
        if (raw[index].device_entry.vendor_id != AMAZON_VENDOR_ID) continue;
        if (selected >= 0) {
            fprintf(stderr, "Multiple Amazon MTP devices found; connect one Kindle.\n");
            free(raw);
            return NULL;
        }
        selected = index;
    }
    if (selected < 0) {
        fprintf(stderr, "No Amazon Kindle MTP device found.\n");
        free(raw);
        return NULL;
    }

    printf("Using Amazon MTP device VID=%04x PID=%04x.\n",
           raw[selected].device_entry.vendor_id,
           raw[selected].device_entry.product_id);
    LIBMTP_mtpdevice_t *device = LIBMTP_Open_Raw_Device(&raw[selected]);
    free(raw);
    if (device) {
        char *model = LIBMTP_Get_Modelname(device);
        if (model) {
            printf("Connected MTP model: %s.\n", model);
            free(model);
        }
    }
    return device;
}

static int storage_exists(LIBMTP_mtpdevice_t *device, uint32_t storage_id) {
    if (!storage_id || LIBMTP_Get_Storage(
            device, LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0) return 0;
    for (LIBMTP_devicestorage_t *storage = device->storage; storage;
         storage = storage->next) {
        if (storage->id == storage_id) return 1;
    }
    return 0;
}

static int files_equal(const char *left_path, const char *right_path) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    if (!left || !right) {
        if (left) fclose(left);
        if (right) fclose(right);
        return 0;
    }

    unsigned char left_buffer[8192];
    unsigned char right_buffer[8192];
    int equal = 1;
    while (equal) {
        size_t left_size = fread(left_buffer, 1, sizeof(left_buffer), left);
        size_t right_size = fread(right_buffer, 1, sizeof(right_buffer), right);
        if (left_size != right_size ||
            memcmp(left_buffer, right_buffer, left_size) != 0) equal = 0;
        if (left_size < sizeof(left_buffer)) {
            if (ferror(left) || ferror(right)) equal = 0;
            break;
        }
    }
    int left_close = fclose(left);
    int right_close = fclose(right);
    if (left_close != 0 || right_close != 0) equal = 0;
    return equal;
}

static int verify_remote_file(LIBMTP_mtpdevice_t *device, uint32_t item_id,
                              const char *local_path) {
    char downloaded[] = "/tmp/mtg-life-counter-mtp-verify-XXXXXX";
    int descriptor = mkstemp(downloaded);
    if (descriptor < 0) return 1;
    close(descriptor);

    int failed = LIBMTP_Get_File_To_File(
        device, item_id, downloaded, NULL, NULL) != 0;
    if (failed) {
        LIBMTP_Dump_Errorstack(device);
    } else if (!files_equal(local_path, downloaded)) {
        fprintf(stderr, "Readback mismatch: %s\n", local_path);
        failed = 1;
    }
    unlink(downloaded);
    return failed;
}

static int local_regular_file(const char *path, struct stat *status) {
    if (lstat(path, status) != 0) {
        perror(path);
        return 0;
    }
    if (!S_ISREG(status->st_mode)) {
        fprintf(stderr, "Release input is not a regular file: %s\n", path);
        return 0;
    }
    return 1;
}

static uint32_t upload_file(LIBMTP_mtpdevice_t *device, const char *local,
                            const char *name, uint32_t parent,
                            uint32_t storage, uint32_t *retained_item) {
    if (retained_item) *retained_item = 0;
    struct stat status;
    if (!local_regular_file(local, &status)) return 0;
    LIBMTP_file_t *file = LIBMTP_new_file_t();
    if (!file) return 0;
    file->filename = strdup(name);
    if (!file->filename) {
        LIBMTP_destroy_file_t(file);
        return 0;
    }
    file->filesize = (uint64_t)status.st_size;
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;
    file->parent_id = parent;
    file->storage_id = storage;
    int send_failed = LIBMTP_Send_File_From_File(
        device, local, file, NULL, NULL);
    uint32_t item_id = file->item_id;
    if (retained_item) *retained_item = item_id;
    if (send_failed) {
        fprintf(stderr, "Failed to upload file: %s\n", local);
        LIBMTP_Dump_Errorstack(device);
        if (item_id && !LIBMTP_Delete_Object(device, item_id) && retained_item) {
            *retained_item = 0;
        }
        LIBMTP_destroy_file_t(file);
        return 0;
    }
    if (verify_remote_file(device, item_id, local)) {
        fprintf(stderr, "Failed readback verification: %s\n", local);
        if (LIBMTP_Delete_Object(device, item_id)) {
            LIBMTP_Dump_Errorstack(device);
        } else if (retained_item) {
            *retained_item = 0;
        }
        LIBMTP_destroy_file_t(file);
        return 0;
    }
    printf("Uploaded and verified %s (%lld bytes, id %u)\n", name,
           (long long)status.st_size, item_id);
    LIBMTP_destroy_file_t(file);
    return item_id;
}

static int local_path(char *buffer, size_t size, const char *root,
                      const char *relative) {
    int length = snprintf(buffer, size, "%s/%s", root, relative);
    return length >= 0 && (size_t)length < size;
}

static int download_remote_file(LIBMTP_mtpdevice_t *device,
                                LIBMTP_file_t *file,
                                const char *destination) {
    if (!file || LIBMTP_Get_File_To_File(
            device, file->item_id, destination, NULL, NULL)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
    }
    struct stat status;
    if (lstat(destination, &status) != 0 || !S_ISREG(status.st_mode) ||
        (uint64_t)status.st_size != file->filesize) {
        fprintf(stderr, "Invalid MTP backup readback: %s\n", destination);
        return 1;
    }
    return 0;
}

static size_t count_direct_files(LIBMTP_file_t *files, uint32_t parent) {
    size_t count = 0;
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id == parent) ++count;
    }
    return count;
}

static void cleanup_code_backup(CodeBackup *backup) {
    if (backup->has_launch) (void)unlink(backup->launch);
    if (backup->has_binary) (void)unlink(backup->binary);
    if (backup->has_config) (void)unlink(backup->config);
    if (backup->has_menu) (void)unlink(backup->menu);
    if (backup->has_ready) (void)unlink(backup->ready);
    if (backup->root[0]) (void)rmdir(backup->root);
    memset(backup, 0, sizeof(*backup));
}

static int backup_old_code(LIBMTP_mtpdevice_t *device,
                           LIBMTP_file_t *files,
                           const OldObjects *old,
                           CodeBackup *backup) {
    memset(backup, 0, sizeof(*backup));
    if (snprintf(backup->root, sizeof(backup->root), "%s", BACKUP_TEMPLATE) < 0 ||
        !mkdtemp(backup->root)) {
        perror("mkdtemp");
        return 1;
    }

    backup->has_bin = old->bin != NULL;
    if (old->bin &&
        (old->bin->child || !old->launch || !old->binary ||
         count_direct_files(files, old->bin->folder_id) != 2)) {
        fprintf(stderr, "Existing MTP bin manifest is not safe to replace.\n");
        cleanup_code_backup(backup);
        return 1;
    }

    struct BackupFile {
        LIBMTP_file_t *remote;
        const char *name;
        char *path;
        size_t path_size;
        int *present;
    } backup_files[] = {
        {old->launch, "launch.sh", backup->launch,
         sizeof(backup->launch), &backup->has_launch},
        {old->binary, "mtg-life-counter", backup->binary,
         sizeof(backup->binary), &backup->has_binary},
        {old->config, "config.xml", backup->config,
         sizeof(backup->config), &backup->has_config},
        {old->menu, "menu.json", backup->menu,
         sizeof(backup->menu), &backup->has_menu},
    };
    for (size_t index = 0;
         index < sizeof(backup_files) / sizeof(backup_files[0]); ++index) {
        if (!backup_files[index].remote) continue;
        *backup_files[index].present = 1;
        if (!local_path(backup_files[index].path,
                        backup_files[index].path_size,
                        backup->root, backup_files[index].name) ||
            download_remote_file(device, backup_files[index].remote,
                                 backup_files[index].path) ||
            verify_remote_file(device, backup_files[index].remote->item_id,
                               backup_files[index].path)) {
            cleanup_code_backup(backup);
            return 1;
        }
    }
    if (!local_path(backup->ready, sizeof(backup->ready),
                    backup->root, "READY")) {
        cleanup_code_backup(backup);
        return 1;
    }
    int ready_file = open(backup->ready, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (ready_file >= 0) backup->has_ready = 1;
    static const char ready_content[] = "READY\n";
    int ready_failed = ready_file < 0;
    if (!ready_failed &&
        write(ready_file, ready_content, sizeof(ready_content) - 1) !=
            (ssize_t)(sizeof(ready_content) - 1)) ready_failed = 1;
    if (!ready_failed && fsync(ready_file) != 0) ready_failed = 1;
    if (ready_file >= 0 && close(ready_file) != 0) ready_failed = 1;
    if (ready_failed) {
        cleanup_code_backup(backup);
        return 1;
    }
    return 0;
}

static int create_stage(LIBMTP_mtpdevice_t *device, const char *local,
                        uint32_t extensions, uint32_t storage,
                        StageObjects *stage) {
    memset(stage, 0, sizeof(*stage));
    char stage_name[] = STAGE_NAME;
    stage->app = LIBMTP_Create_Folder(
        device, stage_name, extensions, storage);
    if (!stage->app) return 1;
    char bin_name[] = "bin";
    stage->bin = LIBMTP_Create_Folder(device, bin_name, stage->app, storage);
    if (!stage->bin) return 1;

    char path[4096];
    if (!local_path(path, sizeof(path), local, "bin/launch.sh")) return 1;
    if (!upload_file(device, path, "launch.sh", stage->bin, storage,
                     &stage->launch)) return 1;
    if (!local_path(path, sizeof(path), local, "bin/mtg-life-counter")) return 1;
    if (!upload_file(device, path, "mtg-life-counter", stage->bin, storage,
                     &stage->binary)) return 1;
    if (!local_path(path, sizeof(path), local, "config.xml")) return 1;
    if (!upload_file(device, path, "config.xml", stage->app, storage,
                     &stage->config)) return 1;
    if (!local_path(path, sizeof(path), local, "menu.json")) return 1;
    return upload_file(device, path, "menu.json", stage->app, storage,
                       &stage->menu) ? 0 : 1;
}

static int create_remote_backup(LIBMTP_mtpdevice_t *device,
                                const CodeBackup *local,
                                uint32_t extensions, uint32_t storage,
                                StageObjects *remote) {
    memset(remote, 0, sizeof(*remote));
    char backup_name[] = BACKUP_NAME;
    remote->app = LIBMTP_Create_Folder(
        device, backup_name, extensions, storage);
    if (!remote->app) return 1;
    if (local->has_bin) {
        char bin_name[] = "bin";
        remote->bin = LIBMTP_Create_Folder(
            device, bin_name, remote->app, storage);
        if (!remote->bin) return 1;
    }
    if (local->has_launch &&
        (!remote->bin || !upload_file(device, local->launch, "launch.sh",
                                      remote->bin, storage,
                                      &remote->launch))) return 1;
    if (local->has_binary &&
        (!remote->bin || !upload_file(device, local->binary,
                                      "mtg-life-counter", remote->bin,
                                      storage, &remote->binary))) return 1;
    if (local->has_config &&
        !upload_file(device, local->config, "config.xml", remote->app,
                     storage, &remote->config)) return 1;
    if (local->has_menu &&
        !upload_file(device, local->menu, "menu.json", remote->app,
                     storage, &remote->menu)) return 1;
    return upload_file(device, local->ready, "READY", remote->app,
                       storage, &remote->ready) ? 0 : 1;
}

static int delete_stage_objects(LIBMTP_mtpdevice_t *device,
                                const StageObjects *stage) {
    if (stage->ready && LIBMTP_Delete_Object(device, stage->ready)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
    }
    const uint32_t objects[] = {
        stage->launch, stage->binary, stage->bin, stage->config,
        stage->menu, stage->app,
    };
    int failed = 0;
    for (size_t index = 0; index < sizeof(objects) / sizeof(objects[0]); ++index) {
        if (objects[index] && LIBMTP_Delete_Object(device, objects[index])) {
            failed = 1;
        }
    }
    if (failed) LIBMTP_Dump_Errorstack(device);
    return failed;
}

static int delete_remote_files(LIBMTP_mtpdevice_t *device, uint32_t parent,
                               LIBMTP_file_t *files) {
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id != parent) continue;
        if (LIBMTP_Delete_Object(device, file->item_id)) {
            LIBMTP_Dump_Errorstack(device);
            return 1;
        }
    }
    return 0;
}

static int delete_remote_tree(LIBMTP_mtpdevice_t *device,
                              LIBMTP_folder_t *folder,
                              LIBMTP_file_t *files) {
    if (!folder) return 0;
    for (LIBMTP_folder_t *child = folder->child; child; child = child->sibling) {
        if (delete_remote_tree(device, child, files)) return 1;
    }
    if (delete_remote_files(device, folder->folder_id, files)) return 1;
    if (LIBMTP_Delete_Object(device, folder->folder_id)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
    }
    return 0;
}

static int delete_named_files(LIBMTP_mtpdevice_t *device,
                              LIBMTP_file_t *files, uint32_t parent,
                              const char *name) {
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id != parent || !file->filename ||
            strcmp(file->filename, name) != 0) continue;
        if (LIBMTP_Delete_Object(device, file->item_id)) {
            LIBMTP_Dump_Errorstack(device);
            return 1;
        }
    }
    return 0;
}

static int copy_remote_file(LIBMTP_mtpdevice_t *device, uint32_t source,
                            const char *name, uint32_t parent,
                            uint32_t storage, uint32_t *retained_item) {
    if (!source) return 1;
    char downloaded[] = "/tmp/mtg-life-counter-recovery-XXXXXX";
    int descriptor = mkstemp(downloaded);
    if (descriptor < 0) return 1;
    close(descriptor);
    if (LIBMTP_Get_File_To_File(
            device, source, downloaded, NULL, NULL)) {
        LIBMTP_Dump_Errorstack(device);
        unlink(downloaded);
        return 1;
    }
    int failed = !upload_file(device, downloaded, name, parent,
                              storage, retained_item);
    if (unlink(downloaded) != 0) failed = 1;
    return failed;
}

static int load_remote_objects(LIBMTP_folder_t *folder,
                               LIBMTP_file_t *files,
                               StageObjects *objects) {
    memset(objects, 0, sizeof(*objects));
    if (!folder) return 0;
    objects->app = folder->folder_id;
    LIBMTP_folder_t *bin = NULL;
    if (find_unique_direct_folder(folder, "bin", &bin)) return 1;
    if (bin) {
        objects->bin = bin->folder_id;
        LIBMTP_file_t *launch = NULL;
        LIBMTP_file_t *binary = NULL;
        if (find_unique_direct_file(files, bin->folder_id,
                                    "launch.sh", &launch) ||
            find_unique_direct_file(files, bin->folder_id,
                                    "mtg-life-counter", &binary)) return 1;
        if (launch) objects->launch = launch->item_id;
        if (binary) objects->binary = binary->item_id;
    }
    LIBMTP_file_t *config = NULL;
    LIBMTP_file_t *menu = NULL;
    LIBMTP_file_t *ready = NULL;
    if (find_unique_direct_file(files, folder->folder_id,
                                "config.xml", &config) ||
        find_unique_direct_file(files, folder->folder_id,
                                "menu.json", &menu) ||
        find_unique_direct_file(files, folder->folder_id,
                                "READY", &ready)) return 1;
    if (config) objects->config = config->item_id;
    if (menu) objects->menu = menu->item_id;
    if (ready) objects->ready = ready->item_id;
    return 0;
}

static LIBMTP_folder_t *find_folder_by_id(LIBMTP_folder_t *folder,
                                          uint32_t folder_id) {
    for (LIBMTP_folder_t *current = folder; current; current = current->sibling) {
        if (current->folder_id == folder_id) return current;
        LIBMTP_folder_t *found = find_folder_by_id(current->child, folder_id);
        if (found) return found;
    }
    return NULL;
}

static int write_temporary_text(char *path, const char *text) {
    int descriptor = mkstemp(path);
    if (descriptor < 0) return 1;
    size_t remaining = strlen(text);
    const char *cursor = text;
    int failed = 0;
    while (remaining) {
        ssize_t written = write(descriptor, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            failed = 1;
            break;
        }
    }
    if (!failed && fsync(descriptor) != 0) failed = 1;
    if (close(descriptor) != 0) failed = 1;
    return failed;
}

static int read_remote_uint32(LIBMTP_mtpdevice_t *device, uint32_t item_id,
                              uint32_t *value) {
    char path[] = "/tmp/mtg-life-counter-lock-id-XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0) return 1;
    close(descriptor);
    if (LIBMTP_Get_File_To_File(device, item_id, path, NULL, NULL)) {
        unlink(path);
        return 1;
    }
    FILE *stream = fopen(path, "r");
    unsigned long long parsed = 0;
    char extra = '\0';
    int failed = !stream || fscanf(stream, "%llu%c", &parsed, &extra) != 2 ||
        extra != '\n' || parsed == 0 || parsed > UINT32_MAX;
    if (stream && fclose(stream) != 0) failed = 1;
    if (unlink(path) != 0) failed = 1;
    if (!failed) *value = (uint32_t)parsed;
    return failed;
}

static int verify_installer_lock(LIBMTP_mtpdevice_t *device,
                                 const InstallerLock *lock) {
    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_file_t *files = LIBMTP_Get_Filelisting(device);
    LIBMTP_folder_t *app = find_folder_by_id(folders, lock->app);
    LIBMTP_folder_t *data = NULL;
    LIBMTP_folder_t *folder = NULL;
    LIBMTP_file_t *pid = NULL;
    LIBMTP_file_t *marker = NULL;
    uint32_t marked_folder = 0;
    int failed = !folders || !app ||
        find_unique_direct_folder(app, "data", &data) || !data ||
        data->folder_id != lock->data ||
        find_unique_direct_folder(data, "app.lock", &folder) || !folder ||
        folder->folder_id != lock->folder ||
        find_unique_direct_file(files, folder->folder_id, "pid", &pid) ||
        !pid || pid->item_id != lock->pid ||
        find_unique_direct_file(files, data->folder_id,
                                INSTALLER_MARKER, &marker) ||
        !marker || marker->item_id != lock->marker;
    if (!failed && (read_remote_uint32(device, marker->item_id,
                                       &marked_folder) ||
                    marked_folder != lock->folder)) failed = 1;
    destroy_file_list(files);
    if (folders) LIBMTP_destroy_folder_t(folders);
    return failed;
}

static int acquire_installer_lock(LIBMTP_mtpdevice_t *device,
                                  uint32_t app_id, uint32_t storage,
                                  InstallerLock *lock) {
    memset(lock, 0, sizeof(*lock));
    lock->app = app_id;
    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_folder_t *app = find_folder_by_id(folders, app_id);
    LIBMTP_folder_t *data = NULL;
    LIBMTP_folder_t *existing_lock = NULL;
    int failed = !folders || !app ||
        find_unique_direct_folder(app, "data", &data) ||
        (data && (find_unique_direct_folder(
            data, "app.lock", &existing_lock) || existing_lock));
    if (!failed && !data) {
        char data_name[] = "data";
        lock->data = LIBMTP_Create_Folder(
            device, data_name, app_id, storage);
        lock->data_created = 1;
        failed = !lock->data;
    } else if (data) {
        lock->data = data->folder_id;
    }
    if (folders) LIBMTP_destroy_folder_t(folders);
    if (failed) return 1;

    char lock_name[] = "app.lock";
    lock->folder = LIBMTP_Create_Folder(
        device, lock_name, lock->data, storage);
    failed = !lock->folder;
    char pid_path[] = "/tmp/mtg-life-counter-install-pid-XXXXXX";
    char marker_path[] = "/tmp/mtg-life-counter-install-marker-XXXXXX";
    char marker_text[32];
    snprintf(marker_text, sizeof(marker_text), "%u\n", lock->folder);
    if (!failed && (write_temporary_text(pid_path, "1\n") ||
                    write_temporary_text(marker_path, marker_text))) failed = 1;
    if (!failed && !upload_file(device, pid_path, "pid", lock->folder,
                                storage, &lock->pid)) failed = 1;
    if (!failed && !upload_file(device, marker_path, INSTALLER_MARKER,
                                lock->data, storage, &lock->marker)) failed = 1;
    if (unlink(pid_path) != 0 || unlink(marker_path) != 0) failed = 1;
    return failed || verify_installer_lock(device, lock);
}

static int release_installer_lock(LIBMTP_mtpdevice_t *device,
                                  InstallerLock *lock) {
    if (!lock->folder && !lock->marker) return 0;
    if (lock->pid) (void)LIBMTP_Delete_Object(device, lock->pid);
    if (lock->folder) (void)LIBMTP_Delete_Object(device, lock->folder);

    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_folder_t *owned = find_folder_by_id(folders, lock->folder);
    int failed = !folders || owned != NULL;
    if (folders) LIBMTP_destroy_folder_t(folders);
    if (!failed && lock->marker &&
        LIBMTP_Delete_Object(device, lock->marker)) failed = 1;
    if (!failed) memset(lock, 0, sizeof(*lock));
    return failed;
}

static int reclaim_stale_installer_lock(LIBMTP_mtpdevice_t *device,
                                        uint32_t app_id) {
    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_file_t *files = LIBMTP_Get_Filelisting(device);
    LIBMTP_folder_t *app = find_folder_by_id(folders, app_id);
    LIBMTP_folder_t *data = NULL;
    LIBMTP_folder_t *lock = NULL;
    LIBMTP_file_t *pid = NULL;
    LIBMTP_file_t *marker = NULL;
    uint32_t marked_folder = 0;
    int failed = !folders || !app ||
        find_unique_direct_folder(app, "data", &data) ||
        (data && find_unique_direct_folder(data, "app.lock", &lock));
    if (!failed && data &&
        find_unique_direct_file(files, data->folder_id,
                                INSTALLER_MARKER, &marker)) failed = 1;
    if (!failed && marker && read_remote_uint32(
            device, marker->item_id, &marked_folder)) failed = 1;
    if (!failed && lock && !marker) {
        uint32_t installer_pid = 0;
        if (find_unique_direct_file(files, lock->folder_id, "pid", &pid) ||
            !pid || read_remote_uint32(device, pid->item_id, &installer_pid) ||
            installer_pid != 1) {
            failed = 1;
        } else {
            marked_folder = lock->folder_id;
        }
    }
    if (!failed && lock && marked_folder != lock->folder_id) failed = 1;
    if (!failed && lock) {
        (void)delete_remote_tree(device, lock, files);
    }
    uint32_t lock_id = lock ? lock->folder_id : 0;
    uint32_t marker_id = marker ? marker->item_id : 0;
    destroy_file_list(files);
    if (folders) LIBMTP_destroy_folder_t(folders);
    if (failed || (!lock_id && !marker_id)) return failed;

    folders = LIBMTP_Get_Folder_List(device);
    int remains = lock_id && find_folder_by_id(folders, lock_id) != NULL;
    if (folders) LIBMTP_destroy_folder_t(folders);
    if (remains) return 1;
    if (marker_id && LIBMTP_Delete_Object(device, marker_id)) return 1;
    return 0;
}

static int purge_live_code(LIBMTP_mtpdevice_t *device, uint32_t app_id) {
    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_file_t *files = LIBMTP_Get_Filelisting(device);
    LIBMTP_folder_t *app = find_folder_by_id(folders, app_id);
    if (!folders || !app) {
        destroy_file_list(files);
        if (folders) LIBMTP_destroy_folder_t(folders);
        return 1;
    }

    for (LIBMTP_folder_t *child = app->child; child; child = child->sibling) {
        if (child->name && strcmp(child->name, "bin") == 0) {
            (void)delete_remote_tree(device, child, files);
        }
    }
    (void)delete_named_files(device, files, app_id, "config.xml");
    (void)delete_named_files(device, files, app_id, "menu.json");
    destroy_file_list(files);
    LIBMTP_destroy_folder_t(folders);

    folders = LIBMTP_Get_Folder_List(device);
    files = LIBMTP_Get_Filelisting(device);
    app = find_folder_by_id(folders, app_id);
    int failed = !folders || !app;
    if (!failed) {
        for (LIBMTP_folder_t *child = app->child; child; child = child->sibling) {
            if (child->name && strcmp(child->name, "bin") == 0) failed = 1;
        }
        for (LIBMTP_file_t *file = files; file; file = file->next) {
            if (file->parent_id == app_id && file->filename &&
                (strcmp(file->filename, "config.xml") == 0 ||
                 strcmp(file->filename, "menu.json") == 0)) failed = 1;
        }
    }
    destroy_file_list(files);
    if (folders) LIBMTP_destroy_folder_t(folders);
    return failed;
}

static int restore_remote_journal(LIBMTP_mtpdevice_t *device,
                                  uint32_t storage, uint32_t app_id,
                                  const StageObjects *remote) {
    if (!remote->app || !remote->ready || !app_id ||
        purge_live_code(device, app_id)) return 1;

    StageObjects created = {0};
    if (remote->bin) {
        char bin_name[] = "bin";
        created.bin = LIBMTP_Create_Folder(device, bin_name, app_id, storage);
        if (!created.bin ||
            (remote->launch && copy_remote_file(
                device, remote->launch, "launch.sh", created.bin,
                storage, &created.launch)) ||
            (remote->binary && copy_remote_file(
                device, remote->binary, "mtg-life-counter", created.bin,
                storage, &created.binary))) goto restore_failed;
    }
    if ((remote->config && copy_remote_file(
            device, remote->config, "config.xml", app_id,
            storage, &created.config)) ||
        (remote->menu && copy_remote_file(
            device, remote->menu, "menu.json", app_id,
            storage, &created.menu))) goto restore_failed;
    return delete_stage_objects(device, remote);

restore_failed:
    (void)delete_stage_objects(device, &created);
    return 1;
}

static int recover_stale_transaction(LIBMTP_mtpdevice_t *device,
                                     LIBMTP_folder_t *extensions,
                                     LIBMTP_folder_t *existing,
                                     LIBMTP_folder_t *stage_folder,
                                     LIBMTP_folder_t *backup_folder,
                                     LIBMTP_file_t *files,
                                     const InstallerLock *installer_lock) {
    if (!stage_folder && !backup_folder) return 0;
    if (existing && (!installer_lock->folder ||
                     verify_installer_lock(device, installer_lock))) {
        fprintf(stderr, "Life counter is running; cannot recover installation.\n");
        return 1;
    }

    StageObjects remote = {0};
    if (backup_folder && load_remote_objects(backup_folder, files, &remote)) {
        fprintf(stderr, "Ambiguous remote recovery journal.\n");
        return 1;
    }
    if (backup_folder && remote.ready) {
        uint32_t app_id = existing ? existing->folder_id : 0;
        if (!app_id) {
            char app_name[] = APP_NAME;
            app_id = LIBMTP_Create_Folder(
                device, app_name, extensions->folder_id,
                extensions->storage_id);
            if (!app_id) return 1;
        }
        if (restore_remote_journal(device, extensions->storage_id,
                                   app_id, &remote)) return 1;
    } else if (backup_folder &&
               delete_remote_tree(device, backup_folder, files)) {
        return 1;
    }

    if (stage_folder && delete_remote_tree(device, stage_folder, files)) return 1;
    puts("Recovered a stale MTP installation transaction.");
    return 0;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int hash_remote_file(LIBMTP_mtpdevice_t *device, uint32_t item_id,
                            uint64_t *content_hash) {
    char downloaded[] = "/tmp/mtg-life-counter-data-hash-XXXXXX";
    int descriptor = mkstemp(downloaded);
    if (descriptor < 0) return 1;
    close(descriptor);
    if (LIBMTP_Get_File_To_File(device, item_id, downloaded, NULL, NULL)) {
        LIBMTP_Dump_Errorstack(device);
        unlink(downloaded);
        return 1;
    }

    int stream = open(downloaded, O_RDONLY);
    if (stream < 0) {
        unlink(downloaded);
        return 1;
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char buffer[8192];
    int failed = 0;
    for (;;) {
        ssize_t count = read(stream, buffer, sizeof(buffer));
        if (count > 0) {
            hash = hash_bytes(hash, buffer, (size_t)count);
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        failed = 1;
        break;
    }
    if (close(stream) != 0) failed = 1;
    if (unlink(downloaded) != 0) failed = 1;
    if (!failed) *content_hash = hash;
    return failed;
}

static uint64_t object_hash(const char *name, uint32_t item, uint32_t parent,
                            uint64_t size, unsigned char kind,
                            uint64_t content_hash) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, &kind, sizeof(kind));
    hash = hash_bytes(hash, &item, sizeof(item));
    hash = hash_bytes(hash, &parent, sizeof(parent));
    hash = hash_bytes(hash, &size, sizeof(size));
    hash = hash_bytes(hash, &content_hash, sizeof(content_hash));
    return hash_bytes(hash, name, strlen(name) + 1);
}

static void snapshot_add(DataSnapshot *snapshot, uint64_t hash, int is_folder) {
    snapshot->xor_hash ^= hash;
    snapshot->sum_hash += hash;
    if (is_folder) ++snapshot->folders;
    else ++snapshot->files;
}

static int snapshot_data_tree(LIBMTP_mtpdevice_t *device,
                              LIBMTP_folder_t *folder,
                              LIBMTP_file_t *files,
                              DataSnapshot *snapshot) {
    if (!folder) return 0;
    snapshot_add(snapshot,
                 object_hash(folder->name ? folder->name : "",
                             folder->folder_id, folder->parent_id,
                             0, 'D', 0), 1);
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id != folder->folder_id) continue;
        uint64_t content_hash = 0;
        if (hash_remote_file(device, file->item_id, &content_hash)) return 1;
        snapshot_add(snapshot,
                     object_hash(file->filename ? file->filename : "",
                                 file->item_id, file->parent_id,
                                 file->filesize, 'F', content_hash), 0);
    }
    for (LIBMTP_folder_t *child = folder->child; child; child = child->sibling) {
        if (snapshot_data_tree(device, child, files, snapshot)) return 1;
    }
    return 0;
}

static int data_snapshot(LIBMTP_mtpdevice_t *device, LIBMTP_folder_t *app,
                         LIBMTP_file_t *files, DataSnapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    LIBMTP_folder_t *data = NULL;
    if (find_unique_direct_folder(app, "data", &data)) return 1;
    if (!data) return 0;
    snapshot->exists = 1;
    return snapshot_data_tree(device, data, files, snapshot);
}

static int data_snapshot_equal(const DataSnapshot *left,
                               const DataSnapshot *right) {
    return left->exists == right->exists &&
           left->folders == right->folders && left->files == right->files &&
           left->xor_hash == right->xor_hash &&
           left->sum_hash == right->sum_hash;
}

static int delete_old_object(LIBMTP_mtpdevice_t *device,
                             LIBMTP_file_t *file) {
    if (!file) return 0;
    if (LIBMTP_Delete_Object(device, file->item_id)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
    }
    return 0;
}

static int delete_old_code(LIBMTP_mtpdevice_t *device,
                           const OldObjects *old) {
    if (delete_old_object(device, old->launch) ||
        delete_old_object(device, old->binary)) {
        return 1;
    }
    if (old->bin) {
        if (LIBMTP_Delete_Object(device, old->bin->folder_id)) {
            LIBMTP_Dump_Errorstack(device);
            return 1;
        }
    }
    if (delete_old_object(device, old->config) ||
        delete_old_object(device, old->menu)) {
        return 1;
    }
    return 0;
}

static int promote_stage(LIBMTP_mtpdevice_t *device, uint32_t extensions,
                         uint32_t storage, LIBMTP_folder_t *existing,
                         const StageObjects *stage, const OldObjects *old,
                         CutoverState *state) {
    memset(state, 0, sizeof(*state));
    if (existing) {
        state->had_existing = 1;
        state->final_app = existing->folder_id;
        if (delete_old_code(device, old)) return 1;
    } else {
        char app_name[] = APP_NAME;
        state->final_app = LIBMTP_Create_Folder(
            device, app_name, extensions, storage);
        if (!state->final_app) return 1;
        state->final_app_created = 1;
    }
    if (LIBMTP_Move_Object(device, stage->bin, storage, state->final_app) ||
        LIBMTP_Move_Object(device, stage->config, storage, state->final_app) ||
        LIBMTP_Move_Object(device, stage->menu, storage, state->final_app)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
    }
    return 0;
}

static int find_unique_child_object(LIBMTP_file_t *objects, const char *name,
                                    int want_folder, uint32_t *item) {
    *item = 0;
    for (LIBMTP_file_t *object = objects; object; object = object->next) {
        int is_folder = object->filetype == LIBMTP_FILETYPE_FOLDER;
        if (is_folder != want_folder || !object->filename ||
            strcmp(object->filename, name) != 0) continue;
        if (*item) return 1;
        *item = object->item_id;
    }
    return 0;
}

static LIBMTP_file_t *get_fresh_children(LIBMTP_mtpdevice_t *device,
                                         uint32_t storage, uint32_t parent) {
    int cached = device->cached;
    device->cached = 0;
    LIBMTP_file_t *children =
        LIBMTP_Get_Files_And_Folders(device, storage, parent);
    device->cached = cached;
    return children;
}

static int verify_promoted_code(LIBMTP_mtpdevice_t *device,
                                const char *local,
                                LIBMTP_folder_t *app,
                                const StageObjects *stage) {
    LIBMTP_file_t *app_children = app ? get_fresh_children(
        device, app->storage_id, app->folder_id) : NULL;
    uint32_t bin = 0;
    uint32_t config = 0;
    uint32_t menu = 0;
    int failed = !app || !app_children ||
        find_unique_child_object(app_children, "bin", 1, &bin) ||
        find_unique_child_object(app_children, "config.xml", 0, &config) ||
        find_unique_child_object(app_children, "menu.json", 0, &menu);
    LIBMTP_file_t *bin_children = !failed && bin ? get_fresh_children(
        device, app->storage_id, bin) : NULL;
    uint32_t launch = 0;
    uint32_t binary = 0;
    failed = failed || !bin_children ||
        find_unique_child_object(bin_children, "launch.sh", 0, &launch) ||
        find_unique_child_object(bin_children, "mtg-life-counter", 0, &binary);
    if (failed || bin != stage->bin || launch != stage->launch ||
        binary != stage->binary || config != stage->config ||
        menu != stage->menu) {
        fprintf(stderr,
                "Promoted MTP manifest mismatch: app=%u bin=%u/%u "
                "launch=%u/%u binary=%u/%u config=%u/%u menu=%u/%u.\n",
                app ? app->folder_id : 0,
                bin, stage->bin, launch, stage->launch,
                binary, stage->binary, config, stage->config,
                menu, stage->menu);
        destroy_file_list(bin_children);
        destroy_file_list(app_children);
        return 1;
    }
    destroy_file_list(bin_children);
    destroy_file_list(app_children);

    struct PromotedFile {
        uint32_t item;
        const char *relative;
    } promoted[] = {
        {stage->launch, "bin/launch.sh"},
        {stage->binary, "bin/mtg-life-counter"},
        {stage->config, "config.xml"},
        {stage->menu, "menu.json"},
    };
    char path[PATH_BUFFER_SIZE];
    for (size_t index = 0;
         index < sizeof(promoted) / sizeof(promoted[0]); ++index) {
        if (!local_path(path, sizeof(path), local, promoted[index].relative) ||
            verify_remote_file(device, promoted[index].item, path)) {
            return 1;
        }
    }
    return 0;
}

static int rollback_cutover(LIBMTP_mtpdevice_t *device, uint32_t storage,
                            const StageObjects *stage,
                            const StageObjects *remote_backup,
                            const CutoverState *state) {
    int failed = delete_stage_objects(device, stage);
    if (failed) {
        fprintf(stderr, "New MTP objects could not be fully removed.\n");
    } else if (state->final_app_created) {
        if (state->final_app &&
            LIBMTP_Delete_Object(device, state->final_app)) failed = 1;
    } else if (state->had_existing && restore_remote_journal(
                   device, storage, state->final_app, remote_backup)) {
        failed = 1;
    }
    if (failed) {
        fprintf(stderr, "MTP rollback was incomplete; reconnect before retrying.\n");
        LIBMTP_Dump_Errorstack(device);
    }
    return failed;
}

static int delete_obsolete_assets(LIBMTP_mtpdevice_t *device,
                                  const OldObjects *old,
                                  LIBMTP_file_t *files) {
    return old->assets ? delete_remote_tree(device, old->assets, files) : 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s LOCAL_APP_DIR\n", argv[0]);
        return 2;
    }

    int host_lock = open(HOST_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    struct flock lease;
    memset(&lease, 0, sizeof(lease));
    lease.l_type = F_WRLCK;
    lease.l_whence = SEEK_SET;
    if (host_lock < 0 || fcntl(host_lock, F_SETLK, &lease) != 0) {
        fprintf(stderr, "Another MTP installer is already active.\n");
        if (host_lock >= 0) close(host_lock);
        return 3;
    }

    LIBMTP_Init();
    LIBMTP_mtpdevice_t *device = open_single_kindle();
    if (!device) {
        close(host_lock);
        return 3;
    }

    int result = 1;
    int rollback_incomplete = 0;
    int keep_local_backup = 0;
    int retain_installer_lock = 0;
    StageObjects stage = {0};
    StageObjects remote_backup = {0};
    OldObjects old = {0};
    CodeBackup backup = {0};
    CutoverState cutover = {0};
    InstallerLock installer_lock = {0};
    DataSnapshot before = {0};
    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_file_t *files = NULL;
    LIBMTP_folder_t *extensions = find_unique_root_child(folders, "extensions");
    if (!extensions || !storage_exists(device, extensions->storage_id)) {
        fprintf(stderr, "Could not identify one root Kindle extensions folder.\n");
        goto cleanup;
    }

    LIBMTP_folder_t *existing = NULL;
    LIBMTP_folder_t *staged_folder = NULL;
    LIBMTP_folder_t *backup_folder = NULL;
    if (find_unique_direct_folder(extensions, APP_NAME, &existing) ||
        find_unique_direct_folder(extensions, STAGE_NAME, &staged_folder) ||
        find_unique_direct_folder(extensions, BACKUP_NAME, &backup_folder)) {
        fprintf(stderr, "Ambiguous MTP installation folders.\n");
        goto cleanup;
    }
    if (existing &&
        (reclaim_stale_installer_lock(device, existing->folder_id) ||
         acquire_installer_lock(device, existing->folder_id,
                                extensions->storage_id, &installer_lock))) {
        fprintf(stderr, "Life counter is running or installer lock failed.\n");
        goto cleanup;
    }
    if (staged_folder || backup_folder) {
        files = LIBMTP_Get_Filelisting(device);
        if (!files || recover_stale_transaction(
                device, extensions, existing, staged_folder,
                backup_folder, files, &installer_lock)) {
            fprintf(stderr, "Could not recover stale MTP transaction.\n");
            retain_installer_lock = installer_lock.folder != 0;
            goto cleanup;
        }
        destroy_file_list(files);
        files = NULL;
        LIBMTP_destroy_folder_t(folders);
        folders = LIBMTP_Get_Folder_List(device);
        extensions = find_unique_root_child(folders, "extensions");
        existing = NULL;
        staged_folder = NULL;
        backup_folder = NULL;
        if (!extensions ||
            find_unique_direct_folder(extensions, APP_NAME, &existing) ||
            find_unique_direct_folder(extensions, STAGE_NAME, &staged_folder) ||
            find_unique_direct_folder(extensions, BACKUP_NAME, &backup_folder) ||
            staged_folder || backup_folder) {
            fprintf(stderr, "Stale MTP recovery did not converge.\n");
            goto cleanup;
        }
        if (existing && !installer_lock.folder && acquire_installer_lock(
                device, existing->folder_id, extensions->storage_id,
                &installer_lock)) {
            fprintf(stderr, "Could not lock recovered MTP installation.\n");
            goto cleanup;
        }
    }
    if (existing && verify_installer_lock(device, &installer_lock)) {
        fprintf(stderr, "Life counter is running; exit it before installing.\n");
        goto cleanup;
    }
    if (create_stage(device, argv[1], extensions->folder_id,
                     extensions->storage_id, &stage)) {
        fprintf(stderr, "Could not stage and verify the new application.\n");
        (void)delete_stage_objects(device, &stage);
        goto cleanup;
    }

    LIBMTP_destroy_folder_t(folders);
    folders = LIBMTP_Get_Folder_List(device);
    extensions = find_unique_root_child(folders, "extensions");
    staged_folder = NULL;
    existing = NULL;
    if (!extensions ||
        find_unique_direct_folder(extensions, STAGE_NAME, &staged_folder) ||
        !staged_folder || staged_folder->folder_id != stage.app ||
        find_unique_direct_folder(extensions, APP_NAME, &existing) ||
        (existing && verify_installer_lock(device, &installer_lock))) {
        fprintf(stderr, "Could not safely confirm staged application.\n");
        (void)delete_stage_objects(device, &stage);
        goto cleanup;
    }

    if (existing) {
        files = LIBMTP_Get_Filelisting(device);
        if (!files || data_snapshot(device, existing, files, &before)) {
            fprintf(stderr, "Could not hash preserved Kindle game data.\n");
            (void)delete_stage_objects(device, &stage);
            goto cleanup;
        }
        if (find_unique_direct_folder(existing, "bin", &old.bin) ||
            find_unique_direct_folder(existing, "assets", &old.assets) ||
            find_unique_direct_file(files, existing->folder_id,
                                    "config.xml", &old.config) ||
            find_unique_direct_file(files, existing->folder_id,
                                    "menu.json", &old.menu)) {
            fprintf(stderr, "Existing MTP code manifest is ambiguous.\n");
            (void)delete_stage_objects(device, &stage);
            goto cleanup;
        }
        if (old.bin &&
            (find_unique_direct_file(files, old.bin->folder_id,
                                     "launch.sh", &old.launch) ||
             find_unique_direct_file(files, old.bin->folder_id,
                                     "mtg-life-counter", &old.binary))) {
            fprintf(stderr, "Existing MTP bin manifest is ambiguous.\n");
            (void)delete_stage_objects(device, &stage);
            goto cleanup;
        }
        if (backup_old_code(device, files, &old, &backup) ||
            create_remote_backup(device, &backup, extensions->folder_id,
                                 extensions->storage_id, &remote_backup)) {
            fprintf(stderr, "Could not create verified code recovery journal.\n");
            (void)delete_stage_objects(device, &remote_backup);
            (void)delete_stage_objects(device, &stage);
            goto cleanup;
        }

        LIBMTP_folder_t *guard_folders = LIBMTP_Get_Folder_List(device);
        LIBMTP_folder_t *guard_extensions =
            find_unique_root_child(guard_folders, "extensions");
        LIBMTP_folder_t *guard_app = NULL;
        int guard_failed = !guard_extensions ||
            find_unique_direct_folder(guard_extensions, APP_NAME, &guard_app) ||
            !guard_app || guard_app->folder_id != existing->folder_id ||
            verify_installer_lock(device, &installer_lock);
        if (guard_folders) LIBMTP_destroy_folder_t(guard_folders);
        if (guard_failed) {
            fprintf(stderr, "Life counter started during MTP staging.\n");
            (void)delete_stage_objects(device, &remote_backup);
            (void)delete_stage_objects(device, &stage);
            goto cleanup;
        }
    }

    if (promote_stage(device, extensions->folder_id, extensions->storage_id,
                      existing, &stage, &old, &cutover)) {
        rollback_incomplete = rollback_cutover(
            device, extensions->storage_id, &stage, &remote_backup, &cutover);
        retain_installer_lock = rollback_incomplete;
        keep_local_backup = rollback_incomplete && backup.root[0];
        goto cleanup;
    }

    LIBMTP_folder_t *after_folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_file_t *after_files = LIBMTP_Get_Filelisting(device);
    LIBMTP_folder_t *after_extensions =
        find_unique_root_child(after_folders, "extensions");
    LIBMTP_folder_t *after_app = NULL;
    int after_invalid = !after_folders || !after_files || !after_extensions ||
        find_unique_direct_folder(after_extensions, APP_NAME, &after_app) ||
        !after_app || after_app->folder_id != cutover.final_app ||
        verify_promoted_code(device, argv[1], after_app, &stage);
    DataSnapshot after = {0};
    if (!after_invalid && data_snapshot(device, after_app, after_files, &after)) {
        after_invalid = 1;
    }
    int data_changed = !data_snapshot_equal(&before, &after);
    if (!after_invalid && data_changed) {
        fprintf(stderr,
                "Preserved MTP data mismatch: before=%d/%zu/%zu/%llu/%llu "
                "after=%d/%zu/%zu/%llu/%llu.\n",
                before.exists, before.folders, before.files,
                (unsigned long long)before.xor_hash,
                (unsigned long long)before.sum_hash,
                after.exists, after.folders, after.files,
                (unsigned long long)after.xor_hash,
                (unsigned long long)after.sum_hash);
    }
    if (after_invalid || data_changed) {
        fprintf(stderr, "Promoted code or preserved game data failed verification.\n");
        rollback_incomplete = rollback_cutover(
            device, extensions->storage_id, &stage, &remote_backup, &cutover);
        retain_installer_lock = rollback_incomplete;
        keep_local_backup = rollback_incomplete && backup.root[0];
        destroy_file_list(after_files);
        if (after_folders) LIBMTP_destroy_folder_t(after_folders);
        goto cleanup;
    }
    destroy_file_list(after_files);
    LIBMTP_destroy_folder_t(after_folders);

    if (LIBMTP_Delete_Object(device, stage.app) ||
        delete_stage_objects(device, &remote_backup) ||
        delete_obsolete_assets(device, &old, files)) {
        fprintf(stderr, "New application verified, but old-code cleanup failed.\n");
        LIBMTP_Dump_Errorstack(device);
        keep_local_backup = backup.root[0] != '\0';
        goto cleanup;
    }
    result = 0;

cleanup:
    if (retain_installer_lock && installer_lock.folder) {
        fprintf(stderr, "Installer lock retained until reconnect recovery.\n");
    } else if (release_installer_lock(device, &installer_lock)) {
        fprintf(stderr, "Installer lock cleanup is incomplete; retry installation.\n");
        result = 1;
        if (backup.root[0]) keep_local_backup = 1;
    }
    if (keep_local_backup) {
        fprintf(stderr, "Local recovery backup retained at: %s\n", backup.root);
    } else {
        cleanup_code_backup(&backup);
    }
    destroy_file_list(files);
    if (folders) LIBMTP_destroy_folder_t(folders);
    LIBMTP_Release_Device(device);
    close(host_lock);
    if (!result) puts("INSTALL_COMPLETE");
    return result;
}
