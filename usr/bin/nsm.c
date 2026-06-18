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
#define CMD_BUF                 4096
#define STR_BUF                 512
#define PATH_BUF                1024
#define META_FILE               "meta"

typedef struct {
    int rc;
    char out[8192];
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

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static CmdResult run_cmdv(const char *fmt, va_list ap)
{
    CmdResult r = {-1, ""};
    char cmd[CMD_BUF];
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    char full[CMD_BUF];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    if (!p) return r;
    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        strncat(r.out, buf, sizeof(r.out) - strlen(r.out) - 1);
    int st = pclose(p);
    r.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

static CmdResult run_cmdf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    CmdResult r = run_cmdv(fmt, ap);
    va_end(ap);
    return r;
}

static void shell(const char *cmd)
{
    int r = system(cmd);
    (void)r;
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
    CmdResult r = run_cmdf("findmnt -no %s --target %s", field, target);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    snprintf(out, sz, "%s", r.out);
    return true;
}

static bool resolve_lvm_info(const char *source, LvmInfo *info)
{
    CmdResult r = run_cmdf("lvs --noheadings --separator / -o vg_name,lv_name,segtype %s", source);
    if (r.rc != 0) return false;
    trim(r.out);
    if (r.out[0] == '\0') return false;
    char vg[STR_BUF] = "", lv[STR_BUF] = "", seg[STR_BUF] = "";
    if (sscanf(r.out, "%511[^/]/%511[^/]/%511s", vg, lv, seg) != 3) return false;
    trim(vg);
    trim(lv);
    trim(seg);
    if (vg[0] == '\0' || lv[0] == '\0' || seg[0] == '\0') return false;
    snprintf(info->vg, sizeof(info->vg), "%s", vg);
    snprintf(info->lv, sizeof(info->lv), "%s", lv);
    snprintf(info->segtype, sizeof(info->segtype), "%s", seg);
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
    return run_cmdf("fsfreeze -f %s", mountpoint).rc == 0;
}

static void unfreeze_mount(const char *mountpoint)
{
    run_cmdf("fsfreeze -u %s", mountpoint);
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
    bool frozen = !is_root && freeze_mount(mountpoint);
    CmdResult r;
    if (info.is_thin) {
        r = run_cmdf("lvcreate -s -n %s %s", snapname, info.lvpath);
    } else {
        r = run_cmdf("lvcreate -s -n %s -l 50%%ORIGIN %s", snapname, info.lvpath);
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
    CmdResult r = run_cmdf("lvremove -fy %s", snapshot_lvpath);
    return r.rc == 0;
}

static int path_depth(const char *p)
{
    int d = 0;
    for (const char *s = p; *s; s++) if (*s == '/') d++;
    return d;
}

static int cmp_path_desc(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    int da = path_depth(pa);
    int db = path_depth(pb);
    if (da != db) return db - da;
    size_t la = strlen(pa), lb = strlen(pb);
    if (la != lb) return (lb > la) - (lb < la);
    return strcmp(pa, pb);
}

static bool rollback_home(const SnapshotMeta *meta)
{
    shell("fuser -km /home > /dev/null 2>&1");
    sleep(1);
    sync();
    if (umount(HOME_SOURCE) != 0) {
        shell("umount -l /home > /dev/null 2>&1");
        sleep(1);
    }
    CmdResult ck = run_cmdf("fuser /home 2>/dev/null");
    if (ck.rc == 0 && ck.out[0] != '\0') {
        printf("Rollback failed: processes still hold /home open.\n");
        shell("mount /home > /dev/null 2>&1");
        return false;
    }
    sync();
    run_cmdf("lvchange -an %s", meta->origin_lvpath);
    CmdResult r = run_cmdf("lvconvert --merge %s", meta->snapshot_lvpath);
    if (r.rc != 0) {
        printf("Rollback failed: %s\n", r.out);
        run_cmdf("lvchange -ay %s", meta->origin_lvpath);
        shell("mount /home > /dev/null 2>&1");
        return false;
    }
    run_cmdf("lvchange -ay %s", meta->origin_lvpath);
    shell("mount /home > /dev/null 2>&1");
    printf("SUCCESS: Home rollback complete. REBOOT may be required if the merge is pending.\n");
    return true;
}

static bool initramfs_has_lvm_hook(void)
{
    CmdResult r = run_cmdf("lsinitramfs /boot/initrd.img-$(uname -r) 2>/dev/null | grep -q lvm");
    return r.rc == 0;
}

static bool rollback_root(const SnapshotMeta *meta)
{
    if (!initramfs_has_lvm_hook()) {
        printf("Warning: LVM hook not detected in initramfs.\n");
        printf("The merge is scheduled but may not apply on reboot.\n");
        printf("Run: update-initramfs -u   then reboot.\n");
    }
    CmdResult r = run_cmdf("lvconvert --merge %s", meta->snapshot_lvpath);
    if (r.rc != 0) {
        printf("Rollback failed: %s\n", r.out);
        return false;
    }
    printf("SUCCESS: Root rollback scheduled. REBOOT REQUIRED.\n");
    return true;
}

static bool find_snapshot(const char *name, char *dir_out, size_t sz)
{
    const char *dirs[] = { MANUAL_DIR, AUTO_DIR, DAEMON_AUTO_DIR };
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
    if (nr > 6) {
        char path[PATH_BUF], meta[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, roots[0]);
        snprintf(meta, sizeof(meta), "%s/%s", path, META_FILE);
        SnapshotMeta sm;
        if (read_snapshot_meta(path, &sm)) {
            delete_subvol(sm.snapshot_lvpath);
        }
        unlink(meta);
        rmdir(path);
    }
    if (nh > 6) {
        char path[PATH_BUF], meta[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, homes[0]);
        snprintf(meta, sizeof(meta), "%s/%s", path, META_FILE);
        SnapshotMeta sm;
        if (read_snapshot_meta(path, &sm)) {
            delete_subvol(sm.snapshot_lvpath);
        }
        unlink(meta);
        rmdir(path);
    }
    for (int i = 0; i < nr; i++) free(roots[i]);
    for (int i = 0; i < nh; i++) free(homes[i]);
}

static void autodel_snapshots(void)
{
    cleanup_dir(AUTO_DIR);
    cleanup_dir(DAEMON_AUTO_DIR);
    cleanup_dir(MANUAL_DIR);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [argument]\n\n", prog);
    printf("Commands:\n");
    printf("  create <name>    Create snapshots on XFS/ext4/LVM volumes\n");
    printf("                   Auto type if name matches 'apt', 'apt-*', or 'nsmd'\n");
    printf("  rollback <name>  Roll back to named snapshot (must start with root- or home-)\n");
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
