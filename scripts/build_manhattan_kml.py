#!/usr/bin/env python3
"""
Pull official GIS layers from NYC Open Data + MTA, filter to Manhattan,
emit one styled KML for Google Earth Pro.

Layers:
  - NTAs (Neighborhood Tabulation Areas, 2020 — DCP) — polygons
  - Community Districts                              — polygons
  - Historic Districts (LPC)                         — polygons
  - Parks (NYC Parks Properties)                     — polygons
  - MODZCTA ZIP code areas                           — polygons
  - Bike Routes                                      — lines
  - Subway Stations (MTA)                            — points
  - Subway Entrances/Exits (MTA)                     — points
"""

import json
import urllib.request
import os
import sys
from xml.sax.saxutils import escape

# Manhattan bounding box — drop anything outside before writing KML
MN_BBOX = (-74.030, 40.680, -73.905, 40.885)   # (minlon, minlat, maxlon, maxlat)


def fetch(url, label):
    print(f"  ↓ {label}: {url}", file=sys.stderr)
    req = urllib.request.Request(url, headers={"User-Agent": "dsco/1.0"})
    with urllib.request.urlopen(req, timeout=45) as r:
        return json.loads(r.read().decode("utf-8"))


def in_bbox_pt(lon, lat, b=MN_BBOX):
    return b[0] <= lon <= b[2] and b[1] <= lat <= b[3]


def in_bbox_any(coords, b=MN_BBOX):
    """Walk arbitrarily-nested coord lists and return True if ANY point lies in box."""
    if isinstance(coords, (int, float)):
        return False
    if len(coords) >= 2 and all(isinstance(c, (int, float)) for c in coords[:2]):
        return in_bbox_pt(coords[0], coords[1], b)
    return any(in_bbox_any(c, b) for c in coords)


# ─── KML emitters ────────────────────────────────────────────────────────────

def coords_str(ring):
    # ring is [[lon,lat], ...]
    return " ".join(f"{c[0]:.6f},{c[1]:.6f},0" for c in ring)


def polygon_kml(coords):
    # coords is GeoJSON Polygon = [outer, hole1, hole2, ...]
    outer = coords[0]
    holes = coords[1:]
    parts = [f"<Polygon><outerBoundaryIs><LinearRing><coordinates>{coords_str(outer)}</coordinates></LinearRing></outerBoundaryIs>"]
    for h in holes:
        parts.append(f"<innerBoundaryIs><LinearRing><coordinates>{coords_str(h)}</coordinates></LinearRing></innerBoundaryIs>")
    parts.append("</Polygon>")
    return "".join(parts)


def multipoly_kml(multi):
    return "<MultiGeometry>" + "".join(polygon_kml(p) for p in multi) + "</MultiGeometry>"


def line_kml(coords):
    return f"<LineString><coordinates>{coords_str(coords)}</coordinates></LineString>"


def multiline_kml(lines):
    return "<MultiGeometry>" + "".join(line_kml(l) for l in lines) + "</MultiGeometry>"


def point_kml(coords):
    return f"<Point><coordinates>{coords[0]:.6f},{coords[1]:.6f},0</coordinates></Point>"


def placemark(name, desc, style_id, geom_kml):
    name = escape(name or "")
    desc = escape(desc or "")
    return (f'<Placemark><name>{name}</name>'
            f'<description>{desc}</description>'
            f'<styleUrl>#{style_id}</styleUrl>{geom_kml}</Placemark>')


def feature_to_kml(feat, name_field, desc_fields, style_id):
    props = feat.get("properties") or {}
    geom = feat.get("geometry") or {}
    gtype = geom.get("type", "")
    coords = geom.get("coordinates")
    if coords is None or not in_bbox_any(coords):
        return None

    name = ""
    if isinstance(name_field, (list, tuple)):
        for nf in name_field:
            if props.get(nf):
                name = str(props[nf]); break
    else:
        name = str(props.get(name_field, ""))

    desc = "\n".join(f"{k}: {props[k]}" for k in desc_fields if props.get(k) is not None)

    if gtype == "Polygon":
        g = polygon_kml(coords)
    elif gtype == "MultiPolygon":
        g = multipoly_kml(coords)
    elif gtype == "LineString":
        g = line_kml(coords)
    elif gtype == "MultiLineString":
        g = multiline_kml(coords)
    elif gtype == "Point":
        g = point_kml(coords)
    elif gtype == "MultiPoint":
        g = "<MultiGeometry>" + "".join(point_kml(c) for c in coords) + "</MultiGeometry>"
    else:
        return None

    return placemark(name, desc, style_id, g)


