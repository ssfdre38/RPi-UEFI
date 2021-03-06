/** @file
*
*  Copyright (c) 2011-2014, ARM Limited. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include "BdsInternal.h"

#include <Library/ArmBdsHelperLib.h>
#include <Library/NetLib.h>

#include <Protocol/Bds.h>
#include <Protocol/UsbIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/Dhcp4.h>
#include <Protocol/Mtftp4.h>

#define IS_DEVICE_PATH_NODE(node,type,subtype) (((node)->Type == (type)) && ((node)->SubType == (subtype)))

/*
   Constant strings and define related to the message indicating the amount of
   progress in the dowloading of a TFTP file.
*/

// Frame for the progression slider
STATIC CONST CHAR16 mTftpProgressFrame[] = L"[                                        ]";

// Number of steps in the progression slider
#define TFTP_PROGRESS_SLIDER_STEPS  ((sizeof (mTftpProgressFrame) / sizeof (CHAR16)) - 3)

// Size in number of characters plus one (final zero) of the message to
// indicate the progress of a tftp download. The format is "[(progress slider:
// 40 characters)] (nb of KBytes downloaded so far: 7 characters) Kb". There
// are thus the number of characters in mTftpProgressFrame[] plus 11 characters
// (2 // spaces, "Kb" and seven characters for the number of KBytes).
#define TFTP_PROGRESS_MESSAGE_SIZE  ((sizeof (mTftpProgressFrame) / sizeof (CHAR16)) + 12)

// String to delete the tftp progress message to be able to update it :
// (TFTP_PROGRESS_MESSAGE_SIZE-1) '\b'
STATIC CONST CHAR16 mTftpProgressDelete[] = L"\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b";


// Extract the FilePath from the Device Path
CHAR16*
BdsExtractFilePathFromDevicePath (
  IN  CONST CHAR16    *StrDevicePath,
  IN  UINTN           NumberDevicePathNode
  )
{
  UINTN       Node;
  CHAR16      *Str;

  Str = (CHAR16*)StrDevicePath;
  Node = 0;
  while ((Str != NULL) && (*Str != L'\0') && (Node < NumberDevicePathNode)) {
    if ((*Str == L'/') || (*Str == L'\\')) {
        Node++;
    }
    Str++;
  }

  if (*Str == L'\0') {
    return NULL;
  } else {
    return Str;
  }
}

BOOLEAN
BdsFileSystemSupport (
  IN EFI_DEVICE_PATH *DevicePath,
  IN EFI_HANDLE Handle,
  IN EFI_DEVICE_PATH *RemainingDevicePath
  )
{
  EFI_STATUS  Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     *FsProtocol;

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FsProtocol);

  return (!EFI_ERROR (Status) && IS_DEVICE_PATH_NODE (RemainingDevicePath, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP));
}

EFI_STATUS
BdsFileSystemLoadImage (
  IN     EFI_DEVICE_PATH *DevicePath,
  IN     EFI_HANDLE Handle,
  IN     EFI_DEVICE_PATH *RemainingDevicePath,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN OUT EFI_PHYSICAL_ADDRESS* Image,
  OUT    UINTN                 *ImageSize
  )
{
  FILEPATH_DEVICE_PATH*             FilePathDevicePath;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     *FsProtocol;
  EFI_FILE_PROTOCOL                   *Fs;
  EFI_STATUS Status;
  EFI_FILE_INFO       *FileInfo;
  EFI_FILE_PROTOCOL   *File;
  UINTN               Size;

  ASSERT (IS_DEVICE_PATH_NODE (RemainingDevicePath, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP));

  FilePathDevicePath = (FILEPATH_DEVICE_PATH*)RemainingDevicePath;

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FsProtocol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Try to Open the volume and get root directory
  Status = FsProtocol->OpenVolume (FsProtocol, &Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  File = NULL;
  Status = Fs->Open (Fs, &File, FilePathDevicePath->PathName, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Size = 0;
  File->GetInfo (File, &gEfiFileInfoGuid, &Size, NULL);
  FileInfo = AllocatePool (Size);
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, FileInfo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Get the file size
  Size = FileInfo->FileSize;
  if (ImageSize) {
    *ImageSize = Size;
  }
  FreePool (FileInfo);

  Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(Size), Image);
  // Try to allocate in any pages if failed to allocate memory at the defined location
  if ((Status == EFI_OUT_OF_RESOURCES) && (Type != AllocateAnyPages)) {
    Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesCode, EFI_SIZE_TO_PAGES(Size), Image);
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Read (File, &Size, (VOID*)(UINTN)(*Image));
  }

  return Status;
}

