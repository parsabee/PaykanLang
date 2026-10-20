# Homebrew formula for PaykanLang: `brew install parsabee/paykanlang/paykanlang`.
#
# Builds the core compiler from the tag's source archive: the recursive-descent
# frontend and the c backend, which need a C++20 compiler to build and a C
# compiler at run time.  Nothing is downloaded (the LLVM backend is not
# enabled) and the test suite is not built.  `cmake --install` places paykan
# in bin/, the runtime archive in lib/, Runtime.h and plugin_api.h in
# include/paykan/, the standard library in lib/paykan/stdlib/<version>/ and an
# empty system plugin directory in lib/paykan/plugins/<version>/.
#
# The Release workflow of parsabee/PaykanLang opens a pull request that sets
# `url` and `sha256` for each release; see README.md.
class Paykanlang < Formula
  desc "Statically-typed, object-oriented language that compiles via C"
  homepage "https://github.com/parsabee/PaykanLang"
  url "https://github.com/parsabee/PaykanLang/archive/refs/tags/v0.1.1.tar.gz"
  sha256 "829682c6cbd71eaae753472b373a1e49e888c708ec39104440fdaed1f3d72028"
  head "https://github.com/parsabee/PaykanLang.git", branch: "develop"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DPAYKAN_FRONTENDS=recursive-descent",
           "-DPAYKAN_BACKENDS=c",
           "-DPAYKAN_BUILD_TESTS=OFF",
           "-DPAYKAN_RELEASE=ON",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s
    system "cmake", "--install", "build"
  end

  test do
    # The installed compiler compiles and runs a program with the system C
    # compiler against the installed runtime.  The program prints nothing, so
    # it is valid in every release: paykan exits with its result, 42.
    (testpath/"answer.pkn").write <<~PKN
      fn main() -> int {
        x: int = 20;
        y: int = 22;
        return x + y;
      }
    PKN
    assert_match "PaykanLang #{version}", shell_output("#{bin}/paykan --version")
    shell_output("#{bin}/paykan #{testpath}/answer.pkn", 42)
    system bin/"paykan", "build", "-o", testpath/"answer", testpath/"answer.pkn"
    shell_output(testpath/"answer", 42)

    assert_path_exists lib/"libpaykan_runtime.a"
    assert_path_exists include/"paykan/Runtime.h"
  end
end
