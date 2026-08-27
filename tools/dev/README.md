# Working on the guest agent

The agent is built inside the VM - C++/WinRT and the WGC interop headers are
not usable from mingw, so it cannot be cross-compiled from Linux.

    ./gssh 'cmd'          run a command in the guest
    ./gsync               push include/ and guest/ into C:\vypr
    ./gssh 'C:\vypr\build-agent.bat'    compile it there

`build-agent.bat` belongs in `C:\vypr\` on the guest. It invokes `cl` directly
rather than through CMake: for a first compile the error output is easier to
read, and there is no generator to argue with.

## Restarting the agent

**Stop `vyprd` and let the agent exit on its own.** Do not `taskkill` it: the
IVSHMEM driver does not clean up after a TerminateProcess, and the process
becomes an unkillable zombie still holding the shared-memory device. Nothing
can map the region after that, `Disable-PnpDevice` on it fails, and only a
guest reboot clears it. This has cost two reboots.

    pkill -x vyprd        # the agent exits within a few seconds
    ./gssh 'schtasks /end /tn vypr-agent'    # if it is being stubborn

`schtasks /run` refuses to start a second instance while it thinks one is
running (0x800710E0), so `/end` first.

## Running it

The agent needs the interactive desktop session - an SSH session has no
desktop, so `GraphicsCaptureSession::IsSupported()` is false there and nothing
can be captured. It is launched through a scheduled task with `/it`, and with
`/rl highest` because a game running elevated will not accept injected input
from a process at lower integrity.