BOOLEAN
BdsMemoryMapSupport (
  IN EFI_DEVICE_PATH *DevicePath,
  IN EFI_HANDLE Handle,
  IN EFI_DEVICE_PATH *RemainingDevicePath
  )
{
  return IS_DEVICE_PATH_NODE (DevicePath, HARDWARE_DEVICE_PATH, HW_MEMMAP_DP) ||
         IS_DEVICE_PATH_NODE (RemainingDevicePath, HARDWARE_DEVICE_PATH, HW_MEMMAP_DP);
}

EFI_STATUS
BdsMemoryMapLoadImage (
  IN     EFI_DEVICE_PATH *DevicePath,
  IN     EFI_HANDLE Handle,
  IN     EFI_DEVICE_PATH *RemainingDevicePath,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN OUT EFI_PHYSICAL_ADDRESS* Image,
  OUT    UINTN                 *ImageSize
  )
{
  EFI_STATUS            Status;
  MEMMAP_DEVICE_PATH*   MemMapPathDevicePath;
  UINTN                 Size;

  if (IS_DEVICE_PATH_NODE (RemainingDevicePath, HARDWARE_DEVICE_PATH, HW_MEMMAP_DP)) {
    MemMapPathDevicePath = (MEMMAP_DEVICE_PATH*)RemainingDevicePath;
  } else {
    ASSERT (IS_DEVICE_PATH_NODE (DevicePath, HARDWARE_DEVICE_PATH, HW_MEMMAP_DP));
    MemMapPathDevicePath = (MEMMAP_DEVICE_PATH*)DevicePath;
  }

  Size = MemMapPathDevicePath->EndingAddress - MemMapPathDevicePath->StartingAddress;
  if (Size == 0) {
      return EFI_INVALID_PARAMETER;
  }

  Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(Size), Image);
  // Try to allocate in any pages if failed to allocate memory at the defined location
  if ((Status == EFI_OUT_OF_RESOURCES) && (Type != AllocateAnyPages)) {
    Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesCode, EFI_SIZE_TO_PAGES(Size), Image);
  }
  if (!EFI_ERROR (Status)) {
    CopyMem ((VOID*)(UINTN)(*Image), (CONST VOID*)(UINTN)MemMapPathDevicePath->StartingAddress, Size);

    if (ImageSize != NULL) {
        *ImageSize = Size;
    }
  }

  return Status;
}

BOOLEAN
BdsFirmwareVolumeSupport (
  IN EFI_DEVICE_PATH *DevicePath,
  IN EFI_HANDLE Handle,
  IN EFI_DEVICE_PATH *RemainingDevicePath
  )
{
  return IS_DEVICE_PATH_NODE (RemainingDevicePath, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP);
}

EFI_STATUS
BdsFirmwareVolumeLoadImage (
  IN     EFI_DEVICE_PATH *DevicePath,
  IN     EFI_HANDLE Handle,
  IN     EFI_DEVICE_PATH *RemainingDevicePath,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN OUT EFI_PHYSICAL_ADDRESS* Image,
  OUT    UINTN                 *ImageSize
  )
{
  EFI_STATUS            Status;
  EFI_FIRMWARE_VOLUME2_PROTOCOL     *FwVol;
  EFI_GUID                          *FvNameGuid;
  EFI_SECTION_TYPE                  SectionType;
  EFI_FV_FILETYPE                   FvType;
  EFI_FV_FILE_ATTRIBUTES            Attrib;
  UINT32                            AuthenticationStatus;
  VOID* ImageBuffer;

  ASSERT (IS_DEVICE_PATH_NODE (RemainingDevicePath, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP));

  Status = gBS->HandleProtocol (Handle, &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&FwVol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FvNameGuid = EfiGetNameGuidFromFwVolDevicePathNode ((CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)RemainingDevicePath);
  if (FvNameGuid == NULL) {
    Status = EFI_INVALID_PARAMETER;
  }

  SectionType = EFI_SECTION_PE32;
  AuthenticationStatus = 0;
  //Note: ReadSection at the opposite of ReadFile does not allow to pass ImageBuffer == NULL to get the size of the file.
  ImageBuffer = NULL;
  Status = FwVol->ReadSection (
                    FwVol,
                    FvNameGuid,
                    SectionType,
                    0,
                    &ImageBuffer,
                    ImageSize,
                    &AuthenticationStatus
                    );
  if (!EFI_ERROR (Status)) {
#if 0
    // In case the buffer has some address requirements, we must copy the buffer to a buffer following the requirements
    if (Type != AllocateAnyPages) {
      Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(*ImageSize),Image);
      if (!EFI_ERROR (Status)) {
        CopyMem ((VOID*)(UINTN)(*Image), ImageBuffer, *ImageSize);
        FreePool (ImageBuffer);
      }
    }
#else
    // We must copy the buffer into a page allocations. Otherwise, the caller could call gBS->FreePages() on the pool allocation
    Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(*ImageSize), Image);
    // Try to allocate in any pages if failed to allocate memory at the defined location
    if ((Status == EFI_OUT_OF_RESOURCES) && (Type != AllocateAnyPages)) {
      Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesCode, EFI_SIZE_TO_PAGES(*ImageSize), Image);
    }
    if (!EFI_ERROR (Status)) {
      CopyMem ((VOID*)(UINTN)(*Image), ImageBuffer, *ImageSize);
      FreePool (ImageBuffer);
    }
