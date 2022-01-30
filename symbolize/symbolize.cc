#include "./symbolize.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

void RawWrite(int fd, const char* buf, int bytes) {
  while (bytes > 0) {
    int n = write(fd, buf, bytes);
    if (n <= 0) {
      if (errno == EINTR) continue;
      write(2, "write failed\n", 5);
      return;
    }
    buf += n;
    bytes -= n;
  }
}

void RawVprintf(const char* format, va_list ap) {
  char buf[512];
  const int n = vsnprintf(buf, sizeof(buf), format, ap);
  RawWrite(2, buf, n);
}

void RawPrintf(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  RawVprintf(format, ap);
  va_end(ap);
}

void RawPerror(const char* format, ...) {
  int saved_errno = errno;
  va_list ap;
  va_start(ap, format);
  RawVprintf(format, ap);
  va_end(ap);
  const char* errno_msg = strerror(saved_errno);
  RawWrite(2, ": ", 2);
  RawWrite(2, errno_msg, strlen(errno_msg));
  RawWrite(2, "\n", 1);
}

class Spinlock {
 public:
  Spinlock() = default;
  Spinlock(const Spinlock& other) = delete;
  Spinlock& operator=(const Spinlock& other) = delete;

  void Lock() {
    intptr_t expected(0);
    while (
        !lock_.compare_exchange_weak(expected, 0, std::memory_order_acquire)) {
      ;
    }
  }
  void Unlock() {
    if (lock_ != 0) RawPrintf("Error: unlocking an unlocked spinlock");
    lock_.store(0, std::memory_order_release);
  }

 private:
  using Word = std::atomic_intptr_t;
  Word lock_ = 0;
};

class SpinlockGuard {
 public:
  SpinlockGuard(const SpinlockGuard& other) = delete;
  SpinlockGuard& operator=(const SpinlockGuard& other) = delete;
  explicit SpinlockGuard(Spinlock* l) : l_(l) { l_->Lock(); }
  ~SpinlockGuard() { l_->Unlock(); }

 private:
  Spinlock* const l_;
};

// Helper for reading a file line by line.
class FileReader {
 public:
  explicit FileReader(int fd) : fd_(fd) {}

  static constexpr int kEof = -1;
  static constexpr int kError = -2;

  // Seek the reader to the given absolute offset.
  bool Seek(off_t off) {
    if (lseek(fd_, off, SEEK_SET) != off) {
      RawPerror("Error: lseek %d", off);
      return false;
    }
    buf_start_ = buf_limit_ = 0;
    return true;
  }

  // Read one line, including the trailing '\n', from the file.  On success, it
  // returns the length of the line, in bytes.  It returns kEof at the end of
  // file, and kError on any other error.
  int Readline(char* buf, int max_len) {
    int n = 0;
    max_len--;  // leave space for '\0'.
    while (max_len > 0) {
      if (buf_start_ == buf_limit_) {
        if (auto r = Fill(); r < 0) {
          if (r == kEof) {
            if (n > 0) return n;
            return kEof;
          }
          RawPerror("read /proc/self/maps (%d)", r);
          return r;
        }
      }
      int ch = buf_[buf_start_++];
      *buf++ = ch;
      n++;
      max_len--;
      if (ch == '\n') break;
    }
    *buf = '\0';
    return n;
  }

 private:
  int fd_ = -1;
  int buf_start_ = 0;
  int buf_limit_ = 0;
  static constexpr int kBufSize = 256;
  char buf_[kBufSize];

  int Fill() {
    for (;;) {
      int n = read(fd_, buf_, kBufSize);
      if (n < 0) {
        if (errno == EINTR) continue;
        return kError;
      }
      buf_start_ = 0;
      buf_limit_ = n;
      break;
    }
    if (buf_limit_ == 0) return kEof;
    return buf_limit_;
  }
};

class MemoryArena {
 public:
  // It returns an unaligned pointer. It may not work on all CPUs.
  template <typename T>
  T* Alloc(int n) {
    if (next_ > 0) {
      next_ = ((next_ - 1) / alignof(T) + 1) * alignof(T);
    }
    if (next_ + n * sizeof(T) >= kSize) {
      RawPrintf("could not allocate %d*%d bytes", n * sizeof(T));
      abort();
    }
    T* p = reinterpret_cast<T*>(arena_ + next_);
    next_ += n * sizeof(T);
    return p;
  }

