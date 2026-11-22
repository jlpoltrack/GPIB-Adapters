#!/bin/zsh
# Clean-build USBGPIB-STM32 (app, dapboot) and LANGPIB-WCH (app .dfu, bootloader); print "sha256  path", sorted.
# Usage: tools/build_hashes.sh [source_root] [out_dir]   (defaults: this repo, a temp dir). dapboot builds in-tree.
set -eu
repo=${0:A:h:h}
src=${1:-$repo}; src=${src:A}
out=${2:-$(mktemp -d)}; mkdir -p "$out"; out=${out:A}
# git archive leaves submodules out: copy any missing one from this repo's checkout (same pinned commits)
for p in $(git -C "$repo" config -f .gitmodules --get-regexp '\.path$' | cut -d' ' -f2); do
  if [[ ! -e $src/$p/.git && -z "$(ls -A "$src/$p" 2>/dev/null)" ]]; then rm -rf "$src/$p"; mkdir -p "$src/${p:h}"; cp -R "$repo/$p" "$src/$p"; fi
done
stm=$src/USBGPIB-STM32/Firmware wch=$src/LANGPIB-WCH/Firmware
rm -rf "$out/usbgpib" "$out/langpib"
{ make -C "$src/USBGPIB-STM32/bootloader/dapboot/src" clean &&
  make -C "$stm" -j8 BUILD="$out/usbgpib" &&
  make -C "$wch" -j8 BUILD="$out/langpib" BOOT_BIN="$out/langpib/boot.bin"; } > "$out/build.log" 2>&1 ||
  { echo "BUILD FAILED (see $out/build.log)" >&2; exit 1; }
cp "$src/USBGPIB-STM32/bootloader/dapboot/src/dapboot.bin" "$out/usbgpib/"
(cd "$out" && shasum -a 256 usbgpib/usbgpib.bin usbgpib/dapboot.bin langpib/langpib.dfu langpib/boot.bin | sort -k2)
if [[ -z ${2:-} ]]; then rm -rf "$out"; fi
