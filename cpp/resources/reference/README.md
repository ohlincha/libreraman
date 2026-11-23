# Reference library (shipped with LibreRamen)

Literature Raman band positions with citations, added to the default library on first start (and to any library
with *Library ▸ Add reference entries*). Entries are peak lists, not spectra: positions, relative intensities
where the source gives them (0 = unknown), widths where stated, assignments, the source(s) read for each band,
the excitation laser, notes on composition/phase caveats and a confidence (high/medium/low).

Band positions are factual data taken from the cited papers; no spectra or figures are copied. RRUFF spectra are
not included: RRUFF's terms forbid redistribution (RRUFF IDs are listed so users can import them themselves).

| File | Contents | Compiled |
|---|---|---|
| battery.json | 33 battery materials: cathodes, anodes and carbons, binders and cellulose, electrolyte salts and anions, SEI products | 2026-11-23 (research agent, sources opened and read; spot-checked) |

Format: an array of `{name, formula, cas, cls, tags, laser_nm, peaks: [{position, intensity, width, label}],
sources: [{ref, doi_or_url, covers}], rruff_id, notes, confidence}`. Bump `kReferenceVersion` in
`src/core/library.cpp` when files change so existing default libraries receive new entries.
