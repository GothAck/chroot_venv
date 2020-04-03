#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <docopt/docopt.h>

#include "config.hpp"
#include "procmounts.hpp"

using namespace std;
namespace fs = std::filesystem;

using namespace procmounts;

namespace chroot_venv {

static const char USAGE[] =
R"(chroot virtual environment manager.

    Usage:
      chroot_venv [options] [--keepfd=<fd>]... <chroot-name> [<command-or-args> ...]
      chroot_venv (-h | --help)

    Options:
      -f <fd> --keepfd=<fd>    Keep FD open
      -b <base> --base=<base>  Set or override base image
      -p --print               Print build_root yaml
      -v --verbose             Print verbose messages
      -h --help                Show this screen.
)";

bool verbose = false;

const array<const string, 4> SYSTEM_FS = {
  "/proc",
  "/sys",
  "/dev",
  "/dev/pts",
};

class FileLock {
  int fd_;
public:
  FileLock(int fd) : fd_(fd) {};

  void lock() {
    if (flock(fd_, LOCK_EX)) {
      cerr << "Failed to lock " << fd_ << endl;
      throw exception();
    }
  }

  void unlock() {
    flock(fd_, LOCK_UN);
  }
};

int mount(string src, string dst, string fs, int flags, string opts) {
  if (verbose)
    cerr << "mount(" << src << ", " << dst << ", " << fs << ", " << flags << ", " << opts << ")" << endl;
  return ::mount(src.c_str(), dst.c_str(), fs.c_str(), flags, opts.c_str());
}

int umount(string dst) {
  if (verbose)
    cerr << "umount(" << dst << ")" << endl;
  return ::umount(dst.c_str());
}

struct State {
  State(fs::path root): build_root(root), build_root_orig(root) {}
  fs::path build_root;
  const fs::path build_root_orig;
  deque<fs::path> mounted_system_fs;
  vector<fs::path> mounted_binds;
  vector<fs::path> mounted_tmpfs;
  unordered_set<int> keepfd;
  int mtabLockFd = -1;
  shared_ptr<FileLock> mtabLock;
  int exitstatus = 0;
};
int pid = -1;

bool halting = false;

void signalHandler(int signum) {
  cerr << "Interrupt signal (" << signum << ") received." << endl;
  if (halting) return;
  if (pid > 1) kill(pid, signum);
  halting = true;
}

enum class Stage {
  NONE = 0,
  MKTEMP,
  ROOT,
  SYSTEM_FS,
  BINDS,
  TMPFS,
  PROCESSES,
  MTAB,
};

