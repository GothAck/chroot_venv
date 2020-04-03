#include <string>
#include <vector>
#include <map>
#include <functional>
#include <iostream>
#include <memory>

#pragma once

namespace procmounts {

struct ProcMount {
  using TVec = std::vector<ProcMount>;
  using TMap  = std::map<std::string, ProcMount>;
  std::string mnt_fsname;
  std::string mnt_dir;
  std::string mnt_type;
  std::string mnt_opts;
  int mnt_freq;
  int mnt_passno;

  static const TVec read(std::string path = "/proc/self/mounts");
  static const TMap by(const TVec &mounts, std::function<std::string(const ProcMount &)> fn);

  template<std::string ProcMount::*ptr>
  static const TMap byPtr(const TVec &mounts) {
    return by(mounts, [](auto mnt) {
      return mnt.*ptr;
    });
  }

  static bool any_of(const TVec &mounts, std::function<bool(const ProcMount &)> fn);
};

struct ProcMountInfo : std::enable_shared_from_this<ProcMountInfo> {
  using TShared = std::shared_ptr<ProcMountInfo>;
  using TWeak = std::weak_ptr<ProcMountInfo>;
  using TSharedConst = std::shared_ptr<const ProcMountInfo>;
  using TVec = std::vector<TShared>;
  template<typename TKey>
  using TMap  = std::map<TKey, TShared>;

  size_t mount_id;
  size_t parent_id;
  std::string major_minor;
  std::string root;
  std::string mount_point;
  std::string options;
  std::map<std::string, std::string> optional_fields;
  // separator
  std::string filesystem;
  std::string source;
  std::string super_options;

  TWeak parent;
  TVec children;


  static const TShared read(int pid);
  static const TShared read(std::string path = "/proc/self/mountinfo");

  TVec recursiveChildren() const;
  TSharedConst findMountPoint(std::string find_mount_point) const;
  TMap<std::string> by(std::function<std::string(const TShared &)> fn) const;

  template<std::string ProcMountInfo::*ptr>
  TMap<std::string> byPtr() const {
    return by([](auto mnt) {
      return mnt.*ptr;
    });
  }

  bool anyOf(std::function<bool(const TShared &)> fn) const;
};

std::ostream &operator <<(std::ostream &os, const std::shared_ptr<ProcMountInfo> &o);
std::ostream &operator <<(std::ostream &os, const std::shared_ptr<const ProcMountInfo> &o);
std::ostream &operator <<(std::ostream &os, const ProcMountInfo &o);

} // namespace
