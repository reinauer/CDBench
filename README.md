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

Mount identity includes best-effort version metadata. CDBench records the
handler `$VER:` string by scanning the live loaded filesystem seglist, records
the matching `FileSystem.resource` version when available, and records the
backing Exec device library version/id after opening the device.

The raw SCSI baseline is opt-in. It opens the backing Exec device identified
from the mount, issues SCSI READ CAPACITY and READ(10) through `HD_SCSICMD`
or `NSCMD_TD_SCSI`, and only reads 2048-byte sectors. The raw row is a device
baseline from LBA 0; `seq_data_efficiency` remains skipped until the tool can
prove that the raw range matches the filesystem file being read.

`make fixture-iso` creates `build/fixtures/cdbench-plain.iso` with volume
label `CDBENCH_FIXTURE`, a 32 MiB sequential target, an 8 MiB random target,
cache-size probe files, a wide small-file directory, and a deep path.
