#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_cmd(const char *command) {
    return system(command);
}

void parse_metadata(const char *path, char *name, char *version, char *maint) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Name=\"", 6) == 0) {
            sscanf(line, "Name=\"%255[^\"]\"", name);
        } else if (strncmp(line, "Version=\"", 9) == 0) {
            sscanf(line, "Version=\"%255[^\"]\"", version);
        } else if (strncmp(line, "Maintainer=\"", 12) == 0) {
            sscanf(line, "Maintainer=\"%255[^\"]\"", maint);
        }
    }
    fclose(f);
}

void print_progress(int percent, const char* name, const char* version, const char* maint) {
    int width = 13;
    int pos = (percent * width) / 100;
    printf("\rInstalling %s %s by %s [", name, version, maint);
    for (int i = 0; i < width; ++i) {
        putchar(i < pos ? '=' : ' ');
    }
    printf("] %d%%", percent);
    fflush(stdout);
    if (percent == 100) printf("\n");
}

void cleanup_mounts(void) {
    run_cmd("umount -l /nf-tree/rebase/dev/pts >/dev/null 2>&1");
    run_cmd("umount -l /nf-tree/rebase/dev >/dev/null 2>&1");
    run_cmd("umount -l /nf-tree/rebase/proc >/dev/null 2>&1");
    run_cmd("umount -l /nf-tree/rebase/sys >/dev/null 2>&1");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.nfrb> [--apply-host]\n", argv[0]);
        return 1;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "Error: Root privileges required.\n");
        return 1;
    }

    char *nfrb_file = argv[1];
    int apply_host = 0;
    if (argc >= 3 && strcmp(argv[2], "--apply-host") == 0) {
        apply_host = 1;
    }

    char name[256] = "Unknown OS";
    char version[256] = "0.0";
    char maint[256] = "Unknown Maintainer";

    run_cmd("rm -rf /tmp/nfrb_work >/dev/null 2>&1");
    run_cmd("mkdir -p /tmp/nfrb_work >/dev/null 2>&1");

    char *tar_args[] = {"tar", "-xJf", nfrb_file, "-C", "/tmp/nfrb_work", NULL};
    if (run_argv(tar_args) != 0) {
        fprintf(stderr, "Error: Failed to extract .nfrb archive.\n");
        return 1;
    }

    parse_metadata("/tmp/nfrb_work/METADATA", name, version, maint);
    print_progress(10, name, version, maint);

    int status = 0;

    if (apply_host) {
        int has_nextferret = (run_cmd("dpkg-divert --list | grep -q nextferret-core") == 0);

        char *divert1[] = {"dpkg-divert", "--local", "--divert", "/etc/os-release.nfrb", "--rename", "/etc/os-release", NULL};
        char *divert2[] = {"dpkg-divert", "--local", "--divert", "/etc/motd.nfrb", "--rename", "/etc/motd", NULL};

        if (run_argv(divert1) != 0) {
            if (!has_nextferret) status = 1;
        }
        if (run_argv(divert2) != 0) {
            if (!has_nextferret) status = 1;
        }

        print_progress(60, name, version, maint);

        if (status == 0) {
            run_cmd("tar -cf - -C /tmp/nfrb_work/etc --exclude=shadow --exclude=sudoers --exclude=passwd . | tar -xf - -C /etc >/dev/null 2>&1");
            run_cmd("cp -a /tmp/nfrb_work/usr/* /usr/ >/dev/null 2>&1");
            run_cmd("cp -a /tmp/nfrb_work/debs /tmp/nfrb_debs >/dev/null 2>&1");
            print_progress(75, name, version, maint);

            if (run_cmd("DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades /tmp/nfrb_debs/*.deb >/dev/null 2>&1") != 0) {
                status = 1;
            }
        }

        print_progress(90, name, version, maint);
        run_cmd("rm -rf /tmp/nfrb_debs >/dev/null 2>&1");
        run_cmd("rm -rf /tmp/nfrb_work >/dev/null 2>&1");

        if (status == 0) {
            print_progress(100, name, version, maint);
            printf("Successfully applied %s %s by %s directly to host \xE2\x9C\x94\n", name, version, maint);
            return 0;
        } else {
            fprintf(stderr, "\nError during direct apply to host.\n");
            return 1;
        }
    } else {
        run_cmd("mkdir -p /nf-tree >/dev/null 2>&1");
        run_cmd("btrfs subvolume delete /nf-tree/rebase >/dev/null 2>&1");
        print_progress(30, name, version, maint);

        char *snapshot_args[] = {"btrfs", "subvolume", "snapshot", "/", "/nf-tree/rebase", NULL};
        if (run_argv(snapshot_args) != 0) {
            fprintf(stderr, "\nError: Failed to create btrfs snapshot.\n");
            run_cmd("rm -rf /tmp/nfrb_work >/dev/null 2>&1");
            return 1;
        }

        print_progress(40, name, version, maint);

        mkdir("/nf-tree/rebase/dev", 0755);
        mkdir("/nf-tree/rebase/dev/pts", 0755);
        mkdir("/nf-tree/rebase/proc", 0755);
        mkdir("/nf-tree/rebase/sys", 0755);

        char *mount_dev[] = {"mount", "--bind", "/dev", "/nf-tree/rebase/dev", NULL};
        char *mount_pts[] = {"mount", "--bind", "/dev/pts", "/nf-tree/rebase/dev/pts", NULL};
        char *mount_proc[] = {"mount", "--bind", "/proc", "/nf-tree/rebase/proc", NULL};
        char *mount_sys[] = {"mount", "--bind", "/sys", "/nf-tree/rebase/sys", NULL};

        run_argv(mount_dev);
        run_argv(mount_pts);
        run_argv(mount_proc);
        run_argv(mount_sys);

        print_progress(50, name, version, maint);

        int has_nextferret = (run_cmd("chroot /nf-tree/rebase sh -c 'dpkg-divert --list | grep -q nextferret-core'") == 0);

        char *divert1[] = {"chroot", "/nf-tree/rebase", "dpkg-divert", "--local", "--divert", "/etc/os-release.nfrb", "--rename", "/etc/os-release", NULL};
        char *divert2[] = {"chroot", "/nf-tree/rebase", "dpkg-divert", "--local", "--divert", "/etc/motd.nfrb", "--rename", "/etc/motd", NULL};

        if (run_argv(divert1) != 0) {
            if (!has_nextferret) status = 1;
        }
        if (run_argv(divert2) != 0) {
            if (!has_nextferret) status = 1;
        }

        print_progress(60, name, version, maint);

        if (status == 0) {
            run_cmd("tar -cf - -C /tmp/nfrb_work/etc --exclude=shadow --exclude=sudoers --exclude=passwd . | tar -xf - -C /nf-tree/rebase/etc >/dev/null 2>&1");
            run_cmd("cp -a /tmp/nfrb_work/usr/* /nf-tree/rebase/usr/ >/dev/null 2>&1");
            run_cmd("cp -a /tmp/nfrb_work/debs /nf-tree/rebase/tmp/nfrb_debs >/dev/null 2>&1");
            print_progress(75, name, version, maint);

            if (run_cmd("chroot /nf-tree/rebase sh -c 'DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades /tmp/nfrb_debs/*.deb >/dev/null 2>&1'") != 0) {
                status = 1;
            }
        }

        print_progress(90, name, version, maint);
        cleanup_mounts();
        run_cmd("rm -rf /nf-tree/rebase/tmp/nfrb_debs >/dev/null 2>&1");
        run_cmd("rm -rf /tmp/nfrb_work >/dev/null 2>&1");

        if (status == 0) {
            run_cmd("mkdir -p /mnt/nf-tree-pool >/dev/null 2>&1");
            if (run_cmd("mount -t btrfs -o subvolid=5 / /mnt/nf-tree-pool >/dev/null 2>&1") == 0) {
                time_t t = time(NULL);
                struct tm* tm = localtime(&t);
                char timestamp[64];
                strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm);

                char cmd[1024];
                snprintf(cmd, sizeof(cmd), "mv /mnt/nf-tree-pool/@ /mnt/nf-tree-pool/@_old_%s >/dev/null 2>&1", timestamp);
                run_cmd(cmd);

                snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot /mnt/nf-tree-pool/@_old_%s/nf-tree/rebase /mnt/nf-tree-pool/@ >/dev/null 2>&1", timestamp);
                if (run_cmd(cmd) != 0) {
                    status = 1;
                }

                run_cmd("umount -l /mnt/nf-tree-pool >/dev/null 2>&1");
            } else {
                status = 1;
            }
        }

        if (status == 0) {
            print_progress(100, name, version, maint);
            printf("Successfully installed %s %s by %s \xE2\x9C\x94\n", name, version, maint);
            printf("REBOOT REQUIRED to apply changes.\n");
            return 0;
        } else {
            fprintf(stderr, "\nError during rebase subvolume swap. Rolling back...\n");
            cleanup_mounts();
            run_cmd("btrfs subvolume delete /nf-tree/rebase >/dev/null 2>&1");
            return 1;
        }
    }
}
