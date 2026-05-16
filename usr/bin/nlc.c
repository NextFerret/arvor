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
#include <unistd.h>

#define BASE_DIR "/nsm/chroots"
#define VERSION "1.0.0"

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
} Options;

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

static int run_command(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Command failed: %s\n", cmd);
    }
    return rc;
}

static char *dup_string(const char *value) {
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);
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
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, dst);
    if (run_command(cmd) != 0) {
        fail("Failed to copy file");
    }
}

static void mount_chroot(const char *path) {
    const char *mounts[] = {"/proc", "/sys", "/dev", "/dev/pts", "/tmp", "/dev/dri"};
    char target[4096];
    char cmd[8192];
    struct stat st;
    int i;

    for (i = 0; i < 6; i++) {
        if (stat(mounts[i], &st) == 0) {
            snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
            mkdir_p(target);
            snprintf(cmd, sizeof(cmd), "mountpoint -q '%s' || mount --bind '%s' '%s'", target, mounts[i], target);
            run_command(cmd);
        }
    }

    if (stat("/tmp/.X11-unix", &st) == 0) {
        snprintf(target, sizeof(target), "%s/tmp/.X11-unix", path);
        mkdir_p(target);
        snprintf(cmd, sizeof(cmd), "mountpoint -q '%s' || mount --bind '/tmp/.X11-unix' '%s'", target, target);
        run_command(cmd);
    }

    snprintf(cmd, sizeof(cmd), "chmod 1777 '%s/tmp' 2>/dev/null", path);
    run_command(cmd);
}

static void umount_chroot(const char *path) {
    const char *mounts[] = {"/dev/dri", "/tmp/.X11-unix", "/tmp", "/dev/pts", "/dev", "/sys", "/proc"};
    char target[4096];
    char cmd[8192];
    int i;

    for (i = 0; i < 7; i++) {
        snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
        snprintf(cmd, sizeof(cmd), "umount -l '%s' 2>/dev/null", target);
        system(cmd);
    }
}

static void download_archive(const char *source, const char *destination) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "curl -L --fail '%s' -o '%s'", source, destination);
    if (run_command(cmd) != 0) {
        fail("Failed to download archive");
    }
}

static void extract_archive(const char *archive, const char *target) {
    char cmd[8192];

    if (tar_supports(archive, ".tar.gz") || tar_supports(archive, ".tgz")) {
        snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C '%s'", archive, target);
    } else if (tar_supports(archive, ".tar.xz") || tar_supports(archive, ".txz")) {
        snprintf(cmd, sizeof(cmd), "tar -xJf '%s' -C '%s'", archive, target);
    } else if (tar_supports(archive, ".tar.bz2") || tar_supports(archive, ".tbz2")) {
        snprintf(cmd, sizeof(cmd), "tar -xjf '%s' -C '%s'", archive, target);
    } else if (tar_supports(archive, ".tar")) {
        snprintf(cmd, sizeof(cmd), "tar -xf '%s' -C '%s'", archive, target);
    } else {
        fail("Unsupported archive format: %s", archive);
    }

    if (run_command(cmd) != 0) {
        fail("Failed to extract archive");
    }
}

static void install_chroot_from_archive(const char *source, const char *target) {
    char archive[4096];
    char cmd[8192];

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
        extract_archive(archive, target);
        snprintf(cmd, sizeof(cmd), "rm -f '%s'", archive);
        system(cmd);
    } else {
        extract_archive(source, target);
    }
}

static void bootstrap_chroot(const char *distro, const char *release, const char *target) {
    char cmd[8192];
    const char *mirror;

    if (!release) {
        fail("Missing release. Use -r <release> when creating from distro");
    }

    mirror = mirror_for_distro(distro);
    if (!mirror) {
        fail("Unsupported distro: %s", distro);
    }

    mkdir_p(target);
    snprintf(cmd, sizeof(cmd), "debootstrap --arch=%s --variant=minbase %s '%s' %s", sizeof(void *) == 8 ? "amd64" : "i386", release, target, mirror);
    if (run_command(cmd) != 0) {
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
    char cmd[8192];
    const char *user = host_user();

    snprintf(cmd, sizeof(cmd), "chroot '%s' getent group '%s' >/dev/null 2>&1 || chroot '%s' groupadd '%s'", target, user, target, user);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "chroot '%s' id -u '%s' >/dev/null 2>&1 || chroot '%s' useradd -m -g '%s' -s /bin/bash '%s'", target, user, target, user, user);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "chroot '%s' usermod -aG sudo '%s' >/dev/null 2>&1", target, user);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "chroot '%s' sh -c \"printf '%%s ALL=(ALL) NOPASSWD: ALL\\n' '%s' > /etc/sudoers.d/%s\"", target, user, user);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "chroot '%s' chmod 0440 /etc/sudoers.d/%s >/dev/null 2>&1", target, user);
    system(cmd);
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
        } else {
            fail("Unknown argument: %s", argv[i]);
        }
    }

    if (options->distro) {
        lower_string(options->distro);
    }
}

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s -c -n=<name> -d <distro> -r <release>\n", argv0);
    printf("  %s -c -n=<name> -u <archive-or-url>\n", argv0);
    printf("  %s -e -n=<name> [-r] [-exec=\"command\"]\n", argv0);
    printf("  %s -del -n=<name>\n", argv0);
    printf("  %s -l\n", argv0);
    printf("  %s -v\n", argv0);
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

