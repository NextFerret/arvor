#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include <sys/sysmacros.h>

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    return s;
}

static int run_execvp(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int run_capture(char *const argv[], char *out, size_t outsz) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    ssize_t n = read(fds[0], out, outsz - 1);
    if (n < 0) n = 0;
    out[n] = 0;
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int get_root_device(char *dev, size_t sz) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        syslog(LOG_ERR, "cannot open /proc/mounts");
        return -1;
    }
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char d[256], mp[256];
        if (sscanf(line, "%255s %255s", d, mp) == 2) {
            if (strcmp(mp, "/") == 0) {
                strncpy(dev, d, sz - 1);
                dev[sz - 1] = 0;
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    if (!found) {
        syslog(LOG_ERR, "could not find device mounted at /");
        return -1;
    }
    return 0;
}

static int try_lvs_lookup(char *const argv[], char *vg, size_t vgsz, char *lv, size_t lvsz) {
    char out[512];
    if (run_capture(argv, out, sizeof(out)) != 0) return -1;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    char *sep = strchr(out, '|');
    if (!sep) return -1;
    *sep = 0;
    char *v = trim(out);
    char *l = trim(sep + 1);
    if (!*v || !*l) return -1;
    strncpy(vg, v, vgsz - 1); vg[vgsz - 1] = 0;
    strncpy(lv, l, lvsz - 1); lv[lvsz - 1] = 0;
    return 0;
}

static int get_vg_lv(const char *dev, char *vg, size_t vgsz, char *lv, size_t lvsz) {
    syslog(LOG_INFO, "root device is %s", dev);

    char *argv1[] = {"lvs", "--noheadings", "--nosuffix", "-o", "vg_name,lv_name", "--separator", "|", (char *)dev, NULL};
    if (try_lvs_lookup(argv1, vg, vgsz, lv, lvsz) == 0) {
        syslog(LOG_INFO, "vg=%s lv=%s (matched by name)", vg, lv);
        return 0;
    }

    syslog(LOG_INFO, "name-based lvs lookup failed for %s, falling back to major:minor match", dev);

    struct stat st;
    if (stat(dev, &st) != 0) {
        syslog(LOG_ERR, "stat(%s) failed: %s", dev, strerror(errno));
        return -1;
    }
    unsigned int maj = major(st.st_rdev);
    unsigned int min = minor(st.st_rdev);
    char sel[96];
    snprintf(sel, sizeof(sel), "lv_kernel_major=%u && lv_kernel_minor=%u", maj, min);
    char *argv2[] = {"lvs", "--noheadings", "--nosuffix", "-o", "vg_name,lv_name", "--separator", "|", "-S", sel, NULL};
    if (try_lvs_lookup(argv2, vg, vgsz, lv, lvsz) != 0) {
        syslog(LOG_ERR, "major:minor lookup (%u:%u) also failed - is root on LVM?", maj, min);
        return -1;
    }
    syslog(LOG_INFO, "vg=%s lv=%s (matched by %u:%u)", vg, lv, maj, min);
    return 0;
}

static int lv_exists(const char *vg, const char *lv) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", vg, lv);
    char out[512];
    char *argv1[] = {"lvs", "--noheadings", "-o", "lv_name", path, NULL};
    return run_capture(argv1, out, sizeof(out)) == 0;
}

static volatile sig_atomic_t g_frozen = 0;

static void thaw_now(void) {
    if (g_frozen) {
        g_frozen = 0;
        char *thaw[] = {"fsfreeze", "-u", "/", NULL};
        run_execvp(thaw);
    }
}

static void crash_handler(int sig) {
    (void)sig;
    thaw_now();
    _exit(1);
}

static void setup_freeze_safety_net(void) {
    signal(SIGALRM, crash_handler);
    signal(SIGTERM, crash_handler);
    signal(SIGINT, crash_handler);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    atexit(thaw_now);
}

