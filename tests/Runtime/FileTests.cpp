// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanFile runtime functions.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"

// ============================================================================
// Helpers
// ============================================================================

// Create a temporary file, write content into it, rewind, and return the path.
// The caller is responsible for removing the file with std::remove().
static std::string makeTempFile(const char *content) {
  char path[] = "/tmp/paykan_file_test_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return "";
  if (content && *content)
    write(fd, content, strlen(content));
  close(fd);
  return std::string(path);
}

static PaykanObject *strObj(const char *s) {
  return (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s));
}

// ============================================================================
// PaykanFile_new
// ============================================================================

TEST(FileNew, AllocatesWithNullHandle) {
  PaykanFile *f = PaykanFile_new();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->handle, nullptr);
  PaykanFile_destroy((PaykanObject *)f);
}

TEST(FileNew, VtableIsFileVtable) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ((void *)f->vtable, (void *)&PaykanFile_vtable);
  PaykanFile_destroy((PaykanObject *)f);
}

// ============================================================================
// PaykanFile_destroy
// ============================================================================

TEST(FileDestroy, ClosesOpenHandle) {
  // Open a real file, then destroy — fclose should be called without crashing.
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());
  PaykanFile *f = PaykanFile_new();
  f->handle = fopen(path.c_str(), "r");
  ASSERT_NE(f->handle, nullptr);
  PaykanFile_destroy((PaykanObject *)f); // must not crash
  std::remove(path.c_str());
}

TEST(FileDestroy, NullHandleIsHarmless) {
  PaykanFile *f = PaykanFile_new();
  f->handle = nullptr;
  PaykanFile_destroy((PaykanObject *)f); // must not crash
}

// ============================================================================
// PaykanFile_toString
// ============================================================================

TEST(FileToString, ContainsFilePrefix) {
  PaykanFile *f = PaykanFile_new();
  PaykanShared *shared = PaykanFile_toString((PaykanObject *)f);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  EXPECT_NE(std::string(s->data).find("File@"), std::string::npos);
  Paykan_release(shared);
  PaykanFile_destroy((PaykanObject *)f);
}

// ============================================================================
// PaykanFile_equals
// ============================================================================

TEST(FileEquals, SameObjectIsEqual) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ(
      paykanTestEquals(PaykanFile_equals, (PaykanObject *)f, (PaykanObject *)f),
      1);
  PaykanFile_destroy((PaykanObject *)f);
}

TEST(FileEquals, DifferentObjectsAreNotEqual) {
  PaykanFile *a = PaykanFile_new();
  PaykanFile *b = PaykanFile_new();
  EXPECT_EQ(
      paykanTestEquals(PaykanFile_equals, (PaykanObject *)a, (PaykanObject *)b),
      0);
  PaykanFile_destroy((PaykanObject *)a);
  PaykanFile_destroy((PaykanObject *)b);
}

// ============================================================================
// PaykanFile_open
// ============================================================================

TEST(FileOpen, SuccessReturnsFile) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");

  PaykanShared *shared = PaykanFile_open(pathStr, modeStr);
  ASSERT_NE(shared, nullptr);
  PaykanObject *obj = PaykanShared_get(shared);
  // Should NOT be None — it must be a PaykanFile with a valid handle.
  EXPECT_NE(obj, &PaykanObject_None);
  EXPECT_NE(((PaykanFile *)obj)->handle, nullptr);

  Paykan_release(shared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileOpen, FailureReturnsError) {
  PaykanObject *pathStr = strObj("/no/such/path/that/exists");
  PaykanObject *modeStr = strObj("r");

  PaykanShared *shared = PaykanFile_open(pathStr, modeStr);
  ASSERT_NE(shared, nullptr);
  PaykanObject *obj = PaykanShared_get(shared);
  // open failure: returns an Error object describing the failure.
  EXPECT_NE(obj, nullptr);
  EXPECT_NE(obj, &PaykanObject_None);
  EXPECT_EQ(obj->vtable, &PaykanError_vtable);

  Paykan_release(shared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
}

TEST(FileOpen, OpenForWrite) {
  char path[] = "/tmp/paykan_file_write_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *shared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *obj = PaykanShared_get(shared);
  EXPECT_NE(obj, &PaykanObject_None);

  Paykan_release(shared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path);
}

// ============================================================================
// PaykanFile_write
// ============================================================================

TEST(FileWrite, WritesContentToFile) {
  char path[] = "/tmp/paykan_file_write2_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  // Open for writing.
  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanObject *content = strObj("hello paykan\n");
  PaykanFile_write(fObj, content);
  PaykanString_destroy(content);

  Paykan_release(fShared); // closes handle via destroy
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);

  // Read back and verify.
  FILE *fp = fopen(path, "r");
  ASSERT_NE(fp, nullptr);
  char buf[64] = {};
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  EXPECT_EQ(std::string(buf, n), "hello paykan\n");
  std::remove(path);
}

TEST(FileWrite, EmptyStringIsHarmless) {
  char path[] = "/tmp/paykan_file_empty_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  PaykanObject *pathStr = strObj(path);
  PaykanObject *modeStr = strObj("w");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanObject *empty = strObj("");
  PaykanFile_write(fObj, empty); // must not crash
  PaykanString_destroy(empty);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path);
}

// ============================================================================
// PaykanFile_readln
// ============================================================================

