#!/usr/bin/env python3
"""Convert open reference spectra into LibreRamen reference JSON (spectra entries).

  make_spectra.py interphase Raman.xlsx            > ../interphase.json   (Dryad doi:10.5061/dryad.v15dv421w, CC0)
  make_spectra.py polymers Data2_processed.csv     > ../polymers.json     (OSF osf.io/7cqv4, CC BY 4.0)
  make_spectra.py rod <folder of .rod files>        > ../rod.json          (Raman Open Database, CC0)

Needs openpyxl for the .xlsx. Output entries have the reference format of battery.json plus
"spectrum": {"label", "processed", "x": [...], "y": [...]}; peak tables are picked by LibreRamen on import.
"""
import csv
import json
import math
import sys

DRYAD_REF = {
    "ref": "Karapin-Springorum L., Sarycheva A., Dopilka A. et al., An infrared, Raman, and X-ray database of "
           "battery interphase components, Sci. Data 12 (2025); dataset Dryad doi:10.5061/dryad.v15dv421w (CC0)",
    "doi_or_url": "https://doi.org/10.5061/dryad.v15dv421w",
    "covers": "measured spectrum (\"Final\": background-subtracted, FFT-smoothed, normalised 0-1); 488 nm, "
              "Renishaw Qontor, samples sealed under argon",
}
# sheet prefix -> (name, formula, cas, cls, tags, notes)
DRYAD = {
    "CH3COOLi": ("Lithium acetate", "CH3COOLi", "546-89-4", "battery SEI", ["SEI", "interphase"], ""),
    "Li2CO3": ("Lithium carbonate (measured spectrum)", "Li2CO3", "554-13-2", "battery SEI",
               ["SEI", "interphase", "CEI"], ""),
    "LiPF6": ("Lithium hexafluorophosphate (measured spectrum)", "LiPF6", "21324-40-3", "battery electrolyte salt",
              ["electrolyte", "salt"], ""),
    "Li2O": ("Lithium oxide (measured spectrum)", "Li2O", "12057-24-8", "battery SEI", ["SEI", "interphase"],
             "T2g mode at about 523 cm-1."),
    "MnF2": ("Manganese(II) fluoride", "MnF2", "7782-64-1", "battery CEI", ["CEI", "interphase", "transition metal"], ""),
    "NiF2": ("Nickel(II) fluoride", "NiF2", "10028-18-9", "battery CEI", ["CEI", "interphase", "transition metal"],
             "The weak band at 2330 cm-1 is atmospheric N2, not NiF2."),
    "H(OCH2CH2)nOH": ("Poly(ethylene oxide) (PEO)", "(C2H4O)n", "25322-68-3", "polymer",
                      ["PEO", "polymer electrolyte", "SEI"], ""),
}
# left out: 6LiF / 7LiF (rock salt, no first-order Raman; raw only), LiH (no clean first-order bands)

