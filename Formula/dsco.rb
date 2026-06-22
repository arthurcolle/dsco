class Dsco < Formula
  desc "Local-first self-introspecting agentic runtime written in pure C"
  homepage "https://github.com/arthurcolle/dsco"
  url "https://github.com/arthurcolle/dsco/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "1f1f67254a1a709a41cfe22f5ba1518fca7c28d37dfebd6d8e44e1c7aad1b3df"
  license "MIT"
  head "https://github.com/arthurcolle/dsco.git", branch: "perf/dramatic-20260621"

  depends_on "pkg-config" => :build
  depends_on "hiredis"
  depends_on "libsodium"
  depends_on "libuv"
  # macOS-only: links Security, Metal, LocalAuthentication, Accelerate, etc.
  depends_on :macos
  depends_on "mbedtls@3"
  depends_on "readline"

  uses_from_macos "curl"
  uses_from_macos "sqlite"

  def install
    # The Makefile self-detects deps via pkg-config and links the vendored GSL
    # that ships in the tarball. Build the release binary, the lite worker, and
    # the dsc helper. CC must support -std=c2y (Apple clang 16+ / recent LLVM).
    system "make", "dsco", "dsco-lite", "dsc", "CC=#{ENV.cc}"

    bin.install "dsco", "dsco-lite", "dsc"

    # The binary resolves this at runtime via <exe_dir>/../share/dsco/.
    pkgshare.install "include/tool_embeddings.bin"
  end

  test do
    assert_match "dsco v#{version}", shell_output("#{bin}/dsco --version") unless build.head?
    assert_match "dsco v", shell_output("#{bin}/dsco --version")

    # The cwd tool runs entirely locally (no network, no API key) and exercises
    # the tool-dispatch path end to end. (Path realpath-resolves through
    # /private on macOS, so assert on the JSON envelope, not the exact dir.)
    assert_match(/"ok":\s*true/, shell_output("#{bin}/dsco --tool-exec cwd '{}'"))
  end
end
