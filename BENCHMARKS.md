# CD Filesystem Benchmarks

CDBench results comparing eight AmigaOS CD filesystem handlers, all run on
`a4091.device` 42.39 unit 2, 2048-byte sectors, against the same 55 MiB
`CDBENCH_FIXTURE` disc (143 entries). The benchmarks were on purpose run on
WinUAE with an emulated disk drive to put more emphasis on driver bottlenecks
as opposed to measuring CD/DVD drive latencies.

Each result is the median of three runs, each after a fresh boot, with the
handler active before CDBench starts. Repeat runs differed by at most 0.1% in
throughput and 2 µs in latency. ODFileSystem 1.0 beta3 is the 68020 build of
v0.9.0-30, built with GCC 16.2 and measured on 2026-11-02; the other
filesystems were measured on 2026-10-30.

ODFileSystem is the fastest CD filesystem in every single CDBench metric.

Sorted by sequential read throughput;
**bold** = best in column.

## Throughput & directory traversal — higher is better

| Filesystem | Version | Seq read KiB/s (×speed) | Discovery ent/s | ExAll traversal ent/s |
|---|---|--:|--:|--:|
| **ODFileSystem** | 1.0 beta3 | **205 197** (1368×) | **9 770** | **13 064** |
| AllegroCDFS | 3.9 | 203 539 (1357×) | 7 163 | 7 429 |
| CDFileSystem | 47.26 | 186 612 (1244×) | 7 560 | 7 613 |
| BabelCDFS | 1.2 (1993) | 186 525 (1244×) | 9 365 | 9 446 |
| CacheCDFS43 | 43.4 (1995) | 171 111 (1141×) | 5 862 | 5 314 |
| CacheCDFS | 106.1 (1995) | 171 105 (1141×) | 5 882 | 5 319 |
| AmiCDFS | 2.40 | 111 286 (742×) | 5 890 | 5 926 |
| AsimCDFS | 3.10 | 8 231 (55×) | 3 724 | 3 840 |

## Latency (µs) — lower is better

| Filesystem | Rand same-off | Rand stride | Rand seeded | Deep lock | Open-from-lock | Small open+read |
|---|--:|--:|--:|--:|--:|--:|
| **ODFileSystem** | **53** | **161** | **153** | **137** | **159** | **319** |
| AllegroCDFS | 156 | 283 | 156 | 547 | 349 | 1 832 |
| CDFileSystem | 56 | 683 | 605 | 146 | 213 | 1 103 |
| BabelCDFS | 170 | 315 | 172 | 613 | 429 | 2 823 |
| CacheCDFS43 | 59 | 373 | 265 | 993 | 1 076 | 3 274 |
| CacheCDFS | 58 | 373 | 265 | 992 | 1 075 | 3 255 |
| AmiCDFS | 69 | 256 | 185 | 681 | 439 | 2 733 |
| AsimCDFS | 273 | 419 | 410 | 288 | 529 | 1 731 |

## ODFileSystem 0.6.0 to 1.0 beta3

All 68020 builds, measured the same way:

| Test | 0.6.0 | 1.0 beta1 | 1.0 beta2 | 1.0 beta3 | Change since 0.6.0 |
|---|--:|--:|--:|--:|--:|
| Seq read KiB/s | 188 185 | 188 232 | 188 260 | 205 197 | +9.0% |
| Discovery ent/s | 7 328 | 7 529 | 7 679 | 9 770 | +33.3% |
| ExAll traversal ent/s | 11 600 | 12 105 | 12 214 | 13 064 | +12.6% |
| Rand same-off µs | 68 | 68 | 57 | 53 | −22.1% |
| Rand stride µs | 275 | 274 | 196 | 161 | −41.5% |
| Rand seeded µs | 215 | 214 | 191 | 153 | −28.8% |
| Deep lock µs | 319 | 225 | 207 | 137 | −57.1% |
| Open-from-lock µs | 290 | 211 | 193 | 159 | −45.2% |
| Small open+read µs | 644 | 416 | 371 | 319 | −50.5% |

## What stands out

- **ODFileSystem** is the fastest in every metric. Its biggest leads are in
  metadata: ExAll traversal at 13 064 ent/s is ~1.38× the next-best BabelCDFS,
  and small-file open+read at 319 µs is ~3.46× faster than CDFileSystem at
  1 103. Its narrowest leads are in sequential throughput, under 1% ahead of
  AllegroCDFS, and in seeded and same-offset random reads, 3 µs ahead of
  AllegroCDFS and CDFileSystem. In 1.0 beta3, file data goes from the drive
  straight into the program's buffer with SCSI READ(10) commands, a Seek
  starts reading the sectors it lands on, and a path is resolved in one walk
  through the cached directories instead of one lookup per name.
- **AllegroCDFS 3.9** is second in sequential throughput and seeded random
  reads, but its metadata is middling: small-file open+read takes 1 832 µs.
- **CDFileSystem 47.26** is second in deep lock (146 µs) and same-offset reads,
  but has the slowest stride and seeded random reads of the field.
- **CacheCDFS 106.1 vs 43.4** are effectively identical twins on every metric
  — the newer build brought no measurable change on this workload.
- **AsimCDFS is an outlier** at 55× sequential. It mounted with `buffers=2`
  rather than the 192 in its mount entry, which looks like a config artifact
  rather than the handler's true ceiling. It also needs an
  `AsimCDFS_Buffer:` assign, or it stops at a requester.
- **AmiCDFS and BabelCDFS** report a different disc fingerprint (`6639d0c8`)
  than the rest (`b214f3c8`) — both present the directory lowercase-mapped
  (AmiCDFS via its `LC` control), not reading a different disc.
- **BabelCDFS** — once mounted the right way (it is a DOS *handler*, mounted via
  `Handler`/`Startup=<device>/<unit>`, not a block-device filesystem), the 1993
  vintage is startlingly competitive: the 2nd-fastest directory Discovery
  (9 365 ent/s) and ExAll traversal (9 446), behind only ODFileSystem — though
  its small-file open+read (2 823 µs) is among the slowest.

## Measuring Discovery

Discovery is sensitive to when the disc is first mounted. A handler mounted
without `Activate = 1` mounts on first access, so Workbench's disc-insert work
runs during the timed walk. For ODFileSystem 1.0 beta1 that halves the result,
from 7 529 to about 3 900 ent/s. Mount entries for these results set
`Activate = 1`.