#endif
  } else {
    // Try a raw file, since a PE32 SECTION does not exist
    Status = FwVol->ReadFile (
                        FwVol,
                        FvNameGuid,
                        NULL,
                        ImageSize,
                        &FvType,
                        &Attrib,
                        &AuthenticationStatus
                        );
    if (!EFI_ERROR (Status)) {
      Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(*ImageSize), Image);
      // Try to allocate in any pages if failed to allocate memory at the defined location
      if ((Status == EFI_OUT_OF_RESOURCES) && (Type != AllocateAnyPages)) {
        Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesCode, EFI_SIZE_TO_PAGES(*ImageSize), Image);
      }
      if (!EFI_ERROR (Status)) {
        Status = FwVol->ReadFile (
                                FwVol,
                                FvNameGuid,
                                (VOID*)(UINTN)(*Image),
                                ImageSize,
                                &FvType,
                                &Attrib,
                                &AuthenticationStatus
                                );
      }
    }
  }
  return Status;
}

BOOLEAN
BdsPxeSupport (
  IN EFI_DEVICE_PATH*           DevicePath,
  IN EFI_HANDLE                 Handle,
  IN EFI_DEVICE_PATH*           RemainingDevicePath
  )
{
  EFI_STATUS                  Status;
  EFI_PXE_BASE_CODE_PROTOCOL* PxeBcProtocol;

  if (!IsDevicePathEnd (RemainingDevicePath)) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiPxeBaseCodeProtocolGuid, (VOID **)&PxeBcProtocol);
  if (EFI_ERROR (Status)) {
    return FALSE;
  } else {
    return TRUE;
  }
}

