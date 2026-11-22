# Compile-cache measurement receipts

29 corrected GTX1080/Linux replay runs, 11,016 item rows. See the adjacent
`astra_shader_compile_report.md` for scope, source identities, definitions,
controlled comparisons and platform limitations.

- `runs.json`: numeric summaries, batch reports, top costs, source/input hashes,
  cache manifests and process measurements. Batch walls overlap.
- `pipelines.csv`: every recorded create identity, elapsed time, SPIR-V size,
  feedback and concurrent memory attribution. Hashes are decimal strings;
  spreadsheet imports should treat them as text to avoid precision loss.
- `images.json`: numeric held-image A/A and A/B errors and temporal noise;
  no images or private scene metadata.
- `shader-hashes.json`: four identical baseline/fixed generated header hashes;
  the headers themselves are not included.
- `control-proof.json`: worker reuse and completion receipts extracted from logs.
- `host-restoration.json`: final remote restoration/release verification.

`export_receipts.py` generates the first two files from private raw run directories.
The measured package source is distinct from the later evidence commit.
Oversize cases use explicit size-query fault injection on real driver caches.
AMD/Windows and supported RT are untested because the AMD host was unreachable.
