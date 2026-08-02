// Copyright (c) 2025  SRI International
// This file was released under the Apache 2.0 license (same license as
// Remill).  All rights reserved.
//
// Minimal header-only replacement for Google glog.  Provides just enough
// macros for the codebase to compile without depending on the glog library.

#ifndef REMILL_BC_LOGGING_H_
#define REMILL_BC_LOGGING_H_

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

// ---- Stream wrapper for LOG(FATAL) ----
class FatalStream {
 public:
  FatalStream(const char *file, int line, const char *function)
      : file_(file), line_(line), function_(function) {}
  ~FatalStream() noexcept(false) {
    std::cerr << "[FATAL] " << file_ << ":" << line_ << " (" << function_
              << ") " << stream_.str() << std::endl;
    std::abort();
  }
  template <typename T>
  FatalStream &operator<<(const T &value) {
    stream_ << value;
    return *this;
  }

 private:
  const char *file_;
  int line_;
  const char *function_;
  std::ostringstream stream_;
};

// ---- Stream wrapper for LOG(ERROR/WARNING/INFO) ----
class LogStream {
 public:
  LogStream() = default;
  explicit LogStream(bool /* suppress */) : suppress_(true) {}
  template <typename T>
  LogStream &operator<<(const T &value) {
    if (!suppress_) {
      stream_ << value;
    }
    return *this;
  }
  ~LogStream() {
    if (!suppress_) {
      std::cerr << stream_.str();
    }
  }

 private:
  std::ostringstream stream_;
  bool suppress_ = false;
};

// ---- Conditional log stream for LOG_IF ----
class ConditionalLogStream {
 public:
  ConditionalLogStream(const char *file, int line, const char *function,
                       bool fatal, bool condition)
      : file_(file), line_(line), function_(function), fatal_(fatal),
        condition_(condition) {}

  template <typename T>
  ConditionalLogStream &operator<<(const T &value) {
    if (condition_) {
      stream_ << value;
    }
    return *this;
  }

  ~ConditionalLogStream() noexcept(false) {
    if (!condition_) return;
    if (fatal_) {
      std::cerr << "[FATAL] " << file_ << ":" << line_ << " (" << function_
                << ") " << stream_.str() << std::endl;
      std::abort();
    } else {
      std::cerr << stream_.str();
    }
  }

 private:
  const char *file_;
  int line_;
  const char *function_;
  bool fatal_;
  bool condition_;
  std::ostringstream stream_;
};

// ---- CHECK stream wrapper (for CHECK(cond) << msg) ----
class CheckStream {
 public:
  CheckStream(bool condition, const char *file, int line, const char *function,
              const char *condition_str)
      : condition_(condition),
        file_(file),
        line_(line),
        function_(function),
        condition_str_(condition_str) {}

  template <typename T>
  CheckStream &operator<<(const T &value) {
    if (!condition_) {
      stream_ << value;
    }
    return *this;
  }

  ~CheckStream() noexcept(false) {
    if (!condition_) {
      std::cerr << "[CHECK FAILED] " << file_ << ":" << line_ << " ("
                << function_ << ") "
                << "Condition `" << condition_str_ << "` was false.\n"
                << stream_.str();
      std::abort();
    }
  }

 private:
  bool condition_;
  const char *file_;
  int line_;
  const char *function_;
  const char *condition_str_;
  std::ostringstream stream_;
};

// ---- CHECK binary op helpers (CHECK_EQ, CHECK_NE, etc.) ----
template <typename A, typename B>
class CheckBinaryOp {
 public:
  CheckBinaryOp(bool condition, const char *file, int line, const char *function,
                const char *condition_str, A a, B b)
      : condition_(condition),
        file_(file),
        line_(line),
        function_(function),
        condition_str_(condition_str),
        a_(std::move(a)),
        b_(std::move(b)) {}

  template <typename T>
  CheckBinaryOp &operator<<(const T &value) {
    if (!condition_) {
      stream_ << value;
    }
    return *this;
  }

