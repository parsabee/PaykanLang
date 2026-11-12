// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the runtime's file: the functions behind ::io's File.

#include <cerrno>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unistd.h>

extern "C" {
#include "Runtime.h"
}

// -- Helpers

// Create a temporary file, write content into it, rewind, and return the path.
// The caller is responsible for removing the file with std::remove().
static std::string makeTempFile(const char *content) {
  char path[] = "/tmp/paykan_file_test_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return "";
  size_t len = content ? strlen(content) : 0;
  bool ok = true;
  for (size_t off = 0; off < len;) {
    ssize_t n = write(fd, content + off, len - off);
    if (n <= 0) {
      ok = false;
      break;
    }
    off += static_cast<size_t>(n);
  }
  close(fd);
  if (!ok) {
    std::remove(path);
    return "";
  }
  return std::string(path);
}

/// The runtime's file that the handle @p file names: its address, as the
/// `int` an io::File holds.
static PaykanFile *fileOf(int64_t file) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<PaykanFile *>(file);
}

static PaykanObject *strObj(const char *s) {
  return (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s));
}

// -- Helpers on ::io's natives: a file is the `int` an io::File holds.

/// Everything in the file at @p path.
static std::string fileText(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "cannot reopen " << path;
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// The file at @p path opened with @p mode through ::io's native.
static int64_t ioOpen(const std::string &path, const char *mode) {
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj(mode);
  int64_t err = 0;
  int64_t f = Paykan_io_open(pathStr, modeStr, &err);
  Paykan_dispose(pathStr);
  Paykan_dispose(modeStr);
  EXPECT_NE(f, 0) << path << " (" << mode << "): errno " << err;
  return f;
}

/// ::io's write of @p s: its errno, 0 when it wrote.
static int64_t ioWrite(int64_t f, const char *s) {
  PaykanObject *str = strObj(s);
  int64_t err = Paykan_io_write(f, str);
  Paykan_dispose(str);
  return err;
}

/// What a read through ::io gave, @p r with @p err: its text, "EOF" at the
/// end of the file, or the errno of its failure.
static std::string ioText(PaykanShared *r, int64_t err) {
  if (!r)
    return err ? "errno " + std::to_string(err) : "EOF";
  auto *s = (PaykanString *)PaykanShared_get(r);
  std::string text(s->data, (size_t)s->len);
  Paykan_release(r);
  return text;
}

static std::string ioReadln(int64_t f) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_readln(f, &err);
  return ioText(r, err);
}

static std::string ioReadbytes(int64_t f, int64_t n) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_readbytes(f, n, &err);
  return ioText(r, err);
}

static std::string ioRead(int64_t f) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_read(f, &err);
  return ioText(r, err);
}

// -- Paykan_io_open

TEST(FileOpen, SuccessReturnsAFileOverAnOpenStream) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_NE(fileOf(f)->handle, nullptr);
  EXPECT_EQ(fileOf(f)->lastOp, PAYKAN_FILE_OP_NONE);
  Paykan_io_close(f);
  std::remove(path.c_str());
}

TEST(FileOpen, FailureReturnsNoFileAndErrno) {
  PaykanObject *pathStr = strObj("/no/such/path/that/exists");
  PaykanObject *modeStr = strObj("r");

  // open failure: no file, with C's errno for the standard library's Error.
  int64_t err = 0;
  EXPECT_EQ(Paykan_io_open(pathStr, modeStr, &err), 0);
  EXPECT_EQ(err, ENOENT);
  EXPECT_EQ(Paykan_error_code(err), PAYKAN_ERROR_NOT_FOUND);

  Paykan_dispose(pathStr);
  Paykan_dispose(modeStr);
}

// fopen reads a path and a mode up to their first NUL byte, so a Str holding
// one would open another file, or in another mode: it is not opened, EINVAL
// (InvalidArgument), and nothing is made under the part before the NUL.
TEST(FileOpen, ANulByteInThePathOrTheModeIsInvalid) {
  auto dir = std::filesystem::temp_directory_path() /
             ("paykan_file_nul_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  const std::string prefix = (dir / "nul").string();
  const std::string path = prefix + std::string("\0trunc.txt", 10);
  PaykanObject *pathStr =
      (PaykanObject *)PaykanString_new(path.data(), (int64_t)path.size());
  PaykanObject *write = strObj("w");
  int64_t err = 0;
  EXPECT_EQ(Paykan_io_open(pathStr, write, &err), 0);
  EXPECT_EQ(err, EINVAL);
  EXPECT_EQ(Paykan_error_code(err), PAYKAN_ERROR_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(prefix));

  const std::string file = (dir / "three.txt").string();
  PaykanObject *fileStr = strObj(file.c_str());
  PaykanObject *mode = (PaykanObject *)PaykanString_new("w\0garbage", 9);
  err = 0;
  EXPECT_EQ(Paykan_io_open(fileStr, mode, &err), 0);
  EXPECT_EQ(err, EINVAL);
  EXPECT_FALSE(std::filesystem::exists(file));

  Paykan_dispose(pathStr);
  Paykan_dispose(write);
  Paykan_dispose(fileStr);
  Paykan_dispose(mode);
  std::filesystem::remove_all(dir);
}

// -- Paykan_io_write

TEST(FileWrite, WritesContentToFile) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "w");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, "hello paykan\n"), 0);
  Paykan_io_close(f); // writes out what C buffered
  EXPECT_EQ(fileText(path), "hello paykan\n");
  std::remove(path.c_str());
}

