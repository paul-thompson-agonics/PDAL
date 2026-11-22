#!/usr/bin/env python3
"""Turn OGRWriterTest's stored ogrinfo output into C++ expected values.

OGRWriterTest compares `ogrinfo -nomd -al` output with the .ogrinfo files
here. This reads those files and prints, for each one, a checkLayer() call
with the same facts as an Expected initializer: geometry type, feature
count, extent, SRS name, fields with their first feature values, the first
point, and the points in each multipoint feature.

Usage: ogrinfo_to_expected.py FILE.ogrinfo...
"""

import re
import sys

GEOM_TYPES = {
    "3D Point": "wkbPoint25D",
    "3D Measured Point": "wkbPointZM",
    "3D Multi Point": "wkbMultiPoint25D",
    "3D Measured Multi Point": "wkbMultiPointZM",
}

FIELD_TYPES = {
    "Integer": "OFTInteger",
    "Integer64": "OFTInteger64",
    "Real": "OFTReal",
}


def num(text):
    """Write a number as ogrinfo printed it, without trailing zeros."""
    float(text)  # check it is one
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text


def points(wkt):
    """The coordinates of each point in a POINT or MULTIPOINT WKT."""
    m = re.fullmatch(r"(MULTI)?POINT (Z|ZM) \((.*)\)", wkt)
    if not m:
        sys.exit(f"unexpected geometry: {wkt}")
    body = m.group(3)
    if m.group(1):
        body = body.removeprefix("(").removesuffix(")")
        return [p.split() for p in body.split("),(")]
    return [body.split()]


def parse(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()

    exp = {"fields": [], "features": []}
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("Geometry: "):
            exp["geom"] = GEOM_TYPES[line[len("Geometry: "):]]
        elif line.startswith("Feature Count: "):
            exp["count"] = int(line[len("Feature Count: "):])
        elif line.startswith("Extent: "):
            exp["extent"] = [num(v) for v in re.findall(r"-?\d+\.?\d*", line)]
        elif line == "Layer SRS WKT:":
            i += 1
            m = re.match(r'\w+\["([^"]*)"', lines[i])
            # "(unknown)" means the layer has no SRS
            exp["srs"] = m.group(1) if m else ""
        elif m := re.fullmatch(r"(\w+): (\w+) \(\d+\.\d+\)", line):
            # A Shapefile driver adds FID to a layer without fields
            if m.group(1) != "FID":
                exp["fields"].append([m.group(1), FIELD_TYPES[m.group(2)]])
        elif line.startswith("OGRFeature("):
            exp["features"].append({"values": {}, "points": None})
        elif exp["features"] and line.startswith("  "):
            feat = exp["features"][-1]
            m = re.fullmatch(r"  (\w+) \((\w+)\) = (.*)", line)
            if m:
                feat["values"][m.group(1)] = num(m.group(3))
            else:
                feat["points"] = points(line.strip())
        i += 1

    if len(exp["features"]) != exp["count"]:
        sys.exit(f"{path}: {exp['count']} features, but "
                 f"{len(exp['features'])} printed")
    return exp


def cpp(exp):
    first = exp["features"][0]
    out = [f"    checkLayer(out, {{ {exp['geom']}, {exp['count']},",
           "        envelope({}),".format(", ".join(exp["extent"])),
           "        {{ {} }},".format(", ".join(map(num,
                                                    first["points"][0])))]

    if exp["fields"]:
        rows = [f'{{ "{name}", {ftype}, {first["values"][name]} }}'
                for name, ftype in exp["fields"]]
        out.append("        { " + rows[0] + ("," if len(rows) > 1 else ""))
        for n, row in enumerate(rows[1:], 2):
            out.append("          " + row + ("," if n < len(rows) else ""))
        out[-1] += " },"
    else:
        out.append("        {},")

    if exp["geom"].startswith("wkbMulti"):
        counts = [str(len(f["points"])) for f in exp["features"]]
        out.append("        {{ {} }},".format(", ".join(counts)))
    else:
        out.append("        {},")

    out.append(f'        "{exp["srs"]}" }});')
    return "\n".join(out)


for path in sys.argv[1:]:
    print(f"// {path.rsplit('/', 1)[-1]}")
    print(cpp(parse(path)))
    print()
