# CDBench

CDBench is an AmigaOS CD filesystem benchmark utility. It benchmarks the
mounted AmigaDOS view of a disc, so it works with plain ISO9660, Rock Ridge,
Joliet, UDF, bridge discs, and audio tracks exposed by the filesystem as
files.

Build:

```sh
make
make fixture-iso
```

Typical use on AmigaOS:

```text
CDBench DEVICE CD0:
CDBench DEVICE CD0: VERBOSE
CDBench DEVICE CD0: CSV
CDBench DEVICE CD0: RAW CACHE
```

The default human-readable output prints a compact summary. `VERBOSE` prints
all per-test rows with the full selected paths. `CSV` always emits the full
machine-readable result set.

The raw SCSI baseline is opt-in. In this initial implementation it reports a
clear skipped row until safe data-track LBA equivalence is implemented.

`make fixture-iso` creates `build/fixtures/cdbench-plain.iso` with volume
label `CDBENCH_FIXTURE`, a 32 MiB sequential target, an 8 MiB random target,
cache-size probe files, a wide small-file directory, and a deep path.
