#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE_DIR "/nsm/chroots"
#define VERSION "2.0"

typedef struct {
    int create;
    int enter;
    int delete_mode;
    int list;
    int version;
    int root;
    char *name;
    char *distro;
    char *release;
    char *exec_cmd;
    char *url;
    char *fs_type;
    char *size;
    int moo;
} Options;

static Options *g_active_options = NULL;

static void free_options(Options *o) {
    if (!o) return;
    free(o->name);
    free(o->distro);
    free(o->release);
    free(o->exec_cmd);
    free(o->url);
    free(o->fs_type);
    free(o->size);
    memset(o, 0, sizeof(*o));
}

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    if (g_active_options) {
        free_options(g_active_options);
    }
    exit(1);
}

static void exec_abs(const char *binary, const char *arg0, ...) {
    char *args[64];
    int n = 0;
    args[n++] = (char *)arg0;
    va_list ap;
    va_start(ap, arg0);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < 63) {
        args[n++] = (char *)a;
    }
    va_end(ap);
    args[n] = NULL;

    if (binary[0] == '/') {
        execv(binary, args);
    } else {
        const char *paths[] = {"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
        char fullpath[4096];
        for (size_t i = 0; i < 4; i++) {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", paths[i], binary);
            execv(fullpath, args);
        }
    }
    _exit(127);
}

static char *dup_string(const char *value) {
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) {
        fail("Out of memory");
    }
    memcpy(copy, value, len);
    return copy;
}

static const char *host_user(void) {
    const char *user = getenv("SUDO_USER");
    if (!user || !user[0]) {
        user = getenv("USER");
    }
    if (!user || !user[0]) {
        fail("Unable to detect host user");
    }
    return user;
}

static const char *host_home(void) {
    struct passwd *pw = getpwnam(host_user());
    if (!pw || !pw->pw_dir) {
        fail("Unable to detect host home");
    }
    return pw->pw_dir;
}

