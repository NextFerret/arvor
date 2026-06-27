#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <sys/utsname.h>

#define SNAPSHOT_BASE           "/nsm/snapshots"
#define AUTO_DIR                "/nsm/snapshots/auto"
#define DAEMON_AUTO_DIR         "/nsm/snapshots/auto/daemon"
#define MANUAL_DIR              "/nsm/snapshots/manual"
#define ROOT_SOURCE             "/"
#define STR_BUF                 512
#define PATH_BUF                1024
#define META_FILE               "meta"
#define THIN_POOL_WARN_PERCENT  80.0
#define VG_WARN_PERCENT         7.0
#define OUT_BUF                 8192

typedef struct {
    int rc;
    char out[OUT_BUF];
} CmdResult;

typedef struct {
    char vg[STR_BUF];
    char lv[STR_BUF];
    char lvpath[PATH_BUF];
    char segtype[STR_BUF];
    char pool[STR_BUF];
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

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static CmdResult exec_cmd(const char **argv)
{
    CmdResult r = {-1, ""};
    if (!argv || !argv[0]) return r;
    int pipefd[2];
    if (pipe(pipefd) == -1) return r;
    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return r;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        size_t outlen = strlen(r.out);
        if (outlen + (size_t)n < sizeof(r.out) - 1) {
            memcpy(r.out + outlen, buf, n);
            r.out[outlen + n] = '\0';
        }
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    r.rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
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
    const char *argv[] = {"findmnt", "-no", field, "--target", target, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    snprintf(out, sz, "%s", r.out);
    return true;
}

static bool resolve_lvm_info(const char *source, LvmInfo *info)
{
    memset(info, 0, sizeof(*info));
    const char *argv[] = {"lvs", "--noheadings", "--separator", "|", "-o",
                          "vg_name,lv_name,segtype,pool_lv", source, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    char vg[STR_BUF] = "", lv[STR_BUF] = "", seg[STR_BUF] = "", pool[STR_BUF] = "";
    if (sscanf(r.out, "%511[^|]|%511[^|]|%511[^|]|%511s", vg, lv, seg, pool) < 3) return false;
    trim(vg);
    trim(lv);
    trim(seg);
    trim(pool);
    if (vg[0] == '\0' || lv[0] == '\0' || seg[0] == '\0') return false;
    snprintf(info->vg, sizeof(info->vg), "%s", vg);
    snprintf(info->lv, sizeof(info->lv), "%s", lv);
    snprintf(info->segtype, sizeof(info->segtype), "%s", seg);
    snprintf(info->pool, sizeof(info->pool), "%s", pool);
    snprintf(info->lvpath, sizeof(info->lvpath), "/dev/%s/%s", vg, lv);
    info->is_thin = strstr(seg, "thin") != NULL;
    return true;
}

static bool snapshot_mount_fstype(const char *mountpoint)
{
    char fstype[64];
    if (!read_mount_field(mountpoint, "FSTYPE", fstype, sizeof(fstype))) return false;
    return strcmp(fstype, "xfs") == 0 || strcmp(fstype, "ext4") == 0
        || strcmp(fstype, "ext3") == 0 || strcmp(fstype, "btrfs") == 0;
}

static bool freeze_mount(const char *mountpoint)
{
    const char *argv[] = {"fsfreeze", "-f", mountpoint, NULL};
    return exec_cmd(argv).rc == 0;
}

static void unfreeze_mount(const char *mountpoint)
{
    const char *argv[] = {"fsfreeze", "-u", mountpoint, NULL};
    exec_cmd(argv);
}

static bool write_snapshot_meta(const char *dir, const SnapshotMeta *meta)
{
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", dir, META_FILE);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "kind=%s\n", meta->kind);
    fprintf(f, "type=%s\n", meta->type);
    fprintf(f, "name=%s\n", meta->name);
    fprintf(f, "timestamp=%s\n", meta->timestamp);
    fprintf(f, "origin_mount=%s\n", meta->origin_mount);
    fprintf(f, "origin_source=%s\n", meta->origin_source);
    fprintf(f, "origin_vg=%s\n", meta->origin_vg);
    fprintf(f, "origin_lv=%s\n", meta->origin_lv);
    fprintf(f, "origin_lvpath=%s\n", meta->origin_lvpath);
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
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "kind") == 0) snprintf(meta->kind, sizeof(meta->kind), "%s", val);
        else if (strcmp(key, "type") == 0) snprintf(meta->type, sizeof(meta->type), "%s", val);
        else if (strcmp(key, "name") == 0) snprintf(meta->name, sizeof(meta->name), "%s", val);
        else if (strcmp(key, "timestamp") == 0) snprintf(meta->timestamp, sizeof(meta->timestamp), "%s", val);
        else if (strcmp(key, "origin_mount") == 0) snprintf(meta->origin_mount, sizeof(meta->origin_mount), "%s", val);
        else if (strcmp(key, "origin_source") == 0) snprintf(meta->origin_source, sizeof(meta->origin_source), "%s", val);
        else if (strcmp(key, "origin_vg") == 0) snprintf(meta->origin_vg, sizeof(meta->origin_vg), "%s", val);
        else if (strcmp(key, "origin_lv") == 0) snprintf(meta->origin_lv, sizeof(meta->origin_lv), "%s", val);
        else if (strcmp(key, "origin_lvpath") == 0) snprintf(meta->origin_lvpath, sizeof(meta->origin_lvpath), "%s", val);
        else if (strcmp(key, "snapshot_lvpath") == 0) snprintf(meta->snapshot_lvpath, sizeof(meta->snapshot_lvpath), "%s", val);
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

static bool get_thinpool_usage(const char *vg, const char *pool, double *data_pct, double *meta_pct)
{
    char lvpath[PATH_BUF];
    snprintf(lvpath, sizeof(lvpath), "/dev/%s/%s", vg, pool);
    const char *argv[] = {"lvs", "--noheadings", "--nosuffix", "-o",
                          "data_percent,meta_percent", lvpath, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return false;
    trim(r.out);
    *data_pct = 0.0;
    *meta_pct = 0.0;
    if (sscanf(r.out, "%lf %lf", data_pct, meta_pct) != 2) return false;
    return true;
}

static bool create_snapshot(const char *mountpoint, const char *prefix, const char *type, const char *name)
{
    char source[PATH_BUF];
    if (!read_mount_field(mountpoint, "SOURCE", source, sizeof(source))) {
        printf("Error: cannot resolve source for %s.\n", mountpoint);
        return false;
    }
    if (!snapshot_mount_fstype(mountpoint)) {
        printf("Error: %s filesystem is not supported (xfs/ext4/ext3/btrfs required).\n", mountpoint);
        return false;
    }
    LvmInfo info;
    if (!resolve_lvm_info(source, &info)) {
        printf("Error: %s is not an LVM volume or cannot be resolved.\n", mountpoint);
        return false;
    }
    if (info.is_thin && info.pool[0] != '\0') {
        double data_pct = 0.0, meta_pct = 0.0;
        if (get_thinpool_usage(info.vg, info.pool, &data_pct, &meta_pct)) {
            if (data_pct >= THIN_POOL_WARN_PERCENT || meta_pct >= THIN_POOL_WARN_PERCENT) {
                printf("Warning: thinpool %s usage high (data: %.1f%%, meta: %.1f%%).\n", info.pool, data_pct, meta_pct);
                printf("Snapshot may fail if pool is full.\n");
            }
        }
    }
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", tm_info);
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
    bool frozen = freeze_mount(mountpoint);
    CmdResult r;
    if (info.is_thin) {
        const char *argv[] = {"lvcreate", "-s", "-n", snapname, info.lvpath, NULL};
        r = exec_cmd(argv);
    } else {
        const char *argv[] = {"lvcreate", "-s", "-n", snapname, "-l", "50%ORIGIN", info.lvpath, NULL};
        r = exec_cmd(argv);
    }
    if (frozen) unfreeze_mount(mountpoint);
    if (r.rc != 0) {
        printf("%s\n", r.out);
        remove_snapshot_meta(snapdir);
        return false;
    }
    SnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.kind, sizeof(meta.kind), "%s", prefix);
    snprintf(meta.type, sizeof(meta.type), "%s", type);
    snprintf(meta.name, sizeof(meta.name), "%s", snapname);
    snprintf(meta.timestamp, sizeof(meta.timestamp), "%s", ts);
    snprintf(meta.origin_mount, sizeof(meta.origin_mount), "%s", mountpoint);
    snprintf(meta.origin_source, sizeof(meta.origin_source), "%s", source);
    snprintf(meta.origin_vg, sizeof(meta.origin_vg), "%s", info.vg);
    snprintf(meta.origin_lv, sizeof(meta.origin_lv), "%s", info.lv);
    snprintf(meta.origin_lvpath, sizeof(meta.origin_lvpath), "%s", info.lvpath);
    snprintf(meta.snapshot_lvpath, sizeof(meta.snapshot_lvpath), "/dev/%s/%s", info.vg, snapname);
    if (!write_snapshot_meta(snapdir, &meta)) {
        printf("Error: snapshot was created, but metadata could not be written.\n");
        return false;
    }
    printf("Created (%s): %s\n", type, snapname);
    return true;
}

static bool delete_snapshot_lv(const char *snapshot_lvpath)
{
    const char *argv[] = {"lvremove", "-fy", snapshot_lvpath, NULL};
    return exec_cmd(argv).rc == 0;
}

static bool initramfs_has_lvm_hook(void)
{
    struct utsname uts;
    if (uname(&uts) != 0) return false;
    char initrd[PATH_BUF];
    snprintf(initrd, sizeof(initrd), "/boot/initrd.img-%s", uts.release);
    const char *argv[] = {"lsinitramfs", initrd, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return false;
    char *line = strtok(r.out, "\n");
    while (line) {
        if (strstr(line, "lvm") != NULL) return true;
        line = strtok(NULL, "\n");
    }
    return false;
}

static bool rollback_root(const SnapshotMeta *meta)
{
    if (!initramfs_has_lvm_hook()) {
        printf("Warning: LVM hook not detected in initramfs.\n");
        printf("The merge is scheduled but may not apply on reboot.\n");
        printf("Run: update-initramfs -u   then reboot.\n");
    }
    const char *argv[] = {"lvconvert", "--merge", meta->snapshot_lvpath, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) {
        printf("Rollback failed: %s\n", r.out);
        return false;
    }
    printf("SUCCESS: Root rollback scheduled. REBOOT REQUIRED.\n");
    return true;
}

static bool find_snapshot(const char *name, char *dir_out, size_t sz)
{
    const char *dirs[] = {MANUAL_DIR, AUTO_DIR, DAEMON_AUTO_DIR};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        char meta[PATH_BUF];
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
    if (!find_snapshot(name, dir, sizeof(dir))) {
        printf("Error: Snapshot '%s' not found.\n", name);
        return false;
    }
    SnapshotMeta meta;
    if (!read_snapshot_meta(dir, &meta)) {
        printf("Error: Snapshot metadata could not be read.\n");
        return false;
    }
    if (!delete_snapshot_lv(meta.snapshot_lvpath)) {
        printf("Error: failed to remove LV %s.\n", meta.snapshot_lvpath);
        return false;
    }
    char metapath[PATH_BUF];
    snprintf(metapath, sizeof(metapath), "%s/%s", dir, META_FILE);
    unlink(metapath);
    rmdir(dir);
    printf("Deleted: %s\n", name);
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
    printf("Error: invalid snapshot kind.\n");
    return false;
}

static void list_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    char *roots[256];
    int nr = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char meta[PATH_BUF];
        snprintf(meta, sizeof(meta), "%s/%s/%s", dir, e->d_name, META_FILE);
        if (access(meta, F_OK) != 0) continue;
        if (strncmp(e->d_name, "root-", 5) == 0) {
            if (nr < 256) roots[nr++] = strdup(e->d_name);
        }
    }
    closedir(d);
    printf("  [Root]\n");
    if (!nr) printf("    (none)\n");
    for (int i = 0; i < nr; i++) {
        printf("    %s\n", roots[i]);
        free(roots[i]);
    }
}

static void list_snapshots(void)
{
    printf("\nManual Snapshots\n");
    list_dir(MANUAL_DIR);
    printf("\napt Snapshots\n");
    list_dir(AUTO_DIR);
    printf("\nnsm Daemon Snapshots\n");
    list_dir(DAEMON_AUTO_DIR);
    printf("\n");
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void delete_subvol(const char *path)
{
    delete_snapshot_lv(path);
}

static double vgfree_percent(const char *vg)
{
    const char *argv[] = {"vgs", "--noheadings", "--nosuffix", "--units", "b",
                          "-o", "vg_free_count,vg_extent_count", vg, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return 100.0;
    trim(r.out);
    unsigned long long free_ext = 0, total_ext = 0;
    if (sscanf(r.out, "%llu %llu", &free_ext, &total_ext) != 2 || total_ext == 0)
        return 100.0;
    return 100.0 * (double)free_ext / (double)total_ext;
}

static char *detect_vg(void)
{
    const char *argv[] = {"vgs", "--noheadings", "-o", "vg_name", NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc == 0) {
        trim(r.out);
        char *nl = strchr(r.out, '\n');
        if (nl) *nl = '\0';
        if (r.out[0]) return strdup(r.out);
    }
    return NULL;
}

static bool detect_thinpool(const char *vg, char *pool_out, size_t sz)
{
    char vgpath[PATH_BUF];
    snprintf(vgpath, sizeof(vgpath), "/dev/%s", vg);
    const char *argv[] = {"lvs", "--noheadings", "-o", "lv_name,segtype", vgpath, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return false;
    char *line = strtok(r.out, "\n");
    while (line) {
        char name[STR_BUF], segtype[STR_BUF];
        if (sscanf(line, "%511s %511s", name, segtype) == 2) {
            trim(name);
            trim(segtype);
            if (strcmp(segtype, "thin-pool") == 0) {
                snprintf(pool_out, sz, "%s", name);
                return true;
            }
        }
        line = strtok(NULL, "\n");
    }
    return false;
}

typedef struct {
    char dir[PATH_BUF];
    char name[STR_BUF];
    char lvpath[PATH_BUF];
    unsigned long long size_bytes;
} SnapEntry;

static int cmp_snap_size_desc(const void *a, const void *b)
{
    const SnapEntry *sa = (const SnapEntry *)a;
    const SnapEntry *sb = (const SnapEntry *)b;
    if (sb->size_bytes > sa->size_bytes) return 1;
    if (sb->size_bytes < sa->size_bytes) return -1;
    return 0;
}

static unsigned long long lv_size_bytes(const char *lvpath)
{
    const char *argv[] = {"lvs", "--noheadings", "--nosuffix", "--units", "b", "-o", "lv_size", lvpath, NULL};
    CmdResult r = exec_cmd(argv);
    if (r.rc != 0) return 0;
    trim(r.out);
    unsigned long long sz = 0;
    sscanf(r.out, "%llu", &sz);
    return sz;
}

static void collect_snapshots(const char *dir, SnapEntry *entries, int *count, int max)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *count < max) {
        if (e->d_name[0] == '.') continue;
        char mpath[PATH_BUF];
        snprintf(mpath, sizeof(mpath), "%s/%s/%s", dir, e->d_name, META_FILE);
        if (access(mpath, F_OK) != 0) continue;
        char snap_dir[PATH_BUF];
        snprintf(snap_dir, sizeof(snap_dir), "%s/%s", dir, e->d_name);
        SnapshotMeta sm;
        if (!read_snapshot_meta(snap_dir, &sm)) continue;
        SnapEntry *se = &entries[*count];
        snprintf(se->dir, sizeof(se->dir), "%s", snap_dir);
        snprintf(se->name, sizeof(se->name), "%s", e->d_name);
        snprintf(se->lvpath, sizeof(se->lvpath), "%s", sm.snapshot_lvpath);
        se->size_bytes = lv_size_bytes(sm.snapshot_lvpath);
        (*count)++;
    }
    closedir(d);
}

static void delete_snap_entry(const SnapEntry *se)
{
    char mpath[PATH_BUF];
    snprintf(mpath, sizeof(mpath), "%s/%s", se->dir, META_FILE);
    delete_subvol(se->lvpath);
    unlink(mpath);
    rmdir(se->dir);
}

static void cleanup_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    char *roots[256];
    int nr = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char meta[PATH_BUF];
        snprintf(meta, sizeof(meta), "%s/%s/%s", dir, e->d_name, META_FILE);
        if (access(meta, F_OK) != 0) continue;
        if (strncmp(e->d_name, "root-", 5) == 0) {
            if (nr < 256) roots[nr++] = strdup(e->d_name);
        }
    }
    closedir(d);
    qsort(roots, nr, sizeof(char *), cmp_str);
    while (nr > 6) {
        char path[PATH_BUF], meta[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, roots[0]);
        snprintf(meta, sizeof(meta), "%s/%s", path, META_FILE);
        SnapshotMeta sm;
        if (read_snapshot_meta(path, &sm)) {
            delete_subvol(sm.snapshot_lvpath);
        }
        unlink(meta);
        rmdir(path);
        free(roots[0]);
        memmove(roots, roots + 1, (nr - 1) * sizeof(char *));
        nr--;
    }
    for (int i = 0; i < nr; i++) free(roots[i]);
}

