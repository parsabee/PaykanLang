// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The .pkm container (docs/design/pkm.md §1): the writer's layout, the
// reader's checks of §1.3/§1.7, derived hashes, hostile input and files.

#include "PKMTestUtils.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace pkmtest;

namespace {

TEST(Container, WriterLayoutAndRoundTrip) {
  std::vector<uint8_t> b = sampleFileBytes();
  ASSERT_GE(b.size(), 64u);
  EXPECT_EQ(std::memcmp(b.data(), "\x89PKM\r\n\x1a\n", 8), 0);
  EXPECT_EQ(getU32(b, 8), kFormatVersion); // the release's
  EXPECT_EQ(getU32(b, 12), 64u);
  EXPECT_EQ(getU32(b, 16), 6u);
  EXPECT_EQ(getU32(b, 20), 64u);

  StatusOr<File> f = File::read(b);
  ASSERT_TRUE(f) << f.status().message();
  const auto &s = (*f).sections();
  ASSERT_EQ(s.size(), 6u);
  // Sorted by (kind, offset), 8-aligned, past the table, non-overlapping.
  std::vector<uint32_t> kinds;
  for (size_t i = 0; i < s.size(); ++i) {
    kinds.push_back(s[i].Kind);
    EXPECT_EQ(s[i].Offset % 8, 0u);
    EXPECT_GE(s[i].Offset, 64u + 64u * 6);
    if (i > 0) {
      EXPECT_GE(s[i].Offset, s[i - 1].Offset + s[i - 1].Size);
    }
    EXPECT_EQ(support::sha256((*f).section(i)), s[i].Hash);
  }
  EXPECT_EQ(kinds, (std::vector<uint32_t>{0x0001, 0x0100, 0x0200, 0x0201,
                                          0x0210, 0x0210}));
  EXPECT_EQ((*f).count(Kind::Payload), 2u);
  EXPECT_EQ((*f).count(Kind::Tmpl), 0u);
  EXPECT_EQ(vec((*f).section(Kind::Payload, 0)), bytesOf("payload one"));
  EXPECT_EQ(vec((*f).section(Kind::Payload, 1)),
            bytesOf("payload two, longer"));
  EXPECT_TRUE((*f).section(Kind::Payload, 2).empty());
  EXPECT_TRUE((*f).section(Kind::Debug).empty());
  EXPECT_EQ((*f).manifest(), sampleManifest());
  EXPECT_EQ((*f).formatVersion(), kFormatVersion);
}

TEST(Container, DerivedHashes) {
  StatusOr<File> f = File::read(sampleFileBytes());
  ASSERT_TRUE(f);
  EXPECT_EQ((*f).ifaceHash(), support::sha256((*f).section(Kind::Iface)));
  EXPECT_EQ((*f).codeHash(), support::sha256((*f).section(Kind::Code)));
  EXPECT_EQ((*f).tmplHash(), support::Hash256{});
  support::Sha256 h;
  for (Kind k : {Kind::Manifest, Kind::Iface, Kind::Tmpl, Kind::Code}) {
    support::Hash256 part =
        k == Kind::Tmpl ? support::Hash256{} : support::sha256((*f).section(k));
    h.update(part.data(), part.size());
  }
  EXPECT_EQ((*f).moduleHash(), h.finish());

  // Adding or stripping a payload leaves the module hash unchanged.
  Writer w;
  w.add(Kind::Manifest, kFlagRequired, encodeManifest(sampleManifest()));
  w.add(Kind::Iface, kFlagRequired, writeInterface(sampleInterface()));
  w.add(Kind::Code, kFlagRequired, bytesOf("binary pir bytes here"));
  StatusOr<File> g = File::read(*w.finish());
  ASSERT_TRUE(g);
  EXPECT_EQ((*g).moduleHash(), (*f).moduleHash());
}

TEST(Container, Deterministic) {
  EXPECT_EQ(sampleFileBytes(), sampleFileBytes());
  EXPECT_EQ(minimalFileBytes(), minimalFileBytes());
}

TEST(Container, WriterRefusals) {
  Writer none;
  EXPECT_FALSE(none.finish());
  Writer noManifest;
  noManifest.add(Kind::Iface, kFlagRequired, bytesOf("x"));
  EXPECT_FALSE(noManifest.finish());
  Writer twice;
  twice.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest()));
  twice.add(Kind::Code, kFlagRequired, bytesOf("a"));
  twice.add(Kind::Code, kFlagRequired, bytesOf("b"));
  EXPECT_FALSE(twice.finish());
  Writer twoManifests;
  twoManifests.add(Kind::Manifest, kFlagRequired, bytesOf("a"));
  twoManifests.add(Kind::Manifest, kFlagRequired, bytesOf("b"));
  EXPECT_FALSE(twoManifests.finish());
  Writer badFlags;
  badFlags.add(Kind::Manifest, 8, bytesOf("a"));
  EXPECT_FALSE(badFlags.finish());
  // A private kind, kept in the table and never interpreted.
  Writer priv;
  priv.add(Kind::Manifest, kFlagRequired, encodeManifest(minimalManifest()));
  priv.add(0x7F01u, 0, bytesOf("tool data"));
  StatusOr<std::vector<uint8_t>> b = priv.finish();
  ASSERT_TRUE(b);
  StatusOr<File> f = File::read(*b);
  ASSERT_TRUE(f) << f.status().message();
  EXPECT_EQ((*f).sections().size(), 2u);
  EXPECT_EQ(vec((*f).section(1)), bytesOf("tool data"));
}

