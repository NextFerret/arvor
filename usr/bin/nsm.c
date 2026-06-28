#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

#define SNAPSHOT_BASE           "/nsm/snapshots"
#define AUTO_DIR                "/nsm/snapshots/auto"
#define DAEMON_AUTO_DIR         "/nsm/snapshots/auto/daemon"
#define MANUAL_DIR              "/nsm/snapshots/manual"
#define ROOT_SOURCE             "/"
#define HOME_SOURCE             "/home"
#define STR_BUF                 512
#define PATH_BUF                1024
#define META_FILE               "meta"
#define OUT_BUF                 8192
#define MAX_ARGV                32

typedef struct {
    int rc;
    char out[OUT_BUF];
} CmdResult;

typedef struct {
    char vg[STR_BUF];
    char lv[STR_BUF];
    char lvpath[PATH_BUF];
    char segtype[STR_BUF];
    bool is_thin;
} LvmInfo;

typedef struct {
    char kind[16];
    char type[16];
    char name[STR_BUF];
    char timestamp[64];
    char origin_mount[PATH_BUF];
    char origin_source[PATH_BUF];
    char origin_vg[STR_BUF];
    char origin_lv[STR_BUF];
    char origin_lvpath[PATH_BUF];
    char snapshot_lvpath[PATH_BUF];
} SnapshotMeta;

typedef struct {
    char name[STR_BUF];
    char dir[PATH_BUF];
    char lvpath[PATH_BUF];
    unsigned long long size;
} SnapSizeInfo;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static int exec_capture(char *const argv[], char *out, size_t sz)
{
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pfd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < sz - 1 &&
           (n = read(pfd[0], out + total, sz - 1 - total)) > 0)
        total += (size_t)n;
    out[total] = '\0';
    close(pfd[0]);

    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int exec_silent(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static CmdResult run_argv(char *const argv[])
{
    CmdResult r = {-1, ""};
    r.rc = exec_capture(argv, r.out, sizeof(r.out));
    return r;
}

static CmdResult run_args(const char *first, ...)
{
    char *argv[MAX_ARGV];
    int n = 0;
    argv[n++] = (char *)first;

    va_list ap;
    va_start(ap, first);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < MAX_ARGV - 1)
        argv[n++] = (char *)a;
    va_end(ap);
    argv[n] = NULL;

    return run_argv(argv);
}

static int run_args_silent(const char *first, ...)
{
    char *argv[MAX_ARGV];
    int n = 0;
    argv[n++] = (char *)first;

    va_list ap;
    va_start(ap, first);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < MAX_ARGV - 1)
        argv[n++] = (char *)a;
    va_end(ap);
    argv[n] = NULL;

    return exec_silent(argv);
}

static bool ensure_dir(const char *path)
{
    char tmp[PATH_BUF];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/' && len > 1) tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool read_mount_field(const char *target, const char *field, char *out, size_t sz)
{
    CmdResult r = run_args("findmnt", "-no", field, "--target", target, NULL);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    snprintf(out, sz, "%s", r.out);
    return true;
}

static bool resolve_lvm_info(const char *source, LvmInfo *info)
{
    CmdResult r = run_args("lvs", "--noheadings", "--separator", "/",
                           "-o", "vg_name,lv_name,segtype", source, NULL);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    char vg[STR_BUF] = "", lv[STR_BUF] = "", seg[STR_BUF] = "";
    if (sscanf(r.out, "%511[^/]/%511[^/]/%511s", vg, lv, seg) != 3) return false;
    trim(vg); trim(lv); trim(seg);
    if (vg[0] == '\0' || lv[0] == '\0' || seg[0] == '\0') return false;
    snprintf(info->vg,     sizeof(info->vg),     "%s", vg);
    snprintf(info->lv,     sizeof(info->lv),     "%s", lv);
    snprintf(info->segtype,sizeof(info->segtype), "%s", seg);
    snprintf(info->lvpath, sizeof(info->lvpath),  "/dev/%s/%s", vg, lv);
    info->is_thin = strstr(seg, "thin") != NULL;
    return true;
}

static bool snapshot_mount_fstype(const char *mountpoint)
{
    char fstype[64];
    if (!read_mount_field(mountpoint, "FSTYPE", fstype, sizeof(fstype))) return false;
    return strcmp(fstype, "xfs") == 0  || strcmp(fstype, "ext4") == 0
        || strcmp(fstype, "ext3") == 0 || strcmp(fstype, "btrfs") == 0;
}

static bool freeze_mount(const char *mountpoint)
{
    return run_args("fsfreeze", "-f", mountpoint, NULL).rc == 0;
}

static void unfreeze_mount(const char *mountpoint)
{
    run_args("fsfreeze", "-u", mountpoint, NULL);
}

static bool write_snapshot_meta(const char *dir, const SnapshotMeta *meta)
{
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, META_FILE);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "kind=%s\n",            meta->kind);
    fprintf(f, "type=%s\n",            meta->type);
    fprintf(f, "name=%s\n",            meta->name);
    fprintf(f, "timestamp=%s\n",       meta->timestamp);
    fprintf(f, "origin_mount=%s\n",    meta->origin_mount);
    fprintf(f, "origin_source=%s\n",   meta->origin_source);
    fprintf(f, "origin_vg=%s\n",       meta->origin_vg);
    fprintf(f, "origin_lv=%s\n",       meta->origin_lv);
    fprintf(f, "origin_lvpath=%s\n",   meta->origin_lvpath);
    fprintf(f, "snapshot_lvpath=%s\n", meta->snapshot_lvpath);
    fclose(f);
    return true;
}

