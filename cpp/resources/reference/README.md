# Reference library (shipped with LibreRamen)

Literature Raman band positions with citations and measured spectra from open data sets, added to the default library on first start (and to any library
with *Library ▸ Add reference entries*). Entries are peak lists, not spectra: positions, relative intensities
where the source gives them (0 = unknown), widths where stated, assignments, the source(s) read for each band,
the excitation laser, notes on composition/phase caveats and a confidence (high/medium/low).

Band positions are factual data taken from the cited papers; no spectra or figures are copied. RRUFF spectra are
not included: RRUFF's terms forbid redistribution (RRUFF IDs are listed so users can import them themselves).

| File | Contents | Compiled |
|---|---|---|
| battery.json | 33 battery materials: cathodes, anodes and carbons, binders and cellulose, electrolyte salts and anions, SEI products | 2026-11-23 (research agent, sources opened and read; spot-checked) |
| solvents.json | 26 solvents, ASTM E1840 shift standards and PET; several solvents positions only (intensity 0, from Flanagan & Glavin, Sci. Data 2025) | 2026-11-23 (Sonnet research agent; spot-checked; formic acid and C–H-only PE/PP/PMMA dropped) |
| interphase.json | 7 measured spectra (488 nm): Li2CO3, LiPF6, Li2O, lithium acetate, MnF2, NiF2, PEO. Dryad doi:10.5061/dryad.v15dv421w, **CC0**; Karapin-Springorum et al., Sci. Data 2025. LiF (no first-order Raman) and LiH left out | 2026-11-23 (tools/make_spectra.py) |
| polymers.json | 42 measured spectra (532 nm): polymers incl. PE/HDPE/UHMWPE, PP, PTFE, PVDF, nylon/PA, PET, PMMA, PS, PI, PEEK, cellulose, cellulose acetate, rubbers. OSF osf.io/7cqv4, **CC BY 4.0**; Miller et al., Sci. Data 9, 780 (2022), doi:10.1038/s41597-022-01883-5. One spectrum per polymer (most typical replicate) | 2026-11-23 (tools/make_spectra.py) |
| electrolyte.json | 10 carbonate-electrolyte entries: EC, PC, DMC, EMC, DEC, FEC, EC and PC coordinated to Li+, LP30 (1 M LiPF6 in EC/DMC), [Li(G4)]+. Literature band lists; EMC, DEC (SERS) and FEC (SERS) low confidence; no intensities | 2026-11-23 (Opus research agent; many publishers blocked automated access) |

Format: an array of `{name, formula, cas, cls, tags, laser_nm, peaks: [{position, intensity, width, label}],
sources: [{ref, doi_or_url, covers}], rruff_id, notes, confidence}`. Bump `kReferenceVersion` in
`src/core/library.cpp` when files change so existing default libraries receive new entries.

Spectra entries carry `"spectrum": {label, processed, x, y}` and their own `"licence"`; LibreRamen picks their peak
tables on import. Attribution for CC BY data is kept in each entry's licence and sources. Regenerate with
`tools/make_spectra.py` from the original downloads (not kept in the repository).
