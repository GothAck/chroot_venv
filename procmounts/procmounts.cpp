#include <fstream>
#include <sstream>
#include <iostream>

#include "procmounts.hpp"

namespace procmounts {

using namespace std;

istream &operator >>(istream &is, ProcMount &o) {
  is >> o.mnt_fsname;
  is >> o.mnt_dir;
  is >> o.mnt_type;
  is >> o.mnt_opts;
  is >> o.mnt_freq;
  is >> o.mnt_passno;
  return is;
}

const vector<ProcMount> ProcMount::read(std::string path) {
  vector<ProcMount> ret;
  ifstream file;
  file.open(path);
  file.exceptions(ifstream::failbit | ifstream::eofbit);
  while (! file.eof()) {
    try {
      ProcMount mnt;
      file >> mnt;
      ret.push_back(mnt);
    } catch (ifstream::failure &e) {
      break;
    }
  }
  return ret;
}

const map<string, ProcMount> ProcMount::by(const vector<ProcMount> &mounts, std::function<string(const ProcMount &)> fn) {
  map<string, ProcMount> ret;
  for (auto &mnt : mounts) {
    ret[fn(mnt)] = mnt;
  }
  return ret;
}

bool ProcMount::any_of(const std::vector<ProcMount> &mounts, std::function<bool(const ProcMount &)> fn) {
  return std::any_of(mounts.cbegin(), mounts.cend(), fn);
}

istream &operator >>(istream &is, ProcMountInfo &o) {
  is >> o.mount_id;
  is >> o.parent_id;
  is >> o.major_minor;
  is >> o.root;
  is >> o.mount_point;
  is >> o.options;
  std::string field;
  is >> field;
  while (field != "-") {
    auto separator = field.find(':');
    string key = field.substr(0, separator);
    string val = separator == string::npos ? "" : field.substr(separator + 1);
    o.optional_fields[key] = val;
    is >> field;
  }
  is >> o.filesystem;
  is >> o.source;
  is >> o.super_options;
  return is;
}

const ProcMountInfo::TShared ProcMountInfo::read(int pid) {
  string path = "/proc/" + to_string(pid) + "/mountinfo";
  return read(path);
}

const ProcMountInfo::TShared ProcMountInfo::read(std::string path) {
  TMap<size_t> mounts;

  ifstream file;
  file.open(path);
  file.exceptions(ifstream::failbit | ifstream::eofbit);
  while (! file.eof()) {
    try {
      auto mnt = make_shared<ProcMountInfo>();;
      file >> *mnt;
      mounts.insert({ mnt->mount_id, mnt });
    } catch (ifstream::failure &e) {
      break;
    }
  }

  TShared root;
  for (auto it = mounts.cbegin(); it != mounts.cend(); ++it) {
    auto p = *it;
    auto mnt_ptr = p.second;
    auto id = p.first;
    auto parent_id = mnt_ptr->parent_id;
    if (mounts.contains(parent_id)) {
      auto parent_ptr = mounts.at(parent_id);
      parent_ptr->children.push_back(mnt_ptr);
      mnt_ptr->parent = parent_ptr;
    } else {
      if (root) {
        cerr << "Multiple roots found " << root->mount_id << " " << mnt_ptr->mount_id << endl;
      }
      root = mnt_ptr;
    }
  }
  if (!root) {
    cerr << "No root found" << endl;
  }
  return root;
}

ProcMountInfo::TVec ProcMountInfo::recursiveChildren() const {
  TVec ret;
  for (auto c : children) {
    ret.push_back(c);
    for (auto cc : c->recursiveChildren()) {
      ret.push_back(cc);
    }
  }
  return ret;
}

ProcMountInfo::TSharedConst ProcMountInfo::findMountPoint(string find_mount_point) const {
  if (mount_point == find_mount_point) return shared_from_this();
  for (auto c : children) {
    auto ret = c->findMountPoint(find_mount_point);
    if (ret) return ret;
  }
  return nullptr;
}

ProcMountInfo::TMap<string> ProcMountInfo::by(std::function<string(const TShared &)> fn) const {
  TMap<string> ret;
  for (auto mnt : recursiveChildren()) {
    ret[fn(mnt)] = mnt;
  }
  return ret;
}

bool ProcMountInfo::anyOf(std::function<bool(const TShared &)> fn) const {
  auto rec = recursiveChildren();
  return std::any_of(rec.cbegin(), rec.cend(), [fn](auto it) {
    return fn(it);
  });
}

std::ostream &operator <<(std::ostream &os, const shared_ptr<ProcMountInfo> &o) {
  if (o) return os << *o;
  return os << "Invalid pointer";
}

std::ostream &operator <<(std::ostream &os, const shared_ptr<const ProcMountInfo> &o) {
  if (o) return os << *o;
  return os << "Invalid pointer";
}

std::ostream &operator <<(std::ostream &os, const ProcMountInfo &o) {
  os
    << "id: " << o.mount_id
    << " parent: " << o.parent_id
    << " major_minor: " << o.major_minor
    << " root: " << o.root
    << " mount_point: " << o.mount_point
    << " options: " << o.options
    << " optional_fields: {";

  for (auto &pair : o.optional_fields) {
    os << " [" << pair.first << "=" << pair.second << "] ";
  }

  os
    << "} filesystem: " << o.filesystem
    << " source: " << o.source
    << " super_options: " << o.super_options;
  if (!o.children.empty()) {
    os << endl << "children:";
    stringstream ss_children;
    for (auto child: o.children) {
      ss_children << endl << child;
    }
    for (string line; getline(ss_children, line);) {
      if (line.empty()) continue;
      os << endl << "  " << line;
    }
  }
  return os;
}

} // namespace