TEST(Container, HeaderErrors) {
  std::vector<uint8_t> b = minimalFileBytes();
  {
    std::vector<uint8_t> t(b.begin(), b.begin() + 20);
    EXPECT_EQ(readError(t).Code, ErrorCode::Truncated);
  }
  {
    std::vector<uint8_t> t = b;
    t[1] = 'Q';
    EXPECT_EQ(readError(t).Code, ErrorCode::NotAPkm);
  }
  {
    // Any other version, older or newer, is rejected.
    for (uint32_t other : {kFormatVersion - 1, kFormatVersion + 1}) {
      std::vector<uint8_t> t = b;
      putU32(t, 8, other);
      Error e = readError(t);
      EXPECT_EQ(e.Code, ErrorCode::UnsupportedFormat);
      EXPECT_EQ(e.str(), "format " + versionString(other) +
                             "; this paykan reads " +
                             versionString(kFormatVersion));
    }
  }
  {
    std::vector<uint8_t> t = b;
    putU32(t, 12, 48);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  for (uint32_t count : {0u, 1025u}) {
    std::vector<uint8_t> t = b;
    putU32(t, 16, count);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU32(t, 20, 32);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU64(t, 24, 60);
    EXPECT_EQ(readError(t).Code, ErrorCode::BadHeader);
  }
  {
    std::vector<uint8_t> t = b;
    putU64(t, 24, 1ull << 62); // overflow-safe
    EXPECT_EQ(readError(t).Code, ErrorCode::Truncated);
  }
  {
    std::vector<uint8_t> t = b;
    t[40] ^= 1;
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::HashMismatch);
    EXPECT_EQ(e.Section, -1);
  }
}

