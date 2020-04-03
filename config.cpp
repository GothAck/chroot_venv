#include <filesystem>

#include "config.hpp"

using namespace std;
namespace fs = filesystem;

namespace chroot_venv {

string Config::optionsLower() const {
  string options;
  auto lowerCopy = lower;
  if (base && fs::is_directory(*base)) {
    lowerCopy.insert(lowerCopy.begin(), *base);
  }
  for (auto it = lowerCopy.rbegin(); it != lowerCopy.rend(); ++it) {
    auto lowerStr = *it;
    if (base) {
      auto based = lowerStr + "." + *base;
      if (fs::is_directory(based)) {
        lowerStr = based;
      }
    }
    if (fs::is_directory(lowerStr)) {
      if (!options.empty()) options += ":";
      options += lowerStr;
    }
  }
  return options;
}

Config Config::loadFile(string build_file) {
  return YAML::LoadFile(build_file).as<Config>();
}

map<string, Config> Config::loadBuildRoots(string dir) {
  fs::path path(dir);
  map<string, Config> ret;
  if (!fs::is_directory(path))
    return ret;
  for (auto p : fs::directory_iterator(path)) {
    auto pp = p.path();
    if (!fs::is_directory(pp))
      continue;
    pp /= ".buildroot.yaml";
    if (!fs::is_regular_file(pp))
      continue;
    try {
      ret[p.path()] = loadFile(pp);
    } catch (exception &e) {
      continue;
    }
  }

  return ret;
}

} // namespace

namespace YAML {

Node convert<chroot_venv::Config>::encode(const chroot_venv::Config& rhs) {
  Node node;
  if (rhs.base) node["base"] = *rhs.base;
  node["lower"] = rhs.lower;
  node["binds"] = rhs.binds;
  node["tmpfs"] = rhs.tmpfs;
  node["mktemp"] = rhs.mktemp;
  node["noupper"] = rhs.noupper;
  node["indexoff"] = rhs.indexoff;
  node["nosystem"] = rhs.nosystem;
  node["nochroot"] = rhs.nochroot;
  node["newnamespace"] = rhs.newnamespace;
  node["cwd"] = rhs.cwd;
  if (rhs.shell.size() == 1) {
    node["shell"] = rhs.shell[0];
  } else {
    node["shell"] = rhs.shell;
  }
  if (rhs.exec) node["exec"] = *rhs.exec;
  if (rhs.args) node["args"] = *rhs.args;
  node["env"] = rhs.env;

  return node;
}

bool convert<chroot_venv::Config>::decode(const Node &node, chroot_venv::Config& rhs) {
  if (node["base"])     rhs.base = node["base"].as<string>();
  if (node["lower"])    rhs.lower = node["lower"].as<vector<string>>();
  if (node["binds"])    rhs.binds = node["binds"].as<map<string, string>>();
  if (node["tmpfs"])    rhs.tmpfs = node["tmpfs"].as<vector<string>>();
  if (node["mktemp"])   rhs.mktemp = node["mktemp"].as<bool>();
  if (node["noupper"])  rhs.noupper = node["noupper"].as<bool>();
  if (node["indexoff"])  rhs.indexoff = node["indexoff"].as<bool>();
  if (node["nosystem"]) rhs.nosystem = node["nosystem"].as<bool>();
  if (node["nochroot"]) rhs.nochroot = node["nochroot"].as<bool>();
  if (node["newnamespace"]) rhs.newnamespace = node["newnamespace"].as<bool>();
  if (node["cwd"])      rhs.cwd = node["cwd"].as<string>();
  if (node["shell"]) {
    auto shell = node["shell"];
    if (shell.IsSequence()) {
      rhs.shell = node["shell"].as<vector<string>>();
    } else {
      rhs.shell = vector<string> { node["shell"].as<string>() };
    }
  }
  if (node["exec"])     rhs.exec = node["exec"].as<string>();
  if (node["args"])     rhs.args = node["args"].as<vector<string>>();
  if (node["env"])      rhs.env = node["env"].as<map<string, string>>();
  return true;
}

} // namespace