EFI_STATUS
BdsPxeLoadImage (
  IN     EFI_DEVICE_PATH*       DevicePath,
  IN     EFI_HANDLE             Handle,
  IN     EFI_DEVICE_PATH*       RemainingDevicePath,
  IN     EFI_ALLOCATE_TYPE      Type,
  IN OUT EFI_PHYSICAL_ADDRESS   *Image,
  OUT    UINTN                  *ImageSize
  )
{
  EFI_STATUS              Status;
  EFI_LOAD_FILE_PROTOCOL  *LoadFileProtocol;
  UINTN                   BufferSize;
  EFI_PXE_BASE_CODE_PROTOCOL *Pxe;

  // Get Load File Protocol attached to the PXE protocol
  Status = gBS->HandleProtocol (Handle, &gEfiLoadFileProtocolGuid, (VOID **)&LoadFileProtocol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LoadFileProtocol->LoadFile (LoadFileProtocol, DevicePath, TRUE, &BufferSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Status = gBS->AllocatePages (Type, EfiBootServicesCode, EFI_SIZE_TO_PAGES(BufferSize), Image);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = LoadFileProtocol->LoadFile (LoadFileProtocol, DevicePath, TRUE, &BufferSize, (VOID*)(UINTN)(*Image));
    if (!EFI_ERROR (Status) && (ImageSize != NULL)) {
      *ImageSize = BufferSize;
    }
  }

  if (Status == EFI_ALREADY_STARTED) {
    Status = gBS->LocateProtocol (&gEfiPxeBaseCodeProtocolGuid, NULL, (VOID **)&Pxe);
    if (!EFI_ERROR(Status)) {
      // If PXE is already started, we stop it
      Pxe->Stop (Pxe);
      // And we try again
      return BdsPxeLoadImage (DevicePath, Handle, RemainingDevicePath, Type, Image, ImageSize);
    }
  }
  return Status;
}

BOOLEAN
BdsTftpSupport (
  IN EFI_DEVICE_PATH  *DevicePath,
  IN EFI_HANDLE       Handle,
  IN EFI_DEVICE_PATH  *RemainingDevicePath
  )
{
  EFI_STATUS       Status;
  EFI_DEVICE_PATH  *NextDevicePath;
  VOID             *Interface;

  // Validate the Remaining Device Path
  if (IsDevicePathEnd (RemainingDevicePath)) {
    return FALSE;
  }
  if (!IS_DEVICE_PATH_NODE (RemainingDevicePath, MESSAGING_DEVICE_PATH, MSG_IPv4_DP) &&
      !IS_DEVICE_PATH_NODE (RemainingDevicePath, MESSAGING_DEVICE_PATH, MSG_IPv6_DP)) {
    return FALSE;
  }
  NextDevicePath = NextDevicePathNode (RemainingDevicePath);
  if (IsDevicePathEnd (NextDevicePath)) {
    return FALSE;
  }
  if (!IS_DEVICE_PATH_NODE (NextDevicePath, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP)) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (
                  Handle, &gEfiDevicePathProtocolGuid,
                  &Interface
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  //
  // Check that the controller (identified by its handle "Handle") supports the
  // MTFTPv4 Service Binding Protocol. If it does, it means that it supports the
  // EFI MTFTPv4 Protocol needed to download the image through TFTP.
  //
  Status = gBS->HandleProtocol (
                  Handle, &gEfiMtftp4ServiceBindingProtocolGuid,
                  &Interface
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  return TRUE;
}

/**
  Worker function that get the size in numbers of bytes of a file from a TFTP
  server before to download the file.

  @param[in]   Mtftp4    MTFTP4 protocol interface
  @param[in]   FilePath  Path of the file, Ascii encoded
  @param[out]  FileSize  Address where to store the file size in number of
                         bytes.

  @retval  EFI_SUCCESS   The size of the file was returned.
  @retval  !EFI_SUCCESS  The size of the file was not returned.

**/
STATIC
EFI_STATUS
Mtftp4GetFileSize (
  IN  EFI_MTFTP4_PROTOCOL  *Mtftp4,
  IN  CHAR8                *FilePath,
  OUT UINT64               *FileSize
  )
{
  EFI_STATUS         Status;
  EFI_MTFTP4_OPTION  ReqOpt[1];
  EFI_MTFTP4_PACKET  *Packet;
  UINT32             PktLen;
  EFI_MTFTP4_OPTION  *TableOfOptions;
  EFI_MTFTP4_OPTION  *Option;
  UINT32             OptCnt;
  UINT8              OptBuf[128];

  ReqOpt[0].OptionStr = (UINT8*)"tsize";
  OptBuf[0] = '0';
  OptBuf[1] = 0;
  ReqOpt[0].ValueStr = OptBuf;

  Status = Mtftp4->GetInfo (
             Mtftp4,
             NULL,
             (UINT8*)FilePath,
             NULL,
             1,
             ReqOpt,
             &PktLen,
             &Packet
             );

  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = Mtftp4->ParseOptions (
                     Mtftp4,
                     PktLen,
                     Packet,
                     (UINT32 *) &OptCnt,
                     &TableOfOptions
                     );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Option = TableOfOptions;
  while (OptCnt != 0) {
    if (AsciiStrnCmp ((CHAR8 *)Option->OptionStr, "tsize", 5) == 0) {
      *FileSize = AsciiStrDecimalToUint64 ((CHAR8 *)Option->ValueStr);
      break;
    }
    OptCnt--;
    Option++;
  }
  FreePool (TableOfOptions);

  if (OptCnt == 0) {
    Status = EFI_UNSUPPORTED;
  }

Error :

  return Status;
}

/**
  Update the progress of a file download
  This procedure is called each time a new TFTP packet is received.

  @param[in]  This       MTFTP4 protocol interface
  @param[in]  Token      Parameters for the download of the file
  @param[in]  PacketLen  Length of the packet
  @param[in]  Packet     Address of the packet

  @retval  EFI_SUCCESS  All packets are accepted.

**/
STATIC
EFI_STATUS
Mtftp4CheckPacket (
  IN EFI_MTFTP4_PROTOCOL  *This,
  IN EFI_MTFTP4_TOKEN     *Token,
  IN UINT16               PacketLen,
  IN EFI_MTFTP4_PACKET    *Packet
  )
{
  BDS_TFTP_CONTEXT  *Context;
  CHAR16            Progress[TFTP_PROGRESS_MESSAGE_SIZE];
  UINT64            NbOfKb;
  UINTN             Index;
  UINTN             LastStep;
  UINTN             Step;
  UINT64            LastNbOf50Kb;
  UINT64            NbOf50Kb;

  if ((NTOHS (Packet->OpCode)) == EFI_MTFTP4_OPCODE_DATA) {
    Context = (BDS_TFTP_CONTEXT*)Token->Context;

    if (Context->DownloadedNbOfBytes == 0) {
      if (Context->FileSize > 0) {
        Print (L"%s       0 Kb", mTftpProgressFrame);
      } else {
        Print (L"    0 Kb");
      }
    }

    //
    // The data is the packet are prepended with two UINT16 :
    // . OpCode = EFI_MTFTP4_OPCODE_DATA
    // . Block  = the number of this block of data
    //
    Context->DownloadedNbOfBytes += PacketLen - sizeof (Packet->OpCode) - sizeof (Packet->Data.Block);
    NbOfKb = Context->DownloadedNbOfBytes / 1024;

    Progress[0] = L'\0';
    if (Context->FileSize > 0) {
      LastStep  = (Context->LastReportedNbOfBytes * TFTP_PROGRESS_SLIDER_STEPS) / Context->FileSize;
      Step      = (Context->DownloadedNbOfBytes   * TFTP_PROGRESS_SLIDER_STEPS) / Context->FileSize;
      if (Step > LastStep) {
        Print (mTftpProgressDelete);
        StrCpy (Progress, mTftpProgressFrame);
        for (Index = 1; Index < Step; Index++) {
          Progress[Index] = L'=';
        }
        Progress[Step] = L'>';

        UnicodeSPrint (
          Progress + (sizeof (mTftpProgressFrame) / sizeof (CHAR16)) - 1,
          sizeof (Progress) - sizeof (mTftpProgressFrame),
          L" %7d Kb",
          NbOfKb
          );
        Context->LastReportedNbOfBytes = Context->DownloadedNbOfBytes;
      }
    } else {
      //
      // Case when we do not know the size of the final file.
      // We print the updated size every 50KB of downloaded data
      //
      LastNbOf50Kb = Context->LastReportedNbOfBytes / (50*1024);
      NbOf50Kb     = Context->DownloadedNbOfBytes   / (50*1024);
      if (NbOf50Kb > LastNbOf50Kb) {
        Print (L"\b\b\b\b\b\b\b\b\b\b");
        UnicodeSPrint (Progress, sizeof (Progress), L"%7d Kb", NbOfKb);
        Context->LastReportedNbOfBytes = Context->DownloadedNbOfBytes;
      }
    }
    if (Progress[0] != L'\0') {
      Print (L"%s", Progress);
    }
  }

  return EFI_SUCCESS;
}

/**
  Download an image from a TFTP server

  @param[in]   DevicePath           Device path of the TFTP boot option
  @param[in]   ControllerHandle     Handle of the network controller
  @param[in]   RemainingDevicePath  Device path of the TFTP boot option but
                                    the first node that identifies the network controller
  @param[in]   Type                 Type to allocate memory pages
  @param[out]  Image                Address of the bufer where the image is stored in
                                    case of success
  @param[out]  ImageSize            Size in number of bytes of the i;age in case of
                                    success

  @retval  EFI_SUCCESS   The image was returned.
  @retval  !EFI_SUCCESS  Something went wrong.

**/
EFI_STATUS
BdsTftpLoadImage (
  IN     EFI_DEVICE_PATH*       DevicePath,
  IN     EFI_HANDLE             ControllerHandle,
  IN     EFI_DEVICE_PATH*       RemainingDevicePath,
  IN     EFI_ALLOCATE_TYPE      Type,
  IN OUT EFI_PHYSICAL_ADDRESS   *Image,
  OUT    UINTN                  *ImageSize
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              Dhcp4ChildHandle;
  EFI_DHCP4_PROTOCOL      *Dhcp4;
  BOOLEAN                 Dhcp4ToStop;
  EFI_HANDLE              Mtftp4ChildHandle;
  EFI_MTFTP4_PROTOCOL     *Mtftp4;
  EFI_DHCP4_CONFIG_DATA   Dhcp4CfgData;
  EFI_DHCP4_MODE_DATA     Dhcp4Mode;
  EFI_MTFTP4_CONFIG_DATA  Mtftp4CfgData;
  IPv4_DEVICE_PATH        *IPv4DevicePathNode;
  FILEPATH_DEVICE_PATH    *FilePathDevicePathNode;
  CHAR8                   *AsciiFilePath;
  EFI_MTFTP4_TOKEN        Mtftp4Token;
  UINT64                  FileSize;
  UINT64                  TftpBufferSize;
  BDS_TFTP_CONTEXT        *TftpContext;

  ASSERT(IS_DEVICE_PATH_NODE (RemainingDevicePath, MESSAGING_DEVICE_PATH, MSG_IPv4_DP));
  IPv4DevicePathNode = (IPv4_DEVICE_PATH*)RemainingDevicePath;

  Dhcp4ChildHandle  = NULL;
  Dhcp4             = NULL;
  Dhcp4ToStop       = FALSE;
  Mtftp4ChildHandle = NULL;
  Mtftp4            = NULL;
  AsciiFilePath     = NULL;
  TftpContext       = NULL;

  if (!IPv4DevicePathNode->StaticIpAddress) {
    //
    // Using the DHCP4 Service Binding Protocol, create a child handle of the DHCP4 service and
    // install the DHCP4 protocol on it. Then, open the DHCP protocol.
    //
    Status = NetLibCreateServiceChild (
               ControllerHandle,
               gImageHandle,
               &gEfiDhcp4ServiceBindingProtocolGuid,
               &Dhcp4ChildHandle
               );
    if (!EFI_ERROR (Status)) {
      Status = gBS->OpenProtocol (
                      Dhcp4ChildHandle,
                      &gEfiDhcp4ProtocolGuid,
                      (VOID **) &Dhcp4,
                      gImageHandle,
                      ControllerHandle,
                      EFI_OPEN_PROTOCOL_BY_DRIVER
                      );
    }
    if (EFI_ERROR (Status)) {
      Print (L"Unable to open DHCP4 protocol\n");
      goto Error;
    }
  }

  //
  // Using the MTFTP4 Service Binding Protocol, create a child handle of the MTFTP4 service and
  // install the MTFTP4 protocol on it. Then, open the MTFTP4 protocol.
  //
  Status = NetLibCreateServiceChild (
             ControllerHandle,
             gImageHandle,
             &gEfiMtftp4ServiceBindingProtocolGuid,
             &Mtftp4ChildHandle
             );
  if (!EFI_ERROR (Status)) {
    Status = gBS->OpenProtocol (
                    Mtftp4ChildHandle,
                    &gEfiMtftp4ProtocolGuid,
                    (VOID **) &Mtftp4,
                    gImageHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_BY_DRIVER
                    );
  }
  if (EFI_ERROR (Status)) {
    Print (L"Unable to open MTFTP4 protocol\n");
    goto Error;
  }

  if (!IPv4DevicePathNode->StaticIpAddress) {
    //
    // Configure the DHCP4, all default settings. It is acceptable for the configuration to
    // fail if the return code is equal to EFI_ACCESS_DENIED which means that the configuration
    // has been done by another instance of the DHCP4 protocol or that the DHCP configuration
    // process has been started but is not completed yet.
    //
    ZeroMem (&Dhcp4CfgData, sizeof (EFI_DHCP4_CONFIG_DATA));
    Status = Dhcp4->Configure (Dhcp4, &Dhcp4CfgData);
    if (EFI_ERROR (Status)) {
      if (Status != EFI_ACCESS_DENIED) {
        Print (L"Error while configuring the DHCP4 protocol\n");
        goto Error;
      }
    }

    //
    // Start the DHCP configuration. This may have already been done thus do not leave in error
    // if the return code is EFI_ALREADY_STARTED.
    //
    Status = Dhcp4->Start (Dhcp4, NULL);
    if (EFI_ERROR (Status)) {
      if (Status != EFI_ALREADY_STARTED) {
        Print (L"DHCP configuration failed\n");
        goto Error;
      }
    } else {
      Dhcp4ToStop = TRUE;
    }

    Status = Dhcp4->GetModeData (Dhcp4, &Dhcp4Mode);
    if (EFI_ERROR (Status)) {
      goto Error;
    }

    if (Dhcp4Mode.State != Dhcp4Bound) {
      Status = EFI_TIMEOUT;
      Print (L"DHCP configuration failed\n");
      goto Error;
    }
  }

  //
  // Configure the TFTP4 protocol
  //

  ZeroMem (&Mtftp4CfgData, sizeof (EFI_MTFTP4_CONFIG_DATA));
  Mtftp4CfgData.UseDefaultSetting = FALSE;
  Mtftp4CfgData.TimeoutValue      = 4;
  Mtftp4CfgData.TryCount          = 6;

  if (IPv4DevicePathNode->StaticIpAddress) {
    CopyMem (&Mtftp4CfgData.StationIp , &IPv4DevicePathNode->LocalIpAddress, sizeof (EFI_IPv4_ADDRESS));
    CopyMem (&Mtftp4CfgData.SubnetMask, &IPv4DevicePathNode->SubnetMask, sizeof (EFI_IPv4_ADDRESS));
    CopyMem (&Mtftp4CfgData.GatewayIp , &IPv4DevicePathNode->GatewayIpAddress, sizeof (EFI_IPv4_ADDRESS));
  } else {
    CopyMem (&Mtftp4CfgData.StationIp , &Dhcp4Mode.ClientAddress, sizeof (EFI_IPv4_ADDRESS));
    CopyMem (&Mtftp4CfgData.SubnetMask, &Dhcp4Mode.SubnetMask   , sizeof (EFI_IPv4_ADDRESS));
    CopyMem (&Mtftp4CfgData.GatewayIp , &Dhcp4Mode.RouterAddress, sizeof (EFI_IPv4_ADDRESS));
  }

  CopyMem (&Mtftp4CfgData.ServerIp  , &IPv4DevicePathNode->RemoteIpAddress, sizeof (EFI_IPv4_ADDRESS));

  Status = Mtftp4->Configure (Mtftp4, &Mtftp4CfgData);
  if (EFI_ERROR (Status)) {
    Print (L"Error while configuring the MTFTP4 protocol\n");
    goto Error;
  }

  //
  // Convert the Unicode path of the file to Ascii
  //

  FilePathDevicePathNode = (FILEPATH_DEVICE_PATH*)(IPv4DevicePathNode + 1);
  AsciiFilePath = AllocatePool ((StrLen (FilePathDevicePathNode->PathName) + 1) * sizeof (CHAR8));
  if (AsciiFilePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }
  UnicodeStrToAsciiStr (FilePathDevicePathNode->PathName, AsciiFilePath);

  //
  // Try to get the size of the file in bytes from the server. If it fails,
  // start with a 8MB buffer to download the file.
  //
  FileSize = 0;
  if (Mtftp4GetFileSize (Mtftp4, AsciiFilePath, &FileSize) == EFI_SUCCESS) {
    TftpBufferSize = FileSize;
  } else {
    TftpBufferSize = SIZE_8MB;
  }

  TftpContext = AllocatePool (sizeof (BDS_TFTP_CONTEXT));
  if (TftpContext == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }
  TftpContext->FileSize = FileSize;

  for (; TftpBufferSize <= FixedPcdGet32 (PcdMaxTftpFileSize);
         TftpBufferSize = (TftpBufferSize + SIZE_8MB) & (~(SIZE_8MB-1))) {
    //
    // Allocate a buffer to hold the whole file.
    //
    Status = gBS->AllocatePages (
                    Type,
                    EfiBootServicesCode,
                    EFI_SIZE_TO_PAGES (TftpBufferSize),
                    Image
                    );
    if (EFI_ERROR (Status)) {
      Print (L"Failed to allocate space for image\n");
      goto Error;
    }

    TftpContext->DownloadedNbOfBytes   = 0;
    TftpContext->LastReportedNbOfBytes = 0;

    ZeroMem (&Mtftp4Token, sizeof (EFI_MTFTP4_TOKEN));
    Mtftp4Token.Filename    = (UINT8*)AsciiFilePath;
    Mtftp4Token.BufferSize  = TftpBufferSize;
    Mtftp4Token.Buffer      = (VOID *)(UINTN)*Image;
    Mtftp4Token.CheckPacket = Mtftp4CheckPacket;
    Mtftp4Token.Context     = (VOID*)TftpContext;

    Print (L"Downloading the file <%s> from the TFTP server\n", FilePathDevicePathNode->PathName);
    Status = Mtftp4->ReadFile (Mtftp4, &Mtftp4Token);
    Print (L"\n");
    if (EFI_ERROR (Status)) {
      if (Status == EFI_BUFFER_TOO_SMALL) {
        Print (L"Downloading failed, file larger than expected.\n");
        gBS->FreePages (*Image, EFI_SIZE_TO_PAGES (TftpBufferSize));
        continue;
      } else {
        goto Error;
      }
    }

    *ImageSize = Mtftp4Token.BufferSize;
    break;
  }

Error:
  if (Dhcp4ChildHandle != NULL) {
    if (Dhcp4 != NULL) {
      if (Dhcp4ToStop) {
        Dhcp4->Stop (Dhcp4);
      }
      gBS->CloseProtocol (
             Dhcp4ChildHandle,
             &gEfiDhcp4ProtocolGuid,
             gImageHandle,
             ControllerHandle
            );
    }
    NetLibDestroyServiceChild (
      ControllerHandle,
      gImageHandle,
      &gEfiDhcp4ServiceBindingProtocolGuid,
      Dhcp4ChildHandle
      );
  }

  if (Mtftp4ChildHandle != NULL) {
    if (Mtftp4 != NULL) {
      if (AsciiFilePath != NULL) {
        FreePool (AsciiFilePath);
      }
      if (TftpContext != NULL) {
        FreePool (TftpContext);
      }
      gBS->CloseProtocol (
             Mtftp4ChildHandle,
             &gEfiMtftp4ProtocolGuid,
             gImageHandle,
             ControllerHandle
            );
    }
    NetLibDestroyServiceChild (
      ControllerHandle,
      gImageHandle,
      &gEfiMtftp4ServiceBindingProtocolGuid,
      Mtftp4ChildHandle
      );
  }

  if (EFI_ERROR (Status)) {
    Print (L"Failed to download the file - Error=%r\n", Status);
  }

  return Status;
}

BDS_FILE_LOADER FileLoaders[] = {
    { BdsFileSystemSupport, BdsFileSystemLoadImage },
    { BdsFirmwareVolumeSupport, BdsFirmwareVolumeLoadImage },
    //{ BdsLoadFileSupport, BdsLoadFileLoadImage },
    { BdsMemoryMapSupport, BdsMemoryMapLoadImage },
    { BdsPxeSupport, BdsPxeLoadImage },
    { BdsTftpSupport, BdsTftpLoadImage },
    { NULL, NULL }
};

EFI_STATUS
BdsLoadImageAndUpdateDevicePath (
  IN OUT EFI_DEVICE_PATH       **DevicePath,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN OUT EFI_PHYSICAL_ADDRESS* Image,
  OUT    UINTN                 *FileSize
  )
{
  EFI_STATUS      Status;
  EFI_HANDLE      Handle;
  EFI_DEVICE_PATH *RemainingDevicePath;
  BDS_FILE_LOADER*  FileLoader;

  Status = BdsConnectAndUpdateDevicePath (DevicePath, &Handle, &RemainingDevicePath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FileLoader = FileLoaders;
  while (FileLoader->Support != NULL) {
    if (FileLoader->Support (*DevicePath, Handle, RemainingDevicePath)) {
      return FileLoader->LoadImage (*DevicePath, Handle, RemainingDevicePath, Type, Image, FileSize);
    }
    FileLoader++;
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
BdsLoadImage (
  IN     EFI_DEVICE_PATH       *DevicePath,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN OUT EFI_PHYSICAL_ADDRESS* Image,
  OUT    UINTN                 *FileSize
  )
{
  return BdsLoadImageAndUpdateDevicePath (&DevicePath, Type, Image, FileSize);
}

/**
  Start an EFI Application from a Device Path

  @param  ParentImageHandle     Handle of the calling image
  @param  DevicePath            Location of the EFI Application

  @retval EFI_SUCCESS           All drivers have been connected
  @retval EFI_NOT_FOUND         The Linux kernel Device Path has not been found
  @retval EFI_OUT_OF_RESOURCES  There is not enough resource memory to store the matching results.

**/
EFI_STATUS
BdsStartEfiApplication (
  IN EFI_HANDLE                  ParentImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN UINTN                       LoadOptionsSize,
  IN VOID*                       LoadOptions
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   ImageHandle;
  EFI_PHYSICAL_ADDRESS         BinaryBuffer;
  UINTN                        BinarySize;
  EFI_LOADED_IMAGE_PROTOCOL*   LoadedImage;

  // Find the nearest supported file loader
  Status = BdsLoadImageAndUpdateDevicePath (&DevicePath, AllocateAnyPages, &BinaryBuffer, &BinarySize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Load the image from the Buffer with Boot Services function
  Status = gBS->LoadImage (TRUE, ParentImageHandle, DevicePath, (VOID*)(UINTN)BinaryBuffer, BinarySize, &ImageHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Passed LoadOptions to the EFI Application
  if (LoadOptionsSize != 0) {
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **) &LoadedImage);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    LoadedImage->LoadOptionsSize  = LoadOptionsSize;
    LoadedImage->LoadOptions      = LoadOptions;
  }

  // Before calling the image, enable the Watchdog Timer for  the 5 Minute period
  gBS->SetWatchdogTimer (5 * 60, 0x0000, 0x00, NULL);
  // Start the image
  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  // Clear the Watchdog Timer after the image returns
  gBS->SetWatchdogTimer (0x0000, 0x0000, 0x0000, NULL);

  return Status;
}
