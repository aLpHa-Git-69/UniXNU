/** @file
  UniXNURun() and logging.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#include <Protocol/SerialIo.h>

#ifdef LAVENDER_STAGE1
#include <PiDxe.h>
#include <Library/HobLib.h>

STATIC CONST EFI_GUID mLavenderDiagnosticGuid = {
  0x97C43BF7, 0xD25A, 0x4188, { 0xB0, 0x41, 0x61, 0x66, 0xB4, 0xB6, 0xE8, 0x11 }
};

STATIC
VOID
LavenderMenuDiagnostic (
  IN OUT UNIXNU_UI  *Ui
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  CONST UINT8       *Data;

  Hob = GetFirstGuidHob (&mLavenderDiagnosticGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != 56)) {
    UnicodeSPrint (Ui->Notice, sizeof (Ui->Notice), L"LAVENDER: diagnostic HOB missing");
    return;
  }

  // Wire layout is UINT64 arg, UINT32 status, UINT32 FDT size, two UINT64
  // initrd bounds, then raw arg/SCTLR/PAR. Keep the common package
  // independent of a SoC package.
  Data = GET_GUID_HOB_DATA (Hob);
  UnicodeSPrint (Ui->Notice, sizeof (Ui->Notice), L"H%u raw=0x%lx PA=0x%lx",
                 ReadUnaligned32 ((CONST UINT32 *)(Data + 8)),
                 ReadUnaligned64 ((CONST UINT64 *)(Data + 32)),
                 ReadUnaligned64 ((CONST UINT64 *)Data));
  UnicodeSPrint (Ui->Info, sizeof (Ui->Info), L"M%u FDT%u RD 0x%lx-0x%lx PAR0x%lx",
                 (UINT32)(ReadUnaligned64 ((CONST UINT64 *)(Data + 40)) & 1),
                 ReadUnaligned32 ((CONST UINT32 *)(Data + 12)),
                 ReadUnaligned64 ((CONST UINT64 *)(Data + 16)),
                 ReadUnaligned64 ((CONST UINT64 *)(Data + 24)),
                 ReadUnaligned64 ((CONST UINT64 *)(Data + 48)));
}
#endif

STATIC EFI_SERIAL_IO_PROTOCOL  *mSerial;

VOID
UniXNULogInit (
  VOID
  )
{
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiSerialIoProtocolGuid, NULL, (VOID **)&mSerial))) {
    mSerial = NULL;
  }
}

VOID
EFIAPI
UniXNULog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[256];
  UINTN    Prefix;
  UINTN    Size;

  Prefix = AsciiSPrint (Line, sizeof (Line), "UNIXNU: ");
  VA_START (Args, Format);
  AsciiVSPrint (Line + Prefix, sizeof (Line) - Prefix - 2, Format, Args);
  VA_END (Args);
  AsciiStrCatS (Line, sizeof (Line), "\r\n");

  DEBUG ((PcdGet32 (PcdUniXNUDebugLevel), "%a", Line));
  if (mSerial != NULL) {
    Size = AsciiStrLen (Line);
    mSerial->Write (mSerial, &Size, Line);
  }
}

EFI_STATUS
EFIAPI
UniXNURun (
  IN EFI_HANDLE      ImageHandle,
  IN UNIXNU_HOST  Host
  )
{
  EFI_STATUS       Status;
  UNIXNU_CONFIG        Config;
  UNIXNU_UI            Ui;
  UNIXNU_INJECT_STATS  Stats;
  UINTN            Index;
  BOOLEAN          Countdown;
  UNIXNU_ENTRY         *Entry;
  UNIXNU_ENTRY         BootCopy;

  //
  // A boot manager waits for people; the 5 minute BDS watchdog does not.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  UniXNULogInit ();
  UniXNULog (
    "UniXNU %a starting as %a, firmware %s rev 0x%x",
    UNIXNU_VERSION_STRING,
    (Host == UniXNUHostFirmware) ? "firmware boot option" : "application",
    (gST->FirmwareVendor != NULL) ? gST->FirmwareVendor : L"?",
    gST->FirmwareRevision
    );

  //
  // Storage may not be connected yet, whichever way UniXNU was entered. Started
  // from BDS it is because nothing has connected the controllers; started as a
  // DXE driver it is because the ramdisk BlockIo is dispatched before
  // UniXNURamDiskDxe installs it, so without this the initrd volume never gets a
  // SimpleFileSystem and payload discovery reports that no payload exists.
  //
  UniXNUConnectAll ();

  UniXNUConfigLoad (ImageHandle, &Config);
  UniXNULog (
    "config: %u entries, %u drivers, %u dt patches, %u acpi tables, %u slices, %u kits loaded, %u skipped",
    (UINT32)Config.EntryCount,
    (UINT32)Config.DriverCount,
    (UINT32)Config.PatchCount,
    (UINT32)Config.AcpiCount,
    (UINT32)Config.SliceCount,
    (UINT32)Config.KitsLoaded,
    (UINT32)Config.KitsSkipped
    );

  UniXNUPublishSlices (&Config);

  //
  // Drivers before the UI: an injected driver may provide the display or
  // the input device the menu needs.
  //
  ZeroMem (&Stats, sizeof (Stats));
  UniXNUInjectDrivers (ImageHandle, &Config, &Stats);
  UniXNUInjectAcpi (&Config, &Stats);

  UniXNUUiInit (&Config, &Ui);

  UnicodeSPrint (
    Ui.Info,
    sizeof (Ui.Info),
    L"drivers %u loaded / %u failed    kits %u    acpi %u",
    (UINT32)Stats.DriversLoaded,
    (UINT32)Stats.DriversFailed,
    (UINT32)Config.KitsLoaded,
    (UINT32)Stats.AcpiInstalled
    );

#ifdef LAVENDER_STAGE1
  LavenderMenuDiagnostic (&Ui);
#endif

  Countdown = TRUE;
  for ( ; ;) {
    Index  = UniXNUMenuRun (&Ui, &Config, Countdown);
    Entry  = &Config.Entries[Index];
    BootCopy = *Entry;
    if (Entry->Confirm != NULL) {
      UINTN  Choice;

      if (!UniXNUMenuConfirm (&Ui, Entry, &Choice)) {
        UniXNULog ("menu: entry %u not confirmed", (UINT32)Index);
        UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' cancelled", Entry->Title);
        Countdown = FALSE;
        continue;
      }

      if (Entry->ConfirmCount > 0) {
        //
        // The chosen option's arguments follow the entry's own, on a copy: the entry itself stays as configured.
        //
        UINTN  Length = (Entry->Args != NULL ? StrLen (Entry->Args) : 0) + 1 + StrLen (Entry->ConfirmArgs[Choice]) + 1;

        BootCopy.Args = AllocateZeroPool (Length * sizeof (CHAR16));
        if (BootCopy.Args != NULL) {
          UnicodeSPrint (BootCopy.Args, Length * sizeof (CHAR16), L"%s %s", Entry->Args != NULL ? Entry->Args : L"", Entry->ConfirmArgs[Choice]);
        } else {
          BootCopy.Args = Entry->Args;
        }
      }
    }

    if ((Entry->Type == UniXNUEntryAction) && (Entry->Action == UniXNUActionTools)) {
      UniXNUMenuRunTools (&Ui, &Config);
      Countdown = FALSE;
      continue;
    }

    Status = UniXNUBootEntry (ImageHandle, &Config, &BootCopy);
    if ((Entry->Type == UniXNUEntryAction) && (Entry->Action == UniXNUActionExit)) {
      return EFI_SUCCESS;
    }

    //
    // The payload returned or failed: back to the menu, without the
    // countdown so a broken default cannot loop on a machine with input.
    //
    UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' returned: %r", Entry->Title, Status);
    UniXNULog ("menu: entry %u returned %r", (UINT32)Index, Status);
    gBS->SetWatchdogTimer (0, 0, 0, NULL);
    Countdown = FALSE;
  }
}