TEST(FileWrite, EmptyStringIsHarmless) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "w");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, ""), 0);
  EXPECT_EQ(fileOf(f)->lastOp, PAYKAN_FILE_OP_NONE);
  Paykan_io_close(f);
  EXPECT_EQ(fileText(path), "");
  std::remove(path.c_str());
}

// A write on a stream opened for reading fails with C's errno, and the
// stream reads on after it.
TEST(FileWrite, OnAFileOpenedForReadingFails) {
  std::string path = makeTempFile("abc\n");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_NE(ioWrite(f, "x"), 0);
  EXPECT_EQ(ioReadln(f), "abc\n");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// -- Paykan_io_readln

TEST(FileReadln, ReadsEachLineThenTheEnd) {
  std::string path = makeTempFile("line1\nline2\nline3\n");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioReadln(f), "line1\n");
  EXPECT_EQ(ioReadln(f), "line2\n");
  EXPECT_EQ(ioReadln(f), "line3\n");
  EXPECT_EQ(ioReadln(f), "EOF");
  EXPECT_EQ(ioReadln(f), "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

TEST(FileReadln, LineWithoutTrailingNewline) {
  std::string path = makeTempFile("no newline");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioReadln(f), "no newline");
  EXPECT_EQ(ioReadln(f), "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

TEST(FileReadln, LongLineTriggersBufferGrowth) {
  // Longer than the first 128-byte buffer, several doublings over.
  std::string line(1000, 'x');
  std::string path = makeTempFile((line + "\nnext\n").c_str());
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioReadln(f), line + "\n");
  EXPECT_EQ(ioReadln(f), "next\n");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// -- write + readln round-trip

TEST(FileRoundTrip, WriteAndReadBack) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  int64_t w = ioOpen(path, "w");
  ASSERT_NE(w, 0);
  EXPECT_EQ(ioWrite(w, "first line\n"), 0);
  EXPECT_EQ(ioWrite(w, "second line\n"), 0);
  Paykan_io_close(w);

  int64_t r = ioOpen(path, "r");
  ASSERT_NE(r, 0);
  EXPECT_EQ(ioReadln(r), "first line\n");
  EXPECT_EQ(ioReadln(r), "second line\n");
  EXPECT_EQ(ioReadln(r), "EOF");
  Paykan_io_close(r);
  std::remove(path.c_str());
}

// -- Paykan_io_readbytes

TEST(FileReadbytes, ReadsUpToNThenLessAtTheEnd) {
  std::string path = makeTempFile("hello world");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioReadbytes(f, 5), "hello");
  EXPECT_EQ(ioReadbytes(f, 100), " world");
  EXPECT_EQ(ioReadbytes(f, 5), "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// A count that is not positive reads nothing and is EINVAL
// (InvalidArgument); the stream is where it was.
TEST(FileReadbytes, ACountThatIsNotPositiveIsInvalid) {
  std::string path = makeTempFile("abc");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioReadbytes(f, 0), "errno " + std::to_string(EINVAL));
  EXPECT_EQ(ioReadbytes(f, -3), "errno " + std::to_string(EINVAL));
  EXPECT_EQ(ioReadbytes(f, 3), "abc");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// -- Paykan_io_read

TEST(FileRead, ReadsEntireFile) {
  std::string text;
  for (int i = 0; text.size() < 10000; ++i) // past the first 4096-byte buffer
    text += std::to_string(i) + "\n";
  std::string path = makeTempFile(text.c_str());
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_TRUE(ioRead(f) == text);
  EXPECT_EQ(ioRead(f), "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

TEST(FileRead, AnEmptyFileIsAtItsEnd) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioRead(f), "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// -- Update modes ("r+", "w+", "a+"): reads and writes on one stream

// C lets a read follow a write on an update stream only after an fflush, and
// a write follow a read only after a file positioning call (C11 7.21.5.3);
// without them the C library dropped the first write.  In "w+": the read
// after the write finds the end of the file, and the second write follows
// the first.
TEST(FileUpdate, WPlusKeepsAWriteThatAReadFollows) {
  std::string path = makeTempFile("old\n");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "w+");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, "hello\n"), 0);
  EXPECT_EQ(ioRead(f), "EOF");
  EXPECT_EQ(ioWrite(f, "world\n"), 0);
  Paykan_io_close(f);
  EXPECT_EQ(fileText(path), "hello\nworld\n");
  std::remove(path.c_str());
}

// In "r+", from the start of the file: each read starts where the write
// before it ended, and each write where the read before it stopped (or at
// the end, after a read that reached it), through every read.
TEST(FileUpdate, RPlusReadsAndWritesWhereTheOtherStopped) {
  std::string path = makeTempFile("abcdef\nghi\n");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r+");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, "XY"), 0);
  EXPECT_EQ(ioReadbytes(f, 3), "cde");
  EXPECT_EQ(ioWrite(f, "Z"), 0);
  EXPECT_EQ(ioReadln(f), "\n");
  EXPECT_EQ(ioWrite(f, "W"), 0);
  EXPECT_EQ(ioRead(f), "hi\n");
  EXPECT_EQ(ioWrite(f, "!"), 0);
  EXPECT_EQ(ioReadln(f), "EOF");
  Paykan_io_close(f);
  EXPECT_EQ(fileText(path), "XYcdeZ\nWhi\n!");
  std::remove(path.c_str());
}

// In "a+" every write goes to the end of the file, so the read after one
// finds the end (where C leaves the first read before any write differs:
// the start on Linux, the end on macOS, so the test does not read first).
TEST(FileUpdate, APlusKeepsAWriteThatAReadFollows) {
  std::string path = makeTempFile("abcdef\n");
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "a+");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, "hello\n"), 0);
  EXPECT_EQ(ioRead(f), "EOF");
  EXPECT_EQ(ioWrite(f, "world\n"), 0);
  EXPECT_EQ(ioReadbytes(f, 4), "EOF");
  Paykan_io_close(f);
  EXPECT_EQ(fileText(path), "abcdef\nhello\nworld\n");
  std::remove(path.c_str());
}

