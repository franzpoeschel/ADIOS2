# clang-tidy report: hybrid SST dataplane

Command:

```sh
clang-tidy -p build <file>
```

Files checked:

- `source/adios2/toolkit/sst/dp/hybrid_dp.c`
- `source/adios2/toolkit/sst/dp_config.c`
- `source/adios2/toolkit/sst/dp/rdma_dp.c`
- `testing/adios2/engine/sst/SstHybridHarness.cpp`
- `testing/adios2/engine/sst/TestSstDataplaneConfig.cpp`

## Modifiable static state

| File | Variable | Resolution |
|---|---|---|
| `hybrid_dp.c` | `HybridRegistry` | Made `const`. |
| `hybrid_dp.c` | `HybridSubDPInfoList` | Made `const`. |
| `hybrid_dp.c` | `HybridReaderContactList` | Made `const`. |
| `hybrid_dp.c` | `HybridReaderContactStructs` | Made `const`. |
| `hybrid_dp.c` | `HybridWriterContactList` | Made `const`. |
| `hybrid_dp.c` | `HybridWriterContactStructs` | Made `const`. |
| `hybrid_dp.c` | `HybridTimestepInfoList` | Made `const`. |
| `hybrid_dp.c` | `HybridTimestepInfoStructs` | Made `const`. |
| `hybrid_dp.c` | `HybridDPInterface` | Made `const`; loader returns a casted pointer. |
| `rdma_dp.c` | `GlobalFabrics` fabric cache | Removed; each stream now owns and closes its `fi_fabric`. |

No modifiable static state remains in the newly added code.

## Remaining warnings

All warnings in the new hybrid/config/harness/test code were resolved.

`rdma_dp.c` still reports pre-existing warnings from master: init-variable checks, narrowing conversions, magic numbers, TODO format, and non-const pre-existing globals such as mutexes, format tables, and `RdmaDPInterface`. Those are not introduced by this branch and were left unchanged.

## Verification

- `sst` and core libraries build successfully.
- `Test.Engine.SST.SstDataplaneConfig.Serial`: 3/3 passed.
- `Test.SST.UCXEndpoints`: passed.
- Hybrid MPI harness with `shm;evpath`: 2 steps, zero failures.
