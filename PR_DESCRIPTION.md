# SST: Add hybrid composite dataplane

## Summary

This branch adds a new `hybrid` SST dataplane that combines an ordered list of sub-dataplanes, plus generic plumbing for configuring dataplanes through `IO.SetParameter`.

## Features

- **Hybrid composite dataplane**
  - Instantiates multiple sub-dataplanes with per-instance parameters.
  - Exchanges and multiplexes each sub-dataplane's contact and timestep metadata.
  - Routes each read to one sub-dataplane in configured order.
  - Falls back to the next sub-dataplane when a read cannot be posted.
  - Supports optional `id=` names and `scope=local|remote|any` filters.
  - Adds `shm` shorthand: `shm` expands to a libfabric shm instance with `FABRIC_PROVIDER=shm`, `FABRIC_IFACE=shm`, and `scope=local`.
  - Adds `libfabric` as an alias for the RDMA dataplane.

- **Dataplane configuration through engine parameters**
  - Dataplane environment variables can now be set with `IO.SetParameter(key, value)`.
  - Keys are uppercased and filtered to dataplane-related prefixes.
  - Lookup precedence: per-instance parameters, engine parameters, environment.

- **RDMA/libfabric improvements**
  - Adds optional per-peer route-status reporting to the DP interface.
  - RDMA tolerates per-peer `fi_av_insert()` failures instead of failing the whole stream.
  - Shares `fi_fabric` handles per provider/fabric name, allowing multiple providers in one process.
  - Checks `init_fabric()` failures before starting progress threads or continuing initialization.

- **Robustness and lifetimes**
  - Preserves decoded sub-timestep info until read completion.
  - Keeps sub-dataplane interfaces alive independently of the parent writer stream.
  - Reports invalid scopes, excess sub-dataplane entries, and option-storage failures.

- **Tests and tooling**
  - Adds unit tests for dataplane configuration precedence.
  - Adds an MPI hybrid harness and Slurm script for multi-node validation.
  - Documents hybrid routing, configuration grammar, `shm` shorthand, and selection behavior.

## Example

```sh
SST_HYBRID_DP='shm;libfabric?id=ib&FABRIC_PROVIDER=verbs&FABRIC_IFACE=ib0'
```

This uses libfabric shared memory for same-node peers and verbs for remote peers.

## Testing

- Dataplane configuration unit tests.
- UCX endpoint regression test.
- Local MPI hybrid harness with `ucx;evpath`.
- HPC hybrid harness with `shm` plus tcp/verbs across two nodes.

## Notes

- A read is serviced by one sub-dataplane; there is no striping, duplication, or load balancing.
- Fallback occurs only when a sub-dataplane fails before posting the read.
- Only the names `hybrid`, `SST_HYBRID_DP`, and `SST_HYBRID_NODE_ID` are supported; branch-local aliases were removed.
