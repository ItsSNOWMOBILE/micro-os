/*
 * uefi.h — Minimal UEFI type definitions and protocol GUIDs.
 *
 * This header provides just enough of the UEFI specification to build a
 * standalone bootloader without pulling in gnu-efi or EDK2.  Only the
 * types and protocols actually used by the bootloader are defined.
 */

#ifndef UEFI_H
#define UEFI_H

#include <stdint.h>
#include <stddef.h>

/* ── Base types ──────────────────────────────────────────────────────────── */

typedef uint64_t    UINTN;
typedef int64_t     INTN;
typedef uint64_t    EFI_STATUS;
typedef void       *EFI_HANDLE;
typedef void       *EFI_EVENT;
typedef uint64_t    EFI_PHYSICAL_ADDRESS;
typedef uint64_t    EFI_VIRTUAL_ADDRESS;
typedef uint16_t    CHAR16;
typedef uint8_t     BOOLEAN;
typedef void        VOID;

#define IN
#define OUT
#define OPTIONAL
#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS             0
#define EFI_BUFFER_TOO_SMALL    ((EFI_STATUS)0x8000000000000005ULL)

/* ── GUID ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* ── Table header ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ── Simple Text Output Protocol ─────────────────────────────────────────── */

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    IN struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16                                 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    IN struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void                  *Reset;
    EFI_TEXT_STRING        OutputString;
    void                  *TestString;
    void                  *QueryMode;
    void                  *SetMode;
    void                  *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN  ClearScreen;
    void                  *SetCursorPosition;
    void                  *EnableCursor;
    void                  *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ── Memory map ──────────────────────────────────────────────────────────── */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t             Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t             NumberOfPages;
    uint64_t             Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ── Graphics Output Protocol (GOP) ──────────────────────────────────────── */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t                  Version;
    uint32_t                  HorizontalResolution;
    uint32_t                  VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    uint32_t                  PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                             MaxMode;
    uint32_t                             Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                 FrameBufferBase;
    UINTN                                FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN  struct _EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
    IN  uint32_t                                    ModeNumber,
    OUT UINTN                                      *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION       **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN uint32_t                              ModeNumber
);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    void                                   *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

/* ── Boot Services ───────────────────────────────────────────────────────── */

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    IN OUT UINTN                 *MemoryMapSize,
    OUT    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    OUT    UINTN                 *MapKey,
    OUT    UINTN                 *DescriptorSize,
    OUT    uint32_t              *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    IN  EFI_ALLOCATE_TYPE        Type,
    IN  EFI_MEMORY_TYPE          MemoryType,
    IN  UINTN                    Pages,
    OUT EFI_PHYSICAL_ADDRESS    *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    IN EFI_PHYSICAL_ADDRESS Memory,
    IN UINTN                Pages
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN  EFI_MEMORY_TYPE  PoolType,
    IN  UINTN            Size,
    OUT VOID            **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    IN VOID *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    IN EFI_HANDLE ImageHandle,
    IN UINTN      MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN  EFI_GUID *Protocol,
    IN  VOID     *Registration OPTIONAL,
    OUT VOID    **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    IN UINTN    Timeout,
    IN uint64_t WatchdogCode,
    IN UINTN    DataSize,
    IN CHAR16  *WatchdogData OPTIONAL
);

typedef struct {
    EFI_TABLE_HEADER         Hdr;
    void                    *RaiseTPL;
    void                    *RestoreTPL;
    EFI_ALLOCATE_PAGES       AllocatePages;
    EFI_FREE_PAGES           FreePages;
    EFI_GET_MEMORY_MAP       GetMemoryMap;
    EFI_ALLOCATE_POOL        AllocatePool;
    EFI_FREE_POOL            FreePool;
    void                    *CreateEvent;
    void                    *SetTimer;
    void                    *WaitForEvent;
    void                    *SignalEvent;
    void                    *CloseEvent;
    void                    *CheckEvent;
    void                    *InstallProtocolInterface;
    void                    *ReinstallProtocolInterface;
    void                    *UninstallProtocolInterface;
    void                    *HandleProtocol;
    void                    *Reserved;
    void                    *RegisterProtocolNotify;
    void                    *LocateHandle;
    void                    *LocateDevicePath;
    void                    *InstallConfigurationTable;
    void                    *LoadImage;
    void                    *StartImage;
    void                    *Exit;
    void                    *UnloadImage;
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;
    void                    *GetNextMonotonicCount;
    void                    *Stall;
    EFI_SET_WATCHDOG_TIMER  SetWatchdogTimer;
    void                    *ConnectController;
    void                    *DisconnectController;
    void                    *OpenProtocol;
    void                    *CloseProtocol;
    void                    *OpenProtocolInformation;
    void                    *ProtocolsPerHandle;
    void                    *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL     LocateProtocol;
    /* ... remaining fields not needed ... */
} EFI_BOOT_SERVICES;

/* ── System Table ────────────────────────────────────────────────────────── */

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    void                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    void                            *StdErr;
    void                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    void                            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* UEFI_H */