TEST(Container, TableErrors) {
  std::vector<uint8_t> b = minimalFileBytes();
  auto patched = [&](auto edit) {
    std::vector<uint8_t> t = b;
    edit(t);
    rehash(t);
    return t;
  };
  // Unsorted: swap the two entries.
  {
    std::vector<uint8_t> t = patched([](std::vector<uint8_t> &t) {
      std::vector<uint8_t> e0(t.begin() + 64, t.begin() + 128);
      std::copy(t.begin() + 128, t.begin() + 192, t.begin() + 64);
      std::copy(e0.begin(), e0.end(), t.begin() + 128);
    });
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::BadSectionTable);
    EXPECT_EQ(e.Section, 1);
    EXPECT_EQ(e.Kind, 0x0001u);
  }
  // Overlap: the IFACE entry starts inside the manifest.
  {
    uint64_t manifestOff = 64 + 64 * 2;
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU64(t, entryAt(1) + 8, manifestOff);
    });
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::BadSectionTable);
    EXPECT_EQ(e.Section, 1);
    EXPECT_EQ(e.Kind, 0x0100u);
    EXPECT_EQ(e.str().substr(0, 20), "section IFACE (#1) a");
  }
  // Alignment.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU64(t, entryAt(1) + 8, getU64(t, entryAt(1) + 8) + 4);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Offset inside the table.
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU64(t, entryAt(0) + 8, 64); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Size past the end (and overflow).
  for (uint64_t size : {uint64_t(1) << 20, ~uint64_t(0)}) {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU64(t, entryAt(1) + 16, size); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Unknown required kind.
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1), 0x7F00); });
    EXPECT_EQ(readError(t).Code, ErrorCode::UnknownRequiredSection);
  }
  // Unknown optional kind is kept; but then IFACE is missing from the
  // manifest's point of view only when CODE is claimed -- here it reads.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(1), 0x7F00);
      putU32(t, entryAt(1) + 4, 0);
    });
    StatusOr<File> f = File::read(t);
    ASSERT_TRUE(f) << f.status().message();
    EXPECT_EQ((*f).sections()[1].Kind, 0x7F00u);
    EXPECT_TRUE((*f).section(Kind::Iface).empty());
  }
  // Duplicate kind: the IFACE entry relabelled MANIFEST (offset order kept).
  {
    std::vector<uint8_t> t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1), 0x0001); });
    EXPECT_EQ(readError(t).Code, ErrorCode::DuplicateSection);
  }
  // No MANIFEST: relabel it IFACE and the IFACE a payload.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(0), 0x0100);
      putU32(t, entryAt(1), 0x0210);
      putU32(t, entryAt(1) + 4, kFlagInstanced);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::MissingManifest);
  }
  // INSTANCED on a kind that may not repeat; unknown flag bits.
  {
    std::vector<uint8_t> t = patched([&](std::vector<uint8_t> &t) {
      putU32(t, entryAt(1) + 4, kFlagRequired | kFlagInstanced);
    });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
    t = patched(
        [&](std::vector<uint8_t> &t) { putU32(t, entryAt(1) + 4, 0x10); });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Reserved != 0.
  {
    std::vector<uint8_t> t =
        patched([&](std::vector<uint8_t> &t) { t[entryAt(0) + 56] = 1; });
    EXPECT_EQ(readError(t).Code, ErrorCode::BadSectionTable);
  }
  // Section hash mismatch, with the documented message.
  {
    std::vector<uint8_t> t = b;
    size_t ifaceOff =
        static_cast<size_t>(File::read(b).value().sections()[1].Offset);
    t[ifaceOff + 4] ^= 0xFF;
    Error e = readError(t);
    EXPECT_EQ(e.Code, ErrorCode::HashMismatch);
    EXPECT_EQ(e.Section, 1);
    char expect[64];
    std::snprintf(expect, sizeof expect,
                  "section IFACE (#1) at offset 0x%zx: hash mismatch",
                  ifaceOff);
    EXPECT_EQ(e.str(), expect);
    // The test hook skips step 4.
    ReadOptions lax;
    lax.VerifyHashes = false;
    EXPECT_TRUE(File::read(t, lax));
  }
  // A manifest that disagrees with the table (HAS_CODE without CODE).
  {
    Manifest m = minimalManifest();
    m.Contents |= kContentsHasCode;
    Writer w;
    w.add(Kind::Manifest, kFlagRequired, encodeManifest(m));
    Interface i;
    i.Module = "m";
    w.add(Kind::Iface, kFlagRequired, writeInterface(i));
    Error e = readError(*w.finish());
    EXPECT_EQ(e.Code, ErrorCode::BadManifest);
    EXPECT_EQ(e.Section, 0);
  }
  {
    Writer w;
    w.add(Kind::Manifest, kFlagRequired, bytesOf("PKMMxxxx"));
    EXPECT_EQ(readError(*w.finish()).Code, ErrorCode::BadManifest);
  }
}

TEST(Container, TruncationAndBitFlipsNeverCrash) {
  std::vector<uint8_t> b = minimalFileBytes();
  for (size_t n = 0; n < b.size(); ++n) {
    std::vector<uint8_t> t(b.begin(), b.begin() + n);
    EXPECT_FALSE(File::read(t)) << "truncated to " << n;
  }
  // Bytes no check covers: the format minor (a higher minor is accepted)
  // and the zero padding between sections.
  std::vector<bool> unchecked(b.size(), false);
  unchecked[10] = unchecked[11] = true;
  StatusOr<File> f = File::read(b);
  ASSERT_TRUE(f);
  uint64_t end = 64 + 64 * (*f).sections().size();
  for (const SectionEntry &e : (*f).sections()) {
    for (uint64_t p = end; p < e.Offset; ++p)
      unchecked[p] = true;
    end = e.Offset + e.Size;
  }
  for (size_t at = 0; at < b.size(); ++at)
    for (uint8_t mask : {uint8_t(1), uint8_t(0x80), uint8_t(0xFF)}) {
      std::vector<uint8_t> t = b;
      t[at] ^= mask;
      StatusOr<File> r = File::read(t);
      if (!unchecked[at]) {
        EXPECT_FALSE(r) << "flip at " << at << " mask " << unsigned(mask);
      }
    }
}

