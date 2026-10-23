"""Structural check of the generated PDF report: page tree, fonts, text, embedded font program."""
import struct
import sys
import zlib

import pypdf

path = sys.argv[1] if len(sys.argv) > 1 else "i18n/report_test.pdf"
r = pypdf.PdfReader(path)
print("pages:", len(r.pages))
page = r.pages[0]
text = page.extract_text()
print("---- extracted text ----")
print(text[:600])
print("---- fonts ----")
fonts = page["/Resources"]["/Font"]
for name, ref in fonts.items():
    f = ref.get_object()
    print(name, f.get("/Subtype"), f.get("/BaseFont"), "ToUnicode" in f)
    if f.get("/Subtype") == "/Type0":
        cid = f["/DescendantFonts"][0].get_object()
        print("   descendant:", cid.get("/Subtype"), cid.get("/BaseFont"), "W entries:", len(str(cid.get("/W"))) // 8)
        fd = cid["/FontDescriptor"].get_object()
        print("   descriptor bbox:", fd.get("/FontBBox"), "ascent", fd.get("/Ascent"), "descent", fd.get("/Descent"))
        ff = fd["/FontFile2"].get_object()
        raw = ff.get_data()
        print("   font program bytes:", len(raw), "Length1:", ff.get("/Length1"))
        # parse it back
        assert raw[:4] == b"\x00\x01\x00\x00", raw[:4]
        n = struct.unpack(">H", raw[4:6])[0]
        tabs = {}
        for i in range(n):
            tag, cs, off, ln = struct.unpack(">4sIII", raw[12 + 16 * i:28 + 16 * i])
            tabs[tag.decode("latin1")] = (off, ln, cs)
        print("   tables:", sorted(tabs))
        head = raw[tabs["head"][0]: tabs["head"][0] + tabs["head"][1]]
        upem = struct.unpack(">H", head[18:20])[0]
        locaFmt = struct.unpack(">h", head[50:52])[0]
        maxp = raw[tabs["maxp"][0]: tabs["maxp"][0] + tabs["maxp"][1]]
        ng = struct.unpack(">H", maxp[4:6])[0]
        hhea = raw[tabs["hhea"][0]: tabs["hhea"][0] + tabs["hhea"][1]]
        nhm = struct.unpack(">H", hhea[34:36])[0]
        print(f"   head upem={upem} locaFmt={locaFmt} numGlyphs={ng} numHMetrics={nhm}")
        loca = struct.unpack(">%dI" % (ng + 1), raw[tabs["loca"][0]: tabs["loca"][0] + 4 * (ng + 1)])
        print("   loca monotonic:", all(loca[i] <= loca[i + 1] for i in range(ng)), "glyf len:", tabs["glyf"][1], "last loca:", loca[-1])
        # first few glyph headers: number of contours
        contours = []
        for g in range(min(ng, 600)):
            if loca[g + 1] > loca[g]:
                contours.append(struct.unpack(">h", raw[tabs["glyf"][0] + loca[g]: tabs["glyf"][0] + loca[g] + 2])[0])
        print("   glyph contour counts (first 12):", contours[:12])
        print("   glyphs with outlines:", sum(1 for c in contours if c != 0), "/", len(contours))
print("metadata title:", r.metadata.get("/Title"))