static void create_chroot(const Options *options) {
    char target[4096];

    if (!options->name) {
        fail("Missing chroot name");
    }
    if (!options->distro && !options->url) {
        fail("Use -d <distro> with -r <release> or -u <archive-or-url>");
    }
    if (options->distro && options->url) {
        fail("Use only one source: distro or archive/url");
    }

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (path_exists(target)) {
        fail("Chroot already exists: %s", options->name);
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
    char cmd[16384];
    char inner[12288];
    char runtime_target[4096];
    const char *user = host_user();
    const char *home = host_home();
    const char *display = getenv("DISPLAY");
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    int xauth_mounted = 0;

    if (!options->name) {
        fail("Missing chroot name");
    }

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (!path_exists(target)) {
        fail("Chroot not found: %s", options->name);
    }

    if (!display || !display[0]) {
        display = ":0";
    }

    mount_chroot(target);

    snprintf(cmd, sizeof(cmd), "xhost +local:%s >/dev/null 2>&1", user);
    system(cmd);

    snprintf(xauth, sizeof(xauth), "%s/.Xauthority", home);
    if (path_exists(xauth)) {
        snprintf(chroot_xauth, sizeof(chroot_xauth), "%s/home/%s/.Xauthority", target, user);
        snprintf(cmd, sizeof(cmd), "touch '%s' && mount --bind '%s' '%s'", chroot_xauth, xauth, chroot_xauth);
        if (system(cmd) == 0) {
            xauth_mounted = 1;
        }
    }

    if (wayland && runtime_dir && wayland[0] && runtime_dir[0]) {
        snprintf(runtime_target, sizeof(runtime_target), "%s/tmp/%s", target, wayland);
        snprintf(cmd, sizeof(cmd), "touch '%s' && mountpoint -q '%s' || mount --bind '%s/%s' '%s'", runtime_target, runtime_target, runtime_dir, wayland, runtime_target);
        system(cmd);
    }

    snprintf(inner, sizeof(inner), "export TERM=xterm; export DISPLAY='%s'; export XDG_RUNTIME_DIR=/tmp; export WAYLAND_DISPLAY='%s'; export QT_QPA_PLATFORM=wayland; export XAUTHORITY='/home/%s/.Xauthority'; %s",
             display,
             wayland ? wayland : "",
             user,
             options->exec_cmd ? options->exec_cmd : "/bin/bash");

    if (options->root) {
        snprintf(cmd, sizeof(cmd), "chroot '%s' /bin/bash -lc \"%s\"", target, inner);
    } else {
        snprintf(cmd, sizeof(cmd), "chroot '%s' /bin/su - '%s' -c \"%s\"", target, user, inner);
    }

    system(cmd);

    if (xauth_mounted) {
        snprintf(cmd, sizeof(cmd), "umount -l '%s/home/%s/.Xauthority' 2>/dev/null", target, user);
        system(cmd);
    }

    umount_chroot(target);
}

static void delete_chroot(const Options *options) {
    char target[4096];
    char cmd[8192];

    if (!options->name) {
        fail("Missing chroot name");
    }

    snprintf(target, sizeof(target), "%s/%s", BASE_DIR, options->name);
    if (!path_exists(target)) {
        fail("Chroot not found: %s", options->name);
    }

    umount_chroot(target);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", target);
    if (run_command(cmd) != 0) {
        fail("Failed to delete chroot");
    }
}

int main(int argc, char **argv) {
    Options options;

    if (geteuid() != 0) {
        fail("Run as root");
    }

    if (argc == 1) {
        usage(argv[0]);
        return 0;
    }

    ensure_base_dirs();
    parse_args(argc, argv, &options);

    if (options.version) {
        printf("nlc %s\n", VERSION);
        return 0;
    }
    if (options.list) {
        list_chroots();
        return 0;
    }
    if (options.create) {
        create_chroot(&options);
        return 0;
    }
    if (options.enter) {
        enter_chroot(&options);
        return 0;
    }
    if (options.delete_mode) {
        delete_chroot(&options);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