static bool read_snapshot_meta(const char *dir, SnapshotMeta *meta)
{
    memset(meta, 0, sizeof(*meta));
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, META_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[PATH_BUF * 2];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
#define FIELD(K, DST) if (strcmp(key, K) == 0) snprintf(DST, sizeof(DST), "%s", val);
        FIELD("kind",            meta->kind)
        FIELD("type",            meta->type)
        FIELD("name",            meta->name)
        FIELD("timestamp",       meta->timestamp)
        FIELD("origin_mount",    meta->origin_mount)
        FIELD("origin_source",   meta->origin_source)
        FIELD("origin_vg",       meta->origin_vg)
        FIELD("origin_lv",       meta->origin_lv)
        FIELD("origin_lvpath",   meta->origin_lvpath)
        FIELD("snapshot_lvpath", meta->snapshot_lvpath)
#undef FIELD
    }
    fclose(f);
    return meta->kind[0] != '\0' && meta->snapshot_lvpath[0] != '\0';
}

static void remove_snapshot_meta(const char *dir)
{
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, META_FILE);
    unlink(path);
    rmdir(dir);
}

static bool delete_snapshot_lv(const char *snapshot_lvpath)
{
    return run_args("lvremove", "-fy", snapshot_lvpath, NULL).rc == 0;
}

static bool find_snapshot(const char *name, char *dir_out, size_t sz)
{
    const char *dirs[] = { MANUAL_DIR, AUTO_DIR, DAEMON_AUTO_DIR };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[PATH_BUF], meta[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        snprintf(meta, sizeof(meta), "%s/%s", path, META_FILE);
        if (access(meta, F_OK) == 0) {
            snprintf(dir_out, sz, "%s", path);
            return true;
        }
    }
    return false;
}

static bool delete_snapshot(const char *name)
{
    char dir[PATH_BUF];
    if (!find_snapshot(name, dir, sizeof(dir))) return false;
    SnapshotMeta meta;
    if (!read_snapshot_meta(dir, &meta)) return false;
    if (!delete_snapshot_lv(meta.snapshot_lvpath)) return false;
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, META_FILE);
    unlink(path);
    rmdir(dir);
    return true;
}