OSF_REF = {
    "ref": "Miller E. et al., A Raman spectral reference library of potential anthropogenic and biological ocean "
           "polymers, Sci. Data 9, 780 (2022); data osf.io/7cqv4 (CC BY 4.0)",
    "doi_or_url": "https://doi.org/10.1038/s41597-022-01883-5",
    "covers": "measured spectrum (processed: baseline-corrected, normalised); 532 nm",
}
# sample label -> (name, formula, cas, cls, tags)
OSF = {
    "ABS": ("ABS (acrylonitrile butadiene styrene)", "", "9003-56-9", "polymer", ["ABS"]),
    "algin": ("Alginate", "", "9005-38-3", "biopolymer", ["alginate", "binder"]),
    "BR": ("Butadiene rubber (BR)", "", "9003-17-2", "polymer", ["rubber"]),
    "calcium carbonate": ("Calcium carbonate (biogenic)", "CaCO3", "471-34-1", "mineral", ["calcite", "aragonite"]),
    "CA": ("Cellulose acetate (CA)", "", "9004-35-7", "polymer", ["cellulose", "separator"]),
    "cellulose": ("Cellulose (measured spectrum)", "(C6H10O5)n", "9004-34-6", "biopolymer",
                  ["cellulose", "separator", "pulp"]),
    "charcoal": ("Charcoal", "C", "", "carbon", ["carbon", "amorphous carbon"]),
    "chitin": ("Chitin", "", "1398-61-4", "biopolymer", ["chitin"]),
    "EPS": ("Expanded polystyrene (EPS)", "", "9003-53-6", "polymer", ["PS", "foam"]),
    "FEP": ("FEP (fluorinated ethylene propylene)", "", "25067-11-2", "polymer", ["fluoropolymer"]),
    "FPM": ("Fluoroelastomer (FPM/FKM)", "", "", "polymer", ["fluoropolymer", "rubber", "O-ring"]),
    "HDPE": ("Polyethylene, high density (HDPE)", "(C2H4)n", "9002-88-4", "polymer", ["PE", "separator"]),
    "LDPE": ("Polyethylene, low density (LDPE)", "(C2H4)n", "9002-88-4", "polymer", ["PE"]),
    "Latex": ("Natural rubber latex", "", "9006-04-6", "polymer", ["rubber", "gloves"]),
    "NBR": ("Nitrile rubber (NBR)", "", "9003-18-3", "polymer", ["rubber", "gloves"]),
    "nylon": ("Nylon", "", "", "polymer", ["polyamide", "PA", "separator"]),
    "PA": ("Polyamide (PA)", "", "", "polymer", ["nylon", "separator"]),
    "PBAT": ("PBAT (polybutylene adipate terephthalate)", "", "55231-08-8", "polymer", ["biodegradable"]),
    "PBS": ("PBS (polybutylene succinate)", "", "25777-14-4", "polymer", ["biodegradable"]),
    "PCL": ("Polycaprolactone (PCL)", "", "24980-41-4", "polymer", ["polyester"]),
    "PC": ("Polycarbonate (PC)", "", "25037-45-0", "polymer", ["polycarbonate"]),
    "PEEK": ("PEEK (polyether ether ketone)", "", "29658-26-2", "polymer", ["PEEK", "cell parts"]),
    "PEI": ("Polyetherimide (PEI)", "", "61128-46-9", "polymer", ["Ultem"]),
    "PE": ("Polyethylene (PE)", "(C2H4)n", "9002-88-4", "polymer", ["PE", "separator"]),
    "PETE": ("Poly(ethylene terephthalate) (PET, measured spectrum)", "", "25038-59-9", "polymer", ["PET"]),
    "PETG": ("PETG (glycol-modified PET)", "", "", "polymer", ["PET"]),
    "PEX": ("Cross-linked polyethylene (PEX)", "", "", "polymer", ["PE"]),
    "PFA": ("PFA (perfluoroalkoxy alkane)", "", "26655-00-5", "polymer", ["fluoropolymer"]),
    "PI": ("Polyimide (PI, Kapton-type)", "", "", "polymer", ["Kapton", "tape"]),
    "PLA": ("Poly(lactic acid) (PLA)", "", "26100-51-6", "polymer", ["biodegradable"]),
    "PMMA": ("Poly(methyl methacrylate) (PMMA)", "", "9011-14-7", "polymer", ["acrylic", "PMMA"]),
    "POM": ("Polyoxymethylene (POM, acetal)", "", "9002-81-7", "polymer", ["acetal"]),
    "PPO": ("Poly(phenylene oxide) (PPO)", "", "25134-01-4", "polymer", ["PPE"]),
    "PP": ("Polypropylene (PP)", "(C3H6)n", "9003-07-0", "polymer", ["PP", "separator", "Celgard"]),
    "PPS": ("Poly(phenylene sulfide) (PPS)", "", "26125-40-6", "polymer", []),
    "PS": ("Polystyrene (measured spectrum)", "(C8H8)n", "9003-53-6", "polymer", ["PS"]),
    "PTFE": ("Polytetrafluoroethylene (PTFE)", "(C2F4)n", "9002-84-0", "polymer", ["fluoropolymer", "binder", "Teflon"]),
    "PVA": ("Poly(vinyl alcohol) (PVA)", "", "9002-89-5", "polymer", ["binder"]),
    "PVC": ("Poly(vinyl chloride) (PVC)", "", "9002-86-2", "polymer", []),
    "PVDF": ("PVDF (measured spectrum)", "(C2H2F2)n", "24937-79-9", "polymer", ["binder", "fluoropolymer"]),
    "SBS": ("SBS (styrene butadiene styrene)", "", "9003-55-8", "polymer", ["rubber", "SBR-like"]),
    "UHMW": ("Polyethylene, ultra-high molecular weight (UHMWPE)", "(C2H4)n", "9002-88-4", "polymer",
             ["PE", "separator"]),
}
# left out: unidentified field samples, bone/dentin/proteins, and ambiguous labels (P, BPF, PSA, PPE)


