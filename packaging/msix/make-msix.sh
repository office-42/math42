#!/bin/sh
# math42 - build the Microsoft Store package.
# Copyright (C) 2026 The math42 authors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Puts math42.exe, the GTK runtime it needs and the Store's tiles into
# one directory laid out like a MinGW prefix, and asks the Windows SDK
# to pack it as an .msix.
#
#   packaging/msix/make-msix.sh              -> builddir/math42-<ver>-x64.msix
#   packaging/msix/make-msix.sh --test-sign  -> and a signed copy for
#                                               installing it here
#
# The package the Store wants is the unsigned one: it is signed with
# the publisher's own certificate on the way in.  The signed copy is
# only so that the package can be installed and looked at first.

set -e

# The SDK tools take /flags, which a POSIX shell on Windows would
# otherwise helpfully turn into paths, and Windows paths, which it
# would otherwise leave alone.
noconv () { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$@"; }
win () { cygpath -w "$1"; }

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
build=${BUILDDIR:-$top/builddir}
stage=$build/msix/math42
sign=no

for arg in "$@"; do
  case $arg in
    --test-sign) sign=yes ;;
    *) echo "usage: $0 [--test-sign]" >&2; exit 2 ;;
  esac
done

# The MinGW prefix everything is copied out of.
prefix=
for p in "$MINGW_PREFIX" /mingw64 /c/msys64/mingw64; do
  if [ -n "$p" ] && [ -f "$p/bin/libgtk-4-1.dll" ]; then prefix=$p; break; fi
done
[ -n "$prefix" ] || { echo "no MinGW64 prefix with GTK 4 in it" >&2; exit 1; }
PATH="$prefix/bin:$PATH"
export PATH

# And the Windows SDK the packaging tools come from.
sdk=$(ls -d "/c/Program Files (x86)/Windows Kits/10/bin"/10.*/x64 2>/dev/null |
        sort -V | tail -1)
[ -n "$sdk" ] || { echo "no Windows 10 SDK: install the Windows SDK" >&2; exit 1; }

version=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$top/meson.build")
[ -n "$version" ] || { echo "no version in meson.build" >&2; exit 1; }
# A Store version is four numbers and the last of them must be zero, so
# a version still being worked on -- 1.0.1-dev -- goes in as the release
# it is on its way to.  The file keeps the whole name, to tell the two
# apart on disk; a package built from such a tree is for looking at, not
# for sending.
appxver=$(echo "$version" | sed 's/-.*$//').0
msix=$build/math42-$version-x64.msix

echo "math42 $version as $appxver, from $prefix," \
     "packed by $(basename "$(dirname "$sdk")")"

# ---------------------------------------------------------------- build

[ -d "$build" ] || meson setup "$build" >/dev/null
meson compile -C "$build" >/dev/null

# ---------------------------------------------------------------- stage

rm -rf "$build/msix"
mkdir -p "$stage/bin" "$stage/Assets" \
         "$stage/share/glib-2.0/schemas" "$stage/share/icons/hicolor"

cp "$build/src/math42.exe" "$stage/bin/"

# Every DLL the program asks for, and every DLL those ask for, as far
# as they live in the prefix: what is left is Windows' own.
python - "$prefix/bin" "$stage/bin" <<'EOF'
import os, re, shutil, subprocess, sys

bindir, dest = sys.argv[1], sys.argv[2]

def imports(path):
    out = subprocess.run(['objdump', '-p', path],
                         capture_output=True, text=True).stdout
    return re.findall(r'DLL Name:\s*(\S+)', out)

seen, queue, bytes_ = set(), [os.path.join(dest, 'math42.exe')], 0
while queue:
    for name in imports(queue.pop()):
        key = name.lower()
        if key in seen:
            continue
        src = os.path.join(bindir, name)
        if not os.path.exists(src):
            continue            # a DLL of Windows' own
        seen.add(key)
        shutil.copy2(src, dest)
        queue.append(src)
        bytes_ += os.path.getsize(src)

print('  %d DLLs, %.0f MB' % (len(seen), bytes_ / 1e6))
EOF

# GTK reads its settings through GSettings, and GSettings ends the
# process when a schema it asks for is not there.
cp "$prefix/share/glib-2.0/schemas/gschemas.compiled" \
   "$stage/share/glib-2.0/schemas/"

# The window and the about dialog ask for the icon by name, so it has
# to be in an icon theme.  As PNG: rendering the SVG at runtime would
# mean carrying librsvg, and GTK 4 reads PNG by itself.
svg=$top/data/icons/scalable/apps/net.office42.math42.svg
cp "$prefix/share/icons/hicolor/index.theme" "$stage/share/icons/hicolor/"
for n in 16 24 32 48 64 128 256; do
  mkdir -p "$stage/share/icons/hicolor/${n}x${n}/apps"
  rsvg-convert -w "$n" -h "$n" -b none "$svg" \
    -o "$stage/share/icons/hicolor/${n}x${n}/apps/net.office42.math42.png"
