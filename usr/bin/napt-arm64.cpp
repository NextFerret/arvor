#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <utility>
#include <set>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <apt-pkg/init.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/depcache.h>
#include <apt-pkg/acquire.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/packagemanager.h>
#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/policy.h>
#include <apt-pkg/upgrade.h>
#include <apt-pkg/error.h>
#include <apt-pkg/version.h>

using namespace std;
namespace fs = std::filesystem;

const string TREE_ROOT = "/nsm/napt/root";
const string NF_TREE_BIN = "/usr/bin/nsm";
const string AUTO_SNAP_DIR = "/nsm/snapshots/auto";
const string NAPT_ETC_DIR = "/etc/napt";
const string NAPT_SOURCES_PATH = "/etc/napt/sources.list";
const string NAPT_CACHE_DIR = "/etc/napt/cache";
const string NAPT_ALLOWED_FILE = "/etc/napt/allowed";

struct NaptSource {
    string base_url;
    string release;
};

struct NaptRepoMetadata {
    string base_url;
    string release;
    map<string, pair<string, string>> packages;
    vector<string> required_packages;
};

struct NaptPackageCandidate {
    bool found = false;
    string base_url;
    string release;
    string file_name;
    string version;
    string sha256;
};

struct AptPackageState {
    bool found = false;
    bool installed = false;
    string installed_version;
    string candidate_version;
};

struct InstallDecision {
    string package_name;
    string apt_argument;
    string selected_version;
    bool from_napt = false;
};

void perform_install_transaction(const vector<string>& pkgs, bool apply_host);

bool nf_tree_available() {
    return access(NF_TREE_BIN.c_str(), X_OK) == 0;
}

void show_help() {
    cout << "New Advanced Packaging Tool - napt 3.0 - arm64\n\n"
         << "Usage: napt [command] [options]\n\n"
         << "Commands:\n"
         << "  install          Installs packages or local .deb files in a chroot; applies to host only if successful.\n"
         << "  remove           Removes packages in a chroot; applies to host only if successful.\n"
         << "  sync             Updates repository metadata.\n"
         << "  upgrade          Upgrades all packages, or selected packages, using the chroot-first method.\n"
         << "  dist-upgrade     Full release upgrade\n"
         << "  purge            Removes packages and their configuration files.\n"
         << "  clean            Cleans the Napt download cache.\n\n"
         << "Options:\n"
         << "  --apply-host     Skip the chroot and apply changes directly to the host.\n"
         << "  --v              Show version information.\n"
         << "  --vb             Enable verbose logging for debugging libapt transactions.\n"
         << "  -h               Show this help message.\n\n"
         << "                 This napt Has Super Cow Powers.\n";
}

static bool wait_for_child(pid_t pid, int& status) {
    status = 0;
    while (true) {
        pid_t ret = waitpid(pid, &status, 0);
        if (ret == pid) return true;
        if (ret == -1 && errno == EINTR) continue;
        return false;
    }
}

