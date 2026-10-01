// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Error reporting across the backend interface.
//
// The core is built with -fno-exceptions and must not use llvm::Expected, so
// backends report failure through these two types: a Status is "ok" or an
// error message; a StatusOr<T> is a T or an error message.

#pragma once

#include <cassert>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace paykan {

class Status {
public:
  /// Success.
  Status() = default;

  static Status ok() { return Status(); }
  static Status error(std::string message) {
    Status s;
    s.Message = std::move(message);
    s.Failed = true;
    return s;
  }

  bool isOk() const { return !Failed; }
  explicit operator bool() const { return isOk(); }

  /// The error message; empty on success.
  const std::string &message() const { return Message; }

private:
  std::string Message;
  bool Failed = false;
};

template <typename T> class StatusOr {
public:
  /// A value.
  StatusOr(T value)
      : Value(std::move(value)) {} // NOLINT(google-explicit-constructor)
  /// An error.  @p status must not be ok.
  StatusOr(Status status) // NOLINT(google-explicit-constructor)
      : Err(std::move(status)) {
    assert(!Err.isOk() && "StatusOr built from an ok Status without a value");
  }

  bool isOk() const { return Value.has_value(); }
  explicit operator bool() const { return isOk(); }

  /// The value; only when isOk().  Asking for the value of an error is a
  /// bug, so it ends the program rather than returning something.
  T &value() {
    if (!Value.has_value())
      std::abort();
    return *Value;
  }
  const T &value() const {
    if (!Value.has_value())
      std::abort();
    return *Value;
  }
  T &operator*() { return value(); }
  const T &operator*() const { return value(); }

  /// The error; ok when there is a value.
  const Status &status() const { return Err; }

private:
  std::optional<T> Value;
  Status Err;
};

} // namespace paykan
