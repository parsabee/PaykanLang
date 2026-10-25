// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PaykanFile runtime functions.

#include <cerrno>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"

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

static PaykanObject *strObj(const char *s) {
  return (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s));
}

// -- PaykanFile_new

TEST(FileNew, AllocatesWithNullHandle) {
  PaykanFile *f = PaykanFile_new();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->handle, nullptr);
  Paykan_dispose((PaykanObject *)f);
}

TEST(FileNew, VtableIsFileVtable) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ((void *)f->vtable, (void *)&PaykanFile_vtable);
  Paykan_dispose((PaykanObject *)f);
}

// -- PaykanFile_destroy

TEST(FileDestroy, ClosesOpenHandle) {
  // Open a real file, then destroy — fclose should be called without crashing.
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  PaykanFile *f = PaykanFile_new();
  f->handle = fopen(path.c_str(), "r");
  ASSERT_NE(f->handle, nullptr);
  Paykan_dispose((PaykanObject *)f); // must not crash
  std::remove(path.c_str());
}

TEST(FileDestroy, NullHandleIsHarmless) {
  PaykanFile *f = PaykanFile_new();
  f->handle = nullptr;
  Paykan_dispose((PaykanObject *)f); // must not crash
}

// -- PaykanFile_toString

TEST(FileToString, ContainsFilePrefix) {
  PaykanFile *f = PaykanFile_new();
  PaykanShared *shared = PaykanFile_toString((PaykanObject *)f);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  EXPECT_NE(std::string(s->data).find("File@"), std::string::npos);
  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)f);
}

// -- PaykanFile_equals

TEST(FileEquals, SameObjectIsEqual) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ(
      paykanTestEquals(PaykanFile_equals, (PaykanObject *)f, (PaykanObject *)f),
      1);
  Paykan_dispose((PaykanObject *)f);
}

TEST(FileEquals, DifferentObjectsAreNotEqual) {
  PaykanFile *a = PaykanFile_new();
  PaykanFile *b = PaykanFile_new();
  EXPECT_EQ(
      paykanTestEquals(PaykanFile_equals, (PaykanObject *)a, (PaykanObject *)b),
      0);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

// -- PaykanFile_open

/// Where a failing PaykanFile_open puts errno (the tests that open expect
/// success).
static int64_t openErr;

TEST(FileOpen, SuccessReturnsFile) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");

  PaykanShared *shared = PaykanFile_open(pathStr, modeStr, &openErr);
  ASSERT_NE(shared, nullptr);
  PaykanObject *obj = PaykanShared_get(shared);
  // Should NOT be None — it must be a PaykanFile with a valid handle.
  EXPECT_NE(obj, &PaykanObject_None);
  EXPECT_NE(((PaykanFile *)obj)->handle, nullptr);

  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileOpen, FailureReturnsNullAndErrno) {
  PaykanObject *pathStr = strObj("/no/such/path/that/exists");
  PaykanObject *modeStr = strObj("r");

  // open failure: NULL, with C's errno for the standard library's Error.
  int64_t err = 0;
  EXPECT_EQ(PaykanFile_open(pathStr, modeStr, &err), nullptr);
  EXPECT_EQ(err, ENOENT);
  EXPECT_EQ(Paykan_error_code(err), PAYKAN_ERROR_NOT_FOUND);

  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
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
  EXPECT_EQ(PaykanFile_open(pathStr, write, &err), nullptr);
  EXPECT_EQ(err, EINVAL);
  EXPECT_EQ(Paykan_error_code(err), PAYKAN_ERROR_INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(prefix));

  const std::string file = (dir / "three.txt").string();
  PaykanObject *fileStr = strObj(file.c_str());
  PaykanObject *mode = (PaykanObject *)PaykanString_new("w\0garbage", 9);
  err = 0;
  EXPECT_EQ(PaykanFile_open(fileStr, mode, &err), nullptr);
  EXPECT_EQ(err, EINVAL);
  EXPECT_FALSE(std::filesystem::exists(file));

  // Through ::io's native too.
  err = 0;
  EXPECT_EQ(Paykan_io_open(pathStr, write, &err), nullptr);
  EXPECT_EQ(err, EINVAL);

  Paykan_dispose(pathStr);
  Paykan_dispose(write);
  Paykan_dispose(fileStr);
  Paykan_dispose(mode);
  std::filesystem::remove_all(dir);
}

TEST(FileOpen, OpenForWrite) {
  char path[] = "/tmp/paykan_file_write_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *shared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *obj = PaykanShared_get(shared);
  EXPECT_NE(obj, &PaykanObject_None);

  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path);
}