def r4(v):
    return float(f"{v:.4g}")


def entry(name, formula, cas, cls, tags, laser, src, notes, x, y, label, licence):
    return {"licence": licence, "name": name, "formula": formula, "cas": cas, "cls": cls, "tags": tags, "laser_nm": laser,
            "peaks": [], "sources": [src], "rruff_id": "", "notes": notes, "confidence": "high",
            "spectrum": {"label": label, "processed": True, "x": [round(v, 2) for v in x], "y": [r4(v) for v in y]}}


def interphase(path):
    import openpyxl
    wb = openpyxl.load_workbook(path, read_only=True, data_only=True)
    out = []
    for ws in wb.worksheets:
        key = ws.title.split(" ")[0]
        if key not in DRYAD:
            continue
        rows = list(ws.iter_rows(values_only=True))
        hdr = rows[0]
        xi = next(i for i, h in enumerate(hdr) if h and "column F" in str(h))
        yi = next(i for i, h in enumerate(hdr) if h and str(h).startswith("Final"))
        pts = sorted((float(r[xi]), float(r[yi])) for r in rows[1:] if r[xi] is not None and r[yi] is not None)
        name, formula, cas, cls, tags, notes = DRYAD[key]
        note = ("Measured spectrum from the Dryad battery-interphase database (488 nm). " + notes).strip()
        out.append(entry(name, formula, cas, cls, tags, 488, DRYAD_REF, note,
                         [p[0] for p in pts], [p[1] for p in pts], "Dryad Final (488 nm)",
                         "CC0 1.0 (Dryad); cite Karapin-Springorum et al., Sci. Data 2025"))
    return out


def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    sa = math.sqrt(sum((v - ma) ** 2 for v in a)); sb = math.sqrt(sum((v - mb) ** 2 for v in b))
    return sum((u - ma) * (v - mb) for u, v in zip(a, b)) / (sa * sb or 1)


def polymers(path):
    spectra = {}
    for row in csv.DictReader(open(path, newline="")):
        if row["SAMPLE"] not in OSF:
            continue
        spectra.setdefault(row["SAMPLE"], {}).setdefault(row["PARTICLE"], []).append(
            (float(row["WAVE"]), float(row["INTENSITY"])))
    out = []
    for sample, parts in spectra.items():
        ids = sorted(parts)
        ys = {p: [v for _, v in sorted(parts[p])] for p in ids}
        # the most typical replicate: highest mean correlation with the others
        best = max(ids, key=lambda p: sum(corr(ys[p], ys[q]) for q in ids if q != p)) if len(ids) > 1 else ids[0]
        pts = sorted(parts[best])
        name, formula, cas, cls, tags = OSF[sample]
        note = f"Measured spectrum, OSF Raman polymer library sample {best} (532 nm)"
        if len(ids) > 1:
            note += f"; most typical of {len(ids)} samples ({', '.join(ids)})"
        out.append(entry(name, formula, cas, cls, tags, 532, OSF_REF, note + ".",
                         [p[0] for p in pts], [p[1] for p in pts], f"OSF {best} (532 nm)",
                         "CC BY 4.0; Miller et al., Sci. Data 9, 780 (2022), osf.io/7cqv4"))
    return sorted(out, key=lambda e: e["name"])


ROD_LASER_RANK = {"532": 0, "514": 1, "514.5": 1, "488": 2, "633": 3, "632.8": 3, "780": 4, "785": 4}
ROD_BAD_NAMES = {"", ",", ";", "co", "phase ii or wii", "phase iii", "phase iv"}
# deposits whose spectrum is not the named compound (checked against a measured standard)
ROD_BAD_CODES = {
    "3500085",   # "Cobalt (II) oxide": the spectrum is polystyrene (1002, 1031, 1603, 2905, 3053; no CoO bands)
}


