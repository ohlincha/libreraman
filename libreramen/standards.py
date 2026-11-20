"""
Raman shift reference values (cm^-1) for calibration standards.

Sources and confidence:
* "ASTM" - ASTM E1840 (Standard Guide for Raman Shift Standards for Spectrometer Calibration).
  The values here were entered from the standard as commonly quoted; check them against the
  current edition before relying on them for accredited work.
* "lit"  - typical literature values for neat liquids / pure solids at room temperature,
  generally reliable to about +-1-2 cm^-1. Broad bands are listed with a wider tolerance.

Each band: (position, relative strength 1-3, tolerance). Strength helps the matcher prefer
strong bands; tolerance is the expected uncertainty of the reference (not the instrument's).
"""

S, M, W = 3, 2, 1     # strong / medium / weak

STANDARDS = {
    # --- ASTM E1840 -------------------------------------------------------------------------
    "cyclohexane": dict(kind="liquid", source="ASTM", bands=[
        (384.1, M), (426.3, M), (801.3, S), (1028.3, S), (1157.6, M), (1266.4, M), (1444.4, S),
        (2664.4, W), (2852.9, S), (2923.8, S), (2938.3, S)]),
    "naphthalene": dict(kind="solid", source="ASTM", bands=[
        (513.8, M), (763.8, S), (1021.6, M), (1147.2, M), (1382.2, S), (1464.5, M), (1576.6, M),
        (3056.4, M)]),
    "sulfur": dict(kind="solid", source="ASTM", bands=[(153.8, S), (219.1, S), (473.2, S)]),
    "polystyrene": dict(kind="solid", source="ASTM", bands=[
        (620.9, M), (795.8, M), (1001.4, S), (1031.8, M), (1155.3, M), (1450.5, M), (1583.1, M),
        (1602.3, S), (2852.4, W), (2904.5, M), (3054.3, S)]),
    "paracetamol": dict(kind="solid", source="ASTM", note="a plain tablet: cheap multi-point calibration", aka=["4-acetamidophenol", "acetaminophen"], bands=[
        (213.3, M), (329.2, M), (390.9, M), (465.1, M), (504.0, M), (651.6, M), (710.8, M),
        (797.2, M), (834.5, M), (857.9, S), (968.7, W), (1105.5, M), (1168.5, S), (1236.8, S),
        (1278.5, M), (1323.9, S), (1371.5, M), (1515.1, W), (1561.5, M), (1648.4, S), (2931.1, M),
        (3064.6, M)]),
    "benzonitrile": dict(kind="liquid", source="ASTM", bands=[
        (460.9, M), (548.5, M), (751.3, M), (767.1, M), (1000.7, S), (1026.6, M), (1177.9, M),
        (1192.6, M), (1598.9, S), (2229.4, S), (3072.3, M)]),
    "toluene/acetonitrile 50:50": dict(kind="liquid", source="ASTM", aka=["toluene-acetonitrile"], bands=[
        (378.5, M), (521.7, M), (786.5, S), (919.0, M), (1003.6, S), (1030.6, M), (1211.4, M),
        (1376.4, M), (1605.1, M), (2253.7, S), (2940.8, S), (3057.1, M)]),
    "indene": dict(kind="liquid", source="ASTM", bands=[
        (730.4, S), (1018.6, M), (1205.6, M), (1552.6, S), (1610.2, M), (3054.7, M)]),
    # --- solvents (lit) ----------------------------------------------------------------------
    "carbon tetrachloride": dict(kind="liquid", source="lit", aka=["ccl4", "tetrachloromethane"], bands=[
        (218, S), (314, S), (459, S), (762, M), (790, M)]),
    "chloroform": dict(kind="liquid", source="lit", aka=["chcl3", "trichloromethane"], bands=[
        (261.5, M), (364, S), (667, S), (760, M), (1216, M), (3019, S)]),
    "dichloromethane": dict(kind="liquid", source="lit", aka=["dcm", "ch2cl2", "methylene chloride"], bands=[
        (282, S), (704, S), (737, M), (1157, W), (1423, W), (2987, S), (3047, W)]),
    "toluene": dict(kind="liquid", source="lit", bands=[
        (521.7, M), (786.5, S), (1003.6, S), (1030.6, M), (1211.4, M), (1605.1, M), (2920.0, M), (3052.8, M)]),
    "benzene": dict(kind="liquid", source="lit", bands=[(606, M), (992, S), (1178, M), (1585, M), (3062, S)]),
    "acetonitrile": dict(kind="liquid", source="lit", aka=["mecn"], bands=[
        (379, M), (918, S), (1376, M), (2249, S), (2942, S)]),
    "acetone": dict(kind="liquid", source="lit", bands=[(787, S), (1066, M), (1430, M), (1710, M), (2925, S)]),
    "ethanol": dict(kind="liquid", source="lit", aka=["etoh"], bands=[
        (433, M), (882, S), (1052, M), (1096, M), (1454, M), (2878, S), (2930, S), (2974, S)]),
    "methanol": dict(kind="liquid", source="lit", aka=["meoh"], bands=[(1033, S), (1453, M), (2837, S), (2945, S)]),
    "isopropanol": dict(kind="liquid", source="lit", aka=["2-propanol", "ipa"], bands=[
        (819, S), (954, M), (1129, M), (1453, M), (2884, S), (2938, S), (2975, S)]),
    "dmso": dict(kind="liquid", source="lit", aka=["dimethyl sulfoxide"], bands=[
        (667, S), (698, S), (953, M), (1042, M), (1419, M), (2913, S), (2997, M)]),
    "ethylene glycol": dict(kind="liquid", source="lit", bands=[
        (482, M), (523, M), (864, S), (1036, M), (1089, M), (1463, M), (2880, S), (2942, S)]),
    "cyclohexane (lit)": dict(kind="liquid", source="lit", bands=[(801, S), (1028, S), (1444, S), (2853, S), (2938, S)]),
    # --- solids (lit) ------------------------------------------------------------------------
    "silicon": dict(kind="solid", source="lit", aka=["si", "si wafer"], bands=[(520.5, S)],
                    note="single sharp band: one-point (offset) check; 520.5-520.7 depending on temperature"),
    "diamond": dict(kind="solid", source="lit", bands=[(1332.5, S)]),
    "calcite": dict(kind="solid", source="lit", aka=["caco3"], bands=[(155, M), (281, M), (711, M), (1085.5, S)]),
    "anatase": dict(kind="solid", source="lit", aka=["tio2 anatase"], bands=[
        (144, S), (197, W), (399, M), (513, M), (639, M)]),
    "rutile": dict(kind="solid", source="lit", aka=["tio2 rutile"], bands=[(447, S), (612, S)]),
    "ptfe": dict(kind="solid", source="lit", aka=["teflon"], bands=[
        (292, M), (386, M), (576, W), (732, S), (1216, M), (1299, M), (1381, M)]),
    "barium sulfate": dict(kind="solid", source="lit", aka=["baso4"], bands=[(453, M), (461, M), (988, S)]),
}


def find(name):
    key = name.strip().lower()
    for k, v in STANDARDS.items():
        if key == k or key in [a.lower() for a in v.get("aka", [])]:
            return k, v
    raise KeyError(f"unknown standard {name!r}; known: {', '.join(sorted(STANDARDS))}")


def bands(name):
    """[(position, strength, tolerance)] with the default tolerance filled in."""
    _, v = find(name)
    tol = 0.5 if v["source"] == "ASTM" else 2.0
    return [(b[0], b[1], b[2] if len(b) > 2 else tol) for b in v["bands"]]