// -- PaykanFile_write

TEST(FileWrite, WritesContentToFile) {
  char path[] = "/tmp/paykan_file_write2_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  // Open for writing.
  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanObject *content = strObj("hello paykan\n");
  PaykanFile_write(fObj, content);
  Paykan_dispose((PaykanObject *)content);

  Paykan_release(fShared); // closes handle via destroy
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);

  // Read back and verify (the stream closed on every path).
  std::string text;
  if (FILE *fp = fopen(path, "r")) {
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    text.assign(buf, n);
  } else {
    ADD_FAILURE() << "cannot reopen " << path;
  }
  EXPECT_EQ(text, "hello paykan\n");
  std::remove(path);
}

TEST(FileWrite, EmptyStringIsHarmless) {
  char path[] = "/tmp/paykan_file_empty_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanObject *empty = strObj("");
  PaykanFile_write(fObj, empty); // must not crash
  Paykan_dispose((PaykanObject *)empty);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path);
}

// -- PaykanFile_readln

TEST(FileReadln, ReadsSingleLine) {
  std::string path = makeTempFile("hello\nworld\n");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *lineShared = PaykanFile_readln(fObj);
  PaykanString *line = (PaykanString *)PaykanShared_get(lineShared);
  EXPECT_EQ(std::string(line->data, (size_t)line->len), "hello\n");
  Paykan_release(lineShared);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, ReadsMultipleLines) {
  std::string path = makeTempFile("line1\nline2\nline3\n");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  const char *expected[] = {"line1\n", "line2\n", "line3\n"};
  for (const char *exp : expected) {
    PaykanShared *ls = PaykanFile_readln(fObj);
    PaykanString *l = (PaykanString *)PaykanShared_get(ls);
    EXPECT_EQ(std::string(l->data, (size_t)l->len), exp);
    Paykan_release(ls);
  }

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, ReturnsNoneAtEOF) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  // EOF with no bytes read signals end-of-file by returning None.
  PaykanShared *ls = PaykanFile_readln(fObj);
  EXPECT_EQ(PaykanShared_get(ls), &PaykanObject_None);
  Paykan_release(ls);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, LineWithoutTrailingNewline) {
  std::string path = makeTempFile("no newline");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *ls = PaykanFile_readln(fObj);
  PaykanString *l = (PaykanString *)PaykanShared_get(ls);
  EXPECT_EQ(std::string(l->data, (size_t)l->len), "no newline");
  Paykan_release(ls);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, LongLineTriggersBufferGrowth) {
  // Write a line longer than the initial 128-byte buffer.
  std::string longLine(300, 'x');
  longLine += '\n';
  std::string path = makeTempFile(longLine.c_str());
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *ls = PaykanFile_readln(fObj);
  PaykanString *l = (PaykanString *)PaykanShared_get(ls);
  EXPECT_EQ((size_t)l->len, longLine.size());
  EXPECT_EQ(std::string(l->data, (size_t)l->len), longLine);
  Paykan_release(ls);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

// -- write + readln round-trip

TEST(FileRoundTrip, WriteAndReadBack) {
  char path[] = "/tmp/paykan_roundtrip_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  // Write two lines.
  {
    PaykanObject *pathStr = strObj(path);
    PaykanObject *modeStr = strObj("w");
    PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
    PaykanObject *fObj = PaykanShared_get(fShared);
    ASSERT_NE(fObj, &PaykanObject_None);

    PaykanObject *l1 = strObj("first line\n");
    PaykanObject *l2 = strObj("second line\n");
    PaykanFile_write(fObj, l1);
    PaykanFile_write(fObj, l2);
    PaykanString_destroy(l1);
    PaykanString_destroy(l2);

    Paykan_release(fShared);
    Paykan_dispose((PaykanObject *)pathStr);
    Paykan_dispose((PaykanObject *)modeStr);
  }

  // Read them back.
  {
    PaykanObject *pathStr = strObj(path);
    PaykanObject *modeStr = strObj("r");
    PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
    PaykanObject *fObj = PaykanShared_get(fShared);
    ASSERT_NE(fObj, &PaykanObject_None);

    PaykanShared *ls1 = PaykanFile_readln(fObj);
    PaykanString *l1 = (PaykanString *)PaykanShared_get(ls1);
    EXPECT_EQ(std::string(l1->data, (size_t)l1->len), "first line\n");
    Paykan_release(ls1);

    PaykanShared *ls2 = PaykanFile_readln(fObj);
    PaykanString *l2 = (PaykanString *)PaykanShared_get(ls2);
    EXPECT_EQ(std::string(l2->data, (size_t)l2->len), "second line\n");
    Paykan_release(ls2);

    Paykan_release(fShared);
    Paykan_dispose((PaykanObject *)pathStr);
    Paykan_dispose((PaykanObject *)modeStr);
  }

  std::remove(path);
}

// -- PaykanFile_readbytes

TEST(FileReadbytes, ReadsUpToN) {
  std::string path = makeTempFile("hello world");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 5);
  PaykanString *s = (PaykanString *)PaykanShared_get(chunk);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "hello");
  Paykan_release(chunk);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadbytes, ReadsLessThanNAtEOF) {
  std::string path = makeTempFile("hi");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 100);
  PaykanString *s = (PaykanString *)PaykanShared_get(chunk);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "hi");
  Paykan_release(chunk);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileReadbytes, ReturnsNoneAtEOF) {
  std::string path = makeTempFile("");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 8);
  EXPECT_EQ(PaykanShared_get(chunk), &PaykanObject_None);
  Paykan_release(chunk);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