done

# --------------------------------------------------------------- tiles

# The Store's tiles and icons, at every scale Windows asks for.  The
# small icons are the whole picture; the tiles sit on the background
# colour named in the manifest, with the picture inset in the middle.
base=$build/msix/icon-2048.png
rsvg-convert -w 2048 -h 2048 -b none "$svg" -o "$base"

python - "$base" "$stage/Assets" <<'EOF' > "$build/msix/tiles.sh"
import sys
base, assets = sys.argv[1], sys.argv[2]

tiles = [                       # name, width, height, how much is picture
    ('StoreLogo',          50,  50, 1.00),
    ('Square44x44Logo',    44,  44, 1.00),
    ('Square71x71Logo',    71,  71, 0.66),
    ('Square150x150Logo', 150, 150, 0.66),
    ('Square310x310Logo', 310, 310, 0.66),
    ('Wide310x150Logo',   310, 150, 0.66),
]
scales = [100, 125, 150, 200, 400]
# What the taskbar, the task switcher and the file explorer reach for.
targets = [16, 24, 32, 48, 256]

PNG = '-depth 8 -define png:color-type=6'   # 32-bit RGBA, as Windows wants

def emit(out, w, h, inset):
    side = max(1, round(min(w, h) * inset))
    if inset >= 1.0 and w == h:
        print("magick '%s' -resize %dx%d %s '%s'" % (base, w, h, PNG, out))
    else:
        # The parentheses matter: without them the resize would take the
        # canvas with it and the tile would come out the size of the
        # picture inside it.
        print("magick -size %dx%d xc:none \\( '%s' -resize %dx%d \\) "
              "-gravity center -composite %s '%s'"
              % (w, h, base, side, side, PNG, out))

for name, w, h, inset in tiles:
    for s in scales:
        suffix = '' if s == 100 else '.scale-%d' % s
        emit('%s/%s%s.png' % (assets, name, suffix),
             round(w * s / 100), round(h * s / 100), inset)

for t in targets:
    for form in ('', '_altform-unplated'):
        emit('%s/Square44x44Logo.targetsize-%d%s.png' % (assets, t, form),
             t, t, 1.00)
EOF
sh "$build/msix/tiles.sh"
rm -f "$base" "$build/msix/tiles.sh"
echo "  $(ls "$stage/Assets" | wc -l) tiles"

sed "s|@VERSION@|$appxver|" "$here/AppxManifest.xml.in" > "$stage/AppxManifest.xml"

# ---------------------------------------------------------------- pack

noconv "$sdk/makepri.exe" createconfig \
    /cf "$(win "$build/msix/priconfig.xml")" /dq en-US /o >/dev/null
# One package, so the tiles at every scale belong in the one index; the
# default configuration would put them in resource packages of their own
# that a single .msix has nowhere to keep.
sed -i '/<packaging>/,/<\/packaging>/d' "$build/msix/priconfig.xml"
noconv "$sdk/makepri.exe" new /pr "$(win "$stage")" \
    /cf "$(win "$build/msix/priconfig.xml")" \
    /mn "$(win "$stage/AppxManifest.xml")" \
    /of "$(win "$stage/resources.pri")" /o >/dev/null

rm -f "$msix"
noconv "$sdk/makeappx.exe" pack /d "$(win "$stage")" /p "$(win "$msix")" /o \
    >/dev/null
echo "$msix"

# ---------------------------------------------------------------- sign

# A package installs here only if it is signed by a certificate this
# machine trusts.  The Store signs the real one; this is a throwaway
# with the same subject name, for looking at the package before it is
# sent.  Installing the result needs its certificate in the machine's
# trusted people store, which the README says how to do.
if [ "$sign" = yes ]; then
  pfx=$build/msix/math42-test.pfx
  signed=${msix%.msix}-test-signed.msix
  powershell -NoProfile -Command "
    \$pw = ConvertTo-SecureString -String 'math42' -Force -AsPlainText
    \$c = New-SelfSignedCertificate -Type Custom -KeyUsage DigitalSignature \
           -Subject 'CN=631F98F7-2280-49EE-8EF8-534CC36D09CF' \
           -CertStoreLocation 'Cert:\CurrentUser\My' \
           -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3',
                            '2.5.29.19={text}')
    Export-PfxCertificate -Cert \$c -FilePath '$(win "$pfx")' -Password \$pw |
      Out-Null
    Export-Certificate -Cert \$c -FilePath '$(win "${pfx%.pfx}.cer")' | Out-Null
  " >/dev/null
  cp "$msix" "$signed"
  noconv "$sdk/signtool.exe" sign /fd SHA256 /a /f "$(win "$pfx")" /p math42 \
      "$(win "$signed")" >/dev/null
  echo "$signed"
  echo "${pfx%.pfx}.cer"
fi