static int exec_argv(const vector<string>& args, int stdout_fd = -1, int stderr_fd = -1, int extra_fd = -1) {
    if (args.empty()) return 1;

    pid_t pid = fork();
    if (pid < 0) return 1;

    if (pid == 0) {
        if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO) {
            dup2(stdout_fd, STDOUT_FILENO);
            close(stdout_fd);
        }
        if (stderr_fd >= 0 && stderr_fd != STDERR_FILENO) {
            dup2(stderr_fd, STDERR_FILENO);
            close(stderr_fd);
        }
        if (extra_fd >= 0 && extra_fd != 3) {
            dup2(extra_fd, 3);
            close(extra_fd);
            fcntl(3, F_SETFD, 0);
        }

        vector<char*> argv_ptrs;
        argv_ptrs.reserve(args.size() + 1);
        for (const auto& a : args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
        argv_ptrs.push_back(nullptr);

        execvp(argv_ptrs[0], argv_ptrs.data());
        _exit(127);
    }

    int status = 0;
    wait_for_child(pid, status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

static int exec_argv_devnull_out(const vector<string>& args) {
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int rc = exec_argv(args, devnull, devnull);
    if (devnull >= 0) close(devnull);
    return rc;
}

static string exec_argv_capture(const vector<string>& args) {
    if (args.empty()) return "";

    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return ""; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        vector<char*> argv_ptrs;
        argv_ptrs.reserve(args.size() + 1);
        for (const auto& a : args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
        argv_ptrs.push_back(nullptr);
        execvp(argv_ptrs[0], argv_ptrs.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buf[512];
    string result;
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        result.append(buf, n);
    close(pipefd[0]);

    int status = 0;
    wait_for_child(pid, status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return result;
    return "";
}

bool create_snapshot(const string& name) {
    if (!nf_tree_available()) return false;
    return exec_argv_devnull_out({NF_TREE_BIN, "create", name}) == 0;
}

enum class PrecheckResult { Proceed, NoChanges, Failed };

PrecheckResult precheck_transaction(const string& action, const vector<string>& pkgs, bool quiet) {
    if (action != "install" && action != "remove" && action != "purge")
        return PrecheckResult::Proceed;

    if (pkgs.empty()) {
        if (!quiet) cout << "No packages were specified.\n";
        return PrecheckResult::Failed;
    }

    pkgCacheFile cache_file;
    pkgCache* cache = cache_file.GetPkgCache();
    pkgDepCache* dep_cache = cache_file.GetDepCache();

    if (cache == nullptr || dep_cache == nullptr)
        return PrecheckResult::Proceed;

    bool has_changes = false;

    for (const auto& pkg_name : pkgs) {
        pkgCache::PkgIterator pkg = cache->FindPkg(pkg_name);
        if (pkg.end()) {
            if (!quiet) cout << "Package " << pkg_name << " not found.\n";
            return PrecheckResult::Failed;
        }

        if (action == "install") {
            pkgCache::VerIterator cand = dep_cache->GetCandidateVersion(pkg);
            if (pkg->CurrentVer != 0 && (cand.end() || cand == pkg.CurrentVer())) {
                if (!quiet) cout << pkg_name << " is already the newest version.\n";
                continue;
            }
            has_changes = true;
            continue;
        }

        if (pkg->CurrentVer == 0) {
            if (!quiet) cout << "Package " << pkg_name << " is not installed.\n";
            continue;
        }

        has_changes = true;
    }

    return has_changes ? PrecheckResult::Proceed : PrecheckResult::NoChanges;
}

static vector<string> build_apt_argv(const string& action, const vector<string>& pkgs, bool quiet) {
    vector<string> argv = {"apt-get", "-y"};
    if (quiet) argv.push_back("-qq");

    if (action == "install" || action == "remove" || action == "purge") {
        if (pkgs.empty()) return {};
        argv.push_back(action);
        for (const auto& p : pkgs) argv.push_back(p);
        return argv;
    }

    if (action == "upgrade" || action == "dist-upgrade") {
        argv.push_back(action);
        return argv;
    }

    return {};
}

static vector<string> build_apt_install_argv(const vector<string>& args, bool quiet) {
    if (args.empty()) return {};
    vector<string> argv = {"apt-get", "-y"};
    if (quiet) argv.push_back("-qq");
    argv.push_back("install");
    for (const auto& a : args) argv.push_back(a);
    return argv;
}

void mount_fs() {
    exec_argv_devnull_out({"mount", "--bind", "/dev",      TREE_ROOT + "/dev"});
    exec_argv_devnull_out({"mount", "--bind", "/dev/pts",  TREE_ROOT + "/dev/pts"});
    exec_argv_devnull_out({"mount", "--bind", "/proc",     TREE_ROOT + "/proc"});
    exec_argv_devnull_out({"mount", "--bind", "/sys",      TREE_ROOT + "/sys"});
}

void umount_fs() {
    exec_argv_devnull_out({"umount", "-l", TREE_ROOT + "/dev/pts"});
    exec_argv_devnull_out({"umount", "-l", TREE_ROOT + "/dev"});
    exec_argv_devnull_out({"umount", "-l", TREE_ROOT + "/proc"});
    exec_argv_devnull_out({"umount", "-l", TREE_ROOT + "/sys"});
}

static string trim_str(const string& s) {
    size_t start = s.find_first_not_of(" \n\r\t<>");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \n\r\t<>");
    return s.substr(start, end - start + 1);
}

static string get_root_device() {
    string out = exec_argv_capture({"findmnt", "-n", "-o", "SOURCE", "/"});
    out = trim_str(out);
    size_t bracket = out.find('[');
    if (bracket != string::npos) out = out.substr(0, bracket);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

static string get_root_fstype() {
    string out = exec_argv_capture({"findmnt", "-n", "-o", "FSTYPE", "/"});
    return trim_str(out);
}

static string get_vg_name(const string& lv_path) {
    string out = exec_argv_capture({"lvs", "--noheadings", "-o", "vg_name", lv_path});
    return trim_str(out);
}

static bool is_lv_thin(const string& lv_path) {
    string out = exec_argv_capture({"lvs", "--noheadings", "-o", "segtype", lv_path});
    out = trim_str(out);
    return out.find("thin") != string::npos;
}

static double get_vg_free_gb(const string& vg_name) {
    string out = exec_argv_capture({"vgs", "--noheadings", "-o", "vg_free", "--units", "g", vg_name});
    out = trim_str(out);
    size_t g = out.find_first_of("gG");
    if (g != string::npos) out = out.substr(0, g);
    try { return stod(out); } catch (...) { return 0.0; }
}

bool manage_sandbox(const string& action) {
    string root_dev = get_root_device();
    string vg_name = get_vg_name(root_dev);
    string snap_lv_name = "napt_sandbox_snap";
    string snap_dev = "/dev/" + vg_name + "/" + snap_lv_name;

    if (action == "create") {
        umount_fs();
        exec_argv_devnull_out({"umount", "-l", TREE_ROOT});
        exec_argv_devnull_out({"lvremove", "-f", snap_dev});
        exec_argv_devnull_out({"mkdir", "-p", "/nsm/napt"});

        if (root_dev.empty() || vg_name.empty()) {
            cout << "Error: could not determine root LVM device or VG.\n";
            return false;
        }

        bool thin = is_lv_thin(root_dev);
        int rc;
        if (thin) {
            rc = exec_argv_devnull_out({"lvcreate", "-s", "--name", snap_lv_name, "-k", "n", root_dev});
        } else {
            double free_gb = get_vg_free_gb(vg_name);
            if (free_gb < 1.0) {
                cout << "Error: not enough VG free space. Need 1GB, have " << free_gb << "GB.\n";
                return false;
            }
            rc = exec_argv_devnull_out({"lvcreate", "-L", "1G", "-s", "--name", snap_lv_name, root_dev});
        }

        if (rc != 0) {
            cout << "Error: LVM snapshot of " << root_dev << " failed (rc=" << rc << ").\n";
            return false;
        }

        exec_argv_devnull_out({"mkdir", "-p", TREE_ROOT});

        string fstype = get_root_fstype();
        int mount_rc;
        if (fstype == "xfs") {
            mount_rc = exec_argv_devnull_out({"mount", "-t", "xfs", "-o", "nouuid", snap_dev, TREE_ROOT});
        } else if (!fstype.empty()) {
            mount_rc = exec_argv_devnull_out({"mount", "-t", fstype, snap_dev, TREE_ROOT});
        } else {
            mount_rc = exec_argv_devnull_out({"mount", snap_dev, TREE_ROOT});
        }

        if (mount_rc != 0) {
            cout << "Error: failed to mount snapshot to " << TREE_ROOT << " (rc=" << mount_rc << ").\n";
            exec_argv_devnull_out({"lvremove", "-f", snap_dev});
            return false;
        }

        struct stat st;
        if (stat(TREE_ROOT.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            cout << "Error: chroot root " << TREE_ROOT << " was not created.\n";
            return false;
        }

        exec_argv_devnull_out({"mkdir", "-p", TREE_ROOT + "/tmp"});
        return true;

    } else if (action == "delete") {
        umount_fs();
        exec_argv_devnull_out({"umount", "-l", TREE_ROOT});
        exec_argv_devnull_out({"lvremove", "-f", snap_dev});
        return true;
    }

    return false;
}

string get_latest_snapshot(const string& prefix) {
    DIR* dir = opendir(AUTO_SNAP_DIR.c_str());
    if (!dir) return "";
    struct dirent* entry;
    vector<string> matches;
    while ((entry = readdir(dir)) != NULL) {
        string name = entry->d_name;
        if (name.find(prefix) == 0) matches.push_back(name);
    }
    closedir(dir);
    if (matches.empty()) return "";
    sort(matches.begin(), matches.end());
    return matches.back();
}

void do_rollback(const string& prefix) {
    if (!nf_tree_available()) return;
    string root_snap = get_latest_snapshot("root-auto-" + prefix);
    if (!root_snap.empty()) {
        cout << "Rolling back root: " << root_snap << "\n";
        exec_argv_devnull_out({NF_TREE_BIN, "rollback", root_snap});
    }
}

string fetch_url(const string& url) {
    if (url.empty()) return "";
    return exec_argv_capture({"curl", "-fsSL", url});
}

string trim_copy(const string& s) {
    size_t start = s.find_first_not_of(" \n\r\t");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \n\r\t");
    return s.substr(start, end - start + 1);
}

bool starts_with(const string& value, const string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const string& value, const string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool path_is_directory(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_is_regular_file(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

string path_basename(const string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == string::npos) return path;
    return path.substr(pos + 1);
}

string normalize_napt_base_url(const string& raw_url) {
    string url = trim_copy(raw_url);
    if (url.find("http://") != 0 && url.find("https://") != 0)
        url = "https://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

bool is_repo_allowed(const string& url) {
#ifdef allowrepo
    if (normalize_napt_base_url(string(allowrepo)) == normalize_napt_base_url(url))
        return true;
#endif
    ifstream in(NAPT_ALLOWED_FILE);
    if (!in) return false;
    string line;
    string norm_url = normalize_napt_base_url(url);
    while (getline(in, line)) {
        if (normalize_napt_base_url(line) == norm_url) return true;
    }
    return false;
}

void print_napt_repo_warning(const string& url) {
    if (is_repo_allowed(url)) return;
    static set<string> warned_repos;
    if (warned_repos.count(url)) return;
    warned_repos.insert(url);
    cout << " You are installing packages from an unauthorized third-party napt repository:\n"
         << " " << url << "\n"
         << " These packages are UNVERIFIED and could contain MALWARE, RANSOMWARE, or\n"
         << " utterly DESTROY your operating system.\n"
         << " ONLY PROCEED IF YOU ABSOLUTELY TRUST THE SOURCE!\n";
}

bool parse_napt_source_line(const string& raw_line, NaptSource& source) {
    string line = trim_copy(raw_line);
    if (line.empty() || line[0] == '#') return false;
    istringstream iss(line);
    string type, base_url, release;
    if (!(iss >> type >> base_url >> release)) return false;
    if (type != "deb") return false;
    base_url = normalize_napt_base_url(base_url);
    release = trim_copy(release);
    if (base_url.empty() || release.empty()) return false;
    source.base_url = base_url;
    source.release = release;
    return true;
}

void load_napt_sources_from_file(const string& path, vector<NaptSource>& sources) {
    ifstream in(path);
    if (!in) return;
    string line;
    while (getline(in, line)) {
        NaptSource source;
        if (parse_napt_source_line(line, source)) sources.push_back(source);
    }
}

vector<NaptSource> load_napt_sources() {
    vector<NaptSource> sources;
    if (path_is_regular_file(NAPT_SOURCES_PATH)) {
        load_napt_sources_from_file(NAPT_SOURCES_PATH, sources);
        return sources;
    }

    if (path_is_directory(NAPT_SOURCES_PATH)) {
        DIR* dir = opendir(NAPT_SOURCES_PATH.c_str());
        if (!dir) return sources;
        vector<string> files;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            string name = entry->d_name;
            if (name == "." || name == "..") continue;
            string path = NAPT_SOURCES_PATH + "/" + name;
            if (path_is_regular_file(path)) files.push_back(path);
        }
        closedir(dir);
        sort(files.begin(), files.end());
        for (const auto& path : files) load_napt_sources_from_file(path, sources);
    }

    return sources;
}

bool write_text_file(const string& path, const string& content) {
    ofstream out(path);
    if (!out) return false;
    out << content;
    return out.good();
}

bool read_text_file(const string& path, string& content) {
    ifstream in(path);
    if (!in) return false;
    stringstream buffer;
    buffer << in.rdbuf();
    content = buffer.str();
    return true;
}

bool parse_napt_repo_metadata(const string& text, NaptRepoMetadata& metadata) {
    metadata.packages.clear();
    metadata.required_packages.clear();
    string line;
    bool in_packages = false;
    bool in_required = false;
    stringstream ss(text);
    while (getline(ss, line)) {
        string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed == "[napt repository]") continue;
        if (starts_with(trimmed, "release=")) {
            metadata.release = trim_copy(trimmed.substr(8));
            continue;
        }
        if (trimmed == "packages:") { in_packages = true; in_required = false; continue; }
        if (trimmed == "required:") { in_required = true; in_packages = false; continue; }
        if (in_required) {
            size_t start = trimmed.find('{');
            size_t end = trimmed.find('}');
            if (start != string::npos && end != string::npos && end > start) {
                string req_pkg = trim_copy(trimmed.substr(start + 1, end - start - 1));
                if (!req_pkg.empty()) metadata.required_packages.push_back(req_pkg);
            }
            continue;
        }
        if (in_packages) {
            size_t pos = trimmed.find('=');
            if (pos == string::npos) continue;
            string pkg = trim_copy(trimmed.substr(0, pos));
            string rest = trim_copy(trimmed.substr(pos + 1));
            string file_name, hash;
            size_t sha_pos = rest.find("sha256=");
            if (sha_pos != string::npos) {
                file_name = trim_copy(rest.substr(0, sha_pos));
                hash = trim_copy(rest.substr(sha_pos + 7));
            } else {
                file_name = rest;
            }
            if (!pkg.empty() && !file_name.empty())
                metadata.packages[pkg] = {file_name, hash};
        }
    }
    return !metadata.release.empty();
}

bool sync_napt_metadata() {
    vector<NaptSource> sources = load_napt_sources();
    if (sources.empty()) return true;

    exec_argv_devnull_out({"mkdir", "-p", NAPT_ETC_DIR});

    bool ok = true;
    for (const auto& source : sources) {
        print_napt_repo_warning(source.base_url);
        string url = source.base_url + "/releases/" + source.release + "/repo-metadata";
        string metadata = fetch_url(url);
        if (metadata.empty()) {
            cout << "Failed to fetch metadata: " << url << "\n";
            ok = false;
            continue;
        }
        string release_dir = NAPT_ETC_DIR + "/" + source.release;
        if (exec_argv_devnull_out({"mkdir", "-p", release_dir}) != 0) {
            cout << "Failed to create metadata directory: " << release_dir << "\n";
            ok = false;
            continue;
        }
        string output_path = release_dir + "/repo-metadata";
        if (!write_text_file(output_path, metadata)) {
            cout << "Failed to write metadata: " << output_path << "\n";
            ok = false;
            continue;
        }
        cout << "Synced metadata for " << source.release << " from " << source.base_url << "\n";
    }
    return ok;
}

bool clean_napt_cache() {
    error_code ec;
    if (!fs::exists(NAPT_CACHE_DIR, ec)) {
        if (!fs::create_directories(NAPT_CACHE_DIR, ec)) {
            cout << "Failed to create cache directory: " << NAPT_CACHE_DIR << "\n";
            return false;
        }
        cout << "Cache is already clean.\n";
        return true;
    }

    bool removed_any = false;
    for (const auto& entry : fs::directory_iterator(NAPT_CACHE_DIR, ec)) {
        if (ec) {
            cout << "Failed to read cache directory: " << NAPT_CACHE_DIR << "\n";
            return false;
        }
        fs::remove_all(entry.path(), ec);
        if (ec) {
            cout << "Failed to remove: " << entry.path().string() << "\n";
            return false;
        }
        removed_any = true;
    }

    if (!fs::exists(NAPT_CACHE_DIR, ec) && !fs::create_directories(NAPT_CACHE_DIR, ec)) {
        cout << "Failed to recreate cache directory: " << NAPT_CACHE_DIR << "\n";
        return false;
    }

    if (removed_any)
        cout << "Cache cleaned: " << NAPT_CACHE_DIR << "\n";
    else
        cout << "Cache is already clean.\n";

    return true;
}

void print_install_already_present_message(const string& pkg_name) {
    cout << pkg_name << " is already installed. For upgrades, use napt upgrade "
         << pkg_name << " or just napt upgrade. If it is a huge package, use --apply-host.\n";
}

vector<NaptRepoMetadata> load_cached_napt_metadata() {
    vector<NaptRepoMetadata> repos;
    vector<NaptSource> sources = load_napt_sources();
    for (const auto& source : sources) {
        string path = NAPT_ETC_DIR + "/" + source.release + "/repo-metadata";
        string content;
        if (!read_text_file(path, content)) continue;
        NaptRepoMetadata metadata;
        metadata.base_url = source.base_url;
        metadata.release = source.release;
        if (!parse_napt_repo_metadata(content, metadata)) continue;
        if (metadata.release.empty()) metadata.release = source.release;
        repos.push_back(metadata);
    }
    return repos;
}

int compare_versions(const string& a, const string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return -1;
    if (b.empty()) return 1;
    if (_system != nullptr && _system->VS != nullptr)
        return _system->VS->CmpVersion(a.c_str(), b.c_str());
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

string extract_napt_version(const string& pkg_name, const string& file_name) {
    string base = path_basename(trim_copy(file_name));
    if (!ends_with(base, ".deb")) return "";
    string stem = base.substr(0, base.size() - 4);
    string rest;
    if (starts_with(stem, pkg_name + "_"))
        rest = stem.substr(pkg_name.size() + 1);
    else if (starts_with(stem, pkg_name + "-"))
        rest = stem.substr(pkg_name.size() + 1);
    else
        return "";
    size_t split = rest.find_last_of('_');
    if (split != string::npos && split > 0) return rest.substr(0, split);
    split = rest.find_last_of('-');
    if (split != string::npos && split > 0) return rest.substr(0, split);
    return rest;
}

AptPackageState get_apt_package_state(pkgCacheFile& cache_file, const string& pkg_name) {
    AptPackageState state;
    pkgCache* cache = cache_file.GetPkgCache();
    pkgDepCache* dep_cache = cache_file.GetDepCache();
    if (cache == nullptr || dep_cache == nullptr) return state;
    pkgCache::PkgIterator pkg = cache->FindPkg(pkg_name);
    if (pkg.end()) return state;
    state.found = true;
    if (pkg->CurrentVer != 0) {
        state.installed = true;
        state.installed_version = pkg.CurrentVer().VerStr();
    }
    pkgCache::VerIterator cand = dep_cache->GetCandidateVersion(pkg);
    if (!cand.end()) state.candidate_version = cand.VerStr();
    return state;
}

NaptPackageCandidate find_best_napt_candidate(const vector<NaptRepoMetadata>& repos, const string& pkg_name) {
    NaptPackageCandidate best;
    for (const auto& repo : repos) {
        auto it = repo.packages.find(pkg_name);
        if (it == repo.packages.end()) continue;
        NaptPackageCandidate candidate;
        candidate.found = true;
        candidate.base_url = repo.base_url;
        candidate.release = repo.release;
        candidate.file_name = it->second.first;
        candidate.sha256 = it->second.second;
        candidate.version = extract_napt_version(pkg_name, candidate.file_name);
        if (!best.found || compare_versions(candidate.version, best.version) > 0)
            best = candidate;
    }
    return best;
}

string build_napt_download_url(const NaptPackageCandidate& candidate) {
    string file_name = trim_copy(candidate.file_name);
    while (!file_name.empty() && file_name.front() == '/') file_name.erase(file_name.begin());
    return candidate.base_url + "/releases/" + candidate.release + "/" + file_name;
}

bool cache_napt_package(const NaptPackageCandidate& candidate, string& local_path) {
    string release_dir = NAPT_CACHE_DIR + "/" + candidate.release;
    if (exec_argv_devnull_out({"mkdir", "-p", release_dir}) != 0) return false;
    local_path = release_dir + "/" + path_basename(candidate.file_name);
    string url = build_napt_download_url(candidate);
    return exec_argv_devnull_out({"curl", "-fsSL", "-o", local_path, url}) == 0;
}

string calculate_sha256(const string& file_path) {
    string out = exec_argv_capture({"sha256sum", file_path});
    size_t space_pos = out.find(' ');
    if (space_pos != string::npos) return out.substr(0, space_pos);
    return trim_copy(out);
}

struct DebFileInfo {
    string package_name;
    string version;
    string architecture;
};

bool get_deb_file_info(const string& path, DebFileInfo& info) {
    string out = exec_argv_capture({"dpkg-deb", "--field", path, "Package", "Version", "Architecture"});
    if (out.empty()) return false;
    istringstream ss(out);
    string line;
    while (getline(ss, line)) {
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        string key   = trim_copy(line.substr(0, colon));
        string value = trim_copy(line.substr(colon + 1));
        if (key == "Package")           info.package_name = value;
        else if (key == "Version")      info.version      = value;
        else if (key == "Architecture") info.architecture = value;
    }
    return !info.package_name.empty();
}

bool resolve_install_decisions(const vector<string>& pkgs, vector<InstallDecision>& decisions, bool quiet) {
    if (pkgs.empty()) {
        if (!quiet) cout << "No packages were specified.\n";
        return false;
    }

    pkgCacheFile cache_file;
    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    bool had_error = false;

    for (const auto& pkg_name : pkgs) {
        if (ends_with(pkg_name, ".deb")) {
            if (!path_is_regular_file(pkg_name)) {
                if (!quiet) cout << "Local .deb file not found: " << pkg_name << "\n";
                had_error = true;
                continue;
            }
            DebFileInfo deb_info;
            bool has_info = get_deb_file_info(pkg_name, deb_info);
            if (!quiet) {
                cout << "Installing local .deb: ";
                if (has_info) {
                    cout << deb_info.package_name;
                    if (!deb_info.version.empty())      cout << " (" << deb_info.version << ")";
                    if (!deb_info.architecture.empty()) cout << " [" << deb_info.architecture << "]";
                } else {
                    cout << pkg_name;
                }
                cout << "\n";
            }
            InstallDecision decision;
            decision.package_name     = has_info ? deb_info.package_name : pkg_name;
            decision.apt_argument     = pkg_name;
            decision.selected_version = has_info ? deb_info.version : "";
            decision.from_napt        = false;
            decisions.push_back(decision);
            continue;
        }

        AptPackageState apt_state = get_apt_package_state(cache_file, pkg_name);
        NaptPackageCandidate napt_candidate = find_best_napt_candidate(repos, pkg_name);

        if (!apt_state.found && !napt_candidate.found) {
            if (!quiet) cout << "Package " << pkg_name << " not found.\n";
            had_error = true;
            continue;
        }

        bool use_napt = false;
        if (napt_candidate.found) {
            if (!apt_state.found || apt_state.candidate_version.empty())
                use_napt = true;
            else if (compare_versions(napt_candidate.version, apt_state.candidate_version) > 0)
                use_napt = true;
        }

        if (use_napt) {
            print_napt_repo_warning(napt_candidate.base_url);
            if (apt_state.installed && compare_versions(apt_state.installed_version, napt_candidate.version) >= 0) {
                if (!quiet) print_install_already_present_message(pkg_name);
                continue;
            }
            string local_path;
            if (!cache_napt_package(napt_candidate, local_path)) {
                if (!quiet) cout << "Failed to download Napt package for " << pkg_name << ".\n";
                had_error = true;
                continue;
            }
            if (!napt_candidate.sha256.empty()) {
                string local_hash = calculate_sha256(local_path);
                if (local_hash != napt_candidate.sha256) {
                    if (!quiet) {
                        cout << "SHA256 checksum mismatch for " << pkg_name << ".\n"
                             << "Expected: " << napt_candidate.sha256 << "\n"
                             << "Got:      " << local_hash << "\n"
                             << "Aborting installation of this package.\n";
                    }
                    had_error = true;
                    exec_argv_devnull_out({"rm", "-f", local_path});
                    continue;
                }
            }
            InstallDecision decision;
            decision.package_name     = pkg_name;
            decision.apt_argument     = local_path;
            decision.selected_version = napt_candidate.version;
            decision.from_napt        = true;
            decisions.push_back(decision);
            if (!quiet) {
                cout << "Using Napt package for " << pkg_name;
                if (!napt_candidate.version.empty()) cout << " (" << napt_candidate.version << ")";
                cout << ".\n";
            }
            continue;
        }

        if (!apt_state.found || apt_state.candidate_version.empty()) {
            if (!quiet) cout << "Package " << pkg_name << " not found.\n";
            had_error = true;
            continue;
        }

        if (apt_state.installed && compare_versions(apt_state.installed_version, apt_state.candidate_version) >= 0) {
            if (!quiet) print_install_already_present_message(pkg_name);
            continue;
        }

        InstallDecision decision;
        decision.package_name     = pkg_name;
        decision.apt_argument     = pkg_name;
        decision.selected_version = apt_state.candidate_version;
        decision.from_napt        = false;
        decisions.push_back(decision);
        if (!quiet) {
            cout << "Using Debian package for " << pkg_name;
            if (!apt_state.candidate_version.empty()) cout << " (" << apt_state.candidate_version << ")";
            cout << ".\n";
        }
    }

    return !had_error;
}

void do_nflinux_upgrade(bool apply_host) {
#ifdef nflinux
    string os_release = fetch_url("https://nextferret.github.io/etc/os-release-arm");
    if (!os_release.empty()) {
        ofstream out("/etc/os-release");
        out << os_release;
        out.close();
    }

    string codenames = fetch_url("https://nextferret.github.io/version_codename-arm");
    string repo_number_str = trim_copy(fetch_url("https://nextferret.github.io/repo-number"));
    if (!codenames.empty() && !repo_number_str.empty()) {
        size_t comma = codenames.find(',');
        if (comma != string::npos) {
            string napt_code = trim_copy(codenames.substr(0, comma));
            string debian_code = trim_copy(codenames.substr(comma + 1));
            string base_repo_url = "https://nextferretdur.github.io/repo-nflinux-" + repo_number_str;
            string meta_url = base_repo_url + "/releases/" + napt_code + "/repo-metadata";
            if (!fetch_url(meta_url).empty()) {
                exec_argv_devnull_out({"rm", "-f", "/etc/napt/sources.list"});
                write_text_file("/etc/napt/sources.list", "deb " + base_repo_url + " " + napt_code + "\n");
                string apt_sources = "deb http://deb.debian.org/debian " + debian_code + " main contrib non-free non-free-firmware\n";
                apt_sources += "deb http://deb.debian.org/debian-security " + debian_code + "-security main contrib non-free non-free-firmware\n";
                apt_sources += "deb http://deb.debian.org/debian " + debian_code + "-updates main contrib non-free non-free-firmware\n";
                write_text_file("/etc/apt/sources.list", apt_sources);
            }
        }
    }

    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    vector<string> pkgs_to_install;
    for (const auto& repo : repos) {
        for (const auto& req : repo.required_packages)
            pkgs_to_install.push_back(req);
    }

    if (!pkgs_to_install.empty()) {
        sort(pkgs_to_install.begin(), pkgs_to_install.end());
        pkgs_to_install.erase(unique(pkgs_to_install.begin(), pkgs_to_install.end()), pkgs_to_install.end());
        cout << "Installing required packages from repositories...\n";
        perform_install_transaction(pkgs_to_install, apply_host);
    }
#endif
}

static const int PROGRESS_BAR_WIDTH = 18;

static string format_remaining(double seconds) {
    int s = static_cast<int>(seconds);
    if (s <= 0) return "0s";
    if (s < 60) return to_string(s) + "s";
    int m = s / 60, r = s % 60;
    return to_string(m) + "m " + to_string(r) + "s";
}

static void render_chroot_bar(int filled, const string& time_str) {
    string bar(filled, '#');
    bar += string(PROGRESS_BAR_WIDTH - filled, ' ');
    string line = "\rTesting on the chroot                        ["
                  + bar + "] Estimated Time:" + time_str;
    line += string(max(0, 20 - static_cast<int>(time_str.size())), ' ');
    cout << line;
    cout.flush();
}

static vector<string> stage_debs_for_chroot(const vector<string>& orig_argv, vector<string>& staged_host_paths) {
    vector<string> rewritten;
    for (const auto& token : orig_argv) {
        if (token.size() > 4 && ends_with(token, ".deb")) {
            string filename = path_basename(token);
            string chroot_tmp_host  = TREE_ROOT + "/tmp/" + filename;
            string chroot_tmp_inner = "/tmp/" + filename;
            exec_argv_devnull_out({"mkdir", "-p", TREE_ROOT + "/tmp"});
            int rc = exec_argv_devnull_out({"cp", token, chroot_tmp_host});
            if (rc != 0) cout << "Error: failed to stage " << token << " into chroot tmp.\n";
            staged_host_paths.push_back(chroot_tmp_host);
            rewritten.push_back(chroot_tmp_inner);
        } else {
            rewritten.push_back(token);
        }
    }
    return rewritten;
}

static void cleanup_staged_debs(const vector<string>& staged_host_paths) {
    for (const auto& p : staged_host_paths)
        exec_argv_devnull_out({"rm", "-f", p});
}

void perform_transaction_argv(const vector<string>& transaction_argv, bool apply_host) {
    if (!apply_host) {
        if (!manage_sandbox("create")) {
            cout << "Aborting transaction: chroot could not be created.\n";
            return;
        }
        mount_fs();

        int apt_pipe[2]  = {-1, -1};
        bool have_pipe   = (pipe(apt_pipe) == 0);
        int err_pipe[2]  = {-1, -1};
        bool have_err    = (pipe2(err_pipe, O_CLOEXEC) == 0);

        vector<string> staged_debs;
        vector<string> chroot_argv = stage_debs_for_chroot(transaction_argv, staged_debs);

        if (have_pipe) {
            chroot_argv.push_back("-o");
            chroot_argv.push_back("APT::Status-Fd=3");
        }

        pid_t pid = fork();
        if (pid == 0) {
            if (chroot(TREE_ROOT.c_str()) != 0 || chdir("/") != 0) {
                if (have_err) {
                    const char* msg = "chroot/chdir failed\n";
                    write(err_pipe[1], msg, strlen(msg));
                }
                _exit(1);
            }

            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }

            if (have_err) {
                close(err_pipe[0]);
                if (err_pipe[1] != STDERR_FILENO) {
                    dup2(err_pipe[1], STDERR_FILENO);
                    close(err_pipe[1]);
                }
            } else {
                int dn2 = open("/dev/null", O_WRONLY | O_CLOEXEC);
                if (dn2 >= 0) { dup2(dn2, STDERR_FILENO); close(dn2); }
            }

            if (have_pipe) {
                close(apt_pipe[0]);
                if (apt_pipe[1] != 3) { dup2(apt_pipe[1], 3); close(apt_pipe[1]); }
                fcntl(3, F_SETFD, 0);
            }

            pkgInitConfig(*_config);
            pkgInitSystem(*_config, _system);

            vector<char*> argv_ptrs;
            argv_ptrs.reserve(chroot_argv.size() + 1);
            for (const auto& a : chroot_argv) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
            argv_ptrs.push_back(nullptr);

            setenv("DEBIAN_FRONTEND", "noninteractive", 1);
            execvp(argv_ptrs[0], argv_ptrs.data());
            _exit(127);

        } else if (pid > 0) {
            if (have_pipe) close(apt_pipe[1]);
            if (have_err)  close(err_pipe[1]);

            std::atomic<double> apt_percent(0.0);
            std::atomic<bool>   display_done(false);
            string child_stderr_output;

            std::thread stderr_reader([&]() {
                if (!have_err) return;
                char buf[256];
                ssize_t n;
                while ((n = read(err_pipe[0], buf, sizeof(buf) - 1)) > 0) {
                    buf[n] = '\0';
                    child_stderr_output += buf;
                }
                close(err_pipe[0]);
            });

            std::thread reader_thread([&]() {
                if (!have_pipe) return;
                FILE* f = fdopen(apt_pipe[0], "r");
                if (!f) { close(apt_pipe[0]); return; }
                char line[512];
                while (fgets(line, sizeof(line), f) != NULL) {
                    string s(line);
                    bool is_pm = (s.size() > 9 && s.substr(0, 9) == "pmstatus:");
                    bool is_dl = (!is_pm && s.size() > 9 && s.substr(0, 9) == "dlstatus:");
                    if (!is_pm && !is_dl) continue;
                    size_t c1 = s.find(':');
                    if (c1 == string::npos) continue;
                    size_t c2 = s.find(':', c1 + 1);
                    if (c2 == string::npos) continue;
                    size_t c3 = s.find(':', c2 + 1);
                    if (c3 == string::npos) continue;
                    string pct_str = s.substr(c2 + 1, c3 - c2 - 1);
                    try {
                        double pct = stod(pct_str);
                        if (pct > apt_percent.load()) apt_percent.store(pct);
                    } catch (...) {}
                }
                fclose(f);
            });

            std::thread display_thread([&]() {
                auto start = std::chrono::steady_clock::now();
                while (!display_done.load()) {
                    double pct = apt_percent.load();
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() / 1000.0;
                    int filled;
                    string time_str;
                    if (have_pipe) {
                        filled = static_cast<int>((pct / 100.0) * PROGRESS_BAR_WIDTH);
                        filled = min(filled, PROGRESS_BAR_WIDTH - 1);
                        if (pct > 2.0 && elapsed > 1.0) {
                            double remaining = elapsed * (100.0 - pct) / pct;
                            time_str = format_remaining(remaining);
                        } else {
                            time_str = "estimating...";
                        }
                    } else {
                        filled = min(PROGRESS_BAR_WIDTH - 1,
                                     static_cast<int>(elapsed / 120.0 * PROGRESS_BAR_WIDTH));
                        time_str = format_remaining(max(0.0, 120.0 - elapsed));
                    }
                    render_chroot_bar(filled, time_str);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            });

            int status = 0;
            bool waited = wait_for_child(pid, status);

            display_done.store(true);
            display_thread.join();
            reader_thread.join();
            stderr_reader.join();

            int filled_final;
            {
                double pct = apt_percent.load();
                filled_final = static_cast<int>((pct / 100.0) * PROGRESS_BAR_WIDTH);
                filled_final = min(filled_final, PROGRESS_BAR_WIDTH - 1);
            }

            umount_fs();
            cleanup_staged_debs(staged_debs);
            manage_sandbox("delete");

            auto print_bar = [&](const string& suffix) {
                string bar(filled_final, '#');
                bar += string(PROGRESS_BAR_WIDTH - filled_final, ' ');
                cout << "\rTesting on the chroot                        ["
                     << bar << "]                           " << suffix << "\n";
            };

            if (!waited) { print_bar("...  chroot test interrupted."); return; }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                print_bar("...  chroot test failed.");
                if (!child_stderr_output.empty()) {
                    cout << "\n--- chroot apt error ---\n" << child_stderr_output;
                    if (child_stderr_output.back() != '\n') cout << '\n';
                    cout << "------------------------\n";
                }
                return;
            }

            cout << "\rTesting on the chroot                        ["
                 << string(PROGRESS_BAR_WIDTH, '#') << "] Estimated Time:done          \n";
            cout << "Chroot test successful. Applying to host...\n";
            cout << "Do you want to apply the transaction to the host system?\n";
            cout << "This action cannot be undone without rollback if it fails.\n";
            cout << "Type 'yes' to confirm or anything else to abort: ";
            string confirm;
            getline(cin, confirm);
            if (confirm != "yes" && confirm != "YES") {
                cout << "Transaction aborted by user.\n";
                return;
            }
        } else {
            if (have_pipe) { close(apt_pipe[0]); close(apt_pipe[1]); }
            if (have_err)  { close(err_pipe[0]); close(err_pipe[1]); }
            cout << "Fork failed for chroot test.\n";
            return;
        }
    }

    bool snapshot_created = create_snapshot("apt-pre");

    vector<char*> argv_ptrs;
    argv_ptrs.reserve(transaction_argv.size() + 1);
    for (const auto& a : transaction_argv) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
    argv_ptrs.push_back(nullptr);

    pid_t host_pid = fork();
    if (host_pid == 0) {
        setenv("DEBIAN_FRONTEND", "noninteractive", 1);
        execvp(argv_ptrs[0], argv_ptrs.data());
        _exit(127);
    }

    int host_status = 0;
    bool host_ok = false;
    if (host_pid > 0) {
        wait_for_child(host_pid, host_status);
        host_ok = WIFEXITED(host_status) && WEXITSTATUS(host_status) == 0;
    }

    if (host_ok) {
        create_snapshot("apt-post");
        cout << "Transaction applied successfully.\n";
    } else {
        cout << "Host transaction failed. Rolling back...\n";
        if (snapshot_created) do_rollback("apt-pre");
    }
}

void perform_transaction(const string& action, const vector<string>& pkgs, bool apply_host) {
    PrecheckResult precheck = precheck_transaction(action, pkgs, false);
    if (precheck == PrecheckResult::Failed || precheck == PrecheckResult::NoChanges) return;

    vector<string> argv = build_apt_argv(action, pkgs, false);
    if (argv.empty()) { cout << "Invalid transaction request.\n"; return; }

    perform_transaction_argv(argv, apply_host);
}

void perform_install_transaction(const vector<string>& pkgs, bool apply_host) {
    vector<InstallDecision> decisions;
    if (!resolve_install_decisions(pkgs, decisions, false)) return;
    if (decisions.empty()) return;

    vector<string> args;
    for (const auto& decision : decisions) args.push_back(decision.apt_argument);

    vector<string> argv = build_apt_install_argv(args, false);
    if (argv.empty()) { cout << "Invalid transaction request.\n"; return; }

    perform_transaction_argv(argv, apply_host);
}

void perform_global_upgrade(bool apply_host) {
    pkgCacheFile cache_file;
    pkgCache* cache = cache_file.GetPkgCache();
    if (cache == nullptr) return;

    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    vector<string> napt_upgrade_args;

    auto get_installed_version = [&](const string& pkg_name) -> string {
        AptPackageState s = get_apt_package_state(cache_file, pkg_name);
        return s.installed ? s.installed_version : "";
    };

    auto try_queue_napt_upgrade = [&](const string& pkg_name, const string& installed_version) {
        NaptPackageCandidate candidate = find_best_napt_candidate(repos, pkg_name);
        if (!candidate.found) return;
        if (!installed_version.empty() && compare_versions(candidate.version, installed_version) <= 0) return;
        string local_path;
        if (!cache_napt_package(candidate, local_path)) return;
        if (!candidate.sha256.empty()) {
            string h = calculate_sha256(local_path);
            if (h != candidate.sha256) {
                exec_argv_devnull_out({"rm", "-f", local_path});
                cout << "Checksum mismatch for " << pkg_name << ", skipping.\n";
                return;
            }
        }
        cout << "Queuing napt upgrade: " << pkg_name;
        if (!installed_version.empty()) cout << " (" << installed_version << " -> " << candidate.version << ")";
        else cout << " (" << candidate.version << ")";
        cout << "\n";
        napt_upgrade_args.push_back(local_path);
    };

    set<string> handled_pkgs;
    for (const auto& repo : repos) {
        if (repo.required_packages.empty()) continue;
        string already_installed_req;
        for (const auto& req : repo.required_packages) {
            if (!get_installed_version(req).empty()) { already_installed_req = req; break; }
        }
        if (!already_installed_req.empty()) {
            string iv = get_installed_version(already_installed_req);
            try_queue_napt_upgrade(already_installed_req, iv);
            handled_pkgs.insert(already_installed_req);
        } else {
            for (const auto& req : repo.required_packages) {
                string iv = get_installed_version(req);
                NaptPackageCandidate c = find_best_napt_candidate(repos, req);
                if (!iv.empty()) {
                    if (c.found && compare_versions(c.version, iv) > 0) {
                        cout << "Upgrading required package: " << req << " (" << iv << " -> " << c.version << ")\n";
                        try_queue_napt_upgrade(req, iv);
                        handled_pkgs.insert(req);
                    }
                    continue;
                }
                if (c.found) {
                    cout << "Installing required package: " << req << "\n";
                    perform_install_transaction({req}, apply_host);
                    handled_pkgs.insert(req);
                } else {
                    AptPackageState apt_state = get_apt_package_state(cache_file, req);
                    if (apt_state.found) {
                        cout << "Installing required package: " << req << "\n";
                        perform_install_transaction({req}, apply_host);
                        handled_pkgs.insert(req);
                    } else {
                        cout << "Warning: required package " << req << " not found in any repo, skipping.\n";
                    }
                }
            }
        }
    }

    for (pkgCache::PkgIterator pkg = cache->PkgBegin(); !pkg.end(); ++pkg) {
        if (pkg->CurrentVer == 0) continue;
        string pkg_name = pkg.Name();
        if (handled_pkgs.count(pkg_name)) continue;
        AptPackageState apt_state = get_apt_package_state(cache_file, pkg_name);
        try_queue_napt_upgrade(pkg_name, apt_state.installed_version);
    }

    if (!napt_upgrade_args.empty()) {
        cout << "Upgrading NAPT packages first...\n";
        vector<string> napt_argv = build_apt_install_argv(napt_upgrade_args, false);
        perform_transaction_argv(napt_argv, apply_host);
    }

    cout << "Proceeding with standard apt upgrade...\n";
    perform_transaction("upgrade", vector<string>(), apply_host);
}

void perform_upgrade_transaction(const vector<string>& pkgs, bool apply_host) {
    if (pkgs.empty()) { perform_global_upgrade(apply_host); return; }
    perform_install_transaction(pkgs, apply_host);
}

int main(int argc, char** argv) {
    pkgInitConfig(*_config);
    pkgInitSystem(*_config, _system);
    string command;
    vector<string> pkgs;
    bool apply_host = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "-h") { show_help(); return 0; }
        else if (arg == "--v") { cout << "napt 2.0\n"; return 0; }
        else if (arg == "--vb") { _config->Set("Debug::pkgAcquire", "true"); }
        else if (arg == "--apply-host") { apply_host = true; }
        else if (command.empty() && arg[0] != '-') { command = arg; }
        else if (arg[0] != '-') { pkgs.push_back(arg); }
    }

    if (command.empty()) { show_help(); return 0; }

    if (geteuid() != 0) { cout << "Root privileges required.\n"; return 1; }

    if (command == "sync") {
        int apt_rc = exec_argv({"apt-get", "update"});
        bool napt_ok = sync_napt_metadata();
        return (apt_rc == 0 && napt_ok) ? 0 : 1;
    } else if (command == "clean") {
        return clean_napt_cache() ? 0 : 1;
    } else if (command == "dist-upgrade") {
#ifdef nflinux
        do_nflinux_upgrade(apply_host);
#else
        perform_transaction("dist-upgrade", pkgs, apply_host);
#endif
    } else if (command == "install") {
        perform_install_transaction(pkgs, apply_host);
    } else if (command == "upgrade") {
        perform_upgrade_transaction(pkgs, apply_host);
    } else if (command == "remove" || command == "purge") {
        perform_transaction(command, pkgs, apply_host);
    } else {
        cout << "Unknown command: " << command << "\n";
        show_help();
        return 1;
    }

    return 0;
}
