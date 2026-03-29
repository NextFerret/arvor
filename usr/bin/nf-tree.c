#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>

#define SNAPSHOT_BASE "/nf-tree/snapshots"
#define AUTO_DIR "/nf-tree/snapshots/auto"
#define MANUAL_DIR "/nf-tree/snapshots/manual"
#define ROOT_SOURCE "/"
#define HOME_SOURCE "/home"
#define SERVICE_PATH "/etc/systemd/system/nf-tree.service"

typedef struct {
    int returncode;
    char stdout_data[4096];
} CmdResult;

CmdResult run_cmd(const char* command) {
    CmdResult res = {-1, ""};
    char buffer[256];
    char full_cmd[1024];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", command);
    
    FILE* pipe = popen(full_cmd, "r");
    if (!pipe) return res;

    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        strncat(res.stdout_data, buffer, sizeof(res.stdout_data) - strlen(res.stdout_data) - 1);
    }

    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        res.returncode = WEXITSTATUS(status);
    }
    return res;
}

void setup_dirs() {
    mkdir("/nf-tree", 0755);
    mkdir(SNAPSHOT_BASE, 0755);
    mkdir(AUTO_DIR, 0755);
    mkdir(MANUAL_DIR, 0755);
}

void update_grub() {
    system("update-grub > /dev/null 2>&1");
}

char* get_subvol_id(const char* path) {
    static char id[32];
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume show %s", path);
    CmdResult res = run_cmd(cmd);
    
    if (res.returncode == 0) {
        char* line = strtok(res.stdout_data, "\n");
        while (line != NULL) {
            if (strstr(line, "Subvolume ID:")) {
                char* token = strrchr(line, ' ');
                if (token) {
                    strncpy(id, token + 1, sizeof(id) - 1);
                    id[strcspn(id, "\r\n")] = 0;
                    return id;
                }
            }
            line = strtok(NULL, "\n");
        }
    }
    return NULL;
}

void toggle_swap(bool active) {
    if (!active) {
        system("swapoff -a > /dev/null 2>&1");
    } else {
        if (access("/dev/zram0", F_OK) == 0) {
            system("mkswap /dev/zram0 > /dev/null 2>&1");
            system("swapon /dev/zram0 -p 100 > /dev/null 2>&1");
        } else {
            system("swapon -a > /dev/null 2>&1");
        }
    }
}

bool create_snapshot(const char* source, const char* name, bool is_auto) {
    setup_dirs();
    
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm);
    
    char target_path[1024];
    snprintf(target_path, sizeof(target_path), "%s/%s-%s", is_auto ? AUTO_DIR : MANUAL_DIR, name, timestamp);

    if (access(target_path, F_OK) == 0) return false;

    bool is_root = (strcmp(source, ROOT_SOURCE) == 0);
    if (is_root) toggle_swap(false);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", source, target_path);
    CmdResult res = run_cmd(cmd);
    
    bool success = (res.returncode == 0);
    if (success) printf("Created: %s-%s\n", name, timestamp);

    if (is_root) toggle_swap(true);
    return success;
}

bool rollback_snapshot(const char* name) {
    char snap_path[1024];
    snprintf(snap_path, sizeof(snap_path), "%s/%s", MANUAL_DIR, name);
    if (access(snap_path, F_OK) != 0) {
        snprintf(snap_path, sizeof(snap_path), "%s/%s", AUTO_DIR, name);
        if (access(snap_path, F_OK) != 0) {
            printf("Error: Snapshot %s not found.\n", name);
            return false;
        }
    }

    const char* target = NULL;
    if (strncmp(name, "root-", 5) == 0) target = ROOT_SOURCE;
    else if (strncmp(name, "home-", 5) == 0) target = HOME_SOURCE;

    if (!target) {
        printf("Error: Invalid target.\n");
        return false;
    }

    char* subid = get_subvol_id(snap_path);
    if (!subid) {
        printf("Error: Could not retrieve ID.\n");
        return false;
    }

    printf(">> Setting %s (ID: %s) as default for %s...\n", name, subid, target);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume set-default %s %s", subid, target);
    CmdResult res = run_cmd(cmd);
    
    if (res.returncode == 0) {
        printf("Success: %s is now default. REBOOT REQUIRED.\n", name);
        return true;
    }

    printf("Rollback failed: %s\n", res.stdout_data);
    return false;
}

void list_snapshots() {
    const char* paths[] = {MANUAL_DIR, AUTO_DIR};
    const char* labels[] = {"Manual", "Auto"};

    for (int i = 0; i < 2; i++) {
        printf("\n--- %s Snapshots ---\n", labels[i]);
        DIR* d = opendir(paths[i]);
        if (!d) continue;
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_DIR && dir->d_name[0] != '.') {
                printf("    %s\n", dir->d_name);
            }
        }
        closedir(d);
    }
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) return 1;
    if (argc < 2) return 0;

    char* command = argv[1];

    if (strcmp(command, "create") == 0 && argc >= 3) {
        create_snapshot(ROOT_SOURCE, argv[2], false);
        create_snapshot(HOME_SOURCE, argv[2], false);
        update_grub();
    } else if (strcmp(command, "rollback") == 0 && argc >= 3) {
        if (rollback_snapshot(argv[2])) update_grub();
    } else if (strcmp(command, "list") == 0) {
        list_snapshots();
    }

    return 0;
} 
