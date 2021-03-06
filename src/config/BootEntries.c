#include "BootEntries.h"

#include <util/Except.h>

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/FileHandleLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

#define CHECK_OPTION(x) (StrnCmp(Line, x L"=", ARRAY_SIZE(x)) == 0)

BOOT_ENTRY* gDefaultEntry = NULL;
LIST_ENTRY gBootEntries = INITIALIZE_LIST_HEAD_VARIABLE(gBootEntries);

static CHAR16* ConfigPaths[] = {
        L"boot/tomatboot.cfg",
        L"tomatboot.cfg"
};

BOOT_ENTRY* GetBootEntryAt(int index) {
    int i = 0;
    for (LIST_ENTRY* Link = gBootEntries.ForwardLink; Link != &gBootEntries; Link = Link->ForwardLink, i++) {
        if (index == i) {
            return BASE_CR(Link, BOOT_ENTRY, Link);
        }
    }
    return NULL;
}

static CHAR16* CopyString(CHAR16* String) {
    return AllocateCopyPool((1 + StrLen(String)) * sizeof(CHAR16), String);
}

static EFI_STATUS LoadBootConfig(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FS, LIST_ENTRY* Head) {
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_FILE_PROTOCOL* file = NULL;

    // open the configurations
    CHECK(FS != NULL);
    EFI_CHECK(FS->OpenVolume(FS, &root));

    for (int i = 0; i < ARRAY_SIZE(ConfigPaths); i++) {
        if (!EFI_ERROR(root->Open(root, &file, ConfigPaths[i], EFI_FILE_MODE_READ, 0))) {
            break;
        }

        // just in case
        file = NULL;
    }

    // if no config found just ignore and continue to next fs
    if (file == NULL) {
        goto cleanup;
    }

    // Start from a clean state
    *Head = (LIST_ENTRY)INITIALIZE_LIST_HEAD_VARIABLE(*Head);
    BOOT_ENTRY* CurrentEntry = NULL;

    // now do the actual processing of everything
    while(TRUE) {
        CHAR16 Line[255] = {0};
        UINTN LineSize = sizeof(Line);
        BOOLEAN Ascii = TRUE;

        if (FileHandleEof(file)) {
            break;
        }
        EFI_CHECK(FileHandleReadLine(file, Line, &LineSize, FALSE, &Ascii));

        if(Line[0] == L':') {
            // got new entry (this is the name)
            CurrentEntry = AllocateZeroPool(sizeof(BOOT_ENTRY));
            CurrentEntry->Protocol = BOOT_STIVALE;
            CurrentEntry->Name = CopyString(Line + 1);
            CurrentEntry->Fs = FS;
            CurrentEntry->Path = L"";
            CurrentEntry->Cmdline = L"";
            CurrentEntry->BootModules = (LIST_ENTRY)INITIALIZE_LIST_HEAD_VARIABLE(CurrentEntry->BootModules);
            InsertTailList(Head, &CurrentEntry->Link);

            Print(L"Adding %s\n", CurrentEntry->Name);
        }else {
            // this is just a parameter
            CHECK(CurrentEntry != NULL);

            // path of the bootable (only one)
            if(CHECK_OPTION(L"PATH")) {
                CurrentEntry->Path = CopyString(Line + sizeof("PATH"));

                // command line arguments (only one)
            }else if(CHECK_OPTION(L"CMDLINE")) {
                CurrentEntry->Cmdline = CopyString(Line + sizeof("CMDLINE"));

                // module path (can have multiple)
            }else if(CHECK_OPTION(L"MODULE")) {
                // get the end of the name
                CHAR16* path = Line + sizeof("MODULE");
                while (*path != 0 && *path != L',') {
                    path++;
                }

                // make sure there is even a path
                CHECK(*path != 0);

                // set the , to null termination and skip it
                *path++ = 0;

                // create the module entry
                BOOT_MODULE* Module = AllocateZeroPool(sizeof(BOOT_MODULE));
                Module->Tag = CopyString(Line + sizeof("MODULE"));
                Module->Path = CopyString(path);
                Module->Fs = FS;
                InsertTailList(&CurrentEntry->BootModules, &Module->Link);

                // the boot protocol to use (onyl one)
            } else if(CHECK_OPTION(L"PROTOCOL")) {
                CHAR16* Protocol = Line + sizeof("PROTOCOL");

                // check the options
                if (StrCmp(Protocol, L"LINUX") == 0) {
                    CurrentEntry->Protocol = BOOT_LINUX;
                } else if (StrCmp(Protocol, L"MB2") == 0) {
                    CurrentEntry->Protocol = BOOT_MB2;
                } else if (StrCmp(Protocol, L"STIVALE") == 0) {
                    CurrentEntry->Protocol = BOOT_STIVALE;
                } else {
                    Print(L"Unknown protocol `%a` for option `%a`\n", Protocol, CurrentEntry->Name);
                    CHECK(FALSE);
                }
            }
        }
    }

cleanup:
    if (file != NULL) {
        FileHandleClose(file);
    }

    if (root != NULL) {
        FileHandleClose(root);
    }

    return Status;
}

EFI_STATUS GetBootEntries(LIST_ENTRY* Head) {
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_HANDLE* Handles = NULL;
    UINTN HandlesCount = 0;

    EFI_CHECK(gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandlesCount, &Handles));

    for (int i = 0; i < HandlesCount; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FS = NULL;
        EFI_CHECK(gBS->HandleProtocol(Handles[i], &gEfiSimpleFileSystemProtocolGuid, (void**)&FS));
        CHECK_AND_RETHROW(LoadBootConfig(FS, Head));
    }

cleanup:
    if (Handles != NULL) {
        FreePool(Handles);
    }

    return Status;
}