// -- PaykanFile_read

TEST(FileRead, ReadsEntireFile) {
  std::string path = makeTempFile("line1\nline2\n");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *all = PaykanFile_read(fObj);
  PaykanString *s = (PaykanString *)PaykanShared_get(all);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "line1\nline2\n");
  Paykan_release(all);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

TEST(FileRead, ReturnsNoneOnEmptyFile) {
  std::string path = makeTempFile("");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr, &openErr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *all = PaykanFile_read(fObj);
  EXPECT_EQ(PaykanShared_get(all), &PaykanObject_None);
  Paykan_release(all);

  Paykan_release(fShared);
  Paykan_dispose((PaykanObject *)pathStr);
  Paykan_dispose((PaykanObject *)modeStr);
  std::remove(path.c_str());
}

// -- Update modes ("r+", "w+", "a+"): reads and writes on one stream

/// Everything in the file at @p path.
static std::string fileText(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "cannot reopen " << path;
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/// The file at @p path opened with @p mode through ::io's native.
static PaykanShared *ioOpen(const std::string &path, const char *mode) {
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj(mode);
  int64_t err = 0;
  PaykanShared *f = Paykan_io_open(pathStr, modeStr, &err);
  Paykan_dispose(pathStr);
  Paykan_dispose(modeStr);
  EXPECT_NE(f, nullptr) << path << " (" << mode << "): errno " << err;
  return f;
}

/// ::io's write of @p s: its errno, 0 when it wrote.
static int64_t ioWrite(PaykanShared *f, const char *s) {
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

static std::string ioReadln(PaykanShared *f) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_readln(f, &err);
  return ioText(r, err);
}

static std::string ioReadbytes(PaykanShared *f, int64_t n) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_readbytes(f, n, &err);
  return ioText(r, err);
}

static std::string ioRead(PaykanShared *f) {
  int64_t err = 0;
  PaykanShared *r = Paykan_io_read(f, &err);
  return ioText(r, err);
}

/// What a read through a PaykanFile method gave: its text, or "None".
static std::string methodText(PaykanShared *r) {
  PaykanObject *o = PaykanShared_get(r);
  std::string text = o == &PaykanObject_None
                         ? "None"
                         : std::string(((PaykanString *)o)->data,
                                       (size_t)((PaykanString *)o)->len);
  Paykan_release(r);
  return text;
}

// C lets a read follow a write on an update stream only after an fflush, and
// a write follow a read only after a file positioning call (C11 7.21.5.3);
// without them the C library dropped the first write.  In "w+": the read
// after the write finds the end of the file, and the second write follows
// the first.
TEST(FileUpdate, WPlusKeepsAWriteThatAReadFollows) {
  std::string path = makeTempFile("old\n");
  ASSERT_FALSE(path.empty());
  PaykanShared *f = ioOpen(path, "w+");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(ioWrite(f, "hello\n"), 0);
  EXPECT_EQ(ioRead(f), "EOF");
  EXPECT_EQ(ioWrite(f, "world\n"), 0);
  Paykan_release(f);
  EXPECT_EQ(fileText(path), "hello\nworld\n");
  std::remove(path.c_str());
}