optional<Stage> start(deque<string> args, const Config &config, shared_ptr<State> state) {
  state->mtabLockFd = open("mtab", O_CREAT | O_RDONLY | O_CLOEXEC, 00664);
  if (state->mtabLockFd < 0) {
    cerr << "Failed to open lock file" << endl;
    return Stage::NONE;
  }
  state->mtabLock = make_shared<FileLock>(state->mtabLockFd);
  if (!state->mtabLock) {
    cerr << "Failed to create FileLock" << endl;
    return Stage::NONE;
  }

  if (config.mktemp) {
    string tmp = "/tmp/chroot-XXXXXX";
    if (! mkdtemp(tmp.data())) {
      cerr << "Failed to create temp dir" << endl;
      return Stage::NONE;
    }
    state->build_root = tmp;
  }

  if (config.exec) {
    if (config.args) {
      for (auto it = config.args->rbegin(); it != config.args->rend(); ++it) {
        args.push_front(*it);
      }
    }
    args.push_front(*config.exec);
  }
  clearenv();
  setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin", 1);
  setenv("debian_chroot", state->build_root_orig.c_str(), 1);
  for (auto &p : config.env) {
    auto key = p.first;
    auto val = p.second;
    if (key.front() == '+') {
      key = key.substr(1);
      const auto cur_char = getenv(key.c_str());
      string cur = cur_char ?: "";
      if (!cur.empty()) val += ":";
      val += cur;
    } else if (key.back() == '+') {
      key.pop_back();
      const auto cur_char = getenv(key.c_str());
      string cur = cur_char ?: "";
      if (!cur.empty()) cur += ":";
      val = cur + val;
    }
    setenv(key.c_str(), val.c_str(), 1);
  }

  if (args.empty()) {
    bool set_shell = false;
    for (auto &shell : config.shell) {
      if (fs::exists(state->build_root / shell.substr(1))) {
        args.push_back(shell);
        set_shell = true;
        break;
      }
    }
    if (!set_shell) args.push_back(config.shell.size() ? config.shell[0] : "/bin/sh");
  }

  transform(args.begin(), args.end(), args.begin(), [&](string arg) {
    size_t pos = 0;
    while((pos = arg.find("$$build_root$$", pos)) != string::npos) {
      arg.replace(pos, strlen("$$build_root$$"), state->build_root);
    }
    return arg;
  });

  auto mounts = ProcMount::read();
  if (ProcMount::any_of(mounts, [&](auto mnt) { return mnt.mnt_dir == state->build_root; })) {
    cerr << state->build_root << " already mounted" << endl;
    return Stage::MKTEMP;
  }

  string options = "lowerdir=" + config.optionsLower();

  if (! config.noupper) {
    auto base = config.base;
    auto upperdir = state->build_root_orig;
    upperdir += ".upper";
    if (base) upperdir += "." + *base;
    if (! fs::is_directory(upperdir)) fs::create_directory(upperdir);
    auto workdir = state->build_root_orig;
    workdir += ".work";
    if (base) workdir += "." + *base;
    if (! fs::is_directory(workdir)) fs::create_directory(workdir);
    string upper_opts = ",upperdir=" + upperdir.string() + ",workdir=" + workdir.string();
    if (ProcMount::any_of(mounts, [&](auto mnt) { return mnt.mnt_opts.find(upper_opts) != string::npos; })) {
      cerr << "upperdir and workdir are alreadym mounted" << endl;
      return Stage::MKTEMP;
    }
    options += upper_opts;
  }

  if (config.indexoff) {
    options += ",index=off";
  }

  if (mount(state->build_root_orig, state->build_root, "overlay", 0, options)) {
    cerr << "Error mounting " << state->build_root << " " << strerror(errno) << endl;
    return Stage::MKTEMP;
  }

  if (config.newnamespace) {
    if (unshare(
      CLONE_FS |
      CLONE_NEWCGROUP |
      CLONE_NEWIPC |
      CLONE_NEWNET |
      CLONE_NEWNS |
      CLONE_NEWPID |
      // CLONE_NEWUSER |
      CLONE_NEWUTS |
      CLONE_SYSVSEM
    )) {
      cerr << "Failed to unshare namespaces " << errno << endl;
      return Stage::SYSTEM_FS;
    }
  }

  if (!config.newnamespace && !config.nosystem) {
    auto mounts = ProcMount::by(ProcMount::read(), [](auto mnt) { return mnt.mnt_dir; });
    for (auto &fs : SYSTEM_FS) {
      if (! mounts.count(fs)) {
        cerr << "System does not have " << fs << " mounted" << endl;
        return Stage::SYSTEM_FS;
      }
      auto mount_fs = mounts[fs];
      auto dst = state->build_root / fs.substr(1);
      if (mount(mount_fs.mnt_fsname, dst, mount_fs.mnt_type, 0, "")) {
        cerr << "Failed to mount " << fs << " " << strerror(errno) << endl;
        return Stage::SYSTEM_FS;
      }
      state->mounted_system_fs.push_front(dst);
    }
  }

  for (auto &bind : config.binds) {
    auto dst = state->build_root / bind.first.substr(1);
    if (! fs::exists(dst)) {
      try {
        fs::create_directory(dst);
      } catch (exception &e) {
        cerr << "Error " << e.what() << " creating missing bind destination for " << bind.first << endl;
        if (config.noupper) {
          cerr << "Likely caused by this chroot config having noupper set" << endl;
        }
        return Stage::BINDS;
      }
    }
    if (! fs::is_directory(dst)) {
      cerr << "bind mount destination " << bind.first << " is not a directory" << endl;
      return Stage::BINDS;
    }
    if (mount(bind.second, dst, "bind", MS_BIND, "")) {
      cerr << "Failed to bind mount " << dst << " " << strerror(errno) << endl;
      return Stage::BINDS;
    }
    state->mounted_binds.push_back(dst);
  }

  for (auto &tmpfs : config.tmpfs) {
    auto dst = state->build_root / tmpfs.substr(1);
    if (mount("tmpfs", dst, "tmpfs", 0, "")) {
      cerr << "Failed to tmpfs mount " << dst << " " << strerror(errno) << endl;
      return Stage::TMPFS;
    }
    state->mounted_tmpfs.push_back(dst);
  }

  if (args.empty()) {
    cerr << "Nothing to exec" << endl;
    return Stage::PROCESSES;
  }

  {
    const lock_guard<FileLock>lock(*state->mtabLock);
    ofstream mtab("mtab", ios::app);
    mtab << state->build_root_orig << " " << state->build_root << endl;
  }

  {
    pid = fork();
    if (pid == 0) {
      fs::current_path(state->build_root);
      if (! config.nochroot) {
        if (chroot(".")) {
          cerr << "Failed to chroot " << strerror(errno) << endl;
          exit(-1);
        }
        fs::current_path(config.cwd);
      }
      const char *argv[args.size() + 1];
      for (size_t i = 0; i < args.size(); i++) {
        argv[i] = args[i].c_str();
      }
      argv[args.size()] = nullptr;

      if (seteuid(getuid())) {
        cerr << "Failed to seteuid" << endl;
        exit(-1);
      }

      cerr << "execve: " << argv[0];
      for (auto &arg : args) {
        cerr << " "  << arg;
      }
      cerr << endl;

      int fdlimit = ::sysconf(_SC_OPEN_MAX);
      for (int i = 3; i < fdlimit; ++i) {
        if (state->keepfd.contains(i)) {
          cerr << "Keeping " << i << endl;
          continue;
        }
        close(i);
      }

      if (execve(argv[0], (char *const *)&argv, environ)) {
        cerr << "Failed to exec " << argv[0] << " " << strerror(errno) << endl;
        exit(-1);
      }
    } else if (pid > 0) {
      int wstatus;
      waitpid(pid, &wstatus, 0);
      state->exitstatus = WEXITSTATUS(wstatus);
    } else {
      cerr << "Failed to fork " << strerror(errno) << endl;
      return Stage::MTAB;
    }
  }
  return nullopt;
}