// What a write left in the C library's buffer is written out before a read:
// when that fails (a full device), the read fails with its errno instead of
// reading past the lost data.  /dev/full takes no byte, and reads as zeros;
// the read is longer than the C library's buffer, which glibc reads into
// directly, dropping the buffered write unless it was written out first.
TEST(FileUpdate, AWriteThatCannotBeWrittenOutFailsTheReadAfterIt) {
  if (access("/dev/full", R_OK | W_OK) != 0)
    GTEST_SKIP() << "no /dev/full";
  int64_t f = ioOpen("/dev/full", "r+");
  ASSERT_NE(f, 0);
  EXPECT_EQ(ioWrite(f, "hello"), 0); // buffered
  // (Not EXPECT_EQ, which would print the 64 KiB of zeros it read before.)
  std::string got = ioReadbytes(f, 65536);
  EXPECT_TRUE(got == "errno " + std::to_string(ENOSPC))
      << got.size() << " bytes: " << got.substr(0, 16);
  EXPECT_EQ(Paykan_error_code(ENOSPC), PAYKAN_ERROR_NO_SPACE);
  Paykan_io_close(f);
}

// -- readbytes(n): n is a maximum

namespace {

// RAII: the tracking allocator with zeroed counters; no live blocks at the
// end.
struct TrackingHeap {
  TrackingHeap() {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~TrackingHeap() {
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_heap_set_tracking(0);
  }
};

} // namespace

// readbytes(n)'s buffer starts at 64 KiB at most and grows as reads fill
// it, so readbytes(2^40) of a 5-byte file allocates under 1 MB.  It
// allocated the 2^40 bytes up front (SW-27): out of memory (ENOMEM) where
// the system refuses an allocation that large (Linux), and a 1 TB peak on
// the tracking heap where it does not (macOS).
TEST(FileReadbytes, AMaximumPastTheEndAllocatesWhatIsThere) {
  std::string path = makeTempFile("hello");
  ASSERT_FALSE(path.empty());
  {
    TrackingHeap heap;
    int64_t f = ioOpen(path, "r");
    ASSERT_NE(f, 0);
    EXPECT_EQ(ioReadbytes(f, int64_t{1} << 40), "hello");
    EXPECT_EQ(ioReadbytes(f, int64_t{1} << 40), "EOF");
    Paykan_io_close(f);
    EXPECT_LT(Paykan_heap_peak_bytes(), 1 << 20);
  }
  std::remove(path.c_str());
}

// A read past the first 64 KiB doubles the buffer until it holds n bytes or
// the file ends, also where the end is a doubling's boundary.
TEST(FileReadbytes, ALongReadGrowsItsBufferUpToN) {
  std::string text;
  for (int i = 0; text.size() < 200000; ++i)
    text += std::to_string(i) + ",";
  text.resize(200000);
  std::string path = makeTempFile(text.c_str());
  ASSERT_FALSE(path.empty());
  int64_t f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  // (Not EXPECT_EQ, which would print the bytes.)
  auto expectRead = [&f](int64_t n, const std::string &want) {
    std::string got = ioReadbytes(f, n);
    EXPECT_TRUE(got == want) << "readbytes(" << n << "): " << got.size()
                             << " bytes, not " << want.size();
  };
  expectRead(65536, text.substr(0, 65536));
  expectRead(65537, text.substr(65536, 65537));
  expectRead(int64_t{1} << 40, text.substr(131073));
  expectRead(1, "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());

  path = makeTempFile(text.substr(0, 131072).c_str());
  ASSERT_FALSE(path.empty());
  f = ioOpen(path, "r");
  ASSERT_NE(f, 0);
  expectRead(int64_t{1} << 40, text.substr(0, 131072));
  expectRead(1, "EOF");
  Paykan_io_close(f);
  std::remove(path.c_str());
}

// A read whose text does not fit in memory fails with ENOMEM (::io's
// OutOfMemory), frees what it allocated and panics nowhere: readbytes, read
// and readln each, out of memory partway through a line of 300,000 bytes
// with the tracking heap held to 200 KiB.  What it had read is gone, as
// docs/stdlib/io.md says: each fails growing its buffer from 128 KiB, and
// the next read starts after those 131,072 bytes.
TEST(FileRead, OutOfMemoryPartwayLosesWhatWasRead) {
  std::string text;
  for (int i = 0; text.size() < 300000; ++i)
    text += std::to_string(i) + ",";
  text.resize(300000);
  std::string path = makeTempFile(text.c_str());
  ASSERT_FALSE(path.empty());
  const std::pair<const char *, std::function<std::string(int64_t)>> reads[] = {
      {"readbytes", [](int64_t f) { return ioReadbytes(f, int64_t{1} << 20); }},
      {"read", ioRead},
      {"readln", ioReadln},
  };
  for (const auto &[name, read] : reads) {
    TrackingHeap heap;
    int64_t f = ioOpen(path, "r");
    ASSERT_NE(f, 0) << name;
    Paykan_heap_set_limit(int64_t{200} * 1024);
    EXPECT_EQ(read(f), "errno " + std::to_string(ENOMEM)) << name;
    Paykan_heap_set_limit(0);
    EXPECT_EQ(ioReadbytes(f, 8), text.substr(131072, 8)) << name;
    Paykan_io_close(f);
  }
  EXPECT_EQ(Paykan_error_code(ENOMEM), PAYKAN_ERROR_OUT_OF_MEMORY);
  std::remove(path.c_str());
}

// -- Stdin and closing

// The process's stdin is one file, whatever File names it, and closing it
// leaves C's stdin open: an io::File made by `io::stdin()` closes nothing.
TEST(FileStdin, IsOneFileThatCloseLeavesOpen) {
  const int64_t in = Paykan_io_stdin();
  ASSERT_NE(in, 0);
  EXPECT_EQ(Paykan_io_stdin(), in);
  EXPECT_EQ(fileOf(in)->handle, stdin);
  Paykan_io_close(in);
  Paykan_io_close(in);
  EXPECT_EQ(fileOf(in)->handle, stdin);
  EXPECT_EQ(ferror(stdin), 0);
  EXPECT_NE(fileno(stdin), -1);
}

// A file is one block, which closing it frees: nothing counts it, and
// closing no file (the zero of storage that holds none) does nothing.
TEST(FileClose, FreesTheFileAndWritesOutWhatWasBuffered) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  {
    TrackingHeap heap;
    int64_t f = ioOpen(path, "w");
    ASSERT_NE(f, 0);
    EXPECT_EQ(Paykan_heap_live_blocks(), 1);
    EXPECT_EQ(ioWrite(f, "kept"), 0);
    Paykan_io_close(f);
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_io_close(0);
  }
  EXPECT_EQ(fileText(path), "kept");
  std::remove(path.c_str());
}