static int refresh_snapshot(const char *vg, const char *lv) {
    char snap[256];
    snprintf(snap, sizeof(snap), "%s_b", lv);
    if (lv_exists(vg, snap)) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", vg, snap);
        syslog(LOG_INFO, "removing existing snapshot %s", path);
        char *rmargv[] = {"lvremove", "-f", path, NULL};
        int rc = run_execvp(rmargv);
        if (rc != 0) {
            syslog(LOG_ERR, "lvremove %s failed (exit %d)", path, rc);
            return -1;
        }
    }
    char origin[256];
    snprintf(origin, sizeof(origin), "%s/%s", vg, lv);

    setup_freeze_safety_net();

    syslog(LOG_INFO, "freezing / before snapshot");
    char *freeze[] = {"fsfreeze", "-f", "/", NULL};
    g_frozen = (run_execvp(freeze) == 0);
    if (!g_frozen) {
        syslog(LOG_WARNING, "fsfreeze failed, continuing without freeze");
    }
    if (g_frozen) alarm(30);

    syslog(LOG_INFO, "creating snapshot %s_b from %s", lv, origin);
    char *mkargv[] = {"lvcreate", "-s", "-n", snap, origin, NULL};
    int rc = run_execvp(mkargv);
    if (rc != 0) {
        syslog(LOG_ERR, "lvcreate failed (exit %d)", rc);
    }

    alarm(0);
    thaw_now();
    return rc;
}

static void clear_lvmabd_tags(const char *vg, const char *lv) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", vg, lv);
    char out[1024];
    char *argv1[] = {"lvs", "--noheadings", "-o", "tags", path, NULL};
    if (run_capture(argv1, out, sizeof(out)) != 0) return;
    char *tok = strtok(out, ",\n ");
    while (tok) {
        if (strncmp(tok, "lvmabd_", 7) == 0) {
            char tagbuf[256];
            strncpy(tagbuf, tok, sizeof(tagbuf) - 1);
            tagbuf[sizeof(tagbuf) - 1] = 0;
            char *dt[] = {"lvchange", "--deltag", tagbuf, path, NULL};
            run_execvp(dt);
        }
        tok = strtok(NULL, ",\n ");
    }
}

static void ensure_installed_binary(void) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        syslog(LOG_ERR, "readlink(/proc/self/exe) failed: %s", strerror(errno));
        return;
    }
    self[n] = 0;
    if (strcmp(self, "/usr/local/sbin/lvmabd_start") == 0) return;

    char *args[] = {"install", "-m", "755", self, "/usr/local/sbin/lvmabd_start", NULL};
    int rc = run_execvp(args);
    if (rc != 0) {
        syslog(LOG_ERR, "failed to install binary from %s to /usr/local/sbin/lvmabd_start (exit %d)", self, rc);
    } else {
        syslog(LOG_INFO, "installed binary from %s to /usr/local/sbin/lvmabd_start", self);
    }
}

static void install_systemd(void) {
    char chkout[64];
    char *chk[] = {"systemctl", "is-enabled", "lvmabd.timer", NULL};
    if (run_capture(chk, chkout, sizeof(chkout)) == 0) {
        syslog(LOG_INFO, "lvmabd.timer already enabled, skipping reinstall");
        return;
    }

    FILE *f = fopen("/etc/systemd/system/lvmabd.service", "w");
    if (!f) {
        syslog(LOG_ERR, "cannot write lvmabd.service: %s", strerror(errno));
        return;
    }
    fprintf(f,
        "[Unit]\n"
        "Description=lvmabd snapshot refresh\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=/usr/local/sbin/lvmabd_start 1\n");
    fclose(f);

    f = fopen("/etc/systemd/system/lvmabd.timer", "w");
    if (!f) {
        syslog(LOG_ERR, "cannot write lvmabd.timer: %s", strerror(errno));
        return;
    }
    fprintf(f,
        "[Unit]\n"
        "Description=lvmabd boot success trigger\n"
        "\n"
        "[Timer]\n"
        "OnBootSec=3min\n"
        "AccuracySec=10s\n"
        "\n"
        "[Install]\n"
        "WantedBy=timers.target\n");
    fclose(f);

    char *a1[] = {"systemctl", "daemon-reload", NULL};
    int rc1 = run_execvp(a1);
    if (rc1 != 0) syslog(LOG_ERR, "systemctl daemon-reload failed (exit %d)", rc1);

    char *a2[] = {"systemctl", "enable", "--now", "lvmabd.timer", NULL};
    int rc2 = run_execvp(a2);
    if (rc2 != 0) {
        syslog(LOG_ERR, "systemctl enable --now lvmabd.timer failed (exit %d)", rc2);
    } else {
        syslog(LOG_INFO, "lvmabd.timer enabled and started");
    }
}