optional<Stage> stop(optional<Stage> stage, const Config &config, shared_ptr<State> state) {
  auto cleanup = stage ? *stage : Stage::MTAB;
  switch(cleanup) {
    case Stage::MTAB: {
      const lock_guard<FileLock>lock(*state->mtabLock);
      vector<pair<fs::path, fs::path>> mtabContent;
      {
        ifstream mtab;
        mtab.open("mtab");
        mtab.exceptions(ifstream::failbit | ifstream::eofbit);
        while(!mtab.eof()) {
          try {
            fs::path src;
            fs::path dst;
            mtab >> src;
            mtab >> dst;
            if (src == state->build_root_orig && dst == state->build_root) continue;
            mtabContent.emplace_back(src, dst);
          } catch (exception &e){
            break;
          }
        }
      }
      {
        ofstream mtab("mtab", ios::trunc);
        for (auto &e : mtabContent) {
          try {
            mtab << e.first << " " << e.second << endl;
          } catch (exception &e) {
            cerr << "Error writing mtab " << e.what() << endl;
          }
        }
      }
    }
    // FALLTHROUGH
    case Stage::PROCESSES: {
      bool killed = false;
      for (auto &p : fs::directory_iterator("/proc")) {
        auto root = p.path() / "root";
        try {
          if (fs::is_symlink(root)) {
            auto fn = p.path().filename().string();
            if (fn == "self" || fn == "thread-self") continue;
            auto pid = stoll(fn);
            if (fs::read_symlink(root) == state->build_root) {
              cerr << "Killing lingering process " << pid << endl;
              killed = true;
              if (kill(pid, SIGTERM)) {
                cerr << "Error killing process " << pid << endl;
                return Stage::PROCESSES;
              }
            }
          }
        } catch (fs::filesystem_error &e) {}
      }
      if (killed) sleep(1);
    }
    // FALLTHROUGH
    case Stage::TMPFS: {
      for (auto it = state->mounted_tmpfs.cbegin(); it != state->mounted_tmpfs.cend(); it ) {
        auto dst = *it;
        if (umount(dst)) {
          cerr << "Failed to umount tmpfs " << dst << " " << strerror(errno) << endl;
          return Stage::TMPFS;
        }
        it = state->mounted_tmpfs.erase(it);
      }
    }
    // FALLTHROUGH
    case Stage::BINDS: {
      for (auto it = state->mounted_binds.cbegin(); it != state->mounted_binds.cend(); it ) {
        auto dst = *it;
        if (umount(dst)) {
          cerr << "Failed to umount bind " << dst << " " << strerror(errno) << endl;
          return Stage::BINDS;
        }
        it = state->mounted_binds.erase(it);
      }
    }
    // FALLTHROUGH
    case Stage::SYSTEM_FS: {
      if (! config.nosystem) {
        for (auto it = state->mounted_system_fs.cbegin(); it != state->mounted_system_fs.cend(); it) {
          auto dst = *it;
          if (umount(dst)) {
            cerr << "Failed to umount " << dst << " " << strerror(errno) << endl;
            return Stage::SYSTEM_FS;
          }
          it = state->mounted_system_fs.erase(it);
        }
      }
    }
    // FALLTHROUGH
    case Stage::ROOT: {

      auto root = ProcMountInfo::read()->findMountPoint(state->build_root);
      if (root && !root->children.empty()) {
        cerr << "Found dangling mounts inside chroot:" << endl << root << endl;
        auto children = root->recursiveChildren();
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
          auto mnt = *it;
          if (umount(mnt->mount_point)) {
            cerr << "Failed to umount dangling child mount " << mnt->mount_point << endl;
            return Stage::ROOT;
          }
        }
      }

      if (umount(state->build_root)) {
        cerr << "Failed to umount" << state->build_root << " " << strerror(errno) << endl;
        return Stage::ROOT;
      }
    }
    // FALLTHROUGH
    case Stage::MKTEMP: {
      if (config.mktemp) {
        fs::remove(state->build_root);
      }
    }
    // FALLTHROUGH
    case Stage::NONE: break;
  }
  return nullopt;
}

