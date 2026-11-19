// 日志性能基准测试（AI 编写的压测工具，不属于库代码）
//
// 对比三种写入方式：
//   - sync-buffered : 直接写本地文件，只调用 fwrite（依赖 FILE* 默认缓冲，约 4KB 刷一次）
//   - sync-flush    : 直接写本地文件，每条 fwrite 后立刻 fflush（每条都进内核 Page Cache）
//   - async         : 使用 netlib::AsyncLogging（4MB 双缓冲 + 后台线程批量刷盘）
//
// 覆盖两类负载：
//   - backend : 纯后端写入（固定 100 字节预格式化日志行），衡量日志后端本身开销
//   - logger  : 端到端调用 LOG_INFO 宏，包含时间戳格式化、ostringstream 拼接与后端写入
//
// 用法：
//   log_bench [--dir <临时目录>]
//   log_bench --single <backend|logger> <sync-buffered|sync-flush|async> <threads> <count>
//             [--dir <临时目录>]

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "netlib/base/async_logging.h"
#include "netlib/base/logging.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class Workload { kBackend, kLogger };
enum class Mode { kSyncBuffered, kSyncFlush, kAsync };

// 纯后端测试用的固定日志行：精确 100 字节（含结尾 '\n'），便于换算 MB/s 与校验零丢弃
constexpr char kFixedLine[] =
    "2026-11-19 14:00:00.123456 INFO  [bench#1] hello netlib async logging benchmark "
    "0123456789abcdefghi\n";
constexpr std::size_t kFixedLineLen = sizeof(kFixedLine) - 1;
static_assert(kFixedLineLen == 100, "kFixedLine must be exactly 100 bytes");

// 默认压测规模：
// 50 万条 * ~100 字节 ≈ 50 MB，足以触发十几次 4MB 缓冲切换，又不会触发 100MB 丢弃阈值
constexpr std::size_t kThroughputCount = 500000;
constexpr std::size_t kLatencyCount = 100000;

const char* WorkloadName(Workload w) {
  return w == Workload::kBackend ? "backend" : "logger";
}

const char* ModeName(Mode m) {
  switch (m) {
    case Mode::kSyncBuffered:
      return "sync-buffered";
    case Mode::kSyncFlush:
      return "sync-flush";
    case Mode::kAsync:
      return "async";
  }
  return "unknown";
}

/// 同步文件输出封装（RAII 管理 FILE*）
class SyncFileSink {
public:
  SyncFileSink(const std::string& path, bool flush_each) : flush_each_(flush_each) {
    // "we"：截断/新建写入 + O_CLOEXEC
    fp_ = std::fopen(path.c_str(), "we");
    if (!fp_) {
      std::perror("fopen");
      std::exit(1);
    }
  }

  ~SyncFileSink() { Close(); }

  void Write(const char* data, std::size_t len) {
    std::fwrite(data, 1, len, fp_);
    if (flush_each_) {
      std::fflush(fp_);
    }
  }

  void Flush() {
    if (fp_) {
      std::fflush(fp_);
    }
  }

  void Close() {
    if (fp_) {
      std::fflush(fp_);
      std::fclose(fp_);
      fp_ = nullptr;
    }
  }

private:
  std::FILE* fp_ = nullptr;
  bool flush_each_ = false;
};

/// 线程起跑栅栏：确保所有工作线程创建完毕后同时开始计时，避免把建线程开销算进去
class StartBarrier {
public:
  explicit StartBarrier(int total_threads) : total_threads_(total_threads) {}

  /// 工作线程调用：报到并等待主线程发令
  void ArriveAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++ready_count_;
    if (ready_count_ == total_threads_) {
      ready_cv_.notify_one();
    }
    go_cv_.wait(lock, [this] { return go_; });
  }

  /// 主线程调用：等所有工作线程报到完毕
  void WaitAllReady() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_cv_.wait(lock, [this] { return ready_count_ == total_threads_; });
  }

  /// 主线程调用：发令同时起跑
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      go_ = true;
    }
    go_cv_.notify_all();
  }

private:
  const int total_threads_;
  int ready_count_ = 0;
  bool go_ = false;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::condition_variable go_cv_;
};

/// 退出作用域时恢复 Logger 默认输出，防止悬空回调
class LoggerOutputGuard {
public:
  ~LoggerOutputGuard() {
    netlib::Logger::SetLogOutput(nullptr);
    netlib::Logger::SetLogFlush(nullptr);
  }
};

struct FileCheck {
  std::size_t bytes = 0;
  std::size_t lines = 0;
};