static bool create_snapshot(const char *mountpoint, const char *prefix,
                            const char *type, const char *name)
{
    char source[PATH_BUF];
    if (!read_mount_field(mountpoint, "SOURCE", source, sizeof(source))) {
        printf("Error: cannot resolve source for %s.\n", mountpoint);
        return false;
    }
    if (!snapshot_mount_fstype(mountpoint)) {
        printf("Error: %s filesystem is not supported (xfs/ext4/ext3/btrfs required).\n",
               mountpoint);
        return false;
    }
    LvmInfo info;
    if (!resolve_lvm_info(source, &info)) {
        printf("Error: %s is not an LVM volume or cannot be resolved.\n", mountpoint);
        return false;
    }

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", tm);

    char snapname[STR_BUF];
    snprintf(snapname, sizeof(snapname), "%s-%s-%s-%s", prefix, type, name, ts);

    const char *base = strcmp(type, "auto") == 0 ? AUTO_DIR : MANUAL_DIR;
    if (strcmp(name, "nsmd") == 0) base = DAEMON_AUTO_DIR;

    char snapdir[PATH_BUF];
    snprintf(snapdir, sizeof(snapdir), "%s/%s", base, snapname);
    if (!ensure_dir(snapdir)) {
        printf("Error: cannot create snapshot metadata directory.\n");
        return false;
    }

    bool is_root = (strcmp(mountpoint, "/") == 0);
    bool frozen  = !is_root && freeze_mount(mountpoint);

    CmdResult r;
    if (info.is_thin) {
        r = run_args("lvcreate", "-s", "-n", snapname, info.lvpath, NULL);
    } else {
        r = run_args("lvcreate", "-s", "-n", snapname,
                     "-l", "50%ORIGIN", info.lvpath, NULL);
    }

    if (frozen) unfreeze_mount(mountpoint);

    if (r.rc != 0) {
        printf("%s\n", r.out);
        remove_snapshot_meta(snapdir);
        return false;
    }

    SnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.kind,            sizeof(meta.kind),            "%s", prefix);
    snprintf(meta.type,            sizeof(meta.type),            "%s", type);
    snprintf(meta.name,            sizeof(meta.name),            "%s", snapname);
    snprintf(meta.timestamp,       sizeof(meta.timestamp),       "%s", ts);
    snprintf(meta.origin_mount,    sizeof(meta.origin_mount),    "%s", mountpoint);
    snprintf(meta.origin_source,   sizeof(meta.origin_source),   "%s", source);
    snprintf(meta.origin_vg,       sizeof(meta.origin_vg),       "%s", info.vg);
    snprintf(meta.origin_lv,       sizeof(meta.origin_lv),       "%s", info.lv);
    snprintf(meta.origin_lvpath,   sizeof(meta.origin_lvpath),   "%s", info.lvpath);
    snprintf(meta.snapshot_lvpath, sizeof(meta.snapshot_lvpath),
             "/dev/%s/%s", info.vg, snapname);

    if (!write_snapshot_meta(snapdir, &meta)) {
        printf("Error: snapshot was created, but metadata could not be written.\n");
        return false;
    }
    printf("Created (%s): %s\n", type, snapname);
    return true;
}

static bool initramfs_has_lvm_hook(void)
{
    char kver[STR_BUF] = "";
    {
        CmdResult r = run_args("uname", "-r", NULL);
        if (r.rc != 0) return false;
        trim(r.out);
        snprintf(kver, sizeof(kver), "%.*s", (int)(sizeof(kver) - 1), r.out);
    }

    char initrd[PATH_BUF];
    snprintf(initrd, sizeof(initrd), "/boot/initrd.img-%s", kver);

    CmdResult r = run_args("lsinitramfs", initrd, NULL);
    if (r.rc != 0) return false;
    return strstr(r.out, "lvm") != NULL;
}

static bool rollback_home(const SnapshotMeta *meta)
{
    run_args_silent("fuser", "-km", "/home", NULL);
    sleep(1);
    sync();

    if (umount(HOME_SOURCE) != 0)
        run_args_silent("umount", "-l", "/home", NULL);
    sleep(1);

    CmdResult ck = run_args("fuser", "/home", NULL);
    if (ck.rc == 0 && ck.out[0] != '\0') {
        printf("Rollback failed: processes still hold /home open.\n");
        run_args_silent("mount", "/home", NULL);
        return false;
    }

    sync();
    run_args("lvchange", "-an", meta->origin_lvpath, NULL);
    CmdResult r = run_args("lvconvert", "--merge", meta->snapshot_lvpath, NULL);
    if (r.rc != 0) {
        printf("Rollback failed: %s\n", r.out);
        run_args("lvchange", "-ay", meta->origin_lvpath, NULL);
        run_args_silent("mount", "/home", NULL);
        return false;
    }

    run_args("lvchange", "-ay", meta->origin_lvpath, NULL);
    run_args_silent("mount", "/home", NULL);
    printf("SUCCESS: Home rollback complete. REBOOT may be required if the merge is pending.\n");
    return true;
}

static bool rollback_root(const SnapshotMeta *meta)
{
    if (!initramfs_has_lvm_hook()) {
        printf("Warning: LVM hook not detected in initramfs.\n");
        printf("The merge is scheduled but may not apply on reboot.\n");
        printf("Run: update-initramfs -u   then reboot.\n");
    }
    CmdResult r = run_args("lvconvert", "--merge", meta->snapshot_lvpath, NULL);
    if (r.rc != 0) {
        printf("Rollback failed: %s\n", r.out);
        return false;
    }
    printf("SUCCESS: Root rollback scheduled. REBOOT REQUIRED.\n");
    return true;
}