static void cleanup_by_size(const char *vg, const char *pool, bool is_thin)
{
    SnapEntry entries[768];
    int count = 0;
    collect_snapshots(AUTO_DIR, entries, &count, 768);
    collect_snapshots(DAEMON_AUTO_DIR, entries, &count, 768);
    collect_snapshots(MANUAL_DIR, entries, &count, 768);
    if (count == 0) return;
    qsort(entries, count, sizeof(SnapEntry), cmp_snap_size_desc);
    for (int i = 0; i < count; i++) {
        printf("Freeing space: deleting %s (%llu bytes)\n", entries[i].name, entries[i].size_bytes);
        delete_snap_entry(&entries[i]);
        if (is_thin && pool[0] != '\0') {
            double data_pct = 0.0, meta_pct = 0.0;
            if (get_thinpool_usage(vg, pool, &data_pct, &meta_pct)) {
                if (data_pct < THIN_POOL_WARN_PERCENT && meta_pct < THIN_POOL_WARN_PERCENT)
                    break;
            }
        } else {
            if (vgfree_percent(vg) >= VG_WARN_PERCENT)
                break;
        }
    }
}

static void autodel_snapshots(void)
{
    char *vg = detect_vg();
    char pool[STR_BUF] = "";
    bool thin = false;
    if (vg) {
        thin = detect_thinpool(vg, pool, sizeof(pool));
        if (thin && pool[0] != '\0') {
            double data_pct = 0.0, meta_pct = 0.0;
            if (get_thinpool_usage(vg, pool, &data_pct, &meta_pct)) {
                if (data_pct >= THIN_POOL_WARN_PERCENT || meta_pct >= THIN_POOL_WARN_PERCENT) {
                    printf("Thinpool %s usage high (data: %.1f%%, meta: %.1f%%), deleting heaviest snapshots...\n",
                           pool, data_pct, meta_pct);
                    cleanup_by_size(vg, pool, true);
                    free(vg);
                    return;
                }
            }
        } else if (vgfree_percent(vg) < VG_WARN_PERCENT) {
            printf("VG free space below %.0f%%, deleting heaviest snapshots...\n", VG_WARN_PERCENT);
            cleanup_by_size(vg, pool, false);
            free(vg);
            return;
        }
        free(vg);
    }
    cleanup_dir(AUTO_DIR);
    cleanup_dir(DAEMON_AUTO_DIR);
    cleanup_dir(MANUAL_DIR);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [argument]\n\n", prog);
    printf("Commands:\n");
    printf("  create <name>    Create root snapshot on XFS/ext4/LVM volumes\n");
    printf("                   Auto type if name matches 'apt', 'apt-*', or 'nsmd'\n");
    printf("  delete <name>    Delete a snapshot by name\n");
    printf("  rollback <name>  Roll back to named snapshot\n");
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
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        ensure_dir("/nsm");
        ensure_dir(SNAPSHOT_BASE);
        ensure_dir(AUTO_DIR);
        ensure_dir(DAEMON_AUTO_DIR);
        ensure_dir(MANUAL_DIR);
        bool isauto = false;
        if (strcmp(argv[2], "nsmd") == 0) {
            isauto = true;
        } else if (strncmp(argv[2], "apt", 3) == 0) {
            isauto = true;
        }
        const char *type = isauto ? "auto" : "manual";
        create_snapshot(ROOT_SOURCE, "root", type, argv[2]);
        if (isauto) autodel_snapshots();
    } else if (strcmp(cmd, "delete") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        return delete_snapshot(argv[2]) ? 0 : 1;
    } else if (strcmp(cmd, "rollback") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        rollback_snapshot(argv[2]);
    } else if (strcmp(cmd, "list") == 0) {
        list_snapshots();
    } else if (strcmp(cmd, "autodel") == 0) {
        autodel_snapshots();
    } else {
        fprintf(stderr, "Error: unknown command '%s'\n\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
