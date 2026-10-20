# Package channels

The two package channels each live in a repository of their own
([#178](https://github.com/parsabee/PaykanLang/issues/178),
[#181](https://github.com/parsabee/PaykanLang/issues/181)):

| Directory | Repository | Serves |
|---|---|---|
| [`homebrew-paykanlang/`](homebrew-paykanlang/) | `parsabee/homebrew-paykanlang` | `brew install parsabee/paykanlang/paykanlang` |
| [`paykanlang-apt/`](paykanlang-apt/) | `parsabee/paykanlang-apt` | the signed apt repository at `https://parsabee.github.io/paykanlang-apt` |

These directories are the initial contents of those repositories. Once both are created and
pushed (below), delete this directory from the compiler repository; the repositories are the
source of truth from then on.

The compiler's [Release workflow](../.github/workflows/release.yml) feeds both on every
non-pre-release tag: it opens a pull request on the tap that bumps the formula's `url` and
`sha256` (the tap's CI builds the bottles), and it sends a `paykanlang-release` dispatch to the
apt repository, which re-indexes the releases' `.deb` files, signs the index and deploys.

## One-time setup

1. Create `parsabee/homebrew-paykanlang` and `parsabee/paykanlang-apt` (public, empty) and push
   each directory's contents as its first commit:

   ```sh
   cp -r packaging/homebrew-paykanlang /tmp/ && cd /tmp/homebrew-paykanlang
   git init -b main && git add -A && git commit -m "Initial tap"
   git remote add origin git@github.com:parsabee/homebrew-paykanlang.git && git push -u origin main
   ```

   and the same for `paykanlang-apt`.
2. In the compiler repository, add the secret `RELEASE_CHANNELS_TOKEN`: a fine-grained personal
   access token with **Contents: read and write** and **Pull requests: read and write** on both
   new repositories. (`GITHUB_TOKEN` cannot write to another repository.) Without it the
   Release workflow publishes the GitHub Release and skips both channels with a warning.
3. In `paykanlang-apt`: add the secret `APT_SIGNING_KEY` (see its
   [README](paykanlang-apt/README.md#the-signing-key)), and set Settings → Pages → Source to
   "GitHub Actions". Remove `APT_SIGNING_KEY` from the compiler repository.
4. In `homebrew-paykanlang`: Settings → Actions → General → Workflow permissions → "Read and
   write permissions", so the bottle workflow can push and publish to GitHub Packages.
