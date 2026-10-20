# PaykanLang apt repository

The signed apt repository of the [PaykanLang](https://github.com/parsabee/PaykanLang) compiler,
served from this repository's GitHub Pages site, `https://parsabee.github.io/paykanlang-apt`.

```sh
sudo curl -fsSLo /usr/share/keyrings/paykanlang.gpg https://parsabee.github.io/paykanlang-apt/paykanlang.gpg
echo "deb [signed-by=/usr/share/keyrings/paykanlang.gpg] https://parsabee.github.io/paykanlang-apt stable main" |
  sudo tee /etc/apt/sources.list.d/paykanlang.list
sudo apt update
sudo apt install paykanlang
```

`sudo apt upgrade` then picks up each new release.

## How it is built

The repository has one suite, `stable`, with one component, `main`. It holds the `.deb` of the
latest `KEEP_RELEASES` (5) non-pre-release PaykanLang releases; older packages stay on their
release pages. Nothing is committed here: [`publish.yml`](.github/workflows/publish.yml)
downloads the packages from the releases, indexes them with
[`scripts/build_apt_repo.sh`](scripts/build_apt_repo.sh), signs the `Release` file
(`InRelease` and `Release.gpg`) and deploys the result. It runs on the `paykanlang-release`
dispatch that the compiler's Release workflow sends after it publishes, and by hand from the
Actions tab.

## The signing key

The `Release` file is signed with an OpenPGP key kept only in this repository, as the secret
`APT_SIGNING_KEY`: the armored secret key, without a passphrase. The workflow publishes its
public half as `paykanlang.gpg` at the site's root, the keyring users name in `signed-by=`.

To create one (a signing-only key that expires in two years):

```sh
export GNUPGHOME="$(mktemp -d)"
gpg --batch --passphrase '' --quick-gen-key "PaykanLang apt repository <parsabagheri2013@gmail.com>" ed25519 sign 2y
gpg --armor --export-secret-keys > apt-signing-key.asc   # the APT_SIGNING_KEY secret
```

**Expiry.** apt refuses an index signed by an expired key, and it checks the expiry in the
user's copy of the keyring. Well before the key expires, extend it, update the secret and
re-run the workflow, which publishes the extended public key:

```sh
gpg --import apt-signing-key.asc
gpg --quick-set-expire <fingerprint> 2y
gpg --armor --export-secret-keys <fingerprint> > apt-signing-key.asc
```

The fingerprint does not change; users refresh their keyring with the `sudo curl` line above.

**Rotation.** To replace the key, publish with the new key and announce the new keyring:
users re-download `paykanlang.gpg` (the `sudo curl` line), since apt rejects an index signed
by a key that is not in their keyring. Delete the old secret once the new one is in place.