# ─── Layer definitions ───────────────────────────────────────────────────────

LAYERS = [
    {
        "id": "nta",
        "title": "Neighborhood Tabulation Areas (NTAs) — DCP 2020",
        "url": "https://data.cityofnewyork.us/resource/9nt8-h7nd.geojson?$where=boroname='Manhattan'&$limit=300",
        "name_field": ["ntaname", "nta_name"],
        "desc_fields": ["nta2020", "ntatype", "shape_area"],
        "style": "nta",
        "open": True,
    },
    {
        "id": "cd",
        "title": "Community Districts (DCP)",
        "url": "https://data.cityofnewyork.us/resource/5crt-au7u.geojson?$where=boro_cd<200&$limit=50",
        "name_field": ["boro_cd"],
        "desc_fields": ["boro_cd", "shape_area"],
        "style": "cd",
        "open": False,
        "name_prefix": "CD ",
    },
    {
        "id": "hd",
        "title": "Historic Districts (Landmarks Preservation Commission)",
        "url": "https://data.cityofnewyork.us/resource/skyk-mpzq.geojson?$where=borough='MN'&$limit=300",
        "name_field": ["area_name", "lpc_name"],
        "desc_fields": ["lpc_name", "status_of_", "desig_date", "borough"],
        "style": "hd",
        "open": True,
    },
    {
        "id": "parks",
        "title": "Parks & Open Space (NYC Parks)",
        "url": "https://data.cityofnewyork.us/resource/enfh-gkve.geojson?$where=borough='M'&$limit=5000",
        "name_field": ["signname", "name311"],
        "desc_fields": ["typecategory", "acres", "address"],
        "style": "park",
        "open": True,
    },
    {
        "id": "zip",
        "title": "ZIP Codes (MODZCTA)",
        "url": "https://data.cityofnewyork.us/resource/pri4-ifjk.geojson?$limit=300",
        "name_field": ["modzcta", "label"],
        "desc_fields": ["modzcta", "pop_est", "neighborhood_name"],
        "style": "zip",
        "open": False,
    },
    {
        "id": "bike",
        "title": "Bike Routes (NYC DOT)",
        "url": "https://data.cityofnewyork.us/resource/mzxg-pwib.geojson?$where=boro='1'&$limit=8000",
        "name_field": ["street", "facilityclass"],
        "desc_fields": ["street", "fromstreet", "tostreet", "tf_facilit", "ft_facilit"],
        "style": "bike",
        "open": False,
    },
    {
        "id": "subway_sta",
        "title": "Subway Stations (MTA)",
        "url": "https://data.ny.gov/resource/39hk-dx4f.geojson?$where=borough='M'&$limit=600",
        "name_field": ["stop_name", "name"],
        "desc_fields": ["daytime_routes", "line", "structure", "division"],
        "style": "sub_sta",
        "open": True,
    },
    {
        "id": "subway_ent",
        "title": "Subway Entrances/Exits (MTA)",
        "url": "https://data.ny.gov/resource/i9wp-a4ja.geojson?$limit=3000",
        "name_field": ["stop_name", "station_name", "name"],
        "desc_fields": ["entrance_type", "entry_allowed", "exit_only", "ada"],
        "style": "sub_ent",
        "open": False,
    },
]