// In "r+", from the start of the file: each read starts where the write
// before it ended, and each write where the read before it stopped (or at
// the end, after a read that reached it), through every read.
TEST(FileUpdate, RPlusReadsAndWritesWhereTheOtherStopped) {
  std::string path = makeTempFile("abcdef\nghi\n");
  ASSERT_FALSE(path.empty());
  PaykanShared *f = ioOpen(path, "r+");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(ioWrite(f, "XY"), 0);
  EXPECT_EQ(ioReadbytes(f, 3), "cde");
  EXPECT_EQ(ioWrite(f, "Z"), 0);
  EXPECT_EQ(ioReadln(f), "\n");
  EXPECT_EQ(ioWrite(f, "W"), 0);
  EXPECT_EQ(ioRead(f), "hi\n");
  EXPECT_EQ(ioWrite(f, "!"), 0);
  EXPECT_EQ(ioReadln(f), "EOF");
  Paykan_release(f);
  EXPECT_EQ(fileText(path), "XYcdeZ\nWhi\n!");
  std::remove(path.c_str());
}

// In "a+" every write goes to the end of the file, so the read after one
// finds the end (where C leaves the first read before any write differs:
// the start on Linux, the end on macOS, so the test does not read first).
TEST(FileUpdate, APlusKeepsAWriteThatAReadFollows) {
  std::string path = makeTempFile("abcdef\n");
  ASSERT_FALSE(path.empty());
  PaykanShared *f = ioOpen(path, "a+");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(ioWrite(f, "hello\n"), 0);
  EXPECT_EQ(ioRead(f), "EOF");
  EXPECT_EQ(ioWrite(f, "world\n"), 0);
  EXPECT_EQ(ioReadbytes(f, 4), "EOF");
  Paykan_release(f);
  EXPECT_EQ(fileText(path), "abcdef\nhello\nworld\n");
  std::remove(path.c_str());
}

// The PaykanFile methods (the File vtable's slots) follow the same rule.
TEST(FileUpdate, TheFileMethodsMixReadsAndWrites) {
  std::string path = makeTempFile("abcdef\n");
  ASSERT_FALSE(path.empty());
  PaykanShared *f = ioOpen(path, "r+");
  ASSERT_NE(f, nullptr);
  PaykanObject *x = strObj("X");
  PaykanObject *y = strObj("Y");
  PaykanFile_write(f, x);
  EXPECT_EQ(methodText(PaykanFile_readbytes(f, 2)), "bc");
  PaykanFile_write(f, y);
  EXPECT_EQ(methodText(PaykanFile_readln(f)), "ef\n");
  PaykanFile_write(f, x);
  EXPECT_EQ(methodText(PaykanFile_read(f)), "None");
  Paykan_release(f);
  EXPECT_EQ(fileText(path), "XbcYef\nX");

  f = ioOpen(path, "w+");
  ASSERT_NE(f, nullptr);
  PaykanFile_write(f, x);
  EXPECT_EQ(methodText(PaykanFile_readln(f)), "None");
  PaykanFile_write(f, y);
  Paykan_release(f);
  EXPECT_EQ(fileText(path), "XY");

  Paykan_dispose(x);
  Paykan_dispose(y);
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
  PaykanShared *f = ioOpen("/dev/full", "r+");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(ioWrite(f, "hello"), 0); // buffered
  // (Not EXPECT_EQ, which would print the 64 KiB of zeros it read before.)
  std::string got = ioReadbytes(f, 65536);
  EXPECT_TRUE(got == "errno " + std::to_string(ENOSPC))
      << got.size() << " bytes: " << got.substr(0, 16);
  EXPECT_EQ(Paykan_error_code(ENOSPC), PAYKAN_ERROR_NO_SPACE);
  Paykan_release(f);
}

// -- PaykanFile_Stdin singleton safety

TEST(FileStdin, SingletonHasCorrectVtable) {
  // Stdin uses a separate vtable (no-op destroy), but file operations must
  // match.
  PaykanMethod *vt = PaykanFile_Stdin.vtable;
  ASSERT_NE(vt, nullptr);
  EXPECT_NE(vt, PaykanFile_vtable);
  for (int slot : {PAYKAN_SLOT_FILE_READ, PAYKAN_SLOT_FILE_READLN,
                   PAYKAN_SLOT_FILE_READBYTES, PAYKAN_SLOT_FILE_WRITE})
    EXPECT_EQ(vt[slot], PaykanFile_vtable[slot]) << "slot " << slot;
}

TEST(FileStdin, SingletonHandleIsStdin) {
  EXPECT_EQ(PaykanFile_Stdin.handle, stdin);
}
