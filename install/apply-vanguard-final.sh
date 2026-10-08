#!/bin/bash
# Retired: this script could leave the VM unbootable (it deleted the NVRAM,
# stripped qemu:commandline, or injected a fake IOMMU table). The safe version
# of the whole job is apply-vanguard.sh, next to this file.
exec "$(dirname "${BASH_SOURCE[0]}")/apply-vanguard.sh" "$@"
