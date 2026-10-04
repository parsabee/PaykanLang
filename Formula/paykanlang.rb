# Homebrew formula for PaykanLang.
#
# Not yet updated for v0.1.0: it still points at the v0.0.0 tag with a placeholder
# sha256 (below). Bringing it up to date is a release step of
# https://github.com/parsabee/PaykanLang/issues/27 (section 5).
#
# This file doubles as a tap: a user can install with
#
#   brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
#   brew install parsabee/paykanlang/paykanlang
#
# It builds from the source tarball that the release workflow attaches to the
# v0.0.0 GitHub Release (or the auto-generated source archive for the tag) and
# invokes the project's `cmake --install` rules: the `paykan` binary -> bin/,
# the runtime archive -> lib/, and Runtime.h -> include/paykan/.
#
# It builds the core, like the release tarballs (#27, #123): the
# recursive-descent frontend and the c backend, which need only a C++20
# compiler to build and a C compiler at run time. Nothing is downloaded: the
# opt-in LLVM backend and Bison frontend are not enabled, and the test suite
# (and so GoogleTest) is not built (-DPAYKAN_BUILD_TESTS=OFF).
#
# NOTE: `sha256` below is a placeholder. After the v0.0.0 tag exists, set it to
# the real digest, e.g.:
#   curl -fsSL https://github.com/parsabee/PaykanLang/archive/refs/tags/v0.0.0.tar.gz | shasum -a 256
class Paykanlang < Formula
  desc "Statically-typed, object-oriented language that compiles via C"
  homepage "https://github.com/parsabee/PaykanLang"
  url "https://github.com/parsabee/PaykanLang/archive/refs/tags/v0.0.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DPAYKAN_FRONTENDS=recursive-descent",
           "-DPAYKAN_BACKENDS=c",
           "-DPAYKAN_BUILD_TESTS=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  test do
    # A real end-to-end exercise: the installed compiler must run a program
    # that uses variables, arithmetic, and println, and exit with the value the
    # program returns.
    (testpath/"hello.pkn").write <<~PKN
      fn main() -> int {
        x: int = 20;
        y: int = 22;
        println("Hello from the Homebrew test!");
        return x + y;
      }
    PKN

    # --version reports the compiler version and its frontends and backends.
    assert_match "PaykanLang #{version}", shell_output("#{bin}/paykan --version")

    # The program returns 42, so paykan should exit 42 (c backend: compiled
    # with the system C compiler against the installed runtime).
    output = shell_output("#{bin}/paykan #{testpath}/hello.pkn", 42)
    assert_match "Hello from the Homebrew test!", output

    # Installed layout sanity: runtime archive and public header are staged.
    assert_path_exists lib/"libpaykan_runtime.a"
    assert_path_exists include/"paykan/Runtime.h"
  end
end