static const char *INITRAMFS_HOOK =
"#!/bin/sh\n"
"PREREQ=\"\"\n"
"prereqs()\n"
"{\n"
"    echo \"$PREREQ\"\n"
"}\n"
"case \"$1\" in\n"
"prereqs)\n"
"    prereqs\n"
"    exit 0\n"
"    ;;\n"
"esac\n"
"\n"
". /usr/share/initramfs-tools/hook-functions\n"
"\n"
"for b in lvconvert lvchange lvs blkid xfs_repair; do\n"
"    p=$(command -v \"$b\" 2>/dev/null)\n"
"    [ -n \"$p\" ] && copy_exec \"$p\"\n"
"done\n";

static const char *INITRAMFS_SCRIPT =
"#!/bin/sh\n"
"PREREQ=\"lvm2\"\n"
"prereqs()\n"
"{\n"
"    echo \"$PREREQ\"\n"
"}\n"
"case \"$1\" in\n"
"prereqs)\n"
"    prereqs\n"
"    exit 0\n"
"    ;;\n"
"esac\n"
"\n"
". /scripts/functions\n"
"\n"
"case \"$ROOT\" in\n"
"UUID=*)\n"
"    UUIDVAL=${ROOT#UUID=}\n"
"    DEVPATH=$(blkid -U \"$UUIDVAL\" 2>/dev/null)\n"
"    ;;\n"
"*)\n"
"    DEVPATH=\"$ROOT\"\n"
"    ;;\n"
"esac\n"
"[ -z \"$DEVPATH\" ] && DEVPATH=\"$ROOT\"\n"
"\n"
"VG=$(lvs --noheadings -o vg_name \"$DEVPATH\" 2>/dev/null | tr -d ' ')\n"
"LV=$(lvs --noheadings -o lv_name \"$DEVPATH\" 2>/dev/null | tr -d ' ')\n"
"\n"
"if [ -z \"$VG\" ] || [ -z \"$LV\" ]; then\n"
"    exit 0\n"
"fi\n"
"\n"
"HAVE_XFSCHECK=0\n"
"command -v xfs_repair >/dev/null 2>&1 && HAVE_XFSCHECK=1\n"
"\n"
"TAGS=$(lvs --noheadings -o tags \"$VG/$LV\" 2>/dev/null)\n"
"\n"
"case \"$TAGS\" in\n"
"*lvmabd_boot=inprogress*)\n"
"    TRIEDTAG=$(echo \"$TAGS\" | tr ',' '\\n' | grep '^lvmabd_tried=' | cut -d= -f2-)\n"
"    ALLTRIED=\"$TRIEDTAG\"\n"
"\n"
"    EXCLUDE=\"\"\n"
"    if [ -n \"$TRIEDTAG\" ]; then\n"
"        OLDIFS=$IFS\n"
"        IFS='+'\n"
"        for t in $TRIEDTAG; do\n"
"            EXCLUDE=\"$EXCLUDE $t\"\n"
"        done\n"
"        IFS=$OLDIFS\n"
"    fi\n"
"\n"
"    OTHERS=$(lvs --noheadings -o lv_name,origin --sort -lv_time \"$VG\" 2>/dev/null | awk -v o=\"$LV\" -v b=\"${LV}_b\" '$2==o && $1!=b {print $1}')\n"
"    ORDER=\"${LV}_b $OTHERS\"\n"
"\n"
"    TARGET=\"\"\n"
"    for c in $ORDER; do\n"
"        SKIP=0\n"
"        for e in $EXCLUDE; do\n"
"            [ \"$c\" = \"$e\" ] && SKIP=1\n"
"        done\n"
"        [ \"$SKIP\" = \"1\" ] && continue\n"
"        [ -z \"$c\" ] && continue\n"
"\n"
"        lvchange -ay \"$VG/$c\" 2>/dev/null\n"
"\n"
"        if [ \"$HAVE_XFSCHECK\" = \"1\" ]; then\n"
"            if xfs_repair -n \"/dev/$VG/$c\" >/dev/null 2>&1; then\n"
"                TARGET=\"$c\"\n"
"                break\n"
"            else\n"
"                if [ -z \"$ALLTRIED\" ]; then\n"
"                    ALLTRIED=\"$c\"\n"
"                else\n"
"                    ALLTRIED=\"$ALLTRIED+$c\"\n"
"                fi\n"
"            fi\n"
"        else\n"
"            TARGET=\"$c\"\n"
"            break\n"
"        fi\n"
"    done\n"
"\n"
"    if [ -n \"$TARGET\" ]; then\n"
"        lvchange -an \"$VG/$LV\" 2>/dev/null\n"
"        lvconvert --merge -y \"$VG/$TARGET\" 2>/dev/null\n"
"        lvchange -ay \"$VG/$LV\" 2>/dev/null\n"
"        if [ -z \"$ALLTRIED\" ]; then\n"
"            ALLTRIED=\"$TARGET\"\n"
"        else\n"
"            ALLTRIED=\"$ALLTRIED+$TARGET\"\n"
"        fi\n"
"    fi\n"
"\n"
"    lvchange --deltag \"lvmabd_tried=$TRIEDTAG\" \"$VG/$LV\" 2>/dev/null\n"
"    if [ -n \"$ALLTRIED\" ]; then\n"
"        lvchange --addtag \"lvmabd_tried=$ALLTRIED\" \"$VG/$LV\" 2>/dev/null\n"
"    fi\n"
"    ;;\n"
"*)\n"
"    lvchange --addtag lvmabd_boot=inprogress \"$VG/$LV\" 2>/dev/null\n"
"    ;;\n"
"esac\n";

