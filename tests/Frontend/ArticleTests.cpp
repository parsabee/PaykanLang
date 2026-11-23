// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The article before a quoted name in a diagnostic: a message is written
// "a '" + name + "'", and the engine prints "an" where the name is read
// with a vowel sound first.

#include "DiagEngine.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

// What the engine prints for an error with the message @p written, without
// the "error: " before it and the line's end.
std::string printed(const std::string &written) {
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.error(paykan::ast::SourceLocation(), written);
  const std::string text = os.str();
  const size_t at = text.find("error: ");
  if (at == std::string::npos || text.empty() || text.back() != '\n')
    return "<no error line: " + text + ">";
  return text.substr(at + 7, text.size() - at - 8);
}

} // namespace

TEST(Article, AVowelSoundTakesAn) {
  EXPECT_EQ(printed("'x' is a 'Obj'"), "'x' is an 'Obj'");
  EXPECT_EQ(printed("'x' is a 'int'"), "'x' is an 'int'");
  EXPECT_EQ(printed("a 'inout' parameter"), "an 'inout' parameter");
  EXPECT_EQ(printed("a 'async fn' is called"), "an 'async fn' is called");
  EXPECT_EQ(printed("a 'atomic shared Node'"), "an 'atomic shared Node'");
  EXPECT_EQ(printed("a 'unsigned'"), "an 'unsigned'");
  EXPECT_EQ(printed("a 'Entry<int>'"), "an 'Entry<int>'");
  EXPECT_EQ(printed("a 'Upper'"), "an 'Upper'");
  EXPECT_EQ(printed("a 'UnknownOp'"), "an 'UnknownOp'");
}

TEST(Article, AConsonantSoundTakesA) {
  EXPECT_EQ(printed("'x' is a 'Node'"), "'x' is a 'Node'");
  EXPECT_EQ(printed("an 'Node'"), "a 'Node'");
  EXPECT_EQ(printed("a 'shared mutable Node'"), "a 'shared mutable Node'");
  EXPECT_EQ(printed("a 'Str?'"), "a 'Str?'");
  EXPECT_EQ(printed("an 'HashMap<int, int>'"), "a 'HashMap<int, int>'");
}

// A vowel letter that is read with a consonant first, and a consonant
// letter that is not read at all.
TEST(Article, TheSoundDecidesNotTheLetter) {
  // "you": unique, unit, unary, user, usize, utf; but un-init, un-signed.
  EXPECT_EQ(printed("an 'unique'"), "a 'unique'");
  EXPECT_EQ(printed("an 'Unit'"), "a 'Unit'");
  EXPECT_EQ(printed("an 'UnaryExpr'"), "a 'UnaryExpr'");
  EXPECT_EQ(printed("an 'user'"), "a 'user'");
  EXPECT_EQ(printed("an 'usize'"), "a 'usize'");
  EXPECT_EQ(printed("an 'Utf8'"), "a 'Utf8'");
  EXPECT_EQ(printed("a 'Uninit'"), "an 'Uninit'");
  EXPECT_EQ(printed("a 'Unimplemented'"), "an 'Unimplemented'");
  EXPECT_EQ(printed("an 'Unimodal'"), "a 'Unimodal'");
  EXPECT_EQ(printed("a 'Urgent'"), "an 'Urgent'");
  EXPECT_EQ(printed("an 'UrlParser'"), "a 'UrlParser'");
  // "won": one, once, where they are one word of the name.
  EXPECT_EQ(printed("an 'OneShot'"), "a 'OneShot'");
  EXPECT_EQ(printed("an 'Once'"), "a 'Once'");
  EXPECT_EQ(printed("an 'OnceCell'"), "a 'OnceCell'");
  EXPECT_EQ(printed("an 'one_shot'"), "a 'one_shot'");
  EXPECT_EQ(printed("a 'Only'"), "an 'Only'");
  EXPECT_EQ(printed("a 'OnEvent'"), "an 'OnEvent'");
  EXPECT_EQ(printed("a 'OnError'"), "an 'OnError'");
  // "you" again, but for Euler.
  EXPECT_EQ(printed("an 'Europe'"), "a 'Europe'");
  EXPECT_EQ(printed("a 'EulerStep'"), "an 'EulerStep'");
  // A silent "h".
  EXPECT_EQ(printed("a 'Hour'"), "an 'Hour'");
  EXPECT_EQ(printed("a 'HonestCount'"), "an 'HonestCount'");
  EXPECT_EQ(printed("an 'Holder'"), "a 'Holder'");
}

