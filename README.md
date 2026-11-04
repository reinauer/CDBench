# CDBench

CDBench is a CD filesystem benchmark utility for AmigaOS 3, AmigaOS 4,
and AROS. It benchmarks the mounted AmigaDOS view of a disc, so it works
with plain ISO9660, Rock Ridge,
Joliet, UDF, bridge discs, and audio tracks exposed by the filesystem as
files.

Build the same source with the matching cross-compiler:

```sh
make CC=m68k-amigaos-gcc    # AmigaOS 3: build/CDBench
make CC=ppc-amigaos-gcc     # AmigaOS 4: build/os4/CDBench
make CC=x86_64-aros-gcc     # AROS: build/aros/CDBench
make fixture-iso
```

The compiler target selects the platform flags. `TARGET_OS=os3`, `os4`,
or `aros` can override detection for compiler wrappers; `BUILD_DIR` selects
an independent output directory. Use separate build directories when
changing CPU/ABI flags, such as i386 versus x86-64 AROS. SDKs that require
an explicit sysroot can pass it through `AMIGA_SYSFLAGS`, preserving the
platform's other flags (for example, `--sysroot=/opt/x86_64-aros -static`).
Use SDK headers and runtime libraries matching the guest OS.

The workloads, ReadEClock timing, CSV columns, and summary selection are
shared by all builds. No profiling markers or diagnostic counters are
compiled into the benchmark.

Typical use on AmigaOS:

```text
1> stack 16384
1> CDBench DEVICE CD0:
1> CDBench DEVICE CD0: VERBOSE
1> CDBench DEVICE CD0: CSV
1> CDBench DEVICE CD0: RAW CACHE
```

The default human-readable output prints a compact summary. `VERBOSE` prints
all per-test rows with the full selected paths. `CSV` always emits the full
machine-readable result set. Existing tests such as adjacent/edge reads,
ExNext traversal, shallow locks, negative lookups, and parent operations
are included in the full output even when omitted from the compact summary.

Mount identity includes best-effort version metadata. CDBench records the
handler `$VER:` string by scanning the live loaded filesystem seglist on
classic AmigaOS. That Hunk-layout scan is disabled on AmigaOS 4 and AROS,
where ELF segments require different handling; the `handler_version` and
`handler_version_source` fields remain empty there. All builds record the matching
`FileSystem.resource` version when available and the backing Exec device
library version/id after opening the device. The resource version identifies
the registered entry; it need not identify a separately force-loaded handler.
Record the tested handler binary and its hash separately when comparing
filesystem versions.

The raw SCSI baseline is opt-in. It opens the backing Exec device identified
from the mount, issues SCSI READ CAPACITY and READ(10) through `HD_SCSICMD`
or `NSCMD_TD_SCSI`, and only reads 2048-byte sectors. The raw row is a device
baseline from LBA 0; `seq_data_efficiency` remains skipped until the tool can
prove that the raw range matches the filesystem file being read.

`make fixture-iso` creates fixture images with volume label
`CDBENCH_FIXTURE`, a 32 MiB sequential target, an 8 MiB random target,
cache-size probe files, a wide small-file directory, and a deep path. The
existing `build/fixtures/cdbench-plain.iso` name is kept for compatibility;
when the local ISO tool supports it, ISO9660-only, UDF, and bridge variants
are emitted next to it.
