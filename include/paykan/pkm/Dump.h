// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// `paykan pkm dump` (docs/design/pkm.md §8.5): a stable, golden-tested text
// rendering of a module file.  Deterministic and path-free so two dumps of
// one module compare equal anywhere.

#pragma once

#include "paykan/pkm/File.h"

#include <iosfwd>

namespace paykan::pkm {

struct DumpOptions {
  enum class Section { All, Manifest, Sections, Iface, SymIdx, Payloads };
  Section Which = Section::All;
};

/// Header, section table, manifest (every field), interface (every record),
/// `code`/`symidx`/`debug` as size and hash, payload headers as
/// `kind 0x0210 <size> B (not decoded)`.  The driver appends the decoded PIR
/// text of CODE itself.
void dump(const File &file, std::ostream &os, const DumpOptions &opts = {});

} // namespace paykan::pkm