static bool rollback_snapshot(const char *name)
{
    char dir[PATH_BUF];
    if (!find_snapshot(name, dir, sizeof(dir))) {
        printf("Error: Snapshot '%s' not found.\n", name);
        return false;
    }
    SnapshotMeta meta;
    if (!read_snapshot_meta(dir, &meta)) {
        printf("Error: Snapshot metadata could not be read.\n");
        return false;
    }
    if (strcmp(meta.kind, "root") == 0) return rollback_root(&meta);
    if (strcmp(meta.kind, "home") == 0) return rollback_home(&meta);
    printf("Error: invalid snapshot kind.\n");
    return false;
}

static void list_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    char *roots[256], *homes[256];
    int nr = 0, nh = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char meta[PATH_BUF];
        snprintf(meta, sizeof(meta), "%s/%s/%s", dir, e->d_name, META_FILE);
        if (access(meta, F_OK) != 0) continue;
        if (strncmp(e->d_name, "root-", 5) == 0) {
            if (nr < 256) roots[nr++] = strdup(e->d_name);
        } else if (strncmp(e->d_name, "home-", 5) == 0) {
            if (nh < 256) homes[nh++] = strdup(e->d_name);
        }
    }
    closedir(d);
    printf("  Snapshots:\n");
    if (!nr && !nh) printf("    (none)\n");
    for (int i = 0; i < nr; i++) { printf("    %s\n", roots[i]); free(roots[i]); }
    for (int i = 0; i < nh; i++) { printf("    %s\n", homes[i]); free(homes[i]); }
}

static void list_snapshots(void)
{
    printf("\nManual Snapshots\n");    list_dir(MANUAL_DIR);
    printf("\napt Snapshots\n");       list_dir(AUTO_DIR);
    printf("\nnsm Daemon Snapshots\n"); list_dir(DAEMON_AUTO_DIR);
    printf("\n");
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void drop_oldest(const char *dir, char **arr, int count)
{
    if (count <= 6) return;
    char path[PATH_BUF], metapath[PATH_BUF];
    snprintf(path,     sizeof(path),     "%s/%s", dir, arr[0]);
    snprintf(metapath, sizeof(metapath), "%s/%s", path, META_FILE);
    SnapshotMeta sm;
    if (read_snapshot_meta(path, &sm))
        delete_snapshot_lv(sm.snapshot_lvpath);
    unlink(metapath);
    rmdir(path);
}

static void cleanup_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    char *roots[256], *homes[256];
    int nr = 0, nh = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char meta[PATH_BUF];
        snprintf(meta, sizeof(meta), "%s/%s/%s", dir, e->d_name, META_FILE);
        if (access(meta, F_OK) != 0) continue;
        if (strncmp(e->d_name, "root-", 5) == 0) {
            if (nr < 256) roots[nr++] = strdup(e->d_name);
        } else if (strncmp(e->d_name, "home-", 5) == 0) {
            if (nh < 256) homes[nh++] = strdup(e->d_name);
        }
    }
    closedir(d);

    qsort(roots, nr, sizeof(char *), cmp_str);
    qsort(homes, nh, sizeof(char *), cmp_str);

    drop_oldest(dir, roots, nr);
    drop_oldest(dir, homes, nh);

    for (int i = 0; i < nr; i++) free(roots[i]);
    for (int i = 0; i < nh; i++) free(homes[i]);
}

static void autodel_snapshots(void)
{
    cleanup_dir(AUTO_DIR);
    cleanup_dir(DAEMON_AUTO_DIR);
    cleanup_dir(MANUAL_DIR);
}

static bool get_lv_pool(const char *lvpath, char *pool, size_t sz)
{
    CmdResult r = run_args("lvs", "-o", "pool_lv", "--noheadings", lvpath, NULL);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    snprintf(pool, sz, "%s", r.out);
    return true;
}

static double get_thinpool_usage(const char *vg, const char *pool)
{
    char target[PATH_BUF];
    snprintf(target, sizeof(target), "%s/%s", vg, pool);
    CmdResult r = run_args("lvs", "-o", "data_percent", "--noheadings", target, NULL);
    if (r.rc != 0) return 0.0;
    trim(r.out);
    if (r.out[0] == '\0') return 0.0;
    return atof(r.out);
}

