# Lavender integration branch

This branch is consumed by <https://github.com/aLpHa-Git-69/UniXNU-lavender>,
the Redmi Note 7 / SDM660 board workspace.

- Firmware: <https://github.com/aLpHa-Git-69/uefi-lavender>
- Loader: <https://github.com/aLpHa-Git-69/DarwinBoot>
- Darwin 22 kernel patches: <https://github.com/aLpHa-Git-69/ios-lavender>

`drivers/DarwinBoot` is pinned to the loader fork through the submodule link.
The loader's optional typed ADT policy selects Darwin 22 arguments, 16 KiB
pages and the Gold boot CPU for the initial iOS 16.5 integration.

The 16 KiB loader uses 32 MiB-aligned virtual/physical bootstrap banks. The
native linked header stays at its offset within that bank; segment copies,
boot arguments and ADT placement use the same base. This fixes the first
handoff test's 16 MiB KVA/PA phase error.

For the board build, `LAVENDER_STAGE1` keeps the fallback menu visible and
shows the SEC handoff diagnostic HOB in the menu footer. Mu-Silicium's
entry-argument HOB is consumed by the shared read-only RAM filesystem driver;
the platform owns the actual FDT/initrd reservations.

Hardware Stage 1 and Gold/RAM diagnostic probes passed in the board workspace.
Kernel boot and the larger OS payload require their own firmware/image and
device checks. Source and test evidence are recorded in the board repository.