static void mkdir_p(const char *path) {
    char tmp[4096];
    size_t len;
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) {
        return;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static int is_url(const char *value) {
    if (!value) {
        return 0;
    }
    return strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0 || strncmp(value, "ftp://", 6) == 0;
}

static const char *mirror_for_distro(const char *distro) {
    if (!distro || strcmp(distro, "debian") == 0) {
        return "http://deb.debian.org/debian";
    }
    if (strcmp(distro, "ubuntu") == 0) {
        return "http://archive.ubuntu.com/ubuntu";
    }
    if (strcmp(distro, "kali") == 0) {
        return "http://http.kali.org/kali";
    }
    if (strcmp(distro, "devuan") == 0) {
        return "http://deb.devuan.org/merged";
    }
    if (strcmp(distro, "parrot") == 0) {
        return "http://deb.parrot.sh/parrot";
    }
    return NULL;
}

static int tar_supports(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return 0;
    }
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int archive_source(const char *value) {
    if (!value) {
        return 0;
    }
    if (is_url(value)) {
        return 1;
    }
    return tar_supports(value, ".tar.gz") || tar_supports(value, ".tgz") ||
           tar_supports(value, ".tar.xz") || tar_supports(value, ".txz") ||
           tar_supports(value, ".tar.bz2") || tar_supports(value, ".tbz2") ||
           tar_supports(value, ".tar");
}

static void ensure_base_dirs(void) {
    mkdir_p(BASE_DIR);
}

static void copy_file(const char *src, const char *dst) {
    pid_t pid = fork();
    if (pid == 0) {
        exec_abs("cp", "cp", src, dst, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Failed to copy file");
    }
}

static void mount_chroot(const char *path) {
    const char *mounts[] = {"/proc", "/sys", "/dev", "/dev/pts", "/tmp", "/dev/dri"};
    char target[4096];
    struct stat st;
    int i;

    for (i = 0; i < 6; i++) {
        if (stat(mounts[i], &st) == 0) {
            snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
            mkdir_p(target);

            pid_t pid = fork();
            if (pid == 0) {
                exec_abs("mountpoint", "mountpoint", "-q", target, NULL);
                _exit(127);
            }
            int status;
            waitpid(pid, &status, 0);

            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                pid = fork();
                if (pid == 0) {
                    exec_abs("mount", "mount", "--bind", mounts[i], target, NULL);
                    _exit(127);
                }
                waitpid(pid, NULL, 0);
            }
        }
    }

    if (stat("/tmp/.X11-unix", &st) == 0) {
        snprintf(target, sizeof(target), "%s/tmp/.X11-unix", path);
        mkdir_p(target);

        pid_t pid = fork();
        if (pid == 0) {
            exec_abs("mountpoint", "mountpoint", "-q", target, NULL);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            pid = fork();
            if (pid == 0) {
                exec_abs("mount", "mount", "--bind", "/tmp/.X11-unix", target, NULL);
                _exit(127);
            }
            waitpid(pid, NULL, 0);
        }
    }

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", path);
    pid_t pid = fork();
    if (pid == 0) {
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        exec_abs("chmod", "chmod", "1777", tmp_path, NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

static void umount_chroot(const char *path) {
    const char *mounts[] = {"/dev/dri", "/tmp/.X11-unix", "/tmp", "/dev/pts", "/dev", "/sys", "/proc"};
    char target[4096];
    int i;

    for (i = 0; i < 7; i++) {
        snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
        pid_t pid = fork();
        if (pid == 0) {
            exec_abs("mountpoint", "mountpoint", "-q", target, NULL);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            pid = fork();
            if (pid == 0) {
                FILE *devnull = fopen("/dev/null", "w");
                if (devnull) {
                    dup2(fileno(devnull), STDERR_FILENO);
                    fclose(devnull);
                }
                exec_abs("umount", "umount", "-l", target, NULL);
                _exit(127);
            }
            waitpid(pid, NULL, 0);
        }
    }
}

static void download_archive(const char *source, const char *destination) {
    pid_t pid = fork();
    if (pid == 0) {
        exec_abs("curl", "curl", "-fsSL", "--proto", "=https", "--proto-redir", "=https", source, "-o", destination, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Failed to download archive");
    }
}

static void extract_archive(const char *archive, const char *target) {
    const char *flag = NULL;
    if (tar_supports(archive, ".tar.gz") || tar_supports(archive, ".tgz")) {
        flag = "-xzf";
    } else if (tar_supports(archive, ".tar.xz") || tar_supports(archive, ".txz")) {
        flag = "-xJf";
    } else if (tar_supports(archive, ".tar.bz2") || tar_supports(archive, ".tbz2")) {
        flag = "-xjf";
    } else if (tar_supports(archive, ".tar")) {
        flag = "-xf";
    } else {
        fail("Unsupported archive format: %s", archive);
    }

    pid_t pid = fork();
    if (pid == 0) {
        exec_abs("tar", "tar", flag, archive, "-C", target, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Failed to extract archive");
    }
}

static void install_chroot_from_archive(const char *source, const char *target) {
    char archive[4096];

    mkdir_p(target);

    if (is_url(source)) {
        snprintf(archive, sizeof(archive), "/tmp/nlc-%d-download", getpid());
        if (tar_supports(source, ".tar.gz")) {
            strncat(archive, ".tar.gz", sizeof(archive) - strlen(archive) - 1);
        } else if (tar_supports(source, ".tgz")) {
            strncat(archive, ".tgz", sizeof(archive) - strlen(archive) - 1);
        } else if (tar_supports(source, ".tar.xz")) {
            strncat(archive, ".tar.xz", sizeof(archive) - strlen(archive) - 1);
        } else if (tar_supports(source, ".txz")) {
            strncat(archive, ".txz", sizeof(archive) - strlen(archive) - 1);
        } else if (tar_supports(source, ".tar.bz2")) {
            strncat(archive, ".tar.bz2", sizeof(archive) - strlen(archive) - 1);
        } else if (tar_supports(source, ".tbz2")) {
            strncat(archive, ".tbz2", sizeof(archive) - strlen(archive) - 1);
        } else {
            strncat(archive, ".tar", sizeof(archive) - strlen(archive) - 1);
        }
        download_archive(source, archive);
        
        pid_t pid = fork();
        if (pid == 0) {
            const char *flag = (tar_supports(archive, ".tar.gz") || tar_supports(archive, ".tgz")) ? "-xzf" :
                               (tar_supports(archive, ".tar.xz") || tar_supports(archive, ".txz")) ? "-xJf" :
                               (tar_supports(archive, ".tar.bz2") || tar_supports(archive, ".tbz2")) ? "-xjf" : "-xf";
            exec_abs("tar", "tar", flag, archive, "-C", target, NULL);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        unlink(archive);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fail("Failed to extract archive");
        }
    } else {
        extract_archive(source, target);
    }
}

static void bootstrap_chroot(const char *distro, const char *release, const char *target) {
    const char *mirror;

    if (!release) {
        fail("Missing release. Use -r <release> when creating from distro");
    }

    mirror = mirror_for_distro(distro);
    if (!mirror) {
        fail("Unsupported distro: %s", distro);
    }

    mkdir_p(target);
    const char *arch = (sizeof(void *) == 8) ? "amd64" : "i386";

    pid_t pid = fork();
    if (pid == 0) {
        exec_abs("debootstrap", "debootstrap", "--arch", arch, "--variant=minbase", release, target, mirror, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Failed to create chroot with debootstrap");
    }
}

static void prepare_network(const char *target) {
    char resolv[4096];
    snprintf(resolv, sizeof(resolv), "%s/etc/resolv.conf", target);
    mkdir_p(target);
    copy_file("/etc/resolv.conf", resolv);
}

static void ensure_host_user_inside(const char *target) {
    const char *user = host_user();
    int status;
    pid_t pid;

    pid = fork();
    if (pid == 0) {
        exec_abs("chroot", "chroot", target, "getent", "group", user, NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        pid = fork();
        if (pid == 0) {
            exec_abs("chroot", "chroot", target, "groupadd", user, NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }

    pid = fork();
    if (pid == 0) {
        exec_abs("chroot", "chroot", target, "id", "-u", user, NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        pid = fork();
        if (pid == 0) {
            exec_abs("chroot", "chroot", target, "useradd", "-m", "-g", user, "-s", "/bin/bash", user, NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }

    pid = fork();
    if (pid == 0) {
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        exec_abs("chroot", "chroot", target, "usermod", "-aG", "sudo", user, NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    char sudoers_cmd[8192];
    snprintf(sudoers_cmd, sizeof(sudoers_cmd), "printf '%%s ALL=(ALL) NOPASSWD: ALL\\n' '%s' > /etc/sudoers.d/%s", user, user);
    pid = fork();
    if (pid == 0) {
        exec_abs("chroot", "chroot", target, "/bin/sh", "-c", sudoers_cmd, NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    char sud_file[256];
    snprintf(sud_file, sizeof(sud_file), "/etc/sudoers.d/%s", user);
    pid = fork();
    if (pid == 0) {
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        exec_abs("chroot", "chroot", target, "chmod", "0440", sud_file, NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

static void lower_string(char *value) {
    size_t i;
    for (i = 0; value[i]; i++) {
        value[i] = (char)tolower((unsigned char)value[i]);
    }
}

static char *value_after_equals(const char *arg) {
    const char *eq = strchr(arg, '=');
    if (!eq) {
        return NULL;
    }
    return dup_string(eq + 1);
}

static char *next_value(int *index, int argc, char **argv) {
    if (*index + 1 >= argc) {
        fail("Missing value for %s", argv[*index]);
    }
    *index += 1;
    return dup_string(argv[*index]);
}

static void validate_size(const char *size);

static void parse_args(int argc, char **argv, Options *options) {
    int i;
    memset(options, 0, sizeof(*options));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            options->create = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            options->enter = 1;
        } else if (strcmp(argv[i], "-del") == 0) {
            options->delete_mode = 1;
        } else if (strcmp(argv[i], "-l") == 0) {
            options->list = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            options->version = 1;
        } else if (strncmp(argv[i], "-r=", 3) == 0) {
            options->release = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-r") == 0) {
            if (options->create) {
                options->release = next_value(&i, argc, argv);
            } else {
                options->root = 1;
            }
        } else if (strncmp(argv[i], "-n=", 3) == 0) {
            options->name = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-n") == 0) {
            options->name = next_value(&i, argc, argv);
        } else if (strncmp(argv[i], "-d=", 3) == 0) {
            options->distro = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-d") == 0) {
            options->distro = next_value(&i, argc, argv);
        } else if (strncmp(argv[i], "-exec=", 6) == 0) {
            options->exec_cmd = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-exec") == 0) {
            options->exec_cmd = next_value(&i, argc, argv);
        } else if (strncmp(argv[i], "-u=", 3) == 0) {
            options->url = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-u") == 0) {
            options->url = next_value(&i, argc, argv);
        } else if (strncmp(argv[i], "-fs=", 4) == 0) {
            options->fs_type = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-fs") == 0) {
            options->fs_type = next_value(&i, argc, argv);
        } else if (strncmp(argv[i], "-s=", 3) == 0) {
            options->size = value_after_equals(argv[i]);
        } else if (strcmp(argv[i], "-s") == 0) {
            options->size = next_value(&i, argc, argv);
        } else if (strcmp(argv[i], "moo") == 0 || strcmp(argv[i], "-moo") == 0) {
            options->moo = 1;
        } else {
            fail("Unknown argument: %s", argv[i]);
        }
    }

    if (options->size) {
        validate_size(options->size);
    }
}

static void usage(const char *argv0) {
    printf("\n  nlc %s — chroot lifecycle manager\n\n", VERSION);

    printf("  Creates, enters, and removes named chroot environments\n");
    printf("  stored under %s.\n\n", BASE_DIR);

    printf("  Usage:\n\n");

    printf("    %s -c -n <name> -d <distro> -r <release>\n", argv0);
    printf("        Bootstrap a minimal chroot via debootstrap.\n");
    printf("        Supported distros: debian, ubuntu, kali, devuan, parrot\n\n");

    printf("    %s -c -n <name> -d <distro> -r <release> -fs <fstype> -s <size>\n", argv0);
    printf("        Same as above, but creates a sparse image file of the given\n");
    printf("        size, formats it with <fstype> (e.g. ext4, xfs), mounts it,\n");
    printf("        and installs the bootstrap into it.\n\n");

    printf("    %s -c -n <name> -u <archive-or-url>\n", argv0);
    printf("        Install a chroot from a local tar archive or remote URL.\n");
    printf("        Supported formats: .tar.gz  .tgz  .tar.xz  .txz  .tar.bz2  .tar\n\n");

    printf("    %s -e -n <name> [-r] [-exec <command>]\n", argv0);
    printf("        Enter a chroot as the current user (or root with -r).\n");
    printf("        Bind-mounts /proc /sys /dev /tmp and X11/Wayland sockets.\n");
    printf("        Optionally runs <command> instead of an interactive shell.\n\n");

    printf("    %s -del -n <name>\n", argv0);
    printf("        Unmount and permanently delete a chroot.\n\n");

    printf("    %s -l\n", argv0);
    printf("        List all existing chroots.\n\n");

    printf("    %s -v\n", argv0);
    printf("        Print version and exit.\n\n");
}

static void list_chroots(void) {
    DIR *dir = opendir(BASE_DIR);
    struct dirent *entry;
    int found = 0;

    if (!dir) {
        if (errno == ENOENT) {
            return;
        }
        fail("Failed to open %s", BASE_DIR);
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        printf("%s\n", entry->d_name);
        found = 1;
    }

    if (!found) {
        printf("No chroots found\n");
    }

    closedir(dir);
}

static void run_cmd(const char *prog, ...) {
    va_list ap;
    const char *args[64];
    int n = 0;
    args[n++] = prog;
    va_start(ap, prog);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < 63) {
        args[n++] = a;
    }
    va_end(ap);
    args[n] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        if (prog[0] == '/') {
            execv(prog, (char * const *)args);
        } else {
            const char *paths[] = {"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
            char fullpath[4096];
            for (size_t i = 0; i < 4; i++) {
                snprintf(fullpath, sizeof(fullpath), "%s/%s", paths[i], prog);
                execv(fullpath, (char * const *)args);
            }
        }
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Command failed: %s", prog);
    }
}

static void validate_size(const char *size) {
    if (!size || size[0] == '\0') {
        fail("Size parameter (-s) cannot be empty");
    }
    size_t len = strlen(size);
    size_t i = 0;
    while (i < len && isdigit((unsigned char)size[i])) {
        i++;
    }
    if (i == 0) {
        fail("Invalid size (-s '%s'): must start with a number", size);
    }
    if (i < len) {
        if (i + 1 == len) {
            char suffix = toupper((unsigned char)size[i]);
            if (suffix != 'K' && suffix != 'M' && suffix != 'G' && suffix != 'T') {
                fail("Invalid size suffix (-s '%s'): must be K, M, G, or T", size);
            }
        } else {
            fail("Invalid size format (-s '%s')", size);
        }
    }
}

static void setup_sparse_image(const Options *options, const char *img_path, char *mkfs_cmd, size_t mkfs_sz) {
    run_cmd("truncate", "-s", options->size, img_path, NULL);

    if (strcmp(options->fs_type, "xfs") == 0) {
        snprintf(mkfs_cmd, mkfs_sz, "mkfs.xfs");
    } else if (strcmp(options->fs_type, "ext4") == 0) {
        snprintf(mkfs_cmd, mkfs_sz, "mkfs.ext4");
    } else if (strcmp(options->fs_type, "ext3") == 0) {
        snprintf(mkfs_cmd, mkfs_sz, "mkfs.ext3");
    } else if (strcmp(options->fs_type, "ext2") == 0) {
        snprintf(mkfs_cmd, mkfs_sz, "mkfs.ext2");
    } else if (strcmp(options->fs_type, "btrfs") == 0) {
        snprintf(mkfs_cmd, mkfs_sz, "mkfs.btrfs");
    } else {
        fail("Unsupported filesystem type: %s (supported: ext2, ext3, ext4, xfs, btrfs)", options->fs_type);
    }

    run_cmd(mkfs_cmd, "-F", img_path, NULL);
}

static void setup_loop_device(const char *img_path, char *loop_dev, size_t sz) {
    int losetup_pipe[2];
    if (pipe(losetup_pipe) != 0)
        fail("Failed to create pipe for losetup");

    pid_t lpid = fork();
    if (lpid < 0) {
        close(losetup_pipe[0]);
        close(losetup_pipe[1]);
        fail("Failed to fork for losetup");
    }
    if (lpid == 0) {
        close(losetup_pipe[0]);
        dup2(losetup_pipe[1], STDOUT_FILENO);
        close(losetup_pipe[1]);
        exec_abs("losetup", "losetup", "-f", "--show", img_path, NULL);
        _exit(127);
    }
    close(losetup_pipe[1]);
    FILE *fp = fdopen(losetup_pipe[0], "r");
    if (!fp) {
        close(losetup_pipe[0]);
        fail("Failed to open pipe for losetup output");
    }
    int lstat;
    waitpid(lpid, &lstat, 0);
    if (!WIFEXITED(lstat) || WEXITSTATUS(lstat) != 0) {
        fclose(fp);
        fail("losetup failed on %s", img_path);
    }
    if (!fgets(loop_dev, sz, fp)) {
        fclose(fp);
        fail("Failed to read loop device name from losetup");
    }
    fclose(fp);

    size_t len = strlen(loop_dev);
    while (len > 0 && (loop_dev[len - 1] == '\n' || loop_dev[len - 1] == '\r')) {
        loop_dev[--len] = '\0';
    }

    if (len == 0) {
        fail("losetup returned empty device name");
    }
}

static void mount_and_bootstrap(const Options *options, const char *img_path, const char *loop_dev, const char *target) {
    (void)img_path;
    mkdir_p(target);
    run_cmd("mount", loop_dev, target, NULL);

    if (options->url) {
        install_chroot_from_archive(options->url, target);
    } else if (archive_source(options->distro)) {
        install_chroot_from_archive(options->distro, target);
    } else {
        bootstrap_chroot(options->distro, options->release, target);
    }

    run_cmd("umount", target, NULL);
    run_cmd("losetup", "-d", loop_dev, NULL);
}

static void create_sparse_chroot(const Options *options, const char *target) {
    char img_path[4096];
    char loop_dev[256];
    char mkfs_cmd[64];

    snprintf(img_path, sizeof(img_path), "%s.img", target);

    if (path_exists(img_path)) {
        fail("Image already exists: %s", img_path);
    }

    setup_sparse_image(options, img_path, mkfs_cmd, sizeof(mkfs_cmd));
    setup_loop_device(img_path, loop_dev, sizeof(loop_dev));
    mount_and_bootstrap(options, img_path, loop_dev, target);

    printf("Sparse image created at %s (%s, %s)\n", img_path, options->fs_type, options->size);
    printf("Mount with:  mount -o loop %s %s\n", img_path, target);
}

static void validate_name(const char *name) {
    if (!name || name[0] == '\0')
        fail("Chroot name must not be empty");
    if (name[0] == '.')
        fail("Invalid chroot name: '%s' must not start with '.'", name);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        fail("Invalid chroot name: '%s'", name);
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.')
            fail("Chroot name contains invalid character '%c'. Only alphanumeric, '-', '_', '.' are allowed.", *p);
    }
}

static void create_chroot(const Options *options) {
    char target[4096];

    if (!options->name) {
        fail("Missing chroot name");
    }
    validate_name(options->name);
    if (!options->distro && !options->url) {
        fail("Use -d <distro> with -r <release>, or -u <archive-or-url>");
    }
    if (options->distro && options->url) {
        fail("Use only one source: distro or archive/url");
    }
    if (options->fs_type && !options->size) {
        fail("Filesystem type (-fs) requires a size (-s <size>, e.g. 10G)");
    }
    if (options->size && !options->fs_type) {
        fail("Size (-s) requires a filesystem type (-fs <fstype>, e.g. ext4, xfs)");
    }

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (path_exists(target)) {
        fail("Chroot already exists: %s", options->name);
    }

    if (options->fs_type) {
        create_sparse_chroot(options, target);
        return;
    }

    if (options->url) {
        install_chroot_from_archive(options->url, target);
    } else if (archive_source(options->distro)) {
        install_chroot_from_archive(options->distro, target);
    } else {
        bootstrap_chroot(options->distro, options->release, target);
    }

    prepare_network(target);
    mount_chroot(target);
    ensure_host_user_inside(target);
    umount_chroot(target);
}

static void enter_chroot(const Options *options) {
    char target[4096];
    char xauth[4096];
    char chroot_xauth[4096];
    char runtime_target[4096];
    const char *user = host_user();
    const char *home = host_home();
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    int xauth_mounted = 0;
    int status;
    pid_t pid;

    if (!options->name) {
        fail("Missing chroot name");
    }
    validate_name(options->name);

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (!path_exists(target)) {
        fail("Chroot not found: %s", options->name);
    }

    if (!display || !display[0]) {
        display = ":0";
    }

    mount_chroot(target);

    char local_user[256];
    snprintf(local_user, sizeof(local_user), "+local:%s", user);
    pid = fork();
    if (pid == 0) {
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) {
            dup2(fileno(devnull), STDOUT_FILENO);
            dup2(fileno(devnull), STDERR_FILENO);
            fclose(devnull);
        }
        exec_abs("xhost", "xhost", local_user, NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);

    snprintf(xauth, sizeof(xauth), "%s/.Xauthority", home);
    if (path_exists(xauth)) {
        snprintf(chroot_xauth, sizeof(chroot_xauth), "%s/home/%s/.Xauthority", target, user);

        pid = fork();
        if (pid == 0) {
            exec_abs("touch", "touch", chroot_xauth, NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);

        pid = fork();
        if (pid == 0) {
            exec_abs("mountpoint", "mountpoint", "-q", chroot_xauth, NULL);
            _exit(127);
        }
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            pid = fork();
            if (pid == 0) {
                exec_abs("mount", "mount", "--bind", xauth, chroot_xauth, NULL);
                _exit(127);
            }
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                xauth_mounted = 1;
            }
        }
    }

    if (wayland && runtime_dir && wayland[0] && runtime_dir[0]) {
        snprintf(runtime_target, sizeof(runtime_target), "%s/tmp/%s", target, wayland);

        pid = fork();
        if (pid == 0) {
            exec_abs("touch", "touch", runtime_target, NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);

        pid = fork();
        if (pid == 0) {
            exec_abs("mountpoint", "mountpoint", "-q", runtime_target, NULL);
            _exit(127);
        }
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            char src_wayland[4096];
            snprintf(src_wayland, sizeof(src_wayland), "%s/%s", runtime_dir, wayland);
            pid = fork();
            if (pid == 0) {
                exec_abs("mount", "mount", "--bind", src_wayland, runtime_target, NULL);
                _exit(127);
            }
            waitpid(pid, NULL, 0);
        }
    }

    char xauth_env[4096];
    snprintf(xauth_env, sizeof(xauth_env), "/home/%s/.Xauthority", user);

    if (options->root) {
        pid = fork();
        if (pid == 0) {
            setenv("TERM",              "xterm",                       1);
            setenv("DISPLAY",           display,                       1);
            setenv("XDG_RUNTIME_DIR",   "/tmp",                        1);
            setenv("WAYLAND_DISPLAY",   wayland ? wayland : "",        1);
            setenv("QT_QPA_PLATFORM",   "wayland",                     1);
            setenv("XAUTHORITY",        xauth_env,                     1);
            if (options->exec_cmd) {
                exec_abs("chroot", "chroot", target, "/bin/sh", "-c", options->exec_cmd, NULL);
            } else {
                exec_abs("chroot", "chroot", target, "/bin/bash", "-l", NULL);
            }
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    } else {
        pid = fork();
        if (pid == 0) {
            setenv("TERM",              "xterm",                       1);
            setenv("DISPLAY",           display,                       1);
            setenv("XDG_RUNTIME_DIR",   "/tmp",                        1);
            setenv("WAYLAND_DISPLAY",   wayland ? wayland : "",        1);
            setenv("QT_QPA_PLATFORM",   "wayland",                     1);
            setenv("XAUTHORITY",        xauth_env,                     1);
            if (options->exec_cmd) {
                exec_abs("chroot", "chroot", target, "/bin/su", "-", user, "-c", options->exec_cmd, NULL);
            } else {
                exec_abs("chroot", "chroot", target, "/bin/su", "-", user, NULL);
            }
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }

    if (xauth_mounted) {
        char xa_path[4096];
        snprintf(xa_path, sizeof(xa_path), "%s/home/%s/.Xauthority", target, user);
        pid = fork();
        if (pid == 0) {
            FILE *devnull = fopen("/dev/null", "w");
            if (devnull) {
                dup2(fileno(devnull), STDERR_FILENO);
                fclose(devnull);
            }
            exec_abs("umount", "umount", "-l", xa_path, NULL);
            _exit(127);
        }
        waitpid(pid, NULL, 0);
    }

    umount_chroot(target);
}

static void delete_chroot(const Options *options) {
    char target[4096];
    int status;
    pid_t pid;

    if (!options->name) {
        fail("Missing chroot name");
    }
    validate_name(options->name);

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (!path_exists(target)) {
        fail("Chroot not found: %s", options->name);
    }

    umount_chroot(target);
    pid = fork();
    if (pid == 0) {
        exec_abs("rm", "rm", "-rf", target, NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fail("Failed to delete chroot");
    }
}

int main(int argc, char **argv) {
    Options options;
    memset(&options, 0, sizeof(options));
    g_active_options = &options;

    if (argc == 1) {
        usage(argv[0]);
        free_options(&options);
        return 0;
    }

    parse_args(argc, argv, &options);

    if (options.moo) {
        printf("\n         (__) \n"
               "         (oo)   chroot edition\n"
               "   /------\\/ \n"
               "  / |    ||  \n"
               " *  ||---||  \n"
               "    ^^   ^^  \n"
               "...This NLC Has Super Chroot Powers.\n\n");
        free_options(&options);
        g_active_options = NULL;
        return 0;
    }

    if (geteuid() != 0) {
        fail("Run as root");
    }

    ensure_base_dirs();

    int rc = 0;
    if (options.version) {
        printf("nlc %s\n", VERSION);
    } else if (options.list) {
        list_chroots();
    } else if (options.create) {
        create_chroot(&options);
    } else if (options.enter) {
        enter_chroot(&options);
    } else if (options.delete_mode) {
        delete_chroot(&options);
    } else {
        usage(argv[0]);
        rc = 1;
    }

    free_options(&options);
    g_active_options = NULL;
    return rc;
}