static void write_file_exec(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) {
        syslog(LOG_ERR, "cannot write %s (%s)", path, strerror(errno));
        return;
    }
    fputs(content, f);
    fclose(f);
    chmod(path, 0755);
}

static void install_initramfs(void) {
    syslog(LOG_INFO, "installing initramfs hook and script");
    write_file_exec("/etc/initramfs-tools/hooks/lvmabd", INITRAMFS_HOOK);
    write_file_exec("/etc/initramfs-tools/scripts/local-top/lvmabd", INITRAMFS_SCRIPT);
    char *a1[] = {"update-initramfs", "-u", NULL};
    int rc = run_execvp(a1);
    if (rc != 0) {
        syslog(LOG_ERR, "update-initramfs -u failed (exit %d)", rc);
    }
}

static void stop_systemd(void) {
    char *a1[] = {"systemctl", "disable", "--now", "lvmabd.timer", NULL};
    run_execvp(a1);
    remove("/etc/systemd/system/lvmabd.timer");
    remove("/etc/systemd/system/lvmabd.service");
    char *a2[] = {"systemctl", "daemon-reload", NULL};
    run_execvp(a2);
}

int main(int argc, char **argv) {
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    openlog("lvmabd_start", LOG_PID, LOG_DAEMON);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <1|0>\n", argv[0]);
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "lvmabd_start: must be run as root (try: sudo %s %s)\n", argv[0], argv[1]);
        return 1;
    }
    if (strcmp(argv[1], "0") == 0) {
        syslog(LOG_INFO, "disabling timer/service");
        stop_systemd();
        syslog(LOG_INFO, "done");
        closelog();
        return 0;
    }
    if (strcmp(argv[1], "1") != 0) {
        fprintf(stderr, "invalid argument\n");
        return 1;
    }
    char dev[256], vg[256], lv[256];
    if (get_root_device(dev, sizeof(dev)) != 0) return 1;
    if (get_vg_lv(dev, vg, sizeof(vg), lv, sizeof(lv)) != 0) return 1;
    if (refresh_snapshot(vg, lv) != 0) return 1;
    clear_lvmabd_tags(vg, lv);
    ensure_installed_binary();
    install_systemd();
    install_initramfs();
    syslog(LOG_INFO, "done");
    closelog();
    return 0;
}
