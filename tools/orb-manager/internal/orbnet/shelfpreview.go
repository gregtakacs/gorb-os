package orbnet

import (
	"encoding/binary"
	"fmt"
)

// The hub-side preview plane. docs/theme-preview-plan.md §4 and §6, and
// docs/bugfix-batch-2026-10-29.md #6.
//
// A theme's preview is a baked region inside its own .orbtheme container — one opaque
// RGB565 plate at class ORB_ASSET_PREVIEW (9), slot 0, frame 0 — not a side file. On
// the orb it is read by orb_theme_preview_read (main/art/orb_theme.c:1094) driving
// read_one_plane; the Themes face reaches that through the device's GET
// /api/v1/sd/preview. A theme that lives only on the hub's shelf has no card row to
// name and no orb to answer, so the hub reads the same plane out of the bytes it
// already parsed to list the row. This file is that read.
//
// The walk mirrors the firmware exactly: the geometry region's index (themecontainer.go
// already walks it for the slugs) names each section's blob and its art bank inside the
// file; the blob's element table is unpacked at the row width the blob's own length
// carries (orb_bundle.c's blob_check), and the class-9 slot-0 row's frame0 indexes the
// sprite catalogue (orb_bundle_spr_at), which is the only authority on a plane's
// off/size/extent. frame0 is never turned into off + N*stride: orb_elems.h states the
// reason, and the plate's first row already breaks it.

// classPreview is ORB_ASSET_PREVIEW in main/assets/orb_assets.h — class 9, one static
// plate. slot 0 is the only slot the class carries.
const (
	classPreview = 9
	blobHead     = 120 // (BLOB_COUNT_WORDS 4 + ORB_BUNDLE_SCALARS_N 26) * 4
	scalarsN     = 26  // ORB_BUNDLE_SCALARS_N: the clock dialect, carried opaque
	elemWordsMin = 17  // ORB_BUNDLE_ELEM_WORDS_MIN
	elemWordsDef = 25  // ORB_BUNDLE_ELEM_WORDS: the width a row-less table claims
)

// ThemePreviewPlane reads a container's class-9 slot-0 plane and returns it framed in
// the device's wire shape: [u16 w LE][u16 h LE][RGB565 LE pixels]. It answers (nil, nil)
// for a container with no preview row — the design's quiet answer, never an error — and
// a non-nil error only for a container this reader cannot walk. Its only caller passes a
// body ContainerName already accepted, so magic, format, ABI, header checksum, the two
// budgets and the one-section rule are already proved; this reader starts at the art
// bank and trusts that gate rather than re-running it.
func ThemePreviewPlane(body []byte) ([]byte, error) {
	if len(body) < themeHdrBytes {
		return nil, fmt.Errorf("orbnet: that file is %d B, shorter than the %d B header a .orbtheme starts with", len(body), themeHdrBytes)
	}
	geomOff := binary.LittleEndian.Uint32(body[wGeomOff*4:])
	geomSize := binary.LittleEndian.Uint32(body[wGeomSize*4:])
	if geomOff != themeHdrBytes || int(geomOff)+int(geomSize) > len(body) {
		return nil, fmt.Errorf("orbnet: container parts are not where this format puts them")
	}
	region := body[geomOff : geomOff+geomSize]
	n := binary.LittleEndian.Uint32(body[wNSections*4:])
	if n < 1 || int(n) > bundleSectMax || len(region) < bundleIdxHead+int(n)*bundleIdxWords*4 {
		return nil, fmt.Errorf("orbnet: index claims %d sections this reader cannot walk", n)
	}
	// FORMAT 3 carries one section and the baker refuses a second class:slot pair, so
	// the first class-9 row is the only one; walking in index order is what keeps a
	// future two-section bake from being skipped silently.
	for i := uint32(0); i < n; i++ {
		rec := region[bundleIdxHead+i*uint32(bundleIdxWords)*4:]
		recOff := binary.LittleEndian.Uint32(rec[12:])
		recSize := binary.LittleEndian.Uint32(rec[16:])
		if int(recOff)+int(recSize) > len(region) {
			return nil, fmt.Errorf("orbnet: section %d's blob range is outside the region", i)
		}
		blob := region[recOff : recOff+recSize]
		// The record's art_off is an ABSOLUTE offset inside the file, not a base to
		// add to the header's: orb_bundle.c reads file + s->art_off against file_size
		// directly (line 365), and the shipped bake's plane lands on EOF by that rule.
		artBase := binary.LittleEndian.Uint32(rec[24:])
		if plane := planeInBlob(body, blob, artBase); plane != nil {
			return plane, nil
		}
	}
	return nil, nil
}

