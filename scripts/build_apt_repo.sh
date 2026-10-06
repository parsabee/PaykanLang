#!/usr/bin/env bash
# Builds the PaykanLang apt repository from a directory of .deb packages.
#
#   scripts/build_apt_repo.sh <debs-dir> <out-dir>
#
# The repository has one suite, "stable", with one component, "main":
#
#   <out-dir>/pool/main/paykanlang_<version>_<arch>.deb
#   <out-dir>/dists/stable/main/binary-<arch>/Packages{,.gz}
#   <out-dir>/dists/stable/{Release,Release.gpg,InRelease}
#   <out-dir>/paykanlang.gpg            the signing key (public, binary keyring)
#
# The Release file is signed with the secret key that the GPG home directory
# holds (GNUPGHOME, or ~/.gnupg); APT_SIGNING_KEY_ID picks one key when it
# holds several.  The Pages workflow (.github/workflows/pages.yml) runs this
# on every release's .deb and publishes the result under /apt on the site.
#
# Needs dpkg-scanpackages (dpkg-dev), gzip, sha256sum, md5sum and gpg: CI-only
# tools, none of which a PaykanLang build or install needs.
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <debs-dir> <out-dir>" >&2
  exit 2
fi
debs_dir=$1
out_dir=$2
suite=stable
component=main

shopt -s nullglob
debs=("${debs_dir}"/*.deb)
if [ "${#debs[@]}" -eq 0 ]; then
  echo "error: no .deb in ${debs_dir}" >&2
  exit 1
fi

mkdir -p "${out_dir}/pool/${component}"
cp "${debs[@]}" "${out_dir}/pool/${component}/"

cd "${out_dir}"
archs=$(for deb in pool/"${component}"/*.deb; do dpkg-deb --field "${deb}" Architecture; done | sort -u)
for arch in ${archs}; do
  dir="dists/${suite}/${component}/binary-${arch}"
  mkdir -p "${dir}"
  dpkg-scanpackages --multiversion --arch "${arch}" "pool/${component}" /dev/null > "${dir}/Packages"
  gzip -9nkf "${dir}/Packages"
done

# The Release file: what the suite holds and the checksums of its indices.
cd "dists/${suite}"
{
  echo "Origin: PaykanLang"
  echo "Label: PaykanLang"
  echo "Suite: ${suite}"
  echo "Codename: ${suite}"
  echo "Date: $(LC_ALL=C date -Ru)"
  echo "Architectures: $(echo "${archs}" | tr '\n' ' ' | sed 's/ $//')"
  echo "Components: ${component}"
  echo "Description: PaykanLang compiler packages"
  for sum in MD5Sum:md5sum SHA256:sha256sum; do
    echo "${sum%%:*}:"
    find "${component}" -type f | LC_ALL=C sort | while read -r file; do
      printf ' %s %16d %s\n' "$("${sum#*:}" "${file}" | cut -d' ' -f1)" "$(wc -c < "${file}")" "${file}"
    done
  done
} > Release

key=()
if [ -n "${APT_SIGNING_KEY_ID:-}" ]; then
  key=(--local-user "${APT_SIGNING_KEY_ID}")
fi
gpg --batch --yes "${key[@]}" --armor --detach-sign --output Release.gpg Release
gpg --batch --yes "${key[@]}" --clearsign --output InRelease Release
cd ../..

# The public half of the signing key, for /usr/share/keyrings.
gpg --batch --yes --export ${APT_SIGNING_KEY_ID:+"${APT_SIGNING_KEY_ID}"} > paykanlang.gpg
if [ ! -s paykanlang.gpg ]; then
  echo "error: no public key to export" >&2
  exit 1
fi
echo "apt repository: ${out_dir} (${archs//$'\n'/ })"
