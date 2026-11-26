# LibreRaman

Software for the Anton Paar Cora 5000 Raman spectrometer. The product is the C++/Qt6 app in `cpp/`; the Python
package `libreraman/` is frozen (unmaintained; the golden test data it made are kept as they are).

Read `knowledge/index.md` first: instrument constraints (532 nm only, laser power manual, one GC vial), methods,
file formats, build and test commands, and recorded decisions (Open Knowledge Format bundle). When you learn a
durable fact or a decision is made, add or update a file there and a line in `knowledge/log.md`.

- Build/test: `knowledge/build/toolchain.md` (`source ~/opt/libreraman-env.sh`, build dirs in `~/opt/build-*`).
- Work is tracked in GitHub issues (ohlincha/libreraman); reference them in commits.
- Keep tests green (`ctest`); add a test for each change in behaviour; update `docs/MANUAL.md` for user-visible changes.