  ~CheckBinaryOp() noexcept(false) {
    if (!condition_) {
      std::ostringstream detail;
      detail << "Values: " << a_ << " vs " << b_;
      std::cerr << "[CHECK FAILED] " << file_ << ":" << line_ << " ("
                << function_ << ") "
                << "Condition `" << condition_str_ << "` was false.\n"
                << detail.str() << stream_.str();
      std::abort();
    }
  }

 private:
  bool condition_;
  const char *file_;
  int line_;
  const char *function_;
  const char *condition_str_;
  A a_;
  B b_;
  std::ostringstream stream_;
};

// ---- CHECK_NOTNULL helper ----
template <typename T>
class CheckNotNull {
 public:
  CheckNotNull(T ptr, const char *file, int line, const char *function,
               const char *ptr_name)
      : ptr_(ptr),
        file_(file),
        line_(line),
        function_(function),
        ptr_name_(ptr_name) {}

  template <typename T2>
  CheckNotNull &operator<<(const T2 &value) {
    if (!ptr_) {
      stream_ << value;
    }
    return *this;
  }

  ~CheckNotNull() noexcept(false) {
    if (!ptr_) {
      std::cerr << "[CHECK FAILED] " << file_ << ":" << line_ << " ("
                << function_ << ") "
                << "Condition `" << ptr_name_ << "` was null.\n"
                << stream_.str();
      std::abort();
    }
  }

  T Get() const { return ptr_; }

 private:
  T ptr_;
  const char *file_;
  int line_;
  const char *function_;
  const char *ptr_name_;
  std::ostringstream stream_;
};

// ---- Macros ----

#define LOG(level) \
  _REMILL_LOG_##level##_STREAM(__FILE__, __LINE__, __func__)

#define _REMILL_LOG_FATAL_STREAM(file, line, func) \
  FatalStream(file, line, func)
#define _REMILL_LOG_ERROR_STREAM(file, line, func) \
  LogStream()
#define _REMILL_LOG_WARNING_STREAM(file, line, func) \
  LogStream()
#define _REMILL_LOG_INFO_STREAM(file, line, func) \
  LogStream()

#define LOG_IF(severity, condition) \
  (ConditionalLogStream( \
      __FILE__, __LINE__, __func__, \
      (std::string(#severity) == "FATAL"), !(condition)))

#define DLOG(level) LogStream(true)
#define DCHECK_EQ(a, b) static_cast<void>(0)

// CHECK(cond) << msg
#define CHECK(cond) \
  (CheckStream(static_cast<bool>(cond), __FILE__, __LINE__, __func__, #cond))

// CHECK_EQ, CHECK_NE, CHECK_LT, CHECK_LE, CHECK_GT, CHECK_GE
#define CHECK_EQ(a, b) \
  (CheckBinaryOp((a) == (b), __FILE__, __LINE__, __func__, #a " == " #b, (a), (b)))
#define CHECK_NE(a, b) \
  (CheckBinaryOp((a) != (b), __FILE__, __LINE__, __func__, #a " != " #b, (a), (b)))
#define CHECK_LT(a, b) \
  (CheckBinaryOp((a) < (b), __FILE__, __LINE__, __func__, #a " < " #b, (a), (b)))
#define CHECK_LE(a, b) \
  (CheckBinaryOp((a) <= (b), __FILE__, __LINE__, __func__, #a " <= " #b, (a), (b)))
#define CHECK_GT(a, b) \
  (CheckBinaryOp((a) > (b), __FILE__, __LINE__, __func__, #a " > " #b, (a), (b)))
#define CHECK_GE(a, b) \
  (CheckBinaryOp((a) >= (b), __FILE__, __LINE__, __func__, #a " >= " #b, (a), (b)))

#define CHECK_NOTNULL(x) \
  (CheckNotNull((x), __FILE__, __LINE__, __func__, #x))

// google::InitGoogleLogging — no-op.
namespace google {
inline void InitGoogleLogging(const char *) {}
inline void ShutdownGoogleLogging() {}
inline void SetVersionString(const std::string &) {}
}  // namespace google

#endif  // REMILL_BC_LOGGING_H_
