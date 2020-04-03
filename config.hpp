#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <optional>
#include <numeric>

#include <filesystem>

#include <yaml-cpp/yaml.h>

#pragma once

namespace chroot_venv {

struct Config {
  std::optional<std::string> base;
  std::vector<std::string> lower;
  std::map<std::string, std::string> binds;
  std::vector<std::string> tmpfs;
  bool mktemp = false;
  bool noupper = false;
  bool indexoff = false;
  bool nosystem = false;
  bool nochroot = false;
  bool newnamespace = false;
  std::string cwd = "/";
  std::vector<std::string> shell = {"/bin/sh"};
  std::optional<std::string> exec;
  std::optional<std::vector<std::string>> args;
  std::map<std::string, std::string> env;

  std::string optionsLower() const;

  static Config loadFile(std::string buildFile);
  static std::map<std::string, Config> loadBuildRoots(std::string dir);
};

} // namespace

namespace YAML {

template<>
struct convert<chroot_venv::Config> {
  static Node encode(const chroot_venv::Config& rhs);
  static bool decode(const Node &node, chroot_venv::Config& rhs);
};

} // namespace