// planeInBlob unpacks one section's element table and sprite catalogue and returns the
// class-9 slot-0 plane framed for the wire, or nil when the section carries no preview
// row. The element row's width and the catalogue's row width are not assumed: they are
// derived from the blob's length, the way blob_check does, because a table baked before
// a column was appended still addresses whole at the narrower width, and a delta-dialect
// catalogue costs 24 B/row instead of 16. Probing both catalogue widths and taking the
// one whose length closes over its counts reads a v3 and a v4 file with one code path
// and no ABI guessing. body is the whole container (the plane's pixels live in the art
// bank, outside the geometry region), blob is the section's region, and artBase is that
// bank's absolute offset inside body.
func planeInBlob(body, blob []byte, artBase uint32) []byte {
	if len(blob) < blobHead {
		return nil
	}
	ne := int(binary.LittleEndian.Uint32(blob[0:]))
	ns := int(binary.LittleEndian.Uint32(blob[4:]))
	if int(binary.LittleEndian.Uint32(blob[8:])) != scalarsN || ne < 0 || ne > 0xFFFF || ns < 0 || ns > 0xFFFF {
		return nil
	}
	nl := int(binary.LittleEndian.Uint32(blob[12:]))
	pad := nl + (4-(nl%4))%4
	for _, sprBytes := range []int{16, 24} {
		fixed := blobHead + ns*sprBytes + pad
		if len(blob) < fixed {
			continue
		}
		eb := len(blob) - fixed
		stride := elemWordsDef
		if ne > 0 {
			if eb%(ne*4) != 0 {
				continue
			}
			stride = eb / (ne * 4)
			if stride < elemWordsMin {
				continue
			}
		} else if eb != 0 {
			continue
		}
		catOff := blobHead + ne*stride*4
		for j := 0; j < ne; j++ {
			r := blob[blobHead+j*stride*4:]
			if len(r) < 28 || r[4] != classPreview {
				continue
			}
			slot := int(binary.LittleEndian.Uint16(r[20:]))
			frame0 := int(binary.LittleEndian.Uint16(r[24:]))
			if slot != 0 || frame0 >= ns {
				continue
			}
			so := catOff + frame0*sprBytes
			if so+16 > len(blob) {
				return nil
			}
			off := binary.LittleEndian.Uint32(blob[so:])
			size := binary.LittleEndian.Uint32(blob[so+4:])
			w := binary.LittleEndian.Uint32(blob[so+8:])
			h := binary.LittleEndian.Uint32(blob[so+12:])
			// An opaque RGB565 plate: orb_asset_slot_bytes gives class 9 w*h*2, and the
			// baker writes one square plane (the shell reads a preview into a half-res
			// buffer, so a non-square bake is refused at the door). A masked row carries
			// a third stride the class never bakes; read_one_plane refuses it with
			// ORB_ASSET_CLASS, and so does this reader — nil, the quiet answer.
			if w == 0 || w != h || size != w*h*2 {
				continue
			}
			start := int(artBase) + int(off)
			if start < 0 || start+int(size) > len(body) {
				return nil
			}
			out := make([]byte, 4+int(size))
			binary.LittleEndian.PutUint16(out[0:], uint16(w))
			binary.LittleEndian.PutUint16(out[2:], uint16(h))
			copy(out[4:], body[start:start+int(size)])
			return out
		}
	}
	return nil
}