  char* Strdup(char* s) {
    const int len = strlen(s);
    char* buf = Alloc<char>(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
  }

 private:
  static constexpr int kSize = 4 << 20;
  int next_ = 0;
  uint8_t arena_[kSize];
};

struct MemoryMapEntry {
  intptr_t start_addr;
  intptr_t limit_addr;
  intptr_t offset;
  char* path;
};

Spinlock g_lock;
bool g_initialized;
MemoryArena g_arena;

class MemoryMap {
 public:
  void Reserve(int max_ents) {
    max_ents_ = max_ents;
    ents_ = g_arena.Alloc<MemoryMapEntry>(max_ents);
  }
  void AddEntry(MemoryMapEntry ent) {
    if (n_ents_ >= max_ents_) {
      RawPrintf("too many ents %d >= %d", n_ents_, max_ents_);
      abort();
    }
    auto* dest = &ents_[n_ents_++];
    *dest = ent;
  }
  const MemoryMapEntry* Find(intptr_t addr) {
    for (int i = 0; i < n_ents_; i++) {
      const auto* ent = &ents_[i];
      if (addr >= ent->start_addr && addr < ent->limit_addr) return ent;
    }
    return nullptr;
  }

 private:
  int n_ents_ = 0;
  int max_ents_ = 0;
  MemoryMapEntry* ents_ = nullptr;
};

class FileCloser {
 public:
  explicit FileCloser(int fd) : fd_(fd) {}
  FileCloser(const FileCloser&) = delete;
  FileCloser& operator=(const FileCloser&) = delete;
  ~FileCloser() { close(fd_); }

 private:
  const int fd_;
};

MemoryMap g_mmap;

MemoryMap ReadMemoryMap() {
  MemoryMap mmap;
  char buf[2048];
  int n_lines = 0;

  const int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) {
    RawPerror("Error: open /proc/self/maps");
    return mmap;
  }
  FileCloser close_fd(fd);
  FileReader reader(fd);
  {
    for (;;) {
      const int n = reader.Readline(buf, sizeof(buf));
      if (n == FileReader::kEof) break;
      if (n < 0) {
        close(fd);
        return mmap;
      }
      n_lines++;
    }
  }
  if (!reader.Seek(0)) return mmap;
  mmap.Reserve(n_lines);
  for (;;) {
    const int n = reader.Readline(buf, sizeof(buf));
    if (n < 0) break;
    buf[n] = '\0';
    intptr_t start_addr, limit_addr, offset;
    char prot[5];
    int ino0, ino1;
    ssize_t size;
    char path[2048];
    int sn = sscanf(buf, "%lx-%lx %4s %lx %d:%d %ld %s", &start_addr,
                    &limit_addr, prot, &offset, &ino0, &ino1, &size, path);
    if (!sn) {
      RawPrintf("Warning: ignore maps line: [%s]\n", buf);
      continue;
    }
    // printf("Read: [%s] n=%d %lx %lx [%s]\n", buf, sn, start_addr, limit_addr,
    // path);
    mmap.AddEntry(MemoryMapEntry{
        .start_addr = start_addr,
        .limit_addr = limit_addr,
        .offset = offset,
        .path = g_arena.Strdup(path),
    });
  }
  return mmap;
}

void MaybeInitialize() {
  if (!g_initialized) {
    g_mmap = ReadMemoryMap();
  }
}

class SymbolCache {
  struct Entry {
    intptr_t addr;
    char* symbol;
    Entry* next;
  };

  char* Get(intptr_t addr) {
    const auto h = Hash64(addr);
    Entry* e = buckets_[h];
    while (e) {
      if (e->addr == addr) return e->symbol;
      e = e->next;
    }
    return nullptr;
  }

  void Set(intptr_t addr, char* symbol) {
    const auto h = Hash64(addr);
    auto* e = g_arena.Alloc<Entry>(1);
    e->addr = addr;
    e->symbol = g_arena.Strdup(symbol);
    e->next = buckets_[h];
    buckets_[h] = e;
  }

 private:
  intptr_t Hash64(intptr_t val) {
    static_assert(sizeof(intptr_t) == 8);
    static constexpr intptr_t k0 = 0xc3a5c85c97cb3127ULL;
    auto h = k0 * val;
    return (h >> 21) % kTableSize;
  }