static int gather_snapshots(const char *dir, SnapSizeInfo *list, int count, int max)
{
    DIR *d = opendir(dir);
    if (!d) return count;
    struct dirent *e;
    while ((e = readdir(d)) && count < max) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        SnapshotMeta meta;
        if (read_snapshot_meta(path, &meta)) {
            snprintf(list[count].name, sizeof(list[count].name), "%s", e->d_name);
            snprintf(list[count].dir, sizeof(list[count].dir), "%s", path);
            snprintf(list[count].lvpath, sizeof(list[count].lvpath), "%s", meta.snapshot_lvpath);
            CmdResult r = run_args("lvs", "-o", "lv_size", "--noheadings", "--units", "b", meta.snapshot_lvpath, NULL);
            if (r.rc == 0) {
                trim(r.out);
                list[count].size = strtoull(r.out, NULL, 10);
            } else {
                list[count].size = 0;
            }
            count++;
        }
    }
    closedir(d);
    return count;
}

static int cmp_snap_size(const void *a, const void *b)
{
    unsigned long long sa = ((const SnapSizeInfo *)a)->size;
    unsigned long long sb = ((const SnapSizeInfo *)b)->size;
    if (sa < sb) return 1;
    if (sa > sb) return -1;
    return 0;
}

static void monitor_thinpool(const char *mountpoint)
{
    char source[PATH_BUF];
    if (!read_mount_field(mountpoint, "SOURCE", source, sizeof(source))) return;
    LvmInfo info;
    if (!resolve_lvm_info(source, &info)) return;
    if (!info.is_thin) return;

    char pool[STR_BUF];
    if (!get_lv_pool(info.lvpath, pool, sizeof(pool))) return;

    double usage = get_thinpool_usage(info.vg, pool);
    if (usage >= 93.0) {
        printf("Warning: Thin pool %s/%s usage is %.2f%% (threshold 93%% passed).\n", info.vg, pool, usage);
        autodel_snapshots();

        usage = get_thinpool_usage(info.vg, pool);
        if (usage >= 93.0) {
            SnapSizeInfo snaps[512];
            int count = 0;
            count = gather_snapshots(MANUAL_DIR, snaps, count, 512);
            count = gather_snapshots(AUTO_DIR, snaps, count, 512);
            count = gather_snapshots(DAEMON_AUTO_DIR, snaps, count, 512);

            qsort(snaps, count, sizeof(SnapSizeInfo), cmp_snap_size);

            for (int i = 0; i < count && usage >= 93.0; i++) {
                delete_snapshot_lv(snaps[i].lvpath);
                char metapath[PATH_BUF];
                snprintf(metapath, sizeof(metapath), "%s/%s", snaps[i].dir, META_FILE);
                unlink(metapath);
                rmdir(snaps[i].dir);
                usage = get_thinpool_usage(info.vg, pool);
            }
        }
    }
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [argument]\n\n", prog);
    printf("Commands:\n");
    printf("  create <name>    Create snapshots on XFS/ext4/LVM volumes\n");
    printf("                   Auto type if name matches 'apt', 'apt-*', or 'nsmd'\n");
    printf("  rollback <name>  Roll back to named snapshot\n");
    printf("  delete <name>    Delete a specific snapshot\n");
    printf("  list             List all snapshots\n");
    printf("  autodel          Delete oldest snapshots, keeping the latest 6\n");
}

int main(int argc, char *argv[])
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required.\n");
        return 1;
    }
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        ensure_dir("/nsm");
        ensure_dir(SNAPSHOT_BASE);
        ensure_dir(AUTO_DIR);
        ensure_dir(DAEMON_AUTO_DIR);
        ensure_dir(MANUAL_DIR);
        bool isauto = (strcmp(argv[2], "nsmd") == 0 || strncmp(argv[2], "apt", 3) == 0);
        const char *type = isauto ? "auto" : "manual";
        create_snapshot(ROOT_SOURCE, "root", type, argv[2]);
        if (isauto) autodel_snapshots();
        monitor_thinpool(ROOT_SOURCE);
        monitor_thinpool(HOME_SOURCE);

    } else if (strcmp(cmd, "rollback") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        rollback_snapshot(argv[2]);

    } else if (strcmp(cmd, "delete") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        if (delete_snapshot(argv[2])) {
            printf("Snapshot deleted successfully.\n");
        } else {
            fprintf(stderr, "Error: snapshot deletion failed.\n");
            return 1;
        }

    } else if (strcmp(cmd, "list") == 0) {
        list_snapshots();

    } else if (strcmp(cmd, "autodel") == 0) {
        autodel_snapshots();
        monitor_thinpool(ROOT_SOURCE);
        monitor_thinpool(HOME_SOURCE);

    } else {
        fprintf(stderr, "Error: unknown command '%s'\n\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