/// A directory of the temporary directory for one test of this process:
/// build trees that run the suite at once never share one.
std::filesystem::path testDir(const std::string &name) {
  return std::filesystem::temp_directory_path() /
         ("paykan_pkm_" + name + "_" + std::to_string(getpid()));
}

TEST(Container, Files) {
  namespace fs = std::filesystem;
  fs::path dir = testDir("files");
  fs::remove_all(dir);
  fs::path file = dir / "a" / "b.pkm";
  std::vector<uint8_t> b = minimalFileBytes();
  // The directory must exist: the file's writer makes none.
  Status missing = writeFileAtomically(file, b);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.message(),
            "the directory '" + (dir / "a").string() + "' does not exist");
  EXPECT_FALSE(fs::exists(dir));
  fs::create_directories(dir / "a");
  Status s = writeFileAtomically(file, b);
  ASSERT_TRUE(s) << s.message();
  StatusOr<std::vector<uint8_t>> back = readFileBytes(file);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, b);
  // No temporary left behind.
  size_t entries = 0;
  for (const auto &e : fs::directory_iterator(dir / "a")) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 1u);
  EXPECT_FALSE(readFileBytes(dir / "missing.pkm"));
  // A directory is an error, not an exception from the stream (finding 33).
  StatusOr<std::vector<uint8_t>> ofDir = readFileBytes(dir / "a");
  ASSERT_FALSE(ofDir);
  EXPECT_EQ(ofDir.status().message(),
            "'" + (dir / "a").string() + "' is a directory, not a file");
  fs::remove_all(dir);
}

// What is not a regular file is written in place: the rename that replaces
// a regular file whole would replace the node itself, so `--emit-pkm -o`
// made a symbolic link, a device (`-o /dev/null`, run as root, replaced
// /dev/null) or a FIFO a regular file.  The device here is reached through
// a link, so this test never writes over /dev/null itself.
TEST(Container, WritesIntoWhatIsNotARegularFile) {
  namespace fs = std::filesystem;
  fs::path dir = testDir("nodes");
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<uint8_t> b = minimalFileBytes();
  // A link to a file: the file gets the bytes, and the link stays.
  std::ofstream(dir / "file") << "old";
  fs::create_symlink("file", dir / "link");
  Status s = writeFileAtomically(dir / "link", b);
  ASSERT_TRUE(s) << s.message();
  EXPECT_TRUE(fs::is_symlink(dir / "link"));
  StatusOr<std::vector<uint8_t>> back = readFileBytes(dir / "file");
  ASSERT_TRUE(back) << back.status().message();
  EXPECT_EQ(*back, b);
  // A link to a device: the device stays one.
  fs::create_symlink("/dev/null", dir / "null");
  s = writeFileAtomically(dir / "null", b);
  ASSERT_TRUE(s) << s.message();
  EXPECT_TRUE(fs::is_symlink(dir / "null"));
  EXPECT_TRUE(fs::is_character_file(dir / "null"));
  // A FIFO stays one, and its reader gets the bytes.
  fs::path fifo = dir / "fifo";
  ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0) << std::strerror(errno);
  int reader = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
  ASSERT_GE(reader, 0) << std::strerror(errno);
  s = writeFileAtomically(fifo, b);
  EXPECT_TRUE(s) << s.message();
  EXPECT_TRUE(fs::is_fifo(fs::symlink_status(fifo)));
  std::vector<uint8_t> got(b.size() + 1);
  ssize_t n = read(reader, got.data(), got.size());
  close(reader);
  ASSERT_EQ(n, static_cast<ssize_t>(b.size()));
  got.resize(b.size());
  EXPECT_EQ(got, b);
  // A directory is an error that says why.
  fs::create_directories(dir / "d");
  Status ofDir = writeFileAtomically(dir / "d", b);
  ASSERT_FALSE(ofDir);
  EXPECT_EQ(ofDir.message(), std::strerror(EISDIR));
  // No temporary file is left behind.
  size_t entries = 0;
  for (const auto &e : fs::directory_iterator(dir)) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 5u);
  fs::remove_all(dir);
}

} // namespace