/// 统计落盘文件的字节数与换行数（在计时结束后执行，不影响压测结果）
FileCheck InspectLogFile(const std::string& path) {
  FileCheck result;
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) {
    result.bytes = static_cast<std::size_t>(st.st_size);
  }

  std::FILE* fp = std::fopen(path.c_str(), "re");
  if (!fp) {
    return result;
  }

  std::vector<char> buf(64 * 1024);
  while (true) {
    std::size_t n = std::fread(buf.data(), 1, buf.size(), fp);
    if (n == 0) {
      break;
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        ++result.lines;
      }
    }
  }
  std::fclose(fp);
  return result;
}

std::string MakeTempLogPath(const std::string& dir, const char* tag) {
  return dir + "/netlib_log_bench_" + std::to_string(::getpid()) + "_" + tag + ".log";
}

struct ThroughputResult {
  double front_sec = 0.0;      // 业务线程把全部日志交出去的耗时（不含 Stop/Close）
  double total_sec = 0.0;      // 含最终刷盘（Stop/Close）的总耗时
  double throughput = 0.0;     // 前端吞吐（条/秒）
  double mb_per_sec = 0.0;     // 前端吞吐（MB/s）
  double avg_us = 0.0;         // 业务线程单条平均耗时（微秒，按总墙钟时间/条数）
  std::size_t file_bytes = 0;  // 实际落盘字节数
  std::size_t file_lines = 0;  // 实际落盘行数
  bool intact = false;         // 行数/字节数是否与预期完全一致
};

ThroughputResult RunThroughput(Workload workload, Mode mode, int num_threads,
                               std::size_t total_count, const std::string& dir) {
  const std::string path = MakeTempLogPath(dir, "tp");
  // AsyncLogging 以追加模式打开，开跑前必须清理同名旧文件
  std::remove(path.c_str());

  std::unique_ptr<SyncFileSink> sync_sink;
  std::unique_ptr<netlib::AsyncLogging> async_log;
  LoggerOutputGuard guard;

  // 准备好后端，并按需绑定到 Logger
  std::function<void(const char*, std::size_t)> write_fn;
  if (mode == Mode::kAsync) {
    async_log = std::make_unique<netlib::AsyncLogging>(path);
    async_log->Start();
    write_fn = [&async_log](const char* data, std::size_t len) { async_log->Append(data, len); };
  } else {
    sync_sink = std::make_unique<SyncFileSink>(path, mode == Mode::kSyncFlush);
    write_fn = [&sync_sink](const char* data, std::size_t len) { sync_sink->Write(data, len); };
  }

  if (workload == Workload::kLogger) {
    netlib::Logger::SetLogOutput(write_fn);
    if (sync_sink) {
      netlib::Logger::SetLogFlush([&sync_sink] { sync_sink->Flush(); });
    } else {
      netlib::Logger::SetLogFlush(nullptr);
    }
  }

  StartBarrier barrier(num_threads);
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(num_threads));

  const std::size_t base_count = total_count / static_cast<std::size_t>(num_threads);
  const std::size_t remainder = total_count % static_cast<std::size_t>(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    const std::size_t my_count = base_count + (t == 0 ? remainder : 0);
    workers.emplace_back([&, my_count] {
      barrier.ArriveAndWait();
      if (workload == Workload::kBackend) {
        for (std::size_t i = 0; i < my_count; ++i) {
          write_fn(kFixedLine, kFixedLineLen);
        }
      } else {
        for (std::size_t i = 0; i < my_count; ++i) {
          LOG_INFO << "bench payload 0123456789abcdef seq=" << i;
        }
      }
    });
  }

  barrier.WaitAllReady();
  const auto t0 = Clock::now();
  barrier.Release();

  for (auto& th : workers) {
    th.join();
  }
  const auto t1 = Clock::now();

  // 收尾刷盘：同步模式 flush + close，异步模式 Stop() 等后台写完所有残留缓冲
  if (async_log) {
    async_log->Stop();
  } else {
    sync_sink->Close();
  }
  const auto t2 = Clock::now();

  const FileCheck check = InspectLogFile(path);
  std::remove(path.c_str());

  ThroughputResult res;
  res.front_sec = std::chrono::duration<double>(t1 - t0).count();
  res.total_sec = std::chrono::duration<double>(t2 - t0).count();
  res.throughput = static_cast<double>(total_count) / res.front_sec;
  res.mb_per_sec = (static_cast<double>(check.bytes) / (1024.0 * 1024.0)) / res.front_sec;
  res.avg_us = (res.front_sec * 1e6) / static_cast<double>(total_count);
  res.file_bytes = check.bytes;
  res.file_lines = check.lines;
  if (workload == Workload::kBackend) {
    res.intact = (check.lines == total_count && check.bytes == total_count * kFixedLineLen);
  } else {
    res.intact = (check.lines == total_count);
  }
  return res;
}