TEST(FileReadln, ReadsSingleLine) {
  std::string path = makeTempFile("hello\nworld\n");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *lineShared = PaykanFile_readln(fObj);
  PaykanString *line = (PaykanString *)PaykanShared_get(lineShared);
  EXPECT_EQ(std::string(line->data, (size_t)line->len), "hello\n");
  Paykan_release(lineShared);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, ReadsMultipleLines) {
  std::string path = makeTempFile("line1\nline2\nline3\n");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
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
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, ReturnsNoneAtEOF) {
  std::string path = makeTempFile("");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  // EOF with no bytes read signals end-of-file by returning None.
  PaykanShared *ls = PaykanFile_readln(fObj);
  EXPECT_EQ(PaykanShared_get(ls), &PaykanObject_None);
  Paykan_release(ls);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileReadln, LineWithoutTrailingNewline) {
  std::string path = makeTempFile("no newline");
  ASSERT_FALSE(path.empty());

  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *ls = PaykanFile_readln(fObj);
  PaykanString *l = (PaykanString *)PaykanShared_get(ls);
  EXPECT_EQ(std::string(l->data, (size_t)l->len), "no newline");
  Paykan_release(ls);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
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
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *ls = PaykanFile_readln(fObj);
  PaykanString *l = (PaykanString *)PaykanShared_get(ls);
  EXPECT_EQ((size_t)l->len, longLine.size());
  EXPECT_EQ(std::string(l->data, (size_t)l->len), longLine);
  Paykan_release(ls);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

// ============================================================================
// write + readln round-trip
// ============================================================================

TEST(FileRoundTrip, WriteAndReadBack) {
  char path[] = "/tmp/paykan_roundtrip_XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);

  // Write two lines.
  {
    PaykanObject *pathStr = strObj(path);
    PaykanObject *modeStr = strObj("w");
    PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
    PaykanObject *fObj = PaykanShared_get(fShared);
    ASSERT_NE(fObj, &PaykanObject_None);

    PaykanObject *l1 = strObj("first line\n");
    PaykanObject *l2 = strObj("second line\n");
    PaykanFile_write(fObj, l1);
    PaykanFile_write(fObj, l2);
    PaykanString_destroy(l1);
    PaykanString_destroy(l2);

    Paykan_release(fShared);
    PaykanString_destroy(pathStr);
    PaykanString_destroy(modeStr);
  }

  // Read them back.
  {
    PaykanObject *pathStr = strObj(path);
    PaykanObject *modeStr = strObj("r");
    PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
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
    PaykanString_destroy(pathStr);
    PaykanString_destroy(modeStr);
  }

  std::remove(path);
}

// ============================================================================
// PaykanFile_readbytes
// ============================================================================

TEST(FileReadbytes, ReadsUpToN) {
  std::string path = makeTempFile("hello world");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 5);
  PaykanString *s = (PaykanString *)PaykanShared_get(chunk);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "hello");
  Paykan_release(chunk);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileReadbytes, ReadsLessThanNAtEOF) {
  std::string path = makeTempFile("hi");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 100);
  PaykanString *s = (PaykanString *)PaykanShared_get(chunk);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "hi");
  Paykan_release(chunk);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileReadbytes, ReturnsNoneAtEOF) {
  std::string path = makeTempFile("");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *chunk = PaykanFile_readbytes(fObj, 8);
  EXPECT_EQ(PaykanShared_get(chunk), &PaykanObject_None);
  Paykan_release(chunk);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

// ============================================================================
// PaykanFile_read
// ============================================================================

TEST(FileRead, ReadsEntireFile) {
  std::string path = makeTempFile("line1\nline2\n");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *all = PaykanFile_read(fObj);
  PaykanString *s = (PaykanString *)PaykanShared_get(all);
  EXPECT_EQ(std::string(s->data, (size_t)s->len), "line1\nline2\n");
  Paykan_release(all);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

TEST(FileRead, ReturnsNoneOnEmptyFile) {
  std::string path = makeTempFile("");
  PaykanObject *pathStr = strObj(path.c_str());
  PaykanObject *modeStr = strObj("r");
  PaykanShared *fShared = PaykanFile_open(pathStr, modeStr);
  PaykanObject *fObj = PaykanShared_get(fShared);
  ASSERT_NE(fObj, &PaykanObject_None);

  PaykanShared *all = PaykanFile_read(fObj);
  EXPECT_EQ(PaykanShared_get(all), &PaykanObject_None);
  Paykan_release(all);

  Paykan_release(fShared);
  PaykanString_destroy(pathStr);
  PaykanString_destroy(modeStr);
  std::remove(path.c_str());
}

// ============================================================================
// PaykanFile_Stdin singleton safety
// ============================================================================

TEST(FileStdin, SingletonHasCorrectVtable) {
  // Stdin uses a separate vtable (no-op destroy), but file operations must
  // match.
  auto *vt = (PaykanFileVTable *)PaykanFile_Stdin.vtable;
  ASSERT_NE(vt, nullptr);
  EXPECT_EQ((void *)vt->read, (void *)PaykanFile_vtable.read);
  EXPECT_EQ((void *)vt->readln, (void *)PaykanFile_vtable.readln);
  EXPECT_EQ((void *)vt->readbytes, (void *)PaykanFile_vtable.readbytes);
  EXPECT_EQ((void *)vt->write, (void *)PaykanFile_vtable.write);
}

TEST(FileStdin, SingletonHandleIsStdin) {
  EXPECT_EQ(PaykanFile_Stdin.handle, stdin);
}
