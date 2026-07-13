#include <libmtp.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static LIBMTP_folder_t *find_any(LIBMTP_folder_t *folder, const char *name) {
    for (; folder; folder = folder->sibling) {
        if (folder->name && strcmp(folder->name, name) == 0) return folder;
        LIBMTP_folder_t *found = find_any(folder->child, name);
        if (found) return found;
    }
    return NULL;
}

static LIBMTP_folder_t *find_child(LIBMTP_folder_t *folder, uint32_t parent,
                                   const char *name) {
    for (; folder; folder = folder->sibling) {
        if (folder->parent_id == parent && folder->name &&
            strcmp(folder->name, name) == 0) return folder;
        LIBMTP_folder_t *found = find_child(folder->child, parent, name);
        if (found) return found;
    }
    return NULL;
}

static int delete_remote_files(LIBMTP_mtpdevice_t *device, uint32_t parent,
                               LIBMTP_file_t *files) {
    for (LIBMTP_file_t *file = files; file; file = file->next) {
        if (file->parent_id == parent) {
            printf("Deleting file %s\n", file->filename);
            if (LIBMTP_Delete_Object(device, file->item_id)) {
                LIBMTP_Dump_Errorstack(device);
                return 1;
            }
        }
    }
    return 0;
}

static int delete_remote_tree(LIBMTP_mtpdevice_t *device,
                              LIBMTP_folder_t *folder,
                              LIBMTP_file_t *files) {
    for (LIBMTP_folder_t *child = folder->child; child; child = child->sibling) {
        if (delete_remote_tree(device, child, files)) return 1;
    }
    if (delete_remote_files(device, folder->folder_id, files)) return 1;
    printf("Deleting folder %s\n", folder->name);
    if (LIBMTP_Delete_Object(device, folder->folder_id)) {
        LIBMTP_Dump_Errorstack(device);
        return 1;
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
            memcmp(left_buffer, right_buffer, left_size) != 0) {
            equal = 0;
        }
        if (left_size < sizeof(left_buffer)) {
            if (ferror(left) || ferror(right)) equal = 0;
            break;
        }
    }
    if (fclose(left) != 0 || fclose(right) != 0) equal = 0;
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

static int upload_tree(LIBMTP_mtpdevice_t *device, const char *local,
                       uint32_t parent, uint32_t storage, uint32_t app_id,
                       uint32_t preserved_data_id) {
    DIR *directory = opendir(local);
    if (!directory) {
        perror(local);
        return 1;
    }

    int failed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", local, entry->d_name) >=
            (int)sizeof(path)) {
            fprintf(stderr, "Path too long: %s/%s\n", local, entry->d_name);
            failed = 1;
            break;
        }

        struct stat status;
        if (stat(path, &status) != 0) {
            perror(path);
            failed = 1;
            break;
        }

        if (S_ISDIR(status.st_mode)) {
            uint32_t folder_id = parent == app_id && preserved_data_id &&
                                 strcmp(entry->d_name, "data") == 0
                ? preserved_data_id
                : LIBMTP_Create_Folder(device, entry->d_name, parent, storage);
            if (!folder_id) {
                fprintf(stderr, "Failed to create folder: %s\n", path);
                LIBMTP_Dump_Errorstack(device);
                failed = 1;
                break;
            }
            printf("Using folder %s (id %u)\n", entry->d_name, folder_id);
            if (upload_tree(device, path, folder_id, storage,
                            app_id, preserved_data_id)) {
                failed = 1;
                break;
            }
        } else if (S_ISREG(status.st_mode)) {
            LIBMTP_file_t *file = LIBMTP_new_file_t();
            file->filename = strdup(entry->d_name);
            file->filesize = (uint64_t)status.st_size;
            file->filetype = LIBMTP_FILETYPE_UNKNOWN;
            file->parent_id = parent;
            file->storage_id = storage;
            if (LIBMTP_Send_File_From_File(device, path, file, NULL, NULL)) {
                fprintf(stderr, "Failed to upload file: %s\n", path);
                LIBMTP_Dump_Errorstack(device);
                failed = 1;
                LIBMTP_destroy_file_t(file);
                break;
            }
            if (verify_remote_file(device, file->item_id, path)) {
                fprintf(stderr, "Failed readback verification: %s\n", path);
                failed = 1;
                LIBMTP_destroy_file_t(file);
                break;
            }
            printf("Uploaded and verified %s (%lld bytes, id %u)\n", entry->d_name,
                   (long long)status.st_size, file->item_id);
            LIBMTP_destroy_file_t(file);
        }
    }
    closedir(directory);
    return failed;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s LOCAL_APP_DIR\n", argv[0]);
        return 2;
    }

    LIBMTP_Init();
    LIBMTP_mtpdevice_t *device = LIBMTP_Get_First_Device();
    if (!device) {
        fprintf(stderr, "No MTP Kindle found. Reconnect and unlock the device.\n");
        return 3;
    }

    LIBMTP_folder_t *folders = LIBMTP_Get_Folder_List(device);
    LIBMTP_folder_t *extensions = find_any(folders, "extensions");
    if (!extensions) {
        fprintf(stderr, "Could not find the Kindle extensions folder.\n");
        LIBMTP_destroy_folder_t(folders);
        LIBMTP_Release_Device(device);
        return 4;
    }

    uint32_t app_id = 0;
    uint32_t preserved_data_id = 0;
    LIBMTP_folder_t *existing = find_child(
        folders, extensions->folder_id, "mtg-life-counter");
    if (existing) {
        LIBMTP_file_t *files = LIBMTP_Get_Filelisting(device);
        LIBMTP_folder_t *data = find_child(
            existing, existing->folder_id, "data");
        if (data) preserved_data_id = data->folder_id;

        for (LIBMTP_folder_t *child = existing->child; child;
             child = child->sibling) {
            if (child->folder_id == preserved_data_id) continue;
            if (delete_remote_tree(device, child, files)) {
                LIBMTP_destroy_file_t(files);
                LIBMTP_destroy_folder_t(folders);
                LIBMTP_Release_Device(device);
                return 5;
            }
        }
        if (delete_remote_files(device, existing->folder_id, files)) {
            LIBMTP_destroy_file_t(files);
            LIBMTP_destroy_folder_t(folders);
            LIBMTP_Release_Device(device);
            return 5;
        }
        LIBMTP_destroy_file_t(files);
        app_id = existing->folder_id;
        printf("Updating extensions/mtg-life-counter (id %u); preserving data id %u\n",
               app_id, preserved_data_id);
    } else {
        char app_name[] = "mtg-life-counter";
        app_id = LIBMTP_Create_Folder(
            device, app_name, extensions->folder_id, extensions->storage_id);
        if (!app_id) {
            fprintf(stderr, "Could not create extensions/mtg-life-counter.\n");
            LIBMTP_Dump_Errorstack(device);
            LIBMTP_destroy_folder_t(folders);
            LIBMTP_Release_Device(device);
            return 6;
        }
        printf("Created extensions/mtg-life-counter (id %u)\n", app_id);
    }

    int result = upload_tree(device, argv[1], app_id,
                             extensions->storage_id, app_id,
                             preserved_data_id);
    LIBMTP_destroy_folder_t(folders);
    LIBMTP_Release_Device(device);
    if (!result) puts("INSTALL_COMPLETE");
    return result;
}