struct LatencyResult {
  double avg_us = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double max_us = 0.0;
  bool intact = false;
};

double PercentileUs(const std::vector<uint64_t>& sorted_ns, double ratio) {
  if (sorted_ns.empty()) {
    return 0.0;
  }
  std::size_t idx = static_cast<std::size_t>(ratio * static_cast<double>(sorted_ns.size() - 1));
  return static_cast<double>(sorted_ns[idx]) / 1000.0;
}

LatencyResult RunLatency(Workload workload, Mode mode, int num_threads, std::size_t total_count,
                         const std::string& dir) {
  const std::string path = MakeTempLogPath(dir, "lat");
  std::remove(path.c_str());

  std::unique_ptr<SyncFileSink> sync_sink;
  std::unique_ptr<netlib::AsyncLogging> async_log;
  LoggerOutputGuard guard;

  std::function<void(const char*, std::size_t)> write_fn;
  if (mode == Mode::kAsync) {
    async_log = std::make_unique<netlib::AsyncLogging>(path);
    async_log->Start();
    write_fn = [&async_log](const char* data, std::size_t len) { async_log->Append(data, len); };
  } else {
    sync_sink = std::make_unique<SyncFileSink>(path, mode == Mode::kSyncFlush);
    write_fn = [&sync_sink](const char* data, std::size_t len) { sync_sink->Write(data, len); };
  }

  if (workload == Workload::kLogger) {
    netlib::Logger::SetLogOutput(write_fn);
    if (sync_sink) {
      netlib::Logger::SetLogFlush([&sync_sink] { sync_sink->Flush(); });
    } else {
      netlib::Logger::SetLogFlush(nullptr);
    }
  }

  // 每个线程独占一个预分配好的延迟数组，避免循环内扩容或共享写入
  std::vector<std::vector<uint64_t>> per_thread_ns(static_cast<std::size_t>(num_threads));
  const std::size_t base_count = total_count / static_cast<std::size_t>(num_threads);
  const std::size_t remainder = total_count % static_cast<std::size_t>(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    const std::size_t my_count = base_count + (t == 0 ? remainder : 0);
    per_thread_ns[static_cast<std::size_t>(t)].reserve(my_count);
  }

  StartBarrier barrier(num_threads);
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(num_threads));

  for (int t = 0; t < num_threads; ++t) {
    const std::size_t my_count = base_count + (t == 0 ? remainder : 0);
    auto& samples = per_thread_ns[static_cast<std::size_t>(t)];
    workers.emplace_back([&, my_count] {
      barrier.ArriveAndWait();
      if (workload == Workload::kBackend) {
        for (std::size_t i = 0; i < my_count; ++i) {
          const auto s0 = Clock::now();
          write_fn(kFixedLine, kFixedLineLen);
          const auto s1 = Clock::now();
          samples.push_back(static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
        }
      } else {
        for (std::size_t i = 0; i < my_count; ++i) {
          const auto s0 = Clock::now();
          LOG_INFO << "bench payload 0123456789abcdef seq=" << i;
          const auto s1 = Clock::now();
          samples.push_back(static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
        }
      }
    });
  }

  barrier.WaitAllReady();
  barrier.Release();

  for (auto& th : workers) {
    th.join();
  }

  if (async_log) {
    async_log->Stop();
  } else {
    sync_sink->Close();
  }

  const FileCheck check = InspectLogFile(path);
  std::remove(path.c_str());

  std::vector<uint64_t> all_ns;
  all_ns.reserve(total_count);
  for (const auto& v : per_thread_ns) {
    all_ns.insert(all_ns.end(), v.begin(), v.end());
  }
  std::sort(all_ns.begin(), all_ns.end());

  const uint64_t sum_ns = std::accumulate(all_ns.begin(), all_ns.end(), uint64_t{0});

  LatencyResult res;
  res.avg_us = (static_cast<double>(sum_ns) / static_cast<double>(all_ns.size())) / 1000.0;
  res.p50_us = PercentileUs(all_ns, 0.50);
  res.p99_us = PercentileUs(all_ns, 0.99);
  res.p999_us = PercentileUs(all_ns, 0.999);
  res.max_us = static_cast<double>(all_ns.back()) / 1000.0;
  if (workload == Workload::kBackend) {
    res.intact = (check.lines == total_count && check.bytes == total_count * kFixedLineLen);
  } else {
    res.intact = (check.lines == total_count);
  }
  return res;
}

void PrintSectionHeader(Workload w, int threads) {
  std::printf("\n=== [%s] threads=%d ===\n", WorkloadName(w), threads);
  std::printf("%-14s | %12s | %8s | %9s | %9s | %8s | %8s | %8s | %9s | %s\n", "mode", "msg/s",
              "MB/s", "front(ms)", "total(ms)", "avg(us)", "p50(us)", "p99(us)", "max(us)",
              "check");
  std::printf(
      "---------------+--------------+----------+-----------+-----------+----------+----------+----"
      "------+-----------+-------\n");
}

void RunAndPrintRow(Workload w, Mode m, int threads, const std::string& dir) {
  const ThroughputResult tp = RunThroughput(w, m, threads, kThroughputCount, dir);
  const LatencyResult lat = RunLatency(w, m, threads, kLatencyCount, dir);
  const bool ok = tp.intact && lat.intact;

  std::printf("%-14s | %12.0f | %8.1f | %9.1f | %9.1f | %8.3f | %8.3f | %8.3f | %9.1f | %s\n",
              ModeName(m), tp.throughput, tp.mb_per_sec, tp.front_sec * 1e3, tp.total_sec * 1e3,
              lat.avg_us, lat.p50_us, lat.p99_us, lat.max_us, ok ? "OK" : "FAIL");
}

bool ParseWorkload(const char* s, Workload* out) {
  if (std::strcmp(s, "backend") == 0) {
    *out = Workload::kBackend;
    return true;
  }
  if (std::strcmp(s, "logger") == 0) {
    *out = Workload::kLogger;
    return true;
  }
  return false;
}

bool ParseMode(const char* s, Mode* out) {
  if (std::strcmp(s, "sync-buffered") == 0) {
    *out = Mode::kSyncBuffered;
    return true;
  }
  if (std::strcmp(s, "sync-flush") == 0) {
    *out = Mode::kSyncFlush;
    return true;
  }
  if (std::strcmp(s, "async") == 0) {
    *out = Mode::kAsync;
    return true;
  }
  return false;
}

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s [--dir <dir>]\n"
               "  %s --single <backend|logger> <sync-buffered|sync-flush|async> <threads> <count> "
               "[--dir <dir>]\n",
               prog, prog);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string dir = ".";
  bool single = false;
  Workload single_workload = Workload::kBackend;
  Mode single_mode = Mode::kAsync;
  int single_threads = 1;
  std::size_t single_count = kThroughputCount;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
      dir = argv[++i];
    } else if (std::strcmp(argv[i], "--single") == 0 && i + 4 < argc) {
      single = true;
      if (!ParseWorkload(argv[++i], &single_workload) || !ParseMode(argv[++i], &single_mode)) {
        PrintUsage(argv[0]);
        return 1;
      }
      single_threads = std::atoi(argv[++i]);
      single_count = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
      if (single_threads <= 0 || single_count == 0) {
        PrintUsage(argv[0]);
        return 1;
      }
    } else {
      PrintUsage(argv[0]);
      return 1;
    }
  }

  // 压测期间保持默认 kInfo，确保 LOG_INFO 正常执行
  netlib::SetMinLogLevel(netlib::LogLevel::kInfo);

  if (single) {
    const ThroughputResult tp =
        RunThroughput(single_workload, single_mode, single_threads, single_count, dir);
    std::printf(
        "single workload=%s mode=%s threads=%d count=%zu dir=%s\n"
        "  front=%.2f ms, total=%.2f ms, throughput=%.0f msg/s (%.1f MB/s), avg=%.3f us\n"
        "  file_bytes=%zu, file_lines=%zu, check=%s\n",
        WorkloadName(single_workload), ModeName(single_mode), single_threads, single_count,
        dir.c_str(), tp.front_sec * 1e3, tp.total_sec * 1e3, tp.throughput, tp.mb_per_sec,
        tp.avg_us, tp.file_bytes, tp.file_lines, tp.intact ? "OK" : "FAIL");
    return tp.intact ? 0 : 1;
  }

  std::printf("netlib log benchmark (dir=%s, throughput_n=%zu, latency_n=%zu)\n", dir.c_str(),
              kThroughputCount, kLatencyCount);
  std::printf("Note: msg/s, MB/s, front(ms) reflect business-thread submission time;\n");
  std::printf(
      "      total(ms) includes final flush/Stop(); avg/p50/p99/max are per-call latency.\n");

  // 简短预热，减少首次分配与文件系统冷启动抖动
  RunThroughput(Workload::kBackend, Mode::kAsync, 1, 20000, dir);

  const Workload workloads[] = {Workload::kBackend, Workload::kLogger};
  const int thread_counts[] = {1, 4};
  const Mode modes[] = {Mode::kSyncBuffered, Mode::kSyncFlush, Mode::kAsync};

  for (Workload w : workloads) {
    for (int threads : thread_counts) {
      PrintSectionHeader(w, threads);
      for (Mode m : modes) {
        RunAndPrintRow(w, m, threads, dir);
      }
    }
  }

  return 0;
}
