# CD Filesystem Benchmarks

CDBench results comparing seven AmigaOS CD filesystem handlers, all run on
`a4091.device` unit 2, 2048-byte sectors, against the same 55 MiB
`CDBENCH_FIXTURE` disc (143 entries). The benchmarks were on purpose run on
WinUAE with an emulated disk drive to put more emphasis on driver bottlenecks
as opposed to measuring CD/DVD drive latencies. 

Sorted by sequential read throughput;
**bold** = best in column.

## Throughput & directory traversal — higher is better

| Filesystem | Version | Seq read KiB/s (×speed) | Discovery ent/s | ExAll traversal ent/s |
|---|---|--:|--:|--:|
| CDFileSystem | 47.26 | 155 559 (1037×) | 7 070 | 7 479 |
| AllegroCDFS | 3.4* | **217 287** (1449×) | 6 785 | 6 913 |
| CacheCDFS43 | 43.4 (1995) | 179 327 (1196×) | 5 829 | 5 171 |
| CacheCDFS | 106.1 (1995) | 178 927 (1193×) | 5 885 | 5 299 |
| AmiCDFS | 2.40 | 113 432 (756×) | 5 888 | 5 922 |
| AsimCDFS | 3.10 | 8 157 (54×) | 3 683 | 3 765 |
| **ODFileSystem** | 0.6.0 | 197 673 (1318×) | **7 520** | **11 657** |

## Latency (µs) — lower is better

| Filesystem | Rand same-off | Rand stride | Rand seeded | Deep lock | Open-from-lock | Small open+read |
|---|--:|--:|--:|--:|--:|--:|
| CDFileSystem | 59 | 471 | 341 | **226** | **241** | 1 090 |
| AllegroCDFS | 148 | 272 | **148** | 617 | 383 | 1 897 |
| CacheCDFS43 | **58** | 364 | 255 | 949 | 1 098 | 3 260 |
| CacheCDFS | **58** | 365 | 256 | 930 | 1 080 | 3 283 |
| AmiCDFS | 69 | **247** | 177 | 669 | 437 | 2 742 |
| AsimCDFS | 275 | 415 | 405 | 269 | 491 | 1 719 |
| **ODFileSystem** | 68 | 267 | 204 | 328 | 297 | **655** |


## What stands out

- **ODFileSystem** wins the metadata game decisively: fastest discovery,
  ~1.6× the ExAll traversal rate of anything else (11 657 ent/s), and by far
  the fastest small-file open+read (655 µs — next best is CDFileSystem at
  1 090). Its sequential throughput (1318×) is second only to
  AllegroCDFS, 27% ahead of the shipping CDFileSystem 47.26.
- **AllegroCDFS's** chart-topping 1449× sequential number is worth a caveat:
  Great streaming, but the worst small-open latency after the CacheCDFS pair.
- **CacheCDFS 106.1 vs 43.4** are effectively identical twins on every metric
  — the newer build brought no measurable change on this workload.
- **AsimCDFS is an outlier** at 54× sequential. Note it mounted with
  `buffers=2` (not the 192 from its mount entry) — this looks like a config
  artifact rather than the handler's true ceiling. Worth a re-run with buffers
  forced up before trusting that row.
- **AmiCDFS** reports a different disc fingerprint (`4d0207c8`) than everyone
  else (`ba34ca48`) — it's presenting the directory differently (its `LC`
  lowercase-mapping control), not reading a different disc.
- **BabelCDFS produced no result** — consistent with it not mounting (1993
  vintage / unverified DosType).

\* AllegroCDFS reports `$VER: AllegroCDFS V3.4` (fs_version 3.3).

