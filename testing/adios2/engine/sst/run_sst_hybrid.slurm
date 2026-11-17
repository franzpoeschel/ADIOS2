#!/bin/bash
# SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
#
# SPDX-License-Identifier: Apache-2.0
#
#SBATCH --job-name=sst-hybrid
#SBATCH --nodes=2
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=2
#SBATCH --time=00:10:00
#SBATCH --output=sst-hybrid-%j.out
#SBATCH --error=sst-hybrid-%j.err

set -euo pipefail

BIN=${BIN:-./Test.SST.HybridHarness}
STEPS=${STEPS:-5}
ELEMENTS=${ELEMENTS:-1024}
LOCAL_PROVIDER=${LOCAL_PROVIDER:-shm}
LOCAL_IFACE=${LOCAL_IFACE:-shm}
REMOTE_PROVIDER=${REMOTE_PROVIDER:-verbs}
REMOTE_IFACE=${REMOTE_IFACE:-ib0}
SRUN_MPI_TYPE=${SRUN_MPI_TYPE:-pmix}

CONFIG="libfabric?id=shm&FABRIC_PROVIDER=${LOCAL_PROVIDER}&FABRIC_IFACE=${LOCAL_IFACE};libfabric?id=ib&FABRIC_PROVIDER=${REMOTE_PROVIDER}&FABRIC_IFACE=${REMOTE_IFACE}"

if [[ ! -x "${BIN}" ]]; then
    echo "Executable not found: ${BIN}" >&2
    echo "Set BIN=... when submitting, or build Test.SST.HybridHarness first." >&2
    exit 1
fi

echo "SST hybrid harness configuration:"
echo "  config: ${CONFIG}"
echo "  steps: ${STEPS}, elements: ${ELEMENTS}"
echo "  nodes: ${SLURM_NNODES:-2}, tasks: ${SLURM_NTASKS:-4}"
echo
echo "Node layout:"
srun hostname | sort | uniq -c

srun --mpi="${SRUN_MPI_TYPE}" \
    --nodes="${SLURM_NNODES:-2}" \
    --ntasks="${SLURM_NTASKS:-4}" \
    "${BIN}" --mode split --steps "${STEPS}" --elements "${ELEMENTS}" --config "${CONFIG}"
