# Experimental evidence archive

Generated measurements, logs and copies of experimental source are archived
here so the feature PR exposes its production code, tests and workflow guides
in a manageable diff. No production source, test fixture, model, runtime
configuration or workflow guide is removed by this consolidation.

The 63 archives preserve 1,669 original files, totaling 39,590,761 bytes,
in 5,474,319 compressed bytes. `manifest.json` lists every original repository
path, byte count and SHA-256, together with each archive's identity. Every
member was compared byte for byte with the Git object at
`170802d44c28cde77dfdb9c31bb45244297c21a8` before its loose copy was removed.
The original receipt manifests and their hashes are retained inside the
archives without rewriting their contents.

Workflow guides link to readable copies at that immutable revision. Local
and installed packages also retain all archived files without network access.
From the repository root, verify all archives with Python 3.9 or later:

```sh
python3 docs/stock-evidence/verify.py
```

Restore exact original repository paths below a separate directory:

```sh
python3 docs/stock-evidence/verify.py --extract /tmp/gpr-evidence
```

For an older reproduction command that reads `docs/*-receipts/...`, restore
into the checkout first using `--extract .`. This adds the original evidence
files as untracked files; it does not change production code. The verifier
checks all archives and files before extraction and refuses to overwrite a
different existing file. Run experimental drivers from the checkout, following
their original guide: source inputs, external reference decoders and model
weights may still be required exactly as documented in those receipts.

In an installed or relocated package, the verifier lives at
`share/gpr/docs/stock-evidence/verify.py`. Its default archive location is its
own directory, so verification does not depend on the checkout or current
working directory. An individual trusted archive can also be read with normal
`tar` tools; its members use original `docs/...` paths.

The separate compact `stock-cnn-followup-receipts/evidence.tar.gz` and its
existing member manifest retain their own previously documented layout.
Other measurements inherited from the RAW-video foundation remain loose.

Archiving changes evidence storage, not its acceptance criteria. Failed
experiments, historical hashes, timing misses and quality limitations remain
part of the delivered record. A green hosted run validates its recorded
revision; archive hashes do not turn earlier measurements into new runs.

[Consolidation validation](consolidation.json) records the before/after diff,
three fresh stripped/relocated package checks, verification from each installed
package outside the checkout, extraction checks and the preceding green CI head.
The measured diff falls from 1,939 files / 687,796 added lines to approximately
338 files / 44,000 added lines; production code and test coverage are unchanged.