STYLES = """
<Style id="nta">
  <LineStyle><color>ffff5050</color><width>2.5</width></LineStyle>
  <PolyStyle><color>30ff5050</color></PolyStyle>
  <LabelStyle><color>ffffffff</color><scale>0.9</scale></LabelStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="cd">
  <LineStyle><color>ff00ffff</color><width>3</width></LineStyle>
  <PolyStyle><color>1500ffff</color></PolyStyle>
  <LabelStyle><color>ffffffff</color><scale>1.0</scale></LabelStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="hd">
  <LineStyle><color>ffd400ff</color><width>2</width></LineStyle>
  <PolyStyle><color>40d400ff</color></PolyStyle>
  <LabelStyle><color>ffd400ff</color><scale>0.8</scale></LabelStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="park">
  <LineStyle><color>ff008000</color><width>1.5</width></LineStyle>
  <PolyStyle><color>5000c800</color></PolyStyle>
  <LabelStyle><color>ff00ff00</color><scale>0.75</scale></LabelStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="zip">
  <LineStyle><color>ff00d7ff</color><width>1.5</width></LineStyle>
  <PolyStyle><color>1500d7ff</color></PolyStyle>
  <LabelStyle><color>ffffd700</color><scale>0.85</scale></LabelStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="bike">
  <LineStyle><color>ff32cd32</color><width>2</width></LineStyle>
  <IconStyle><scale>0</scale></IconStyle>
</Style>
<Style id="sub_sta">
  <IconStyle><scale>1.1</scale><Icon><href>http://maps.google.com/mapfiles/kml/shapes/subway.png</href></Icon></IconStyle>
  <LabelStyle><color>ffffffff</color><scale>0.85</scale></LabelStyle>
</Style>
<Style id="sub_ent">
  <IconStyle><scale>0.6</scale><Icon><href>http://maps.google.com/mapfiles/kml/shapes/arrow.png</href></Icon></IconStyle>
  <LabelStyle><color>ffaaaaaa</color><scale>0.6</scale></LabelStyle>
</Style>
"""


def build():
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<kml xmlns="http://www.opengis.net/kml/2.2">',
           '<Document>',
           '<name>NYC — Hyper-local Manhattan Overlay</name>',
           '<description>Official GIS layers from NYC Open Data + MTA. Filtered to Manhattan.</description>',
           STYLES]

    totals = {}
    for layer in LAYERS:
        try:
            gj = fetch(layer["url"], layer["title"])
        except Exception as e:
            print(f"  ✗ {layer['id']}: {e}", file=sys.stderr)
            continue

        feats = gj.get("features") or []
        out.append(f'<Folder><name>{escape(layer["title"])}</name><open>{1 if layer["open"] else 0}</open>')
        kept = 0
        for f in feats:
            pm = feature_to_kml(f, layer["name_field"], layer["desc_fields"], layer["style"])
            if pm:
                if layer.get("name_prefix") and "<name>" in pm:
                    pm = pm.replace("<name>", f"<name>{layer['name_prefix']}", 1)
                out.append(pm)
                kept += 1
        out.append("</Folder>")
        totals[layer["id"]] = (len(feats), kept)
        print(f"  ✓ {layer['id']}: fetched {len(feats)}, kept {kept} in MN bbox", file=sys.stderr)

    # Anchor pin for The Maritime Hotel
    out.append('<Folder><name>Anchors</name><open>1</open>'
               '<Placemark><name>The Maritime Hotel</name>'
               '<description>363 W 16th St — Chelsea anchor</description>'
               '<Point><coordinates>-74.0030,40.7407,0</coordinates></Point>'
               '</Placemark></Folder>')

    out.append("</Document></kml>")

    print("\nSummary:", file=sys.stderr)
    for k, (got, kept) in totals.items():
        print(f"  {k:12s}  got={got:5d}  kept={kept:5d}", file=sys.stderr)

    return "\n".join(out)


if __name__ == "__main__":
    dst = sys.argv[1] if len(sys.argv) > 1 else "/Users/arthurcolle/manhattan_hyperlocal.kml"
    kml = build()
    with open(dst, "w") as f:
        f.write(kml)
    print(f"\nWrote {dst} ({os.path.getsize(dst)/1024:.1f} KB)", file=sys.stderr)
