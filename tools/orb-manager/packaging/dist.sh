#!/bin/sh
# dist.sh — turn `make release` output into what a person downloads.
#
#   dist.sh stage <linux|windows|macos> <release-dir> <version> <out-dir>
#   dist.sh pack  <linux|windows|macos> <version> <out-dir>
#
# stage lays one platform out under <out-dir>/stage/<name>/ (binaries, library, README,
# licences; for macOS the "Orb Manager.app" bundle); pack archives that folder into
# <out-dir>/<name>.{tar.gz,zip,dmg}. They are two steps because signing happens between
# them: CI signs the staged Windows exes and the staged Mac app, then packs.
#
# <version> is the release tag (v1.2.6). The macOS bundle needs lipo to join the two
# darwin binaries into one universal one, so it stages only on a Mac; pack macos needs
# hdiutil, likewise. Linux and Windows stage and pack anywhere.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
pkg=$(dirname "$here")
repo=$(cd "$pkg/../.." && pwd)

die() { echo "dist.sh: $*" >&2; exit 1; }
crlf() { sed 's/$/\r/' "$1" > "$2"; }

[ $# -ge 1 ] || die "usage: dist.sh stage|pack ..."
step=$1; shift

name() { # name <platform> <version>
	case $1 in
	linux) echo "orb-manager-$2-linux-amd64" ;;
	windows) echo "orb-manager-$2-windows-amd64" ;;
	macos) echo "orb-manager-$2-macos" ;;
	*) die "unknown platform $1" ;;
	esac
}

case $step in
stage)
	[ $# -eq 4 ] || die "usage: dist.sh stage <platform> <release-dir> <version> <out-dir>"
	plat=$1 rel=$2 ver=$3 out=$4
	n=$(name "$plat" "$ver")
	d="$out/stage/$n"
	rm -rf "$d"
	mkdir -p "$d"
	case $plat in
	linux)
		src="$rel/orb-manager-linux-amd64"
		[ -x "$src/orb-manager" ] || die "no $src/orb-manager — run make release first"
		cp -R "$src/." "$d/"
		cp "$here/linux/orb-manager.desktop" "$here/linux/60-orb.rules" \
			"$here/linux/install.sh" "$here/linux/orb-manager.png" "$d/"
		sed "s/@VERSION@/$ver/" "$here/linux/README.txt" > "$d/README.txt"
		cp "$repo/LICENSE" "$d/LICENSE.txt"
		cp "$here/THIRD_PARTY_NOTICES.txt" "$d/"
		;;
	windows)
		src="$rel/orb-manager-windows-amd64"
		[ -f "$src/orb-manager.exe" ] || die "no $src/orb-manager.exe — run make release first"
		cp -R "$src/." "$d/"
		sed "s/@VERSION@/$ver/" "$here/windows/README.txt" > "$d/README.tmp"
		crlf "$d/README.tmp" "$d/README.txt"
		rm "$d/README.tmp"
		crlf "$repo/LICENSE" "$d/LICENSE.txt"
		crlf "$here/THIRD_PARTY_NOTICES.txt" "$d/THIRD_PARTY_NOTICES.txt"
		;;
	macos)
		command -v lipo >/dev/null || die "the macOS app needs lipo: stage it on a Mac (CI does)"
		a="$rel/orb-manager-darwin-arm64" i="$rel/orb-manager-darwin-amd64"
		[ -f "$a/orb-manager" ] && [ -f "$i/orb-manager" ] || die "no darwin binaries in $rel — run make release first"
		app="$d/Orb Manager.app"
		mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
		lipo -create -output "$app/Contents/MacOS/orb-manager" "$a/orb-manager" "$i/orb-manager"
		chmod 755 "$app/Contents/MacOS/orb-manager"
		cp "$here/macos/orb-manager.icns" "$app/Contents/Resources/"
		# CFBundle*Version must be plain x.y.z: v1.2.6-2 and a describe string both trim to it.
		num=$(echo "${ver#v}" | sed 's/[^0-9.].*//')
		sed "s/@VERSION@/${num:-0.0.0}/g" "$here/macos/Info.plist" > "$app/Contents/Info.plist"
		printf 'APPL????' > "$app/Contents/PkgInfo"
		sed "s/@VERSION@/$ver/" "$here/macos/README.txt" > "$d/README.txt"
		cp "$repo/LICENSE" "$d/LICENSE.txt"
		cp "$here/THIRD_PARTY_NOTICES.txt" "$d/"
		ln -s /Applications "$d/Applications"
		;;
	*) die "unknown platform $plat" ;;
	esac
	echo "staged $d"
	;;
pack)
	[ $# -eq 3 ] || die "usage: dist.sh pack <platform> <version> <out-dir>"
	plat=$1 ver=$2 out=$3
	n=$(name "$plat" "$ver")
	[ -d "$out/stage/$n" ] || die "nothing staged at $out/stage/$n"
	case $plat in
	linux)
		tar -C "$out/stage" -czf "$out/$n.tar.gz" "$n"
		echo "packed $out/$n.tar.gz"
		;;
	windows)
		rm -f "$out/$n.zip"
		if command -v zip >/dev/null; then
			(cd "$out/stage" && zip -qr9 "../$n.zip" "$n")
		else
			(cd "$out/stage" && python3 -m zipfile -c "../$n.zip" "$n")
		fi
		echo "packed $out/$n.zip"
		;;
	macos)
		command -v hdiutil >/dev/null || die "the macOS disk image needs hdiutil: pack it on a Mac (CI does)"
		rm -f "$out/$n.dmg"
		hdiutil create -volname "Orb Manager" -srcfolder "$out/stage/$n" -fs HFS+ -format UDZO -ov "$out/$n.dmg"
		echo "packed $out/$n.dmg"
		;;
	*) die "unknown platform $plat" ;;
	esac
	;;
*) die "unknown step $step (stage or pack)" ;;
esac
