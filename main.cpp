#define FUSE_USE_VERSION 31

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <fuse3/fuse.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <span>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct options {
  char* myfn;
};

#define OPTION(t, p)                                                                                                   \
  { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {OPTION("", myfn), FUSE_OPT_END};

std::filesystem::path root;
struct memfs;
memfs* glbmemfs;
struct memfs {
  std::unordered_map<std::string, std::string> files;
  memfs() {
    std::cout << "READING" << std::endl;
    std::ifstream jfile(root);
    auto info = nlohmann::json::parse(jfile).get<std::unordered_map<std::string, std::string>>();
    std::vector<char> buf(1024 * 1024 * 16);
    for (auto& [destination, source] : info) {
      if (!destination.starts_with("/")) {
        throw std::runtime_error("Invalid destination: " + destination);
      }
      {
        std::string data;
        std::ifstream is(source, std::ios_base::binary);
        do {
          is.read(buf.data(), buf.size());
          data.append(std::string_view(buf.data(), is.gcount()));
        } while (is);
        files.emplace(destination, std::move(data));
      }
    }
    std::cout << "READY " << std::endl;
  }
  int getattr(std::string path, struct stat& stbuf) {
    if (is_dir(path)) {
      stbuf.st_mode = S_IFDIR | 0755;
      stbuf.st_nlink = 2;
      stbuf.st_uid = geteuid();
      stbuf.st_gid = getegid();
      return 0;
    } else if (files.contains(path)) {
      stbuf.st_mode = S_IFREG | 0444;
      stbuf.st_nlink = 1;
      stbuf.st_size = files.at(path).size();
      stbuf.st_uid = geteuid();
      stbuf.st_gid = getegid();
      return 0;
    }
    return -ENOENT;
  }
  int open(std::string path, struct fuse_file_info* fi) {
    if (files.contains(path)) {
      if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
      }
      return 0;
    }
    return -ENOENT;
  }
  int read(std::string path, std::span<char> target, off_t offset) {
    if (files.contains(path)) {
      std::string_view data(files.at(path));
      std::string_view ctx = data.substr(offset);
      ctx = ctx.substr(0, target.size());
      std::copy(ctx.begin(), ctx.end(), target.begin());
      return ctx.size();
    }
    return -ENOENT;
  }
  bool is_dir(std::string path) const {
    if (!path.ends_with("/"))
      path += "/";
    for (const auto& [filename, data] : files) {
      if (filename.starts_with(path))
        return true;
    }
    return false;
  }
  template <typename add_file_t> int readdir(std::string path, add_file_t add_file) {
    if (!path.ends_with("/"))
      path += "/";
    if (is_dir(path)) {
      add_file(".");
      add_file("..");
      std::unordered_set<std::string> items;
      for (const auto& [filename, data] : files) {
        if (filename.substr(0, path.size()) == path) {
          std::string fnp = filename.substr(path.size());
          if (fnp.find("/") != std::string::npos)
            fnp = fnp.substr(0, fnp.find("/"));
          items.insert(fnp);
        }
      }
      for (const auto& item : items) {
        if (item.size())
          add_file(item);
      }
      return 0;
    }
    return -ENOENT;
  }
};

static const struct fuse_operations hello_oper = {
    .getattr = [](const char* path, struct stat* stbuf, struct fuse_file_info* fi) -> int {
      (void)fi;
      try {
        return reinterpret_cast<memfs*>(fuse_get_context()->private_data)->getattr(path, *stbuf);
      } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -EIO;
      }
    },
    .open = [](const char* path, struct fuse_file_info* fi) -> int {
      try {
        return reinterpret_cast<memfs*>(fuse_get_context()->private_data)->open(path, fi);
      } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -EIO;
      }
    },
    .read = [](const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) -> int {
      (void)fi;
      try {
        return reinterpret_cast<memfs*>(fuse_get_context()->private_data)
            ->read(path, std::span<char>(buf, size), offset);
      } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -EIO;
      }
    },
    .readdir = [](const char* path,
                  void* buf,
                  fuse_fill_dir_t filler,
                  off_t offset,
                  struct fuse_file_info* fi,
                  enum fuse_readdir_flags flags) -> int {
      (void)offset;
      (void)fi;
      (void)flags;
      try {
        return reinterpret_cast<memfs*>(fuse_get_context()->private_data)
            ->readdir(path, [filler, buf](std::string_view name) {
              filler(buf, name.data(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
            });
      } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -EIO;
      }
    },
    .init = [](struct fuse_conn_info* conn, fuse_config* cfg) -> void* {
      (void)conn;
      cfg->kernel_cache = 1;
      return glbmemfs;
    },
    //.destroy = [](void* private_data) { delete reinterpret_cast<memfs*>(private_data); }
};

int main(int argc, char* argv[]) {
  if ((argc < 3) || (argv[argc - 2][0] == '-') || (argv[argc - 1][0] == '-')) {
    std::cerr << "Missing JSON" << std::endl;
    return 1;
  }
  root = std::filesystem::canonical(argv[argc - 2]);
  argv[argc - 2] = argv[argc - 1];
  argv[argc - 1] = NULL;
  argc--;
  glbmemfs = new memfs();

  int ret;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

  /* Parse options */
  if (fuse_opt_parse(&args, nullptr, option_spec, NULL) == -1)
    return 1;

  ret = fuse_main(args.argc, args.argv, &hello_oper, NULL);
  fuse_opt_free_args(&args);
  return ret;
}