// A name of one letter, a letter before a number and a letter before
// another capital (initials, or a letter of its own) are read by the
// letter's name: "an eff thirty-two", "a you eight", "an aitch tee tee pee
// client".
TEST(Article, ALetterIsReadByItsName) {
  EXPECT_EQ(printed("a 'F'"), "an 'F'");
  EXPECT_EQ(printed("a 'f32'"), "an 'f32'");
  EXPECT_EQ(printed("a 'i64'"), "an 'i64'");
  EXPECT_EQ(printed("a 'x'"), "an 'x'");
  EXPECT_EQ(printed("an 'u8'"), "a 'u8'");
  EXPECT_EQ(printed("an 'T'"), "a 'T'");
  EXPECT_EQ(printed("an 'K'"), "a 'K'");
  EXPECT_EQ(printed("a 'S3Bucket'"), "an 'S3Bucket'");
  EXPECT_EQ(printed("a 'H264Frame'"), "an 'H264Frame'");
  EXPECT_EQ(printed("a 'HTTPClient'"), "an 'HTTPClient'");
  EXPECT_EQ(printed("a 'XMLNode'"), "an 'XMLNode'");
  EXPECT_EQ(printed("a 'LRUCache'"), "an 'LRUCache'");
  EXPECT_EQ(printed("a 'RGB'"), "an 'RGB'");
  EXPECT_EQ(printed("a 'LValue'"), "an 'LValue'");
  EXPECT_EQ(printed("a 'IOError'"), "an 'IOError'");
  EXPECT_EQ(printed("an 'ULong'"), "a 'ULong'");
  EXPECT_EQ(printed("an 'UPSClient'"), "a 'UPSClient'");
  EXPECT_EQ(printed("an 'UInt'"), "a 'UInt'");
  EXPECT_EQ(printed("an 'CString'"), "a 'CString'");
}

// The underscores before a name are not read.
TEST(Article, LeadingUnderscoresAreSkipped) {
  EXPECT_EQ(printed("a '__init__'"), "an '__init__'");
  EXPECT_EQ(printed("an '__deinit__'"), "a '__deinit__'");
  EXPECT_EQ(printed("a '_Entry'"), "an '_Entry'");
  EXPECT_EQ(printed("an '_'"), "a '_'");
}

// Only an article before a quoted name is the engine's: a quotation that
// starts with no name, and every other word, is printed as written.
TEST(Article, OtherTextIsAsWritten) {
  EXPECT_EQ(printed("a '(int, Str)'"), "a '(int, Str)'");
  EXPECT_EQ(printed("an '[int]'"), "an '[int]'");
  EXPECT_EQ(printed("a '?'"), "a '?'");
  EXPECT_EQ(printed("a int is an Node"), "a int is an Node");
  EXPECT_EQ(printed("'a' 'int'"), "'a' 'int'");
  EXPECT_EQ(printed("field 'a' 'int'"), "field 'a' 'int'");
  EXPECT_EQ(printed("banana 'int'"), "banana 'int'");
  EXPECT_EQ(printed("a"), "a");
  EXPECT_EQ(printed("it is a '"), "it is a '");
}

// An article is a word of the message: at its start, or after a space or
// "(".  Right after a quote it starts what the message quotes.
TEST(Article, AnArticleIsAWordOfTheMessage) {
  EXPECT_EQ(printed("the C symbol 'a 'int'' of native function 'f'"),
            "the C symbol 'a 'int'' of native function 'f'");
  EXPECT_EQ(printed("cannot parse \"a 'int'\""), "cannot parse \"a 'int'\"");
  EXPECT_EQ(printed("'x' (a 'int')"), "'x' (an 'int')");
}

// What a message quotes of a program's string or of the file system is
// passed through verbatim(): it is printed as it is, wherever it stands and
// whatever it says, and the rest of the message is not.
TEST(Article, AVerbatimStretchIsPrintedAsItIs) {
  using paykan::sema::DiagEngine;
  EXPECT_EQ(printed("the C symbol '" + DiagEngine::verbatim("x a 'int'") +
                    "' is not a 'identifier'"),
            "the C symbol 'x a 'int'' is not an 'identifier'");
  EXPECT_EQ(printed("not found (tried " +
                    DiagEngine::verbatim("a 'old' tree/m.pkn, (a 'int')") +
                    "), a 'import'"),
            "not found (tried a 'old' tree/m.pkn, (a 'int')), an 'import'");
  EXPECT_EQ(printed(DiagEngine::verbatim("")), "");
  EXPECT_EQ(printed(DiagEngine::verbatim("a 'int'") + " a 'int'"),
            "a 'int' an 'int'");
  // The marks are the engine's: a text that holds one loses it, and an
  // unclosed stretch runs to the message's end.
  EXPECT_EQ(printed(DiagEngine::verbatim(std::string("a\x01"
                                                     "b\x02"
                                                     "c"))),
            "abc");
  EXPECT_EQ(printed("x \x01 a 'int'"), "x  a 'int'");
  // The same for a message printed by other means.
  EXPECT_EQ(DiagEngine::printed("a 'int' " + DiagEngine::verbatim("a 'int'")),
            "an 'int' a 'int'");
}

TEST(Article, EveryArticleOfAMessageIsChosen) {
  EXPECT_EQ(printed("A 'int' is not a 'Obj', nor a 'Node'"),
            "An 'int' is not an 'Obj', nor a 'Node'");
  EXPECT_EQ(printed("An 'Node' (a 'int')"), "A 'Node' (an 'int')");
}

// Warnings and notes go through the same rule.
TEST(Article, WarningsAndNotesToo) {
  std::ostringstream os;
  paykan::sema::DiagEngine diag(os);
  diag.warning(paykan::ast::SourceLocation(), "a 'int' is unused");
  diag.note(paykan::ast::SourceLocation(), "declared a 'Obj' here");
  EXPECT_NE(os.str().find("warning: an 'int' is unused\n"), std::string::npos)
      << os.str();
  EXPECT_NE(os.str().find("note: declared an 'Obj' here\n"), std::string::npos)
      << os.str();
}