def rod_file(path):
    """One ROD (CIF-style) file: metadata dict and the spectrum."""
    meta, x, y = {}, [], []
    lines = open(path, errors="replace").read().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith("_") and not ln.startswith("_raman_spectrum."):
            parts = ln.split(None, 1)
            if len(parts) == 2:
                meta[parts[0]] = parts[1].strip().strip("'")
            elif i + 1 < len(lines) and lines[i + 1].startswith(";"):   # multi-line value
                j, buf = i + 2, []
                while j < len(lines) and not lines[j].startswith(";"):
                    buf.append(lines[j].strip())
                    j += 1
                meta[parts[0]] = " ".join(buf).strip()
                i = j
        elif ln == "_raman_spectrum.intensity":
            for row in lines[i + 1:]:
                f = row.split()
                if len(f) != 2:
                    break
                try:
                    x.append(float(f[0])); y.append(float(f[1]))
                except ValueError:
                    break
            break
        i += 1
    # first author: the line after _publ_author_name
    for k, ln in enumerate(lines):
        if ln.strip() == "_publ_author_name" and k + 1 < len(lines):
            meta["author"] = lines[k + 1].strip().strip("'")
            break
    return meta, x, y


def rod(folder):
    import os
    best = {}
    for dp, _, fs in os.walk(folder):
        for f in fs:
            if not f.endswith(".rod"):
                continue
            meta, x, y = rod_file(os.path.join(dp, f))
            if meta.get("_raman_determination_method", "experimental") != "experimental" or len(x) < 50:
                continue
            mineral = meta.get("_chemical_name_mineral", "")
            name = mineral or meta.get("_chemical_name_common", "") or meta.get("_chemical_name_systematic", "")
            if name.strip().lower() in ROD_BAD_NAMES or f[:-4] in ROD_BAD_CODES:
                continue
            laser = meta.get("_raman_measurement_device.excitation_laser_wavelength", "")
            pts = sorted(zip(x, y))
            span = pts[-1][0] - pts[0][0]
            score = (ROD_LASER_RANK.get(laser, 5), -span)
            key = name.strip().lower()
            if key not in best or score < best[key][0]:
                best[key] = (score, f, meta, pts, name.strip(), bool(mineral))
    out = []
    for key, (_, f, meta, pts, name, is_mineral) in best.items():
        # 2 cm-1 grid at most (size), intensity 0..1
        xs, ys = [], []
        for xv, yv in pts:
            if xs and xv - xs[-1] < 2.0:
                continue
            xs.append(xv); ys.append(yv)
        lo, hi = min(ys), max(ys)
        ys = [(v - lo) / (hi - lo or 1) for v in ys]
        code = meta.get("_rod_database.code", f[:-4])
        laser = meta.get("_raman_measurement_device.excitation_laser_wavelength", "")
        try:
            laser_nm = float(laser)
        except ValueError:
            laser_nm = 0
        formula = meta.get("_chemical_formula_sum", "").replace(" ", "")
        title = meta.get("_publ_section_title", "")
        journal = meta.get("_journal_name_full", "")
        year = meta.get("_journal_year", "")
        src = {"ref": f"{meta.get('author', '')}: {title} ({journal} {year}); Raman Open Database entry {code}".strip(),
               "doi_or_url": f"https://solsa.crystallography.net/rod/{code}.html",
               "covers": "measured spectrum (as deposited; intensity scaled 0-1, at most one point per 2 cm-1)"}
        notes = f"Raman Open Database {code}" + (f", {laser} nm" if laser else "")
        dev = " ".join(v for v in (meta.get("_raman_measurement_device.company", ""),
                                     meta.get("_raman_measurement_device.model", "")) if v)
        if dev:
            notes += f", {dev}"
        if meta.get("_chemical_compound_source"):
            notes += f"; sample: {meta['_chemical_compound_source']}"
        disp = name[0].upper() + name[1:] if name else name
        e = entry(disp, formula, "", "mineral" if is_mineral else "inorganic", ["ROD"], laser_nm, src,
                  notes + ".", xs, ys, f"ROD {code}" + (f" ({laser} nm)" if laser else ""),
                  "CC0 1.0 (Raman Open Database, solsa.crystallography.net/rod); cite El Mendili et al., "
                  "Sci. Data 6, 2019 and the depositor")
        e["spectrum"]["processed"] = False
        out.append(e)
    return sorted(out, key=lambda e: e["name"].lower())


if __name__ == "__main__":
    kind, path = sys.argv[1], sys.argv[2]
    data = {"interphase": interphase, "polymers": polymers, "rod": rod}[kind](path)
    json.dump(data, sys.stdout, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write("\n")
    print(f"{len(data)} entries", file=sys.stderr)