bool check_permissions(const fs::path &config_file) {
  if (! fs::is_regular_file(config_file)) {
    cerr << config_file << " is not a regular file" << endl;
    return false;
  }
  struct stat file_stat;
  auto file_perm = fs::status(config_file).permissions();
  if (::stat(config_file.string().c_str(), &file_stat)) {
    cerr << "failed to stat " << config_file << endl;
    return false;
  }
  if (file_stat.st_uid != 0) {
    cerr << config_file << " not owned by root" << endl;
    return false;
  }
  if (file_stat.st_gid != 0) {
    if ((file_perm & fs::perms::group_write) != fs::perms::none) {
      cerr << config_file << " has insecure permissions" << endl;
      return false;
    }
  }
  if ((file_perm & fs::perms::others_write) != fs::perms::none) {
    cerr << config_file << " has insecure permissions" << endl;
    return false;
  }
  return true;
}

int main(int argc, const char *argv[]) {
  std::map<std::string, docopt::value> args
      = docopt::docopt(USAGE, { argv + 1, argv + argc }, true, "", true);

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  fs::current_path(fs::absolute(argv[0]).parent_path());

  shared_ptr<State> state;
  {
    fs::path build_root = args["<chroot-name>"].asString();

    if (build_root.has_root_path()) {
      cerr
        << "Only relative subdirectories of "
        << fs::current_path()
        << " are allowed."
        << endl;
      return 1;
    }

    if (any_of(build_root.begin(), build_root.end(), [](auto p) { return p == ".."; })) {
      cerr
        << "No .. relative operators are allowed." << endl;
      return 1;
    }
    build_root = fs::absolute(build_root);
    state = make_shared<State>(build_root);
  }

  verbose = !!args["--verbose"];

  if (args["--keepfd"]) {
    for (auto s : args["--keepfd"].asStringList()) {
      try {
        state->keepfd.insert(stoi(s));
      } catch (exception &e) {
        cerr << "Failed to convert '" << s << "' to an integer" << endl;
        return 1;
      }
    }
  }

  if (! fs::is_directory(state->build_root)) {
    cerr << state->build_root << " is not a directory" << endl;
    return 1;
  }

  auto build_file = state->build_root / ".buildroot.yaml";

  cout << state->build_root << endl;

  if (!check_permissions(build_file)) return 1;

  auto config = Config::loadFile(build_file);

  if (args["--base"]) {
    config.base = args["--base"].asString();
  }

  if (args["--print"].asBool()) {
    YAML::Node cfg(config);
    cerr << cfg << endl;
    return 99;
  }

  auto commandArgs = args["<command-or-args>"].asStringList();

  auto ret = start({commandArgs.begin(), commandArgs.end()}, config, state);

  int retries = 3;
  const bool was_error = !!ret;
  do {
    ret = stop(ret, config, state);
    if (ret) {
      cerr <<  "Error occurred whilst stopping";
      if (retries) {
        cerr << " retrying";
        sleep(1);
      }
      cerr << endl;
    }
  } while (ret && retries--);

  return state->exitstatus | (was_error || ret);
}

} // namespace

int main(int argc, const char *argv[]) {
  return chroot_venv::main(argc, argv);
}
