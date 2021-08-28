#define FUSE_USE_VERSION 31

#include <assert.h>
#include <chrono>
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
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static const struct fuse_opt option_spec[] = {FUSE_OPT_END};
using namespace std::literals;

std::filesystem::path root;
struct memfs;
memfs* cache_instance;
struct memfs {
  std::unordered_map<std::string, std::string> files;
  std::unordered_map<std::string, std::unordered_set<std::string>> dirs;
  std::ofstream errlog;
  memfs() : errlog("/tmp/missing.txt") {
    std::cout << "READING" << std::endl;
    std::ifstream jfile(root);
    auto info = nlohmann::json::parse(jfile).get<std::unordered_map<std::string, std::string>>();
    std::vector<char> buf(1024 * 1024 * 16);
    std::cout << std::endl;
    size_t cnt = 0;
    std::vector<std::string> list;
    for (const auto& [destination, source] : info)
      list.push_back(destination);
    auto last = std::chrono::steady_clock::now();
    std::sort(list.begin(), list.end(), [&info](std::string a, std::string b) { return info.at(a) < info.at(b); });
    for (const auto& destination : list) {
      auto source = info.at(destination);
      if (!destination.starts_with("/")) {
        throw std::runtime_error("Invalid destination: " + destination);
      }
      std::string pre = "/";
      for (size_t i = 0; i < destination.size(); i = destination.find("/", i + 1)) {
        std::string fnp = destination.substr(i + 1);
        if (fnp.find("/") != std::string::npos)
          fnp = fnp.substr(0, fnp.find("/"));
        dirs[pre].insert(fnp);
        pre += fnp + "/";
      }
      try {
        std::string data;
        std::ifstream is;
        is.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        is.open(source, std::ios_base::binary);
        if (!(is.good() && is.is_open()))
          throw std::runtime_error("Open Error");
        is.exceptions(std::ifstream::badbit);
        do {
          is.read(buf.data(), buf.size());
          data.append(std::string_view(buf.data(), is.gcount()));
        } while (is);
        files.emplace(destination, std::move(data));
        const auto& d = files.at(destination);
        mlock(d.data(), d.size());
      } catch (...) {
        std::cout << source << std::endl;
        throw;
      }
      cnt++;
      if ((std::chrono::steady_clock::now() - last) > 200ms) {
        last = std::chrono::steady_clock::now();
        std::string fn = destination;
        while (fn.size() < 1000)
          fn.append(" ");
        fn = fn.substr(0, 120);
        std::cout << cnt << "/" << info.size() << " " << fn << "\r" << std::flush;
      }
    }
    dirs["//"] = dirs["/"];
    std::cout << "READY " << std::endl;
  }
  int getattr(std::string path, struct stat& stbuf) {
    if (dirs.contains(path + "/")) {
      stbuf.st_mode = S_IFDIR | 0755;
      stbuf.st_nlink = 2;
      stbuf.st_uid = geteuid();
      stbuf.st_gid = getegid();
      return 0;
    } else if (files.contains(path)) {
      stbuf.st_mode = S_IFREG | 0666;
      stbuf.st_nlink = 1;
      stbuf.st_size = files.at(path).size();
      stbuf.st_uid = geteuid();
      stbuf.st_gid = getegid();
      return 0;
    }
    errlog << path << std::endl << std::flush;
    return -ENOENT;
  }
  int open(std::string path, struct fuse_file_info* fi) {
    if (files.contains(path)) {
      if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
      }
      return 0;
    }
    errlog << path << std::endl << std::flush;
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
    errlog << path << std::endl << std::flush;
    return -ENOENT;
  }
  template <typename add_file_t> int readdir(std::string path, add_file_t add_file) {
    if (!path.ends_with("/"))
      path.append("/");
    if (dirs.contains(path)) {
      add_file(".");
      add_file("..");
      for (const auto& item : dirs.at(path)) {
        if (item.size())
          add_file(item);
      }
      return 0;
    }
    errlog << path << std::endl << std::flush;
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
      return cache_instance;
    },
    .destroy =
        [](void* private_data) {
          // delete reinterpret_cast<memfs*>(private_data); // Moved to global
        }};

int main(int argc, char* argv[]) {
  if ((argc < 3) || (argv[argc - 2][0] == '-') || (argv[argc - 1][0] == '-')) {
    std::cerr << "Missing JSON" << std::endl;
    return 1;
  }
  root = std::filesystem::canonical(argv[argc - 2]);
  argv[argc - 2] = argv[argc - 1];
  argv[argc - 1] = NULL;
  argc--;
  cache_instance = new memfs();
  int ret;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  if (fuse_opt_parse(&args, nullptr, option_spec, NULL) == -1)
    return 1;
  ret = fuse_main(args.argc, args.argv, &hello_oper, NULL);
  fuse_opt_free_args(&args);
  return ret;
}