  static constexpr int kTableSize = 2048;
  Entry* buckets_[kTableSize] = {
      nullptr,
  };
};

// Manages a single instance of addr2line.  Start() starts addr2line with the
// given executable / so file. Start() may be called repeatedly, in which case
// the old addr2line instance will be killed.
class Addr2line {
 public:
  // Start looking up the given executable or so file.
  void Start(const char* exec_path) {
    CloseAll();
    if (pipe(stdin_) != 0) {
      RawPerror("pipe (stdin)");
      CloseAll();
      return;
    }
    if (pipe(stdout_) != 0) {
      RawPerror("pipe (stdout)");
      CloseAll();
      return;
    }
    pid_ = fork();
    if (pid_ == 0) {
      close(stdin_[1]);
      close(stdout_[0]);
      if (dup2(stdin_[0], 0) != 0) {
        RawPerror("dup2 0");
      }
      if (dup2(stdout_[1], 1) != 1) {
        RawPerror("dup2 1");
      }
      execl("/usr/bin/addr2line", "/usr/bin/addr2line", "-p", "-f", "-C", "-e",
            exec_path, nullptr);
      return;
    }
    close(stdin_[0]);
    stdin_[0] = -1;
    close(stdout_[1]);
    stdout_[1] = -1;
    exec_path_ = exec_path;
  }

  // Find the function and file/line for the given address in the executable.
  // If found, it stores result in human-readable string in buf, and returns
  // true.
  bool Lookup(intptr_t addr, char* buf, int buf_size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    last_use_time_ = ts.tv_sec + ts.tv_nsec / 1e9;

    snprintf(buf, buf_size, "0x%lx\n", addr);
    RawWrite(stdin_[1], buf, strlen(buf));

    FileReader r(stdout_[0]);
    int n = r.Readline(buf, buf_size);
    if (n <= 0) {
      RawPerror("read addr");
      CloseAll();
      return false;
    }
    buf[n - 1] = '\0';  // Remove the trailing '\n'.
    return true;
  }

  // Reports the path given to the last Start() call.
  const char* exec_path() const { return exec_path_; }

  // Reports the time of last use, as #seconds since 1970-1-1.
  double last_use_time() const { return last_use_time_; }

 private:
  const char* exec_path_ = nullptr;
  // stdin_[0] becomes addr2line's stdin. stdin_[1] is used to feed data to
  // addr2line.
  int stdin_[2] = {-1};
  // stdout_[1] becomes addr2line's stdout. stdout_[0] is used to read output
  // from addr2line.
  int stdout_[2] = {-1};
  // pid of addr2line.
  pid_t pid_ = 0;
  // Time of last call to Lookup. #seconds from 1970-1-1.
  double last_use_time_;

  void MaybeClose(int* fd) {
    if (*fd >= 0) {
      close(*fd);
      *fd = -1;
    }
  }
  void CloseAll() {
    MaybeClose(&stdin_[0]);
    MaybeClose(&stdin_[1]);
    MaybeClose(&stdout_[0]);
    MaybeClose(&stdout_[1]);
    if (pid_ > 0) {
      kill(pid_, 9);
      int status;
      waitpid(pid_, &status, 0);
      pid_ = 0;
    }
  }
};

constexpr int kMaxAddr2lines = 6;
Addr2line g_addr2line[kMaxAddr2lines];

// Find an Addr2line instance that has opened the same executable/so. If not,
// repurpose the LRU one.
Addr2line* FindOrCreateAddr2lineInstance(const char* path) {
  Addr2line* oldest = nullptr;
  for (int i = 0; i < kMaxAddr2lines; i++) {
    auto* a = &g_addr2line[i];
    if (a->exec_path() && strcmp(a->exec_path(), path) == 0) {
      return a;
    }
    if (!oldest || a->last_use_time() < oldest->last_use_time()) {
      oldest = a;
    }
  }
  oldest->Start(path);
  return oldest;
}

}  // namespace

bool symbolize::Symbolize(intptr_t addr, char* buf, int buf_size) {
  SpinlockGuard lock(&g_lock);
  MaybeInitialize();
  auto* ent = g_mmap.Find(addr);
  if (!ent) return false;

  Addr2line* addr2line = FindOrCreateAddr2lineInstance(ent->path);
  return addr2line->Lookup(addr - ent->start_addr + ent->offset, buf, buf_size);
}
